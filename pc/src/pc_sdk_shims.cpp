// pc_sdk_shims.cpp — host implementations of the Dolphin SDK surface the
// game links against (see docs/ARCHITECTURE.md). M2 scope: enough to link
// and boot headless. GX lives in pc_gx_stub.cpp.
//
// Design notes:
// - Opaque GC structs (threads, queues, mutexes) are backed by side-tables
//   keyed on the game's struct pointer, so no GC struct layout is relied on.
// - ARAM is a host buffer; ARQ "DMA" is memcpy (offsets < ARAM size on the
//   ARAM side, host pointers on the main-memory side — see the widened
//   signatures in include/Dolphin/ar.h).
// - The DSP mailbox pretends the ucode handshake succeeded (0x88881357) so
//   jaudio boots; real synthesis is M5 (docs/ARCHITECTURE.md).
// - CARD reports NOCARD everywhere until M4.

#include <errno.h>
#include <map>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "pc_paths.h"

#include "Dolphin/ai.h"
#include "Dolphin/ar.h"
#include "Dolphin/card.h"
#include "Dolphin/dvd.h"
#include "Dolphin/exi.h"
#include "Dolphin/hio.h"
#include "Dolphin/gx.h"
#include "Dolphin/mtx.h"
#include "Dolphin/os.h"
#include "Dolphin/pad.h"
#include "Dolphin/vi.h"

// ---------------------------------------------------------------- logging
static bool pc_verbose() {
	static int v = -1;
	if (v < 0) v = getenv("PC_VERBOSE") ? 1 : 0;
	return v == 1;
}

extern "C" {

void OSReport(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void OSPanic(const char* file, int line, const char* fmt, ...) {
	fprintf(stderr, "[OSPanic] %s:%d: ", file, line);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	abort();
}

// ------------------------------------------------------------------- time
#define PC_TIMER_CLOCK 40500000ull // GC bus/4

static u64 pc_now_ns() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

OSTime OSGetTime() { return (s64)((__uint128_t)pc_now_ns() * PC_TIMER_CLOCK / 1000000000ull); }
u32 OSGetTick() { return (u32)OSGetTime(); }
u32 __OSBusClock = 162000000;

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
	(void)ticks;
	time_t t = time(nullptr);
	struct tm tmv;
	localtime_r(&t, &tmv);
	memset(td, 0, sizeof(*td));
	td->sec  = tmv.tm_sec;
	td->min  = tmv.tm_min;
	td->hour = tmv.tm_hour;
	td->mday = tmv.tm_mday;
	td->mon  = tmv.tm_mon;
	td->year = tmv.tm_year + 1900;
}

// ------------------------------------------------------------ arena/heaps
#define PC_ARENA_SIZE (64u << 20)
static u8* s_arena;
static uintptr_t s_arena_lo, s_arena_hi, s_heap_cursor;

static void pc_arena_init() {
	if (s_arena) return;
	s_arena = (u8*)calloc(1, PC_ARENA_SIZE);
	s_arena_lo = ((uintptr_t)s_arena + 31) & ~(uintptr_t)31;
	s_arena_hi = (uintptr_t)s_arena + PC_ARENA_SIZE;
	s_heap_cursor = s_arena_lo;
}

void OSInit() { pc_arena_init(); }
void* OSGetArenaLo() { pc_arena_init(); return (void*)s_arena_lo; }
void* OSGetArenaHi() { pc_arena_init(); return (void*)s_arena_hi; }

// The game calls OSInitAlloc then carves one big AyuHeap out of the arena;
// OSAllocFromHeap is only a bootstrap allocator here.
void* OSInitAlloc(void* lo, void* hi, int maxHeaps) {
	(void)hi; (void)maxHeaps;
	return lo;
}
volatile s32 __OSCurrHeap = 0;
void* OSAllocFromHeap(s32 heap, u32 size) {
	(void)heap;
	pc_arena_init();
	uintptr_t p = (s_heap_cursor + 31) & ~(uintptr_t)31;
	if (p + size > s_arena_hi) return nullptr;
	s_heap_cursor = p + size;
	return (void*)p;
}

// -------------------------------------------------------------- interrupts
BOOL OSDisableInterrupts() { return TRUE; }
BOOL OSRestoreInterrupts(BOOL level) { (void)level; return TRUE; }
// Interrupt handlers are recorded rather than dropped: M5's DSP seam has to
// raise __OS_INTERRUPT_DSP_DSP itself, because on hardware the ucode's
// per-subframe interrupt is what advances jaudio's frame pipeline (see
// pc_dsp.cpp). Nothing else fires yet.
#define PC_MAX_INTERRUPTS 32
static __OSInterruptHandler s_int_handlers[PC_MAX_INTERRUPTS];

__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler) {
	if (interrupt < 0 || interrupt >= PC_MAX_INTERRUPTS) return nullptr;
	__OSInterruptHandler old = s_int_handlers[interrupt];
	s_int_handlers[interrupt] = handler;
	return old;
}

void pc_os_raise_interrupt(int interrupt) {
	if (interrupt < 0 || interrupt >= PC_MAX_INTERRUPTS) return;
	__OSInterruptHandler h = s_int_handlers[interrupt];
	if (!h) {
		static int reported[PC_MAX_INTERRUPTS];
		if (!reported[interrupt]) {
			reported[interrupt] = 1;
			OSReport("[PC] interrupt %d raised with no handler registered\n", interrupt);
		}
		return;
	}
	h((__OSInterrupt)interrupt, nullptr);
}
OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask) { return 0; }

