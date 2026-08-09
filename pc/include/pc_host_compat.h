#ifndef PC_HOST_COMPAT_H
#define PC_HOST_COMPAT_H

// Force-included into every translation unit of the host build (see
// pc/CMakeLists.txt). Bridges MWCC/MSL-era expectations onto the host
// toolchain. Keep this minimal: add entries only when a compile error
// proves they're needed, and note where the original lived.

#include <math.h>
#include <stdint.h>

// --- LP64 pointer-cast vocabulary ---------------------------------------
// PC_PTR_TO_U32 marks a *deliberate* truncation: a GC-era 32-bit protocol
// slot (opaque owner token, DSP mailbox word, seq format parameter) that
// still carries a host pointer on the PC build. Every use is a known audit
// item owned by a shim milestone — grep for this macro to
// enumerate them. Do NOT use it to silence a cast error you haven't
// classified; pointer *arithmetic* must go through (uintptr_t) instead.
#define PC_PTR_TO_U32(p) ((u32)(uintptr_t)(p))

// PC_INT_AS_PTR is the other direction, and it is NOT the inverse: the value
// was never a host pointer to begin with. An ARAM offset (ARAM is its own
// 32-bit address space — pc_dsp.cpp resolves it as pc_aram_base() + offset),
// or a small integer riding in an OSMessage slot, stored into a pointer-typed
// field only because the GC API declared that field void*/u8*. It round-trips
// back out through (uintptr_t) and is never dereferenced as a host address, so
// unlike PC_PTR_TO_U32 nothing is lost. The explicit type argument keeps the
// result assignable in C++ (a bare void* is not).
#define PC_INT_AS_PTR(type, v) ((type)(uintptr_t)(v))

// --- MSL math constants (decomp include/stl/math.h, not on the host
// include path because its function decls conflict with host libc) -------
#ifndef TAU
#define LONG_TAU   6.2831854820251465
#define TAU        6.2831855f
#define PI         3.1415927f
#define HALF_PI    1.5707964f
#define THIRD_PI   1.0471976f
#define QUARTER_PI 0.7853982f
#define SIN_2_5    0.43633234f
#endif

// --- PPC intrinsics -----------------------------------------------------
// __frsqrte is the PowerPC reciprocal-square-root *estimate* instruction.
// Every caller (sysMath.h speedy_sqrtf, stl/math.h sqrtf) refines the
// estimate with Newton-Raphson iterations, so returning the exact value is
// a strict accuracy improvement, not a behavior change.
static inline double __frsqrte(double x) { return 1.0 / sqrt(x); }

#ifdef __cplusplus
// MWCC/MSL declares the C float math functions inside namespace std; the
// host's <math.h> keeps them global. Game headers (nlib/Math.h) spell
// them std::sqrtf etc.
#include <cmath>
namespace std {
using ::fmodf;
using ::sqrtf;
}
#else
// jaudio's C files use `nullptr` (types.h provides it only for C++).
#ifndef nullptr
#define nullptr ((void*)0)
#endif
#endif

// True when jaudio is enabled. Public Jac_* entry points early-return when it
// is off — grep pc_audio_enabled to find the gates.
//
// M7: this now reads audio.enabled from settings.ini (default 1), with PC_AUDIO
// still overriding. A double-clicked .app has no environment to set, so gating
// sound on an env var would have meant a shipped build was permanently silent.
#ifdef __cplusplus
extern "C" {
#endif
// Cooperative thread cancellation — see OSCancelThread in pc_sdk_shims.cpp.
// A worker started with OSCreateThread must poll this in its loop; returning
// TRUE means OSCancelThread is waiting for it to leave before the caller tears
// down memory it may be using.
int pc_thread_cancel_requested(void); // int, not BOOL: this header is included before types.h

int pc_config_int(const char* key, int fallback); // pc_config.h

static inline int pc_audio_enabled(void) {
	static int v = -1;
	if (v < 0) v = pc_config_int("audio.enabled", 1) ? 1 : 0;
	return v;
}
#ifdef __cplusplus
}
#endif

#endif // PC_HOST_COMPAT_H
