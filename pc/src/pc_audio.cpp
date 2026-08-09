// pc_audio.cpp — SDL2 audio backend for jaudio (M5 P1).
//
// Ported from the prior port's pc_audio.c, which shipped this exact architecture:
//
//   audio thread   jaudio's own audioproc runs a frame on each "AI DMA
//                  interrupt" and hands the finished DAC buffer to AIInitDMA
//   producer pump  fires that interrupt, paced by how full the ring is
//   SDL callback   drains the ring to the device
//
// The ring decouples sample production from OS scheduling, so a preempted
// game or audio thread doesn't drop audio. The producer is paced by ring FILL
// rather than a free-running timer: the device clock then governs the
// long-term rate, which is what keeps a 32000 Hz device fed by a nominally
// 32028.5 Hz source (JAC_DAC_RATE) from slowly drifting apart. That 0.09%
// difference is inaudible; per the M5 plan it is accepted for now and only
// revisited if an A/B shows pitch issues.
//
// Pikmin deltas from the prior port: no DMA address translation is needed — jaudio
// hands AIInitDMA a real host pointer (the DsyncFrame/AIInitDMA signatures
// were widened to uintptr_t back in M1) — and a frame is 560 stereo samples
// (DAC_SIZE * 2 bytes = 1120 s16 = 560 frames), not AC's cadence.

#include "pc_host_compat.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "Dolphin/ai.h"
#include "Dolphin/os.h"
}

// The device rate. JAC_DAC_RATE is 32028.5 Hz; see the header comment.
#define PC_AUDIO_RATE 32000

// Lock-free SPSC ring: producer is the audio pump, consumer is the SDL
// callback. Power of two so the mask works.
#define RING_SAMPLES (32768) // ~512 ms at 32 kHz stereo
#define RING_MASK    (RING_SAMPLES - 1)

// One AI DMA period: DAC_SIZE * 2 bytes = 1120 s16 = 560 stereo frames.
#define PC_DAC_PERIOD 1120
// Interrupts allowed ahead of what the device has consumed. Enough to fill the
// ring past the callback's appetite and to prime jaudio's own triple buffer.
#define PC_DAC_PREFILL 8

static s16 s_ring[RING_SAMPLES];
static SDL_atomic_t s_write_pos; // written by the producer
static SDL_atomic_t s_read_pos;  // written by the SDL callback
static SDL_AudioDeviceID s_device;
// The device's pause state is the AND of two independent owners: the game
// (AIStartDMA/AIStopDMA) and the host lifecycle (iOS backgrounding). Tracking
// them separately matters because either one alone would otherwise clobber the
// other — resuming from the background by calling AIStartDMA would start audio
// the game had deliberately stopped, and the game calling AIStartDMA while
// backgrounded would hand CoreAudio work to a suspended app.
static bool s_dma_on;
static bool s_host_suspended;

typedef void (*AIDCallbackT)(void);
static AIDCallbackT s_ai_dma_cb;
static u32 s_ai_dsp_rate = PC_AUDIO_RATE;

static u64 s_frames_produced;
static u64 s_underruns;
static u64 s_frames_requested;

// ------------------------------------------------------------------ WAV dump
// PC_DUMP_WAV=path records exactly what enters the ring — s16le stereo at the
// device rate. This is the M5 plan's "eyes-equivalent for ears": every later
// phase is verified against a capture from a deterministic boot, so it exists
// from P1 rather than being retrofitted.
//
// Headless captures usually end in a kill, which no atexit handler survives,
// so the RIFF sizes are patched in place periodically — the file on disk is
// always playable, not just after a clean shutdown.

static FILE* s_wav;
static u32 s_wav_samples;
static int s_wav_checked;
static u32 s_wav_since_patch;

