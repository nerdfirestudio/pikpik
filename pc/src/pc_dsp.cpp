// pc_dsp.cpp — host side of the DSP mailbox (M5 P2).
//
// P2 scope is the SEAM ONLY: capture the shared tables, take over DsyncFrame,
// and emit silence. The actual voice synthesis (VPB decode/resample/mix, then
// the FX blocks) is P3/P4 and lands in this file behind pc_dsp_sync_frame.
//
// Licence note, per docs/ARCHITECTURE.md: Dolphin's
// Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp is the authoritative reference
// for what this ucode does, and the decomp already borrows its field names
// (dspinterface.h says so outright). Dolphin is GPLv2+, so it is used here as
// a SEMANTIC reference only — describing behaviour, not copying code — which
// is the same relationship the decomp itself has to it.

#include "pc_host_compat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The jaudio headers are C++ (bx.h carries templates), so they must not sit
// inside an extern "C" block; only the SDK surface does, matching pc_audio.cpp.
#include "jaudio/dspinterface.h"
#include "jaudio/pc_dsp.h"
#include "jaudio/rate.h"

// Defined in src/jaudio/dspdriver.c (compiled as C++, so C++ linkage).
u32 pc_dspch_in_use(void);
u32 pc_dspch_stopping(void);
u32 pc_dspch_probe(u32 idx, const void** owner, int* ownerSoundId, int* ownerPointsBack, int* hasCb);
extern u32 g_pc_cmdq_sent, g_pc_cmdq_delivered, g_pc_cmdq_blocked;

extern "C" {
#include "Dolphin/os.h"
void pc_os_raise_interrupt(int interrupt);
}

// Dolphin/OS/OSInterrupt.h's __OS_INTERRUPT_DSP_DSP.
#define PC_INTERRUPT_DSP_DSP 7

// ---------------------------------------------------------- shared tables
// Registered once at boot by DsetupTable. These are plain host structs that
// jaudio's CPU-side code (dspinterface.c, dspdriver.c) writes into every
// frame; the renderer only ever reads them.

static DSPchannel_* s_chBuf;
static u32 s_chBufLength;
static FXBuffer* s_fxBuf;
static void* s_resFilter;
static void* s_adpcmFilter;
static bool s_setupDone;

static u64 s_frameCount;

extern "C" void pc_dsp_setup_table(u32 chBufLength, void* chBuf, void* resFilter, void* adpcmFilter, void* fxBuf)
{
	s_chBufLength = chBufLength;
	s_chBuf       = (DSPchannel_*)chBuf;
	s_resFilter   = resFilter;
	s_adpcmFilter = adpcmFilter;
	s_fxBuf       = (FXBuffer*)fxBuf;
	s_setupDone   = true;

	// The VPB stride is the one thing a silent renderer can still get wrong in
	// a way that only shows up much later, so state it now.
	OSReport("[DSP] tables registered: CH_BUF %p x%u (stride 0x%zx), FX_BUF %p, res %p, adpcm %p\n", chBuf, chBufLength,
	         sizeof(DSPchannel_), fxBuf, resFilter, adpcmFilter);
}


// ============================================================ voice renderer
// P3: dry voices only — fetch, decode, resample, ramp, mix. No per-voice
// filters and no FX blocks yet (P4).
//
// Semantics follow Dolphin's Zelda HLE, which is what this ucode family is and
// whose field names the decomp already uses. Semantic reference only; see the
// licence note at the top of this file.

extern "C" u8* pc_aram_base(void);
extern "C" u32 pc_aram_size(void);

// samplesSourceType is COMP_BLOCKBYTES[wave->compBlockIdx] — the bytes per
// source block — which doubles as the format tag (dspinterface.c:145).
// Anything below 4 is one of the ucode's synthetic generators, and
// DSP_SetWaveInfo deliberately skips wave setup for those.
#define SRC_AFC_LQ 5  // 5-byte blocks, 16 samples: 2-bit ADPCM
#define SRC_PCM8   8
#define SRC_AFC_HQ 9  // 9-byte blocks, 16 samples: 4-bit ADPCM
#define SRC_PCM16  16
// syncstream.c's Play_DirectPCM: 16-bit PCM streamed from MAIN MEMORY.
#define SRC_DIRECT_PCM 0x21

// Host pointers for direct-PCM voices, kept out of the VPB (see pc_dsp.h).
static s16* s_directPcm[64];

// Scene-transition voice audit: mark every currently-enabled voice, then
// report which of them are STILL enabled some frames later. A voice that
// outlives the section that started it is precisely the "previous section
// failed to stop it" case.
static u8 s_markedAtScene[64];
static u64 s_markFrame;
static bool s_auditPending;

extern "C" void pc_dsp_mark_scene_change(void)
{
	if (!s_chBuf) {
		return;
	}
	for (u32 i = 0; i < s_chBufLength && i < 64; i++) {
		s_markedAtScene[i] = s_chBuf[i].enabled ? 1 : 0;
	}
	s_markFrame    = s_frameCount;
	s_auditPending = true;
}

// ------------------------------------------------- looping-voice attribution
// PC_DSP_LOOPTAG=1. See pc_dsp.h: every jc_ that receives a DSP channel tags
// it here, so when a looping voice persists the report names its owner instead
// of leaving another round of code-reading.

struct VoiceTag {
	u32 soundValue;    // full SOUNDID_ (bank/prog/key/vel); 0 when unknown
	int soundId;       // jc_::soundId snapshot (the mono/poly key)
	const void* owner; // the jc_ (or stream slot) the channel was allocated to
	u64 frame;         // s_frameCount at tag time
	u8 note;
	char kind;         // see pc_dsp.h; 0 = never tagged
};
static VoiceTag s_voiceTag[64];

static int pc_looptag_on(void)
{
	static int on = -1;
	if (on < 0) {
		on = getenv("PC_DSP_LOOPTAG") ? 1 : 0;
	}
	return on;
}

extern "C" void pc_dsp_tag_voice(u32 idx, u32 soundValue, int soundId, u8 note, char kind, const void* owner)
{
	if (idx >= 64) {
		return;
	}
	VoiceTag* t   = &s_voiceTag[idx];
	t->soundValue = soundValue;
	t->soundId    = soundId;
	t->owner      = owner;
	t->frame      = s_frameCount;
	t->note       = note;
	t->kind       = kind;
}

extern "C" void pc_dsp_untag_voice(u32 idx)
{
	if (idx < 64) {
		s_voiceTag[idx].kind = 0;
	}
}

// Seconds of audio rendered since boot — the wav-dump clock, for correlating
// driver-side events with listening timestamps.
extern "C" double pc_dsp_time(void)
{
	return (double)s_frameCount * (560.0 / 32000.0);
}

extern "C" void pc_dsp_note_dealloc_miss(u32 idx, const void* callerId, const void* owner)
{
	if (!pc_looptag_on()) {
		return;
	}
	static u32 n;
	if (++n <= 60) {
		const VoiceTag* t = idx < 64 ? &s_voiceTag[idx] : NULL;
		OSReport("[DEALLOC-MISS] ch%02u dealloc refused: caller id=%p owner=%p (tag %c sound=0x%08x age=%llu)\n", idx, callerId,
		         owner, t && t->kind ? t->kind : '-', t ? t->soundValue : 0,
		         t ? (unsigned long long)(s_frameCount - t->frame) : 0ull);
	}
}

// Called from the render loop: report every looping voice still enabled, with
// its tag and whether the driver side still owns it. ~3 s cadence (57 fps).
static void pc_looptag_report(DSPchannel_* chBuf, u32 chBufLength)
{
	if (!pc_looptag_on() || (s_frameCount % 171) != 0) {
		return;
	}
	u32 nLoop = 0;
	for (u32 i = 0; i < chBufLength && i < 64; i++) {
		DSPchannel_* v = &chBuf[i];
		if (!v->enabled || !v->isLooping) {
			continue;
		}
		nLoop++;
		const void* owner = NULL;
		int ownerSound = 0, back = 0, cb = 0;
		u32 state         = pc_dspch_probe(i, &owner, &ownerSound, &back, &cb);
		const VoiceTag* t = &s_voiceTag[i];
		OSReport("[LOOPTAG] ch%02u age=%-5llu base=0x%08x vol=%d endReq=%u %c sound=%02x/%02x/%02x/%02x id=0x%08x "
		         "state=%u cb=%d owner=%p%s ownerId=0x%08x%s fmode=0x%x bq=[%d %d %d %d] lp=%d\n",
		         i, (unsigned long long)(s_frameCount - t->frame), v->baseAddress,
		         (int)(s16)v->mixChannels[0].currentVolume, v->endRequested, t->kind ? t->kind : '-',
		         (t->soundValue >> 24) & 0xff, (t->soundValue >> 16) & 0xff, (t->soundValue >> 8) & 0xff,
		         t->soundValue & 0xff, (u32)t->soundId, state, cb, owner,
		         owner == t->owner ? "" : " RECYCLED", (u32)ownerSound, back ? "" : " NOBACK",
		         (unsigned)(u16)v->filterMode, (int)(s16)v->biquadFilterCoeffs[0], (int)(s16)v->biquadFilterCoeffs[1],
		         (int)(s16)v->biquadFilterCoeffs[2], (int)(s16)v->biquadFilterCoeffs[3], (int)(s16)v->lowPassCoeff);
	}
	if (nLoop) {
		OSReport("[LOOPTAG] %u looping voice(s) enabled, dspch in use %u/64\n", nLoop, pc_dspch_in_use());
	}
}

