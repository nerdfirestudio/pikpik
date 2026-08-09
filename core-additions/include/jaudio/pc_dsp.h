#ifndef _JAUDIO_PC_DSP_H
#define _JAUDIO_PC_DSP_H

// PC: the DSP mailbox seam (M5 P2).
//
// On hardware, jaudio drives the Zelda-lineage DSP ucode through a three-word
// mailbox — DsetupTable (0x81) registers the shared tables once, DsyncFrame
// (0x82) renders one frame, DwaitFrame (0x80) syncs — and all synthesis state
// lives in main memory that the ucode reads directly. That makes the mailbox
// the natural seam: everything ABOVE it (all ~22K LOC of jaudio) runs
// unmodified, and only the frame renderer below it has to be reimplemented.
// See docs/ARCHITECTURE.md.
//
// dspproc.c packs those commands into a `u32 commands[5]` array, which also
// truncates the table pointers on LP64 (an M1 audit item filed as "M5 replaces
// wholesale"). We do exactly that: on TARGET_PC the three live commands call
// straight in here with real pointers, and the u32 packing is bypassed.

#if defined(TARGET_PC)

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registered once, from dspinterface.c:426. chBuf is DSPchannel_[chBufLength]
// (the Zelda VPB array), fxBuf is FXBuffer[4], and the two filter tables are
// the resampling and ADPCM coefficient sets.
void pc_dsp_setup_table(u32 chBufLength, void* chBuf, void* resFilter, void* adpcmFilter, void* fxBuf);

// Render one frame of JAC_FRAMESAMPLES (560) samples per channel.
//
// THE OUTPUT IS TWO PLANAR CHANNEL BLOCKS, NOT INTERLEAVED, and the channel
// order is the opposite of what the argument names suggest. dspbuf.c calls
// DsyncFrame(7, &buf[0], &buf[560]) on a 1120-sample buffer, and Jac_VframeWork
// then does:
//
//     Jac_imixcopy(&buf[560], &buf[0], dac, 560);   // *dac++ = *ta++, *dac++ = *tb++
//
// so buf[560..1120) supplies the FIRST slot of each interleaved DAC frame (the
// left channel) and buf[0..560) supplies the second (the right). In other
// words `dspbufStart` is the RIGHT channel base and `dspbufEnd` is the LEFT.
// This was pinned by reading the mixdown, as the M5 plan asked.
void pc_dsp_sync_frame(u32 subframes, s16* rightOut, s16* leftOut);

// DSP_MIXERLEVEL, which DsyncFrame packs into the low 16 bits of its command
// word and the ucode applies to the final mixdown. Jac_SetMixerLevel(1.1, 2.0)
// makes it 8192 (4096 * 2.0) against a 0x4000 default, so ignoring it left the
// mix twice as loud as the hardware's.
void pc_dsp_set_mixer_level(u16 level);

// Direct-PCM streams (syncstream.c's Play_DirectPCM) play from MAIN MEMORY,
// not ARAM: the VPB's samplesSourceType is 0x21 and baseAddress would have to
// hold a host pointer. baseAddress is a u32 and the VPB's 0x180 stride is
// verified against hardware, so the pointer is kept in a side table keyed by
// voice index instead of widening the field.
void pc_dsp_set_direct_pcm(u8 idx, s16* addr);

// Diagnostic: snapshot enabled voices at a scene change and report survivors.
void pc_dsp_mark_scene_change(void);

// Seconds of audio rendered since boot — the wav-dump clock. Use for
// correlating driver-side events with listening timestamps.
double pc_dsp_time(void);

// Looping-voice attribution (PC_DSP_LOOPTAG=1). v89 measured the menu static
// as looping voices accumulating (0,0,2,3,3,4 across surveys) with
// endRequested never set — so the key-off never reaches them. Tag every DSP
// channel with the SOUNDID_ and owner that started it, and the renderer
// periodically reports each looping voice still enabled: its age, its owner,
// and whether that owner still points back at the channel. kind: 'N' melodic
// note-on, 'P' perc, 'O' osc, 'W' promoted from the wait queue, 'S' stream.
void pc_dsp_tag_voice(u32 idx, u32 soundValue, int soundId, u8 note, char kind, const void* owner);
void pc_dsp_untag_voice(u32 idx);
// DeAllocDSPchannel refused a dealloc because the caller's id does not match
// the channel's recorded owner — the shape that orphans a voice forever.
void pc_dsp_note_dealloc_miss(u32 idx, const void* callerId, const void* owner);

// Stop any voice still reading ARAM inside [start, start+size). Called when a
// wave archive's ARAM heap is released: a voice left playing there will be
// decoding whatever the NEXT archive loads over it.
void pc_dsp_stop_voices_in_aram(u32 start, u32 size);
void pc_dsp_relocate_voices_in_aram(u32 src, u32 dst, u32 size);

// DwaitFrame / DSPWaitFinish. We render synchronously inside sync_frame, so
// there is nothing to wait for — kept as a seam in case the renderer is later
// moved onto its own thread.
void pc_dsp_wait_frame(void);

#ifdef __cplusplus
}
#endif

#endif // TARGET_PC

#endif
