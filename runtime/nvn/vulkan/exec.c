// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../kernel.h"
#include "shaders.h"
#include "vk.h"
#include "../renderer.h"
#include "../../settings.h"

static uint64_t stat_skip_prog, stat_skip_failed, stat_skip_mode, stat_skip_pipe;
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
static uint64_t stat_upload_bytes;
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
static char stats_text[640];
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
    VkDescriptorPool gpools[4];  // update-after-bind pools for the global memory set (device.c g_vk.gmem_uab)
    int ngpools, cur_gpool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;
static bool xfb_pass;  // the open pass is a transform feedback capture's (xfb_begin_pass)

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

void vk_wait_seq(uint64_t seq) {
    if (!seq) return;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r != VK_SUCCESS) {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        vk_wait_seq(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        for (int i = 0; i < s->ngpools; i++) vkResetDescriptorPool(g_vk.dev, s->gpools[i], 0);
        s->cur_pool = s->cur_gpool = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    begin_cb(&s->upload, &s->upload_begun);
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
static char gp_label[512][112];
static uint32_t gp_draws[512];
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("LARECOMP_VK_DEVLOST_TRACE") ? atoi(getenv("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= 511) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 512};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) vkCmdResetQueryPool(cb, gp_qp, 0, 512);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
volatile int g_gpuprof_request;  // (hle/input.c Shift+F9; device.c)

// One line per profiled frame (Shift+F9): GPU time first to last mark, the frame's wall time, the three costliest rows
void vk_gpuprof_summary(uint64_t frame, double frame_ms) {
    if (!gp_n) return;
    renderer_flush();
    uint64_t t[512];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        int top[3] = {-1, -1, -1};
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            for (int k = 0; k < 3; k++) {
                if (top[k] >= 0 && ms <= (t[top[k] + 1] - t[top[k]]) * per) continue;
                for (int j = 2; j > k; j--) top[j] = top[j - 1];
                top[k] = i;
                break;
            }
        }
        LOG("[gpuprof] frame %llu: gpu %.1f ms (%d marks), frame %.1f ms | top:", (unsigned long long)frame, total, gp_n, frame_ms);
        for (int k = 0; k < 3; k++)
            if (top[k] >= 0) LOG(" %.1f ms %.60s%s", (t[top[k] + 1] - t[top[k]]) * per, gp_label[top[k]], k < 2 ? ";" : "");
        LOG("\n");
    }
    gp_n = 0;
}

void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    uint64_t t[512];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
        }
    }
    gp_n = 0;
}

// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
        VK_CHECK(sr);
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    pthread_mutex_unlock(&seq_lock);
    cur_sub = (cur_sub + 1) % SUBS;
    stat_flushes++;
    cum_submits++;
    ring_retire();
    vk_collect_dead();
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }

void renderer_flush(void) {
    renderer_submit();
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 5, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// The global memory set (gmem_set) when it is update-after-bind: from pools of its own carrying that flag, so the
// other sets stay in plain pools.
#define GMEM_POOL_CHUNKS 128  // (= MAX_GCHUNKS, defined further down)
static VkDescriptorSet vk_alloc_gmem_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_gpool == s->ngpools) {
            if (s->ngpools == 4) FATAL("global memory descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 * GMEM_POOL_CHUNKS},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 64,
                                             .poolSizeCount = 2, .pPoolSizes = sizes,
                                             .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->gpools[s->ngpools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->gpools[s->cur_gpool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(g_vk.dev, &ai, &set) == VK_SUCCESS) return set;
        s->cur_gpool++;
    }
}

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed.
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    return nchunks++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, seq; VkSpan span; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size) return gcopy[slot].span;
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s;
    return s;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;

static void *worker(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (!jobs_head) pthread_cond_wait(&jobs_cond, &jobs_lock);
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        pthread_mutex_unlock(&jobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation: one dedicated thread (the translator isn't re-entrant - translate.c's RSC2 string ring - and
// translating on the game thread cost up to ~90 ms per frame on entering an area). Compiles go on to the workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;  // (compute shaders translate on the game thread)
static Job *tjobs_head, *tjobs_tail;
static pthread_mutex_t tjobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tjobs_cond = PTHREAD_COND_INITIALIZER;
static void *translator(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&tjobs_lock);
        while (!tjobs_head) pthread_cond_wait(&tjobs_cond, &tjobs_lock);
        Job *j = tjobs_head;
        tjobs_head = j->next;
        if (!tjobs_head) tjobs_tail = NULL;
        pthread_mutex_unlock(&tjobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}
static bool sync_shaders(void);
static void run_translate(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) { started = true; pthread_t t; pthread_create(&t, NULL, translator, NULL); }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&tjobs_lock);
    if (tjobs_tail) tjobs_tail->next = j; else tjobs_head = j;
    tjobs_tail = j;
    pthread_cond_signal(&tjobs_cond);
    pthread_mutex_unlock(&tjobs_lock);
}

static void run_job(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) {
        started = true;
        // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
        int n = SDL_GetNumLogicalCPUCores() - 4;
        if (n < 4) n = 4;
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
    }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    float alpha_ref;  // nvnCommandBufferSetAlphaRef (the alpha test: color_state[2] = function)
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
    // transform feedback (Ark): bound buffers, whether capturing, bytes written per buffer and vertices captured
    Range xfb_buf[4];
    bool xfb_active, discard;
    uint32_t xfb_off[4], xfb_verts;
} st;

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (LARECOMP_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();
    if (last_t && now - last_t > 50000000) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending\n", (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

static void builds_check(void);
static void save_text(const char *path, const char *text);
const char *renderer_stats(void) {
    builds_check();
    {
        static int n;
        if (++n % 15 == 0) vk_memory_report("periodic");
    }
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped (prog %llu failed %llu mode %llu pipe %llu), %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d, fbcopy %llu, upload %.1fMB",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_skip_prog,
             (unsigned long long)stat_skip_failed, (unsigned long long)stat_skip_mode, (unsigned long long)stat_skip_pipe,
             (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0);
    stat_feedback_copies = stat_upload_bytes = 0;
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu", (unsigned long long)prof_views_created);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    stat_skip_prog = stat_skip_failed = stat_skip_mode = stat_skip_pipe = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    prof = getenv("LARECOMP_VK_PROF") != NULL;
    if (prof) {
        prof_up_ns = 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static int debug_pass_no;

static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
static NvnTexture *shadow_pass_tex;  // (F9) the current pass draws into this shadow-cascade layer
static uint32_t shadow_pass_layer;
static void debug_save_shadow(NvnTexture *t, uint32_t layer);
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    if (xfb_pass) { xfb_pass = false; return; }
    if (shadow_pass_tex) { NvnTexture *t = shadow_pass_tex; shadow_pass_tex = NULL; debug_save_shadow(t, shadow_pass_layer); }
    if (g_debug_frame && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        renderer_save_texture_tex(st.rt.color[0]->backend, path);
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

// Ark: transform feedback captures run in a pass with no attachments (their pipelines discard rasterization); a draw
// after them begins a real pass, whose full barrier makes the captured vertices visible to it.
static uint32_t xfb_q[5][4];  // XfbInfo of the current capture (see XfbLayout)
static void xfb_begin_pass(void) {
    if (in_pass && xfb_pass) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {1, 1}}, .layerCount = 1};
    vkCmdBeginRendering(cb, &ri);
    in_pass = true;
    xfb_pass = true;
    pass_ncolor = 0;
    memset(pass_color, 0, sizeof pass_color);
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    pass_samples = 1;
    pass_w = pass_h = 1;
    st.rt_dirty = true;  // the next ordinary draw begins its own pass
}

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty && !xfb_pass) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    shadow_pass_tex = NULL;
    if (g_debug_frame && st.rt.depth && st.rt.depth->width == st.rt.depth->height && st.rt.depth->width >= 512 &&
        st.rt.depth->target == 4) {  // (F9: the shadow cascades, saved when the pass ends: debug_save_shadow)
        shadow_pass_tex = st.rt.depth;
        shadow_pass_layer = st.rt.has_dview && (st.rt.dview.setmask & 2) ? st.rt.dview.min_layer : 0;
    }
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    in_pass = true;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void debug_save_depth(TexBackend *b, const char *path);
// (debug frames) the depth attachment right after a depth clear: is every earlier frame's depth gone?
static void debug_after_depth_clear(void) {
    if (!g_debug_frame || !st.rt.depth || !st.rt.depth->backend) return;
    static int n;
    char path[96];
    snprintf(path, sizeof path, "build/fbdepth_clear_%03d.pgm", n++);
    vk_end_pass();
    debug_save_depth(st.rt.depth->backend, path);
}
static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height || (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED))) {
        if (g_debug_frame) LOG("[clear] rt %d depth %d stencil %d: skipped (empty scissor or no such target)\n", index, depth, stencil);
        return;
    }
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
#if defined(__APPLE__)
    // MoltenVK: a depth+stencil vkCmdClearAttachments left the stencil as it was (the opening's silhouette effect marks
    // stencil every frame after such a clear; the marks of every earlier frame stayed: dark trails behind the
    // figures). Stencil clears go through the clear pipeline here, an ordinary draw writing stencil with REPLACE (and
    // depth through the vertex position). ARK_MVK_NATIVE_DS_CLEAR=1: vkCmdClearAttachments again, for comparison.
    static int native_ds = -1;
    if (native_ds < 0) native_ds = getenv("ARK_MVK_NATIVE_DS_CLEAR") != NULL;
    if (stencil && pass_stencil && !native_ds) simple = false;
#endif
    if (g_debug_frame)
        LOG("[clear] rt %d mask %#x depth %d (%g, has %d) stencil %d (%#x, write mask %#x, has %d) rect %d,%d %ux%u via %s\n",
            index, mask, depth, dval, pass_depth != VK_FORMAT_UNDEFINED, stencil, sval, smask, pass_stencil != VK_FORMAT_UNDEFINED,
            rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height, simple ? "vkCmdClearAttachments" : "clear pipeline");
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        if (depth && pass_depth) debug_after_depth_clear();
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    if (depth && pass_depth) debug_after_depth_clear();
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkShaderModule fs_dual;  // (lazy) fs with c1 as the second blend source of target 0 (dual-source blending)
    _Atomic int fs_dual_state;  // 0 not tried, 1 fs_dual ready, 2 impossible (no c1, or it failed to compile)
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    XfbLayout xfb;      // (vkey.xfb) the vertex program's transform feedback layout
    _Atomic int state;  // 0 building, 1 ready, 2 failed
} Prog;