static void pc_wav_write_header(u32 sampleCount)
{
	const u32 dataBytes = sampleCount * 2u;
	u8 h[44];
	memcpy(h, "RIFF", 4);
	u32 riff = 36u + dataBytes;
	memcpy(h + 4, &riff, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	u32 fmtLen      = 16;
	u16 fmtTag      = 1; // PCM
	u16 channels    = 2;
	u32 rate        = PC_AUDIO_RATE;
	u32 byteRate    = PC_AUDIO_RATE * 2 * 2;
	u16 blockAlign  = 4;
	u16 bitsPerSamp = 16;
	memcpy(h + 16, &fmtLen, 4);
	memcpy(h + 20, &fmtTag, 2);
	memcpy(h + 22, &channels, 2);
	memcpy(h + 24, &rate, 4);
	memcpy(h + 28, &byteRate, 4);
	memcpy(h + 32, &blockAlign, 2);
	memcpy(h + 34, &bitsPerSamp, 2);
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &dataBytes, 4);
	fwrite(h, 1, sizeof(h), s_wav);
}

static void pc_wav_open(void)
{
	s_wav_checked  = 1;
	const char* p  = getenv("PC_DUMP_WAV");
	if (!p || !p[0]) {
		return;
	}
	s_wav = fopen(p, "wb");
	if (!s_wav) {
		OSReport("[AUDIO] PC_DUMP_WAV: cannot open '%s'\n", p);
		return;
	}
	pc_wav_write_header(0);
	OSReport("[AUDIO] dumping to '%s' (s16le stereo %d Hz)\n", p, PC_AUDIO_RATE);
}

static void pc_wav_append(const s16* samples, u32 n)
{
	if (!s_wav_checked) {
		pc_wav_open();
	}
	if (!s_wav || n == 0) {
		return;
	}
	fwrite(samples, sizeof(s16), n, s_wav);
	s_wav_samples += n;
	s_wav_since_patch += n;

	// Roughly twice a second, make the on-disk file valid.
	if (s_wav_since_patch >= PC_AUDIO_RATE) {
		s_wav_since_patch = 0;
		long pos          = ftell(s_wav);
		fseek(s_wav, 0, SEEK_SET);
		pc_wav_write_header(s_wav_samples);
		fseek(s_wav, pos, SEEK_SET);
		fflush(s_wav);
	}
}

extern "C" void pc_audio_close_dump(void)
{
	if (!s_wav) {
		return;
	}
	fseek(s_wav, 0, SEEK_SET);
	pc_wav_write_header(s_wav_samples);
	fclose(s_wav);
	s_wav = NULL;
	OSReport("[AUDIO] wav closed: %u stereo frames (%.2f s)\n", s_wav_samples / 2, (double)(s_wav_samples / 2) / PC_AUDIO_RATE);
}

// ------------------------------------------------------------- ring queries

extern "C" int pc_audio_buffer_fill(void) { return SDL_AtomicGet(&s_write_pos) - SDL_AtomicGet(&s_read_pos); }

extern "C" int pc_audio_is_active(void) { return s_device != 0; }

// -------------------------------------------------------------- SDL callback

static void pc_audio_callback(void*, Uint8* stream, int len)
{
	s16* out  = (s16*)stream;
	int total = len / (int)sizeof(s16);

	u32 wp = (u32)SDL_AtomicGet(&s_write_pos);
	SDL_MemoryBarrierAcquire();
	u32 rp   = (u32)SDL_AtomicGet(&s_read_pos);
	u32 used = wp - rp;

	if (used > RING_SAMPLES) { // producer lapped us
		rp   = (wp - RING_SAMPLES) & ~1u;
		used = wp - rp;
	}

	int avail = (int)used & ~1;         // whole stereo frames only
	int copy  = (avail < total ? avail : total) & ~1;

	for (int i = 0; i < copy; i++) {
		out[i] = s_ring[(rp + (u32)i) & RING_MASK];
	}
	if (copy < total) {
		memset(&out[copy], 0, (size_t)(total - copy) * sizeof(s16));
		s_underruns++;
	}

	SDL_MemoryBarrierRelease();
	SDL_AtomicSet(&s_read_pos, (int)(rp + (u32)copy));
}

