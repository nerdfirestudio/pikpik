// pc_card.cpp — GameCube memory card (CARD) API backed by host files.
//
// M4. The decomp ships the REAL CARD library in src/card/*.c, but it sits on
// EXI (serial bus + DMA + interrupt callbacks), DSP (the unlock scramble),
// OSAlarm and SRAM — emulating that stack is a hardware-emulation project.
// So this is an API-level shim, exactly like the prior port's shipped pc_card.c,
// with src/card/*.c used as the SEMANTIC REFERENCE for result codes, field
// derivation and validation (every rule below cites its source file).
//
// Storage: one host directory per channel (save/card_a, save/card_b), and per
// card file a data file <name>.bin plus a metadata sidecar <name>.stat. The
// sidecar carries the CARDStat fields the game round-trips (gameName/company,
// banner+icon formats, icon/comment addresses, time) and the file's slot
// number, so fileNo stays stable across runs — CARDFastOpen/CARDFastDelete
// address files by slot, and the directory scan in cardutil.cpp walks all 127
// slots expecting a negative result for the empty ones.
//
// Async variants complete synchronously and then fire their callback: the
// game never relies on a later completion — it spins on CARDGetResultCode
// (MemoryCard::waitWhileBusy, memoryCard.cpp:580), which our immediate
// completion satisfies on the first read.
//
// PC_NO_CARD=1 restores the pre-M4 behaviour (every entry point NOCARD).
// PC_CARD_DIR=<path> relocates the card root (default:
// ~/Library/Application Support/Pikmin/save — see pc_paths.h).

#include "Dolphin/card.h"
#include "Dolphin/os.h"
#include "pc_paths.h"
#include "pc_sync.h"
#include "types.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------- constants
// A "Memory Card 251": 251 usable 8 KiB blocks, 127 directory slots. The game
// only cares that free space exceeds MemoryCard::mRequiredFreeSpace.
#define PC_CARD_SECTOR_SIZE 8192u
#define PC_CARD_BLOCKS      251
#define PC_CARD_STAT_MAGIC  0x50434D31u // 'PCM1'

// Seconds between 1970-01-01 and 2000-01-01 — CARDDir::time is "secs since
// 01/01/2000 00:00" (card.h:80).
#define PC_CARD_EPOCH_2000 946684800u

static int pc_card_disabled(void) {
	static int v = -1;
	if (v < 0) v = getenv("PC_NO_CARD") ? 1 : 0;
	return v;
}

static int pc_card_verbose(void) {
	static int v = -1;
	if (v < 0) v = getenv("PC_VERBOSE") ? 1 : 0;
	return v;
}

#define CARD_TRACE(...)                                    \
	do {                                                   \
		if (pc_card_verbose()) {                           \
			fprintf(stderr, "[CARD] " __VA_ARGS__);        \
		}                                                  \
	} while (0)

// ---------------------------------------------------------------- state
struct PcCardEntry {
	bool inUse;
	char name[CARD_FILENAME_MAX + 1];
	u32 length; // bytes, always a multiple of the sector size
	u8 gameName[4];
	u8 company[2];
	u8 bannerFormat;
	u8 permission;
	u32 time; // secs since 2000
	u32 iconAddr;
	u16 iconFormat;
	u16 iconSpeed;
	u32 commentAddr;
};

struct PcCardChannel {
	bool mounted;
	s32 result; // last result, for CARDGetResultCode
	s32 xferred;
	PcCardEntry dir[CARD_MAX_FILE];
};

static PcCardChannel s_chan[2];

// The real library keeps this array; other translation units reference it.
CARDControl __CARDBlock[2];

static PcCardChannel* pc_chan(s32 chan) {
	if (chan < 0 || chan > 1) return nullptr;
	return &s_chan[chan];
}

// ---------------------------------------------------------------- paths
// Exported so pc_input_rec.cpp can fingerprint the save state a recording was
// made against — replaying against a different save desyncs silently, which is
// baffling to debug. Kept as a thin wrapper rather than un-static'ing the
// function, so the resolution order below stays the single source of truth.
extern "C" const char* pc_card_root_public(void);

static const char* pc_card_root(void) {
	const char* e = getenv("PC_CARD_DIR");
	if (e) {
		return e;
	}
	// M7: saves live in ~/Library/Application Support/Pikmin/save, not "./save".
	// A bundled .app is launched with an arbitrary cwd and should stay
	// read-only, so relative saves would land somewhere unpredictable (or fail
	// outright). PC_CARD_DIR still overrides, which is what the headless tests
	// and PC_CARD_SELFTEST use.
	// NOTE: no std::string here. pc_card_selftest() runs from main() BEFORE
	// gsys->Initialise(), and the game replaces operator new — before the arena
	// exists that allocator returns NULL, and with -fno-exceptions std::string
	// does not throw, it memmoves into a null pointer. See pc_paths.cpp.
	static char root[1024];
	if (!root[0]) {
		snprintf(root, sizeof(root), "%s/save", pc_path_support());

		// The default moved in M7. Anyone with a save under the old build-tree
		// location would otherwise find an empty card and assume the save
		// system broke, so say so once — explicitly rather than migrating the
		// files, because silently relocating somebody's save is not a decision
		// this layer should be making.
		struct stat st;
		char mine[1100];
		snprintf(mine, sizeof(mine), "%s/card_a", root);
		if (stat(mine, &st) != 0) {
			// Where a save could have been left by an earlier layout: the
			// original build-tree default, and Application Support (which was
			// the default before config went portable-when-loose).
			char appsup[1100];
			appsup[0]         = '\0';
			const char* home  = getenv("HOME");
			if (home && *home) {
				snprintf(appsup, sizeof(appsup), "%s/Library/Application Support/Pikmin/save/card_a", home);
			}
			const char* legacy[] = { "save/card_a", "build-m1/save/card_a", appsup };
			for (unsigned i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
				if (!legacy[i][0]) {
					continue;
				}
				if (stat(legacy[i], &st) == 0) {
					OSReport("[PC] saves now live in %s\n", root);
					OSReport("[PC] an older save exists at %s — copy that directory across to keep it\n", legacy[i]);
					break;
				}
			}
		}
	}
	return root;
}