// Set layouts shared by every program: GMEM_SET (global memory chunks + their map), and an empty one (compute
// programs have no set 1).
static VkDescriptorSetLayout g_empty_layout, g_gmem_layout;
static void global_layouts(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!g_gmem_layout) {
        VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)maxwell_gmem_chunks, VK_SHADER_STAGE_ALL, NULL},
                                              {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL}};
        VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb};
        // (device.c: an update-after-bind set when that lifts the chunk limit; its pools carry the matching flag)
        VkDescriptorBindingFlags bf[2] = {VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0};
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                           .bindingCount = 2, .pBindingFlags = bf};
        if (g_vk.gmem_uab) ci.pNext = &bfi, ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &g_gmem_layout));
        ci.pNext = NULL, ci.flags = 0;
        ci.bindingCount = 0;
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &g_empty_layout));
    }
    pthread_mutex_unlock(&lock);
}

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    global_layouts();
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkDescriptorSetLayout sl[3] = {p->set_layout[0], nsets > 1 ? p->set_layout[1] : g_empty_layout, g_gmem_layout};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
                                     .pSetLayouts = sl};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("LARECOMP_SAVE_MSL")) return;
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

void vk_save_prog_glsl(const void *prog, const char *prefix) {
    const Prog *p = prog;
    char path[160];
    snprintf(path, sizeof path, "%s_vs.glsl", prefix);
    save_text(path, p && p->vi ? p->vi->msl : NULL);
    snprintf(path, sizeof path, "%s_fs.glsl", prefix);
    save_text(path, p && p->fi ? p->fi->msl : NULL);
}

static void compile_program(void *arg) {
    Prog *p = arg;
    size_t nv = 0, nf = 0;
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
}

// The program's fragment shader with c1 as the dual-source blend input (made once; NULL when there is no c1 or it
// does not compile). Called from the pipeline workers.
static VkShaderModule prog_fs_dual(Prog *p) {
    int st = atomic_load(&p->fs_dual_state);
    if (st) return st == 1 ? p->fs_dual : VK_NULL_HANDLE;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!atomic_load(&p->fs_dual_state)) {
        static const char from[] = "layout(location = 1) out vec4 c1;";
        const char *src = p->fi ? p->fi->msl : NULL, *at = src ? strstr(src, from) : NULL;
        VkShaderModule m = VK_NULL_HANDLE;
        if (at) {
            static const char to[] = "layout(location = 0, index = 1) out vec4 c1;";
            size_t n = strlen(src), pre = (size_t)(at - src);
            char *dual = malloc(n + sizeof to);
            memcpy(dual, src, pre);
            memcpy(dual + pre, to, sizeof to - 1);
            strcpy(dual + pre + sizeof to - 1, at + sizeof from - 1);
            size_t nw = 0;
            uint32_t *spv = vk_glsl_to_spirv(dual, STAGE_FRAGMENT, "fs_dual", &nw);
            if (spv) m = vk_module(spv, nw);
            free(spv);
            free(dual);
        }
        p->fs_dual = m;
        atomic_store(&p->fs_dual_state, m ? 1 : 2);
    }
    pthread_mutex_unlock(&lock);
    return atomic_load(&p->fs_dual_state) == 1 ? p->fs_dual : VK_NULL_HANDLE;
}

static void translate_program(void *arg) {
    Prog *p = arg;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    pthread_mutex_lock(&translate_lock);
    maxwell_xfb = p->vkey.xfb ? &p->xfb : NULL;
    p->vi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX, &p->vkey);
    maxwell_xfb = NULL;
    if (p->vi->msl && !p->code[1]) {
        // Ark: vertex-only program: a fragment shader that writes nothing (out_mask 0 masks every colour write)
        static ShaderInfo empty_fs;
        if (!empty_fs.msl) empty_fs.msl = "#version 450\nvoid main() {}\n";
        p->fi = &empty_fs;
    } else if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], STAGE_FRAGMENT, &p->fkey);
    }
    pthread_mutex_unlock(&translate_lock);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s\n", p->fi->error);
        atomic_store(&p->state, 2);
        return;
    }
    if (p->vi->drops_image_writes || p->fi->drops_image_writes)
        LOG("[shader] program %llx/%llx: image stores dropped (storage images not bound yet)\n",
            (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    run_job(compile_program, p);
}

// Ark: transform feedback layout from a vertex program's NVN control section (worked out from Legends: Arceus's
// skinning programs): flag at 0x6ED; per buffer b an enable byte at 0x6E8 + b and a u16 stride at 0x6DC + 2b; varying j
// of buffer b at 0xDC + 0x80b + 4j (0x20 + k: output attribute k) with its component count at 0x4DC + 0x80b + 4j.
static bool xfb_program(const uint8_t *control) { return control && control[0x6ED]; }
static void xfb_layout(const uint8_t *control, XfbLayout *L, uint64_t vs) {
    memset(L, 0, sizeof *L);
    if (!xfb_program(control)) return;
    char desc[256] = "";
    for (int b = 0; b < 4; b++) {
        if (!control[0x6E8 + b]) continue;
        L->stride[b] = *(const uint16_t *)(control + 0x6DC + 2 * b);
        for (int j = 0; j < 32; j++) {
            uint32_t id = *(const uint32_t *)(control + 0xDC + 0x80 * b + 4 * j);
            uint32_t n = *(const uint32_t *)(control + 0x4DC + 0x80 * b + 4 * j);
            if (!n) break;
            L->attr[b][j] = (uint8_t)id;
            L->ncomp[b][j] = (uint8_t)n;
            L->nvar[b] = (uint8_t)(j + 1);
            size_t l = strlen(desc);
            snprintf(desc + l, sizeof desc - l, " b%d:%s%u.%u", b, id >= 0x20 && id < 0x40 ? "a" : "?", id >= 0x20 && id < 0x40 ? id - 0x20 : id, n);
        }
        size_t l = strlen(desc);
        snprintf(desc + l, sizeof desc - l, " (stride %u)", L->stride[b]);
    }
    static uint64_t seen[64];
    static int nseen;
    bool known = false;
    for (int i = 0; i < nseen; i++) known |= seen[i] == vs;
    if (!known && nseen < 64) { seen[nseen++] = vs; LOG("[xfb] vertex program %llx captures%s\n", (unsigned long long)vs, desc); }
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(np->stage_mask & 1) || !(np->stage_mask & 2) || (np->stage_mask & ~3u & 0x1F)) {
        static uint32_t seen[64];
        static int nseen;
        bool known = false;
        for (int i = 0; i < nseen; i++) known |= seen[i] == np->stage_mask;
        if (!known && nseen < 64) {
            seen[nseen++] = np->stage_mask;
            LOG("[vk] programs with stages %#x (bit 0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess eval): stages past "
            "vertex+fragment are not drawn yet; vertex-only is drawn depth-only "
                "(vs %llx)\n", np->stage_mask, (unsigned long long)np->stages[0].code_addr);
        }
        // vertex-only programs (shadow maps, depth pre-passes) are drawn with an empty fragment shader
        if (!(np->stage_mask & 1)) return NULL;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = (np->stage_mask >> i) & 1 ? np->stages[i].code_addr : 0; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 64; i++) { h ^= (uint64_t)(i < 32 ? fkey->tex_uint >> i : fkey->tex_sint >> (i - 32)) & 1; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    Prog *p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    for (int i = 0; i < 2; i++) {
        p->code[i] = (np->stage_mask >> i) & 1 ? np->stages[i].code_addr : 0;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    if (vkey->xfb) xfb_layout(np->stages[0].control, &p->xfb, p->code[0]);
    run_translate(translate_program, p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: {
        static uint8_t told[256];
        if (!told[f & 0xFF]++) LOG("[vk] vertex attribute format %#x unknown: read as 4 floats\n", f);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

// Dual-source blending: factors 0x10..0x13 (SRC1_*) read a second colour from the fragment shader. On Maxwell that is
// output 1 (c1); Vulkan and Metal take it from location 0, index 1. A program that writes c1 gets a variant with c1
// declared that way (prog_fs_dual). Without one (no c1, or several targets), SRC1 is taken as zero: the factors are
// rewritten to what a zero second colour gives. (Desktop drivers blend an undefined value there; Metal refuses
// the pipeline: "Fragment shader does not write to render target color(0), index(1) that is required for blending".)
static bool is_src1(uint8_t f) { return f >= 0x10 && f <= 0x13; }
static uint8_t src1_as_zero(uint8_t f) {
    switch (f) {
    case 0x10: case 0x12: return 1;  // SRC1_COLOR / SRC1_ALPHA -> ZERO
    case 0x11: case 0x13: return 2;  // ONE_MINUS_SRC1_* -> ONE
    default: return f;
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
    uint8_t discard;  // rasterizer discard (transform feedback capture)
} PipeKey;

typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state; } Pipe;  // state: 0 building, 1 ready, 2 failed
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// Pipeline builds in progress (the driver compiles the shaders here): one that takes long is reported once, with its
// shaders' GLSL saved to build/slow_<vs>_<fs>_{vs,fs}.glsl, since a driver compiler that runs out of memory takes
// the process down without saying which shader it was on.
#define MAX_BUILDS 32
static struct { _Atomic uint64_t start; const void *prog; uint64_t vs, fs; bool reported; } builds[MAX_BUILDS];
static int build_begin(const void *prog, uint64_t vs, uint64_t fs) {
    for (int i = 0; i < MAX_BUILDS; i++) {
        uint64_t z = 0;
        if (atomic_compare_exchange_strong(&builds[i].start, &z, ~0ull)) {
            builds[i].prog = prog; builds[i].vs = vs; builds[i].fs = fs; builds[i].reported = false;
            atomic_store(&builds[i].start, SDL_GetTicksNS());
            return i;
        }
    }
    return -1;
}
static void build_end(int i) { if (i >= 0) atomic_store(&builds[i].start, 0); }
static void save_text(const char *path, const char *text) {
    FILE *f = text ? fopen(path, "w") : NULL;
    if (f) { fputs(text, f); fclose(f); }
}
static void builds_check(void) {
    uint64_t now = SDL_GetTicksNS();
    for (int i = 0; i < MAX_BUILDS; i++) {
        uint64_t t = atomic_load(&builds[i].start);
        if (!t || t == ~0ull || builds[i].reported || now - t < 5000000000ull) continue;
        builds[i].reported = true;
        LOG("[vk] a pipeline build has taken %.0fs so far: vs %llx fs %llx (GLSL in build/slow_%llx_%llx_*.glsl)\n",
            (now - t) / 1e9, (unsigned long long)builds[i].vs, (unsigned long long)builds[i].fs, (unsigned long long)builds[i].vs,
            (unsigned long long)builds[i].fs);
        extern void vk_save_prog_glsl(const void *prog, const char *prefix);
        char prefix[96];
        snprintf(prefix, sizeof prefix, "build/slow_%llx_%llx", (unsigned long long)builds[i].vs, (unsigned long long)builds[i].fs);
        vk_save_prog_glsl(builds[i].prog, prefix);
    }
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    PipeKey *k = &pp->k;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1,
                                                 .rasterizerDiscardEnable = k->discard};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            uint8_t b[8];
            memcpy(b, k->blend[i], sizeof b);
            if (is_src1(b[1]) || is_src1(b[2]) || is_src1(b[3]) || is_src1(b[4])) {
                // dual-source: c1 is the second source where there is one target and the program writes c1
                VkShaderModule dual = i == 0 && k->ncolor == 1 && g_vk.dual_src ? prog_fs_dual(k->prog) : VK_NULL_HANDLE;
                if (dual) stages[1].module = dual;
                else for (int j = 1; j <= 4; j++) b[j] = src1_as_zero(b[j]);
            }
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    int bi = build_begin(k->prog, k->prog->code[0], k->prog->code[1]);
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);
    build_end(bi);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) {
            LOG("[vk] pipeline creation failed: %d (vs %llx fs %llx)\n", r, (unsigned long long)k->prog->code[0],
                (unsigned long long)k->prog->code[1]);
            // the program's GLSL, for reproducing the failure offline (e.g. MoltenVK's SPIR-V -> MSL conversion)
            mkdir("build/glsl-failed", 0755);
            char prefix[96];
            snprintf(prefix, sizeof prefix, "build/glsl-failed/%llx_%llx", (unsigned long long)k->prog->code[0],
                     (unsigned long long)k->prog->code[1]);
            vk_save_prog_glsl(k->prog, prefix);
        }
    }
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static int nfeedback;
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

// (F9, LARECOMP_DUMP_TEX) GPU image read-backs, written once their submission has completed (vk_debug_readbacks)
// (vk_readback is one shared staging buffer, so each copy is submitted and waited for on its own; the first version
// copied mid-frame and read pass-image saves instead)
static struct { NvnTexture *t; char path[160]; } rb[4];
static int nrb;
void vk_debug_readbacks(void) {
    for (int i = 0; i < nrb; i++) {
        NvnTexture *t = rb[i].t;
        TexBackend *b = t ? (TexBackend *)t->backend : NULL;
        const FormatInfo *fi = t ? t->layout.fmt : NULL;
        if (!b || !b->image || !fi || !fi->bpb) continue;
        uint32_t wb = (b->width + fi->bw - 1) / fi->bw, hb = (b->height + fi->bh - 1) / fi->bh;
        uint64_t sz = (uint64_t)wb * hb * fi->bpb;
        vk_end_pass();
        VkSpan sp = vk_readback(sz);
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkBufferImageCopy rg = {.bufferOffset = sp.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {b->width, b->height, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, sp.buf, 1, &rg);
        vk_full_barrier(cb);
        renderer_flush();
        vk_wait_seq(submit_seq);
        FILE *f = fopen(rb[i].path, "wb");
        if (f) { fwrite(sp.ptr, 1, sz, f); fclose(f); }
        LOG("[vk] read back %s (%llu bytes)\n", rb[i].path, (unsigned long long)sz);
    }
    nrb = 0;
}

// (F9) a shader texture's sampler: NVN wrap modes (0 CLAMP, 1 REPEAT, 2 MIRROR_CLAMP, 3 MIRROR_CLAMP_TO_EDGE,
// 4 MIRROR_CLAMP_TO_BORDER, 5 CLAMP_TO_BORDER, 6 MIRRORED_REPEAT, 7 CLAMP_TO_EDGE), filters and border colour
static void debug_sampler(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tsc = (uint32_t)(h >> 20) & 0xFFF;
    if (!h || !st.spool || tsc >= (uint32_t)st.spool->count) { LOG("[draw]     sampler: none (default)\n"); return; }
    const NvnSamplerDesc *d = &((NvnSamplerDesc *)st.spool->entries)[tsc];
    LOG("[draw]     sampler %u: wrap %u %u %u min %u mag %u border %g %g %g %g", tsc, d->wrap[0], d->wrap[1], d->wrap[2],
        d->min_filter, d->mag_filter, d->border[0], d->border[1], d->border[2], d->border[3]);
    // the component swizzle the image view gets (vk_view: the pool view's if it sets one, else the texture's;
    // NVN 0 zero, 1 one, 2 r, 3 g, 4 b, 5 a; all zero: never set, identity)
    uint32_t tic = h & 0xFFFFF;
    if (st.tpool && tic < (uint32_t)st.tpool->count) {
        const NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            const uint8_t *sw = e->has_view && (e->view.setmask & 8) ? e->view.swizzle : e->tex->swizzle;
            LOG(" | swizzle %u %u %u %u (%s)", sw[0], sw[1], sw[2], sw[3], e->has_view && (e->view.setmask & 8) ? "view" : "texture");
            if (e->has_view && (e->view.setmask & 4)) LOG(" view format %#x", e->view.format);
        }
    }
    LOG("\n");
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// (F9) a shadow-cascade layer after each pass into it (the last one wins): build/shadow_L<layer>.pgm (scaled to its
// depth range, the first word of the log line) and build/shadow_L<layer>.f32 (the raw floats, row by row)
static void debug_save_shadow(NvnTexture *t, uint32_t layer) {
    TexBackend *b = t->backend;
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    if (b->format != VK_FORMAT_D32_SFLOAT && b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    if (layer >= b->layers) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) if (d[i] < 1.0f) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    char path[96];
    snprintf(path, sizeof path, "build/shadow_L%u.f32", layer);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(d, 4, n, f); fclose(f); }
    snprintf(path, sizeof path, "build/shadow_L%u.pgm", layer);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(d[i] >= 1.0f ? 255 : hi > lo ? (int)((d[i] - lo) / (hi - lo) * 254.0f) : 0, f);
    fclose(f);
    static int nlog;
    if (nlog++ < 64) LOG("[shadow] saved %s (%ux%u, depth below 1: %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// (debug frames, LARECOMP_DUMP_R8=1) an R8 image's level 0 as a PGM: what the GPU actually samples
static void debug_save_r8(TexBackend *b, const char *path) {
    if (!b || !b->image || b->format != VK_FORMAT_R8_UNORM) return;
    uint64_t n = (uint64_t)b->width * b->height;
    vk_end_pass();
    VkSpan out = vk_readback(n);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    fwrite(out.ptr, 1, n, f);
    fclose(f);
    LOG("[vk] saved %s\n", path);
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                feedback_tex[nfeedback] = t;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG / STG / RED) and in-place vertex / index data: guest memory imported as GPU buffers
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so GPU results land in guest memory.
// Legends: Arceus keeps its heaps in the SDK's virtual-address memory (svcMapPhysicalMemory, runtime/kernel.c), in
// two clusters: many small maps from ALIAS_BASE up, and one large map (~3.2 GB) far above. Each is an arena: arena 1
// is the largest physical mapping (>= 256 MB), arena 0 spans every mapping below it. Arenas are divided into chunks
// of the device's buffer range limit (maxwell_gmem_chunk_log2, at most 64 MB), arena 0's from slot 0 up, arena 1's
// at the top of the array; shaders get them as one descriptor array (set GMEM_SET, maxwell/shader.h). Only chunks
// holding an NVN memory pool are imported - the GPU only reads pools - since an import commits and pins all of its
// pages (importing the arenas whole ran a 16 GB Windows PC out of commit). Other addresses read 0.
// LARECOMP_VK_NO_GMEM=1: no imports, reads return zero.
// Ark: chunks are found through a table (maxwell_gmem_table): gch is indexed by chunk number (arena 0: 0..127,
// arena 1: 128..255) and only imported chunks get a descriptor slot, so the device's slot limit (31 on MoltenVK)
// covers every chunk holding a pool instead of the first 2 GB of a linear layout.
#define MAX_GCHUNKS 256  // chunk numbers
#define ARENA_CHUNKS 128
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint64_t base, size; bool failed; } GChunk;
static GChunk gch[MAX_GCHUNKS];
static struct { uint64_t base, size; int c0, n; } arenas[2];  // c0: the arena's first chunk number
static int16_t slot_of[MAX_GCHUNKS], chunk_of_slot[128];      // (-1: none)
static bool slots_init;
static void slots_reset(void) {
    for (int i = 0; i < MAX_GCHUNKS; i++) slot_of[i] = -1;
    for (int i = 0; i < 128; i++) chunk_of_slot[i] = -1;
    slots_init = true;
}
// Checks that the game never gives back pages under an import (kernel.c svcUnmapPhysicalMemory -> plat_discard): the
// GPU would stall, and could go on reading the old pages
static void gmem_discard_check(uint64_t addr, uint64_t size) {
    for (int kn = 0; kn < MAX_GCHUNKS; kn++) {
        const GChunk *c = &gch[kn];
        if (!c->buf || addr >= c->base + c->size || addr + size <= c->base) continue;
        static int n;
        if (n++ < 8)
            LOG("[vk] warning: guest pages %llx +%llu KB given back under imported chunk %d (%llx +%llu MB)\n",
                (unsigned long long)addr, (unsigned long long)(size >> 10), kn, (unsigned long long)c->base,
                (unsigned long long)(c->size >> 20));
        return;
    }
}
__attribute__((constructor)) static void gmem_discard_hook_install(void) {
    extern void (*g_discard_hook)(uint64_t, uint64_t);
    g_discard_hook = gmem_discard_check;
}

static void slot_assign(int kn) {
    if (slot_of[kn] >= 0) return;
    for (int i = 0; i < maxwell_gmem_chunks && i < 128; i++)
        if (chunk_of_slot[i] < 0) {
            chunk_of_slot[i] = (int16_t)kn, slot_of[kn] = (int16_t)i;
            LOG("[vk] guest memory chunk %d (%llx +%llu MB) -> slot %d\n", kn, (unsigned long long)gch[kn].base,
                (unsigned long long)(gch[kn].size >> 20), i);
            return;
        }
    static int warned;
    if (!warned++)
        LOG("[vk] warning: more guest memory chunks hold pools than the %d descriptor slots: chunk %d (%llx) unreachable "
            "from shaders (reads return zero)\n", maxwell_gmem_chunks, kn, (unsigned long long)gch[kn].base);
}
static void slots_free_arena1(void) {
    for (int kn = ARENA_CHUNKS; kn < MAX_GCHUNKS; kn++)
        if (slot_of[kn] >= 0) chunk_of_slot[slot_of[kn]] = -1, slot_of[kn] = -1;
}
static uint64_t arena_gen = ~0ull, pool_gen_seen = ~0ull;
static uint64_t gmem_version;  // bumped whenever a chunk is (re)imported
// NVN memory pools (backend_pool_created; objects.c), the ranges to import
static struct { uint64_t addr, size; } pools[4096];
static int npools;
static _Atomic uint64_t pool_gen;
static pthread_mutex_t pools_lock = PTHREAD_MUTEX_INITIALIZER;

void backend_pool_created(uint64_t addr, uint64_t size) {
    if (!size) return;
    pthread_mutex_lock(&pools_lock);
    if (npools < 4096) { pools[npools].addr = addr; pools[npools].size = size; npools++; }
    pool_gen++;
    pthread_mutex_unlock(&pools_lock);
}

// Replaced imports (arena 0's regions grow as pools are added), freed once no pending submission can use them: kept
// for good they piled up in MoltenVK's residency set until a level load ran the GPU out of memory (2026-11-03)
static struct { VkBuffer buf; VkDeviceMemory mem; uint64_t seq; } gdead[256];
static int ngdead;
static void gmem_collect_dead(void) {
    if (!ngdead) return;
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < ngdead; i++) {
        if (gdead[i].seq <= done) { vkDestroyBuffer(g_vk.dev, gdead[i].buf, NULL); vkFreeMemory(g_vk.dev, gdead[i].mem, NULL); }
        else gdead[k++] = gdead[i];
    }
    ngdead = k;
}
static void gmem_retire(GChunk *ch) {
    if (!ch->buf) return;
    if (ngdead == 256) { vk_wait_seq(submit_seq); gmem_collect_dead(); }
    gdead[ngdead].buf = ch->buf, gdead[ngdead].mem = ch->mem, gdead[ngdead].seq = vk_pending_seq();
    ngdead++;
}

static bool gmem_import(GChunk *ch, uint64_t base, uint64_t size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    plat_commit(base, size);  // (Windows: an import needs committed pages)
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)base, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)base};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    gmem_retire(ch);  // (the replaced import: freed once recorded work can no longer use it)
    *ch = (GChunk){buf, mem, base, size, false};
    gmem_version++;
    return true;
}