// ------------------------------------------------------------- cache/misc
void DCFlushRange(void*, u32) { }
void DCFlushRangeNoSync(void*, u32) { }
void DCInvalidateRange(void*, u32) { }
void DCStoreRange(void*, u32) { }
void DCStoreRangeNoSync(void*, u32) { }
void DCZeroRange(void* p, u32 n) { memset(p, 0, n); }
void PPCSync() { }
void OSClearContext(OSContext*) { }
void OSSetCurrentContext(OSContext*) { }
u32 OSGetStackPointer() { return (u32)(uintptr_t)__builtin_frame_address(0); }
BOOL OSGetResetSwitchState() { return FALSE; }
u32 OSGetProgressiveMode() { return 0; }
void OSSetProgressiveMode(u32) { }
u32 OSGetSoundMode() { return 1; /* stereo */ }
void OSSetSoundMode(u32) { }
void OSResetSystem(int, u32, BOOL) {
	fprintf(stderr, "[PC] OSResetSystem -> exit\n");
	exit(0);
}

// ---------------------------------------------------------------- threads
struct PCThread {
	pthread_t handle;
	OSThreadStartFunction func;
	void* param;
	bool started;
	bool terminated;
};
static pthread_mutex_t s_os_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_os_cond = PTHREAD_COND_INITIALIZER;
#define PC_MAX_THREADS 32
static OSThread* s_thread_keys[PC_MAX_THREADS];
static PCThread s_thread_pool[PC_MAX_THREADS];
static int s_nthreads;
static PCThread* pc_thread_find(OSThread* t) {
	for (int i = 0; i < s_nthreads; i++)
		if (s_thread_keys[i] == t) return &s_thread_pool[i];
	return nullptr;
}

static void* pc_thread_tramp(void* arg) {
	PCThread* t = (PCThread*)arg;
	void* r = t->func(t->param);
	pthread_mutex_lock(&s_os_lock);
	t->terminated = true;
	pthread_cond_broadcast(&s_os_cond);
	pthread_mutex_unlock(&s_os_lock);
	return r;
}

BOOL OSCreateThread(OSThread* thread, OSThreadStartFunction func, void* param, void* stack, u32 stackSize,
                    OSPriority priority, u16 attr) {
	(void)stack; (void)stackSize; (void)priority; (void)attr;
	pthread_mutex_lock(&s_os_lock);
	PCThread* tp = pc_thread_find(thread);
	if (!tp && s_nthreads < PC_MAX_THREADS) {
		s_thread_keys[s_nthreads] = thread;
		tp = &s_thread_pool[s_nthreads++];
	}
	if (!tp) { pthread_mutex_unlock(&s_os_lock); return FALSE; }
	PCThread& t = *tp;
	t.func = func;
	t.param = param;
	t.started = false;
	t.terminated = false;
	pthread_mutex_unlock(&s_os_lock);
	return TRUE;
}

s32 OSResumeThread(OSThread* thread) {
	pthread_mutex_lock(&s_os_lock);
	PCThread* t = pc_thread_find(thread);
	if (t && !t->started) {
		t->started = true;
		pthread_create(&t->handle, nullptr, pc_thread_tramp, t);
	}
	pthread_mutex_unlock(&s_os_lock);
	return 1;
}

void OSExitThread(void*) { pthread_exit(nullptr); }
// PC: on GC OSCancelThread genuinely terminates the target thread, and the
// movie section leans on that — MovSampleSection::update() cancels its playback
// thread and then calls softReset(), which resets the heap and allocates a
// fresh 55 MB app stack straight over the memory the HVQM decoder is working
// in. As a no-op this left the decoder running against memory the MAIN thread
// was concurrently zeroing. Found with a watchpoint on the decoder's tree
// pointer: it went to 0x0000000600000000 inside System::alloc, on thread #1,
// while the playback thread was mid-DecodeBpic.
//
// pthread_cancel is the wrong tool — it would fire at whatever cancellation
// point the decoder happened to be in and leave the workspace half-updated.
// Cancellation is instead COOPERATIVE: raise a flag the target's own loop
// checks via pc_thread_cancel_requested(), then wait for it to actually leave.
// The wait is bounded so a thread that never polls cannot hang the game; it
// says so loudly instead.
static bool s_thread_cancel[PC_MAX_THREADS];

extern "C" int pc_thread_cancel_requested(void) {
	pthread_t self = pthread_self();
	int r          = 0;
	pthread_mutex_lock(&s_os_lock);
	for (int i = 0; i < s_nthreads; i++) {
		if (s_thread_pool[i].started && pthread_equal(s_thread_pool[i].handle, self)) {
			r = s_thread_cancel[i] ? 1 : 0;
			break;
		}
	}
	pthread_mutex_unlock(&s_os_lock);
	return r;
}

void OSCancelThread(OSThread* thread) {
	pthread_mutex_lock(&s_os_lock);
	int idx = -1;
	for (int i = 0; i < s_nthreads; i++) {
		if (s_thread_keys[i] == thread) { idx = i; break; }
	}
	if (idx < 0) { pthread_mutex_unlock(&s_os_lock); return; }
	s_thread_cancel[idx] = true;

	PCThread* t = &s_thread_pool[idx];
	if (!t->started || t->terminated) { pthread_mutex_unlock(&s_os_lock); return; }

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += 2;
	while (!t->terminated) {
		if (pthread_cond_timedwait(&s_os_cond, &s_os_lock, &ts) == ETIMEDOUT) {
			OSReport("[PC] OSCancelThread: thread %p did not stop within 2s — it is not polling "
			         "pc_thread_cancel_requested(), and whatever it touches may be freed underneath it\n",
			         (void*)thread);
			break;
		}
	}
	pthread_mutex_unlock(&s_os_lock);
}
BOOL OSJoinThread(OSThread* thread, void** val) {
	pthread_mutex_lock(&s_os_lock);
	PCThread* t = pc_thread_find(thread);
	pthread_mutex_unlock(&s_os_lock);
	if (t && t->started) pthread_join(t->handle, val);
	return TRUE;
}
void OSYieldThread() { sched_yield(); }
OSThread* OSGetCurrentThread() { return (OSThread*)pthread_self(); }