// The ucode's final-mix scale. 0x4000 is the reset default; the game lowers it
// to 8192 via Jac_SetMixerLevel(1.1, 2.0) -> DsetMixerLevel(2.0).
static u16 s_mixerLevel = 0x4000;

extern "C" void pc_dsp_set_mixer_level(u16 level) { s_mixerLevel = level ? level : 0x4000; }

// A wave archive's ARAM heap is being freed. Any voice still reading inside it
// is about to decode whatever the next archive loads over that memory.
//
// The game does not stop these itself — a scene-change audit shows every
// survivor with endRequested == 0, i.e. it never asks. On hardware the same
// voices would read the same reused ARAM, so this is a safety net rather than
// a faithful reproduction; but playing from freed memory is wrong under any
// reading, and it is what makes a released archive audible as noise (a voice
// was observed decoding ARAM that had been overwritten with ASCII text).
//
// endRequested is used rather than clearing `enabled` so the voice ramps out
// over one frame and the driver reclaims the channel through its normal path.
// A wave archive's ARAM is being MOVED by heap compaction
// (Jac_GarbageCollection_St): when an archive is freed, every archive above
// the hole slides down to close it. The data moves; a voice already playing
// out of it does not, because nothing fixes up its addresses — so it keeps
// reading the old location, which the next archive promptly loads into.
//
// That is the menu static: right pitch and envelope (the voice params are
// intact), someone else's samples. Measured by parking on the save-select
// screen — spectral flatness 0.0155 against a Dolphin reference of 0.0035.
//
// baseAddress and currentAramAddr are ARAM byte addresses and shift with the
// block. loopAddress is NOT an address despite the name — it is a loop-start
// SAMPLE index (see dspinterface.h:108) — so it must be left alone.
extern "C" void pc_dsp_relocate_voices_in_aram(u32 src, u32 dst, u32 size)
{
	if (!s_chBuf || !size || src == dst) {
		return;
	}
	const u32 end   = src + size;
	const s64 delta = (s64)dst - (s64)src;
	u32 moved       = 0;
	for (u32 i = 0; i < s_chBufLength; i++) {
		DSPchannel_* v = &s_chBuf[i];
		if (!v->enabled) {
			continue;
		}
		if (v->baseAddress >= src && v->baseAddress < end) {
			v->baseAddress = (u32)((s64)v->baseAddress + delta);
			if (v->currentAramAddr >= src && v->currentAramAddr < end) {
				v->currentAramAddr = (u32)((s64)v->currentAramAddr + delta);
			}
			moved++;
		}
	}
	if (moved && getenv("PC_DSP_SURVEY")) {
		OSReport("[ARAMMOVE] relocated %u voice(s) 0x%08x -> 0x%08x\n", moved, src, dst);
	}
}

extern "C" void pc_dsp_stop_voices_in_aram(u32 start, u32 size)
{
	if (!s_chBuf || !size) {
		return;
	}
	const u32 end = start + size;
	u32 stopped   = 0;
	for (u32 i = 0; i < s_chBufLength; i++) {
		DSPchannel_* v = &s_chBuf[i];
		if (!v->enabled || v->endRequested) {
			continue;
		}
		if (v->currentAramAddr >= start && v->currentAramAddr < end) {
			v->endRequested = DSP_TRUE;
			stopped++;
		}
	}
	if (stopped && getenv("PC_DSP_SURVEY")) {
		OSReport("[ARAMFREE] stopped %u voice(s) reading the released region 0x%08x..0x%08x\n", stopped, start, end);
	}
}

extern "C" void pc_dsp_set_direct_pcm(u8 idx, s16* addr)
{
	if (idx < 64) {
		s_directPcm[idx] = addr;
	}
}

// Bus ids from connect_table[] in DSP_SetBusConnect. These are DSP DMEM
// addresses; the two main ones are all P3 needs, and the rest (aux, reverb,
// dolby) are P4.
#define BUS_MAIN_L 0x0D00
#define BUS_MAIN_R 0x0D60

// Running ADPCM predictor state. It CANNOT live in loopYN1/loopYN2: those are
// the predictor state at the loop point, written once by DSP_SetWaveInfo from
// the wave header, and they have to survive playback so a loop can restart
// correctly. resampleBuffer[0..1] are unused by this renderer (only [2..3]
// carry the interpolator's straddling samples) and DSP_PlayStart zeroes all
// four, which is exactly the right initial predictor state.
#define PC_AFC_YN1(v) ((v)->resampleBuffer[0])
#define PC_AFC_YN2(v) ((v)->resampleBuffer[1])

static const s16* pc_aram_at(u32 offset)
{
	if (offset >= pc_aram_size()) {
		return NULL;
	}
	return (const s16*)(pc_aram_base() + offset);
}

// AFC decode. Both variants pack 16 samples per block behind a one-byte
// header: the top nibble is a coefficient-pair index into DSPADPCM_FILTER, the
// bottom nibble a shift. HQ spends 4 bits per sample over 8 bytes, LQ 2 bits
// over 4 bytes.
// startIdx > 0 enters the block partway through: nibbles before it are skipped
// entirely and the recurrence begins at startIdx with the CALLER'S yn1/yn2. That
// is what a mid-block loop point needs — the wave header's loopYN1/YN2 are the
// history AT the loop sample, not at the start of its block, so re-deriving them
// by decoding the leading samples would be wrong even if they were discarded.
// out[0..startIdx) is left untouched.
static void pc_afc_decode_block(const u8* src, int bytesPerBlock, const u32* coefTable, s16* out, s16& yn1, s16& yn2,
                                int startIdx = 0)
{
	// AFC's header byte is the OPPOSITE way round from standard GC DSPADPCM:
	// the high nibble is the SCALE and the low nibble is the coefficient
	// index. Having these swapped still produced structured, recognisable
	// audio — just badly distorted — which is why it read as "garbled speech"
	// rather than as noise.
	const u8 header  = src[0];
	const int scale  = (header >> 4) & 0xF;
	const int idx    = header & 0xF;

	// DSPADPCM_FILTER is a u32[16] declared in C (dspinterface.c), each entry
	// packing TWO 1.11 coefficients with c1 in the HIGH half — e.g. 0x1000F800
	// is c1=+4096, c2=-2048. Casting the table to s16* and reading pairs picks
	// them up in the opposite order on a little-endian host, and swapped
	// predictor coefficients make the ADPCM filter diverge: the output is
	// static rather than sound. Unpack the halves explicitly.
	const u32 packed = coefTable[idx];
	const s16 c1     = (s16)(packed >> 16);
	const s16 c2     = (s16)(packed & 0xFFFF);

	int nibbles[16];
	if (bytesPerBlock == 9) {
		for (int i = 0; i < 8; i++) {
			const u8 b        = src[1 + i];
			nibbles[i * 2 + 0] = (int)(s8)(b & 0xF0) >> 4;
			nibbles[i * 2 + 1] = (int)(s8)((b & 0x0F) << 4) >> 4;
		}
	} else {
		// LQ's 2-bit samples carry an extra x4 the header scale does not
		// express: Dolphin's Zelda-ucode HLE places HQ nibbles as
		// (n << 12) >> 1 (= n << 11) but LQ ones as (n << 14) >> 1 (= n << 13).
		// Without it the deltas are 12 dB under the predictor feedback and the
		// output is mostly predictor ringing — a muffled rumble.
		for (int i = 0; i < 4; i++) {
			const u8 b = src[1 + i];
			for (int j = 0; j < 4; j++) {
				const int two      = (b >> (6 - j * 2)) & 3;
				nibbles[i * 4 + j] = ((two & 2) ? (two - 4) : two) << 2;
			}
		}
	}

	for (int i = startIdx; i < 16; i++) {
		// VERIFIED against a REAL AFC source dumped from ARAM (PC_DUMP_AFC +
		// tools/afc_probe.py). The discriminator is DC DRIFT, not smoothness:
		// a wrong predictor integrates, producing a signal that is smooth but
		// is mostly wander. Over 900 blocks of the boot voice:
		//
		//   variant             mean   maxDCdrift     rms
		//   scale=hi shifted   -16.2        142.5  9570.3   <- correct
		//   scale=hi delta*n  -240.5       1154.3  1146.3   drift ~= signal
		//   scale=lo *         huge       8700+          clamps 10-15%
		//
		// So the nibble is shifted by the scale AND into the coefficients'
		// 1.11 fixed point, and the single >>11 below normalises the sum.
		s32 sample = ((s32)nibbles[i] << scale) << 11;
		sample += (s32)yn1 * c1 + (s32)yn2 * c2;
		sample >>= 11;
		if (sample > 32767) sample = 32767;
		if (sample < -32768) sample = -32768;
		out[i] = (s16)sample;
		yn2    = yn1;
		yn1    = (s16)sample;
	}
}

