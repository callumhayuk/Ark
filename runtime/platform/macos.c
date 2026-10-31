// macOS host layer (runtime/platform.h), Apple silicon. PLA recomp, Phase 1.
// Adapted from LAS Recomp's runtime/platform/macos.c (MIT, Peiikai) to Ark's platform.h: Ark's 64 GB guest window,
// plat_discard, per-thread CPU clocks, the sampling profiler's plat_thread_pc and reserved-stack thread starts.
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

static mach_timebase_info_data_t tb;
// Debug watchpoints (runtime/dispatch.c): no single-stepping here; the watch thread re-closes pages instead, as on
// Windows.
int plat_watch_singlestep = 0;
static char g_log_path[1024];

// ---- guest address space
// Everything readable/writable from the start: macOS pages are 16K but the guest maps at 4K granularity.
// Untouched pages cost nothing, so the 64 GB window (rt.h GUEST_HI) is only address space.
// What already sits at addr (for the log when the window can't be claimed).
static void report_occupant(uint64_t addr) {
    vm_address_t r = (vm_address_t)addr;
    vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = vm_region_recurse_64(mach_task_self(), &r, &size, &depth, (vm_region_recurse_info_t)&info, &count);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[plat]   nothing mapped at or above %llx (vm_region: %s)\n", (unsigned long long)addr,
                mach_error_string(kr));
        return;
    }
    fprintf(stderr, "[plat]   next mapping at or above %llx: %llx-%llx (%llu MB), prot %d/%d, user_tag %u, share_mode %d\n",
            (unsigned long long)addr, (unsigned long long)r, (unsigned long long)(r + size),
            (unsigned long long)(size >> 20), info.protection, info.max_protection, info.user_tag, info.share_mode);
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    const char *tries = getenv("ARK_REEXEC");  // (runtime/main.c re-executes up to 20 times when this fails)
    bool say = !tries || atoi(tries) >= 19;
    vm_address_t a = lo;
    kern_return_t kr = vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED);
    if (kr == KERN_SUCCESS && a == lo) return true;
    if (kr == KERN_SUCCESS) vm_deallocate(mach_task_self(), a, hi - lo);
    if (say)
        fprintf(stderr, "[plat] reserving %llx-%llx in one piece failed: %s; trying 1 GB pieces\n", (unsigned long long)lo,
                (unsigned long long)hi, mach_error_string(kr));
    // Claim it piece by piece: works where one request this large is refused, and names what is in the way if a
    // piece is already taken.
    const uint64_t step = 1ull << 30;
    for (uint64_t p = lo; p < hi; p += step) {
        uint64_t n = hi - p < step ? hi - p : step;
        a = (vm_address_t)p;
        kr = vm_allocate(mach_task_self(), &a, n, VM_FLAGS_FIXED);
        if (kr == KERN_SUCCESS && a == p) continue;
        if (say) {
            fprintf(stderr, "[plat] piece %llx-%llx: %s\n", (unsigned long long)p, (unsigned long long)(p + n),
                    mach_error_string(kr));
            report_occupant(p);
        }
        if (p > lo) vm_deallocate(mach_task_self(), (vm_address_t)lo, p - lo);
        return false;
    }
    fprintf(stderr, "[plat] reserved %llx-%llx in 1 GB pieces\n", (unsigned long long)lo, (unsigned long long)hi);
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    vm_address_t a = lo;
    if (vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS) return true;
    for (uint64_t p = lo, step = 1ull << 30; p < hi; p += step) {  // (as plat_reserve_guest: in pieces)
        a = (vm_address_t)p;
        if (vm_allocate(mach_task_self(), &a, hi - p < step ? hi - p : step, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) != KERN_SUCCESS)
            return false;
    }
    return true;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// Ark: reads of [addr, addr+size) see zeroes again. The guest discards at 4K granularity; whole 16K host pages are
