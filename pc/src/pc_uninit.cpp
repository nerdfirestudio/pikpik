// pc_uninit.cpp — host side of the -Wconditional-uninitialized probe.
//
// See include/pc_uninit.h for why this exists. Short version: 40 census sites
// read a variable that a `switch` with no `default` (or a search loop with no
// match) may never have assigned. Which of them the game actually REACHES is a
// measurement, so this counts them over the recorded gates instead of guessing
// from the source.

#include "pc_host_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "pc_uninit.h"

// Deliberately NOT including Dolphin/os.h for OSReport: that header costs 18
// -Wunused-variable / -Wdeprecated-register warnings in any pc_* TU that pulls
// it in (pc_dsp.cpp already pays them), and this branch exists to protect the
// clean-build property. OSReport on PC is exactly vfprintf(stderr, ...)
// (pc_sdk_shims.cpp:51), so stderr directly is the same output with none of the
// noise — the same choice pc_crash.cpp makes.
#define UNINIT_REPORT(...) fprintf(stderr, __VA_ARGS__)

namespace
{

// One row per site. 64 is comfortably above the 40 census sites; overflow is
// reported rather than silently dropped, because a probe that lies about its
// own coverage is worse than no probe.
struct Site {
	const char* name;
	unsigned count;
	double first;
	double last;
	double lo;
	double hi;
};

const int MAX_SITES = 64;
Site sites[MAX_SITES];
int nSites   = 0;
unsigned nDropped = 0;

int probeEnabled()
{
	static int on = -1;
	if (on < 0) {
		on = getenv("PC_UNINIT_PROBE") ? 1 : 0;
	}
	return on;
}

} // namespace

extern "C" void pc_uninit_report(void)
{
	if (!probeEnabled()) {
		return;
	}

	UNINIT_REPORT("[UNINIT] === %d site(s) reached ===\n", nSites);
	for (int i = 0; i < nSites; i++) {
		const Site& s = sites[i];
		if (s.lo == s.hi) {
			UNINIT_REPORT("[UNINIT] %-34s hits=%-8u value=%g\n", s.name, s.count, s.lo);
		} else {
			UNINIT_REPORT("[UNINIT] %-34s hits=%-8u range=%g..%g first=%g last=%g\n", s.name, s.count, s.lo, s.hi, s.first,
			         s.last);
		}
	}
	if (nDropped) {
		UNINIT_REPORT("[UNINIT] %u hit(s) DROPPED — table full, raise MAX_SITES\n", nDropped);
	}
	if (nSites == 0) {
		UNINIT_REPORT("[UNINIT] no instrumented fall-through was reached in this run\n");
	}
}

// Called from main() before gsys->Initialise(). Deliberately not a file-scope
// constructor: nothing that runs before the arena exists may allocate, and an
// explicit call from the one place that is known to be safe beats relying on
// static-init order. Reads the environment and calls atexit, nothing more.
// A replay whose input runs out PARKS rather than exiting (that is the trick the
// menu reproducer relies on), so most gates end in a kill — and a kill skips
// atexit, which is why every site also prints its first hit immediately. Handling
// the terminating signals recovers the per-site TOTALS as well, which is what
// separates "fires once at boot" from "fires every frame".
//
// fprintf in a signal handler is not async-signal-safe. Accepted deliberately:
// this is diagnostic-only tooling, the process is being torn down anyway, and
// the alternative is having no counts at all. pc_crash.cpp, which has to survive
// a genuine SIGSEGV, uses raw write() for exactly the reason this does not.
extern "C" void pc_heap_report(void); // sysNew.cpp, self-gating on PC_HEAPDIAG

static void reportAndExit(int sig)
{
	pc_uninit_report();
	// PC: end-of-session reports that rely on atexit() are LOST here, because
	// _exit() below deliberately skips atexit handlers. That silently cost the
	// heap high-water report on every bounded gate — the report existed, was
	// registered, and simply never ran. Anything that summarises a run belongs
	// on this path too, not only on atexit.
	pc_heap_report();
	_exit(128 + sig);
}

extern "C" void pc_uninit_arm(void)
{
	if (probeEnabled()) {
		UNINIT_REPORT("[UNINIT] probe armed\n");
		atexit(pc_uninit_report);
		signal(SIGTERM, reportAndExit);
		signal(SIGINT, reportAndExit);
	}
}

extern "C" void pc_uninit_hit(const char* site, double value)
{
	if (!probeEnabled()) {
		return;
	}

	// Sites are string literals, so the pointer compare hits on every repeat
	// visit; strcmp is the fallback for the same site name from two TUs.
	for (int i = 0; i < nSites; i++) {
		if (sites[i].name == site || strcmp(sites[i].name, site) == 0) {
			Site& s = sites[i];
			s.count++;
			s.last = value;
			if (value < s.lo) {
				s.lo = value;
			}
			if (value > s.hi) {
				s.hi = value;
			}
			return;
		}
	}

	if (nSites >= MAX_SITES) {
		nDropped++;
		return;
	}

	Site& s = sites[nSites++];
	s.name  = site;
	s.count = 1;
	s.first = value;
	s.last  = value;
	s.lo    = value;
	s.hi    = value;

	// First hit prints immediately: several of these sites are on the audio
	// thread and one of them (syncstream.c:168) faults on the very fall-through
	// being measured, so a run that dies must still report what it reached.
	UNINIT_REPORT("[UNINIT] FIRST %s value=%g\n", site, value);
}