// FX/reverb plumbing lives after the voice renderer; the send-routing in
// pc_render_voice needs these two.
static s32* pc_aux_bus_for(u16 id);
static int pc_fx_on(void);

// Resampler history. The 4-tap filter needs s[-1] plus two samples of
// lookahead, and all four resampleBuffer slots in the VPB are already spoken
// for (two by the AFC predictor, see PC_AFC_YN1/YN2), so it lives in a side
// table keyed by voice index — the same arrangement as s_directPcm. Cleared
// when a voice starts so a new sound never inherits the previous one's tail.
#define PC_RES_HIST 4u
static s16 s_resHist[64][PC_RES_HIST];
static s16 s_resHistSpill[PC_RES_HIST];

// PC_DSP_TAP=<hex baseAddress>: dump one voice's runtime streams — the decoded
// source samples (pre-resample) and the resampled output — for offline
// comparison against a direct decode of the same ARAM bytes.
static u32 s_tapBase;
static FILE* s_tapSrcFile;
static FILE* s_tapResFile;

static bool pc_tap_match(const DSPchannel_* v)
{
	static int init = -1;
	if (init < 0) {
		const char* t = getenv("PC_DSP_TAP");
		s_tapBase     = t ? (u32)strtoul(t, NULL, 16) : 0;
		if (s_tapBase) {
			s_tapSrcFile = fopen("tap_src.raw", "wb");
			s_tapResFile = fopen("tap_res.raw", "wb");
		}
		init = 1;
	}
	return s_tapBase && v->baseAddress == s_tapBase;
}

// One s16 of DSPRES_FILTER, which is declared u32[] with the first coefficient
// of each pair in the HIGH half.
static inline s16 pc_res_coef(const u32* tbl, int idx)
{
	const u32 w = tbl[idx >> 1];
	return (idx & 1) ? (s16)(w & 0xFFFF) : (s16)(w >> 16);
}

// Produce `count` source samples for this voice starting at its current
// position, advancing the VPB's decode state. Returns how many were written;
// short means the voice ran out.
static u32 pc_voice_fetch(DSPchannel_* v, s16* dst, u32 count)
{
	const u16 type = v->samplesSourceType;

	if (v->useConstantSample) {
		for (u32 i = 0; i < count; i++) {
			dst[i] = v->constantSample;
		}
		return count;
	}

	u32 produced = 0;
	while (produced < count) {
		if (v->remainingLength == 0) {
			static int noloop = -1;
			if (noloop < 0) {
				noloop = getenv("PC_DSP_NOLOOP") ? 1 : 0;
			}
			if (!v->isLooping || noloop) {
				v->endReached = DSP_TRUE;
				break;
			}
			// LOOP POINTS. The decomp's field names here are WRONG, and believing
			// them is what produced the honking. Wave_ is the published wsys wave
			// record, whose tail is:
			//
			//   _10 loop (bool)   _14 loopStart   _18 loopEnd   _1C sampleCount
			//
			// so what the decomp calls `loopAddress` (_14) is the loop START
			// SAMPLE and what it calls `loopStartPosition` (_18) is the loop END
			// SAMPLE. Neither is an address; the names are left alone only to keep
			// this file diffable against the decomp.
			//
			// An earlier pass here got half of this right — it correctly rejected
			// loopAddress-as-an-ARAM-address (taken literally it aimed the cursor
			// at 0x24c0 for a wave based at 0x3e160, thousands of bytes below the
			// wave's own data) but then fell back on _18 as the loop start. _18 is
			// the END, and it always sits just under sampleCount, so every loop
			// became a handful of samples at the very tail: 23153..23218 = 65
			// samples, which at 32 kHz is a ~490 Hz tone that repeats forever.
			// THAT is the honking on the data-file, area-select and card-create
			// screens, and it also explains why every scene-change survivor had
			// endRequested == 0 — the game never asked those voices to stop
			// because they were supposed to END on their own, and a voice stuck in
			// a 65-sample tail loop never reaches its end, so `done` never fires
			// and the driver never reclaims the channel.
			//
			// Read as intended, the same wave loops 11094..23153 — 12059 samples,
			// 0.38 s — which is an ordinary sustained-SFX loop.
			const u32 loopEnd = (v->loopStartPosition && (u32)v->loopStartPosition <= (u32)v->_11C)
			                        ? (u32)v->loopStartPosition
			                        : (u32)v->_11C;
			const u32 loopStart = ((u32)v->loopAddress < loopEnd) ? (u32)v->loopAddress : 0u;
			if (loopEnd <= loopStart) {
				v->endReached = DSP_TRUE;
				break;
			}

			v->currentPosition = loopStart;

			// Restore the ADPCM predictor to the state it had AT the loop point.
			// loopYN1/loopYN2 are set once by DSP_SetWaveInfo from the wave
			// header and must survive playback, so the running state lives
			// separately (see PC_AFC_YN1/YN2) — an earlier version used
			// loopYN1/YN2 as the running state, which overwrote the restart
			// values and made every loop continue from wherever the previous
			// pass happened to end.
			// PC_DSP_LOOPYN=0: A/B the loop-restart predictor. If keeping the
			// RUNNING state sounds right and restoring loopYN1/YN2 does not,
			// those header values are being read wrong.
			static int useLoopYn = -1;
			if (useLoopYn < 0) {
				const char* e = getenv("PC_DSP_LOOPYN");
				useLoopYn     = (e && e[0] == '0') ? 0 : 1;
			}
			if (useLoopYn) {
				PC_AFC_YN1(v) = v->loopYN1;
				PC_AFC_YN2(v) = v->loopYN2;
			}
			v->afcRemainingDecodedSamples = 0;

			switch (v->samplesSourceType) {
			case SRC_PCM16:
				v->currentAramAddr = v->baseAddress + loopStart * 2u;
				break;
			case SRC_AFC_HQ:
			case SRC_AFC_LQ: {
				// AFC is stored in 16-sample blocks but loopStartPosition is NOT
				// block-aligned (18747 on the wave that exposed this). Snapping
				// the cursor down to the containing block and then decoding that
				// block whole replays up to 15 samples from BEFORE the loop point
				// on EVERY pass. For a short tail loop those replayed samples are
				// most of what you hear, and they are decoded against a predictor
				// belonging to a later sample — a periodic, mistuned buzz that
				// restarts every block. Decode the block once here and enter it
				// at the loop sample instead.
				const u32 bb  = (v->samplesSourceType == SRC_AFC_HQ) ? 9u : 5u;
				const u32 sub = loopStart % 16u;

				// PC_DSP_LOOPSTAT=1: how often AFC loops land mid-block, and how
				// short the looped region is. A misaligned restart over a short
				// loop is the shape that buzzes.
				static int lstat = -1;
				if (lstat < 0) lstat = getenv("PC_DSP_LOOPSTAT") ? 1 : 0;
				if (lstat) {
					static u32 nTot, nMis, nShown;
					nTot++;
					if (sub) nMis++;
					if (nShown < 10) {
						nShown++;
						OSReport("[LOOPSTAT] loop %u..%u (%u smp) of %u, sub=%u type=%u base=0x%x  yn1=%d yn2=%d\n",
						         loopStart, loopEnd, loopEnd - loopStart, (u32)v->_11C, sub,
						         v->samplesSourceType, v->baseAddress, (int)(s16)v->loopYN1, (int)(s16)v->loopYN2);
					}
					if ((nTot & 0x3FF) == 0) {
						OSReport("[LOOPSTAT] %u AFC loop restarts, %u mid-block (%u%%)\n", nTot, nMis, nMis * 100 / nTot);
					}
				}

				v->currentAramAddr = v->baseAddress + (loopStart / 16u) * bb;
				if (sub) {
					const u8* lsrc = (const u8*)pc_aram_at(v->currentAramAddr);
					if (lsrc) {
						s16 tmp[16];
						s16 y1 = v->loopYN1;
						s16 y2 = v->loopYN2;
						// loopYN1/YN2 are the predictor state at the CONTAINING
						// BLOCK'S BOUNDARY, not at the loop sample. Proven against
						// the data: for select_0.aw's wave 3 (the one mid-block
						// loop on the file-select screen, loop 11094 = block
						// 693 + 6) a linear decode's state before sample 11088 is
						// exactly the header pair (10378, 8618), while the state
						// before 11094 is (2235, 5213). Every block-ALIGNED loop
						// makes the two conventions coincide, which is how the
						// earlier "enter at `sub` with header YN" version
						// survived v75's fixes — and why this one wave played
						// every loop pass as full-scale predictor garbage: the
						// menu static. Decode the whole block from the boundary
						// seed and discard the lead-in samples instead.
						pc_afc_decode_block(lsrc, (int)bb, (const u32*)s_adpcmFilter, tmp, y1, y2, 0);
						PC_AFC_YN1(v) = y1;
						PC_AFC_YN2(v) = y2;
						for (u32 k = sub; k < 16u; k++) {
							v->afcRemainingSamples[k] = (u16)tmp[k];
						}
						// off = 16 - avail below then indexes straight to `sub`.
						v->afcRemainingDecodedSamples = (u16)(16u - sub);
						v->currentAramAddr += bb;
					}
				}
				break;
			}
			default: // PCM8: one byte per sample
				v->currentAramAddr = v->baseAddress + loopStart;
				break;
			}
			v->remainingLength = loopEnd - loopStart;

			if (v->remainingLength == 0) {
				v->endReached = DSP_TRUE;
				break;
			}
		}

		u32 want = count - produced;
		if (want > v->remainingLength) {
			want = v->remainingLength;
		}

		if (type == SRC_DIRECT_PCM) {
			// Already host-endian s16 in main memory — the CPU decoded it.
			// The VPB has no room for a host pointer, so recover the voice index
			// from its position in CH_BUF and look the buffer up.
			const u32 vidx = (u32)(v - s_chBuf);
			const s16* src = (vidx < 64) ? s_directPcm[vidx] : NULL;
			if (!src) break;
			// This buffer is a RING that the CPU refills behind the read cursor,
			// not a linear sample run: Play_DirectPCM passes its length as
			// loopStart, stored as loopStartPosition = loopStart << 16. Reading
			// straight through it ran off the end of the ring after one lap.
			//
			// And for this source type currentPosition is 16.16 within the ring,
			// not a sample index — Get_DirectPCM_Counter returns
			// currentPosition >> 16, and syncstream.c derives which quarter of the
			// ring to refill from it, so the scale has to be right or the producer
			// refills the wrong region.
			const u32 ring = (u32)v->loopStartPosition >> 16;
			const u32 pos  = (u32)v->currentPosition >> 16;
			for (u32 i = 0; i < want; i++) {
				dst[produced + i] = ring ? src[(pos + i) % ring] : src[pos + i];
			}
		} else if (type == SRC_PCM16) {
			const s16* src = pc_aram_at(v->currentAramAddr);
			if (!src) break;
			for (u32 i = 0; i < want; i++) {
				dst[produced + i] = (s16)__builtin_bswap16((u16)src[i]);
			}
			v->currentAramAddr += want * 2;
		} else if (type == SRC_PCM8) {
			const s8* src = (const s8*)pc_aram_at(v->currentAramAddr);
			if (!src) break;
			for (u32 i = 0; i < want; i++) {
				dst[produced + i] = (s16)((int)src[i] << 8);
			}
			v->currentAramAddr += want;
		} else if (type == SRC_AFC_HQ || type == SRC_AFC_LQ) {
			// AFC decodes in 16-sample blocks, but a frame rarely wants a whole
			// number of them. The leftover MUST persist across calls — the VPB
			// carries afcRemainingSamples[16] + afcRemainingDecodedSamples for
			// exactly this. Decoding a fresh block per call and throwing the
			// unconsumed tail away (while still advancing currentAramAddr) drops
			// samples out of the middle of the stream, which is audible as
			// garbled speech rather than silence.
			const int blockBytes = (type == SRC_AFC_HQ) ? 9 : 5;

			if (v->afcRemainingDecodedSamples == 0) {
				const u8* src = (const u8*)pc_aram_at(v->currentAramAddr);
				if (!src) break;
				s16 blk[16];
				s16 yn1 = PC_AFC_YN1(v);
				s16 yn2 = PC_AFC_YN2(v);
				pc_afc_decode_block(src, blockBytes, (const u32*)s_adpcmFilter, blk, yn1, yn2);
				PC_AFC_YN1(v) = yn1;
				PC_AFC_YN2(v) = yn2;
				for (int k = 0; k < 16; k++) {
					v->afcRemainingSamples[k] = (u16)blk[k];
				}
				v->afcRemainingDecodedSamples = 16;
				v->currentAramAddr += blockBytes;
			}

			const u32 avail = v->afcRemainingDecodedSamples;
			const u32 take  = want < avail ? want : avail;
			const u32 off   = 16u - avail;
			for (u32 i = 0; i < take; i++) {
				dst[produced + i] = (s16)v->afcRemainingSamples[off + i];
			}
			v->afcRemainingDecodedSamples -= (u16)take;
			want = take;
		} else {
			// Synthetic generators (square/saw/constant patterns) are P4.
			for (u32 i = 0; i < want; i++) {
				dst[produced + i] = 0;
			}
		}

		produced += want;
		v->remainingLength -= want;
		if (type == SRC_DIRECT_PCM) {
			const u32 ring = (u32)v->loopStartPosition >> 16;
			u32 np         = ((u32)v->currentPosition >> 16) + want;
			if (ring) {
				np %= ring;
			}
			v->currentPosition = np << 16; // 16.16, see the ring note above
		} else {
			v->currentPosition += want;
		}
	}

	for (u32 i = produced; i < count; i++) {
		dst[i] = 0;
	}
	return produced;
}