// ----------------------------------------------------------- producer pump
// The GC fires an interrupt each time the AI finishes a DMA period, and
// jaudio's audioproc renders one frame in response. Here that interrupt is
// paced by what the device has consumed, so production tracks the DAC clock.
//
// The callback is ASYNCHRONOUS, exactly as on hardware: jaudio registers
// AudioSync, which only posts AUDIOPROC_MESSAGE_UPDATE_DAC (NOBLOCK) to its
// audio thread and returns. Firing it therefore does not immediately add
// samples — and, crucially, a request that arrives before jaudio has a buffer
// ready produces no DMA at all, so the pump must not try to pace itself by
// counting outstanding requests.

static SDL_atomic_t s_pump_running;
static SDL_Thread* s_pump;

extern "C" void pc_audio_report(void);
extern "C" void pc_dsp_report(void);

static int pc_audio_pump(void*)
{
	// PC_AUDIO_STATS=1 prints the health line every ~10 s. Underruns are the
	// number that matters: a rising count is the "crackle" symptom, and this
	// is the only way to see it on a headless run.
	const bool stats = getenv("PC_AUDIO_STATS") != NULL;
	Uint32 lastReport = SDL_GetTicks();

	while (SDL_AtomicGet(&s_pump_running)) {
		AIDCallbackT cb = s_ai_dma_cb;
		// PACING. On hardware the AI raises its interrupt once per completed DMA
		// period, at a rate set by the DAC clock — never faster. Reproduce that
		// by driving it off what the DEVICE has actually consumed: one interrupt
		// per DAC period drained from the ring, plus a bounded prefill burst to
		// get the pipeline started.
		//
		// The obvious alternative — fire whenever the ring is below a fill
		// threshold — is what this used to do, and it is subtly wrong: the pump
		// then spins, and since the callback only posts to jaudio's 16-deep
		// audio queue it floods it. The DSP's own per-subframe sync messages are
		// NOBLOCK sends, so they get dropped, jaudio's frame pipeline never
		// advances past its first frame, and the output is a held DC value
		// forever. Cost a full debugging pass; do not "simplify" this back.
		const u64 consumedPeriods = (u64)(u32)SDL_AtomicGet(&s_read_pos) / PC_DAC_PERIOD;
		const u64 target          = consumedPeriods + PC_DAC_PREFILL;
		if (cb && s_frames_requested < target && pc_audio_buffer_fill() < (int)(RING_SAMPLES - PC_DAC_PERIOD)) {
			s_frames_requested++;
			cb();
		} else {
			SDL_Delay(1);
		}
		if (stats && SDL_GetTicks() - lastReport >= 10000) {
			lastReport = SDL_GetTicks();
			pc_audio_report();
			pc_dsp_report();
		}
	}
	return 0;
}

static void pc_audio_pump_start(void)
{
	if (s_pump) {
		return;
	}
	SDL_AtomicSet(&s_pump_running, 1);
	s_pump = SDL_CreateThread(pc_audio_pump, "JaudioPump", NULL);
	if (!s_pump) {
		OSReport("[AUDIO] producer thread failed: %s\n", SDL_GetError());
	}
}

extern "C" void pc_audio_shutdown(void)
{
	SDL_AtomicSet(&s_pump_running, 0);
	if (s_pump) {
		SDL_WaitThread(s_pump, NULL);
		s_pump = NULL;
	}
	if (s_device) {
		SDL_CloseAudioDevice(s_device);
		s_device = 0;
	}
	pc_audio_close_dump();
}

// ------------------------------------------------------------------- AI API