static int gmem_off(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import;
    return off;
}

// Imports the chunks of arena a overlapping [lo, hi) (clipped to the arena) that aren't imported yet (or are
// shorter than the arena now allows: arena 0's last chunk, as it grows).
static void gmem_import_range(int a, uint64_t lo, uint64_t hi) {
    uint64_t cs = 1ull << maxwell_gmem_chunk_log2, ab = arenas[a].base, ae = ab + arenas[a].size;
    if (hi <= ab || lo >= ae) return;
    if (lo < ab) lo = ab;
    if (hi > ae) hi = ae;
    if (!slots_init) slots_reset();
    static int no_arena0 = -1;  // ARK_GMEM_NO_ARENA0=1: experiment, arena 0 (the small mappings) not imported
    if (no_arena0 < 0) no_arena0 = getenv("ARK_GMEM_NO_ARENA0") && atoi(getenv("ARK_GMEM_NO_ARENA0"));
    if (a == 0 && no_arena0) return;
    for (uint64_t k = (lo - ab) / cs; k <= (hi - 1 - ab) / cs; k++) {
        if (k >= ARENA_CHUNKS) break;
        int kn = arenas[a].c0 + (int)k;
        uint64_t b = ab + k * cs, e = b + cs < ae ? b + cs : ae;
        GChunk *ch = &gch[kn];
        if (a == 0) {
            // Arena 0 (the small mappings: heaps the game grows and shrinks) imports only the pools' pages, 64 KB-aligned:
            // svcUnmapPhysicalMemory there swaps pages for fresh ones (plat_discard), and an import over them stalled the
            // GPU for ~75 ms each time and cost 5-15 ms a frame (2026-11-03). The slot's offset in its chunk: gmo.
            uint64_t rb = lo & ~0xFFFFull, re = (hi + 0xFFFF) & ~0xFFFFull;
            if (rb < b) rb = b;
            if (re > e) re = e;
            if (ch->buf) {  // (another pool in the same chunk: the union of the two)
                if (ch->base < rb) rb = ch->base;
                if (ch->base + ch->size > re) re = ch->base + ch->size;
                if (rb == ch->base && re == ch->base + ch->size) continue;
            } else if (ch->failed) continue;
            b = rb, e = re;
        } else if (ch->failed || (ch->buf && ch->base + ch->size >= e)) continue;
        if (!gmem_import(ch, b, e - b)) {
            ch->failed = true;
            LOG("[vk] guest memory import failed at %llx (+%llu MB): shader reads there return zero\n", (unsigned long long)b,
                (unsigned long long)((e - b) >> 20));
            continue;
        }
        slot_assign(kn);
        gmem_version++;
    }
}

// Re-reads the physical mappings and pools when they changed and imports what is new.
static void gmem_update(void) {
    gmem_collect_dead();
    if (gmem_off() || (kernel_phys_gen() == arena_gen && atomic_load(&pool_gen) == pool_gen_seen)) return;
    uint64_t gen, ra[1024], rs[1024];
    int n = kernel_phys_regions(ra, rs, 1024, &gen);
    arena_gen = gen;
    pool_gen_seen = atomic_load(&pool_gen);
    uint64_t cs = 1ull << maxwell_gmem_chunk_log2;
    int big = -1;
    for (int i = 0; i < n; i++)
        if (rs[i] >= (256ull << 20) && (big < 0 || rs[i] > rs[big])) big = i;
    if (big >= 0) {  // arena 1: the large mapping, chunks at the top of the array
        uint64_t b = ra[big] & ~0xFFFFull, e = (ra[big] + rs[big] + 0xFFFF) & ~0xFFFFull;
        if (arenas[1].base != b || arenas[1].size != e - b) {
            int nc = (int)((e - b + cs - 1) / cs);
            if (!slots_init) slots_reset();
            arenas[1] = (typeof(arenas[1])){b, e - b, ARENA_CHUNKS, nc};
            slots_free_arena1();
            for (int i = ARENA_CHUNKS; i < MAX_GCHUNKS; i++) memset(&gch[i], 0, sizeof gch[i]);
            gmem_version++;
            LOG("[vk] guest memory arena 1: %llx..%llx (%d chunks)\n", (unsigned long long)b, (unsigned long long)e, nc);
        }
    }
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < n; i++) {
        if (i == big || (big >= 0 && ra[i] >= ra[big])) continue;
        if (ra[i] < lo) lo = ra[i];
        if (ra[i] + rs[i] > hi) hi = ra[i] + rs[i];
    }
    if (hi > lo) {
        if (!arenas[0].size) arenas[0].base = lo & ~0xFFFFull;  // (arena 0's base stays: chunks are only added)
        uint64_t e = (hi + 0xFFFF) & ~0xFFFFull, maxe = arenas[0].base + (uint64_t)ARENA_CHUNKS * cs;
        if (e > maxe) e = maxe;
        if (e > arenas[0].base + arenas[0].size) { arenas[0].size = e - arenas[0].base; gmem_version++; }
    }
    pthread_mutex_lock(&pools_lock);
    int np = npools;
    pthread_mutex_unlock(&pools_lock);
    for (int i = 0; i < np; i++)
        for (int a = 0; a < 2; a++)
            if (arenas[a].size) gmem_import_range(a, pools[i].addr, pools[i].addr + pools[i].size);
}