// One voice into the two main buses.
static void pc_render_voice(DSPchannel_* v, s32* busL, s32* busR, u32 n)
{
	// DSP_PlayStart only sets resetVpb; on hardware the UCODE is what turns
	// baseAddress into the live read cursor. Nothing on the CPU side ever
	// writes currentAramAddr/remainingLength, which is why a freshly started
	// voice arrives here with aram=0 remain=0 and renders silence.
	if (v->resetVpb) {
		{
			// One-shot dump of a real AFC source so the decoder can be validated
			// offline against genuine data (tools/afc_probe.py).
			static bool dumped;
			if (!dumped && v->samplesSourceType == (getenv("PC_DUMP_AFC_LQ") ? SRC_AFC_LQ : SRC_AFC_HQ)
			    && (getenv("PC_DUMP_AFC") || getenv("PC_DUMP_AFC_LQ"))) {
				dumped        = true;
				const u8* p   = pc_aram_base() + v->baseAddress;
				u32 bytes     = (v->_11C / 16u) * (v->samplesSourceType == SRC_AFC_LQ ? 5u : 9u);
				if (bytes > 32768) bytes = 32768;
				const char* dp = getenv("PC_DUMP_AFC_LQ") ? getenv("PC_DUMP_AFC_LQ") : getenv("PC_DUMP_AFC");
				FILE* f = fopen(dp, "wb");
				if (f) {
					fwrite(p, 1, bytes, f);
					fclose(f);
					OSReport("[DSP] dumped %u bytes of AFC source (base 0x%x, %u samples)\n", bytes, v->baseAddress, v->_11C);
				}
			}
		}
		v->resetVpb                   = DSP_FALSE;
		v->currentAramAddr            = v->baseAddress;
		v->currentPosition            = 0;
		v->currentPosFrac             = 0;
		const u32 vidx0 = (u32)(v - s_chBuf);
		if (vidx0 < 64) {
			for (u32 k = 0; k < PC_RES_HIST; k++) {
				s_resHist[vidx0][k] = 0;
			}
		}

		// Direct-PCM STREAM voices are set up by Play_DirectPCM, which assigns
		// remainingLength (the total sample count of the whole stream) itself and
		// never touches _11C — _11C belongs to the wave path. Re-deriving it here
		// overwrote a live 1377136 with 0, so the voice ended on its very first
		// frame: `done` fired, the stream saw isAtEnd, tore itself down and freed
		// its DSP channels, StreamGetCurrentFrame then returned -1, and
		// MovSampleSection ended the movie a fraction of a second in. Leave
		// stream voices alone.
		if (v->samplesSourceType != SRC_DIRECT_PCM) {
			// A looping voice wraps at its loop END, not at the end of the sample:
			// the samples between loopEnd and sampleCount are the decoder's run-out
			// and are never meant to be heard. (See the loop-point note in
			// pc_voice_fetch for why _18 is the end and _14 the start.)
			v->remainingLength = (v->isLooping && v->loopStartPosition && (u32)v->loopStartPosition <= (u32)v->_11C)
			                         ? (u32)v->loopStartPosition
			                         : (u32)v->_11C;
		}
		v->endReached                 = DSP_FALSE;
		v->afcRemainingDecodedSamples = 0;
	}

	// endRequested is the CPU asking the UCODE to finish this voice
	// (ForceStopDSPchannel sets it together with allocState = Stopping). The
	// ucode ramps the sends out over the frame and then reports `done`, which
	// is what lets the driver reclaim the channel.
	//
	// Ignoring it meant a LOOPING voice could never stop: it never reaches the
	// end of its data, so nothing else ever sets done, and the channel stayed
	// Stopping forever. Non-looping voices hid the bug by ending naturally.
	// That is the sustained tone heard when a menu loads — sounds started by
	// the previous section are asked to stop and simply keep playing.
	const bool ending = v->endRequested != DSP_FALSE;

	// resamplingRatio is 4.12 fixed point: 0x1000 is 1:1.
	const u32 ratio = v->resamplingRatio ? v->resamplingRatio : 0x1000;

	// Consume EXACTLY as many source samples as the frame advances through.
	// Fetching a padded over-estimate and dropping the tail (as this first did)
	// silently discards samples every frame — audible as garbled/chopped sound
	// rather than as silence, because the stream still plays, just missing
	// pieces.
	//
	// The 4-tap resampler below reads s[idx-1 .. idx+2], so it needs one sample
	// of history AND two of lookahead. Carrying FOUR samples across the frame
	// (s[-1], s[0], s[1], s[2]) supplies both while still fetching exactly
	// `consumed` new samples per frame — carrying only the two straddling
	// samples would force a lookahead fetch that permanently over-advances the
	// decoder.
	const u32 startFrac = v->currentPosFrac & 0xFFF;
	const u64 total     = (u64)startFrac + (u64)ratio * n;
	const u32 consumed  = (u32)(total >> 12);

	if (consumed > 4096) {
		return;
	}

	// src[j] holds s[j-1], so the sample at integer offset idx is src[idx + 1]
	// and its tap window is src[idx .. idx+3].
	s16 src[PC_RES_HIST + 4096 + 4];
	const u32 vi = (u32)(v - s_chBuf);
	s16* hist    = (vi < 64) ? s_resHist[vi] : s_resHistSpill;
	for (u32 k = 0; k < PC_RES_HIST; k++) {
		src[k] = hist[k];
	}
	pc_voice_fetch(v, src + PC_RES_HIST, consumed);
	for (u32 k = 0; k < PC_RES_HIST; k++) {
		hist[k] = src[consumed + k];
	}
	if (pc_tap_match(v) && s_tapSrcFile) {
		fwrite(src + PC_RES_HIST, sizeof(s16), consumed, s_tapSrcFile);
	}

	// Resample once into a scratch buffer. Doing it inside the per-send loop (as
	// this used to) repeats the whole filter for every bus the voice feeds.
	static s16 res[1024];

	static int linear = -1;
	if (linear < 0) {
		linear = getenv("PC_DSP_LINEAR") ? 1 : 0;
	}

	if (linear || !s_resFilter) {
		// PC_DSP_LINEAR=1 keeps the old 2-tap path for A/B.
		for (u32 i = 0; i < n; i++) {
			const u32 p   = startFrac + ratio * i;
			const u32 idx = p >> 12;
			const s32 fr  = (s32)(p & 0xFFF);
			const s32 a2  = src[idx + 1];
			const s32 b2  = src[idx + 2];
			res[i]        = (s16)((a2 * (0x1000 - fr) + b2 * fr) >> 12);
		}
	} else {
		// DSPRES_FILTER is the ucode's polyphase resampler: 64 phases of 4 taps,
		// packed two s16 per u32 with the FIRST coefficient in the HIGH half (the
		// same convention as DSPADPCM_FILTER — reading it as a flat s16* picks the
		// pairs up swapped on a little-endian host).
		//
		// Verified by inspection of the table itself: phases 0..63 each sum to
		// ~32768 and are exactly symmetric (phase 63 is phase 0 reversed, 62
		// mirrors 1, and phase 32 has equal centre taps), while entries beyond 63
		// sum to nothing like 32768 and belong to a different table. The 12-bit
		// fraction therefore selects a phase with >> 6.
		const u32* tbl = (const u32*)s_resFilter;
		for (u32 i = 0; i < n; i++) {
			const u32 p     = startFrac + ratio * i;
			const u32 idx   = p >> 12;
			const u32 phase = (p >> 6) & 63u;
			const int c     = (int)(phase * 4u);

			const s32 c0 = pc_res_coef(tbl, c + 0);
			const s32 c1 = pc_res_coef(tbl, c + 1);
			const s32 c2 = pc_res_coef(tbl, c + 2);
			const s32 c3 = pc_res_coef(tbl, c + 3);

			s32 acc = c0 * (s32)src[idx + 0] + c1 * (s32)src[idx + 1] + c2 * (s32)src[idx + 2] + c3 * (s32)src[idx + 3];
			acc >>= 15;
			if (acc > 32767) acc = 32767;
			if (acc < -32768) acc = -32768;
			res[i] = (s16)acc;
		}
	}

	// Per-voice biquad, after resampling and before volume — Dolphin Zelda HLE
	// (semantic reference, see the licence note at the top of this file):
	//
	//     yn0 = clamp((bn1*xn1 + bn2*xn2 + an1*yn1 + an2*yn2) >> 15)
	//
	// with coefficients in biquadFilterCoeffs[] as [bn1 bn2 an1 an2] and
	// history [xn1 xn2 yn1 yn2] (DSP_PlayStart zeroes it). Note there is NO
	// current-sample term — the ucode's filter is one sample delayed.
	//
	// The v76 P4 scoping measured boot->title only, saw the identity set
	// {32767,0,0,0}, and skipped the filter as a no-op. The save-select menu
	// refutes that: its music voices program {18463,1300,8939,7314} — a real
	// smoothing IIR — and rendering them unfiltered is a large part of the
	// "menu static" (textured pads play far brighter than on hardware).
	// Dolphin's own enable test is the same identity check used here.
	// PC_DSP_NOBIQUAD=1 disables it for A/B.
	static int nobiquad = -1;
	if (nobiquad < 0) {
		nobiquad = getenv("PC_DSP_NOBIQUAD") ? 1 : 0;
	}
	if (!nobiquad && (v->filterMode & 0x20)) {
		const s16 bn1 = (s16)v->biquadFilterCoeffs[0];
		const s16 bn2 = (s16)v->biquadFilterCoeffs[1];
		const s16 an1 = (s16)v->biquadFilterCoeffs[2];
		const s16 an2 = (s16)v->biquadFilterCoeffs[3];
		if (!(bn1 == 0x7FFF && bn2 == 0 && an1 == 0 && an2 == 0)) {
			s16 xn1 = v->biquadHistory[0];
			s16 xn2 = v->biquadHistory[1];
			s16 yn1 = v->biquadHistory[2];
			s16 yn2 = v->biquadHistory[3];
			for (u32 i = 0; i < n; i++) {
				const s16 x0  = res[i];
				const s64 acc = (s64)bn1 * xn1 + (s64)bn2 * xn2 + (s64)an1 * yn1 + (s64)an2 * yn2;
				s32 y0        = (s32)(acc >> 15);
				if (y0 > 32767) y0 = 32767;
				if (y0 < -32768) y0 = -32768;
				xn2    = xn1;
				xn1    = x0;
				yn2    = yn1;
				yn1    = (s16)y0;
				res[i] = (s16)y0;
			}
			v->biquadHistory[0] = xn1;
			v->biquadHistory[1] = xn2;
			v->biquadHistory[2] = yn1;
			v->biquadHistory[3] = yn2;
		}
	}

	if (pc_tap_match(v) && s_tapResFile) {
		fwrite(res, sizeof(s16), n, s_tapResFile);
		// One (current, target) volume pair per frame alongside the audio —
		// a fluttering target is an envelope bug amplitude-modulating the
		// voice into a buzz that no sample-path check can see.
		static FILE* volf;
		if (!volf) {
			volf = fopen("tap_vol.raw", "wb");
		}
		if (volf) {
			s16 trip[3] = { (s16)v->mixChannels[0].currentVolume, (s16)v->mixChannels[0].targetVolume,
				            (s16)v->resamplingRatio };
			fwrite(trip, sizeof(s16), 3, volf);
		}
	}

	// Volume ramps: each send walks currentVolume toward targetVolume across
	// the frame, which is what stops note-ons from clicking.
	for (int m = 0; m < 6; m++) {
		DSPMixerChannel* mc = &v->mixChannels[m];
		s32* bus;
		if (mc->id == BUS_MAIN_L) {
			bus = busL;
		} else if (mc->id == BUS_MAIN_R) {
			bus = busR;
		} else {
			// Aux sends: the reverb input buses and the 0x09A0 return. P4.
			bus = pc_aux_bus_for(mc->id);
			if (!bus || !pc_fx_on()) {
				continue;
			}
		}

		const s32 cur  = (s16)mc->currentVolume;
		const s32 targ = ending ? 0 : (s16)mc->targetVolume;
		// The ramp step MUST keep fractional precision. `(targ - cur) / n` in
		// integer arithmetic is 0 for any volume change smaller than n (560)
		// per frame — i.e. for every slow envelope, tremolo and fade — so the
		// volume sat flat all frame and SNAPPED at the boundary. A 57 Hz
		// volume staircase on every sustained voice is zipper noise: sidebands
		// around each partial, audible as structured static riding held notes
		// (measured as ~+4 dB of excess 1.5-3 kHz energy vs a Dolphin capture
		// of the same screen, on a mix whose voices are otherwise bit-exact).
		s64 cur16        = (s64)cur << 16;
		const s64 step16 = (((s64)(targ - cur)) << 16) / (s32)n;

		for (u32 i = 0; i < n; i++) {
			bus[i] += ((s32)res[i] * (s32)(cur16 >> 16)) >> 15;
			cur16 += step16;
		}
		mc->currentVolume = (u16)targ;
	}

	v->currentPosFrac = (u16)(total & 0xFFF);

	// Ramp finished — report completion so the driver can reclaim the channel.
	if (ending) {
		v->endReached = DSP_TRUE;
	}
}