extern "C" const char* pc_card_root_public(void) { return pc_card_root(); }

static void pc_card_dir_path(s32 chan, char* out, size_t n) {
	snprintf(out, n, "%s/card_%c", pc_card_root(), chan == 0 ? 'a' : 'b');
}

// Card names are ASCII and may contain characters awkward in a host path;
// '/' is the only one that would escape the directory. Temp files start with
// '~' (the save protocol in cardutil.cpp DoSave) which is fine on POSIX.
static void pc_card_safe_name(const char* in, char* out, size_t n) {
	size_t i = 0;
	for (; in[i] && i < n - 1 && i < CARD_FILENAME_MAX; i++) {
		char c = in[i];
		out[i] = (c == '/' || c == '\\') ? '_' : c;
	}
	out[i] = '\0';
}

static void pc_card_file_path(s32 chan, const char* name, const char* ext, char* out, size_t n) {
	char dir[512], safe[CARD_FILENAME_MAX + 1];
	pc_card_dir_path(chan, dir, sizeof(dir));
	pc_card_safe_name(name, safe, sizeof(safe));
	snprintf(out, n, "%s/%s%s", dir, safe, ext);
}

static bool pc_card_mkdir_p(const char* path) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char* p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

// ------------------------------------------------------------- sync notify
// Card A is what the save-sync client publishes, so every mutation of it has
// to mark the card dirty. Hooked at the LOW-LEVEL helpers rather than at the
// seven CARD* entry points: create/delete/rename all funnel through
// pc_card_write_stat and pc_card_remove_files, so three call sites (plus the
// raw data write and the format wipe) cover the whole surface and no future
// entry point can quietly bypass them.
//
// Cheap by construction — it sets a flag and a timestamp. The upload itself
// is the worker's problem, and it waits for the card to go quiet first,
// because one save is several of these calls in a row.
static void pc_card_touched(s32 chan)
{
	if (chan == 0) {
		pc_sync_notify_card_changed();
	}
}

// ---------------------------------------------------------------- sidecar io
// Our own metadata format — we own both reader and writer, so it only has to
// be self-consistent (host byte order, fixed field order).
struct PcCardStatFile {
	u32 magic;
	s32 fileNo;
	u8 gameName[4];
	u8 company[2];
	u8 bannerFormat;
	u8 permission;
	u32 time;
	u32 iconAddr;
	u16 iconFormat;
	u16 iconSpeed;
	u32 commentAddr;
	u32 length;
};

static bool pc_card_write_stat(s32 chan, s32 fileNo, const PcCardEntry* e) {
	char path[640];
	pc_card_file_path(chan, e->name, ".stat", path, sizeof(path));
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	PcCardStatFile s;
	memset(&s, 0, sizeof(s));
	s.magic  = PC_CARD_STAT_MAGIC;
	s.fileNo = fileNo;
	memcpy(s.gameName, e->gameName, 4);
	memcpy(s.company, e->company, 2);
	s.bannerFormat = e->bannerFormat;
	s.permission   = e->permission;
	s.time         = e->time;
	s.iconAddr     = e->iconAddr;
	s.iconFormat   = e->iconFormat;
	s.iconSpeed    = e->iconSpeed;
	s.commentAddr  = e->commentAddr;
	s.length       = e->length;
	bool ok        = fwrite(&s, sizeof(s), 1, f) == 1;
	fclose(f);
	if (ok) pc_card_touched(chan);
	return ok;
}

// Keep the last few versions of a save. The game's own save protocol deletes
// the previous file before renaming the new one into place, so every ordinary
// save passes through here — which makes this the one place that sees a save
// about to disappear, whether from a real overwrite, an in-game delete, or a
// test harness mashing through menus.
//
// Same rotation as the prior port's pc_m_card.c: .bakN is dropped, each older
// backup shifts up, and the current file becomes .bak1. Backups end in
// ".bakN" so neither directory scan picks them up (both match the exact
// ".bin"/".stat" suffixes).
#define PC_CARD_MAX_BACKUPS 3

static void pc_card_rotate_backups(const char* basePath) {
	char older[700], newer[700];

	snprintf(older, sizeof(older), "%s.bak%d", basePath, PC_CARD_MAX_BACKUPS);
	remove(older);

	for (int i = PC_CARD_MAX_BACKUPS; i > 1; i--) {
		snprintf(older, sizeof(older), "%s.bak%d", basePath, i - 1);
		snprintf(newer, sizeof(newer), "%s.bak%d", basePath, i);
		remove(newer); // rename() will not overwrite on every platform
		rename(older, newer);
	}

	struct stat st;
	if (stat(basePath, &st) == 0) {
		snprintf(newer, sizeof(newer), "%s.bak1", basePath);
		remove(newer);
		rename(basePath, newer);
	}
}

static void pc_card_remove_files(s32 chan, const char* name) {
	char path[640];
	pc_card_file_path(chan, name, ".bin", path, sizeof(path));
	// Rotate rather than unlink: this moves the .bin aside to .bak1, which
	// both removes it from the card's view and keeps it recoverable.
	pc_card_rotate_backups(path);
	pc_card_file_path(chan, name, ".stat", path, sizeof(path));
	pc_card_rotate_backups(path);
	pc_card_touched(chan);
}

