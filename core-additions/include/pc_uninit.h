#ifndef _PC_UNINIT_H
#define _PC_UNINIT_H

// PC: the -Wconditional-uninitialized probe.
//
// `-Wconditional-uninitialized` finds 40 sites the always-on census set misses:
// a `switch` with no `default` whose subject can take an unhandled value, or a
// search loop that assigns its result only on a match. At -O0 on GameCube these
// read stack residue and usually "worked"; on PC `-ftrivial-auto-var-init=zero`
// makes them a deterministic ZERO, which is a different wrong answer and a real
// divergence from hardware. `__Prepare_BGM`'s uninitialized `set` turning into
// a hard hang was that exact class, found the expensive way.
//
// Reading the other 39 cannot tell us which ones fire: most subjects are bank
// or stream FILE DATA, so the answer is a measurement, not an inference —
// measure, do not infer. So instrument first and run the gates, THEN fix only
// what the game actually reaches.
//
// Each site gets a `default:` (or a post-loop guard) that reports and then falls
// through exactly as before, so the probe is observationally neutral: it changes
// no value and takes no branch the build did not already take. The two sites
// where the fall-through is a null-pointer dereference are marked NULLGUARD and
// documented individually — those DO change behavior, on a path that would
// otherwise segfault.
//
// PC_UNINIT_PROBE=1 to arm. The first hit at each site prints immediately, so a
// run that crashes still tells us what fired before it; the full table prints at
// exit. Counters are deliberately unsynchronised — jaudio hits these from the
// audio thread — because a diagnostic tally does not need to be exact.

#if defined(TARGET_PC)

// No "types.h" here: it costs a -Wunused-variable in every warnings-on pc_* TU
// that includes this, and nothing in this interface needs its typedefs.

#ifdef __cplusplus
extern "C" {
#endif

// `site` must be a string literal with static lifetime; the table stores the
// pointer. Convention: "file.c:line var" so a hit maps straight back to the
// census line.
void pc_uninit_hit(const char* site, double value);

// Arm the probe. Call once, early in main() — see pc_uninit.cpp for why it is
// not a static constructor. Without this the table still fills, but a run with
// no hits reports nothing and cannot be told from a run with no probe.
void pc_uninit_arm(void);

// Dump the table. Registered with atexit() by pc_uninit_arm; also safe to call
// from the crash handler.
void pc_uninit_report(void);

#ifdef __cplusplus
}
#endif

// The value is whatever the switch subject / loop key was, widened to double so
// one entry point covers ints, floats, enums and pointers. Pass 0 when the
// subject is not a scalar worth recording.
#define PC_UNINIT_HIT(site, value) pc_uninit_hit((site), (double)(value))

#else

#define PC_UNINIT_HIT(site, value) ((void)0)

#endif // TARGET_PC

#endif // _PC_UNINIT_H