// ---------------------------------------------------------------- FX/reverb
// The ucode's four reverb pipes (P4). Semantics per Dolphin's Zelda HLE
// ApplyReverb (semantic reference only — see the licence note at the top):
// each enabled FXBuffer is a circular delay of `circularBufferSize` frames of
// 0x50 samples over a fixed input bus (rpb 0..3 <- 0x0DC0/0x0E20/0x0E80/
// 0x0EE0). Pre-render: the delayed frame is read, optionally 8-tap filtered
// (enabled bit 0 = before dest-mix, bit 1 = after), ADDED to the two dest
// buses at their 1.15 volumes, and REPLACES the input bus (feedback). Voices
// then add their sends on top; post-render the bus is written back to the
// delay line.
//
// DELIBERATE DEVIATION: Pikmin routes its reverb return to bus 0x09A0, which
// Dolphin's HLE treats as a write-only sink ("Used by the GC IPL as a reverb
// dest") — i.e. the HLE plays Pikmin DRY. The user's LLE reference audibly
// carries the reverb, so here 0x09A0 is treated as a return bus summed into
// both mains. PC_DSP_NOFX=1 disables the whole block for A/B.

#define PC_FX_CHUNK 0x50u
#define PC_FX_FRAME 560u // JAC_FRAMESAMPLES; sync_frame asserts the geometry