// ---------------------------------------------------------------- directory
static void pc_card_clear_dir(PcCardChannel* c) {
	for (int i = 0; i < CARD_MAX_FILE; i++) c->dir[i].inUse = false;
}

static s32 pc_card_find_free_slot(PcCardChannel* c) {
	for (s32 i = 0; i < CARD_MAX_FILE; i++)
		if (!c->dir[i].inUse) return i;
	return -1;
}

static s32 pc_card_find_by_name(PcCardChannel* c, const char* name) {
	for (s32 i = 0; i < CARD_MAX_FILE; i++)
		if (c->dir[i].inUse && strncmp(c->dir[i].name, name, CARD_FILENAME_MAX) == 0) return i;
	return -1;
}

// Load every <name>.stat in the channel directory into the slot it recorded;
// if that slot is taken (hand-edited files, older layout) fall back to the
// first free one and rewrite the sidecar so it stays stable from then on.
static void pc_card_load_dir(s32 chan) {
	PcCardChannel* c = pc_chan(chan);
	pc_card_clear_dir(c);

	char dirPath[512];
	pc_card_dir_path(chan, dirPath, sizeof(dirPath));
	DIR* d = opendir(dirPath);
	if (!d) return;

	struct dirent* de;
	while ((de = readdir(d)) != nullptr) {
		size_t len = strlen(de->d_name);
		if (len < 6 || strcmp(de->d_name + len - 5, ".stat") != 0) continue;

		char path[640];
		snprintf(path, sizeof(path), "%s/%s", dirPath, de->d_name);
		FILE* f = fopen(path, "rb");
		if (!f) continue;
		PcCardStatFile s;
		bool ok = fread(&s, sizeof(s), 1, f) == 1;
		fclose(f);
		if (!ok || s.magic != PC_CARD_STAT_MAGIC) {
			CARD_TRACE("skipping unreadable sidecar %s\n", de->d_name);
			continue;
		}

		s32 slot = (s.fileNo >= 0 && s.fileNo < CARD_MAX_FILE && !c->dir[s.fileNo].inUse) ? s.fileNo : pc_card_find_free_slot(c);
		if (slot < 0) continue;

		PcCardEntry* e = &c->dir[slot];
		memset(e, 0, sizeof(*e));
		e->inUse = true;
		// the sidecar name is the host file name minus ".stat"
		size_t nameLen = len - 5;
		if (nameLen > CARD_FILENAME_MAX) nameLen = CARD_FILENAME_MAX;
		memcpy(e->name, de->d_name, nameLen);
		e->name[nameLen] = '\0';
		memcpy(e->gameName, s.gameName, 4);
		memcpy(e->company, s.company, 2);
		e->bannerFormat = s.bannerFormat;
		e->permission   = s.permission;
		e->time         = s.time;
		e->iconAddr     = s.iconAddr;
		e->iconFormat   = s.iconFormat;
		e->iconSpeed    = s.iconSpeed;
		e->commentAddr  = s.commentAddr;
		e->length       = s.length;

		if (slot != s.fileNo) pc_card_write_stat(chan, slot, e);
		CARD_TRACE("dir[%d] = \"%s\" %u bytes\n", slot, e->name, e->length);
	}
	closedir(d);
}

static u32 pc_card_used_blocks(PcCardChannel* c) {
	u32 blocks = 0;
	for (int i = 0; i < CARD_MAX_FILE; i++)
		if (c->dir[i].inUse) blocks += (c->dir[i].length + PC_CARD_SECTOR_SIZE - 1) / PC_CARD_SECTOR_SIZE;
	return blocks;
}

static u32 pc_card_now_2000(void) {
	time_t now = time(nullptr);
	if ((u32)now < PC_CARD_EPOCH_2000) return 0;
	return (u32)now - PC_CARD_EPOCH_2000;
}

// __CARDAccess (CARDOpen.c:38): an empty slot reads gameName[0]==0xFF and
// yields NOFILE; a file belonging to another game yields NOPERM.
static s32 pc_card_access(PcCardChannel* c, s32 fileNo) {
	if (!c->dir[fileNo].inUse) return CARD_RESULT_NOFILE;
	DVDDiskID* id = DVDGetCurrentDiskID();
	if (memcmp(c->dir[fileNo].gameName, id->gameName, 4) == 0 && memcmp(c->dir[fileNo].company, id->company, 2) == 0) {
		return CARD_RESULT_READY;
	}
	return CARD_RESULT_NOPERM;
}

// Every entry point funnels its result through here so CARDGetResultCode
// (what the game's busy-wait loops read) always reflects the last operation.
static s32 pc_card_done(s32 chan, s32 result) {
	PcCardChannel* c = pc_chan(chan);
	if (c) c->result = result;
	return result;
}

// ---------------------------------------------------------------- CARDStat
// Faithful port of UpdateIconOffsets (src/card/CARDStat.c:7) — the derived
// offsets the game's icon/banner reader walks (cardutil.cpp:326-375).
static void pc_card_update_icon_offsets(CARDStat* state) {
	u32 offset = state->iconAddr;
	if (offset == 0xffffffff) {
		state->bannerFormat = 0;
		state->iconFormat   = 0;
		state->iconSpeed    = 0;
		offset              = 0;
	}

	BOOL iconTlut = FALSE;
	switch (state->bannerFormat & CARD_STAT_BANNER_MASK) {
	case CARD_STAT_BANNER_C8:
		state->offsetBanner = offset;
		offset += CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
		state->offsetBannerTlut = offset;
		offset += 2 * 256;
		break;
	case CARD_STAT_BANNER_RGB5A3:
		state->offsetBanner = offset;
		offset += 2 * CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
		state->offsetBannerTlut = 0xffffffff;
		break;
	default:
		state->offsetBanner     = 0xffffffff;
		state->offsetBannerTlut = 0xffffffff;
		break;
	}

	for (int i = 0; i < CARD_ICON_MAX; ++i) {
		switch (CARDGetIconFormat(state, i)) {
		case CARD_STAT_ICON_C8:
			state->offsetIcon[i] = offset;
			offset += CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
			iconTlut = TRUE;
			break;
		case CARD_STAT_ICON_RGB5A3:
			state->offsetIcon[i] = offset;
			offset += 2 * CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
			break;
		default:
			state->offsetIcon[i] = 0xffffffff;
			break;
		}
	}

	if (iconTlut) {
		state->offsetIconTlut = offset;
		offset += 2 * 256;
	} else {
		state->offsetIconTlut = 0xffffffff;
	}
	state->offsetData = offset;
}

