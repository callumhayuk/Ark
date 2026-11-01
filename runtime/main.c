// Ark entry (milestone 2: headless boot). Loads the four modules, sets up the kernel and runs nn::init::Start on
// the game's main thread. No window yet: the main host thread just waits, and every kernel call / service request /
// unimplemented host function is logged (stderr).
//
// Usage: ark <game dir>   where <game dir> holds exefs/ (main, sdk, subsdk0, subsdk1, main.npdm) and romfs.bin
//                         (hactool --romfs=romfs.bin on the v1.1.1 update; or runtime/romfs_build.c from a folder).
// Based on LAS Recomp's runtime/main.c (MIT).
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "hle.h"
#include "kernel.h"

const char *g_romfs_dir;

static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// rtld runs each module's DT_INIT (which registers it with the SDK's libc and runs its init_array).
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, (unsigned long long)m->init,
        (unsigned long long)(m->init_array_sz / 8));
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

static Module *module_named(const char *name) {
    for (int i = 0; i < g_nmodules; i++)
        if (!strcmp(g_modules[i].name, name)) return &g_modules[i];
    FATAL("module %s not loaded\n", name);
}

// nn::init::Start calls x2 right after nnosInitialize and x3 after nninitStartup, just before nnMain
// (LAS Recomp's finding). The SDK-side modules initialise first, the game last.
static void rtld_init_libraries(Cpu *c) {
    module_init(c, module_named("sdk"));
    module_init(c, module_named("multimedia"));
    module_init(c, module_named("glslc"));
}
static void rtld_init_main(Cpu *c) { module_init(c, module_named("main")); }
static void rtld_fini(Cpu *c) { (void)c; LOG("nnMain returned (game exit)\n"); }

// Claim the guest address range before anything else can map there.
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    bool low = plat_reserve_guest(GUEST_LO, GUEST_LOW_END);
    bool high = GUEST_HIGH_LO >= GUEST_HI || (low && plat_reserve_guest(GUEST_HIGH_LO, GUEST_HI));
    if (low && high) return;
    if (low) plat_unreserve_guest(GUEST_LO, GUEST_LOW_END);
    const char *tries = getenv("ARK_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        if (GUEST_HIGH_LO < GUEST_HI)
            fprintf(stderr, "FATAL: guest address ranges %llx-%llx and %llx-%llx are not free\n", GUEST_LO, GUEST_LOW_END,
                    GUEST_HIGH_LO, GUEST_HI);
        else
            fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("ARK_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

#ifndef ARK_HEADLESS
static void *run_limit_thread(void *arg) {
    sleep((unsigned)*(int *)arg);
    LOG("ARK_RUN_SECONDS reached, exiting\n");
    extern void vk_exit_now(void);
    vk_exit_now();  // waits for the GPU, keeps the pipeline cache, ends without library teardown
    return NULL;
}
#endif

#ifndef ARK_HEADLESS
// Hang watchdog: no frame presented for 20 s (after the first one) logs every guest thread's wait state once per
// stall, so a log from a frozen game says what it is waiting for. ARK_WATCHDOG=0 turns it off.
static void *watchdog_thread(void *arg) {
    (void)arg;
    extern uint64_t renderer_present_count(void);
    uint64_t last = 0;
    int still = 0;
    bool reported = false;
    for (;;) {
        sleep(1);
        uint64_t n = renderer_present_count();
        if (n != last) { last = n; still = 0; reported = false; continue; }
        if (++still >= (n ? 20 : 40) && !reported) {  // (before the first frame: 40 s, boot included)
            reported = true;
            LOG("[watchdog] no frame presented for %d s (frame %llu): guest threads:\n", still, (unsigned long long)n);
            kernel_dump_waits();
        }
    }
    return NULL;
}
#endif

// LAS's in-place reboot is not used by Ark yet; kernel.c still references these.
int rt_reboot_requested(void) { return 0; }
void rt_request_reboot(int kind) { (void)kind; }

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);  // fs.c opens "<this>.bin"
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    g_romfs_dir = strdup(romfs);

    loader_load(exefs);
    extern void game_patches_apply(void);
    game_patches_apply();
    kernel_init();
    dispatch_init();

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
            extern uint64_t g_system_resource_size;
            g_system_resource_size = *(uint32_t *)(meta + 0x14);
            if (getenv("ARK_NO_VAMM")) g_system_resource_size = 0;  // (test: the SDK allocator without virtual memory)
        }
        fclose(f);
    }
    extern uint64_t g_system_resource_size;
    LOG("main thread: prio %d, stack 0x%x, system resource 0x%llx\n", prio, stack_size,
        (unsigned long long)g_system_resource_size);

    // Legends: Arceus's SDK: nn::init::Start(thread handle, args, init_libraries, init_main, fini). Order inside
    // (from the translated code): _init_libc0, nnosInitialize, x2, _init_libc1, nninitInitializeSdkModule,
    // nninitStartup, _init_libc2, x3, nnMain, x4, nninitFinalizeSdkModule, nnosQuickExit.
    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t init_fn = hle_register("<rtld_init_libraries>", rtld_init_libraries);
    uint64_t main_fn = hle_register("<rtld_init_main>", rtld_init_main);
    uint64_t fini_fn = hle_register("<rtld_fini>", rtld_fini);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    t->cpu.x[4] = fini_fn;
    kernel_run_main(t, t->handle, 0, init_fn, main_fn);
}

int main(int argc, char **argv) {
    plat_init();
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // (the Windows CRT buffers _IOLBF fully: a crash lost the whole log)
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    if (argc < 2) {
        fprintf(stderr, "usage: %s <game dir with exefs/ and romfs.bin>\n", argv[0]);
        return 2;
    }
    boot(argv[1]);
#ifdef ARK_HEADLESS
    // No window: keep the process alive while guest threads run; ARK_RUN_SECONDS ends it.
    const char *secs = getenv("ARK_RUN_SECONDS");
    int limit = secs ? atoi(secs) : 0;
    for (int s = 0; !limit || s < limit; s++) sleep(1);
    LOG("ARK_RUN_SECONDS reached, exiting\n");
    _exit(0);
#else
    // Window and event loop on the main thread (runtime/nvn/vulkan/device.c); the game's threads wait in
    // nvnDeviceInitialize until it is up. ARK_RUN_SECONDS (tests) ends the process after that long.
    const char *secs = getenv("ARK_RUN_SECONDS");
    if (secs && atoi(secs) > 0) {
        static int limit;
        limit = atoi(secs);
        pthread_t t;
        pthread_create(&t, NULL, run_limit_thread, &limit);
    }
    extern void sampler_start(void);
    sampler_start();
    const char *wd = getenv("ARK_WATCHDOG");
    if (!wd || atoi(wd) != 0) {
        pthread_t w;
        pthread_create(&w, NULL, watchdog_thread, NULL);
    }
    extern int ui_run(void);
    ui_run();
    return 0;
#endif
}