// Set GMEM_SET for this submission (made again when the chunks changed).
static VkDescriptorSet gmem_set(void) {
    static uint64_t set_seq = ~0ull, set_ver = ~0ull;
    static VkDescriptorSet set;
    gmem_update();
    uint64_t seq = vk_pending_seq();
    if (set && set_seq == seq && set_ver == gmem_version) return set;
    set = g_vk.gmem_uab ? vk_alloc_gmem_set(g_gmem_layout) : vk_alloc_set(g_gmem_layout);
    set_seq = seq;
    set_ver = gmem_version;
    static uint32_t zero[64];
    VkSpan zs = vk_transient_copy(zero, sizeof zero, 256);
    if (!slots_init) slots_reset();
    VkDescriptorBufferInfo bi[128];
    for (int i = 0; i < maxwell_gmem_chunks; i++) {
        int kn = chunk_of_slot[i];
        bi[i] = kn >= 0 && gch[kn].buf ? (VkDescriptorBufferInfo){gch[kn].buf, 0, gch[kn].size}
                                       : (VkDescriptorBufferInfo){zs.buf, zs.off, sizeof zero};
    }
    uint32_t m[12 + 64 + 128] = {0};  // gmm[3] (arena bases / sizes), gmt (a byte per chunk number: its slot or 0xFF), gmo
    for (int a = 0; a < 2; a++) {
        uint64_t b = arenas[a].size ? arenas[a].base : 0, z = arenas[a].size;
        m[a * 4] = (uint32_t)b; m[a * 4 + 1] = (uint32_t)(b >> 32); m[a * 4 + 2] = (uint32_t)z; m[a * 4 + 3] = (uint32_t)(z >> 32);
    }
    m[8] = (uint32_t)arenas[1].c0;
    for (int kn = 0; kn < MAX_GCHUNKS; kn++) {
        uint32_t v = slot_of[kn] >= 0 && gch[kn].buf ? (uint32_t)slot_of[kn] : 0xFFu;
        m[12 + kn / 4] |= v << ((kn & 3) * 8);
    }
    // gmo: each slot's buffer start within its chunk (arena 0 imports only the pools' pages)
    uint64_t csz = 1ull << maxwell_gmem_chunk_log2;
    for (int i = 0; i < maxwell_gmem_chunks && i < 128; i++) {
        int kn = chunk_of_slot[i];
        if (kn < 0 || !gch[kn].buf) continue;
        int a = kn >= ARENA_CHUNKS;
        uint64_t cb = arenas[a].base + (uint64_t)(kn - arenas[a].c0) * csz;
        m[12 + 64 + i] = (uint32_t)(gch[kn].base - cb);
    }
    VkSpan ms = vk_transient_copy(m, sizeof m, 256);
    VkDescriptorBufferInfo mi = {ms.buf, ms.off, sizeof m};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = (uint32_t)maxwell_gmem_chunks,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &mi}};
    vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
    return set;
}

// Vertex / index data read in place from the imported guest memory (renderer.m bind_guest does the same with
// g_guest_buffer): no per-draw copy, no index scan for the copy size. The game fences its CPU writes as on the
// Switch. Falls back to copies outside the chunks or without the import; LARECOMP_VK_COPY_VERTICES=1 forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy) return false;
    gmem_update();
    for (int i = 0; i < MAX_GCHUNKS; i++)
        if (gch[i].buf && addr >= gch[i].base && addr + size <= gch[i].base + gch[i].size) {
            *buf = gch[i].buf;
            *off = addr - gch[i].base;
            return true;
        }
    return false;
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_cs_n = 0;
    gcopy_gen++;
    vk_wait_seq(submit_seq);
    for (int i = 0; i < MAX_GCHUNKS; i++)
        if (gch[i].buf) { vkDestroyBuffer(g_vk.dev, gch[i].buf, NULL); vkFreeMemory(g_vk.dev, gch[i].mem, NULL); }
    memset(gch, 0, sizeof gch);
    memset(arenas, 0, sizeof arenas);
    slots_reset();
    arena_gen = pool_gen_seen = ~0ull;
    pthread_mutex_lock(&pools_lock);
    npools = 0;
    pthread_mutex_unlock(&pools_lock);
    gmem_version++;
    st_defaults();
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[12], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii) {
    VkDescriptorSet ds = vk_alloc_set(layout);
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    float rs[65][4] = {{1, 1, 0, 0}};
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || (b->binding != 18 && b->binding != 19 && b->binding != XFB_BINDING)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == XFB_BINDING) {
            bi[n] = ubo_span(vk_transient_copy(xfb_q, sizeof xfb_q, 256), sizeof xfb_q);
        } else if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[12] = {0};  // (tint: two vec4s; [8], [9]: the alpha test's function and reference)
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 48, 256), 48);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

