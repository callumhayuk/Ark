// Linux host layer (runtime/platform.h). x86-64 and aarch64.
#define _GNU_SOURCE
#ifndef ARK_HEADLESS
#include <SDL3/SDL.h>
#endif
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
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

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- guest address space
// Everything readable/writable from the start, like macOS: the guest maps at 4K granularity, and untouched pages
// cost nothing (MAP_NORESERVE: the 12 GB window doesn't count against overcommit).
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }  // (kernels before 4.17: just a hint)
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

void plat_discard(uint64_t addr, uint64_t size) {
    uint64_t lo = (addr + 0xFFF) & ~0xFFFull, hi = (addr + size) & ~0xFFFull;
    if (hi > lo) madvise((void *)(uintptr_t)lo, hi - lo, MADV_DONTNEED);  // private anonymous: zero on next touch
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

intptr_t plat_thread_clock(void) {
    clockid_t c;
    return pthread_getcpuclockid(pthread_self(), &c) == 0 ? (intptr_t)c + 1 : 0;
}

uint64_t plat_thread_cpu_ns(intptr_t clock) {
    struct timespec ts;
    if (!clock || clock_gettime((clockid_t)(clock - 1), &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static volatile uint64_t sample_pc;
static volatile int sample_done;
static void sample_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
#if defined(__aarch64__)
    sample_pc = (uint64_t)((ucontext_t *)uc)->uc_mcontext.pc;  // PLA recomp: arm64 hosts
#else
    sample_pc = (uint64_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#endif
    __atomic_store_n(&sample_done, 1, __ATOMIC_RELEASE);
}

bool plat_thread_start(PlatThread *out, void *(*fn)(void *), void *arg, size_t reserve) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, reserve);  // (mmap'd: reserved, pages backed on first touch)
    pthread_t t;
    int r = pthread_create(&t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (r) return false;
    *out = (PlatThread)t;
    return true;
}

void plat_thread_join(PlatThread t) { pthread_join((pthread_t)t, NULL); }

bool plat_thread_pc(intptr_t clock, PlatThread host, uint64_t *pc) {
    (void)clock;
    static bool installed;
    if (!installed) {
        installed = true;
        struct sigaction sa = {0};
        sa.sa_sigaction = sample_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, NULL);
    }
    __atomic_store_n(&sample_done, 0, __ATOMIC_RELEASE);
    if (pthread_kill((pthread_t)host, SIGPROF)) return false;
    for (int i = 0; i < 2000 && !__atomic_load_n(&sample_done, __ATOMIC_ACQUIRE); i++) {
        struct timespec ts = {0, 2000};
        nanosleep(&ts, NULL);
    }
    if (!__atomic_load_n(&sample_done, __ATOMIC_ACQUIRE)) return false;
    *pc = sample_pc;
    return true;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Real-time scheduling where the user may have it (rtkit / limits.conf), else just a higher priority: keeps the
// vsync thread on time while the guest threads load every core.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);  // (per thread on Linux; fails quietly without CAP_SYS_NICE)
}

// ---- process
// Started from a desktop (the AppImage) there is no terminal: stdout / stderr go to lasrecomp.log in the data folder
// (runtime/paths.h; rt_paths_init runs later, so the same rule is applied here). Processes started by this one (the
// start screen re-executing itself for the game) append to the same log.
static char g_log_path[1024];

static void log_to_file_without_terminal(void) {
    if (isatty(STDERR_FILENO)) return;
    if (!getenv("ARK_LOG_DIR")) return;  // Ark: only redirect when asked (ARK_LOG_DIR=<dir>)
    char dir[900];
    const char *env = getenv("ARK_LOG_DIR"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    struct stat st;
    if (env && *env) snprintf(dir, sizeof dir, "%s", env);
    else if (stat("extracted", &st) == 0 && S_ISDIR(st.st_mode)) return;  // dev mode: wherever stderr goes already
    else if (xdg && *xdg == '/') snprintf(dir, sizeof dir, "%s/lasrecomp", xdg);
    else snprintf(dir, sizeof dir, "%s/.local/share/lasrecomp", home && *home ? home : "/tmp");
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    mkdir(dir, 0755);
    snprintf(g_log_path, sizeof g_log_path, "%s/ark.log", dir);
    bool append = getenv("LASRECOMP_LOG_APPEND") != NULL;
    if (!freopen(g_log_path, append ? "a" : "w", stderr)) { g_log_path[0] = 0; return; }
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("LASRECOMP_LOG_APPEND", "1", 1);
}

#if defined(__x86_64__)
// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    fprintf(stderr, "FATAL: LAS Recomp needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or "
                    "newer).\n");
    exit(1);
}
#else
static void require_cpu(void) {}
#endif

void plat_init(void) {
    log_to_file_without_terminal();
    require_cpu();
}

// (inside an AppImage: the binary in its mount, which stays mounted while this process lives - re-exec included)
bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

// Our own command line, from /proc/self/cmdline (NUL-separated).
static char **self_argv(void) {
    static char buf[32768];
    static char *argv[256];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) argv[k++] = p;
    argv[k] = NULL;
    return argv;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    char **av = self_argv();
    if (av && plat_exe_path(path, sizeof path)) execv(path, av);
}