// replaced with fresh zero pages (released at once), and the 4K pieces at either end are cleared by hand.
void plat_discard(uint64_t addr, uint64_t size) {
    uint64_t lo = (addr + 0xFFF) & ~0xFFFull, hi = (addr + size) & ~0xFFFull;
    if (hi <= lo) return;
    uint64_t plo = (lo + PLAT_PAGE - 1) & ~(PLAT_PAGE - 1), phi = hi & ~(PLAT_PAGE - 1);
    if (phi > plo) {
        vm_address_t a = plo;
        if (vm_allocate(mach_task_self(), &a, phi - plo, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) != KERN_SUCCESS)
            memset((void *)(uintptr_t)plo, 0, phi - plo);
        if (plo > lo) memset((void *)(uintptr_t)lo, 0, plo - lo);
        if (hi > phi) memset((void *)(uintptr_t)phi, 0, hi - phi);
    } else {
        memset((void *)(uintptr_t)lo, 0, hi - lo);
    }
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

void plat_sleep_until_ns(uint64_t t) {
    if (!tb.denom) mach_timebase_info(&tb);
    mach_wait_until(t * tb.denom / tb.numer);
}

// Real-time (time-constraint) thread: exempt from the timer coalescing macOS applies to ordinary threads when the
// app isn't frontmost, which otherwise stretches 16 ms waits to ~100 ms.
void plat_thread_realtime(uint64_t period_ns) {
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t period = period_ns * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)period,
        .computation = (uint32_t)(period / 20),
        .constraint = (uint32_t)(period / 4),
        .preemptible = 1,
    };
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                         (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) fprintf(stderr, "[plat] real-time thread policy refused: %d\n", (int)kr);
}

// The clock id is the thread's Mach port + 1 (0 = unknown).
intptr_t plat_thread_clock(void) { return (intptr_t)pthread_mach_thread_np(pthread_self()) + 1; }

uint64_t plat_thread_cpu_ns(intptr_t clock) {
    if (!clock) return 0;
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info((thread_act_t)(clock - 1), THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return ((uint64_t)info.user_time.seconds + (uint64_t)info.system_time.seconds) * 1000000000ull +
           ((uint64_t)info.user_time.microseconds + (uint64_t)info.system_time.microseconds) * 1000ull;
}

// Sampling profiler: stop the thread for a moment and read its pc (no signals needed on macOS).
bool plat_thread_pc(intptr_t clock, PlatThread host, uint64_t *pc) {
    (void)host;
    if (!clock) return false;
    thread_act_t t = (thread_act_t)(clock - 1);
    if (thread_suspend(t) != KERN_SUCCESS) return false;
    arm_thread_state64_t st;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &count);
    thread_resume(t);
    if (kr != KERN_SUCCESS) return false;
    *pc = (uint64_t)arm_thread_state64_get_pc(st);
    return true;
}

// pthread stacks on macOS are mmap'd: the reserve is address space, backed as it grows.
bool plat_thread_start(PlatThread *out, void *(*fn)(void *), void *arg, size_t reserve) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (reserve) pthread_attr_setstacksize(&attr, (reserve + PLAT_PAGE - 1) & ~(size_t)(PLAT_PAGE - 1));
    pthread_t t;
    int r = pthread_create(&t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (r) return false;
    *out = (PlatThread)t;
    return true;
}

void plat_thread_join(PlatThread t) { pthread_join((pthread_t)t, NULL); }

// ---- process
// Only when asked (ARK_LOG_DIR=<dir>), as on Linux: the log goes to <dir>/ark.log.
static void log_to_file_if_asked(void) {
    const char *dir = getenv("ARK_LOG_DIR");
    if (!dir || !*dir || isatty(STDERR_FILENO)) return;
    mkdir(dir, 0755);
    snprintf(g_log_path, sizeof g_log_path, "%s/ark.log", dir);
    if (!freopen(g_log_path, "w", stderr)) { g_log_path[0] = 0; return; }
    dup2(STDERR_FILENO, STDOUT_FILENO);
}

// The game opens many RomFS-backed files and shader caches at once: macOS' default soft limit is 256.
static void raise_open_file_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rlim_t want = rl.rlim_max < 10240 ? rl.rlim_max : 10240;
    if (rl.rlim_cur < want) {
        rl.rlim_cur = want;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

void plat_init(void) {
    log_to_file_if_asked();
    raise_open_file_limit();
}

bool plat_exe_path(char *buf, size_t size) {
    uint32_t len = (uint32_t)size;
    return _NSGetExecutablePath(buf, &len) == 0;
}

extern char ***_NSGetArgv(void);
void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    if (plat_exe_path(path, sizeof path)) execv(path, *_NSGetArgv());
}

// End the process at once, skipping library teardown (the GPU driver's threads may still be busy).
void plat_terminate(void) { _exit(0); }

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error: the details are in the log (or the terminal). An alert window comes with the app bundle (Phase 5).
void plat_fatal_notice(void) {
    if (g_log_path[0]) fflush(stderr);
}

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++) fprintf(stderr, "  %s\n", syms ? syms[i] : "?");
    free(syms);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr;
    if ((sig == SIGSEGV || sig == SIGBUS) && rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);  // (macOS reports most bad guest accesses as SIGBUS)
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path) == 0 || errno == ENOENT ? 0 : -1;
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }
