// Guest address -> recompiled function lookup, and host (HLE) function registry.
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hle.h"

typedef struct { const uint64_t *addrs; const GuestFn *fns; uint64_t count, lo, hi; } FnTable;
static FnTable tables[4];
#define NTABLES ((int)(sizeof tables / sizeof tables[0]))

// HLE stubs live at fake addresses HLE_BASE + 16*i
#define MAX_HLE 4096
typedef struct { const char *name; HostFn fn; } HleSlot;
static HleSlot hle_slots[MAX_HLE];
static int hle_count;

// Overrides of recompiled guest functions (by address)
#define MAX_OVERRIDE 1024
static struct { uint64_t addr; HostFn fn; const char *name; } overrides[MAX_OVERRIDE];
static int override_count;

// Host implementations registered by name (runtime/hle/*.c fill this via HLE_FUNC)
typedef struct { const char *name; HostFn fn; } HleImpl;
#ifdef __APPLE__
extern const HleImpl __start_hle_impls[] __asm__("section$start$__DATA$__hle_impls");
extern const HleImpl __stop_hle_impls[] __asm__("section$end$__DATA$__hle_impls");
#elif defined(__linux__)
extern const HleImpl __start_hle_impls[], __stop_hle_impls[];  // (ELF: defined by the linker)
#else
// PE/COFF: the linker sorts .hle$a < .hle$m (entries, hle.h) < .hle$z by the text after '$'.
__attribute__((section(".hle$a"), used)) static const HleImpl __start_hle_impls[1];
__attribute__((section(".hle$z"), used)) static const HleImpl __stop_hle_impls[1];
#endif

HostFn hle_lookup_impl(const char *name) {
    const char *skip = getenv("LARECOMP_HLE_SKIP");  // debugging: disable HLE for names containing this
    if (skip && strstr(name, skip)) return NULL;
    const HleImpl *start = __start_hle_impls, *stop = __stop_hle_impls;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const HleImpl *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

static void hle_unimplemented(Cpu *c);

uint64_t hle_register(const char *name, HostFn fn) {
    for (int i = 0; i < hle_count; i++)
        if (!strcmp(hle_slots[i].name, name)) return HLE_BASE + 16ull * i;
    if (hle_count == MAX_HLE) FATAL("too many HLE stubs\n");
    hle_slots[hle_count].name = strdup(name);
    hle_slots[hle_count].fn = fn;
    return HLE_BASE + 16ull * hle_count++;
}

void hle_override(uint64_t addr, HostFn fn, const char *name) {
    if (override_count == MAX_OVERRIDE) FATAL("too many overrides\n");
    overrides[override_count].addr = addr;
    overrides[override_count].fn = fn;
    overrides[override_count].name = name;
    override_count++;
}

static __thread uint64_t unimpl_pc;
static int unimpl_counts[MAX_HLE];
static void hle_unimplemented(Cpu *c) {
    int i = (int)((unimpl_pc - HLE_BASE) / 16);
    if (unimpl_counts[i]++ < 3)
        LOG("[hle] UNIMPLEMENTED %s (x0=%llx x1=%llx lr=%llx) -> returning 0\n", hle_slots[i].name, c->x[0],
            c->x[1], c->x[30]);
    c->x[0] = 0;
}

uint64_t guest_sym(const char *name) {
    // small cache: callers pass string literals, so pointer identity is a good key
    static struct { const char *name; uint64_t addr; } cache[64];
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64 && cache[i].name; i++)
        if (cache[i].name == name) { uint64_t a = cache[i].addr; pthread_mutex_unlock(&m); return a; }
    pthread_mutex_unlock(&m);
    uint64_t a = loader_find_symbol(name);
    if (!a) FATAL("guest symbol %s not found\n", name);
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64; i++) if (!cache[i].name) { cache[i].name = name; cache[i].addr = a; break; }
    pthread_mutex_unlock(&m);
    return a;
}

static uint64_t hle_next = HLE_HEAP;
RT_ON_RESET(hle_heap_reset) { hle_next = HLE_HEAP; }  // (guest memory, wiped by the reboot)

void *hle_alloc(uint64_t size) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    uint64_t a = hle_next;
    hle_next += (size + 63) & ~63ull;
    if (hle_next > HLE_HEAP + HLE_HEAP_SIZE) FATAL("HLE heap exhausted\n");
    pthread_mutex_unlock(&m);
    plat_commit(a, size);
    memset((void *)a, 0, size);
    return (void *)a;
}

// Ark: the translated modules are linked into the executable (milestone 2); each module's generated
// <name>_table.c provides sorted guest addresses and the functions at them.
#define MODULE_TABLE(n)                                                                    \
    extern const uint64_t n##_fn_addrs[];                                                  \
    extern const GuestFn n##_fn_ptrs[];                                                    \
    extern const uint64_t n##_fn_count;
MODULE_TABLE(main)
MODULE_TABLE(multimedia)
MODULE_TABLE(glslc)
MODULE_TABLE(sdk)

static FnTable make_table(const uint64_t *a, const GuestFn *f, uint64_t n) {
    return (FnTable){a, f, n, a[0], a[n - 1]};
}

void dispatch_init(void) {
    tables[0] = make_table(main_fn_addrs, main_fn_ptrs, main_fn_count);
    tables[1] = make_table(multimedia_fn_addrs, multimedia_fn_ptrs, multimedia_fn_count);
    tables[2] = make_table(glslc_fn_addrs, glslc_fn_ptrs, glslc_fn_count);
    tables[3] = make_table(sdk_fn_addrs, sdk_fn_ptrs, sdk_fn_count);
}

GuestFn dispatch_lookup(uint64_t addr) {
    for (int t = 0; t < NTABLES; t++) {
        FnTable *ft = &tables[t];
        if (addr < ft->lo || addr > ft->hi) continue;
        uint64_t lo = 0, hi = ft->count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (ft->addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < ft->count && ft->addrs[lo] == addr) return ft->fns[lo];
    }
    return NULL;
}

// Small direct-mapped cache in front of the binary search
#define DCACHE 4096
static __thread struct { uint64_t addr; GuestFn fn; } dcache[DCACHE];

static GuestFn dispatch_slow(Cpu *c) {
    uint64_t pc = c->pc;
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        rt_unwind_point();  // (HLE calls aren't cached below, so every one passes here: a reboot's safe point)
        int i = (int)((pc - HLE_BASE) / 16);
        if (i < hle_count) {
            if (hle_slots[i].fn) return hle_slots[i].fn;
            unimpl_pc = pc;
            return hle_unimplemented;
        }
    }
    for (int i = 0; i < override_count; i++)
        if (overrides[i].addr == pc) return overrides[i].fn;
    GuestFn f = dispatch_lookup(pc);
    if (!f) {
        uint64_t off;
        const char *s = loader_symbol_at(c->x[30], &off);
        FATAL("no function at %llx (lr=%llx %s+%llx)\n", pc, c->x[30], s ? s : "?", off);
    }
    unsigned h = (unsigned)(pc >> 2) & (DCACHE - 1);
    dcache[h].addr = pc;
    dcache[h].fn = f;
    return f;
}

// Always tail-calls, so chains of indirect jumps don't grow the host stack.
void rt_dispatch(Cpu *c) {
    unsigned h = (unsigned)(c->pc >> 2) & (DCACHE - 1);
    GuestFn f = dcache[h].addr == c->pc ? dcache[h].fn : dispatch_slow(c);
    __attribute__((musttail)) return f(c);
}

void guest_call(Cpu *c, uint64_t addr) {
    uint64_t lr = c->x[30];
    c->x[30] = 0;
    c->pc = addr;
    rt_dispatch(c);
    c->x[30] = lr;
}

// Host pc -> recompiled guest function containing it ("f_<addr>"), for hosts without symbol lookup.
static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    static uint64_t (*byhost)[2];  // {host address, guest address}, sorted by host address
    static uint64_t n;
    static __thread char name[32];
    if (!byhost) {
        uint64_t total = 0, k = 0;
        for (int t = 0; t < NTABLES; t++) total += tables[t].count;
        uint64_t (*v)[2] = malloc(total * sizeof *v);
        if (!v) return NULL;
        for (int t = 0; t < NTABLES; t++)
            for (uint64_t i = 0; i < tables[t].count; i++) {
                v[k][0] = (uint64_t)(uintptr_t)tables[t].fns[i];
                v[k++][1] = tables[t].addrs[i];
            }
        qsort(v, k, sizeof *v, cmp_host);
        n = k;
        byhost = v;
    }
    uint64_t lo = 0, hi = n;  // last entry with host address <= pc
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (byhost[mid][0] <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - byhost[lo - 1][0] > 0x100000) return NULL;
    snprintf(name, sizeof name, "f_%llx", (unsigned long long)byhost[lo - 1][1]);
    if (offset) *offset = pc - byhost[lo - 1][0];
    return name;
}