static s32 s_busRevIn[4][PC_FX_FRAME]; // 0x0DC0, 0x0E20, 0x0E80, 0x0EE0
static s32 s_busUnk0[PC_FX_FRAME];     // 0x09A0 — reverb return
static u16 s_fxFrameIdx[4];
static s16 s_fxLast8[4][8];

static int pc_fx_on(void)
{
	static int on = -1;
	if (on < 0) {
		on = getenv("PC_DSP_NOFX") ? 0 : 1;
	}
	return on;
}

// The aux bus a voice send with this id accumulates into, or NULL.
static s32* pc_aux_bus_for(u16 id)
{
	switch (id) {
	case 0x0DC0: return s_busRevIn[0];
	case 0x0E20: return s_busRevIn[1];
	case 0x0E80: return s_busRevIn[2];
	case 0x0EE0: return s_busRevIn[3];
	case 0x09A0: return s_busUnk0;
	default: return NULL;
	}
}

static void pc_fx_filter(s16* buf, const s16* coeffs)
{
	for (u32 i = 0; i < PC_FX_CHUNK; i++) {
		s32 sample = 0;
		for (u32 j = 0; j < 8; j++) {
			sample += (s32)buf[i + j] * coeffs[j];
		}
		sample >>= 15;
		if (sample > 32767) sample = 32767;
		if (sample < -32768) sample = -32768;
		buf[i] = (s16)sample;
	}
}

static void pc_fx_pre(s32* busL, s32* busR, u32 n)
{
	if (!pc_fx_on() || !s_fxBuf) {
		return;
	}
	const u32 chunks = n / PC_FX_CHUNK;
	for (u32 rpb = 0; rpb < 4; rpb++) {
		FXBuffer* fx = &s_fxBuf[rpb];
		if (!fx->enabled || !fx->circularBufferBase || fx->circularBufferSize == 0) {
			continue;
		}
		for (u32 k = 0; k < chunks; k++) {
			const u32 idx = (u32)(s_fxFrameIdx[rpb] + k) % fx->circularBufferSize;
			s16* circ     = fx->circularBufferBase + idx * PC_FX_CHUNK;

			s16 buf[PC_FX_CHUNK + 8];
			for (u32 i = 0; i < 8; i++) {
				buf[i] = s_fxLast8[rpb][i];
			}
			for (u32 i = 0; i < PC_FX_CHUNK; i++) {
				buf[8 + i] = circ[i];
			}
			for (u32 i = 0; i < 8; i++) {
				s_fxLast8[rpb][i] = buf[PC_FX_CHUNK + i];
			}

			if (fx->enabled & 1) {
				pc_fx_filter(buf, fx->filterCoeffs);
			}
			for (int d = 0; d < 2; d++) {
				const u16 id  = fx->dest[d].bufferId;
				const s32 vol = (s16)fx->dest[d].volume;
				if (id == 0 || vol == 0) {
					continue;
				}
				s32* dst = NULL;
				if (id == BUS_MAIN_L) dst = busL + k * PC_FX_CHUNK;
				else if (id == BUS_MAIN_R) dst = busR + k * PC_FX_CHUNK;
				else {
					s32* aux = pc_aux_bus_for(id);
					if (aux) dst = aux + k * PC_FX_CHUNK;
				}
				if (dst) {
					for (u32 i = 0; i < PC_FX_CHUNK; i++) {
						dst[i] += ((s32)buf[i] * vol) >> 15;
					}
				}
			}
			if (fx->enabled & 2) {
				pc_fx_filter(buf, fx->filterCoeffs);
			}
			for (u32 i = 0; i < PC_FX_CHUNK; i++) {
				s_busRevIn[rpb][k * PC_FX_CHUNK + i] = buf[i];
			}
		}
	}
}

static void pc_fx_post(u32 n)
{
	if (!pc_fx_on() || !s_fxBuf) {
		return;
	}
	const u32 chunks = n / PC_FX_CHUNK;
	for (u32 rpb = 0; rpb < 4; rpb++) {
		FXBuffer* fx = &s_fxBuf[rpb];
		if (!fx->enabled || !fx->circularBufferBase || fx->circularBufferSize == 0) {
			continue;
		}
		for (u32 k = 0; k < chunks; k++) {
			const u32 idx = (u32)(s_fxFrameIdx[rpb] + k) % fx->circularBufferSize;
			s16* circ     = fx->circularBufferBase + idx * PC_FX_CHUNK;
			for (u32 i = 0; i < PC_FX_CHUNK; i++) {
				s32 v = s_busRevIn[rpb][k * PC_FX_CHUNK + i];
				if (v > 32767) v = 32767;
				if (v < -32768) v = -32768;
				circ[i] = (s16)v;
			}
		}
		s_fxFrameIdx[rpb] = (u16)((s_fxFrameIdx[rpb] + chunks) % fx->circularBufferSize);
	}
}

// ------------------------------------------------------------- frame render