// main() (sysBootup.cpp) runs the game on the process main thread, and only
// that thread may touch Cocoa/Metal — so anything that presents or pumps SDL
// has to be able to ask. AppKit throws outright otherwise (jaudio's DVD
// thread reaching the loading-screen tick was the first case).
extern "C" int pc_is_main_thread(void) { return pthread_main_np() != 0; }
OSPriority OSGetThreadPriority(OSThread*) { return 16; }
BOOL OSIsThreadTerminated(OSThread* thread) {
	pthread_mutex_lock(&s_os_lock);
	PCThread* t = pc_thread_find(thread);
	bool dead = !t || (t->started && t->terminated);
	pthread_mutex_unlock(&s_os_lock);
	return dead;
}
BOOL OSCheckActiveThreads() { return TRUE; }
void OSInitThreadQueue(OSThreadQueue*) { }

// Sleep/wake on a thread queue: single global cond keyed by queue address.
#define PC_MAX_SLEEPQ 32
static void* s_sleep_keys[PC_MAX_SLEEPQ];
static int s_sleep_gen_pool[PC_MAX_SLEEPQ];
static int s_nsleep;
static int* pc_sleep_gen(void* q) {
	for (int i = 0; i < s_nsleep; i++)
		if (s_sleep_keys[i] == q) return &s_sleep_gen_pool[i];
	int i = (s_nsleep < PC_MAX_SLEEPQ) ? s_nsleep++ : 0;
	s_sleep_keys[i] = q;
	s_sleep_gen_pool[i] = 0;
	return &s_sleep_gen_pool[i];
}
void OSSleepThread(OSThreadQueue* queue) {
	pthread_mutex_lock(&s_os_lock);
	int* g = pc_sleep_gen(queue);
	int gen = *g;
	while (*g == gen)
		pthread_cond_wait(&s_os_cond, &s_os_lock);
	pthread_mutex_unlock(&s_os_lock);
}
void OSWakeupThread(OSThreadQueue* queue) {
	pthread_mutex_lock(&s_os_lock);
	(*pc_sleep_gen(queue))++;
	pthread_cond_broadcast(&s_os_cond);
	pthread_mutex_unlock(&s_os_lock);
}

// ---------------------------------------------------------- message queues
// Fixed pools only: the shim must NEVER allocate through the game's global
// operator new (heap races across threads; the game heap is not
// thread-safe). Learned the hard way when the VI pump thread grew a
// std::vector into System::alloc.
#define PC_QUEUE_RING 256
#define PC_MAX_QUEUES 64
struct PCQueue {
	OSMessage ring[PC_QUEUE_RING];
	int head, count;
	size_t cap;
};
static OSMessageQueue* s_queue_keys[PC_MAX_QUEUES];
static PCQueue s_queue_pool[PC_MAX_QUEUES];
static int s_nqueues;

static PCQueue& pc_queue(OSMessageQueue* q) {
	for (int i = 0; i < s_nqueues; i++)
		if (s_queue_keys[i] == q) return s_queue_pool[i];
	int i = (s_nqueues < PC_MAX_QUEUES) ? s_nqueues++ : 0;
	s_queue_keys[i] = q;
	s_queue_pool[i].head = s_queue_pool[i].count = 0;
	s_queue_pool[i].cap = PC_QUEUE_RING;
	return s_queue_pool[i];
}

void OSInitMessageQueue(OSMessageQueue* queue, OSMessage* msgArray, s32 msgCount) {
	(void)msgArray;
	pthread_mutex_lock(&s_os_lock);
	PCQueue& q = pc_queue(queue);
	q.head = q.count = 0;
	q.cap = (msgCount > 0 && msgCount < PC_QUEUE_RING) ? (size_t)msgCount : PC_QUEUE_RING;
	pthread_mutex_unlock(&s_os_lock);
}

BOOL OSSendMessage(OSMessageQueue* queue, OSMessage msg, s32 flags) {
	pthread_mutex_lock(&s_os_lock);
	PCQueue& q = pc_queue(queue);
	while ((size_t)q.count >= q.cap) {
		if (!(flags & 1 /* OS_MESSAGE_BLOCK */)) {
			pthread_mutex_unlock(&s_os_lock);
			return FALSE;
		}
		pthread_cond_wait(&s_os_cond, &s_os_lock);
	}
	q.ring[(q.head + q.count) % PC_QUEUE_RING] = msg;
	q.count++;
	pthread_cond_broadcast(&s_os_cond);
	pthread_mutex_unlock(&s_os_lock);
	return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* queue, OSMessage* msgPtr, s32 flags) {
	pthread_mutex_lock(&s_os_lock);
	PCQueue& q = pc_queue(queue);
	while (q.count == 0) {
		if (!(flags & 1)) {
			pthread_mutex_unlock(&s_os_lock);
			return FALSE;
		}
		pthread_cond_wait(&s_os_cond, &s_os_lock);
	}
	if (msgPtr) *msgPtr = q.ring[q.head];
	q.head = (q.head + 1) % PC_QUEUE_RING;
	q.count--;
	pthread_cond_broadcast(&s_os_cond);
	pthread_mutex_unlock(&s_os_lock);
	return TRUE;
}