static const char *host_sym(uint64_t pc, uint64_t *offset) {
    const char *s = plat_symbol(pc, offset);
    return s ? s : rt_host_fn_name(pc, offset);
}

void rt_fatal_trace(void) {
    fprintf(stderr, "host backtrace (guest functions are f_<addr>):\n");
    plat_print_backtrace();
    Cpu *c = kernel_current_cpu();
    if (c) {
        fprintf(stderr, "guest: sp=%llx lr=%llx\n", c->sp, c->x[30]);
        for (int i = 0; i < 31; i += 4)
            fprintf(stderr, "  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i, c->x[i], i + 1,
                    c->x[i + 1], i + 2, c->x[i + 2], i + 3, c->x[i + 3]);
    }
    plat_fatal_notice();
    // our crash handler also catches SIGABRT: without this abort() re-entered it forever (a 5 GB log)
    signal(SIGABRT, SIG_DFL);
    abort();
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    FATAL("undefined instruction / trap at %llx\n", pc);
}

void rt_jt_miss(Cpu *c, uint64_t br_pc, uint64_t target) {
    (void)c;
    FATAL("jump table at %llx: target %llx is not one of its cases (re-generate with ARK_TABLE_FALLBACK=1 and "
          "report the address)\n", (unsigned long long)br_pc, (unsigned long long)target);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    FATAL("brk #%u at %llx\n", imm, pc);
}

// ---- crash reporting

// Debug data watchpoints: the pages are protected; accesses inside a watched range are attributed to the host
// function (f_<guest addr>) doing them, then the page is reopened until the watch thread closes it again.
#define MAX_WATCH 32
static struct { uint64_t lo, hi; } watches[MAX_WATCH];
static uint64_t watch_pages[MAX_WATCH];
static int nwatch, nwpage;
static volatile int watch_on, watch_done;
long watch_armed_frame = -1;  // presented frame when armed (timescan.c turns it off ARK_WATCH_FRAMES later)
static void *watch_thread(void *a) {
    (void)a;
    extern int plat_watch_singlestep;  // the platform closes the page again after each access
    for (int i = 0; i < nwpage && plat_watch_singlestep; i++) plat_protect(watch_pages[i], PLAT_PAGE, PLAT_PROT_NONE);
    while (watch_on && plat_watch_singlestep) usleep(1000);
    while (watch_on) {
        for (int i = 0; i < nwpage; i++) {
            int r = plat_protect(watch_pages[i], PLAT_PAGE, PLAT_PROT_NONE);
            static int nerr;
            if (r && nerr++ < 4) fprintf(stderr, "[watch] protect %llx failed\n", (unsigned long long)watch_pages[i]);
        }
        usleep(100);
    }
    for (int i = 0; i < nwpage; i++) plat_protect(watch_pages[i], PLAT_PAGE, PLAT_PROT_RW);
    return NULL;
}
void watch_add(uint64_t lo, uint64_t hi) {
    if (watch_on || nwatch == MAX_WATCH) return;
    watches[nwatch].lo = lo, watches[nwatch].hi = hi, nwatch++;
    uint64_t pg = lo & ~(uint64_t)(PLAT_PAGE - 1);
    for (int i = 0; i < nwpage; i++) if (watch_pages[i] == pg) return;
    watch_pages[nwpage++] = pg;
}
void watch_arm(void) {
    if (!nwpage || watch_on || watch_done) return;
    watch_on = 1;
    extern uint64_t renderer_present_count(void);
    watch_armed_frame = (long)renderer_present_count();
    fprintf(stderr, "[watch] %d ranges on %d pages\n", nwatch, nwpage);
    for (int i = 0; i < nwatch; i++) fprintf(stderr, "[watch]  %llx..%llx\n", (unsigned long long)watches[i].lo, (unsigned long long)watches[i].hi);
    pthread_t t;
    pthread_create(&t, NULL, watch_thread, NULL);
}
bool rt_watch_active(void) { return watch_on; }
void watch_disarm(void) { watch_on = 0, watch_done = 1; }