// ---- diagnostics
// Exported symbols only (the Makefile links with -rdynamic); frames in the generated code are left to the caller,
// which maps them to guest functions (runtime/dispatch.c). Static runtime functions come out as the nearest guest
// function, so backtrace lines also give the offset in the executable (addr2line -fe lasrecomp <offset>).
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di)) return NULL;
    if (!di.dli_sname) {  // a library's unexported code: library+offset
        Dl_info self = {0};
        dladdr((void *)plat_symbol, &self);
        if (!di.dli_fname || di.dli_fbase == self.dli_fbase) return NULL;
        static __thread char name[256];
        const char *b = strrchr(di.dli_fname, '/');
        snprintf(name, sizeof name, "%s", b ? b + 1 : di.dli_fname);
        if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        return name;
    }
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error without a terminal: say so in a message box rather than vanishing (the details are in the log).
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1200];
    snprintf(text, sizeof text, "Ark stopped because of an error.\n\nDetails: %s", g_log_path);
#ifndef ARK_HEADLESS
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ark", text, NULL);
#else
    (void)text;
#endif
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (lasrecomp+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static __thread uint64_t watch_reprotect;
#if defined(__x86_64__)
int plat_watch_singlestep = 1;
#else
int plat_watch_singlestep = 0;
#endif
#if defined(__x86_64__)
static void watch_trap(int sig, siginfo_t *si, void *ctx) {
    (void)sig, (void)si;
    ucontext_t *uc = ctx;
    if (watch_reprotect) {
        extern bool rt_watch_active(void);
        extern void rt_watch_after(void);
        rt_watch_after();
        if (rt_watch_active()) mprotect((void *)(uintptr_t)watch_reprotect, PLAT_PAGE, PROT_NONE);
        watch_reprotect = 0;
    }
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100ll;
}
#endif

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
#else
    // x86-64 has no link register: report the return address on top of the stack (valid at function entry)
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP], lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
#endif
    if (sig == SIGSEGV && rt_fault_watch(fa, pc)) {
#if defined(__x86_64__)
        // debug watchpoint: let this one instruction through, then close the page again (watch_trap)
        watch_reprotect = fa & ~(PLAT_PAGE - 1);
        uc->uc_mcontext.gregs[REG_EFL] |= 0x100;
#endif
        return;
    }
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
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
#if defined(__x86_64__)
    sa.sa_sigaction = watch_trap;
    sigaction(SIGTRAP, &sa, NULL);
#endif
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
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
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
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// End the process at once, skipping the library teardown: as on Windows, the GPU driver's threads crashed in exit()
// while the game's threads still submitted work.
void plat_terminate(void) { _exit(0); }