// ------------------------------------------------------------ mutex / cond
#define PC_MAX_MUTEXES 64
static void* s_mutex_keys[PC_MAX_MUTEXES];
static pthread_mutex_t s_mutex_pool[PC_MAX_MUTEXES];
static int s_nmutexes;
static pthread_mutex_t* pc_mutex(void* m) {
	for (int i = 0; i < s_nmutexes; i++)
		if (s_mutex_keys[i] == m) return &s_mutex_pool[i];
	int i = (s_nmutexes < PC_MAX_MUTEXES) ? s_nmutexes++ : 0;
	s_mutex_keys[i] = m;
	pthread_mutexattr_t at;
	pthread_mutexattr_init(&at);
	pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&s_mutex_pool[i], &at);
	return &s_mutex_pool[i];
}
static void* s_cond_keys[PC_MAX_MUTEXES];
static pthread_cond_t s_cond_pool[PC_MAX_MUTEXES];
static int s_nconds;
static pthread_cond_t* pc_cond(void* c) {
	for (int i = 0; i < s_nconds; i++)
		if (s_cond_keys[i] == c) return &s_cond_pool[i];
	int i = (s_nconds < PC_MAX_MUTEXES) ? s_nconds++ : 0;
	s_cond_keys[i] = c;
	pthread_cond_init(&s_cond_pool[i], nullptr);
	return &s_cond_pool[i];
}

void OSInitMutex(OSMutex* m) {
	pthread_mutex_lock(&s_os_lock);
	pc_mutex(m);
	pthread_mutex_unlock(&s_os_lock);
}
void OSLockMutex(OSMutex* m) {
	pthread_mutex_lock(&s_os_lock);
	pthread_mutex_t* pm = pc_mutex(m);
	pthread_mutex_unlock(&s_os_lock);
	pthread_mutex_lock(pm);
}
void OSUnlockMutex(OSMutex* m) {
	pthread_mutex_lock(&s_os_lock);
	pthread_mutex_t* pm = pc_mutex(m);
	pthread_mutex_unlock(&s_os_lock);
	pthread_mutex_unlock(pm);
}

void OSInitCond(OSCond* c) {
	pthread_mutex_lock(&s_os_lock);
	pc_cond(c);
	pthread_mutex_unlock(&s_os_lock);
}
void OSWaitCond(OSCond* c, OSMutex* m) {
	pthread_mutex_lock(&s_os_lock);
	pthread_cond_t* pc = pc_cond(c);
	pthread_mutex_t* pm = pc_mutex(m);
	pthread_mutex_unlock(&s_os_lock);
	pthread_cond_wait(pc, pm);
}
void OSSignalCond(OSCond* c) {
	pthread_mutex_lock(&s_os_lock);
	pthread_cond_t* pc = pc_cond(c);
	pthread_mutex_unlock(&s_os_lock);
	pthread_cond_broadcast(pc);
}

// --------------------------------------------------------------------- VI
static u32 s_retrace_count;
static VIRetraceCallback s_post_retrace_cb;

// On GC the VI retrace interrupt fires asynchronously; game code blocks in
// waitPostRetrace on a message the callback sends. Fire it from a 60 Hz
// pump thread, never synchronously from VIWaitForRetrace (deadlock).
static bool s_vi_pump_running;
static void* pc_vi_pump(void*) {
	// Absolute-deadline 60 Hz: relative nanosleep(16.67ms) oversleeps 1-3ms
	// per tick on macOS, silently running the "60 Hz" pump at ~50-57 Hz —
	// which paced the whole game at ~25 fps (2 retraces x ~20ms). Sleeping
	// to a fixed deadline grid self-corrects each tick.
	// Don't let the scheduler starve the tick thread under render load.
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	const long kPeriodNs = 16666667;
	for (;;) {
		next.tv_nsec += kPeriodNs;
		while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long ns = (next.tv_sec - now.tv_sec) * 1000000000L + (next.tv_nsec - now.tv_nsec);
		if (ns > 0) {
			struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
			nanosleep(&ts, nullptr);
		} else if (ns < -kPeriodNs) {
			// Behind by more than one period (scheduling jitter, vsync
			// contention, debugger): DROP the backlog instead of bursting
			// ticks — catch-up bursts read as a 1 Hz hitch-then-sprint
			// stutter in the interactive window. Smoothness beats exact
			// 60.000 Hz; a dropped tick is just one 33ms frame.
			next = now;
		}
		s_retrace_count++;
		VIRetraceCallback cb = s_post_retrace_cb;
		if (cb) cb(s_retrace_count);
	}
	return nullptr;
}
static void pc_vi_pump_start() {
	if (s_vi_pump_running) return;
	s_vi_pump_running = true;
	pthread_t t;
	pthread_create(&t, nullptr, pc_vi_pump, nullptr);
}