// ---------------------------------------------------------------- API
extern "C" {

void CARDInit() {
	memset(__CARDBlock, 0, sizeof(__CARDBlock));
	memset(s_chan, 0, sizeof(s_chan));
	CARD_TRACE("init (root=%s%s)\n", pc_card_root(), pc_card_disabled() ? ", DISABLED" : "");
}

BOOL CARDProbe(s32 chan) {
	if (pc_card_disabled() || chan != 0) return FALSE;
	return TRUE;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
	if (pc_card_disabled() || chan != 0) return CARD_RESULT_NOCARD;
	if (memSize) *memSize = PC_CARD_BLOCKS * (s32)PC_CARD_SECTOR_SIZE / (128 * 1024); // Mbits
	if (sectorSize) *sectorSize = (s32)PC_CARD_SECTOR_SIZE;
	return CARD_RESULT_READY;
}

s32 CARDMount(s32 chan, CARDMemoryCard* workArea, CARDCallback detachCallback) {
	(void)workArea;
	(void)detachCallback;
	PcCardChannel* c = pc_chan(chan);
	if (pc_card_disabled() || !c || chan != 0) return pc_card_done(chan, CARD_RESULT_NOCARD);

	// An empty host directory IS a blank formatted card here — the filesystem
	// plays the role of the flash, and there is nothing to format. So create
	// it on first mount rather than sending the player through the GC
	// "this card is unusable, format it?" flow for a brand new install.
	// A directory we cannot create is a real failure (BROKEN, which the game
	// does offer to format).
	char dirPath[512];
	pc_card_dir_path(chan, dirPath, sizeof(dirPath));
	struct stat st;
	if (stat(dirPath, &st) != 0 || !S_ISDIR(st.st_mode)) {
		if (!pc_card_mkdir_p(dirPath)) {
			c->mounted = true;
			pc_card_clear_dir(c);
			CARD_TRACE("mount chan%d: cannot create %s -> BROKEN\n", chan, dirPath);
			return pc_card_done(chan, CARD_RESULT_BROKEN);
		}
		CARD_TRACE("mount chan%d: created blank card at %s\n", chan, dirPath);
	}

	c->mounted = true;
	pc_card_load_dir(chan);
	CARD_TRACE("mount chan%d: READY (%u/%d blocks used)\n", chan, pc_card_used_blocks(c), PC_CARD_BLOCKS);
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDMountAsync(s32 chan, CARDMemoryCard* workArea, CARDCallback detachCallback, CARDCallback attachCallback) {
	s32 res = CARDMount(chan, workArea, detachCallback);
	if (attachCallback) attachCallback(chan, res);
	// The game treats a negative RETURN as "couldn't start" and then polls
	// CARDGetResultCode for the outcome, so report the start as success.
	return res < 0 ? res : CARD_RESULT_READY;
}

s32 CARDUnmount(s32 chan) {
	PcCardChannel* c = pc_chan(chan);
	if (!c) return pc_card_done(chan, CARD_RESULT_NOCARD);
	c->mounted = false;
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDCheck(s32 chan) {
	PcCardChannel* c = pc_chan(chan);
	if (pc_card_disabled() || !c) return pc_card_done(chan, CARD_RESULT_NOCARD);
	if (!c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);
	char dirPath[512];
	pc_card_dir_path(chan, dirPath, sizeof(dirPath));
	struct stat st;
	if (stat(dirPath, &st) != 0 || !S_ISDIR(st.st_mode)) return pc_card_done(chan, CARD_RESULT_BROKEN);
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback) {
	s32 res = CARDCheck(chan);
	if (callback) callback(chan, res);
	return res < 0 ? res : CARD_RESULT_READY;
}

s32 CARDFormat(s32 chan) {
	PcCardChannel* c = pc_chan(chan);
	if (pc_card_disabled() || !c || chan != 0) return pc_card_done(chan, CARD_RESULT_NOCARD);

	char dirPath[512];
	pc_card_dir_path(chan, dirPath, sizeof(dirPath));

	// Wipe any existing card files, then (re)create the directory.
	DIR* d = opendir(dirPath);
	if (d) {
		struct dirent* de;
		while ((de = readdir(d)) != nullptr) {
			size_t len = strlen(de->d_name);
			bool isBin = len > 4 && strcmp(de->d_name + len - 4, ".bin") == 0;
			bool isSt  = len > 5 && strcmp(de->d_name + len - 5, ".stat") == 0;
			if (!isBin && !isSt) continue;
			char path[640];
			snprintf(path, sizeof(path), "%s/%s", dirPath, de->d_name);
			unlink(path);
		}
		closedir(d);
	}

	if (!pc_card_mkdir_p(dirPath)) {
		CARD_TRACE("format chan%d: mkdir %s failed\n", chan, dirPath);
		return pc_card_done(chan, CARD_RESULT_IOERROR);
	}

	pc_card_clear_dir(c);
	c->mounted = true;
	pc_card_touched(chan);
	CARD_TRACE("format chan%d: READY\n", chan);
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback) {
	s32 res = CARDFormat(chan);
	if (callback) callback(chan, res);
	return res < 0 ? res : CARD_RESULT_READY;
}

s32 CARDGetSectorSize(s32 chan, u32* size) {
	PcCardChannel* c = pc_chan(chan);
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);
	if (size) *size = PC_CARD_SECTOR_SIZE;
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
	PcCardChannel* c = pc_chan(chan);
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);

	u32 used  = pc_card_used_blocks(c);
	s32 free_ = (s32)PC_CARD_BLOCKS - (s32)used;
	if (free_ < 0) free_ = 0;

	int files = 0;
	for (int i = 0; i < CARD_MAX_FILE; i++)
		if (c->dir[i].inUse) files++;

	if (byteNotUsed) *byteNotUsed = free_ * (s32)PC_CARD_SECTOR_SIZE;
	if (filesNotUsed) *filesNotUsed = CARD_MAX_FILE - files;
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* state) {
	PcCardChannel* c = pc_chan(chan);
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE) return CARD_RESULT_FATAL_ERROR;
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);

	s32 result = pc_card_access(c, fileNo);
	if (result < CARD_RESULT_READY) return pc_card_done(chan, result);

	const PcCardEntry* e = &c->dir[fileNo];
	memset(state, 0, sizeof(*state));
	memcpy(state->gameName, e->gameName, 4);
	memcpy(state->company, e->company, 2);
	state->length = e->length;
	strncpy(state->fileName, e->name, CARD_FILENAME_MAX);
	state->time         = e->time;
	state->bannerFormat = e->bannerFormat;
	state->iconAddr     = e->iconAddr;
	state->iconFormat   = e->iconFormat;
	state->iconSpeed    = e->iconSpeed;
	state->commentAddr  = e->commentAddr;
	pc_card_update_icon_offsets(state);
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* state) {
	PcCardChannel* c = pc_chan(chan);
	// Validation copied from CARDSetStatusAsync (src/card/CARDStat.c:126).
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE || (state->iconAddr != 0xffffffff && CARD_READ_SIZE <= state->iconAddr)
	    || (state->commentAddr != 0xffffffff
	        && CARD_SYSTEM_BLOCK_SIZE - CARD_COMMENT_SIZE < state->commentAddr % CARD_SYSTEM_BLOCK_SIZE)) {
		return CARD_RESULT_FATAL_ERROR;
	}
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);

	s32 result = pc_card_access(c, fileNo);
	if (result < CARD_RESULT_READY) return pc_card_done(chan, result);

	PcCardEntry* e  = &c->dir[fileNo];
	e->bannerFormat = state->bannerFormat;
	e->iconAddr     = state->iconAddr;
	e->iconFormat   = state->iconFormat;
	e->iconSpeed    = state->iconSpeed;
	e->commentAddr  = state->commentAddr;
	e->time         = pc_card_now_2000();
	if (!pc_card_write_stat(chan, fileNo, e)) return pc_card_done(chan, CARD_RESULT_IOERROR);
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDCreate(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo) {
	PcCardChannel* c = pc_chan(chan);
	if (strlen(fileName) > CARD_FILENAME_MAX) return pc_card_done(chan, CARD_RESULT_NAMETOOLONG);
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);
	// CARDCreate.c: size must be a whole number of sectors.
	if (size == 0 || (size % PC_CARD_SECTOR_SIZE) != 0) return pc_card_done(chan, CARD_RESULT_FATAL_ERROR);
	if (pc_card_find_by_name(c, fileName) >= 0) return pc_card_done(chan, CARD_RESULT_EXIST);

	s32 slot = pc_card_find_free_slot(c);
	if (slot < 0) return pc_card_done(chan, CARD_RESULT_NOENT);

	u32 needBlocks = size / PC_CARD_SECTOR_SIZE;
	if (pc_card_used_blocks(c) + needBlocks > (u32)PC_CARD_BLOCKS) return pc_card_done(chan, CARD_RESULT_INSSPACE);

	char dirPath[512];
	pc_card_dir_path(chan, dirPath, sizeof(dirPath));
	pc_card_mkdir_p(dirPath);

	// Allocate the data file up front, zero-filled, so later writes at any
	// sector offset land in an existing file (the game writes 0x2000 regions
	// out of order: banner, then options, then game files).
	char path[640];
	pc_card_file_path(chan, fileName, ".bin", path, sizeof(path));
	FILE* f = fopen(path, "wb");
	if (!f) return pc_card_done(chan, CARD_RESULT_IOERROR);
	static u8 zero[PC_CARD_SECTOR_SIZE];
	memset(zero, 0, sizeof(zero));
	for (u32 i = 0; i < needBlocks; i++) {
		if (fwrite(zero, sizeof(zero), 1, f) != 1) {
			fclose(f);
			unlink(path);
			return pc_card_done(chan, CARD_RESULT_IOERROR);
		}
	}
	fclose(f);

	PcCardEntry* e = &c->dir[slot];
	memset(e, 0, sizeof(*e));
	e->inUse = true;
	strncpy(e->name, fileName, CARD_FILENAME_MAX);
	e->name[CARD_FILENAME_MAX] = '\0';
	DVDDiskID* id              = DVDGetCurrentDiskID();
	memcpy(e->gameName, id->gameName, 4);
	memcpy(e->company, id->company, 2);
	e->length      = size;
	e->time        = pc_card_now_2000();
	e->iconAddr    = 0xffffffff;
	e->commentAddr = 0xffffffff;
	e->permission  = CARD_ATTR_PUBLIC;

	if (!pc_card_write_stat(chan, slot, e)) {
		e->inUse = false;
		unlink(path);
		return pc_card_done(chan, CARD_RESULT_IOERROR);
	}

	if (fileInfo) {
		fileInfo->chan   = chan;
		fileInfo->fileNo = slot;
		fileInfo->offset = 0;
		fileInfo->length = (s32)size;
		fileInfo->iBlock = (u16)(slot + 5); // plausible non-system block
	}
	CARD_TRACE("create chan%d \"%s\" %u bytes -> slot %d\n", chan, fileName, size, slot);
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fileInfo) {
	PcCardChannel* c = pc_chan(chan);
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE) return CARD_RESULT_FATAL_ERROR;
	if (fileInfo) fileInfo->chan = -1;
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);

	s32 result = pc_card_access(c, fileNo);
	if (result < CARD_RESULT_READY) return pc_card_done(chan, result);

	if (fileInfo) {
		fileInfo->chan   = chan;
		fileInfo->fileNo = fileNo;
		fileInfo->offset = 0;
		fileInfo->length = (s32)c->dir[fileNo].length;
		fileInfo->iBlock = (u16)(fileNo + 5);
	}
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo) {
	PcCardChannel* c = pc_chan(chan);
	if (fileInfo) fileInfo->chan = -1;
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);

	s32 slot = pc_card_find_by_name(c, fileName);
	if (slot < 0) return pc_card_done(chan, CARD_RESULT_NOFILE);
	return CARDFastOpen(chan, slot, fileInfo);
}

