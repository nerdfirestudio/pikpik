// Crash reporter for the host build.
//
// Why this exists: the -O2 campaign (docs/ARCHITECTURE.md) can only gate what
// headless automation can reach, and headless automation cannot PLAY. Input
// is scripted from a fixed retrace schedule, so it can walk and mash A but it
// cannot pluck a Pikmin, fight a boss, or navigate to a later course. Every
// crash in real gameplay is therefore found by a human, and asking that human
// to re-run under lldb to get a backtrace is a bad trade — the repro may be
// twenty minutes in and may not repeat.
//
// So the binary reports its own crashes. On a fatal signal this writes the
// signal, the faulting address and a symbolised backtrace to stderr, then
// restores the default handler and re-raises so the OS still writes its
// normal .ips crash report.
//
// Everything here is async-signal-safe: backtrace() and backtrace_symbols_fd()
// do not allocate (backtrace_symbols(), which returns a malloc'd array, is
// deliberately NOT used), and the formatting goes through a fixed stack
// buffer. That also satisfies this port's standing rule that nothing running
// before gsys->Initialise() may allocate — the game replaces operator new and
// it returns NULL until the arena exists.

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace {

void write_str(const char* s) { (void)!write(STDERR_FILENO, s, strlen(s)); }

// Minimal unsigned-hex writer — snprintf is not async-signal-safe.
void write_hex(unsigned long long v)
{
	char buf[19];
	int i    = sizeof(buf) - 1;
	buf[i--] = '\0';
	if (v == 0) {
		buf[i--] = '0';
	}
	while (v && i >= 2) {
		const char* d = "0123456789abcdef";
		buf[i--]      = d[v & 0xF];
		v >>= 4;
	}
	buf[i--] = 'x';
	buf[i]   = '0';
	write_str(&buf[i]);
}

const char* signal_name(int sig)
{
	switch (sig) {
	case SIGSEGV: return "SIGSEGV (bad memory access)";
	case SIGBUS:  return "SIGBUS (bad address / write to read-only memory)";
	case SIGILL:  return "SIGILL (illegal instruction)";
	case SIGFPE:  return "SIGFPE (arithmetic fault)";
	case SIGABRT: return "SIGABRT (abort)";
	case SIGTRAP: return "SIGTRAP (trap)";
	default:      return "signal";
	}
}

void handler(int sig, siginfo_t* info, void* uap)
{
	write_str("\n================ [PC] CRASH ================\n");
	write_str("signal: ");
	write_str(signal_name(sig));
	if (info && (sig == SIGSEGV || sig == SIGBUS)) {
		write_str("\nfault address: ");
		write_hex((unsigned long long)(unsigned long)info->si_addr);
	}
#if defined(__arm64__) || defined(__aarch64__)
	// The faulting instruction itself. backtrace() walks frame pointers, so a
	// LEAF function — one that never pushes a frame — does not appear in it,
	// and the innermost frame is exactly the one worth having. Verified: a
	// deliberate null store in a leaf showed the caller as frame 2 with the
	// leaf missing entirely. Taking the PC out of the signal context closes
	// that gap.
	if (uap) {
		const ucontext_t* uc = (const ucontext_t*)uap;
		if (uc->uc_mcontext) {
			// Symbolise it through backtrace_symbols_fd rather than printing
			// raw hex: the raw value needs the ASLR slide before atos can do
			// anything with it, and this prints module + symbol + offset
			// directly.
			void* pc[2];
			pc[0] = (void*)(unsigned long)uc->uc_mcontext->__ss.__pc;
			pc[1] = (void*)(unsigned long)uc->uc_mcontext->__ss.__lr;
			write_str("\nfaulting instruction:\n");
			backtrace_symbols_fd(&pc[0], 1, STDERR_FILENO);
			write_str("called from (link register):\n");
			backtrace_symbols_fd(&pc[1], 1, STDERR_FILENO);
		}
	}
#endif
	write_str("backtrace (frame-pointer walk; a leaf frame may be absent —\n"
	          "           the two lines above are the innermost site):\n");

	void* frames[64];
	int n = backtrace(frames, 64);
	// Writes straight to the fd without allocating.
	backtrace_symbols_fd(frames, n, STDERR_FILENO);

	write_str("============================================\n"
	          "Paste the block above. To turn addresses into file:line:\n"
	          "  atos -o <the Pikmin binary> -l <load address> <address>\n"
	          "============================================\n");

	// Restore the default disposition and re-raise, so the OS still produces
	// its own crash report in ~/Library/Logs/DiagnosticReports.
	signal(sig, SIG_DFL);
	raise(sig);
}

} // namespace

extern "C" void pc_crash_handler_install(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);

	static const int kSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
	for (unsigned i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); i++) {
		sigaction(kSignals[i], &sa, nullptr);
	}
}