void VIInit() { pc_vi_pump_start(); }
void VIConfigure(const GXRenderModeObj*) { }
void VIFlush() { }
void VISetBlack(BOOL) { }
void VISetNextFrameBuffer(void*) { }
u32 VIGetRetraceCount() { return s_retrace_count; }
u32 VIGetDTVStatus() { return 0; }
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
	VIRetraceCallback old = s_post_retrace_cb;
	s_post_retrace_cb = cb;
	return old;
}
void pc_window_frame(void); // pc_window.mm — pump events + present
extern "C" void pc_gx_frame_flush(void);
static double pc_now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static double pc_thread_cpu_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
void VIWaitForRetrace() {
	pc_vi_pump_start();
	// PC_PERF=1: split each frame into game logic / flush (seg submit) /
	// render+present / retrace wait, rolling 120-frame averages.
	static int s_perf = -1;
	if (s_perf < 0) s_perf = getenv("PC_PERF") ? 1 : 0;
	// The game bucket's WALL time includes the game's own pacing sleep
	// (waitPostRetrace blocks inside game code until its 2-retrace grid), so
	// it pads to ~one retrace period under any load below the cliff. The
	// THREAD-CPU counterpart excludes blocked time: "game 16.6 (cpu 3.1)"
	// means 3.1ms of real work, 13.5ms of grid sleep. Render's cpu split
	// separates encode work from a blocking nextDrawable (GPU-bound).
	static double s_last_exit, s_last_exit_cpu, s_acc[4], s_acc_cpu[2];
	static int s_nacc;
	double t0 = pc_now_ms(), c0 = pc_thread_cpu_ms();
	// Anchor the retrace wait BEFORE our render+present: a blocking present
	// (CAMetalLayer vsync) can itself cross a retrace boundary, and that
	// boundary must satisfy this call's wait or we pace 3 retraces/frame.
	u32 seen = s_retrace_count;
	pc_gx_frame_flush();
	double t1 = pc_now_ms(), c1 = pc_thread_cpu_ms();
	pc_window_frame();
	double t2 = pc_now_ms(), c2 = pc_thread_cpu_ms();
	// Pacing: GC semantics — VIWaitForRetrace blocks until the NEXT retrace
	// boundary (per-call anchor). The game paces itself: waitPostRetrace
	// (dgxGraphics) already blocks until mFrameRate(2) retraces have elapsed,
	// so this final boundary wait totals exactly 2 retraces/frame = 30 fps
	// (GPIE01 is NTSC). History: a persistent-anchor "2 since my last exit"
	// double-paced against the game's own wait (phase beat -> 25 fps), and
	// the need=1 variant returned INSTANTLY whenever a retrace fired
	// mid-frame (free-ran to 48 fps = 1.6x game speed, user-verified wrong).
	// Per-call anchor can do neither. PC_UNCAP=1 skips the wait entirely
	// (game's own wait still bounds it — fast free-run for testing).
	static int s_uncap = -1;
	if (s_uncap < 0) s_uncap = getenv("PC_UNCAP") ? 1 : 0;
	if (!s_uncap) {
		struct timespec ts = { 0, 1000000 };
		while (s_retrace_count == seen)
			nanosleep(&ts, nullptr);
	}
	if (s_perf) {
		double t3 = pc_now_ms();
		if (s_last_exit > 0) {
			s_acc[0] += t0 - s_last_exit;         // game logic + GX submission
			s_acc_cpu[0] += c0 - s_last_exit_cpu; // ...its real CPU work (no sleeps)
			s_acc[1] += t1 - t0;                  // flush: seg copy into renderer
			s_acc[2] += t2 - t1;                  // encode + present
			s_acc_cpu[1] += c2 - c1;              // ...minus any nextDrawable block
			s_acc[3] += t3 - t2;                  // retrace wait (idle)
			if (++s_nacc == 120) {
				double tot = (s_acc[0] + s_acc[1] + s_acc[2] + s_acc[3]) / 120.0;
				fprintf(stderr,
				        "[PERF] frame %.1fms (%.1f fps): game %.1f (cpu %.1f) | flush %.1f | render %.1f (cpu %.1f) | wait %.1f\n",
				        tot, 1000.0 / tot, s_acc[0] / 120.0, s_acc_cpu[0] / 120.0, s_acc[1] / 120.0,
				        s_acc[2] / 120.0, s_acc_cpu[1] / 120.0, s_acc[3] / 120.0);
				s_nacc = 0;
				s_acc[0] = s_acc[1] = s_acc[2] = s_acc[3] = 0;
				s_acc_cpu[0] = s_acc_cpu[1] = 0;
			}
		}
		s_last_exit = t3;
		s_last_exit_cpu = pc_thread_cpu_ms();
		return;
	}
	// Heartbeat: prove the main loop is alive and measure GX submission.
	extern unsigned long pc_gx_frame_begins, pc_gx_frame_verts, pc_gx_frame_draws, pc_gx_frame_tris_drawn;
	extern unsigned long pc_gx_dl_indx_ops, pc_gx_posmtx_loads;
	extern int pc_gx_posmtx_maxslot, pc_gx_vtxidx_maxslot;
	extern unsigned long pc_gx_tex_nomagic_binds, pc_gx_tex_ok_binds;
	extern float pc_gx_dl_st_min[2], pc_gx_dl_st_max[2];
	extern int pc_gx_dl_st_texmtx_applied;
	// Every 30 retraces (~0.5s): fast enough to catch a scrolling texmtx
	// during the ~3s warp burst — static ranges across prints = the
	// animated texture matrix is NOT reaching the shim's texgen.
	if (pc_verbose() && (s_retrace_count % 30) == 0 && pc_gx_dl_st_max[0] > -1e29f)
		fprintf(stderr, "[VI] dl_st s=[%g..%g] t=[%g..%g] texmtx_applied=%d\n", pc_gx_dl_st_min[0], pc_gx_dl_st_max[0],
		        pc_gx_dl_st_min[1], pc_gx_dl_st_max[1], pc_gx_dl_st_texmtx_applied);
	if (pc_verbose() && (s_retrace_count % 60) == 0)
		fprintf(stderr,
		        "[VI] retrace=%u gx_begins=%lu gx_verts=%lu gx_draws=%lu tris/frame=%lu indx_ops=%lu posmtx=%lu(max %d) vtxidx(max %d) texbind=%lu/%lu-bad\n",
		        s_retrace_count, pc_gx_frame_begins, pc_gx_frame_verts, pc_gx_frame_draws, pc_gx_frame_tris_drawn,
		        pc_gx_dl_indx_ops, pc_gx_posmtx_loads, pc_gx_posmtx_maxslot, pc_gx_vtxidx_maxslot, pc_gx_tex_ok_binds,
		        pc_gx_tex_nomagic_binds);
}

// -------------------------------------------------------------------- PAD
BOOL PADInit() { return TRUE; }
void pc_window_get_pad(unsigned short*, signed char*, signed char*, signed char*, signed char*, unsigned char*,
                       unsigned char*); // pc_window.mm