s32 CARDClose(CARDFileInfo* fileInfo) {
	if (!fileInfo) return CARD_RESULT_FATAL_ERROR;
	s32 chan       = fileInfo->chan;
	fileInfo->chan = -1;
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDRead(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset) {
	if (!fileInfo || fileInfo->chan < 0) return CARD_RESULT_FATAL_ERROR;
	s32 chan         = fileInfo->chan;
	PcCardChannel* c = pc_chan(chan);
	if (!c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);
	// CARDRead.c:115 — both must be whole segments.
	if ((offset % (s32)CARD_SEG_SIZE) != 0 || (length % (s32)CARD_SEG_SIZE) != 0) return pc_card_done(chan, CARD_RESULT_FATAL_ERROR);

	s32 fileNo = fileInfo->fileNo;
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE || !c->dir[fileNo].inUse) return pc_card_done(chan, CARD_RESULT_NOFILE);
	const PcCardEntry* e = &c->dir[fileNo];
	if (offset < 0 || (u32)(offset + length) > e->length) return pc_card_done(chan, CARD_RESULT_LIMIT);

	char path[640];
	pc_card_file_path(chan, e->name, ".bin", path, sizeof(path));
	FILE* f = fopen(path, "rb");
	if (!f) return pc_card_done(chan, CARD_RESULT_NOFILE);
	memset(buf, 0, (size_t)length); // short/absent tail reads as erased card
	if (fseek(f, offset, SEEK_SET) == 0) fread(buf, 1, (size_t)length, f);
	fclose(f);

	c->xferred = length;
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDWrite(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset) {
	if (!fileInfo || fileInfo->chan < 0) return CARD_RESULT_FATAL_ERROR;
	s32 chan         = fileInfo->chan;
	PcCardChannel* c = pc_chan(chan);
	if (!c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);
	// CARDWrite.c:104 — writes go a whole sector at a time.
	if ((offset % (s32)PC_CARD_SECTOR_SIZE) != 0 || (length % (s32)PC_CARD_SECTOR_SIZE) != 0) {
		return pc_card_done(chan, CARD_RESULT_FATAL_ERROR);
	}

	s32 fileNo = fileInfo->fileNo;
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE || !c->dir[fileNo].inUse) return pc_card_done(chan, CARD_RESULT_NOFILE);
	const PcCardEntry* e = &c->dir[fileNo];
	if (offset < 0 || (u32)(offset + length) > e->length) return pc_card_done(chan, CARD_RESULT_LIMIT);

	char path[640];
	pc_card_file_path(chan, e->name, ".bin", path, sizeof(path));
	FILE* f = fopen(path, "r+b");
	if (!f) return pc_card_done(chan, CARD_RESULT_NOFILE);
	bool ok = fseek(f, offset, SEEK_SET) == 0 && fwrite(buf, 1, (size_t)length, f) == (size_t)length;
	fflush(f);
	fclose(f);
	if (!ok) return pc_card_done(chan, CARD_RESULT_IOERROR);

	c->xferred = length;
	pc_card_touched(chan);
	CARD_TRACE("write chan%d slot%d off=%d len=%d\n", chan, fileNo, offset, length);
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDFastDelete(s32 chan, s32 fileNo) {
	PcCardChannel* c = pc_chan(chan);
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE) return CARD_RESULT_FATAL_ERROR;
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);

	s32 result = pc_card_access(c, fileNo);
	if (result < CARD_RESULT_READY) return pc_card_done(chan, result);

	pc_card_remove_files(chan, c->dir[fileNo].name);
	CARD_TRACE("delete chan%d slot%d \"%s\"\n", chan, fileNo, c->dir[fileNo].name);
	c->dir[fileNo].inUse = false;
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDDelete(s32 chan, const char* fileName) {
	PcCardChannel* c = pc_chan(chan);
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);
	s32 slot = pc_card_find_by_name(c, fileName);
	if (slot < 0) return pc_card_done(chan, CARD_RESULT_NOFILE);
	return CARDFastDelete(chan, slot);
}

