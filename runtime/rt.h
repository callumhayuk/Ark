// Runtime-internal declarations.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "cpu.h"
#include "platform.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define FATAL(...) (fprintf(stderr, "FATAL: " __VA_ARGS__), rt_fatal_trace())

// ---- address space layout (guest addresses == host addresses)
// macOS reserves 0x2_0000_0000-0x70_0000_0000, and guest addresses must fit in 39 bits, so the
// guest lives in a 12.5 GB window at 0x70_0000_0000 claimed at startup (see main.c).
#define GUEST_LO     0x7000000000ull
// Ark: module bases (tools/layout.py must agree)
#define MAIN_BASE    0x7000000000ull
#define MULTIMEDIA_BASE 0x7008000000ull
#define GLSLC_BASE   0x700A000000ull
#define SDK_BASE     0x700C000000ull
#define HLE_BASE     0x7080000000ull   // fake addresses for host-implemented functions
#define HLE_SIZE     0x0000100000ull
#define TLS_BASE     0x7090000000ull
#define TLS_SIZE     0x0000100000ull
#define HLE_HEAP     0x7092000000ull   // guest-visible memory owned by host code
#define HLE_HEAP_SIZE 0x000E000000ull
// Ark: the alias region is also what the SDK's virtual-address memory manager (nn::os VammManager) reserves its
// address space from (GetReservedRegionImpl reads svcGetInfo 2/3). Legends: Arceus asks it for 32 GB, so the
// region is 48 GB (on hardware, 64 GB). LAS: 4 GB at 0x71_0000_0000.
#define ALIAS_SIZE   0x0C00000000ull
#define HEAP_MAX     0x00D0000000ull   // 3.25 GB
#define STACK_SIZE   0x0040000000ull
#if defined(__APPLE__)
// PLA recomp, macOS 26+: malloc's heap range (about 24.5 GB of reservations, placed at random between
// 0x72_0000_0000 and 0x7D_0000_0000 on every launch, ASLR or not) sits inside 0x70-0x80, and 0xF_C000_0000-
// 0x70_0000_0000 is reserved. The guest is split in two windows: the modules, HLE, TLS and host heap stay low
// (code addresses are baked into the generated C), and the stacks, heap and alias region go above
// 0x80_0000_0000, where nothing is mapped. The guest address space (svcGetInfo 12/13) spans both.
#define GUEST_LOW_END  0x7100000000ull   // low window [GUEST_LO, GUEST_LOW_END): 4 GB
#define GUEST_HIGH_LO  0x8000000000ull   // high window [GUEST_HIGH_LO, GUEST_HI): 56 GB
#define GUEST_HI       0x8E00000000ull
#define STACK_BASE     0x8000000000ull
#define HEAP_BASE      0x8040000000ull
#define ALIAS_BASE     0x8200000000ull   // alias region (svcMapMemory targets, VAMM reserve), to GUEST_HI
#else
// Ark: one 64 GB window (LAS used 12.5 GB) to hold the 48 GB alias region at its top. Still below
// 0x80_0000_0000 (39-bit guest addresses). Pages are backed only when touched.
#define GUEST_HI       0x8000000000ull
#define GUEST_LOW_END  GUEST_HI          // (one window: no high part)
#define GUEST_HIGH_LO  GUEST_HI
#define STACK_BASE     0x7200000000ull
#define HEAP_BASE      0x7240000000ull
#define ALIAS_BASE     0x7400000000ull   // alias region (svcMapMemory targets, VAMM reserve)
#endif

// ---- loader.c
typedef struct Module {
    const char *name;
    uint64_t base, size;
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint64_t dynamic, init, fini, init_array, init_array_sz, fini_array, fini_array_sz;
    uint64_t symtab, strtab, nsyms;
} Module;

extern Module g_modules[8];
extern int g_nmodules;
void loader_load(const char *exefs_dir);  // (again after a reboot: modules are reloaded into wiped memory)
uint64_t loader_find_symbol(const char *name);
const char *loader_symbol_at(uint64_t addr, uint64_t *offset);

// ---- dispatch.c
typedef void (*HostFn)(Cpu *c);
void dispatch_init(void);
GuestFn dispatch_lookup(uint64_t addr);
uint64_t hle_register(const char *name, HostFn fn);  // returns fake guest address
HostFn hle_lookup_impl(const char *name);
void hle_override(uint64_t guest_addr, HostFn fn, const char *name);
void rt_fatal_trace(void) __attribute__((noreturn));
void crash_handler_install(void);  // per thread (alternate signal stack)

// ---- kernel.c
typedef struct KThread KThread;
void kernel_init(void);
void kernel_stop_threads(void (*idle)(void));  // reboot: unwind every guest thread and join it (idle: called while waiting)
void kernel_reset(void);                       // reboot: empty handle table, memory map, thread list
KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack_size, int prio);
void kernel_run_main(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3);
uint32_t kernel_current_thread_handle(void);
Cpu *kernel_current_cpu(void);

// ---- ipc.c
typedef struct Service Service;
int ipc_handle_request(Cpu *c, uint32_t handle);
uint32_t ipc_connect_named_port(const char *name);

// ---- in-place reboot (main.c): switch seeds or go back to the start screen without leaving the process
enum { RT_REBOOT_NONE, RT_REBOOT_GAME, RT_REBOOT_TITLE };  // GAME: boot again at once (new seed); TITLE: start screen
void rt_request_reboot(int kind);  // any thread; the window loop (ui_run) returns it and main() carries it out
int rt_reboot_requested(void);
extern volatile int g_rt_unwind;   // set while guest threads are being stopped (kernel.c)
void rt_unwind_point(void);        // guest threads: unwind to thread_main if a reboot is stopping them

// ---- ui (runtime/nvn/metal.m)
int ui_run(void);  // window loop; returns the reboot kind requested (the window stays open)

// ---- helpers
void guest_call(Cpu *c, uint64_t addr);   // call guest function at addr (saves/restores x30)
uint64_t guest_sym(const char *name);     // cached loader_find_symbol, fatal if missing
void *hle_alloc(uint64_t size);           // zeroed guest-visible memory (never freed)
extern const char *g_romfs_dir;