static struct CsEnt { uint64_t addr; CsProg *cp; } g_cs_tab[256];  // compute programs by code address

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (n < 256) { tab[n].addr = ss->code_addr; tab[n].cp = cp; g_cs_n = n + 1; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.glsl = 1;
    pthread_mutex_lock(&translate_lock);
    cp->si = maxwell_translate((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    if (cp->si->drops_image_writes)
        LOG("[compute] %llx: image stores dropped (storage images not bound yet)\n", (unsigned long long)ss->code_addr);
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

// (F9) a debug frame's dispatches: program, groups, storage / uniform bindings (GPU particle simulations write their
// particle buffers here: the rain, 2026-11-03) and the GLSL once per program in build/glsl-f9/<cs>_cs.glsl
static void debug_dispatch(CsProg *cp, uint32_t gx, uint32_t gy, uint32_t gz, const char *what) {
    uint64_t cs = st.program->stages[5].code_addr;
    LOG("[dispatch] %s cs=%llx groups %ux%ux%u local %ux%ux%u gmem %d", what, (unsigned long long)cs, gx, gy, gz,
        cp ? cp->local[0] : 0, cp ? cp->local[1] : 0, cp ? cp->local[2] : 0, cp && cp->si ? cp->si->uses_gmem : -1);
    if (cp && cp->si) LOG(" cbufs %#x", cp->si->cbuf_mask);
    LOG("\n");
    for (int i = 0; i < 16; i++)
        if (st.sbuf[5][i].addr)
            LOG("[dispatch]   sbuf%d %llx size %llu\n", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
    for (int i = 0; i < 18; i++)
        if (st.ubuf[5][i].addr) {
            const float *u = (const float *)(uintptr_t)st.ubuf[5][i].addr;
            LOG("[dispatch]   ubuf%d (cb%d) %llx size %u:", i, i + 3, (unsigned long long)st.ubuf[5][i].addr, (unsigned)st.ubuf[5][i].size);
            for (int k = 0; k < 24 && k * 4 < (int)st.ubuf[5][i].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    static uint64_t saved[256];
    static int nsaved;
    bool have = false;
    for (int i = 0; i < nsaved; i++) have |= saved[i] == cs;
    if (!have && nsaved < 256 && cp && cp->si && cp->si->msl) {
        saved[nsaved++] = cs;
        mkdir("build/glsl-f9", 0755);
        char path[96];
        snprintf(path, sizeof path, "build/glsl-f9/%llx_cs.glsl", (unsigned long long)cs);
        FILE *f = fopen(path, "w");
        if (f) { fputs(cp->si->msl, f); fclose(f); }
    }
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp || atomic_load(&cp->p.state) != 1) {
        if (g_debug_frame) debug_dispatch(cp, gx, gy, gz, !cp ? "SKIPPED (no program)" : atomic_load(&cp->p.state) == 2 ? "SKIPPED (failed)" : "SKIPPED (building)");
        stat_skipped++;
        return;
    }
    if (g_debug_frame) debug_dispatch(cp, gx, gy, gz, "run");
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u)", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2]);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    nfeedback = 0;
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    if (cp->si->uses_gmem) {  // (see draw_)
        VkDescriptorSet gs = gmem_set();
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, GMEM_SET, 1, &gs, 0, NULL);
    }
    vkCmdDispatch(cb, gx, gy, gz);
    stat_dispatches++;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
    // (debug frames, ARK_F9_SPLIT_BLEND=1) end the pass after each blended draw into a target with depth, so each one's
    // result is saved as its own pass image: finds the draw that paints the dark trails in the opening
    static int split = -1;
    if (split < 0) split = getenv("ARK_F9_SPLIT_BLEND") ? atoi(getenv("ARK_F9_SPLIT_BLEND")) : 0;  // (=0: off; =2: passes
                                                                                                 //  without depth: HUD)
    static uint64_t split_frame = ~0ull;
    static int nsplit;
    if (split && g_debug_frame && in_pass && (split == 2 ? !st.rt.depth : st.rt.depth != NULL) && (st.color_state[0] & 1)) {
        if (renderer_present_count() != split_frame) { split_frame = renderer_present_count(); nsplit = 0; }
        if (nsplit++ < 160) { LOG("[draw]   (split: pass ends here)\n"); vk_end_pass(); }
    }
}
// ARK_LOG_SHADOW=1: every distinct kind of draw into a square render target of 512..4096 (the shadow cascades), once:
// its targets, polygon offset and program stages, and why it was skipped if it was (shadow acne, 2026-11-02).
static bool shadow_log_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("ARK_LOG_SHADOW") && strcmp(getenv("ARK_LOG_SHADOW"), "0");
    return on;
}
static bool shadow_target(void) {
    const NvnTexture *t = st.rt.depth ? st.rt.depth : st.rt.ncolor ? st.rt.color[0] : NULL;
    return t && t->width == t->height && t->width >= 512 && t->width <= 4096;
}
static uint64_t shadow_sig;  // the current draw's signature (0: not a shadow draw / not logging)
static bool shadow_new(uint64_t sig) {
    static uint64_t seen[256];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == sig) return false;
    if (n < 256) seen[n++] = sig;
    return n < 256;
}
static void shadow_log_draw(void) {
    shadow_sig = 0;
    if (!shadow_log_on() || !shadow_target() || !st.program) return;
    const NvnTexture *d = st.rt.depth, *c0 = st.rt.ncolor ? st.rt.color[0] : NULL;
    uint64_t vs = st.program->stage_mask & 1 ? st.program->stages[0].code_addr : 0;
    uint64_t fs = st.program->stage_mask & 2 ? st.program->stages[1].code_addr : 0;
    uint64_t sig = vs * 0x9E3779B97F4A7C15ull ^ fs ^ (uint64_t)st.program->stage_mask << 56 ^ (uint64_t)(st.poly[3] & 7) << 48 ^
                   (uint64_t)(d ? d->format : 0) << 40 ^ (uint64_t)(c0 ? c0->format : 0) << 32 ^ (uint64_t)st.discard << 60;
    shadow_sig = sig;
    if (!shadow_new(sig)) return;
    LOG("[shadow] draw: depth %s fmt %#x %ux%ux%u target %u | color0 %s fmt %#x | ncolor %d | stages %#x vs %llx fs %llx | "
        "poly offset enables %#x factor %g units %g clamp %g | depth range %g..%g | depth test %d write %d func %d | "
        "discard %d | depth mode %u | frame %llu\n",
        d ? "yes" : "no", d ? d->format : 0, d ? d->width : 0, d ? d->height : 0, d ? d->depth : 0, d ? d->target : 0,
        c0 ? "yes" : "no", c0 ? c0->format : 0, st.rt.ncolor, st.program->stage_mask, (unsigned long long)vs,
        (unsigned long long)fs, st.poly[3], st.poly_offset[0], st.poly_offset[1], st.poly_offset[2], st.depth_range[0][0],
        st.depth_range[0][1], st.ds[0], st.ds[1], st.ds[2], st.discard, nvn_depth_mode(),
        (unsigned long long)renderer_present_count());
}
static void shadow_log_skip(const char *why, const char *detail) {
    if (!shadow_sig || !shadow_new(shadow_sig ^ 0x5bd1e995ull)) return;
    LOG("[shadow]   ^ skipped: %s%s%s\n", why, detail && *detail ? ": " : "", detail ? detail : "");
}

static uint64_t prof_tp_;
static void draw_(CmdRec *r, bool indexed) {
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { stat_skipped++; return; }
    shadow_log_draw();
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    // Ark: transform feedback. A capturing draw runs only its vertex shader (rasterizer discard), in a pass of its own
    // (xfb_begin_pass), its outputs stored to the bound buffers; with discard on and nothing captured, nothing is drawn.
    bool xfb_draw = st.xfb_active && (st.program->stage_mask & 1) && xfb_program(st.program->stages[0].control);
    if (st.discard && !xfb_draw) { stat_skipped++; shadow_log_skip("rasterizer discard", NULL); return; }
    if (xfb_draw) {
        vkey.xfb = 1;
        memset(fkey.rt_kind, 0, sizeof fkey.rt_kind);
        if (!st.discard || indexed) {
            static int warned;
            if (!warned++) LOG("[xfb] captured draw with%s rasterizer discard, %s: only the capture is done\n", st.discard ? "" : "out",
                               indexed ? "indexed (vertex order assumed)" : "arrays");
        }
    }
    // integer-format textures bound to each stage's handle slots (ShaderKey.tex_uint / tex_sint: usampler / isampler)
    if (st.tpool) {
        for (int sg = 0; sg < 2; sg++) {
            ShaderKey *k = sg ? &fkey : &vkey;
            for (int hs = 0; hs < 32; hs++) {
                uint64_t h = st.tex[sg][hs];
                uint32_t tic = h & 0xFFFFF;
                if (!h || tic >= (uint32_t)st.tpool->count) continue;
                const NvnTexEntry *e = &((const NvnTexEntry *)st.tpool->entries)[tic];
                if (!e->tex) continue;
                uint32_t f = e->has_view && (e->view.setmask & 4) ? e->view.format : e->tex->format;
                int ik = renderer_format_is_depth(e->tex->format) ? 0 : rt_kind(f);
                if (ik == 2) k->tex_uint |= 1u << hs;
                else if (ik == 1) k->tex_sint |= 1u << hs;
            }
        }
    }
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (!pr || atomic_load(&pr->state) != 1) {
        stat_skipped++;
        if (pr && atomic_load(&pr->state) == 2) stat_skip_failed++; else stat_skip_prog++;
        shadow_log_skip(!pr ? "no program (stages not drawn)" : atomic_load(&pr->state) == 2 ? "program failed" : "program not ready",
                        pr && atomic_load(&pr->state) == 2 && pr->vi ? pr->vi->error : NULL);
        return;
    }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stage_mask & 2 ? st.program->stages[1].code_addr : 0;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; return; }
        extern bool pot_level_active(void);
        if (last_hash == 0xa2177dbb72197bd9ull && pot_level_active()) { stat_skipped++; return; }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
        extern void widescreen_backdrop_seen(uint64_t hash);
        if (last_hash) widescreen_backdrop_seen(last_hash);
        extern void widescreen_scene_ubo(const float *u, uint64_t size);  // (the camera's view-projection)
        if (st.ubuf[0][5].addr) widescreen_scene_ubo((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size);
    }
    if (g_debug_frame) debug_draw(pr, r, indexed);
    if (xfb_draw) {
        xfb_begin_pass();
    } else {
        resolve_feedback(pr);
        snap_reason = "draw-depthwrite";
        if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
        snap_reason = "draw-colorwrite";
        for (int i = 0; i < st.rt.ncolor; i++)
            if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
        begin_pass();
    }
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) {
        stat_skipped++;
        stat_skip_mode++;
        shadow_log_skip("primitive mode not drawn", NULL);
        static uint32_t seen;
        if (mode < 32 && !(seen & (1u << mode))) { seen |= 1u << mode; LOG("[vk] draws with primitive mode %u are not drawn\n", mode); }
        return;
    }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.ncolor = pass_ncolor;
    memcpy(k.color, pass_color, sizeof k.color);
    k.depth = pass_depth;
    k.stencil = pass_stencil;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    k.a2c = st.ms[0xC] != 0;
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1;
    k.discard = xfb_draw;
    uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
    Pipe *pp = pipe_tab[pslot].pipe;
    if (!pp) {
        pp = calloc(1, sizeof *pp);
        pp->k = k;
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
        run_job(build_pipeline, pp);
    }
    if (atomic_load(&pp->state) != 1) { stat_skipped++; stat_skip_pipe++; shadow_log_skip("pipeline not ready / failed", NULL); return; }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    vkCmdSetDepthTestEnable(cb, dt);
    vkCmdSetDepthWriteEnable(cb, dt && st.ds[1]);
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("LARECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCmdSetDepthCompareOp(cb, dop);
    bool sten = st.ds[3] && pass_stencil;
    vkCmdSetStencilTestEnable(cb, sten);
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilOp(cb, face, stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                              compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF));
            vkCmdSetStencilCompareMask(cb, face, st.stencil_vmask[f] & 0xFF);
            vkCmdSetStencilWriteMask(cb, face, st.stencil_mask[f] & 0xFF);
            vkCmdSetStencilReference(cb, face, st.stencil_ref[f]);
        }
    }
    vkCmdSetBlendConstants(cb, st.blend_color);

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    vkCmdSetCullMode(cb, cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    vkCmdSetFrontFace(cb, front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cb, st.prim_restart && native && (mode == 3 || mode == 5));
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        vkCmdSetDepthBiasEnable(cb, on);
        vkCmdSetDepthBias(cb, f[1], f[2], f[0]);
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    {   // the Items page's cursor brackets while the cursor is on the companion card (runtime/hle/pets.c)
        extern bool pets_hide_item_cursor(void);
        extern bool texpatch_is_item_cursor(int id);
        if (pets_hide_item_cursor())
            for (int i = 0; i < pr->fi->ntex; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
                if (b && b->tp > 0 && texpatch_is_item_cursor(b->tp - 1)) { stat_skipped++; return; }
            }
    }
    float tint[12] = {0};
    {   // the alpha test (NVN fixed function; the shader discards: maxwell/translate.c). 8 = always: off
        static int no_at = -1;  // ARK_NO_ALPHA_TEST=1: off (comparison)
        if (no_at < 0) no_at = getenv("ARK_NO_ALPHA_TEST") && atoi(getenv("ARK_NO_ALPHA_TEST"));
        uint8_t af = st.color_state[2];
        if (!no_at && af >= 1 && af <= 7) tint[8] = af, tint[9] = st.alpha_ref;
    }
    if (pr->fi->uses_tint) {
        extern bool pot_tint(const float *m, float tint[8], uint64_t mesh);
        extern bool pot_glow_active(void);
        if (st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && pot_glow_active())
            pot_tint((const float *)(uintptr_t)st.ubuf[0][2].addr, tint, st.vbuf[0].addr);
        // the gold Escape Bottle (runtime/hle/escape.c): draws sampling the bottle's item icon / model map
        // and the HUD hearts (heart colour, Cosmetics tab)
        extern int texpatch_bottle_kind(int id);
        extern int texpatch_heart_kind(int id);
        extern void texpatch_heart_tint(int kind, float tint[8]);
        int bottle = 0, heart = 0;
        for (int i = 0; i < pr->fi->ntex; i++) {
            NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
            const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
            if (b && b->tp > 0 && !bottle) bottle = texpatch_bottle_kind(b->tp - 1);
            if (b && b->tp > 0 && !heart) heart = texpatch_heart_kind(b->tp - 1);
        }
        if (heart) texpatch_heart_tint(heart, tint);
        if (bottle) {
            extern void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]);
            int u = bottle == 3 ? 3 : 0;  // (the fairy model: its bone matrices, for where it stands - souls.c)
            escape_bottle_tint(bottle, (const float *)(uintptr_t)st.ubuf[0][u].addr, (uint32_t)st.ubuf[0][u].size, tint);
        }
    }
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
    uint32_t xfb_n = 0;  // vertices this draw captures
    if (xfb_draw) {
        memset(xfb_q, 0, sizeof xfb_q);
        xfb_n = (uint32_t)r->x[2];
        for (int b = 0; b < 4; b++) {
            if (!st.xfb_buf[b].addr || !pr->xfb.stride[b]) continue;
            uint64_t a = st.xfb_buf[b].addr + st.xfb_off[b];
            if (st.xfb_buf[b].size && st.xfb_off[b] + (uint64_t)xfb_n * pr->xfb.stride[b] > st.xfb_buf[b].size) {
                static int warned;
                if (warned++ < 4) LOG("[xfb] buffer %d overflow (offset %u + %u x %u > %llu): capture clipped\n", b, st.xfb_off[b], xfb_n,
                                      pr->xfb.stride[b], (unsigned long long)st.xfb_buf[b].size);
                uint64_t room = st.xfb_buf[b].size > st.xfb_off[b] ? st.xfb_buf[b].size - st.xfb_off[b] : 0;
                if (room / pr->xfb.stride[b] < xfb_n) xfb_n = (uint32_t)(room / pr->xfb.stride[b]);
            }
            xfb_q[b][0] = (uint32_t)a;
            xfb_q[b][1] = (uint32_t)(a >> 32);
        }
        xfb_q[4][0] = indexed ? 0 : (uint32_t)r->x[1];
    }
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // camera-attached effects sized for 16:9 are widened with the view (runtime/hle/widescreen.c)
        extern float widescreen_effect_stretch(uint64_t fs_hash);
        float k = widescreen_effect_stretch(g_cur_fs_hash);
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii);
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    // the global memory set only for programs that read or write guest memory: binding it marks every chunk as used
    // (MoltenVK: one useResource per chunk per bind), which with 128 chunks halved the frame rate in the rain
    bool gm = pr->vi->uses_gmem || pr->fi->uses_gmem;
    VkDescriptorSet sets[3] = {vset, fset, gm ? gmem_set() : VK_NULL_HANDLE};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, gm ? 3 : 2, sets, 0, NULL);

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
                else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index) vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && guest_direct(v.addr, v.size, &dbuf, &doff)) {
                vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) { const uint16_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != (uint16_t)scan_restart) max_index = q[i]; }
                else { const uint32_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != scan_restart) max_index = q[i]; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = k.stride[s], need;
            if (!stride) need = 256;
            else if (k.divisor[s]) need = ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    // Instanced draws with item spots among their instances (bushsanity: bushes are drawn in batches, each instance's
    // model matrix in vertex storage buffer 0, indexed by instance id): drawn as runs of instances, gold runs tinted
    // (as renderer.m; gl_InstanceIndex includes the base instance, so a run [b + k, b + k + n) draws exactly those).
    enum { MAX_RUNS = 64 };
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];
    bool run_gold[MAX_RUNS];
    int nruns = 0;
    if (pr->fi->uses_tint && inst > 1 && st.sbuf[0][0].addr) {
        extern bool pot_glow_active(void);
        extern bool pot_instance_gold(const float *m);
        uint64_t recs = st.sbuf[0][0].size / 64;
        if (pot_glow_active() && (uint64_t)base_inst + inst <= recs) {
            const float *m = (const float *)(uintptr_t)st.sbuf[0][0].addr;
            for (uint32_t i = 0; i < inst; i++) {
                bool g = pot_instance_gold(m + (size_t)(base_inst + i) * 16);
                if (nruns && run_gold[nruns - 1] == g) { run_n[nruns - 1]++; continue; }
                if (nruns == MAX_RUNS) { nruns = 0; break; }  // (too fragmented: draw as is)
                run_at[nruns] = i; run_n[nruns] = 1; run_gold[nruns] = g; nruns++;
            }
            if (nruns == 1 && !run_gold[0]) nruns = 0;
        }
    }
    if (!nruns) { run_at[0] = 0; run_n[0] = inst; nruns = 1; run_gold[0] = false; }
    for (int ri = 0; ri < nruns; ri++) {
        if (nruns > 1 || run_gold[0]) {  // this run's tint: a fragment set of its own
            float rtint[12] = {0};
            rtint[8] = tint[8], rtint[9] = tint[9];
            extern void pot_gold_tint(float tint[8]);
            if (run_gold[ri]) pot_gold_tint(rtint);
            VkDescriptorSet rset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, rtint, 0, 0, 0, fii);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 1, 1, &rset, 0, NULL);
        }
        if (!use_index) vkCmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);
        else vkCmdDrawIndexed(cb, icount, run_n[ri], 0, base_vertex, base_inst + run_at[ri]);
    }
    if (xfb_draw) {
        for (int b = 0; b < 4; b++) st.xfb_off[b] += xfb_n * pr->xfb.stride[b];
        st.xfb_verts += xfb_n;
    }
    if (g_debug_frame && getenv("LARECOMP_SPLIT_VS")) {  // debug: a pass image after each draw with these vertex shaders
        char hexvs[24];
        snprintf(hexvs, sizeof hexvs, "%llx", (unsigned long long)st.program->stages[0].code_addr);
        if (strstr(getenv("LARECOMP_SPLIT_VS"), hexvs)) {
            LOG("[split] pass image %d: after vs %s count %llu inst %llu\n", debug_pass_no, hexvs,
                (unsigned long long)r->x[2], (unsigned long long)(indexed ? r->x[6] : r->x[4]));
            st.rt_dirty = true;
            vk_end_pass();
        }
    }
    stat_draws++;
    cum_draws++;
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    // (F9) the GLSL of every program drawn in a debug frame, once each: build/glsl-f9/<vs>_<fs>.{vert,frag}
    {
        static uint64_t saved[512];
        static int nsaved;
        uint64_t key = st.program->stages[0].code_addr * 31 + st.program->stages[1].code_addr;
        bool have = false;
        for (int i = 0; i < nsaved; i++) have |= saved[i] == key;
        if (!have && nsaved < 512) {
            saved[nsaved++] = key;
            mkdir("build/glsl-f9", 0755);
            char prefix[96];
            snprintf(prefix, sizeof prefix, "build/glsl-f9/%llx_%llx", (unsigned long long)st.program->stages[0].code_addr,
                     (unsigned long long)st.program->stages[1].code_addr);
            vk_save_prog_glsl(pr, prefix);
        }
    }
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    if (!(st.color_state[0] & 1) && st.color_state[2] != 8)  // (blending off: the alpha test still applies)
        LOG("[draw]   (no blending) alpha test %u ref %g\n", st.color_state[2], st.alpha_ref);
    if (st.color_state[0] & 1) {  // blend target 0: NVN blend state bytes (target, src/dst colour, src/dst alpha factors, ops)
        const uint8_t *bs = st.blend[0];
        LOG("[draw]   blend0 bytes %02x %02x %02x %02x %02x %02x %02x %02x | blend colour %g %g %g %g | alpha test %u ref %g\n",
            bs[0], bs[1], bs[2], bs[3], bs[4], bs[5], bs[6], bs[7], st.blend_color[0], st.blend_color[1], st.blend_color[2],
            st.blend_color[3], st.color_state[2], st.alpha_ref);
    }
    if (st.ds[3]) {  // stencil test: ops (fail, zfail, zpass) and func per face, ref, write and compare masks
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        LOG("[draw]   stencil%s front ops %x/%x/%x func %x ref %#x wmask %#x cmask %#x | back ops %x/%x/%x func %x ref %#x wmask %#x"
            " cmask %#x\n", pass_stencil ? "" : " (no stencil attachment)", ops & 0xF, (ops >> 8) & 0xF, (ops >> 4) & 0xF,
            st.ds[4] & 0xF, st.stencil_ref[0], st.stencil_mask[0], st.stencil_vmask[0], (ops >> 12) & 0xF, (ops >> 20) & 0xF,
            (ops >> 16) & 0xF, st.ds[4] >> 4, st.stencil_ref[1], st.stencil_mask[1], st.stencil_vmask[1]);
    }
    if (getenv("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        // cbuf_mask bits are hardware slots (the GLSL's cb<N>); the game's uniform binding is N - 3 (cbuf_info)
        for (int stg = 1; stg >= 0; stg--) {  // fragment, then vertex (particle emitters keep per-frame data there)
            uint32_t mask = stg ? pr->fi->cbuf_mask : pr->vi->cbuf_mask;
            for (int c = 3; c < 21; c++) {
                const Range *rg = &st.ubuf[stg][c - 3];
                if (!((mask >> c) & 1) || !rg->addr) continue;
                const float *u = (const float *)(uintptr_t)rg->addr;
                LOG("[draw]   %ccb%d (binding %d) %llx size %u:", stg ? 'f' : 'v', c, c - 3, (unsigned long long)rg->addr,
                    (unsigned)rg->size);
                for (int k = 0; k < 192 && k * 4 < (int)rg->size; k++) LOG(" %.6g", u[k]);  // (768 bytes: UI tables)
                LOG("\n");
            }
        }
    }
    {   // LARECOMP_WATCH_VS=<vertex shader address> or inst<N>: for the first draw with it, log its vertex cbufs and watch the start
        // of its vertex streams and cbufs (names the code that fills per-instance data: [watch] lines)
        static int done;
        const char *wv = getenv("LARECOMP_WATCH_VS");
        // (=inst<N>: the first draw with N instances instead: effect shaders load at a different address each run)
        bool match = wv && (!strncmp(wv, "inst", 4) ? (uint64_t)strtoull(wv + 4, NULL, 10) == (indexed ? r->x[6] : r->x[4])
                                                   : st.program->stages[0].code_addr == strtoull(wv, NULL, 16));
        if (match && !done) {
            done = 1;
            extern void watch_add(uint64_t, uint64_t), watch_arm(void);
            // (vertex streams only with LARECOMP_WATCH_VBUF=1: the GPU reads vertex data in place from imported guest
            // memory, and a GPU read of a protected page stalls it for good: the game hangs)
            for (int s = 0; s < 4 && getenv("LARECOMP_WATCH_VBUF"); s++) if (st.vbuf[s].addr) {
                LOG("[draw]   watch vbuf%d %llx\n", s, (unsigned long long)st.vbuf[s].addr);
                watch_add(st.vbuf[s].addr, st.vbuf[s].addr + 64);
            }
            LOG("[draw]   watch: vs=%llx\n", (unsigned long long)st.program->stages[0].code_addr);
            for (int hc = 3; hc < 21; hc++) {
                int cb = hc - 3;  // (binding; the mask is in hardware slots)
                if (!((pr->vi->cbuf_mask >> hc) & 1) || !st.ubuf[0][cb].addr) continue;
                const float *u = (const float *)(uintptr_t)st.ubuf[0][cb].addr;
                LOG("[draw]   vcb%d (binding %d) %llx size %u:", hc, cb, (unsigned long long)st.ubuf[0][cb].addr, (unsigned)st.ubuf[0][cb].size);
                for (int k = 0; k < 64 && k * 4 < (int)st.ubuf[0][cb].size; k++) LOG(" %.6g", u[k]);
                LOG("\n");
                // (not the bound range itself: the render thread copies it every frame (cbuf_info), and a render-thread
                // fault on a watched page deadlocks the signal handler: only the copies found below are watched)
                // the same 32 bytes elsewhere in the game's memory: where it was copied from (watched too)
                extern int kernel_phys_regions(uint64_t *, uint64_t *, int, uint64_t *);
                uint64_t ra[256], rs[256], gen;
                int nr = kernel_phys_regions(ra, rs, 256, &gen);
                const uint8_t *pat = (const uint8_t *)(uintptr_t)st.ubuf[0][cb].addr + 16;  // (skip a header-like first entry)
                int hits = 0;
                for (int r = 0; r < nr && hits < 4; r++)
                    for (uint64_t o = 0; o + 32 <= rs[r] && hits < 4; o += 4) {
                        uint64_t a = ra[r] + o;
                        if (a == (uint64_t)(uintptr_t)pat || memcmp((const void *)(uintptr_t)a, pat, 32)) continue;
                        LOG("[draw]   vcb%d data also at %llx\n", cb, (unsigned long long)a);
                        watch_add(a, a + 32);
                        hits++;
                    }
            }
            watch_arm();
        }
    }
    for (int i = 0; i < pr->vi->ntex && i < 64; i++) {  // vertex textures (heightmaps, wind)
        NvnTexture *t = shader_texture_object(0, &pr->vi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   vtex%d: none\n", i); continue; }
        LOG("[draw]   vtex%d: %p %ux%ux%u f%#x target %u lv %u addr %llx", i, (void *)t, t->width, t->height, t->depth, t->format,
            t->target, b ? b->levels : 0, (unsigned long long)t->addr);
        if (t->addr && t->format && getenv("LARECOMP_DEBUG_DATA")) {  // the first texels as words
            LOG(" |");
            for (int k = 0; k < 8; k++) LOG(" %08x", ((const uint32_t *)(uintptr_t)t->addr)[k]);
        }
        LOG("\n");
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        debug_sampler(1, &pr->fi->tex[i]);
        uint64_t nz = 0, lo = ~0ull, hi = 0;  // guest memory: non-zero bytes (CPU-written contents?) and where
        if (t->addr) for (uint64_t k = 0; k < (16u << 20) && k < (uint64_t)t->width * t->height; k++)
            if (((const uint8_t *)(uintptr_t)t->addr)[k]) { nz++; if (k < lo) lo = k; hi = k; }
        if (nz) LOG("[draw]   (stride %llu, non-zero bytes %llx..%llx)\n", (unsigned long long)t->stride, (unsigned long long)lo, (unsigned long long)hi);
        if (t->addr && getenv("LARECOMP_DEBUG_DATA")) {  // the texture's own bytes (tegra_total_size): 0xFF count, first 16
            uint64_t tot = tegra_total_size(&t->layout), ff = 0;
            const uint8_t *m = (const uint8_t *)(uintptr_t)t->addr;
            for (uint64_t k = 0; k < tot && k < (16u << 20); k++) ff += m[k] == 0xFF;
            LOG("[draw]   (data %llu bytes, %llu are 0xFF, first:", (unsigned long long)tot, (unsigned long long)ff);
            for (int k = 0; k < 16 && (uint64_t)k < tot; k++) LOG(" %02x", m[k]);
            LOG(")\n");
        }
        {   // LARECOMP_DUMP_TEX=<w>x<h>: the raw guest bytes of matching textures, once per frame each, for offline
            // decoding: build/texdump_<addr>_<w>x<h>_f<format>_bh<block height log2>.bin
            static int dw = -1, dh;
            if (dw < 0) { dw = 0; const char *e = getenv("LARECOMP_DUMP_TEX"); if (e) sscanf(e, "%dx%d", &dw, &dh); }
            if (dw && t->addr && (int)t->width == dw && (int)t->height == dh) {
                char path[160];
                snprintf(path, sizeof path, "build/texdump_%llx_%ux%u_f%x_bh%u.bin", (unsigned long long)t->addr, t->width,
                         t->height, t->format, t->layout.block_height_log2);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite((const void *)(uintptr_t)t->addr, 1, tegra_total_size(&t->layout), f); fclose(f); }
                LOG("[draw]   (dumped %s)\n", path);
                // and the GPU image's level 0 as the GPU holds it (raw blocks / texels, linear), read back at the next
                // present on its own (vk_debug_readbacks): build/texgpu_<addr>_..._vkfmt<n>.bin
                if (b && b->image && nrb < 4) {
                    rb[nrb].t = t;
                    snprintf(rb[nrb].path, sizeof rb[nrb].path, "build/texgpu_%llx_%ux%u_vkfmt%d_lv%u.bin",
                             (unsigned long long)t->addr, b->width, b->height, (int)b->format, b->levels);
                    nrb++;
                    LOG("[draw]   (GPU image to be read back to %s at the next present)\n", rb[nrb - 1].path);
                }
            }
        }
        if (nz && b && b->format == VK_FORMAT_R8_UNORM && b->width >= 1024 && getenv("LARECOMP_DUMP_R8")) {
            static int nd;
            char path[64];
            snprintf(path, sizeof path, "build/r8_%02d.pgm", nd++);
            debug_save_r8((TexBackend *)b, path);
            FILE *f = fopen("build/r8_guest.pgm", "wb");  // and the guest memory it came from (linear, stride)
            if (f) {
                fprintf(f, "P5 %u %u 255\n", t->width, t->height);
                for (uint32_t y = 0; y < t->height; y++) fwrite((const uint8_t *)(uintptr_t)t->addr + y * (t->stride ? t->stride : t->width), 1, t->width, f);
                fclose(f);
            }
        }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s%s%s flags %#x addr %llx nz %llu\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "",
            b && b->gpu_owned ? " gpu" : "", b && b->dynamic ? " dyn" : "", t->flags, (unsigned long long)t->addr, (unsigned long long)nz);
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

// (debug frames) both ends of a depth copy, to see whether the copy carried the current frame's depth
static void debug_copy_depth(TexBackend *s, TexBackend *d) {
    if (!g_debug_frame || !(s->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    static int n;
    char path[96];
    snprintf(path, sizeof path, "build/fbdepth_copy_%03d_src.pgm", n);
    debug_save_depth(s, path);
    snprintf(path, sizeof path, "build/fbdepth_copy_%03d_dst.pgm", n++);
    debug_save_depth(d, path);
}
static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame)
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        debug_copy_depth(s, d);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    debug_copy_depth(s, d);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].fs == fs) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[5];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    if (!smaa && !fsr) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr;
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("LARECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_ALPHA_REF: st.alpha_ref = r->f[0]; break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (getenv("LARECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x\n", (unsigned long long)renderer_present_count(), index,
                    st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2]);
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        // (not implemented: dropped. F9 logs them so a missing effect can be traced to one)
        case CMD_DISPATCH_INDIRECT: case CMD_DRAW_ARRAYS_INDIRECT: case CMD_DRAW_ELEMENTS_INDIRECT: {
            static int nwarn;
            if (g_debug_frame || nwarn++ < 4)
                LOG("[%s] INDIRECT dropped (indirect buffer %llx, %llx)\n", r->op == CMD_DISPATCH_INDIRECT ? "dispatch" : "draw",
                    (unsigned long long)r->x[0], (unsigned long long)r->x[1]);
            break;
        }
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_BIND_XFB_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 4; i++) memcpy(&st.xfb_buf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_RASTERIZER_DISCARD: st.discard = r->x[0] != 0; break;
        case CMD_XFB_CONTROL: {  // control buffer (ours): u32 bytes written per buffer [0..3], u32 vertices captured [4]
            uint32_t *ctl = (uint32_t *)(uintptr_t)r->x[0];
            static int logged;
            if (logged < 8) {
                logged++;
                LOG("[xfb] %s (control %llx) buffers %llx+%llu %llx+%llu %llx+%llu, discard %d\n",
                    r->x[6] == 0 ? "begin" : r->x[6] == 1 ? "end" : r->x[6] == 2 ? "pause" : "resume", (unsigned long long)r->x[0],
                    (unsigned long long)st.xfb_buf[0].addr, (unsigned long long)st.xfb_buf[0].size, (unsigned long long)st.xfb_buf[1].addr,
                    (unsigned long long)st.xfb_buf[1].size, (unsigned long long)st.xfb_buf[2].addr, (unsigned long long)st.xfb_buf[2].size,
                    st.discard);
            }
            if (r->x[6] == 0 || r->x[6] == 3) {  // begin (from offset 0) / resume (from the control buffer)
                st.xfb_active = true;
                if (r->x[6] == 3 && ctl) { for (int b = 0; b < 4; b++) st.xfb_off[b] = ctl[b]; st.xfb_verts = ctl[4]; }
                else { memset(st.xfb_off, 0, sizeof st.xfb_off); st.xfb_verts = 0; }
            } else {  // end / pause: where each buffer got to
                st.xfb_active = false;
                if (ctl) { for (int b = 0; b < 4; b++) ctl[b] = st.xfb_off[b]; ctl[4] = st.xfb_verts; }
            }
            break;
        }
        case CMD_DRAW_XFB: {  // (mode, control buffer): the vertices a capture produced
            const uint32_t *ctl = (const uint32_t *)(uintptr_t)r->x[1];
            uint64_t dbuf[16] = {0};
            CmdRec *d = (CmdRec *)dbuf;
            d->op = CMD_DRAW_ARRAYS;
            d->size = sizeof dbuf;
            d->x[0] = r->x[0];
            d->x[2] = ctl ? ctl[4] : 0;
            d->x[4] = 1;
            if (d->x[2]) draw(d, false);
            break;
        }
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow;
            // the value is our translation time, not GPU time. LARECOMP_VK_COUNTER=frozen: the same timestamp for
            // every counter within a presented frame (experiment).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            static int frozen = -1;
            if (frozen < 0) { const char *e = getenv("LARECOMP_VK_COUNTER"); frozen = e && !strcmp(e, "frozen"); }
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (frozen && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = SDL_GetTicksNS(); }
            if (out) { out[0] = 0; out[1] = frozen ? frame_ts : SDL_GetTicksNS(); }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += r->size;
    }
}

void backend_execute(uint64_t handle) {
    PROF_T0();
    exec_list(handle, 0);
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit();
}