extern "C" int pc_input_tick(unsigned short*, signed char*, signed char*, signed char*, signed char*, unsigned char*,
                             unsigned char*); // pc_input_rec.cpp
u32 PADRead(PADStatus* status) {
	for (int i = 0; i < 4; i++) {
		memset(&status[i], 0, sizeof(PADStatus));
		status[i].err = (i == 0) ? 0 /* PAD_ERR_NONE */ : -1 /* PAD_ERR_NO_CONTROLLER */;
	}
	unsigned short b;
	signed char sx, sy, cx, cy;
	unsigned char tl, tr;
	pc_window_get_pad(&b, &sx, &sy, &cx, &cy, &tl, &tr);
	// PC_AUTO_START=1: press Start at ~5s, A at ~8s (headless probe to
	// drive past the title without a human). Then answer the no-card
	// dialog with Yes (Up+A) and keep tapping A to travel through map
	// select into the day-1 in-game cinematic.
	// PC_AUTO_TITLE=1: only the two Starts (reach title, then idle so the
	// attract-mode cinematic plays by itself).
	if (getenv("PC_AUTO_START") || getenv("PC_AUTO_TITLE")) {
		u32 rc = VIGetRetraceCount();
		if ((rc > 300 && rc < 320) || (rc > 480 && rc < 500)) b |= 0x1000;
		if (!getenv("PC_AUTO_TITLE")) {
			// Up is ONLY correct while the no-card Yes/No dialog is up (Yes
			// sits above No); at the title menu it selects Options and an A
			// press parks the run there. So: Up+A twice in the dialog's
			// window, then plain-A taps to advance the remaining menus into
			// the game, all ending by rc 2100 (past that the presses dive
			// into in-game menus and silently invalidate observations).
			if (rc > 840 && rc < 860) b |= 0x1000;
			if (getenv("PC_SKIP_INTRO")) {
				// Reaching GAMEPLAY: the original aggressive cycle (Up+A
				// every 150 through rc 3600 + Start bursts) is what reliably
				// lands in-world — menu detours don't matter when the goal
				// is day-1. Only harmful when OBSERVING title/cutscene runs,
				// which don't use PC_SKIP_INTRO.
				if (rc > 560 && rc < 3600) {
					u32 ph = rc % 150;
					if (ph < 12) { b |= 0x0008; sy = 90; }
					if (ph >= 40 && ph < 52) b |= 0x0100;
				}
				if (rc > 1100 && rc < 3600 && (rc % 300) < 12) b |= 0x1000;
			} else {
				// Observation runs: Up only inside the no-card dialog's
				// window (Yes above No); title-menu Up would park in
				// Options. Then plain-A to advance; all quiet by rc 2100.
				if ((rc > 600 && rc < 612) || (rc > 750 && rc < 762)) { b |= 0x0008; sy = 90; }
				if ((rc > 640 && rc < 652) || (rc > 790 && rc < 802)) b |= 0x0100;
				if (rc > 900 && rc < 2100 && (rc % 150) < 12) b |= 0x0100;
			}
		}
	}
	// PC_WALK: drive a slow circling stick input once in-game, so the camera
	// pans the world — reveals whether the black ground has (scrolling)
	// structure or is genuinely not rendering.
	if (getenv("PC_WALK")) {
		u32 rc = VIGetRetraceCount();
		if (rc > 1200) {
			int ph = (rc / 60) % 4; // change heading every ~1s
			sx = (ph == 0) ? 90 : (ph == 2) ? -90 : 0;
			sy = (ph == 1) ? 90 : (ph == 3) ? -90 : 0;
		}
	}
	// PC_INPUT_REC / PC_INPUT_PLAY (pc_input_rec.cpp). Placed HERE, after the
	// scripted PC_AUTO_*/PC_WALK synthesis and before the state reaches the
	// game, so a recording captures the effective input and a replay overrides
	// everything — including the scripted input, which is keyed to a
	// wall-clock retrace thread and is exactly what replay exists to replace.
	// This is also the tick source: PADRead is called once per game update.
	pc_input_tick(&b, &sx, &sy, &cx, &cy, &tl, &tr);
	status[0].button = b;
	status[0].stickX = sx;
	status[0].stickY = sy;
	status[0].substickX = cx;
	status[0].substickY = cy;
	status[0].triggerLeft = tl;
	status[0].triggerRight = tr;
	return 0;
}
void PADClamp(PADStatus*) { }
void PADControlMotor(int, u32) { }
BOOL PADRecalibrate(u32) { return TRUE; }
BOOL PADReset(u32) { return TRUE; }
void PADSetSpec(u32) { }

// -------------------------------------------------------------------- DVD
// Loose-file DVD root: PC_GAME_ROOT env var (default "gamedata"), holding
// the extracted disc filesystem. ISO image support can come later.
static std::string pc_dvd_root() {
	// M7: resolved against the EXECUTABLE, not the cwd — see pc_paths.h.
	// PC_GAME_ROOT still wins, so dev workflows are unchanged.
	return pc_path_data();
}
#define PC_MAX_ENTRYNUMS 4096
#define PC_MAX_OPEN_FILES 64
static char s_entrynum_paths[PC_MAX_ENTRYNUMS][192];
static int s_nentrynums;
static DVDFileInfo* s_open_keys[PC_MAX_OPEN_FILES];
static FILE* s_open_pool[PC_MAX_OPEN_FILES];

static std::string pc_dvd_host_path(const char* path) {
	std::string p = path;
	if (!p.empty() && p[0] == '/') p.erase(0, 1);
	return pc_dvd_root() + "/" + p;
}

void DVDInit() { }

