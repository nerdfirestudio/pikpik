// Input recording and replay.
//
// The problem this solves: headless automation cannot PLAY. The scripted
// input in PADRead is keyed to VIGetRetraceCount(), which pc_sdk_shims runs as
// a WALL-CLOCK 60 Hz thread, so it can walk and mash A but it cannot pluck a
// Pikmin or reach a later course — and two runs of the same binary do not even
// receive the same inputs. Measured: two -O0 runs with identical scripted
// input and a pinned timestep diverged 27-85% of pixels in-world.
//
// So a human plays once and the session is recorded, then replayed exactly.
//
// THE TICK IS PADRead. ControllerMgr::update calls it exactly once per game
// update (controllerMgr.cpp:26, the only call site in the tree), so the count
// of PADRead calls IS the simulation's own clock. Indexing input on it — not
// on wall time, not on presented frames — is the whole trick. It also makes
// the recording resolution exactly right: pc_window.mm already latches a
// sub-frame tap to "held for exactly one PADRead", so the game cannot perceive
// finer timing than a tick and nothing is lost.
//
// Device-agnostic by construction: pc_window_get_pad() has already merged
// keyboard and gamepad into one state by the time we see it, so a keyboard
// recording and a controller recording are the same file format and replay
// identically.
//
// Determinism notes, checked rather than assumed:
//   - The RNG is libc rand() via System::getRand (system.h:205). The only
//     srand() in the tree is __CARDUnlock, and pc_card.cpp replaces the whole
//     CARD API, so it never runs: rand() starts from libc's default seed on
//     every launch.
//   - Nothing off the sim thread consumes the RNG (jaudio, pc_audio, pc_dsp
//     are all clean), so the sequence a replay consumes is the same one the
//     recording consumed.
//   - Replay REQUIRES a fixed timestep. Physics advances by
//     gsys->getFrameTime() per tick, and System::updateSysClock derives that
//     from OSGetTick(). PC_FIXED_DT is therefore defaulted on for replay (see
//     pc_input_early_init).

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern "C" const char* pc_card_root_public(void); // pc_card.cpp

namespace {

struct PadState {
	unsigned short b;
	signed char sx, sy, cx, cy;
	unsigned char tl, tr;
};

bool same(const PadState& a, const PadState& b)
{
	return a.b == b.b && a.sx == b.sx && a.sy == b.sy && a.cx == b.cx && a.cy == b.cy && a.tl == b.tl && a.tr == b.tr;
}

struct Entry {
	unsigned int tick;
	PadState st;
};

bool s_init;
FILE* s_rec;
PadState s_last;
bool s_have_last;

Entry* s_play;
unsigned int s_play_n, s_play_cap, s_play_idx;
bool s_play_done_msg;

unsigned int s_tick;

// FNV-1a over the save directory's sorted entry names and sizes. Cheap, and
// enough to catch "you replayed against a different save", which desyncs
// silently and is otherwise baffling.
unsigned long long save_fingerprint()
{
	unsigned long long h = 1469598103934665603ULL;
	const char* root     = pc_card_root_public();
	if (!root) {
		return 0;
	}
	// Collect then sort: readdir order is not stable across filesystems.
	static char names[64][256];
	static long sizes[64];
	int n = 0;
	DIR* d = opendir(root);
	if (!d) {
		return 0;
	}
	struct dirent* e;
	while ((e = readdir(d)) != nullptr && n < 64) {
		if (e->d_name[0] == '.') {
			continue;
		}
		snprintf(names[n], sizeof(names[n]), "%s", e->d_name);
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
		struct stat st;
		sizes[n] = (stat(path, &st) == 0) ? (long)st.st_size : -1;
		n++;
	}
	closedir(d);
	for (int i = 1; i < n; i++) { // insertion sort, n <= 64
		char kn[256];
		long ks = sizes[i];
		snprintf(kn, sizeof(kn), "%s", names[i]);
		int j = i - 1;
		while (j >= 0 && strcmp(names[j], kn) > 0) {
			snprintf(names[j + 1], sizeof(names[j + 1]), "%s", names[j]);
			sizes[j + 1] = sizes[j];
			j--;
		}
		snprintf(names[j + 1], sizeof(names[j + 1]), "%s", kn);
		sizes[j + 1] = ks;
	}
	for (int i = 0; i < n; i++) {
		for (const char* p = names[i]; *p; p++) {
			h = (h ^ (unsigned char)*p) * 1099511628211ULL;
		}
		unsigned long long s = (unsigned long long)sizes[i];
		for (int k = 0; k < 8; k++) {
			h = (h ^ ((s >> (k * 8)) & 0xFF)) * 1099511628211ULL;
		}
	}
	return h;
}

void load_play(const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "[INPUT] replay: cannot open '%s'\n", path);
		return;
	}
	unsigned long long recorded_fp = 0;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#') {
			continue;
		}
		if (strncmp(line, "save ", 5) == 0) {
			sscanf(line + 5, "%llu", &recorded_fp);
			continue;
		}
		unsigned int tick;
		unsigned int b, tl, tr;
		int sx, sy, cx, cy;
		if (sscanf(line, "%u %u %d %d %d %d %u %u", &tick, &b, &sx, &sy, &cx, &cy, &tl, &tr) != 8) {
			continue;
		}
		if (s_play_n == s_play_cap) {
			s_play_cap = s_play_cap ? s_play_cap * 2 : 256;
			s_play     = (Entry*)realloc(s_play, s_play_cap * sizeof(Entry));
		}
		Entry& en   = s_play[s_play_n++];
		en.tick     = tick;
		en.st.b     = (unsigned short)b;
		en.st.sx    = (signed char)sx;
		en.st.sy    = (signed char)sy;
		en.st.cx    = (signed char)cx;
		en.st.cy    = (signed char)cy;
		en.st.tl    = (unsigned char)tl;
		en.st.tr    = (unsigned char)tr;
	}
	fclose(f);

	unsigned long long now_fp = save_fingerprint();
	fprintf(stderr, "[INPUT] replay: %u input changes from '%s', last tick %u\n", s_play_n, path,
	        s_play_n ? s_play[s_play_n - 1].tick : 0);
	if (recorded_fp && now_fp != recorded_fp) {
		fprintf(stderr,
		        "[INPUT] *** SAVE STATE DIFFERS from the recording (%llu vs %llu).\n"
		        "[INPUT] *** The replay WILL desync. Use the same PC_CARD_DIR contents.\n",
		        now_fp, recorded_fp);
	} else if (recorded_fp) {
		fprintf(stderr, "[INPUT] replay: save state matches the recording\n");
	}
	if (!getenv("PC_FIXED_DT")) {
		fprintf(stderr, "[INPUT] *** PC_FIXED_DT is not set — physics advances by wall-clock\n"
		                "[INPUT] *** time per tick and the replay will drift. See docs/CONFIG.md.\n");
	}
}