s32 CARDRename(s32 chan, const char* oldName, const char* newName) {
	PcCardChannel* c = pc_chan(chan);
	if (strlen(newName) > CARD_FILENAME_MAX) return pc_card_done(chan, CARD_RESULT_NAMETOOLONG);
	if (pc_card_disabled() || !c || !c->mounted) return pc_card_done(chan, CARD_RESULT_NOCARD);

	s32 slot = pc_card_find_by_name(c, oldName);
	if (slot < 0) return pc_card_done(chan, CARD_RESULT_NOFILE);
	s32 existing = pc_card_find_by_name(c, newName);
	if (existing >= 0 && existing != slot) return pc_card_done(chan, CARD_RESULT_EXIST);

	char oldBin[640], newBin[640], oldStat[640];
	pc_card_file_path(chan, oldName, ".bin", oldBin, sizeof(oldBin));
	pc_card_file_path(chan, newName, ".bin", newBin, sizeof(newBin));
	pc_card_file_path(chan, oldName, ".stat", oldStat, sizeof(oldStat));
	if (rename(oldBin, newBin) != 0) return pc_card_done(chan, CARD_RESULT_IOERROR);
	unlink(oldStat); // the sidecar is keyed by name; rewrite under the new one

	PcCardEntry* e = &c->dir[slot];
	strncpy(e->name, newName, CARD_FILENAME_MAX);
	e->name[CARD_FILENAME_MAX] = '\0';
	if (!pc_card_write_stat(chan, slot, e)) return pc_card_done(chan, CARD_RESULT_IOERROR);

	CARD_TRACE("rename chan%d slot%d \"%s\" -> \"%s\"\n", chan, slot, oldName, newName);
	return pc_card_done(chan, CARD_RESULT_READY);
}