s32 DVDConvertPathToEntrynum(const char* path) {
	std::string host = pc_dvd_host_path(path);
	struct stat st;
	if (stat(host.c_str(), &st) != 0) return -1;
	for (int i = 0; i < s_nentrynums; i++)
		if (strcmp(s_entrynum_paths[i], host.c_str()) == 0) return (s32)i;
	if (s_nentrynums >= PC_MAX_ENTRYNUMS || host.size() >= 191) return -1;
	strcpy(s_entrynum_paths[s_nentrynums], host.c_str());
	return (s32)s_nentrynums++;
}

static BOOL pc_dvd_open_host(const std::string& host, DVDFileInfo* fileInfo) {
	FILE* f = fopen(host.c_str(), "rb");
	if (!f) return FALSE;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	int slot = -1;
	for (int i = 0; i < PC_MAX_OPEN_FILES; i++)
		if (s_open_keys[i] == fileInfo) { slot = i; break; }
	if (slot < 0)
		for (int i = 0; i < PC_MAX_OPEN_FILES; i++)
			if (!s_open_keys[i]) { slot = i; break; }
	if (slot < 0) { fclose(f); return FALSE; }
	if (s_open_keys[slot] == fileInfo && s_open_pool[slot]) fclose(s_open_pool[slot]);
	s_open_keys[slot] = fileInfo;
	s_open_pool[slot] = f;
	fileInfo->length = (u32)len;
	fileInfo->startAddr = 0;
	if (pc_verbose()) fprintf(stderr, "[DVD] open %s (%ld bytes)\n", host.c_str(), len);
	return TRUE;
}

BOOL DVDOpen(const char* filename, DVDFileInfo* fileInfo) {
	if (!pc_dvd_open_host(pc_dvd_host_path(filename), fileInfo)) {
		if (pc_verbose()) fprintf(stderr, "[DVD] MISSING %s\n", pc_dvd_host_path(filename).c_str());
		return FALSE;
	}
	return TRUE;
}

BOOL DVDFastOpen(s32 entryNum, DVDFileInfo* fileInfo) {
	if (entryNum < 0 || entryNum >= s_nentrynums) return FALSE;
	return pc_dvd_open_host(s_entrynum_paths[entryNum], fileInfo);
}

extern "C" void pc_loading_tick(void); // system.cpp — main-thread loading screen
s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
	(void)prio;
	pc_loading_tick(); // draws the loading banner while the main thread loads
	FILE* f = nullptr;
	for (int i = 0; i < PC_MAX_OPEN_FILES; i++)
		if (s_open_keys[i] == fileInfo) { f = s_open_pool[i]; break; }
	if (!f) return -1;
	fseek(f, offset, SEEK_SET);
	size_t got = fread(addr, 1, (size_t)length, f);
	// GC DVD reads are 32-byte-granular: callers round the request up past
	// EOF and expect the full requested length back. Zero-fill the tail.
	if ((s32)got < length) memset((u8*)addr + got, 0, (size_t)(length - got));
	return got > 0 ? length : 0;
}

// On GC the completion callback arrives later, from the DVD interrupt, so it
// is never re-entrant with respect to the call that queued the read. This shim
// runs the read synchronously and invokes the callback inline, which is fine
// until a callback issues another read — then the two recurse without bound.
// syncstream.c does exactly that: a zero-length read makes __LoadFin call
// __DVDReadAsyncRetry, which reads again, completes with 0 again, and the
// stack overflows (observed blowing the main thread's guard page while
// starting the intro movie's audio).
//
// A depth cap converts the runaway into a loud, survivable failure. It does
// not make the callback truly asynchronous, but it stops one bad read from
// killing the process, and says so.
//
// THE PROPER FIX, sketched here so it does not have to be re-derived: turn the
// recursion into a trampoline.
//
//   depth == 0 -> do the read, run the callback, then drain a queue of any
//                 completions that callback enqueued, repeating until empty
//   depth  > 0 -> enqueue (fileInfo, addr, length, offset, callback, prio) and
//                 return TRUE immediately; the outermost frame will run it
//
// That is iterative rather than recursive, so the stack cannot grow, and it is
// also CLOSER to hardware than what we do now: on GC the completion arrives
// later from the DVD interrupt, so deferring a nested read is more faithful
// than running it inline. It is safe by contract — code that needs the data
// before the call returns must use DVDReadPrio, not the async form.
//
// Two things to get right when doing it: the READ must be deferred along with
// the callback (deferring only the callback reorders reads against
// completions), and the queue needs its own progress cap, since a callback
// that endlessly re-issues a read that never advances would grow the queue
// without bound instead of the stack.
//
// Deliberately not done inline with the M5 audio work: it changes I/O timing
// across the whole game, and the read that provoked it (the zero-length .stx
// retry loop) is fixed, so the cap never fires in practice now.
#define PC_DVD_MAX_CB_DEPTH 32
static int s_dvd_cb_depth;

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio) {
	s32 r = DVDReadPrio(fileInfo, addr, length, offset, prio);
	if (!callback) return TRUE;

	if (s_dvd_cb_depth >= PC_DVD_MAX_CB_DEPTH) {
		static bool reported;
		if (!reported) {
			reported = true;
			OSReport("[PC] DVD async callback recursion capped at %d (len %d, offset %d) - a completion callback is "
			         "re-issuing a read that never makes progress\n",
			         PC_DVD_MAX_CB_DEPTH, length, offset);
		}
		return TRUE;
	}
	s_dvd_cb_depth++;
	callback(r, fileInfo);
	s_dvd_cb_depth--;
	return TRUE;
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
	for (int i = 0; i < PC_MAX_OPEN_FILES; i++)
		if (s_open_keys[i] == fileInfo) {
			fclose(s_open_pool[i]);
			s_open_keys[i] = nullptr;
			s_open_pool[i] = nullptr;
			break;
		}
	return TRUE;
}