void init()
{
	s_init = true;
	if (const char* p = getenv("PC_INPUT_PLAY")) {
		load_play(p);
		return;
	}
	if (const char* p = getenv("PC_INPUT_REC")) {
		s_rec = fopen(p, "w");
		if (!s_rec) {
			fprintf(stderr, "[INPUT] record: cannot write '%s'\n", p);
			return;
		}
		fprintf(s_rec, "# pikpik input recording v1\n"
		               "# tick = PADRead call index (one per game update)\n"
		               "# fields: tick buttons stickX stickY substickX substickY trigL trigR\n"
		               "# only CHANGES are stored; state holds until the next line\n");
		fprintf(s_rec, "save %llu\n", save_fingerprint());
		fflush(s_rec);
		fprintf(stderr, "[INPUT] recording to '%s' (save fingerprint %llu)\n", p, save_fingerprint());
	}
}

} // namespace

// Called from main() before anything can allocate — see sysBootup.cpp. Only
// touches the environment (libc malloc, not the game's replaced operator new).
extern "C" void pc_input_early_init(void)
{
	// Both directions need it, not just replay — and RECORDING is the easier
	// one to get wrong. The recording stores what was pressed at each tick, so
	// if the recording ran on a wall-clock timestep the sim advanced by a
	// different amount per tick than the replay will, and the same inputs
	// produce a different game. Defaulting it on both sides removes the
	// footgun; at a paced 30 fps it is also the value the existing clamp in
	// System::updateSysClock already yields, so play feels unchanged.
	if ((getenv("PC_INPUT_PLAY") || getenv("PC_INPUT_REC")) && !getenv("PC_FIXED_DT")) {
		setenv("PC_FIXED_DT", "0.0333333333", 0);
		fprintf(stderr, "[INPUT] PC_FIXED_DT defaulted to 1/30 — required for record/replay to agree\n");
	}
}

// Returns 1 if the state was overwritten from a recording.
extern "C" int pc_input_tick(unsigned short* b, signed char* sx, signed char* sy, signed char* cx, signed char* cy,
                             unsigned char* tl, unsigned char* tr)
{
	if (!s_init) {
		init();
	}
	const unsigned int tick = s_tick++;

	if (s_play) {
		while (s_play_idx < s_play_n && s_play[s_play_idx].tick <= tick) {
			s_last      = s_play[s_play_idx].st;
			s_have_last = true;
			s_play_idx++;
		}
		if (s_play_idx >= s_play_n && !s_play_done_msg) {
			s_play_done_msg = true;
			fprintf(stderr, "[INPUT] replay: recording exhausted at tick %u — input now neutral\n", tick);
		}
		PadState st = s_have_last ? s_last : PadState();
		*b          = st.b;
		*sx         = st.sx;
		*sy         = st.sy;
		*cx         = st.cx;
		*cy         = st.cy;
		*tl         = st.tl;
		*tr         = st.tr;
		return 1;
	}

	if (s_rec) {
		PadState st;
		st.b  = *b;
		st.sx = *sx;
		st.sy = *sy;
		st.cx = *cx;
		st.cy = *cy;
		st.tl = *tl;
		st.tr = *tr;
		if (!s_have_last || !same(st, s_last)) {
			fprintf(s_rec, "%u %u %d %d %d %d %u %u\n", tick, st.b, st.sx, st.sy, st.cx, st.cy, st.tl, st.tr);
			// Flushed on every change on purpose: the whole point is to
			// record a session that ENDS IN A CRASH, and a buffered tail is
			// exactly the part worth having. Changes are rare, so this costs
			// nothing.
			fflush(s_rec);
			s_last      = st;
			s_have_last = true;
		}
	}
	return 0;
}

extern "C" unsigned int pc_input_current_tick(void) { return s_tick; }
