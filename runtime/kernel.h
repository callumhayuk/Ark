// Kernel object types shared by kernel.c and the IPC/service layer.
#pragma once
#include <setjmp.h>

#include "rt.h"

// (STACK_BASE / STACK_SIZE: rt.h, per platform)

typedef enum { KOBJ_THREAD = 1, KOBJ_EVENT, KOBJ_SESSION, KOBJ_SHMEM, KOBJ_TMEM } KType;
typedef enum { WAIT_NONE = 0, WAIT_MUTEX, WAIT_CONDVAR, WAIT_ARBITER } WaitKind;

typedef struct KObject {
    KType type;
    int refs;
    bool signaled;
} KObject;

typedef KObject KEvent;

struct KThread {
    KObject obj;  // signaled when the thread exits
    Cpu cpu;
    PlatThread host;
    uint64_t entry, tlr, id;
    uint32_t handle;
    int prio, core;
    bool cancel, in_sync_wait;
    // wait state (protected by g_klock)
    WaitKind wait_kind;
    uint64_t wait_addr, wait_mutex;
    uint32_t wait_tag, wait_result;
    KThread *wnext;
    KThread *snext;        // in WaitSynchronization (g_syncwaiters)
    pthread_cond_t cond;   // each thread sleeps on its own condition; wakers signal it directly
    // in-place reboot (kernel_stop_threads): thread_main's setjmp point, where the thread unwinds to and ends
    jmp_buf unwind;
    bool started, finished;
    intptr_t cpu_clock;    // plat_thread_clock() of its host thread (LARECOMP_PROF_SVC's CPU times)
    uint64_t cpu_seen;
    KThread *anext;        // every thread of this boot (g_klock)
};

typedef struct KSharedMem {
    KObject obj;
    uint64_t size, guest_addr;  // guest_addr: where the guest mapped it (0 if not mapped)
    void (*on_map)(struct KSharedMem *s);  // HLE owner fills initial contents
    void *owner;
} KSharedMem;

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle);

typedef struct {
    KObject obj;
    uint64_t addr, size;
} KTransferMem;

extern pthread_mutex_t g_klock;
extern KThread *g_waitlist;

KObject *handle_get(uint32_t h);
uint32_t kernel_new_handle(KObject *o);
void kernel_signal(KObject *o);  // caller holds g_klock
KEvent *kernel_create_event_pair(uint32_t *writable, uint32_t *readable);
KThread *kernel_current_thread(void);
uint64_t kernel_heap_size(void);  // bytes from HEAP_BASE set by SetHeapSize
// Ark: physical-memory mappings (svcMapPhysicalMemory; the SDK's virtual-address memory, where Legends: Arceus keeps
// its heaps and GPU pools). Copies up to max merged ranges, returns how many; *gen changes whenever they change.
int kernel_phys_regions(uint64_t *addr, uint64_t *size, int max, uint64_t *gen);
uint64_t kernel_phys_gen(void);
void kernel_dump_waits(void);
int kernel_live_threads(KThread **out, int max);  // started, not finished (no lock: diagnostics only)  // every guest thread's wait state, to stderr (hang diagnosis)