s32 DVDGetDriveStatus() { return 0; }
DVDDiskID* DVDGetCurrentDiskID() {
	static DVDDiskID id;
	memcpy(id.gameName, "GPIE", 4);
	memcpy(id.company, "01", 2);
	return &id;
}
BOOL DVDPrepareStreamAsync(DVDFileInfo*, u32, u32, DVDCallback) { return FALSE; }
BOOL DVDCancelStream(DVDCommandBlock*) { return TRUE; }

// ---------------------------------------------------------------- AR/ARQ
#define PC_ARAM_SIZE (16u << 20)
static u8* s_aram;
static u8* pc_aram() {
	if (!s_aram) s_aram = (u8*)calloc(1, PC_ARAM_SIZE);
	return s_aram;
}
u32 ARInit(u32*, u32) { pc_aram(); return 0x4000; }

// M5's DSP renderer resolves voice sample data by ARAM offset (the VPB stores
// u32 offsets, which is why that struct needed no LP64 work at all).
u8* pc_aram_base() { return pc_aram(); }
u32 pc_aram_size() { return PC_ARAM_SIZE; }
u32 ARGetBaseAddress() { return 0x4000; }
void ARQInit() { }

void ARQPostRequest(ARQRequest* task, uintptr_t owner, u32 type, u32 priority, uintptr_t source, uintptr_t dest,
                    u32 length, ARQCallback callback) {
	(void)priority;
	task->owner = owner;
	task->type = type;
	task->source = source;
	task->dest = dest;
	task->length = length;
	u8* aram = pc_aram();
	// A DMA that does not fit must be LOUD. Silently skipping it leaves the
	// destination holding whatever was there before — for a wave load that
	// means a voice later decodes unrelated ARAM as samples, which is audible
	// as noise and gives no clue where it came from.
	if (type == 0 /* ARQ_TYPE_MRAM_TO_ARAM */) {
		// PC_ARAM_LOG=1: every MRAM->ARAM transfer, so "was this ARAM range
		// ever actually written?" is answerable. Voices reading a range that
		// no transfer covered are decoding whatever was there before.
		static int aramLog = -1;
		if (aramLog < 0) aramLog = getenv("PC_ARAM_LOG") ? 1 : 0;
		if (aramLog) {
			OSReport("[ARAMW] dest=0x%08llx len=0x%08x end=0x%08llx\n",
			         (unsigned long long)dest, length, (unsigned long long)(dest + length));
		}
		if (dest + length <= PC_ARAM_SIZE) {
			memcpy(aram + dest, (const void*)source, length);
		} else {
			OSReport("[ARQ] DROPPED MRAM->ARAM: dest 0x%llx + 0x%x exceeds ARAM size 0x%x\n", (unsigned long long)dest, length,
			         (u32)PC_ARAM_SIZE);
		}
	} else {
		if (source + length <= PC_ARAM_SIZE) {
			memcpy((void*)dest, aram + source, length);
		} else {
			OSReport("[ARQ] DROPPED ARAM->MRAM: source 0x%llx + 0x%x exceeds ARAM size 0x%x\n", (unsigned long long)source,
			         length, (u32)PC_ARAM_SIZE);
		}
	}
	if (callback) callback((uintptr_t)task);
}

// ------------------------------------------------------------------- DSP
// Fake the ucode handshake so jaudio's boot succeeds; commands are consumed
// and dropped until M5's frame renderer replaces this seam.
vu16 __DSPRegs[32];

// One-mail state machine: the "DSP" starts with its init mail pending, and
// every mail sent to it produces one response mail. That satisfies (a) the
// __DSPCheckMXICBoot2 boot handshake, (b) DSPSendCommands' precondition
// that no unconsumed response is pending, and (c) DSPWaitFinish's wait.
static int s_dsp_mail_pending = 1;
u32 DSPCheckMailToDSP() { return 0; }
u32 DSPCheckMailFromDSP() { return s_dsp_mail_pending; }
// 0x8071FEED: passes the MXIC init check, and makes DSPWaitFinish return
// immediately (any mail other than the 0x88881357 sync value = frame done).
u32 DSPReadMailFromDSP() {
	s_dsp_mail_pending = 0;
	return 0x8071FEED;
}
void DSPSendMailToDSP(u32 mail) {
	(void)mail;
	s_dsp_mail_pending = 1;
}

// -------------------------------------------------------------------- AI
// Moved to pc_audio.cpp (M5 P1): the AI DMA "interrupt" that drives jaudio's
// frame loop is now the SDL backend's producer pump, paced by ring fill
// instead of the fixed timer this used to run. The old timer also assumed
// DAC_SIZE was a byte count and so ticked at twice the real frame rate;
// AIInitDMA is called with DAC_SIZE * 2 bytes = 560 stereo frames = 17.48 ms.

// ------------------------------------------------------------------ CARD
// The whole CARD surface now lives in pc_card.cpp (M4) — host-file backed,
// with src/card/*.c as the semantic reference. PC_NO_CARD=1 there restores
// the old NOCARD-everywhere behaviour.

// --------------------------------------------------------------- EXI/HIO
BOOL EXIProbe(s32) { return FALSE; }
BOOL EXISelect(s32, u32, u32) { return FALSE; }
BOOL EXIDeselect(s32) { return TRUE; }
BOOL EXIUnlock(s32) { return TRUE; }
BOOL EXIImmEx(s32, void*, s32, u32) { return FALSE; }
BOOL HIOInit(s32 chan, HIOCallback) { (void)chan; return FALSE; }
BOOL HIOEnumDevices(HIOEnumCallback) { return FALSE; }
BOOL HIOReadMailbox(u32*) { return FALSE; }
BOOL HIOWrite(u32, void*, s32) { return FALSE; }
BOOL HIOWriteMailbox(u32) { return FALSE; }

} // extern "C"