extern "C" void AIInit(u8*)
{
	if (s_device) {
		return;
	}
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		OSReport("[AUDIO] SDL audio init failed: %s\n", SDL_GetError());
		return;
	}

	SDL_AudioSpec want, have;
	memset(&want, 0, sizeof(want));
	want.freq     = PC_AUDIO_RATE;
	want.format   = AUDIO_S16SYS;
	want.channels = 2;
	want.samples  = 512;
	want.callback = pc_audio_callback;

	s_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!s_device) {
		OSReport("[AUDIO] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
		return;
	}
	OSReport("[AUDIO] device open: %d Hz, %d ch, %d-sample buffer\n", have.freq, have.channels, have.samples);
	atexit(pc_audio_shutdown);
}

extern "C" AIDCallback AIRegisterDMACallback(AIDCallback cb)
{
	AIDCallback old = (AIDCallback)s_ai_dma_cb;
	s_ai_dma_cb     = (AIDCallbackT)cb;
	return old;
}

// jaudio hands over a finished DAC buffer here — a real host pointer, so
// unlike the prior port there is no address translation to do.
extern "C" void AIInitDMA(uintptr_t addr, u32 size)
{
	const s16* src = (const s16*)addr;
	if (!src || size == 0) {
		return;
	}

	u32 n = (size / (u32)sizeof(s16)) & ~1u; // whole stereo frames

	u32 wp = (u32)SDL_AtomicGet(&s_write_pos);
	u32 rp = (u32)SDL_AtomicGet(&s_read_pos);
	SDL_MemoryBarrierAcquire();
	u32 freeSpace = RING_SAMPLES - (wp - rp);
	if (n > freeSpace) {
		n = freeSpace & ~1u;
	}
	if (n == 0) {
		return;
	}

	for (u32 i = 0; i < n; i++) {
		s_ring[(wp + i) & RING_MASK] = src[i];
	}

	// Dump from the ring (post-wrap) so the capture is exactly what plays.
	u32 start = wp & RING_MASK;
	u32 first = (start + n <= RING_SAMPLES) ? n : RING_SAMPLES - start;
	pc_wav_append(&s_ring[start], first);
	if (first < n) {
		pc_wav_append(s_ring, n - first);
	}

	SDL_MemoryBarrierRelease();
	SDL_AtomicSet(&s_write_pos, (int)(wp + n));
	s_frames_produced++;
}

// Apply the two owners' combined intent to the device.
static void pc_audio_apply_pause(void)
{
	if (s_device) {
		SDL_PauseAudioDevice(s_device, (s_dma_on && !s_host_suspended) ? 0 : 1);
	}
}

extern "C" void AIStartDMA()
{
	s_dma_on = true;
	pc_audio_apply_pause();
	pc_audio_pump_start();
}

extern "C" void AIStopDMA()
{
	s_dma_on = false;
	pc_audio_apply_pause();
}

// Host lifecycle (pc_window.mm's iOS background handling). Separate from
// AIStopDMA so the game's own DMA state survives a trip to the background.
extern "C" void pc_audio_set_host_suspended(int suspended)
{
	const bool s = suspended != 0;
	if (s == s_host_suspended) {
		return;
	}
	s_host_suspended = s;
	pc_audio_apply_pause();
}

extern "C" void AISetDSPSampleRate(u32 rate) { s_ai_dsp_rate = rate; }
extern "C" u32 AIGetDSPSampleRate() { return s_ai_dsp_rate; }
extern "C" u32 AIGetStreamSampleCount() { return 0; }
extern "C" u32 AIGetStreamTrigger() { return 0; }
extern "C" void AIResetStreamSampleCount() { }
extern "C" void AISetStreamPlayState(u32) { }
extern "C" void AISetStreamSampleRate(u32) { }
extern "C" void AISetStreamVolLeft(u8) { }
extern "C" void AISetStreamVolRight(u8) { }

extern "C" void pc_audio_report(void)
{
	OSReport("[AUDIO] %llu DMA frames produced, %llu callback underruns, fill %d\n", (unsigned long long)s_frames_produced,
	         (unsigned long long)s_underruns, pc_audio_buffer_fill());
}