s32 CARDGetResultCode(s32 chan) {
	PcCardChannel* c = pc_chan(chan);
	if (!c) return CARD_RESULT_NOCARD;
	return c->result;
}

s32 CARDGetXferredBytes(s32 chan) {
	PcCardChannel* c = pc_chan(chan);
	return c ? c->xferred : 0;
}

// Internal hooks other translation units reference; nothing above the shim
// drives them, so keep them inert rather than pretending to succeed.
void __CARDMountCallback(s32, s32) { }
s32 __CARDReadStatus(s32, u8*) { return CARD_RESULT_NOCARD; }

} // extern "C"

// ---------------------------------------------------------------- self-test
// PC_CARD_SELFTEST=1 exercises the API the way the game does — including the
// temp-file save protocol from cardutil.cpp DoSave (create "~name", write,
// set status, rename) and a simulated relaunch — then exits. Deterministic
// and independent of the game's UI, so a regression here is caught without
// driving menus.
static int s_fails;

static void pc_card_check(bool ok, const char* what, s32 got, s32 want) {
	if (ok) {
		fprintf(stderr, "  ok   %s\n", what);
	} else {
		s_fails++;
		fprintf(stderr, "  FAIL %s (got %d, want %d)\n", what, got, want);
	}
}

extern "C" void pc_card_selftest(void) {
	fprintf(stderr, "[CARDTEST] start (root=%s)\n", pc_card_root());
	static u8 wbuf[0x2000], rbuf[0x2000];
	CARDFileInfo fi;
	CARDStat st;

	CARDInit();
	pc_card_check(CARDProbe(0) == TRUE, "probe chan0", CARDProbe(0), TRUE);

	s32 r = CARDFormat(0);
	pc_card_check(r == CARD_RESULT_READY, "format", r, CARD_RESULT_READY);
	r = CARDMount(0, nullptr, nullptr);
	pc_card_check(r == CARD_RESULT_READY, "mount", r, CARD_RESULT_READY);

	u32 sectorSize = 0;
	CARDGetSectorSize(0, &sectorSize);
	pc_card_check(sectorSize == PC_CARD_SECTOR_SIZE, "sector size", (s32)sectorSize, (s32)PC_CARD_SECTOR_SIZE);

	// empty card: every slot must report NOFILE
	int nonEmpty = 0;
	for (s32 i = 0; i < CARD_MAX_FILE; i++)
		if (CARDGetStatus(0, i, &st) >= 0) nonEmpty++;
	pc_card_check(nonEmpty == 0, "empty card has no readable slots", nonEmpty, 0);

	// --- the game's save protocol -------------------------------------
	const u32 fileSize = 6 * PC_CARD_SECTOR_SIZE;
	r                  = CARDCreate(0, "~pikmin1", fileSize, &fi);
	pc_card_check(r == CARD_RESULT_READY, "create temp file", r, CARD_RESULT_READY);
	s32 slot = fi.fileNo;

	// misaligned writes must be refused exactly like hardware
	r = CARDWrite(&fi, wbuf, 0x2000, 512);
	pc_card_check(r == CARD_RESULT_FATAL_ERROR, "reject unaligned write offset", r, CARD_RESULT_FATAL_ERROR);
	r = CARDRead(&fi, rbuf, 100, 0);
	pc_card_check(r == CARD_RESULT_FATAL_ERROR, "reject unaligned read length", r, CARD_RESULT_FATAL_ERROR);

	for (u32 i = 0; i < sizeof(wbuf); i++) wbuf[i] = (u8)(i * 7 + 3);
	r = CARDWrite(&fi, wbuf, 0x2000, 0);
	pc_card_check(r == CARD_RESULT_READY, "write block 0", r, CARD_RESULT_READY);
	r = CARDWrite(&fi, wbuf, 0x2000, 0x4000);
	pc_card_check(r == CARD_RESULT_READY, "write block 2", r, CARD_RESULT_READY);
	CARDClose(&fi);

	// status round trip, with banner+icons like MemoryCard::initBannerArea
	memset(&st, 0, sizeof(st));
	st.length       = fileSize;
	st.commentAddr  = 0;
	st.iconAddr     = 0x80;
	st.bannerFormat = CARD_STAT_BANNER_C8;
	CARDSetIconFormat(&st, 0, CARD_STAT_ICON_C8);
	CARDSetIconSpeed(&st, 0, CARD_STAT_SPEED_SLOW);
	r = CARDSetStatus(0, slot, &st);
	pc_card_check(r == CARD_RESULT_READY, "set status", r, CARD_RESULT_READY);

	r = CARDRename(0, "~pikmin1", "pikmin1");
	pc_card_check(r == CARD_RESULT_READY, "rename temp -> final", r, CARD_RESULT_READY);

	// --- read back ----------------------------------------------------
	memset(&st, 0, sizeof(st));
	r = CARDGetStatus(0, slot, &st);
	pc_card_check(r == CARD_RESULT_READY, "get status after rename", r, CARD_RESULT_READY);
	pc_card_check(strcmp(st.fileName, "pikmin1") == 0, "status name", 0, 0);
	pc_card_check(st.length == fileSize, "status length", (s32)st.length, (s32)fileSize);
	// derived offsets: C8 banner at iconAddr, then its 512-entry TLUT
	pc_card_check(st.offsetBanner == 0x80, "derived offsetBanner", (s32)st.offsetBanner, 0x80);
	pc_card_check(st.offsetBannerTlut == 0x80 + 96 * 32, "derived offsetBannerTlut", (s32)st.offsetBannerTlut, 0x80 + 96 * 32);
	pc_card_check(st.offsetData > st.offsetIcon[0], "derived offsetData past icons", (s32)st.offsetData, (s32)st.offsetIcon[0]);

	r = CARDFastOpen(0, slot, &fi);
	pc_card_check(r == CARD_RESULT_READY, "fast open", r, CARD_RESULT_READY);
	memset(rbuf, 0, sizeof(rbuf));
	r = CARDRead(&fi, rbuf, 0x2000, 0x4000);
	pc_card_check(r == CARD_RESULT_READY && memcmp(rbuf, wbuf, sizeof(rbuf)) == 0, "read back block 2", r, CARD_RESULT_READY);
	memset(rbuf, 0xAB, sizeof(rbuf));
	r = CARDRead(&fi, rbuf, 0x2000, 0x2000);
	bool blank = true;
	for (u32 i = 0; i < sizeof(rbuf); i++)
		if (rbuf[i] != 0) blank = false;
	pc_card_check(r == CARD_RESULT_READY && blank, "unwritten block reads as zero", r, CARD_RESULT_READY);
	CARDClose(&fi);

	// open by name must find the same slot
	r = CARDOpen(0, "pikmin1", &fi);
	pc_card_check(r == CARD_RESULT_READY && fi.fileNo == slot, "open by name -> same slot", fi.fileNo, slot);
	CARDClose(&fi);

	s32 bytesFree = 0, filesFree = 0;
	CARDFreeBlocks(0, &bytesFree, &filesFree);
	pc_card_check(bytesFree == (PC_CARD_BLOCKS - 6) * (s32)PC_CARD_SECTOR_SIZE, "free bytes after 6 blocks", bytesFree,
	              (PC_CARD_BLOCKS - 6) * (s32)PC_CARD_SECTOR_SIZE);
	pc_card_check(filesFree == CARD_MAX_FILE - 1, "free files", filesFree, CARD_MAX_FILE - 1);

	// --- simulated relaunch: drop all state, remount from disk --------
	CARDUnmount(0);
	memset(s_chan, 0, sizeof(s_chan));
	r = CARDMount(0, nullptr, nullptr);
	pc_card_check(r == CARD_RESULT_READY, "remount", r, CARD_RESULT_READY);
	memset(&st, 0, sizeof(st));
	r = CARDGetStatus(0, slot, &st);
	pc_card_check(r == CARD_RESULT_READY, "file survives relaunch at same slot", r, CARD_RESULT_READY);
	pc_card_check(strcmp(st.fileName, "pikmin1") == 0, "name survives", 0, 0);
	pc_card_check(st.iconAddr == 0x80 && st.bannerFormat == CARD_STAT_BANNER_C8, "banner/icon metadata survives", (s32)st.iconAddr,
	              0x80);
	r = CARDFastOpen(0, slot, &fi);
	memset(rbuf, 0, sizeof(rbuf));
	r = CARDRead(&fi, rbuf, 0x2000, 0x4000);
	pc_card_check(r == CARD_RESULT_READY && memcmp(rbuf, wbuf, sizeof(rbuf)) == 0, "data survives relaunch", r, CARD_RESULT_READY);
	CARDClose(&fi);

	// --- overwrite path: same name again must report EXIST ------------
	r = CARDCreate(0, "pikmin1", fileSize, &fi);
	pc_card_check(r == CARD_RESULT_EXIST, "duplicate create -> EXIST", r, CARD_RESULT_EXIST);

	// --- delete -------------------------------------------------------
	r = CARDFastDelete(0, slot);
	pc_card_check(r == CARD_RESULT_READY, "delete", r, CARD_RESULT_READY);
	r = CARDGetStatus(0, slot, &st);
	pc_card_check(r == CARD_RESULT_NOFILE, "deleted slot -> NOFILE", r, CARD_RESULT_NOFILE);
	CARDFreeBlocks(0, &bytesFree, &filesFree);
	pc_card_check(bytesFree == PC_CARD_BLOCKS * (s32)PC_CARD_SECTOR_SIZE, "space reclaimed", bytesFree,
	              PC_CARD_BLOCKS * (s32)PC_CARD_SECTOR_SIZE);

	fprintf(stderr, "[CARDTEST] %s (%d failure%s)\n", s_fails ? "FAILED" : "PASSED", s_fails, s_fails == 1 ? "" : "s");
}