// After the single step (platform trap handler), rt_watch_after() compares the range: a change is logged as a write.
static __thread int pend_w = -1;
static __thread uint32_t pend_old;
static __thread uint64_t pend_pc;
void rt_watch_after(void) {
    if (pend_w < 0) return;
    int w = pend_w;
    pend_w = -1;
    uint32_t now;
    plat_protect(watches[w].lo & ~(uint64_t)(PLAT_PAGE - 1), PLAT_PAGE, PLAT_PROT_RW);  // (another thread may have closed it)
    memcpy(&now, (const void *)(uintptr_t)watches[w].lo, 4);
    if (now == pend_old) return;
    static struct { int w; uint64_t pc; } seen[512];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i].w == w && seen[i].pc == pend_pc) return;
    if (nseen < 512) seen[nseen].w = w, seen[nseen++].pc = pend_pc;
    uint64_t off = 0;
    const char *nm = host_sym(pend_pc, &off);
    float a, b;
    memcpy(&a, &pend_old, 4), memcpy(&b, &now, 4);
    fprintf(stderr, "[watch] %llx WRITE by %s+%#llx: %g -> %g\n", (unsigned long long)watches[w].lo, nm ? nm : "?",
            (unsigned long long)off, a, b);
}

bool rt_fault_watch(uint64_t fa, uint64_t pc) {
    uint64_t pg = fa & ~(uint64_t)(PLAT_PAGE - 1);
    int p = 0;
    while (p < nwpage && watch_pages[p] != pg) p++;
    if (p == nwpage) return false;
    plat_protect(pg, PLAT_PAGE, PLAT_PROT_RW);
    // Ark: only guest threads may run into a watch. A host thread (the renderer reading a watched constant buffer, the
    // GPU's memory imports) faulting again and again on pages the watch thread keeps closing deadlocks in this handler
    // (dladdr / stdio in a signal handler); the game hung in the rain, 2026-11-03. Its first touch turns the watch off.
    extern struct KThread *kernel_current_thread(void);
    if (!kernel_current_thread()) {
        watch_on = 0, watch_done = 1;
        for (int i = 0; i < nwpage; i++) plat_protect(watch_pages[i], PLAT_PAGE, PLAT_PROT_RW);
        static const char msg[] = "[watch] a host thread touched a watched page: watch off\n";
        write(2, msg, sizeof msg - 1);  // (async-signal-safe)
        pend_w = -1;
        return true;
    }
    pend_w = -1;
    for (int w = 0; w < nwatch; w++) {
        if (fa < watches[w].lo || fa >= watches[w].hi) continue;
        pend_w = w, pend_pc = pc;
        memcpy(&pend_old, (const void *)(uintptr_t)watches[w].lo, 4);
        static struct { int w; char nm[48]; } seen[512];
        static int nseen;
        const char *nm = host_sym(pc, NULL);
        if (!nm) nm = "?";
        bool found = false;
        for (int i = 0; i < nseen && !found; i++) found = seen[i].w == w && !strcmp(seen[i].nm, nm);
        if (!found && nseen < 512) {
            seen[nseen].w = w;
            snprintf(seen[nseen++].nm, 48, "%s", nm);
            fprintf(stderr, "[watch] %llx+%#llx by %s\n", (unsigned long long)watches[w].lo,
                    (unsigned long long)(fa - watches[w].lo), nm);
        }
    }
    return true;
}

void rt_fault_report(const char *what, uint64_t fa, uint64_t pc, uint64_t lr) {
    uint64_t po = 0, lo = 0;
    const char *ps = host_sym(pc, &po), *ls = host_sym(lr, &lo);
    fprintf(stderr, "\n*** %s accessing %#llx\n    host pc %llx (%s+%llx)\n    host lr %llx (%s)\n", what,
            (unsigned long long)fa, (unsigned long long)pc, ps ? ps : "?", (unsigned long long)po,
            (unsigned long long)lr, ls ? ls : "?");
    rt_fatal_trace();
}

void crash_handler_install(void) { plat_crash_handler_install(); }