extern "C" void pc_dsp_sync_frame(u32 subframes, s16* rightOut, s16* leftOut)
{
	if (!s_setupDone) {
		// DsyncFrame before DsetupTable would mean the boot order changed.
		static bool warned;
		if (!warned) {
			warned = true;
			OSReport("[DSP] DsyncFrame before DsetupTable — no tables to render from\n");
		}
	}

	const u32 n = JAC_FRAMESAMPLES; // 560 = subframes(7) * 80
	if (subframes * 80u != n) {
		static bool warned;
		if (!warned) {
			warned = true;
			OSReport("[DSP] unexpected frame geometry: %u subframes x80 != %u samples\n", subframes, n);
		}
	}

	// Dry-voice render: accumulate every enabled VPB into the two main buses at
	// 32-bit width, then clamp down. P4 adds the per-voice filters, the aux and
	// dolby buses, and the four FX blocks.
	static s32 busL[1024];
	static s32 busR[1024];
	memset(busL, 0, n * sizeof(s32));
	memset(busR, 0, n * sizeof(s32));
	memset(s_busRevIn, 0, sizeof(s_busRevIn));
	memset(s_busUnk0, 0, sizeof(s_busUnk0));

	// FX pre-phase: pull each reverb pipe's delayed frame, mix it to its
	// dests, and seed the input buses with the feedback content.
	pc_fx_pre(busL, busR, n);

	// P4 SCOPING RESULT (PC_DSP_SURVEY=1 over boot -> title):
//   voices=1..9, filtered=ALL, dolby=0, non-main sends=0..4, fx[0..3]=1,0,2,0
// Every voice sets filterMode, but the ONLY mode the game uses on this path is
// 0x0020 (biquad) and its coefficients are {32767, 0, 0, 0} — b0 ~= 1.0 in
// 1.15 with the rest zero, i.e. an IDENTITY filter. So implementing the
// per-voice biquad/FIR/low-pass would be pure risk for no audible change here;
// what is actually live is the FX (reverb) blocks and a handful of aux sends.
// Re-run the survey on a cave/large-space scene before assuming that holds.
//
// P4 scoping: measure what the game actually asks for before building it.
	// PC_DSP_SURVEY=1 reports, once a second, how many enabled voices use a
	// per-voice filter and how many mixer sends target buses this renderer
	// currently ignores (aux / reverb / dolby).
	static int survey = -1;
	if (survey < 0) {
		survey = getenv("PC_DSP_SURVEY") ? 1 : 0;
	}
	u32 nFiltered = 0, nNonMainSends = 0, nDolby = 0, nEnabled = 0;

	if (s_chBuf) {
		for (u32 i = 0; i < s_chBufLength; i++) {
			DSPchannel_* v = &s_chBuf[i];
			if (!v->enabled) {
				continue;
			}
			if (survey) {
				nEnabled++;
				{
					static u32 typeHist[32];
					const u16 t = v->samplesSourceType < 32 ? v->samplesSourceType : 31;
					if (typeHist[t] == 0) {
						typeHist[t] = 1;
						OSReport("[SURVEY] NEW source type in use: %u\n", v->samplesSourceType);
					}
				}
				if (v->filterMode) nFiltered++;
				if (v->useDolbyVolume) nDolby++;
				for (int m = 0; m < 6; m++) {
					const u16 id = v->mixChannels[m].id;
					if (id && id != BUS_MAIN_L && id != BUS_MAIN_R && v->mixChannels[m].targetVolume) {
						nNonMainSends++;
						static u16 seenBus[16]; static int nbus;
						bool have = false;
						for (int q = 0; q < nbus; q++) if (seenBus[q] == id) have = true;
						if (!have && nbus < 16) {
							seenBus[nbus++] = id;
							OSReport("[SURVEY] voices send to non-main bus 0x%04x\n", id);
						}
					}
				}
			}
			// PC_DSP_MUTE=<type> silences one source type, to identify which
			// path is producing noise without having to guess.
			static int muteType = -2;
			if (muteType == -2) {
				const char* m = getenv("PC_DSP_MUTE");
				muteType      = m ? atoi(m) : -1;
			}
			if (muteType >= 0 && (int)v->samplesSourceType == muteType) {
				continue;
			}
			// PC_DSP_MUTE_CH=<comma list> silences individual CHANNELS. Type
			// muting cannot separate a bad voice from a good one when both use
			// the same decoder — the menu static and the menu music are both
			// AFC-HQ, so PC_DSP_MUTE=9 removes them together. With a
			// deterministic replay the channel assignment is stable, which
			// makes a per-channel bisect mechanical.
			static char muteCh[64];
			static int muteChInit = 0;
			if (!muteChInit) {
				muteChInit = 1;
				if (const char* e = getenv("PC_DSP_MUTE_CH")) {
					for (const char* q = e; *q;) {
						while (*q == ',' || *q == ' ') q++;
						if (!*q) break;
						long c = strtol(q, (char**)&q, 10);
						if (c >= 0 && c < 64) muteCh[c] = 1;
					}
				}
			}
			if (i < 64 && muteCh[i]) {
				continue;
			}
			// PC_DSP_MUTE_BASE=<lo>,<hi> silences every voice whose baseAddress
			// falls in an ARAM range. The menu garbage is spread across many
			// channels at once, so channel bisection cannot isolate it — but the
			// suspect voices all sit inside the region [ARAMFREE] reports as
			// released, and the clean ones sit outside it. This tests that
			// directly.
			static u32 mbLo = 1, mbHi = 0;
			static int mbInit = 0;
			if (!mbInit) {
				mbInit = 1;
				if (const char* e = getenv("PC_DSP_MUTE_BASE")) {
					char* q = nullptr;
					unsigned long lo = strtoul(e, &q, 0);
					if (q && *q == ',') {
						unsigned long hi = strtoul(q + 1, nullptr, 0);
						mbLo = (u32)lo; mbHi = (u32)hi;
					}
				}
			}
			if (mbLo <= mbHi && v->baseAddress >= mbLo && v->baseAddress < mbHi) {
				continue;
			}
			// PC_DSP_SOLO_BASE=<lo>,<hi> — the inverse: silence every voice whose
			// baseAddress falls OUTSIDE the range, so one voice (or one wave's
			// voices) can be auditioned in isolation.
			static u32 sbLo = 1, sbHi = 0;
			static int sbInit = 0;
			if (!sbInit) {
				sbInit = 1;
				if (const char* e = getenv("PC_DSP_SOLO_BASE")) {
					char* q = nullptr;
					unsigned long lo = strtoul(e, &q, 0);
					if (q && *q == ',') {
						unsigned long hi = strtoul(q + 1, nullptr, 0);
						sbLo = (u32)lo;
						sbHi = (u32)hi;
					}
				}
			}
			if (sbLo <= sbHi && !(v->baseAddress >= sbLo && v->baseAddress < sbHi)) {
				continue;
			}
			// PC_DSP_BASELOG=1: report each DISTINCT baseAddress the mixer
			// actually renders, once. PC_DSP_SURVEY samples periodically and
			// therefore MISSES voices — bisecting against its list sent me
			// chasing five bases that were not the whole set.
			// Cached: this sits in the per-voice, per-frame render loop, where a
			// getenv() per voice per frame is a real cost even when disabled.
			static int baseLog = -1;
			if (baseLog < 0) baseLog = getenv("PC_DSP_BASELOG") ? 1 : 0;
			if (baseLog) {
				static u32 seenBase[256];
				static int nSeen = 0;
				bool known = false;
				for (int k = 0; k < nSeen; k++)
					if (seenBase[k] == v->baseAddress) { known = true; break; }
				if (!known && nSeen < 256) {
					seenBase[nSeen++] = v->baseAddress;
					char hex[64] = { 0 };
					const u8* raw = pc_aram_base() ? pc_aram_base() + v->baseAddress : nullptr;
					if (raw && v->baseAddress + 16 < pc_aram_size()) {
						for (int q = 0; q < 12; q++) {
							snprintf(hex + q * 3, 4, "%02x ", raw[q]);
						}
					}
					OSReport("[BASE] %3d  base=0x%08x type=%2u ratio=0x%04x loop=%u remain=%-6u  bytes: %s\n",
					         nSeen, v->baseAddress, v->samplesSourceType, v->resamplingRatio,
					         v->isLooping, v->remainingLength, hex);
				}
			}
			// PC_DSP_AUXAUDIT=1: is any voice INAUDIBLE because its only live
			// send is to a bus we don't render? That distinguishes "P4 is missing
			// reverb" (cosmetic) from "P4 is dropping whole sounds" (not).
			static int auxaudit = -1;
			if (auxaudit < 0) auxaudit = getenv("PC_DSP_AUXAUDIT") ? 1 : 0;
			if (auxaudit) {
				u32 mainVol = 0, auxVol = 0;
				for (int m = 0; m < 6; m++) {
					const DSPMixerChannel* mc = &v->mixChannels[m];
					const u32 tv              = (u32)(u16)mc->targetVolume;
					if (!mc->id || !tv) continue;
					if (mc->id == BUS_MAIN_L || mc->id == BUS_MAIN_R) mainVol += tv;
					else auxVol += tv;
				}
				static u32 nVoices, nAuxOnly, nWithAux;
				nVoices++;
				if (auxVol) nWithAux++;
				if (auxVol && !mainVol) nAuxOnly++;
				if ((nVoices % 20000) == 0) {
					OSReport("[AUXAUDIT] %u voice-frames: %u with an aux send (%u%%), %u AUDIBLE ONLY via aux (%u%%)\n",
					         nVoices, nWithAux, nWithAux * 100 / nVoices, nAuxOnly, nAuxOnly * 100 / nVoices);
				}
			}
			// PC_DSP_FIRAUDIT=1: every voice programs a filter and we apply none
			// of it. Dump the distinct (mode, biquad, fir8) sets actually in use —
			// if they are passthrough, ignoring the filter costs nothing.
			static int firaudit = -1;
			if (firaudit < 0) firaudit = getenv("PC_DSP_FIRAUDIT") ? 1 : 0;
			if (firaudit) {
				// Dedupe on the full programmed set, not just the mode — the
				// boot-time audit keyed on mode alone and would have hidden a
				// scene that reuses mode 0x20 with REAL coefficients.
				struct FilterSet {
					u16 mode;
					s16 bq[4];
					s16 lp;
				};
				static FilterSet seen[24];
				static int nSeen;
				FilterSet cur;
				cur.mode = v->filterMode;
				for (int q = 0; q < 4; q++) {
					cur.bq[q] = (s16)v->biquadFilterCoeffs[q];
				}
				cur.lp    = (s16)v->lowPassCoeff;
				bool have = false;
				for (int q = 0; q < nSeen; q++) {
					if (memcmp(&seen[q], &cur, sizeof(cur)) == 0) {
						have = true;
						break;
					}
				}
				if (!have && nSeen < 24) {
					seen[nSeen++] = cur;
					OSReport("[FIRAUDIT] base=0x%08x mode=%u (biquad=%u firTaps=%u) lowPass=%d biquad=[%d %d %d %d] fir8[0..3]=[%d %d %d %d]\n",
					         v->baseAddress, v->filterMode, (v->filterMode >> 5) & 1, v->filterMode & 0x1f, (int)cur.lp,
					         (int)cur.bq[0], (int)cur.bq[1], (int)cur.bq[2], (int)cur.bq[3], (s16)v->variableFirCoeffs[0],
					         (s16)v->variableFirCoeffs[1], (s16)v->variableFirCoeffs[2], (s16)v->variableFirCoeffs[3]);
				}
			}
			// PC_DSP_TYPEHIST=1: voice-frames per source type, and how many carry
			// real volume — tells a format we silently drop from one that never
			// actually plays.
			static int typehist = -1;
			if (typehist < 0) typehist = getenv("PC_DSP_TYPEHIST") ? 1 : 0;
			if (typehist) {
				static u32 frames[64], loud[64], total;
				const u32 t = v->samplesSourceType < 64 ? v->samplesSourceType : 63;
				u32 mv      = 0;
				for (int m = 0; m < 6; m++) {
					const DSPMixerChannel* mc = &v->mixChannels[m];
					if (mc->id == BUS_MAIN_L || mc->id == BUS_MAIN_R) mv += (u32)(u16)mc->targetVolume;
				}
				frames[t]++;
				if (mv > 256) loud[t]++;
				if ((++total % 20000) == 0) {
					OSReport("[TYPEHIST] after %u voice-frames:\n", total);
					for (u32 q = 0; q < 64; q++) {
						if (frames[q]) {
							OSReport("    type %2u: %7u frames, %7u with volume\n", q, frames[q], loud[q]);
						}
					}
				}
			}
			pc_render_voice(v, busL, busR, n);
			if (v->endReached) {
				v->done = DSP_TRUE;
			}
		}
	}

	// FX post-phase: store the reverb buses (feedback + this frame's sends)
	// back into the delay lines, then sum the 0x09A0 return into the mains
	// (the Pikmin-port routing decision documented above pc_fx_pre).
	if (pc_fx_on()) {
		pc_fx_post(n);
		for (u32 i = 0; i < n; i++) {
			s32 r = s_busUnk0[i];
			busL[i] += r;
			busR[i] += r;
		}
	}

	// ~2 s after a scene change, report voices that survived it.
	if (s_auditPending && s_frameCount > s_markFrame + 114) {
		s_auditPending = false;
		u32 survivors  = 0;
		for (u32 i = 0; i < s_chBufLength && i < 64; i++) {
			if (s_markedAtScene[i] && s_chBuf[i].enabled) {
				survivors++;
				// endRequested distinguishes "the game never asked this voice to
				// stop" from "it asked and we failed to finish it".
				OSReport("[SURVIVOR] ch%02u type=%u loop=%u remain=%u vol=%d endReq=%u done=%u endReached=%u\n", i,
				         s_chBuf[i].samplesSourceType, s_chBuf[i].isLooping, s_chBuf[i].remainingLength,
				         (int)(s16)s_chBuf[i].mixChannels[0].currentVolume, s_chBuf[i].endRequested, s_chBuf[i].done,
				         s_chBuf[i].endReached);
			}
		}
		if (survivors) {
			OSReport("[SURVIVOR] %u voice(s) outlived the scene change\n", survivors);
		}
	}

	pc_looptag_report(s_chBuf, s_chBufLength);

	if (survey && s_fxBuf) {
		static bool fxDumped;
		if (!fxDumped && (s_fxBuf[0].enabled || s_fxBuf[1].enabled || s_fxBuf[2].enabled || s_fxBuf[3].enabled)) {
			fxDumped = true;
			for (int k = 0; k < 4; k++) {
				OSReport("[SURVEY] FX[%d] enabled=%u size=%u base=%p dest0=0x%04x vol=%d dest1=0x%04x vol=%d\n", k,
				         s_fxBuf[k].enabled, s_fxBuf[k].circularBufferSize, (void*)s_fxBuf[k].circularBufferBase,
				         s_fxBuf[k].dest[0].bufferId, s_fxBuf[k].dest[0].volume, s_fxBuf[k].dest[1].bufferId,
				         s_fxBuf[k].dest[1].volume);
			}
		}
	}

	if (survey && (s_frameCount % 57) == 0 && nEnabled) {
		OSReport("[SURVEY] cmdq sent=%u delivered=%u blockedFrames=%u\n", g_pc_cmdq_sent, g_pc_cmdq_delivered, g_pc_cmdq_blocked);
		OSReport("[SURVEY] voices=%u filtered=%u dolby=%u non-main-sends=%u dspch=%u/64 stopping=%u | fx[0..3] enabled=%u%u%u%u\n", nEnabled, nFiltered,
		         nDolby, nNonMainSends, pc_dspch_in_use(), pc_dspch_stopping(), s_fxBuf ? s_fxBuf[0].enabled : 0, s_fxBuf ? s_fxBuf[1].enabled : 0,
		         s_fxBuf ? s_fxBuf[2].enabled : 0, s_fxBuf ? s_fxBuf[3].enabled : 0);
	}

	// PC_DSP_SURVEY: self-triggering noise catcher. Measure this frame's own
	// zero-crossing rate and, when it spikes into noise territory, dump every
	// voice that contributed. Far better than guessing which second of a
	// capture holds the symptom — the renderer reports the moment itself.
	if (survey && nEnabled) {
		u32 zc = 0;
		for (u32 i = 1; i < n; i++) {
			if ((busL[i - 1] < 0) != (busL[i] < 0)) {
				zc++;
			}
		}
		if (zc * 100u > n * 30u) { // >0.30 crossings/sample
			static int reported;
			if (reported < 6) {
				reported++;
				OSReport("[NOISE] frame zcr=%.2f with %u voices:\n", (double)zc / (double)n, nEnabled);
				for (u32 i = 0; i < s_chBufLength; i++) {
					DSPchannel_* vv = &s_chBuf[i];
					if (!vv->enabled) {
						continue;
					}
					OSReport("        ch%02u type=%2u ratio=0x%04x base=0x%08x aram=0x%08x remain=%6u loop=%u vol=%d/%d bus=0x%04x\n",
					         i, vv->samplesSourceType, vv->resamplingRatio, vv->baseAddress, vv->currentAramAddr,
					         vv->remainingLength, vv->isLooping, (int)(s16)vv->mixChannels[0].currentVolume,
					         (int)(s16)vv->mixChannels[0].targetVolume, vv->mixChannels[0].id);
				}
			}
		}
	}

	// Count how hard the mix is clipping — the audible signature of clipping is
	// exactly the "wind hitting a microphone" distortion reported with 20+
	// simultaneous voices.
	if (survey) {
		static u64 clipped, total;
		for (u32 i = 0; i < n; i++) {
			if (busL[i] > 32767 || busL[i] < -32768 || busR[i] > 32767 || busR[i] < -32768) {
				clipped++;
			}
			total++;
		}
		if ((s_frameCount % 285) == 0 && total) {
			OSReport("[CLIP] %llu/%llu samples clipped (%.3f%%)\n", (unsigned long long)clipped, (unsigned long long)total,
			         100.0 * (double)clipped / (double)total);
		}
	}

	// NOT scaled by s_mixerLevel. The Dolphin A/B matched our UNSCALED output
	// (peak 9432 vs 9422), so whatever the ucode does with DSP_MIXERLEVEL, the
	// net effect at this point is unity for the levels the game uses. Applying
	// it as a >>14 gain halved the output and would have broken that match.
	// The value is captured (pc_dsp_set_mixer_level) in case a later A/B shows
	// it matters; do not apply it without re-running tools/audio_ab.py.
	for (u32 i = 0; i < n; i++) {
		s32 l = busL[i];
		s32 r = busR[i];
		if (l > 32767) l = 32767;
		if (l < -32768) l = -32768;
		if (r > 32767) r = 32767;
		if (r < -32768) r = -32768;
		leftOut[i]  = (s16)l;
		rightOut[i] = (s16)r;
	}

	// PC_DSP_TESTTONE=1 puts 440 Hz in the LEFT block and 880 Hz in the RIGHT.
	// This is how the planar layout and channel order documented in pc_dsp.h
	// were verified end to end rather than merely reasoned about: capture with
	// PC_DUMP_WAV, and the recovered per-channel frequencies say whether the
	// mixdown, the planar split and the L/R assignment are all correct.
	static int tone = -1;
	if (tone < 0) {
		tone = getenv("PC_DSP_TESTTONE") ? 1 : 0;
	}
	if (tone) {
		static u32 phase;
		for (u32 i = 0; i < n; i++) {
			const double t = (double)(phase + i) / (double)JAC_DAC_RATE;
			leftOut[i]     = (s16)(8000.0 * sin(6.283185307179586 * 440.0 * t));
			rightOut[i]    = (s16)(8000.0 * sin(6.283185307179586 * 880.0 * t));
		}
		phase += n;
	}

	// On hardware the ucode raises one interrupt per subframe, and jaudio's
	// audio thread counts them down (DspSyncCountClear sets the count; the last
	// AUDIOPROC_MESSAGE_DSP_SYNC calls DspFrameEnd, which issues the NEXT
	// DsyncFrame). Nothing else advances that pipeline — without these the
	// first frame renders and everything stops, which is exactly what happened:
	// 1 DSP frame against 1149 DMA frames.
	//
	// We render synchronously, so raise all `subframes` interrupts now. The
	// handler only posts NOBLOCK messages, and production stays bounded by
	// dspbuf's triple buffer, which the AI-paced consumer drains.
	for (u32 i = 0; i < subframes; i++) {
		pc_os_raise_interrupt(PC_INTERRUPT_DSP_DSP);
	}

	s_frameCount++;
}

extern "C" void pc_dsp_wait_frame(void)
{
	// Rendering is synchronous inside pc_dsp_sync_frame; nothing to wait on.
}

// ---------------------------------------------------------------- reporting

extern "C" u32 pc_dsp_active_voices(void)
{
	if (!s_chBuf) {
		return 0;
	}
	u32 active = 0;
	for (u32 i = 0; i < s_chBufLength; i++) {
		if (s_chBuf[i].enabled) {
			active++;
		}
	}
	return active;
}

extern "C" void pc_dsp_report(void)
{
	OSReport("[DSP] %llu frames rendered, %u/%u voices enabled\n", (unsigned long long)s_frameCount, pc_dsp_active_voices(),
	         s_chBufLength);
}
