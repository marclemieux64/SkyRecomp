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
#ifdef _WIN32
#include <windows.h>
#endif

#include "../../kernel.h"
#include "shaders.h"
#include "vk.h"
#include "../../vr/vr_fade.h"
#include "../renderer.h"
#include "../../settings.h"
#include "../../frame_rate.h"
#include "../../vr/vr.h"
#include "../../vr/vr_rig.h"
#include "multiview.h"
#include "../paint_filter.h"
#include "../output_scale.h"

static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
static uint64_t stat_descriptor_hits, stat_descriptor_misses;
static uint64_t stat_vr_shared_lists;
static uint64_t stat_mv_passes, stat_mv_draws, stat_mv_skipped;  // (VR multiview, begin_pass)
static bool g_stereo_camera_patched;
static uint64_t g_last_scene_frame;  // (the HUD layer, below: the last frame that drew a 3D scene)
static bool g_scene_since_present;
static NvnTexture *g_black_clear_target;
static uint64_t g_black_clear_frame;
// Scene depth and projection/decal passes are eye-dependent. A native,
// depth-only offscreen shadow map is light-space data and can be shared.
static bool g_stereo_shadow_or_depth;
static uint64_t stat_upload_bytes;
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
// VR: eye 1's share of the above (its list replay; LARECOMP_VK_PROF)
static uint64_t prof_e1_exec, prof_e1_draw, prof_e1_sets, prof_e1_vtx, prof_e1_pre, prof_e1_key, prof_e1_dyn, prof_e1_tex, prof_e1_draws;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits, cum_passes;  // (profiling) never reset: hitch log
static uint32_t prof_end_line[8192];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
static uint64_t stat_tex_deferred;  // (draws skipped for a texture left to a later frame)
static uint64_t op_counts[CMD_COUNT];
static uint64_t stat_vrs_passes;  // (VR foveated passes, begin_pass)
static uint64_t stat_hidden_masks;  // (VR hidden-area masks drawn, hidden_mask)
static uint64_t stat_bounds_passes;  // (VR passes rejecting the hidden area by depth bounds, begin_pass)
// SKYRECOMP_VR_HIDDEN_MASK (opt-in): 1 depth prefill + depth bounds everywhere, 2 + paint red, 3 prefill only.
// Measured in Skyloft at 1.5x: -0.35 ms of ~10.3, and the game's blur passes (bloom, haze) then read stale pixels
// from the hidden area into the visible edge (a hazy band) - so it stays off.
static bool hidden_bounds_on(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("SKYRECOMP_VR_HIDDEN_MASK"); m = e ? atoi(e) : 0; }
    return m == 1 || m == 2;
}
static uint64_t op_ns[CMD_COUNT][2];  // (LARECOMP_VK_PROF) time per command type and eye, nested lists excluded
bool g_debug_frame;
static char stats_text[4096];
void vk_collect_dead(void);
uint64_t renderer_present_count(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    bool vr_ts, vr_frame_end;
    uint64_t vr_frame;
    uint64_t seq;
    VkDescriptorPool pools[64];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static VkQueryPool vr_qp;
static int cur_sub;
static void ss_forget(void);
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;
// ---- barriers between passes (begin_pass). A full barrier used to start every pass. Now: after any work outside a
// pass (copies, uploads, dispatches, clears of images, presents: every vk_end_pass not made by begin_pass itself)
// the next pass still gets one; between two passes with nothing in between, only what the new pass needs - none,
// or an attachment barrier when it draws on to an image a pass since the last full barrier rendered; and a draw that
// samples such an image gets the full barrier first (pass_sample_hazard). LARECOMP_VK_FULL_BARRIERS=1: the old way.
static bool g_outside_work = true;  // work outside passes since the last full barrier at a pass start
static NvnTexture *g_written[32];   // attachments of the passes since that barrier
static int g_nwritten;
static uint64_t stat_barrier_full, stat_barrier_att, stat_barrier_none, stat_barrier_hazard;

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

// (LARECOMP_VK_PROF) waits on the GPU by cause: the caller names it in wait_why just before waiting
static __thread const char *wait_why;
static struct { const char *why; uint64_t ns, n; } wait_tab[12];
static pthread_mutex_t wait_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static void wait_account(uint64_t ns) {
    const char *why = wait_why ? wait_why : "other";
    wait_why = NULL;
    pthread_mutex_lock(&wait_tab_lock);
    for (int i = 0; i < 12; i++)
        if (!wait_tab[i].why || wait_tab[i].why == why) { wait_tab[i].why = why; wait_tab[i].ns += ns; wait_tab[i].n++; break; }
    pthread_mutex_unlock(&wait_tab_lock);
}

static void cq_resolve_done(uint64_t done);

void vk_wait_seq(uint64_t seq) {
    if (!seq || IsVRExitRequested()) { wait_why = NULL; return; }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = VK_TIMEOUT;
    for (int waits = 0; waits < 10000 && !IsVRExitRequested() && r == VK_TIMEOUT; ++waits)
        r = vkWaitSemaphores(g_vk.dev, &wi, 1000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    wait_account(SDL_GetTicksNS() - t0);
    if (IsVRExitRequested()) return;
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r == VK_SUCCESS) cq_resolve_done(seq);  // (the game reads its timing counters after waiting)
    else {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }
uint64_t vk_cum_wait_ns(void) { return cum_wait; }
static uint64_t g_list_ns, g_lists;  // (always counted, cheap: vr.c's per-frame pacing trace)
void vk_cum_lists(uint64_t *ns, uint64_t *lists, uint64_t *draws, uint64_t *passes) {
    *ns = g_list_ns; *lists = g_lists; *draws = cum_draws; *passes = cum_passes;
}  // (vr.c pacing trace: time the renderer waited for the GPU)

// ---- the game's GPU timing counters (nvnCommandBufferReportCounter, two per frame)
// The game times its GPU work with these and drops to 30 fps when frames look slow. They used to get the host clock
// at translation time, so our command translation counted as GPU time: busy scenes (outdoors, ~400 draws) looked slow
// and the game locked itself at 30 even on fast GPUs, at any resolution. Now each counter also records a GPU
// timestamp at its place in the command stream; once that submission is done (the game waits on its fence before
// reading - on the Switch the GPU writes them too) the GPU time replaces the provisional host value, in the host
// clock's terms (offset calibrated at each resolve: a timestamp can't be later than the moment its submission is seen
// done). LARECOMP_VK_COUNTER=cpu: host clock only; =frozen: one value per presented frame (test); =legacy: the host
// clock in ns instead of GPU ticks, as before (test: reproduces the locked 30).
#define CQ_MAX 64
typedef struct { VkQueryPool pool; uint32_t n; uint64_t seq; uint64_t *out[CQ_MAX]; } CounterQ;
static CounterQ cq[SUBS];
static pthread_mutex_t cq_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t cq_offset;  // host ns - GPU ns (the smallest seen)
// Counter timestamps are in the Switch GPU's ticks (614.4 MHz): the game converts the difference to ns itself
// (x 625 / 384). Writing ns made every GPU time look 1.63x as long.
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * 384 / 625); }
static bool cq_calibrated;
static int counter_mode = -1;  // 0 cpu, 1 frozen, 2 gpu
static uint64_t stat_counters_gpu;

static int vk_counter_mode(void) {
    if (counter_mode < 0) {
        const char *e = getenv("LARECOMP_VK_COUNTER");
        counter_mode = e && !strcmp(e, "cpu") ? 0 : e && !strcmp(e, "frozen") ? 1 : e && !strcmp(e, "legacy") ? 3 : 2;
        if (counter_mode == 2 && !g_vk.props.limits.timestampComputeAndGraphics) counter_mode = 0;
    }
    return counter_mode;
}

// ---- frame pacing: the game's own 30 fps mode when we can't hold 60
// The game advances its world one fixed step per frame in its 60 fps mode, and two steps per frame in its 30 fps mode
// (measured: Link walks twice as far per frame there) - so a 60-mode frame that takes 20 ms plays in slow motion, while
// the 30 mode plays at full speed. The game picks its mode from these counters (GPU time per frame), as on the Switch;
// but our frames can be slow for reasons its GPU time doesn't show (command translation, a slower CPU). So each
// presented frame's busy time is measured (present to present, minus the time the presenting thread was blocked in
// kernel waits and sleeps: idle - the game waits for vsync through its own threads); when
// frames don't fit 60 (12 of the last 60 over 16.2 ms), the counters report a frame of at least PACE_SLOW_NS: the game
// takes its 30 fps mode, at full speed. Back to the real times after 2 s with headroom (busy under 13 ms on average,
// at most 2 of the last 60 frames over). The game itself also tries 60 again now and then.
// LARECOMP_PACE=0: off.
#define PACE_SLOW_NS 20000000ull
extern __thread uint64_t g_blocked_ns;  // kernel.c: this thread's time blocked in waits / sleeps
static bool pace_slow;
static uint64_t stat_pace_over;

void vk_pace_frame(void) {
    if (IsVRExitRequested()) return;
    if (!rt_headset_paced()) {
        extern void rt_present_pace(void);
        rt_present_pace();  // (runtime/hle/framepace.c: the game's swap interval, shared with metal.m)
    }
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_PACE"); on = e && atoi(e) != 0; }
    static uint64_t last, last_wait;
    static uint8_t over[60];
    static int idx, nover, calm;
    static double avg;  // busy ms, moving average (~20 frames)
    uint64_t now = plat_now_ns(), w = g_blocked_ns;
    uint64_t p = now - last, wt = w - last_wait;
    bool first = !last;
    last = now, last_wait = w;
    if (first || p > 100000000ull) return;  // (loading, a hitch: not a pace)
    double busy = (p > wt ? p - wt : 0) / 1e6;
    avg = avg ? avg + (busy - avg) * 0.05 : busy;
    static int log = -1;  // LARECOMP_LOG_PACE=1: each frame's period, vsync wait and busy time
    if (log < 0) log = getenv("LARECOMP_LOG_PACE") != NULL;
    if (log) LOG("[pace] period %.2f blocked %.2f busy %.2f ms\n", p / 1e6, wt / 1e6, busy);
    int o = busy > 16.2;
    stat_pace_over += o;
    nover += o - over[idx];
    over[idx] = (uint8_t)o;
    idx = (idx + 1) % 60;
    if (!on) return;  // (measured and counted all the same: LARECOMP_LOG_PACE, the stats)
    if (!pace_slow && nover >= 12) {
        pace_slow = true, calm = 0;
        LOG("[pace] frames don't fit 60 fps (%d of the last 60 over 16.2 ms busy): the game's 30 fps mode\n", nover);
    } else if (pace_slow) {
        // headroom: busy well under a 60 fps frame on average, hardly a frame over (single spikes are normal play)
        calm = avg < 13.0 && nover <= 2 ? calm + 1 : 0;
        if (calm >= 120) {
            pace_slow = false, nover = 0;
            memset(over, 0, sizeof over);
            LOG("[pace] 2 s with headroom: back to the game's own choice\n");
        }
    }
}

// a pair's end counter (the 16 bytes after its start) is at least PACE_SLOW_NS after it while pace_slow
static uint64_t pace_end_counter(const uint64_t *start_entry, const uint64_t *entry, uint64_t value) {
    if (!pace_slow || !start_entry || (const uint8_t *)entry - (const uint8_t *)start_entry != 16) return value;
    uint64_t min = start_entry[1] + ns_to_gpu_ticks(PACE_SLOW_NS);
    return value < min ? min : value;
}

static void cq_resolve(int i) {  // (cq_lock held) submission slot i is done: its counters get their GPU times
    CounterQ *q = &cq[i];
    if (!q->n) return;
    uint64_t t[CQ_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, q->pool, 0, q->n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod;
        int64_t cand = (int64_t)plat_now_ns() - (int64_t)((double)t[q->n - 1] * per);
        if (!cq_calibrated || cand < cq_offset) cq_offset = cand, cq_calibrated = true;
        static int log = -1;  // LARECOMP_LOG_COUNTER=1: the host (provisional) and GPU times of each counter
        if (log < 0) log = getenv("LARECOMP_LOG_COUNTER") != NULL;
        for (uint32_t k = 0; k < q->n; k++) {
            uint64_t gpu = ns_to_gpu_ticks((uint64_t)((int64_t)((double)t[k] * per) + cq_offset));
            if (log) LOG("[counter] seq %llu #%u at %llx host %.3f gpu %.3f ms\n", (unsigned long long)q->seq, k,
                         (unsigned long long)(uintptr_t)q->out[k], q->out[k][1] * 625.0 / 384 / 1e6, gpu * 625.0 / 384 / 1e6);
            q->out[k][1] = pace_end_counter(k ? q->out[k - 1] : NULL, q->out[k], gpu);
        }
        stat_counters_gpu += q->n;
    }
    q->n = 0;
}

static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done
    pthread_mutex_lock(&cq_lock);
    for (int i = 0; i < SUBS; i++) if (cq[i].n && cq[i].seq <= done) cq_resolve(i);
    pthread_mutex_unlock(&cq_lock);
}

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        wait_why = "ring slot";
        vk_wait_seq(s->seq);
        cq_resolve_done(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            s->ts = false;
        }
        if (s->vr_ts) {
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, vr_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8,
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
                static uint64_t measured_frame, measured_ns;
                const uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
                if (measured_frame != s->vr_frame) measured_frame = s->vr_frame, measured_ns = 0;
                measured_ns += ns;
                if (s->vr_frame_end) {
                    g_vr_gpu_ms = (float)((double)measured_ns / 1.0e6);
                    measured_ns = 0;
                }
            }
            s->vr_ts = s->vr_frame_end = false;
            s->vr_frame = 0;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void vr_timestamp_begin(Sub *s, VkCommandBuffer cb) {
    if (!vr_qp || s->vr_ts || !vr_stereo_active()) return;
    s->vr_ts = true;
    s->vr_frame = renderer_present_count() + 1;
    vkCmdResetQueryPool(cb, vr_qp, (uint32_t)cur_sub * 2, 2);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_NONE, vr_qp, (uint32_t)cur_sub * 2);
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
        vr_timestamp_begin(s, s->main);
        ss_forget();  // (a new command buffer starts with no state)
        if (vk_counter_mode() == 2) {  // this slot's timing counter queries (reset outside any render pass)
            CounterQ *q = &cq[cur_sub];
            if (!q->pool) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                            .queryCount = CQ_MAX};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &q->pool) != VK_SUCCESS) q->pool = VK_NULL_HANDLE;
            }
            pthread_mutex_lock(&cq_lock);
            q->n = 0;  // (resolved when the slot was freed: sub())
            q->seq = s->seq;
            pthread_mutex_unlock(&cq_lock);
            if (q->pool) vkCmdResetQueryPool(s->main, q->pool, 0, CQ_MAX);
        }
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
    if (!s->upload_begun) {
        begin_cb(&s->upload, &s->upload_begun);
        vr_timestamp_begin(s, s->upload);
        // after everything submitted before it: a submission's commands may start while the previous one's still run,
        // and these copies overwrite what those may read (textures being replaced, vertex cache pages)
        vk_full_barrier(s->upload);
    }
    return s->upload;
}

// Profiler labels (SKYRECOMP_GPU_MARKERS=1, device.c): named ranges in Radeon GPU Profiler / RenderDoc timelines.
void vk_label_begin(VkCommandBuffer cb, const char *name) {
    if (!g_vk.label_begin) return;
    VkDebugUtilsLabelEXT l = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = name};
    g_vk.label_begin(cb, &l);
}
void vk_label_end(VkCommandBuffer cb) {
    if (g_vk.label_end) g_vk.label_end(cb);
}
static bool pass_labelled;  // (the current pass opened a label: closed with it, vk_end_pass_at)

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);
void vc_flush(void);

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
    vc_flush();
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    if (s->vr_ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, vr_qp, (uint32_t)cur_sub * 2 + 1);
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

void vk_vr_gpu_frame_end(void) {
    const int previous = (cur_sub + SUBS - 1) % SUBS;
    if (subs[previous].vr_ts) subs[previous].vr_frame_end = true;
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }

void renderer_flush(void) {
    renderer_submit();
    if (!wait_why) wait_why = "flush";
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = "QueueFinish / sync all";
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
    wait_why = seq > s ? "SyncWait (fence not submitted yet)" : "SyncWait";
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 64) FATAL("descriptor pools exhausted (64 pools)\n");
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

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed.
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; uint32_t gen; } Chunk;
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
    if (getenv("LARECOMP_VK_RING_SYSMEM")) ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;  // (test: a GPU without resizable BAR)
    VmaAllocationInfo info;
    VkResult vr = vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info);
    if (vr != VK_SUCCESS) {
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    }
    c->ptr = info.pMappedData;
    c->size = size;
    if (!nchunks) {  // where the ring lives: device-local (resizable BAR) or system memory read across PCIe
        VkMemoryPropertyFlags f;
        vmaGetMemoryTypeProperties(g_vk.vma, info.memoryType, &f);
        LOG("[vk] transient ring: memory type %u (%s%s%s)\n", info.memoryType, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local " : "system memory ",
            f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "host-visible " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "cached" : "");
    }
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
    chunks[pick].gen++;  // (recycled: spans handed out before are gone - vk_transient_hold)
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// A span handed out earlier, to be read by the pending submission too (VR eye 1 reusing eye 0's copies after eye 0
// was submitted, exec_list_stereo): true while its chunk was not recycled since (`gen` as noted then), and the chunk is
// then kept until the pending submission is done.
static int transient_chunk(VkBuffer b) {
    for (int i = 0; i < nchunks; i++) if (chunks[i].buf == b) return i;
    return -1;
}
static uint32_t transient_gen(VkBuffer b) {
    const int i = transient_chunk(b);
    return i >= 0 ? chunks[i].gen : 0;
}
static bool vk_transient_hold(VkBuffer b, uint32_t gen) {
    const int i = transient_chunk(b);
    if (i < 0 || chunks[i].gen != gen) return false;
    const uint64_t pending = vk_pending_seq();
    if (chunks[i].seq < pending) chunks[i].seq = pending;
    return true;
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
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C,
       DEDUPE_CAMERA_V0, DEDUPE_CAMERA_V1, DEDUPE_CAMERA_F0, DEDUPE_CAMERA_F1, DEDUPE_SLOTS };
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
static bool vk_transient_hold(VkBuffer b, uint32_t gen);
static uint32_t transient_gen(VkBuffer b);
static void guest_copy_flush(void) { gcopy_gen++; }

// VR stereo: eye 1 replays the list eye 0 just replayed, and sees the same guest memory at each command (the stereo
// rollback, exec_list_stereo). Eye 0's copies are remembered by (command, address, size), and eye 1 reuses them
// instead of copying the same bytes again - the shared table above is dropped at every barrier, and lists have
// some. Across submissions too (eye 0 is submitted before eye 1 is replayed): a copy is reused while its transient
// chunk was not recycled, which is then held for eye 1's submission (vk_transient_hold). A command eye 0 ran twice
// (a list called twice) is not reused.
#define EYE_MEMO_TAB 16384
static struct { const void *rec; uint64_t addr, size; uint32_t list, gen; bool twice; VkSpan span; } eye_memo[EYE_MEMO_TAB];
static uint32_t eye_memo_list;    // the stereo list being replayed (entries of other lists are stale)
static int eye_memo_mode;         // 0 off, 1 eye 0 records, 2 eye 1 reuses
static const void *eye_memo_rec;  // the command being executed (draw())
static uint64_t stat_eye_memo_hits, stat_eye_memo_bytes, stat_eye_splits;
static uint64_t stat_feedback_direct;  // (depth targets sampled in place)
static uint32_t eye_memo_slot(const void *rec, uint64_t addr, uint64_t size) {
    uint64_t h = ((uint64_t)(uintptr_t)rec * 0x9E3779B97F4A7C15ull) ^ (addr * 0xC2B2AE3D27D4EB4Full) ^ size;
    return (uint32_t)(h ^ (h >> 29)) & (EYE_MEMO_TAB - 1);
}

static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint32_t mslot = 0;
    if (eye_memo_mode && eye_memo_rec) {
        mslot = eye_memo_slot(eye_memo_rec, addr, size);
        if (eye_memo_mode == 2) {
            const __typeof__(eye_memo[0]) *e = &eye_memo[mslot];
            if (e->list == eye_memo_list && e->rec == eye_memo_rec && e->addr == addr && e->size == size && !e->twice &&
                vk_transient_hold(e->span.buf, e->gen)) {  // (its chunk still holds it - also after eye 0 was submitted)
                stat_eye_memo_hits++;
                stat_eye_memo_bytes += size;
                return e->span;
            }
        }
    }
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    VkSpan s;
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size) s = gcopy[slot].span;
    else {
        s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
        gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s;
    }
    if (eye_memo_mode == 1 && eye_memo_rec) {
        __typeof__(eye_memo[0]) *e = &eye_memo[mslot];
        if (e->list == eye_memo_list && e->rec == eye_memo_rec && e->addr == addr && e->size == size) e->twice = true;
        else *e = (__typeof__(eye_memo[0])){eye_memo_rec, addr, size, eye_memo_list, transient_gen(s.buf), false, s};
    }
    return s;
}

// ================================================================== worker threads (shader / pipeline builds)
// Settings > Video > Shader compilation (shader_compile_mode): wait builds everything inline before drawing; async
// queues builds here and skips draws until they're ready; hybrid (draw_must_wait) skips only draws that are redrawn
// every frame, because a skipped shadow-map update or compute result can be reused for many frames.
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

static bool sync_shaders(void) { return shader_compile_mode() == SHADERS_WAIT; }

// Whether a draw into `target` (NULL: compute) waits for its shaders and pipeline. Under hybrid, a landscape target
// (the scene, its half/quarter-res effect buffers, the window image) is redrawn every frame, so its draws are skipped
// while they build; square or portrait ones (shadow maps, cube faces, atlases) may be drawn once and reused.
// ponytail: aspect-ratio heuristic; track which targets get read before they're redrawn if a skipped draw sticks.
static bool draw_must_wait(NvnTexture *target) {
    int m = shader_compile_mode();
    // (in VR the scene's targets are the eyes' - taller than wide, and with an eye-1 copy: drawn like the screen, a
    // draw whose pipeline is still building skips a frame instead of stopping it - a room first seen through a door
    // built 33 at once: a 104 ms frame in the headset)
    return m == SHADERS_WAIT || (m == SHADERS_HYBRID && !(target && (target->width > target->height || (target->backend && ((const TexBackend *)target->backend)->vr_eye))));
}

// A waiting draw whose build was already queued by a skippable one: block until a worker finishes it
static void wait_built(_Atomic int *state) { while (atomic_load(state) == 0) SDL_DelayNS(50000); }

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
    float alpha_ref;
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart, depth_clamp;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} st;

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (LARECOMP_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
static uint64_t prof_present;  // backend_present's own time (eye copies, submit, OpenXR frame end)
uint64_t g_vr_wait_ns;          // (vr.c) time blocked in xrWaitFrame
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    prof_present += present_ns;
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

const char *renderer_stats(void) {
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d, fbcopy %llu (in place %llu), upload %.1fMB, descriptors %llu hit/%llu new, VR shared lists %llu, multiview passes %llu draws %llu (eye 1 skipped %llu)",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), (unsigned long long)stat_feedback_copies, (unsigned long long)stat_feedback_direct, (double)stat_upload_bytes / 1048576.0,
             (unsigned long long)stat_descriptor_hits, (unsigned long long)stat_descriptor_misses,
             (unsigned long long)stat_vr_shared_lists, (unsigned long long)stat_mv_passes, (unsigned long long)stat_mv_draws,
             (unsigned long long)stat_mv_skipped);
    stat_feedback_copies = stat_upload_bytes = stat_feedback_direct = 0;
    { extern void tex_upload_prof_log(void); tex_upload_prof_log(); }
    {
        extern void vk_alias_stats(void);
        vk_alias_stats();
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | eye1 exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f vtx %.0f) %llu draws",
                 prof_e1_exec / 1e6, prof_e1_draw / 1e6, prof_e1_pre / 1e6, prof_e1_key / 1e6, prof_e1_dyn / 1e6,
                 prof_e1_sets / 1e6, prof_e1_tex / 1e6, prof_e1_vtx / 1e6, (unsigned long long)prof_e1_draws);
        prof_e1_exec = prof_e1_draw = prof_e1_sets = prof_e1_vtx = prof_e1_pre = prof_e1_key = prof_e1_dyn = prof_e1_tex = 0;
        prof_e1_draws = 0;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | barriers full %llu attachment %llu none %llu hazard %llu | foveated passes %llu, hidden-area masks %llu, bounds passes %llu",
                 (unsigned long long)stat_barrier_full, (unsigned long long)stat_barrier_att, (unsigned long long)stat_barrier_none,
                 (unsigned long long)stat_barrier_hazard,
                 (unsigned long long)stat_vrs_passes, (unsigned long long)stat_hidden_masks, (unsigned long long)stat_bounds_passes);
        stat_vrs_passes = stat_hidden_masks = stat_bounds_passes = 0;
        stat_barrier_full = stat_barrier_att = stat_barrier_none = stat_barrier_hazard = 0;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | present %.0f xrWaitFrame %.0f | eye1 reused %llu copies (%.1f MB), eye 0 submitted early %llu",
                 prof_present / 1e6, g_vr_wait_ns / 1e6, (unsigned long long)stat_eye_memo_hits, stat_eye_memo_bytes / 1048576.0,
                 (unsigned long long)stat_eye_splits);
        stat_eye_memo_hits = stat_eye_memo_bytes = stat_eye_splits = 0;
        prof_present = g_vr_wait_ns = 0;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | ops ms (eye0/eye1)");
        for (int i = 0; i < CMD_COUNT; i++) {
            if (op_ns[i][0] + op_ns[i][1] >= 10000000ull) {
                n = strlen(stats_text);
                snprintf(stats_text + n, sizeof stats_text - n, " %d:%.0f/%.0f", i, op_ns[i][0] / 1e6, op_ns[i][1] / 1e6);
            }
        }
        memset(op_ns, 0, sizeof op_ns);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 8192; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu | counters gpu %llu | pace %s, %llu frames over", (unsigned long long)prof_views_created,
                 (unsigned long long)stat_counters_gpu, pace_slow ? "30" : "60", (unsigned long long)stat_pace_over);
        stat_counters_gpu = stat_pace_over = 0;
        {
            extern uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n,  " | vcache %llu hit %llu in place %.1f MB copied, %llu queries %.2f ms, %llu pages written",
                     (unsigned long long)prof_vc_hits, (unsigned long long)prof_vc_bypass, prof_vc_copied / 1048576.0,
                     (unsigned long long)prof_vc_queries, prof_vc_scan_ns / 1e6, (unsigned long long)prof_vc_scan_pages);
            prof_vc_hits = prof_vc_bypass = prof_vc_copied = prof_vc_queries = prof_vc_scan_pages = prof_vc_scan_ns = 0;
            extern uint64_t prof_tex_watch_skips;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | tex unwritten %llu", (unsigned long long)prof_tex_watch_skips);
            prof_tex_watch_skips = 0;
        }
        pthread_mutex_lock(&wait_tab_lock);
        for (int i = 0; i < 12 && wait_tab[i].why; i++) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "%s%s %llux %.0f ms", i ? "," : " | waits:", wait_tab[i].why,
                     (unsigned long long)wait_tab[i].n, wait_tab[i].ns / 1e6);
        }
        memset(wait_tab, 0, sizeof wait_tab);
        pthread_mutex_unlock(&wait_tab_lock);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    stat_descriptor_hits = stat_descriptor_misses = 0;
    stat_vr_shared_lists = 0;
    stat_mv_passes = stat_mv_draws = stat_mv_skipped = 0;
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
    st.color_state[1] = VK_LOGIC_OP_COPY;
    st.color_state[2] = 8;  // alpha test ALWAYS
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
    VkQueryPoolCreateInfo vqi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                 .queryCount = SUBS * 2};
    if (vkCreateQueryPool(g_vk.dev, &vqi, NULL, &vr_qp) != VK_SUCCESS) vr_qp = VK_NULL_HANDLE;
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
static bool pass_vrs_attached;

// ---- VR multiview: a stereo list's eye-sized passes render both eyes at once (VK_KHR_multiview, viewMask 3) into
// the two layers of their targets (textures.c eye layers), with shaders that pick each eye's constant buffers and
// textures by gl_ViewIndex (multiview.c). Eye 0 decides per target set, keyed by how many render-target commands the
// list has run and the targets themselves; eye 1's replay then skips the draws and clears of those passes (and
// their CPU cost) and still runs everything else (its other passes, copies). A target set whose draws need what one
// pass can't give per eye (a feedback snapshot of the target, a shader the rewrite doesn't understand) is never
// multiview again. Keep separate-eye rendering as the default while the single-pass cutscene flicker is unresolved;
// SKYRECOMP_VR_MULTIVIEW=1 opts into the optimization for diagnosis.
static bool pass_mv;        // the open pass renders both eyes
static RtState pass_rt;     // the open pass's targets
static bool pass_mv_skip;   // eye 1: the current targets were rendered by eye 0's multiview pass
static bool g_mv_list;      // a stereo list is being replayed (either eye)
static uint32_t mv_epoch;   // render-target commands run so far in this eye of the list
static struct { uint32_t epoch; uint64_t sig; bool mv; } mv_pass[1024];
static int mv_npass;
// Target sets that aren't multiview: for good (a feedback snapshot, a shader the rewrite doesn't know), or while
// a texture their draws read is rendered by eye 1's own replay (late[]: re-checked in every list - its producer,
// earlier in the list, may be multiview itself now).
static struct { uint64_t sig; bool always; int nlate; const NvnTexture *late[6]; } mv_black[512];
static int mv_nblack;
// (eye 0) Textures eye 1 renders later in this list - by its own passes and copies -: a multiview pass runs during
// eye 0, before them, and can't read their eye-1 images yet. A target set whose draws read one is never multiview.
static NvnTexture *mv_later[1024];
static int mv_nlater;
// A depth target not scaled like the eyes is a light-space shadow map: the same for both eyes (eye 0's is read).
static bool mv_shared(const NvnTexture *t) {
    const TexBackend *b = t ? t->backend : NULL;
    return !t || (b && !b->eye_layer && !b->scalable && renderer_format_is_depth(t->format));
}
static void mv_note_later(NvnTexture *t) {
    if (!g_mv_list || vk_vr_eye() || !t || mv_shared(t)) return;
    for (int i = 0; i < mv_nlater; i++) if (mv_later[i] == t) return;
    if (mv_nlater < (int)(sizeof mv_later / sizeof mv_later[0])) mv_later[mv_nlater++] = t;
}
static bool mv_is_later(const NvnTexture *t) {
    for (int i = 0; i < mv_nlater; i++) if (mv_later[i] == t) return true;
    return false;
}
static uint64_t hash_words(uint64_t h, const void *p, size_t n);
static void vk_end_pass_at(int line);

static bool mv_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SKYRECOMP_VR_MULTIVIEW"); on = e && atoi(e) != 0; }
    return on && g_vk.multiview;
}
static uint64_t mv_sig(void) { return hash_words(0x6D76u, &st.rt, sizeof st.rt) | 1; }
static void mv_blacklist_late(uint64_t sig, const char *why, const NvnTexture *late) {
    int k = -1;
    for (int i = 0; i < mv_nblack && k < 0; i++) if (mv_black[i].sig == sig) k = i;
    if (k < 0) {
        if (mv_nblack == (int)(sizeof mv_black / sizeof mv_black[0])) return;
        k = mv_nblack++;
        mv_black[k].sig = sig, mv_black[k].always = false, mv_black[k].nlate = 0;
    }
    if (!late) mv_black[k].always = true;
    else {
        for (int i = 0; i < mv_black[k].nlate; i++) if (mv_black[k].late[i] == late) return;
        if (mv_black[k].nlate < 6) mv_black[k].late[mv_black[k].nlate++] = late;
        else mv_black[k].always = true;
    }
    static int logged;
    if (logged++ < 20) LOG("[vr] multiview: a target set falls back to one pass per eye (%s)\n", why);
}
static void mv_blacklist(uint64_t sig, const char *why) { mv_blacklist_late(sig, why, NULL); }
static bool mv_is_later(const NvnTexture *t);
static bool mv_blacklisted(uint64_t sig) {
    for (int i = 0; i < mv_nblack; i++) {
        if (mv_black[i].sig != sig) continue;
        if (mv_black[i].always) return true;
        for (int k = 0; k < mv_black[i].nlate; k++) if (mv_is_later(mv_black[i].late[k])) return true;
        return false;
    }
    return false;
}
// Eye 0's decision for these targets in this list: 1 multiview, 0 not, -1 none yet.
static int mv_lookup(uint64_t sig) {
    for (int i = mv_npass - 1; i >= 0; i--) if (mv_pass[i].epoch == mv_epoch && mv_pass[i].sig == sig) return mv_pass[i].mv;
    return -1;
}
// (eye 0) Whether these targets can be one multiview pass: every attachment both eyes' layers of one image.
static bool mv_eligible(uint64_t sig) {
    if (!g_mv_list || vk_vr_eye() || !mv_enabled() || hidden_bounds_on()) return false;
    if (!st.rt.ncolor && !st.rt.depth) return false;
    if (mv_blacklisted(sig)) return false;
    for (int i = 0; i < st.rt.ncolor; i++) {
        const NvnTexture *t = st.rt.color[i];
        const TexBackend *b = t ? t->backend : NULL;
        if (!t) continue;
        if (!b || !b->eye_layer || b->samples != 1 ||
            (st.rt.has_cview[i] && (st.rt.cview[i].setmask & 2) && st.rt.cview[i].min_layer)) return false;
        // (a target eye 1 renders earlier in the list, in its own replay - after this pass -: not multiview)
        if (mv_is_later(t)) return false;
    }
    if (st.rt.depth) {
        const TexBackend *b = st.rt.depth->backend;
        if (!b || !b->eye_layer || b->samples != 1 || (st.rt.has_dview && (st.rt.dview.setmask & 2) && st.rt.dview.min_layer))
            return false;
        if (mv_is_later(st.rt.depth)) return false;
    }
    return true;
}
// Choose once, before selecting a shader. Resolving a draw's textures can promote an attachment to a
// two-layer image; re-evaluating eligibility in begin_pass would then pair a single-eye pipeline with
// a multiview render pass. Eye 1 must replay the same decision, including a conservative first use.
static bool mv_decide(void) {
    // Materialize first-use attachments before deciding whether the pass has both eye layers. Otherwise a new
    // effect target is rendered only in eye 0, while its consumer can already sample both layers in multiview.
    for (int i = 0; i <= st.rt.ncolor; i++) {
        NvnTexture *t = i == st.rt.ncolor ? st.rt.depth : st.rt.color[i];
        const TexBackend *b = t ? t->backend : NULL;
        if (t && (!b || !b->vr_eye)) {
            vk_end_pass_at(-1);
            st.rt_dirty = true;
            (void)vk_tex(t, true);
        }
    }
    const uint64_t sig = mv_sig();
    const int known = mv_lookup(sig);
    if (known >= 0) return known == 1;
    if (mv_npass == (int)(sizeof mv_pass / sizeof mv_pass[0])) return false;
    const bool mv = in_pass && !memcmp(&pass_rt, &st.rt, sizeof st.rt) ? pass_mv : mv_eligible(sig);
    mv_pass[mv_npass].epoch = mv_epoch;
    mv_pass[mv_npass].sig = sig;
    mv_pass[mv_npass++].mv = mv;
    return mv;
}
// (eye 0, before a draw's program is chosen) Whether the pass this draw goes into renders both eyes.
static bool mv_for_draw(void) {
    if (!g_mv_list || vk_vr_eye()) return false;
    if (in_pass && !st.rt_dirty) return pass_mv;
    return mv_decide();
}
// (eye 1) The current targets were rendered by eye 0's multiview pass: skip this draw / clear.
static bool mv_skip(void) {
    if (!g_mv_list || !vk_vr_eye() || !mv_npass) return false;
    if (!st.rt_dirty) return pass_mv_skip && !in_pass;
    if (mv_lookup(mv_sig()) != 1) return false;
    vk_end_pass_at(-1);
    st.rt_dirty = false;
    pass_mv_skip = true;
    return true;
}
static int debug_pass_no;

// Redundant-state filter: what draw() last set in the current command buffer, so unchanged pipeline binds, dynamic
// state and vertex / index buffer binds are not recorded again (a draw used to issue ~20 vkCmd* calls; most repeat the
// previous draw's). Forgotten (ss_forget) whenever something else may have recorded state: a new command buffer, a new
// render pass (present, ImGui, post-processing and copies all run between game passes) and the clear pipeline.
// Descriptor sets are not filtered: every draw writes new ones (sharing identical sets within a submission was tried:
// a third matched, but comparing them cost more than allocating and writing them on NVIDIA).
// LARECOMP_VK_STATE_FILTER=0: every draw sets everything, as before.
enum { SS_PIPE = 1 << 0, SS_DT = 1 << 1, SS_DW = 1 << 2, SS_DOP = 1 << 3, SS_STEN = 1 << 4, SS_SFACE0 = 1 << 5, SS_SFACE1 = 1 << 6,
       SS_BC = 1 << 7, SS_CULL = 1 << 8, SS_FF = 1 << 9, SS_TOPO = 1 << 10, SS_PRE = 1 << 11, SS_DBE = 1 << 12, SS_DB = 1 << 13,
       SS_VP = 1 << 14, SS_SC = 1 << 15, SS_IB = 1 << 16 };
static struct {
    uint32_t known;  // SS_* bits: the value below is what the command buffer has
    uint32_t vb_known;  // per vertex binding
    VkPipeline pipe;
    VkBool32 dt, dw, sten, pre, dbe;
    VkCompareOp dop;
    uint32_t sface[2][7];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float bc[4], db[3];
    VkCullModeFlags cull;
    VkFrontFace ff;
    VkPrimitiveTopology topo;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkBuffer vb[17]; VkDeviceSize vo[17];
} ss;
static int ss_on = -1;
static int g_bounds_known = -1;  // (the depth bounds test as last set: -1 unknown; see pass_bounds)
static void ss_forget(void) { ss.known = 0; ss.vb_known = 0; g_bounds_known = -1; }
static bool ss_enabled(void) {
    if (ss_on < 0) { const char *e = getenv("LARECOMP_VK_STATE_FILTER"); ss_on = !e || atoi(e) != 0; }
    return ss_on;
}
// true: the state must be recorded (and is remembered as known)
static inline bool ss_need(uint32_t bit, bool same) {
    if (ss_on && (ss.known & bit) && same) return false;
    ss.known |= bit;
    return true;
}

static void vk_end_pass_at(int line);
static NvnTexture *g_depth_direct;  // (feedback snapshots below)
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (line >= 0) g_outside_work = true;  // (-1: begin_pass / pass_sample_hazard - nothing recorded in between)
    if (!in_pass) return;
    if (prof && line >= 0 && line < 8192) prof_end_line[line]++;
    vkCmdEndRendering(vk_cmd());
    if (pass_labelled) { vk_label_end(vk_cmd()); pass_labelled = false; }
    in_pass = false;
    pass_mv = false;
    g_depth_direct = NULL;  // (the next direct read of a depth target ends the next pass with its barrier)
    static long pass_shot = -2;  // (test) SKYRECOMP_VR_PASS_SHOT=<frame>: every pass's first target, both eyes
    if (pass_shot == -2) pass_shot = getenv("SKYRECOMP_VR_PASS_SHOT") ? atol(getenv("SKYRECOMP_VR_PASS_SHOT")) : -1;
    bool shot = g_debug_frame || (pass_shot >= 0 && renderer_present_count() == (uint64_t)pass_shot);
    if (shot && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        // (VR eye 1: its own image of the target, "_eye1" in the name)
        TexBackend *b = st.rt.color[0]->backend;
        const char *eye = "";
        if (vk_vr_eye() && b->vr_eye) { b = b->vr_eye; eye = "_eye1"; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d%s.png", (unsigned long long)debug_pass_frame, debug_pass_no++, eye);
        else snprintf(path, sizeof path, "build/pass_%03d%s.png", debug_pass_no++, eye);
        renderer_save_texture_tex(b, path);
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

// ---- VR fixed foveated shading (VK_KHR_fragment_shading_rate, device.c). The headset's lenses blur toward their
// edges, and the corners of each eye image are hidden behind the faceplate: passes that render the game's eye-sized
// images get a rate image that shades the centre at 1x1, a ring at 2x2 and the far periphery at 4x4 when supported
// (2x2 otherwise). Rings are measured from each eye's own optical axis (the Index's projections are
// asymmetric), relative to the horizontal half field of view. VR Options > Foveated Rendering (g_settings.vr_foveation,
// live: rings 0.6, 0.8); SKYRECOMP_VR_FOVEATION=on / <inner>,<outer> / 0 overrides. Measured in Skyloft at 1.5x:
// ~0.4 ms of ~10 even with everything at 2x2 - the eye passes are bound by geometry and bandwidth, not pixel shading.
// Pipelines take the image's rate (REPLACE; build_pipeline, clear_pipeline); without the image they shade at 1x1.
static float vrs_ring[2] = {-1, -1};
static bool vrs_enabled(void) {
    static bool env_read;
    if (!env_read) {
        env_read = true;
        vrs_ring[0] = 0.6f, vrs_ring[1] = 0.8f;  // environment override defaults ("on")
        const char *e = getenv("SKYRECOMP_VR_FOVEATION");
        if (e) {
            g_settings.vr_foveation = strcmp(e, "0") != 0;
            if (g_settings.vr_foveation && sscanf(e, "%f,%f", &vrs_ring[0], &vrs_ring[1]) == 1)
                vrs_ring[1] = vrs_ring[0] + 0.2f;
            g_settings.vr_foveation_inner = vrs_ring[0];
            g_settings.vr_foveation_outer = vrs_ring[1];
            LOG("[vr] initial foveation from SKYRECOMP_VR_FOVEATION (VR Options remains live)\n");
        }
    }
    vrs_ring[0] = g_settings.vr_foveation_inner;
    vrs_ring[1] = g_settings.vr_foveation_outer;
    if (vrs_ring[1] < vrs_ring[0] + 0.05f) vrs_ring[1] = vrs_ring[0] + 0.05f;
    static float logged_ring[2] = {-1, -1};
    if (g_vk.vrs && (logged_ring[0] != vrs_ring[0] || logged_ring[1] != vrs_ring[1])) {
        LOG("[vr] foveated shading rings %.2f / %.2f of the half field of view\n", vrs_ring[0], vrs_ring[1]);
        memcpy(logged_ring, vrs_ring, sizeof logged_ring);
    }
    return g_vk.vrs && g_settings.vr_foveation && vrs_ring[0] > 0;
}

typedef struct { uint32_t w, h; int eye; float fov[4], ring[2]; VkImage image; VmaAllocation mem; VkImageView view; } VrsImage;
static VrsImage vrs_cache[16];

// The rate image for a w x h target of eye `eye` (made once per size and field of view).
static VkImageView vrs_view(VkCommandBuffer cb, uint32_t w, uint32_t h, int eye) {
    float fov[4];
    if (!vr_eye_fov(eye, fov)) return VK_NULL_HANDLE;
    for (int i = 0; i < 16; i++) {
        VrsImage *c = &vrs_cache[i];
        if (c->view && c->w == w && c->h == h && c->eye == eye && fabsf(c->fov[0] - fov[0]) < 1e-3f &&
            fabsf(c->fov[1] - fov[1]) < 1e-3f && fabsf(c->fov[2] - fov[2]) < 1e-3f && fabsf(c->fov[3] - fov[3]) < 1e-3f &&
            fabsf(c->ring[0] - vrs_ring[0]) < 1e-3f && fabsf(c->ring[1] - vrs_ring[1]) < 1e-3f)
            return c->view;
    }
    static int next;
    VrsImage *c = &vrs_cache[next++ & 15];
    if (c->view) {  // (evicted: a size no longer drawn)
        vk_defer_view(c->view);
        vk_defer_image(c->image, c->mem);
        c->view = VK_NULL_HANDLE;
    }
    const uint32_t tw = g_vk.vrs_texel_w, th = g_vk.vrs_texel_h, gw = (w + tw - 1) / tw, gh = (h + th - 1) / th;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8_UINT,
                            .extent = {gw, gh, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &c->image, &c->mem, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = c->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8_UINT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &c->view));
    c->w = w; c->h = h; c->eye = eye; memcpy(c->fov, fov, sizeof fov); memcpy(c->ring, vrs_ring, sizeof vrs_ring);
    // Attachment encoding is log2(width) << 2 | log2(height): 1x1=0, 1x2=1,
    // 2x1=4, 2x2=5 and 4x4=10. Use only rates enumerated by the device.
    const bool r12 = g_vk.vrs_rates & (1u << 1), r21 = g_vk.vrs_rates & (1u << 3);
    const bool r22 = g_vk.vrs_rates & (1u << 4), r44 = g_vk.vrs_rates & (1u << 8);
    const float tl = tanf(fov[0]), tr = tanf(fov[1]), tu = tanf(fov[2]), td = tanf(fov[3]);
    const float half = 0.5f * (fov[1] - fov[0]);
    const bool flip = renderer_origin_lower_left();
    VkSpan sp = vk_transient((uint64_t)gw * gh, 16);
    uint8_t *px = sp.ptr;
    int count[4] = {0};
    for (uint32_t y = 0; y < gh; y++)
        for (uint32_t x = 0; x < gw; x++) {
            const float u = ((float)x + 0.5f) * tw / (float)w, v = ((float)y + 0.5f) * th / (float)h;
            const float tx = tl + u * (tr - tl), ty = flip ? td + v * (tu - td) : tu - v * (tu - td);
            const float a = atanf(sqrtf(tx * tx + ty * ty)) / half;
            uint8_t rate = 0;
            if (a >= vrs_ring[1]) rate = r44 ? 10 : r22 ? 5 : 0;
            else if (a >= vrs_ring[0])
                rate = r22 ? 5 : fabsf(tx) >= fabsf(ty) ? (r21 ? 4 : 0) : (r12 ? 1 : 0);
            px[y * gw + x] = rate;
            count[rate == 0 ? 0 : rate == 10 ? 3 : rate == 5 ? 2 : 1]++;
        }
    vk_image_init_layout(cb, c->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy bc = {.bufferOffset = sp.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {gw, gh, 1}};
    vkCmdCopyBufferToImage(cb, sp.buf, c->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    vk_full_barrier(cb);
    const float n = (float)(gw * gh) / 100.0f;
    LOG("[vr] foveated shading: eye %d %ux%u: 1x1 %.0f%%, 2x1/1x2 %.0f%%, 2x2 %.0f%%, 4x4 %.0f%%\n",
        eye, w, h, count[0] / n, count[1] / n, count[2] / n, count[3] / n);
    return c->view;
}

// ---- VR hidden area, everywhere (see hidden_mask): passes on eye-sized targets reject the hidden pixels with the
// depth bounds test, which looks at the depth already stored, not at what the draw computes - so it also works for
// the full-screen passes (lighting, fog, post: over half the GPU time at 1.5x) that never depth-test. A pass without
// a depth target gets a mask of its own (D16: hidden 1, visible 0; bounds 0..0.5, never written); a pass whose
// depth target was masked after its clear keeps values short of the hidden one.
static bool pass_mask_injected;       // this pass's depth target is our mask (the game's depth test stays off)
static float pass_bounds[2] = {0, 1};
static bool pass_bounds_on;
static struct { NvnTexture *tex; float hv; uint64_t frame; } g_masked_depth[2];  // per eye: masked after its clear

typedef struct { uint32_t w, h; int eye; float fov[4]; TexBackend img; VkImageView view; } MaskImage;
static MaskImage mask_cache[16];

static VkImageView hidden_mask_view(VkCommandBuffer cb, uint32_t w, uint32_t h, int eye) {
    const float *xy;
    float fov[4];
    const uint32_t n = vr_hidden_mesh(eye, &xy);
    if (!n || !vr_eye_fov(eye, fov)) return VK_NULL_HANDLE;
    for (int i = 0; i < 16; i++) {
        MaskImage *c = &mask_cache[i];
        if (c->view && c->w == w && c->h == h && c->eye == eye && fabsf(c->fov[0] - fov[0]) < 1e-3f &&
            fabsf(c->fov[1] - fov[1]) < 1e-3f && fabsf(c->fov[2] - fov[2]) < 1e-3f && fabsf(c->fov[3] - fov[3]) < 1e-3f)
            return c->view;
    }
    static int next;
    MaskImage *c = &mask_cache[next++ & 15];
    if (c->view) {
        vk_defer_view(c->view);
        vk_defer_image(c->img.image, c->img.mem);
        c->view = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D16_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &c->img.image, &c->img.mem, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = c->img.image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_D16_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &c->view));
    c->w = w; c->h = h; c->eye = eye; memcpy(c->fov, fov, sizeof fov);
    // CPU raster of the hidden triangles (once per size): pixel centres inside any triangle -> 0xFFFF
    const float tl = tanf(fov[0]), tr = tanf(fov[1]), tu = tanf(fov[2]), td = tanf(fov[3]);
    const bool flip = renderer_origin_lower_left();
    VkSpan sp = vk_transient((uint64_t)w * h * 2, 16);
    uint16_t *px = sp.ptr;
    memset(px, 0, (size_t)w * h * 2);
    uint64_t hidden = 0;
    for (uint32_t t = 0; t + 2 < n; t += 3) {
        float X[3], Y[3];
        for (int k = 0; k < 3; k++) {  // view tangents -> pixel coordinates (row 0 = the image's first row)
            X[k] = (xy[(t + k) * 2] - tl) / (tr - tl) * (float)w;
            const float v = (xy[(t + k) * 2 + 1] - td) / (tu - td);  // (0 bottom .. 1 top of the view)
            Y[k] = (flip ? v : 1.0f - v) * (float)h;
        }
        // scanlines: each row's span inside the triangle (pixel centres), from its three edges
        int ya = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), yb = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
        if (ya < 0) ya = 0;
        if (yb > (int)h) yb = (int)h;
        for (int y = ya; y < yb; y++) {
            const float cy = y + 0.5f;
            float lo = 1e30f, hi = -1e30f;
            for (int e = 0; e < 3; e++) {
                const float ax = X[e], ay = Y[e], bx = X[(e + 1) % 3], by = Y[(e + 1) % 3];
                if ((cy < ay) == (cy < by)) continue;  // (the row misses this edge)
                const float x = ax + (cy - ay) / (by - ay) * (bx - ax);
                lo = fminf(lo, x);
                hi = fmaxf(hi, x);
            }
            int x0 = (int)ceilf(lo - 0.5f), x1 = (int)floorf(hi - 0.5f);
            if (x0 < 0) x0 = 0;
            if (x1 > (int)w - 1) x1 = (int)w - 1;
            for (int x = x0; x <= x1; x++) {
                uint16_t *q = &px[(size_t)y * w + x];
                hidden += !*q;
                *q = 0xFFFF;
            }
        }
    }
    vk_image_init_layout(cb, c->img.image, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkBufferImageCopy bc = {.bufferOffset = sp.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, sp.buf, c->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    vk_full_barrier(cb);
    LOG("[vr] hidden-area mask eye %d %ux%u: %.1f%% hidden\n", eye, w, h, 100.0 * (double)hidden / ((double)w * h));
    return c->view;
}

static bool full_barriers_only(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_VK_FULL_BARRIERS") != NULL;
    return on;
}

static bool written_since_barrier(const NvnTexture *t) {  // (the same object, or another one over the same memory)
    for (int i = 0; i < g_nwritten; i++)
        if (g_written[i] == t || (t->addr && g_written[i]->addr == t->addr)) return true;
    return false;
}

static void begin_pass(void) {
    // The same targets as the open pass: it goes on (setting the same targets again would otherwise end the pass
    // and start it again, with its attachment barriers)
    if (in_pass && !memcmp(&pass_rt, &st.rt, sizeof st.rt) &&
        (!pass_mv || (g_mv_list && !vk_vr_eye()))) {
        if (g_mv_list && !vk_vr_eye()) (void)mv_decide();
        st.rt_dirty = false;
        return;
    }
    vk_end_pass_at(-1);
    pass_mv_skip = false;
    bool mv = false;  // (VR multiview: eye 0 renders both eyes here)
    if (g_mv_list && !vk_vr_eye()) {
        mv = mv_decide();
    }
    static const VkComponentMapping mv_id = {0};
    if (!mv && g_mv_list && !vk_vr_eye()) {  // (eye 1 renders these targets in its own replay, later)
        for (int i = 0; i < st.rt.ncolor; i++) mv_note_later(st.rt.color[i]);
        mv_note_later(st.rt.depth);
    }
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    st.rt_dirty = false;
    if (!st.rt.depth && st.rt.ncolor == 0) g_stereo_shadow_or_depth = true;
    VkCommandBuffer cb = vk_cmd();
    {   // passes, copies and dispatches see each other's results (see g_outside_work)
        static int nobar = -1;  // (experiment, wrong images possible) LARECOMP_VK_EXP_NOBARRIER=1: no barrier at all
        if (nobar < 0) nobar = getenv("LARECOMP_VK_EXP_NOBARRIER") != NULL;
        bool overlap = false;
        for (int i = 0; i < st.rt.ncolor && !overlap; i++) overlap = st.rt.color[i] && written_since_barrier(st.rt.color[i]);
        if (st.rt.depth && written_since_barrier(st.rt.depth)) overlap = true;
        if (nobar) {
        } else if (g_outside_work || full_barriers_only() || g_nwritten > 28) {
            vk_full_barrier(cb);
            g_outside_work = false;
            g_nwritten = 0;
            stat_barrier_full++;
        } else if (overlap) {  // (drawing on to an image a previous pass rendered: attachment writes before attachment use)
            VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                   .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
            VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
            vkCmdPipelineBarrier2(cb, &di);
            stat_barrier_att++;
        } else {
            stat_barrier_none++;
        }
        for (int i = 0; i < st.rt.ncolor; i++)
            if (st.rt.color[i] && !written_since_barrier(st.rt.color[i])) g_written[g_nwritten++] = st.rt.color[i];
        if (st.rt.depth && !written_since_barrier(st.rt.depth)) g_written[g_nwritten++] = st.rt.depth;
    }
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
        if (mv) {  // (both layers; eye 1's first-write marks are used up too: its replay skips this pass)
            ca[i].imageView = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D_ARRAY, pass_color[i], lv, 1, 0, 2, mv_id, VK_IMAGE_ASPECT_COLOR_BIT);
            vk_vr_set_eye(1);
            (void)renderer_texture_level_fresh(t), (void)renderer_texture_frame_fresh(t), (void)renderer_texture_first_write(t);
            vk_vr_set_eye(0);
        }
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        // Only an unscaled depth-only target is unambiguously a light-space
        // shadow map. Any pass with color, or scaled scene depth, must replay
        // with the second eye's targets and uniforms.
        if (!b || b->scalable || st.rt.ncolor != 0) g_stereo_shadow_or_depth = true;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            if (mv) a.imageView = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D_ARRAY, b->format, lv, 1, 0, 2, mv_id, b->aspect);
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    pass_mask_injected = pass_bounds_on = false;
    if (w && !mv && g_vk.depth_bounds && hidden_bounds_on() && vr_stereo_active() && pass_samples == 1 && minw >= 512 && minh >= 512) {
        float gsx, gsy;
        renderer_get_scale(&gsx, &gsy);
        const int eye = vk_vr_eye() ? 1 : 0;
        if (gsx > 1.0f && fabsf(pass_sx - gsx) < 1e-3f && fabsf(pass_sy - gsy) < 1e-3f) {
            if (!has_d && !has_s) {  // the full-screen passes: our mask as their (read-only) depth target
                VkImageView mv = hidden_mask_view(cb, minw, minh, eye);
                if (mv) {
                    da = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = mv,
                                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                     .storeOp = VK_ATTACHMENT_STORE_OP_NONE};
                    has_d = true;
                    pass_depth = VK_FORMAT_D16_UNORM;
                    pass_mask_injected = pass_bounds_on = true;
                    pass_bounds[0] = 0.0f; pass_bounds[1] = 0.5f;
                }
            } else if (has_d && g_masked_depth[eye].tex == st.rt.depth && g_masked_depth[eye].frame == renderer_present_count()) {
                pass_bounds_on = true;  // (the game's depth, masked after its clear: its hidden value is rejected)
                if (g_masked_depth[eye].hv > 0.5f) { pass_bounds[0] = 0.0f; pass_bounds[1] = 0.99999f; }
                else { pass_bounds[0] = 0.00001f; pass_bounds[1] = 1.0f; }
            }
            if (pass_bounds_on) stat_bounds_passes++;
        }
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .viewMask = mv ? 3u : 0u, .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    pass_vrs_attached = false;
    // VRS: only the passes drawing an eye's presented image (the scene, with its depth: depth and stencil stay per
    // sample under coarse shading); a pass ends before an alpha-tested draw, which needs per-fragment coverage
    // (CMD_BIND_COLOR). With no rate attachment the pipeline's primitive rate below remains 1x1.
    VkRenderingFragmentShadingRateAttachmentInfoKHR fsr = {.sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
                                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                           .shadingRateAttachmentTexelSize = {g_vk.vrs_texel_w, g_vk.vrs_texel_h}};
    const bool alpha_tested = st.color_state[2] < 8;
    // A scaled color intermediate is not necessarily an eye image. In
    // particular, screen-space shadow/projection masks are eye-sized too; coarse
    // shading there becomes visible as dark attachment-texel squares on the
    // floor. Only the texture ultimately presented for this eye may use VRS.
    const bool eye_output = pass_ncolor > 0 && st.rt.color[0] && renderer_texture_presented(st.rt.color[0]);
    if (w && !mv && eye_output && !alpha_tested && vr_stereo_active() &&
        pass_samples == 1 && minw >= 512 && minh >= 512 && vrs_enabled()) {
        float gsx, gsy;
        renderer_get_scale(&gsx, &gsy);
        if (fabsf(pass_sx - gsx) < 1e-3f && fabsf(pass_sy - gsy) < 1e-3f &&
            (fsr.imageView = vrs_view(cb, minw, minh, vk_vr_eye() ? 1 : 0))) {
            ri.pNext = &fsr;
            pass_vrs_attached = true;
            stat_vrs_passes++;
            if (g_gpuprof_frame) {  // (the capture's pass labels: this one is foveated)
                size_t l = gp_n ? strlen(gp_label[gp_n - 1]) : 0;
                if (gp_n) snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, " [foveated]");
            }
        }
    }
    if (g_vk.label_begin) {  // (profiler markers: each pass named by what it renders)
        char name[96];
        float gsx = 1, gsy = 1;
        renderer_get_scale(&gsx, &gsy);
        const bool eye_sized = vr_stereo_active() && gsx > 1.0f && fabsf(pass_sx - gsx) < 1e-3f && minw >= 512;
        const NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : st.rt.depth;
        extern TexBackend *vk_hud_backend(void);
        if (t0 && t0->backend && t0->backend == vk_hud_backend()) snprintf(name, sizeof name, "[VR] HUD");
        else if (eye_sized) snprintf(name, sizeof name, "[VR] Eye %d %s %ux%u", vk_vr_eye() ? 1 : 0,
                                     st.rt.depth ? "Scene" : "Post Processing", minw, minh);
        else snprintf(name, sizeof name, "%sOffscreen %ux%u", vr_stereo_active() ? (vk_vr_eye() ? "[VR] Eye 1 " : "[VR] Eye 0 ") : "",
                      minw, minh);
        vk_label_begin(cb, name);
        pass_labelled = true;
    }
    vkCmdBeginRendering(cb, &ri);
    ss_forget();  // (whatever ran between the passes may have set its own state)
    in_pass = true;
    pass_rt = st.rt;
    pass_mv = mv;
    if (mv) stat_mv_passes++;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
    cum_passes++;
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
    if (!ss_need(SS_SC, !memcmp(&r, &ss.sc, sizeof r))) return;
    ss.sc = r;
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!ss_need(SS_VP, !memcmp(&vp, &ss.vp, sizeof vp))) return;
    ss.vp = vp;
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
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE};  // (last two: only with g_vk.depth_bounds)
#define NDYN ((uint32_t)(sizeof DYN / sizeof DYN[0]) - (g_vk.depth_bounds ? 0 : 2))

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
    if (g_vk.depth_bounds) {
        vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
        vkCmdSetDepthBounds(cb, 0.0f, 1.0f);
    }
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline_ex(int index, int kind, uint32_t mask, bool depth, bool stencil, bool mesh) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(mesh); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples); MIX(pass_mv);
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
         .module = vk_builtin_shader(mesh ? "hidmask.vert" : "clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkVertexInputBindingDescription mvb = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription mva = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = mesh ? 1 : 0, .pVertexBindingDescriptions = &mvb,
                                               .vertexAttributeDescriptionCount = mesh ? 1 : 0, .pVertexAttributeDescriptions = &mva};
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
        if ((int)i == index) {
            cba[i].colorWriteMask = mask & 0xF;
            if (kind == 3) {
                cba[i].blendEnable = VK_TRUE;
                cba[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba[i].colorBlendOp = VK_BLEND_OP_ADD;
                cba[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba[i].alphaBlendOp = VK_BLEND_OP_ADD;
            }
        }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineFragmentShadingRateStateCreateInfoKHR fsr = {  // (VR foveated passes: the rate image decides, begin_pass)
        .sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, .fragmentSize = {1, 1},
        .combinerOps = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR}};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .viewMask = pass_mv ? 3u : 0u,
                                           .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil, .pNext = g_vk.vrs ? &fsr : NULL};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .flags = g_vk.vrs ? VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR : 0,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vk_create_graphics_pipelines(1, &pi, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    return clear_pipeline_ex(index, kind, mask, depth, stencil, false);
}

// ---- VR hidden area (vr.c vr_hidden_mesh, XR_KHR_visibility_mask: a quarter of each Index eye image). Right after
// the game clears the depth of an eye-sized target whole, the headset's hidden triangles are drawn into it at the
// opposite end of the depth range from the clear value: every later depth-tested draw of the pass fails there, so
// the hidden pixels cost no shading, colour writes or texture reads. Never seen: whatever stays there is harmless.
// Opt-in (SKYRECOMP_VR_HIDDEN_MASK=1, see hidden_bounds_on); =2 also paints the mask red into colour target 0.
static int hidden_mask_mode(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("SKYRECOMP_VR_HIDDEN_MASK"); m = e ? atoi(e) : 0; }
    return m;
}
static bool eye_sized_pass(void) {  // (a pass on a target scaled like the eyes: not the HUD, shadows, small buffers)
    if (!vr_stereo_active() || pass_samples != 1 || pass_w < 512 || pass_h < 512) return false;
    float gsx, gsy;
    renderer_get_scale(&gsx, &gsy);
    return gsx > 1.0f && fabsf(pass_sx - gsx) < 1e-3f && fabsf(pass_sy - gsy) < 1e-3f;
}

static void hidden_mask(float dval) {
    const int mode = hidden_mask_mode();
    if (!mode || !pass_depth || !eye_sized_pass()) return;
    const int eye = vk_vr_eye() ? 1 : 0;
    const float *xy;
    float fov[4];
    const uint32_t n = vr_hidden_mesh(eye, &xy);
    if (!n || !vr_eye_fov(eye, fov)) return;
    const float tl = tanf(fov[0]), tr = tanf(fov[1]), tu = tanf(fov[2]), td = tanf(fov[3]);
    const bool flip = renderer_origin_lower_left();  // (images stored bottom-up: row 0 is the view's bottom)
    VkSpan vb = vk_transient((uint64_t)n * 8, 16);
    float *o = vb.ptr;
    for (uint32_t i = 0; i < n; i++) {
        o[i * 2] = (2.0f * xy[i * 2] - (tr + tl)) / (tr - tl);
        const float fy = (2.0f * xy[i * 2 + 1] - (tu + td)) / (tu - td);
        o[i * 2 + 1] = flip ? fy : -fy;
    }
    const bool paint = mode == 2 && pass_ncolor && pass_color[0] != VK_FORMAT_UNDEFINED;
    VkPipeline p = clear_pipeline_ex(paint ? 0 : -1, 0, paint ? 0xF : 0, true, false, true);
    if (!p) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();
    dyn_defaults(cb);
    vkCmdSetDepthTestEnable(cb, VK_TRUE);
    vkCmdSetDepthWriteEnable(cb, VK_TRUE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    VkViewport vp = {0, 0, (float)pass_w, (float)pass_h, 0, 1};
    VkRect2D sc = {{0, 0}, {pass_w, pass_h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pc[8] = {1, 0, 0, 1, dval < 0.5f ? 1.0f : 0.0f};
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    VkDeviceSize off = vb.off;
    vkCmdBindVertexBuffers(cb, 0, 1, &vb.buf, &off);
    vkCmdDraw(cb, n, 1, 0, 0);
    stat_hidden_masks++;
    g_masked_depth[eye].tex = st.rt.depth;
    g_masked_depth[eye].hv = pc[4];
    g_masked_depth[eye].frame = renderer_present_count();
    if (g_vk.depth_bounds) vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    g_bounds_known = -1;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    if (mv_skip()) return;  // (VR multiview: eye 0's pass cleared both eyes)
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    if (!vk_vr_eye() && index == 0 && (mask & 7) == 7 && st.rt.color[0] &&
        renderer_texture_presented(st.rt.color[0]) && rect.offset.x == 0 && rect.offset.y == 0 &&
        rect.extent.width == pass_w && rect.extent.height == pass_h) {
        const float *rgb = color;
        g_black_clear_target = rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0 ? st.rt.color[0] : NULL;
        g_black_clear_frame = renderer_present_count();
    }
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
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
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();  // (its own pipeline and state: the next draw sets everything again)
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
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    bool mv;  // (VR multiview: both eyes, multiview.c)
    bool inline_build;  // first wanted by a draw that waits (draw_must_wait): translate and compile right away
    _Atomic int state;  // 0 building, 1 ready, 2 failed
} Prog;

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
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

static void compile_program(void *arg) {
    Prog *p = arg;
    size_t nv = 0, nf = 0;
    char *mvs = p->mv ? vk_multiview_glsl(p->vi->msl) : NULL, *mfs = p->mv ? vk_multiview_glsl(p->fi->msl) : NULL;
    if (p->mv && (!mvs || !mfs)) {
        static int logged;
        if (logged++ < 10) LOG("[vr] multiview: shader %llx/%llx not rewritten\n", (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
        free(mvs); free(mfs); atomic_store(&p->state, 2); return;
    }
    uint32_t *sv = vk_glsl_to_spirv(mvs ? mvs : p->vi->msl, STAGE_VERTEX, "vs", &nv);
    uint32_t *sf = sv ? vk_glsl_to_spirv(mfs ? mfs : p->fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
    free(mvs); free(mfs);
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

static void translate_program(void *arg) {
    Prog *p = arg;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    pthread_mutex_lock(&translate_lock);
    p->vi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
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
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    if (p->inline_build) compile_program(p);
    else run_job(compile_program, p);
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey, bool mv, bool wait) {
    if (!(np->stage_mask & 1) || !(np->stage_mask & 2)) return NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = np->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    h ^= fkey->alpha_func; h *= 0x100000001B3ull;
    h ^= fkey->dual_source; h *= 0x100000001B3ull;
    if (mv) { h ^= 0x6D76; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    Prog *p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    p->mv = mv;
    p->inline_build = wait;
    if (wait) translate_program(p);
    else run_translate(translate_program, p);
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
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
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
    uint8_t blend_on, a2c, topo_class, poly_fill, depth_clamp, logic_op, or_max;
    uint32_t write_mask[8];
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
                                                 .depthClampEnable = k->depth_clamp,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
        if ((k->or_max >> i) & 1) {  // logic op OR without the logicOp feature: max blending (see the key below)
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = cba[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].srcAlphaBlendFactor = cba[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].colorBlendOp = cba[i].alphaBlendOp = VK_BLEND_OP_MAX;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .logicOpEnable = k->logic_op != VK_LOGIC_OP_COPY,
                                              .logicOp = (VkLogicOp)k->logic_op,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineFragmentShadingRateStateCreateInfoKHR fsr = {  // (VR foveated passes: the rate image decides, begin_pass)
        .sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, .fragmentSize = {1, 1},
        .combinerOps = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR}};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .viewMask = k->prog->mv ? 3u : 0u,
                                        .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil, .pNext = g_vk.vrs ? &fsr : NULL};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .flags = g_vk.vrs ? VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR : 0,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    VkResult r = vk_create_graphics_pipelines(1, &pi, &pp->pipe);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static VkImageView feedback_view1[4];  // (VR multiview draws: eye 1's, else NULL)
static int nfeedback;
static uint64_t g_cur_fs_hash;
static bool g_draw_samples_screen;  // this draw's fragment shader samples a screen-sized GPU-written texture
static bool g_draw_screen_composite;  // an image-copy quad, despite its perspective camera block
#define WATER_SURFACE_FS 0xbfc2287ba468fc7full  // (the outdoor water surface's fragment shader)

// Row-major 4x4 inverse (Gauss-Jordan, partial pivoting); false when singular.
static bool m4_invert(const float m[16], float out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (!(fabs(a[p][c]) > 1e-12)) return false;
        for (int k = 0; k < 8; k++) { const double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c || a[r][c] == 0) continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = (float)a[r][c + 4];
    return true;
}
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
// A depth target a draw samples while it is attached, without writing it (fog, water and lighting passes read the
// scene depth): sampled in place instead of through a full copy (0.75 ms per eye at 1.9x in VR). The pass is ended
// once, with a barrier, before the first such read; a later write to it (or a clear, a copy) ends it again first,
// so reads and writes never meet inside one rendering. LARECOMP_FEEDBACK_COPY=1: always copy (the old way).
static NvnTexture *g_depth_direct;
static void snapshots_invalidate(NvnTexture *t) {
    if (g_depth_direct && (!t || t == g_depth_direct)) {
        vk_end_pass();
        vk_full_barrier(vk_cmd());
        g_depth_direct = NULL;
    }
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

// VR HUD layer (see draw()): while a 2D-layer draw renders into the HUD texture, reads of the presented image (the
// map's parchment composites the map drawn so far) read the HUD texture instead.
static NvnTexture *g_hud_sub_from, *g_hud_sub_to;
static NvnTexture *g_menu_screen;  // (the screen the last HUD draw was redirected from)
// (the game samples it through other texture objects over the same memory: matched by address and size)
static inline NvnTexture *hud_sub(NvnTexture *t) {
    if (!t || !g_hud_sub_from) return t;
    if (t == g_hud_sub_from) return g_hud_sub_to;
    if (t->addr && t->addr == g_hud_sub_from->addr && t->width == g_hud_sub_from->width && t->height == g_hud_sub_from->height)
        return g_hud_sub_to;
    return t;
}

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return hud_sub(((NvnTexEntry *)st.tpool->entries)[tic].tex);
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src, bool two) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format || d->eye_layer != two)) {
        vk_views_forget(d);  // (resolution change; VR multiview: one or both eyes)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = two ? 2 : 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkResult vr = vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL);
        if (vr != VK_SUCCESS) {
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        }
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        d->eye_layer = two;  // (layer 1: eye 1's snapshot)
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
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, b->layer0, 1},
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

// (debug) LARECOMP_COPY_TRACE=<frame>: draws' attachments / sampled textures, copies and snapshots of that frame ("[ct]")
static bool ct_on(void) {
    static const char *e = (const char *)1;
    if (e == (const char *)1) e = getenv("LARECOMP_COPY_TRACE");
    if (!e) return false;
    static uint64_t memo_f = ~0ull;
    static bool memo;
    uint64_t f = renderer_present_count();
    if (f == memo_f) return memo;
    memo_f = f;
    memo = false;
    for (const char *p = e; *p;) {
        if ((uint64_t)strtoll(p, (char **)&p, 10) == f) { memo = true; break; }
        if (*p == ',') p++; else break;
    }
    if (memo) LOG("[ct] ===== frame %llu\n", (unsigned long long)f);
    return memo;
}
static void ct_draw(Prog *pr, const char *what) {
    LOG("[ct] %s fs %016llx rt", what, (unsigned long long)g_cur_fs_hash);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" c%d=%p/%#x", i, (void *)st.rt.color[i], st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d=%p/%#x%s", (void *)st.rt.depth, st.rt.depth->format, st.ds[0] && st.ds[1] ? "W" : "");
    LOG(" cm %08x vp %.0f %.0f %.0f %.0f sc %d %d %d %d | tex", st.channel_mask, st.viewport[0][0], st.viewport[0][1], st.viewport[0][2],
        st.viewport[0][3], st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < (infos[sg] ? infos[sg]->ntex : 0); i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (t) LOG(" %s%p/%#x", sg ? "" : "v:", (void *)t, t->format);
        }
    LOG("\n");
}

// ================================================================== copy elision (depth copies)
// The game copies its main depth buffer whole, every frame, into textures that are then only sampled while the
// source keeps its depth (the water / transparency depth input), or that get their depth cleared right away
// (stencil kept). At 3x internal resolution each such copy is ~0.15 ms of pure bandwidth. Instead of copying, the
// destination becomes an alias: sampling it (and copying from it) reads the source's image. The alias ends before
// anything changes either side:
//   - the source is about to be overwritten whole (both aspects cleared): the two images swap - the destination
//     keeps the contents without a copy, the source is cleared in the destination's old image;
//   - the destination is about to be overwritten in some aspects (its depth cleared): only the other aspects are
//     copied (stencil, a quarter of the bytes);
//   - anything else (a draw writing depth / stencil, a partial clear, a copy or upload into either, the
//     destination bound as an attachment, a texture freed or reallocated): the real copy is made first.
// Only same-size, same-format, single-level, single-sample depth/stencil images, whole-image copies.
// LARECOMP_VK_COPY_OPT=0: always copy (the old behaviour).
typedef struct { NvnTexture *src, *dst; VkImage simg, dimg; } CopyAlias;
extern uint32_t vk_view_gen;
static CopyAlias aliases[8];
static int naliases;
static uint64_t stat_alias_made, stat_alias_renamed, stat_alias_partial, stat_alias_copied;

static int copy_opt(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_COPY_OPT"); on = e && *e ? atoi(e) : 1; }
    return on;
}

// still valid: both textures keep the images the alias was made with (a resolution change reallocates them)
static bool alias_valid(const CopyAlias *a) {
    const TexBackend *s = a->src->backend, *d = a->dst->backend;
    return s && d && s->image == a->simg && d->image == a->dimg;
}

static void alias_drop(int i) {
    aliases[i] = aliases[--naliases];
    vk_view_gen++;  // (exec.c's per-frame view memo: the destination's views change)
}

// the real copy of the aspects asked for, into the destination's own image
static void alias_copy(CopyAlias *a, VkImageAspectFlags aspects, const char *why) {
    TexBackend *s = a->src->backend, *d = a->dst->backend;
    aspects &= d->aspect;
    if (!aspects) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "copy texture (deferred, %s%s) %ux%u", why, aspects == d->aspect ? "" : ", stencil only", a->dst->width, a->dst->height);
    VkImageCopy c = {.srcSubresource = {aspects, 0, 0, 1}, .dstSubresource = {aspects, 0, 0, 1}, .extent = {d->width, d->height, 1}};
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    if (aspects == d->aspect) stat_alias_copied++; else stat_alias_partial++;
    if (ct_on()) LOG("[ct] alias copy %p -> %p aspects %#x (%s)\n", (void *)a->src, (void *)a->dst, aspects, why);
}

// t is about to be written; full: the aspects of it the write overwrites whole (0: unknown / partial)
static void alias_before_write(NvnTexture *t, VkImageAspectFlags full, const char *why) {
    if (!naliases || !t) return;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (!alias_valid(a)) { alias_drop(i); continue; }
        if (a->dst != t) continue;
        // (LARECOMP_VK_COPY_OPT=2: all aspects even then - for comparing the stencil-only copy's cost)
        alias_copy(a, ((TexBackend *)t->backend)->aspect & (copy_opt() == 2 ? ~0u : ~full), why);
        alias_drop(i);
    }
    NvnTexture *heir = NULL;  // t overwritten whole: one destination can take its image instead of a copy
    if (t->backend && (((TexBackend *)t->backend)->aspect & ~full) == 0)
        for (int i = 0; i < naliases && !heir; i++) if (aliases[i].src == t) heir = aliases[i].dst;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (a->src != t || a->dst == heir) continue;
        alias_copy(a, ((TexBackend *)a->dst->backend)->aspect, why);
        alias_drop(i);
    }
    int rename = -1;
    for (int i = 0; i < naliases; i++) if (heir && aliases[i].src == t && aliases[i].dst == heir) rename = i;
    if (rename >= 0) {
        CopyAlias *a = &aliases[rename];
        TexBackend *s = a->src->backend, *d = a->dst->backend;
        vk_end_pass();
        st.rt_dirty = true;
        VkImage im = s->image; s->image = d->image; d->image = im;
        VmaAllocation m = s->mem; s->mem = d->mem; d->mem = m;
        vk_views_forget(s);
        vk_views_forget(d);
        stat_alias_renamed++;
        if (ct_on()) LOG("[ct] alias rename %p -> %p (%s)\n", (void *)a->src, (void *)a->dst, why);
        alias_drop(rename);
    }
}

// the texture whose image holds t's contents (t itself unless it is an alias)
static NvnTexture *alias_read(NvnTexture *t) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].dst == t) {
            if (alias_valid(&aliases[i])) return aliases[i].src;
            alias_drop(i);
            return t;
        }
    return t;
}

// copy_texture: a whole-image copy that can be an alias instead (true: done)
static bool alias_make(NvnTexture *src, NvnTexture *dst, TexBackend *s, TexBackend *d) {
    // (VR: off. Aliases swap and copy textures' own images, which would mix up the eyes' separate copies.)
    if (!copy_opt() || src == dst || vr_running()) return false;
    if (!(s->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return false;
    if (s->format != d->format || s->width != d->width || s->height != d->height || s->aspect != d->aspect || s->levels != 1 ||
        d->levels != 1 || s->layers != 1 || d->layers != 1 || s->depth != 1 || d->depth != 1 || s->samples != 1 || d->samples != 1 ||
        s->base_type != d->base_type || s->presented || d->presented || !s->image || !d->image || !s->gpu_owned || !d->gpu_owned)
        return false;
    // the destination's previous contents (or alias) are replaced whole; a destination that is itself the source of
    // aliases gets those resolved first
    alias_before_write(dst, d->aspect, "copy dst");
    src = alias_read(src);  // (a copy of an alias: of its source)
    s = src->backend;
    if (src == dst) return true;  // (copied back onto itself)
    if (naliases == (int)(sizeof aliases / sizeof aliases[0])) return false;
    aliases[naliases++] = (CopyAlias){src, dst, s->image, d->image};
    vk_view_gen++;
    stat_alias_made++;
    return true;
}

// textures.c: t is being freed - an alias source hands its contents over first
void vk_alias_texture_destroyed(NvnTexture *t) {
    if (naliases) alias_before_write(t, 0, "freed");
}
void vk_alias_reset(void) { naliases = 0; }  // (reboot: every texture goes)

// (renderer_stats, once per stats interval) copies made aliases / renamed / partly or fully copied later
void vk_alias_stats(void) {
    static int n;
    if (!stat_alias_made || (n++ > 5 && n % 30)) return;
    LOG("[vk] copy alias: %llu made, %llu renamed, %llu stencil-only, %llu copied later (live %d)\n",
        (unsigned long long)stat_alias_made, (unsigned long long)stat_alias_renamed, (unsigned long long)stat_alias_partial,
        (unsigned long long)stat_alias_copied, naliases);
    stat_alias_made = stat_alias_renamed = stat_alias_partial = stat_alias_copied = 0;
}

// draws: attachments written (or a destination bound at all) end the aliases involved
static void alias_draw_check(void) {
    if (!naliases) return;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) alias_before_write(st.rt.color[i], 0, "color target");
    NvnTexture *d = st.rt.depth;
    if (!d) return;
    bool writes = (st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF));
    bool is_dst = false;
    for (int i = 0; i < naliases; i++) is_dst |= aliases[i].dst == d;
    if (writes || is_dst) alias_before_write(d, 0, writes ? "depth write" : "dst bound");
}

// (draw_, before its pass begins) A texture the draw samples was rendered by a pass since the last full barrier: end
// the pass, barrier, and let begin_pass start it again (its own attachments are the feedback logic's business).
static void pass_sample_hazard(Prog *pr) {
    if (!g_nwritten || full_barriers_only()) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!t) continue;
            NvnTexture *rt = naliases ? alias_read(t) : t;
            if ((!written_since_barrier(t) && !written_since_barrier(rt)) || (is_attachment(t) && !st.rt_dirty)) continue;
            vk_end_pass_at(-1);
            vk_full_barrier(vk_cmd());
            g_nwritten = 0;
            st.rt_dirty = true;
            stat_barrier_hazard++;
            return;
        }
}

// mv: the draw renders both eyes (VR multiview): eye 1 reads its own layer / its own snapshot.
static void resolve_feedback(Prog *pr, bool mv) {
    nfeedback = 0;
    memset(feedback_view1, 0, sizeof feedback_view1);
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
            static int copy_always = -1;
            if (copy_always < 0) copy_always = getenv("LARECOMP_FEEDBACK_COPY") != NULL;
            const bool depth_writes = (st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF));
            if (!copy_always && t == st.rt.depth && !depth_writes && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                if (g_depth_direct != t) {
                    vk_end_pass();
                    vk_full_barrier(vk_cmd());
                    g_depth_direct = t;
                }
                static const VkComponentMapping idm = {0};
                feedback_tex[nfeedback] = t;
                if (mv && src->vr_eye)
                    feedback_view1[nfeedback] = vk_image_view(src->vr_eye, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, idm,
                                                              VK_IMAGE_ASPECT_DEPTH_BIT);
                feedback_view[nfeedback++] = vk_image_view(src, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, idm,
                                                           VK_IMAGE_ASPECT_DEPTH_BIT);
                stat_feedback_direct++;
                continue;
            }
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing && (!mv || existing->copy.eye_layer)) {
                feedback_tex[nfeedback] = t;
                if (mv) feedback_view1[nfeedback] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 1, 1, id, sa);
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            if (ct_on()) LOG("[ct] snapshot %p %ux%u (was %s, si %d)\n", (void *)t, src->width, src->height, si >= 0 ? snap_why[si] : "new", si);
            vk_end_pass();
            const bool two = mv && src->eye_layer && !src->layer0;  // (both eyes' layers: VR multiview)
            TexBackend *dst = snapshot_image(t, src, two);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, src->layer0, two ? 2 : 1},
                              .dstSubresource = {src->aspect, 0, 0, two ? 2 : 1}, .extent = {src->width, src->height, 1}};
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
            if (two) feedback_view1[nfeedback] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 1, 1, id, sa);
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

// VR aiming (runtime/hle/vr_aim.c; see aim_probe): the draw being set up is an aiming item's reticle piece, drawn in
// the eye's own picture at the shot's end point. Each eye's main camera comes from its scene draws into this frame's
// largest depth target (the main view's: reflections and shadows draw into smaller ones), and eye 0's depth is that
// target.
static bool g_aim_draw;
static NvnTexture *g_aim_depth;
static uint64_t g_aim_main_frame[2] = {~0ull, ~0ull};
static uint32_t g_aim_main_w[2];
static bool aim_main_target(int eye) {
    NvnTexture *t = st.rt.depth;
    if (!t || eye < 0 || eye > 1) return false;
    const uint64_t f = renderer_present_count();
    if (g_aim_main_frame[eye] != f) g_aim_main_w[eye] = 0, g_aim_main_frame[eye] = f;
    if (t->width < g_aim_main_w[eye]) return false;
    g_aim_main_w[eye] = t->width;
    if (eye == 0) g_aim_depth = t;
    return true;
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
    if (g_settings.watercolor_filter != WATERCOLOR_ORIGINAL && stage == 1 && idx == 2) {
        NvnTexture *t = si->ntex ? shader_texture_object(1, &si->tex[0]) : NULL;
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        float local[SKY_PAINT_PARAMS_MAX / sizeof(float)];
        if (t && b && sky_paint_filter_parameters(local, (const void *)(uintptr_t)r.addr, size, g_cur_fs_hash,
                g_settings.watercolor_filter, t->width, t->height, b->width, b->height))
            return ubo_span(vk_transient_copy(local, size, 256), size);
    }
    // VR stereo: the scene camera block becomes the recording eye's camera in this draw's own copy. Vertex shaders
    // conventionally bind it at uniform slot 0, but fragment shaders do not: the water surface, for example, binds
    // its camera at slot 2 while using it to project the reflection/refraction scene textures. Probe every fragment
    // uniform through the strict camera classifier so those screen-space effects use the same eye as their geometry.
    // Guest memory keeps the game's mono camera.
    if (g_aim_draw && stage == 0 && idx == 0 && size >= 96) {  // VR aiming: a reticle piece, moved to the shot's end point
        extern bool vr_aim_shift(int eye, const float proj[16], float delta[2]);
        const float *u = (const float *)(uintptr_t)r.addr;
        float dl[2];
        if (vr_aim_shift((int)vk_vr_eye(), u, dl)) {
            VkSpan s = vk_transient_copy(u, size, 256);
            ((float *)s.ptr)[19] += dl[0];  // (the pane's matrix, rows at 16 and 20: its translation)
            ((float *)s.ptr)[23] += dl[1];
            return ubo_span(s, size);
        }
    }
    if (!g_draw_screen_composite && (stage == 0 || stage == 1) && (stage == 1 || idx == 0) &&
        size >= 28 * sizeof(float) && vr_stereo_active()) {
        float camera[28];
        const float *source = (const float *)(uintptr_t)r.addr;
        const bool camera_patched = stage == 1 ? vr_patch_scene_fragment_ubo(vk_vr_eye(), camera, source, size)
                                               : vr_patch_scene_ubo(vk_vr_eye(), camera, source, size);
        if (camera_patched && stage == 0) {  // VR aiming (runtime/hle/vr_aim.c): the main view's camera per eye
            float ho[12], hr[12];  // (not a hand's or the door's draw: its block was replaced before the eye's)
            if (!vr_rig_hand_blocks(ho, hr) && aim_main_target((int)vk_vr_eye())) {
                extern void vr_aim_camera(int eye, const float eye_block[28], const float game_block[28]);
                vr_aim_camera((int)vk_vr_eye(), camera, source);
            }
        }
        if (camera_patched) {
            if (!vk_vr_eye()) g_stereo_camera_patched = true;
            // (a draw seen through a per-eye camera: this frame has a 3D scene - also the sky-only shots the
            // projection test in hud_mark_scene_targets misses; else they count as full-screen 2D, copied to the panel)
            g_last_scene_frame = renderer_present_count(), g_scene_since_present = true;
            uint8_t local[0x800];
            VkSpan patched;
            if (size <= sizeof local) {
                memcpy(local, (const void *)(uintptr_t)r.addr, size);
                memcpy(local, camera, sizeof camera);
                float orig[12], repl[12];  // (Link's hand: its view block again past the camera - see below)
                if (vr_rig_hand_blocks(orig, repl)) {
                    float *f = (float *)local;
                    for (int o = 28; o + 12 <= (int)(size / 4); o += 4) {
                        bool same = true;
                        for (int i = 0; same && i < 12; i++) same = fabsf(f[o + i] - orig[i]) <= 1e-4f * (1.0f + fabsf(orig[i]));
                        if (!same) continue;
                        static int said;
                        if (said++ < 4) LOG("[vr] hand: stage %d slot 0 offset %d holds its view block\n", stage, o * 4);
                        memcpy(f + o, repl, sizeof repl);
                        o += 8;
                    }
                }
                const int slot = (stage ? DEDUPE_CAMERA_F0 : DEDUPE_CAMERA_V0) + (int)vk_vr_eye();
                patched = copy_dedupe(slot, local, size, 256);
            } else {
                patched = vk_transient(size, 256);
                memcpy(patched.ptr, (const void *)(uintptr_t)r.addr, size);
                memcpy(patched.ptr, camera, sizeof camera);
            }
            return ubo_span(patched, size);
        }
    }
    if (stage <= 1 && idx >= 1 && vr_stereo_active() && size >= 48 && size <= 0x2000) {
        // Link's hand on its controller (vr_rig.c): wherever another block holds the hand's game view block (its
        // normals' matrix, for the lighting), the controller's replaces it too - else the light followed the game's
        // hand (his arm swinging as he walks) instead of the hand seen.
        float orig[12], repl[12];
        if (vr_rig_hand_blocks(orig, repl)) {
            const float *b = (const float *)(uintptr_t)r.addr;
            const int n = (int)(size / 4);
            float *out = NULL;
            VkSpan patched = {0};
            // (measured: vertex slot 1 holds its rotation alone - the view block with no translation - at float 24:
            // the normals' matrix; a spheremap's follows. The whole block is matched too, should a shader carry it.)
            for (int o = 0; o + 12 <= n; o += 4) {
                bool rot = true, whole = true;
                for (int i = 0; rot && i < 12; i++) {
                    if ((i & 3) == 3) continue;
                    rot = fabsf(b[o + i] - orig[i]) <= 1e-4f * (1.0f + fabsf(orig[i]));
                }
                if (!rot) continue;
                for (int i = 3; whole && i < 12; i += 4) whole = fabsf(b[o + i] - orig[i]) <= 1e-4f * (1.0f + fabsf(orig[i]));
                const bool rot_only = !whole && b[o + 3] == 0 && b[o + 7] == 0 && b[o + 11] == 0;
                if (!whole && !rot_only) continue;
                if (!out) {
                    patched = vk_transient(size, 256);
                    memcpy(patched.ptr, b, size);
                    out = patched.ptr;
                    static int said;
                    if (said++ < 4) LOG("[vr] hand: stage %d slot %d offset %d holds its %s\n", stage, idx, o * 4,
                                        whole ? "view block" : "rotation");
                }
                for (int i = 0; i < 12; i++) out[o + i] = (i & 3) == 3 ? (whole ? repl[i] : 0.0f) : repl[i];
                o += 8;
            }
            if (out) return ubo_span(patched, size);
        }
    }
    if (stage == 0 && idx >= 1 && g_draw_samples_screen && vr_stereo_active() && size >= 96 && size <= 0x2000) {
        // Screen-space texture projections (water surfaces sampling the scene-colour copies, ...; only draws sampling
        // a screen-sized GPU-written texture: others project into the game's own small mono maps): a vertex block
        // beside the camera that holds the game camera's view W (= slot 0's) followed by T = S P (u, v, w: a screen
        // scale-bias times the game's projection). Slot 0 gets the eye's P' (= Pe C, the rig), so T' = S P' = T P^-1 P'
        // projects each vertex where this eye sees it and the copy (this eye's own) is sampled there. Only T whose
        // T P^-1 is a pure scale-bias is changed. SKYRECOMP_VR_NO_SCREEN_PROJ=1: unchanged (the earlier behaviour).
        static int off = -1;
        if (off < 0) off = getenv("SKYRECOMP_VR_NO_SCREEN_PROJ") != NULL;
        static const char *skip_list = (const char *)1;  // (test) SKYRECOMP_VR_SCREEN_PROJ_SKIP=<vs hex>,...
        bool skip_this = false;
        if (skip_list == (const char *)1) skip_list = getenv("SKYRECOMP_VR_SCREEN_PROJ_SKIP");
        if (skip_list && st.program) {
            char hex[24];
            snprintf(hex, sizeof hex, "%llx", (unsigned long long)st.program->stages[0].code_addr);
            if (strstr(skip_list, hex)) skip_this = true;
        }
        const Range c0 = st.ubuf[0][0];
        const float *mono = !off && !skip_this && c0.addr && c0.size >= 28 * sizeof(float) ? (const float *)(uintptr_t)c0.addr : NULL;
        extern bool sky_vr_is_scene_projection(const float *ubo, uint64_t bytes);
        float pinv[16], eye_cam[28];
        bool have_eye = false;
        float *out = NULL;
        VkSpan patched = {0};
        const float *b = (const float *)(uintptr_t)r.addr;
        const int n = (int)(size / 4);
        for (int o = 0; mono && o + 24 <= n; o += 4) {
            bool same = true;
            for (int i = 0; same && i < 12; i++) same = fabsf(b[o + i] - mono[16 + i]) <= 1e-4f * (1.0f + fabsf(mono[16 + i]));
            if (!same) continue;
            if (!have_eye) {
                if (!sky_vr_is_scene_projection(mono, c0.size) || !m4_invert(mono, pinv) ||
                    !vr_patch_scene_ubo(vk_vr_eye(), eye_cam, mono, c0.size)) break;
                have_eye = true;
            }
            const float *t = b + o + 12;
            float s[12];  // S = T P^-1
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++) {
                    float v = 0;
                    for (int k2 = 0; k2 < 4; k2++) v += t[row * 4 + k2] * pinv[k2 * 4 + col];
                    s[row * 4 + col] = v;
                }
            float scale = 0;
            for (int i = 0; i < 12; i++) scale = fmaxf(scale, fabsf(s[i]));
            // (u from x and w, v from y and w, w from w.) The depth column (s[2], s[6], s[10]) is not tested: the
            // game's projection has a tiny depth row (reversed Z, far plane far away), so P^-1 puts float noise
            // there that crossed any threshold on some frames - the surface flickered between both projections.
            // It is zero for a screen projection, and is set so.
            const float z = 1e-3f * scale;
            if (!(scale > 0) || fabsf(s[1]) > z || fabsf(s[4]) > z || fabsf(s[8]) > z || fabsf(s[9]) > z || !(fabsf(s[11]) > z))
                continue;
            s[2] = s[6] = s[10] = 0;
            if (!out) {
                patched = vk_transient(size, 256);
                memcpy(patched.ptr, b, size);
                out = patched.ptr;
                const uint64_t vs_code = st.program ? st.program->stages[0].code_addr : 0;
                static uint64_t said[32];
                static int nsaid;
                bool known = false;
                for (int i = 0; i < nsaid; i++) known |= said[i] == vs_code;
                if (!known && nsaid < 32) {
                    said[nsaid++] = vs_code;
                    LOG("[vr] screen projection per eye: vs %llx slot %d offset %d\n", (unsigned long long)vs_code, idx, o * 4);
                }
            }
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++) {
                    float v = 0;
                    for (int k2 = 0; k2 < 4; k2++) v += s[row * 4 + k2] * eye_cam[k2 * 4 + col];
                    out[o + 12 + row * 4 + col] = v;
                }
            o += 20;  // (past this W and T)
        }
        if (out) return ubo_span(patched, size);
    }
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Guest heap as GPU buffers (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer. An import pins its
// pages - the driver faults in every page of the range - so what is imported is resident whether the game touched it
// or not, where Metal's newBufferWithBytesNoCopy leaves untouched pages alone. Importing the whole heap the game set up
// (SetHeapSize, ~3 GB) at the first draw put the process RSS at ~4.2 GB on Linux against ~1.1 GB on macOS. So two kinds:
//  - gmem_buf, the whole heap: only for shader global memory (LDG/STG, maxwell_gmem_base = HEAP_BASE: one SSBO the
//    shader indexes by guest address, which can't be split), made when a shader that uses it first runs, so compute
//    results land in guest memory.
//  - chunks (gch): vertex / index data (guest_direct) is read from 32 MB slices of the heap imported on first use, so
//    only the slices the game actually draws from get pinned.
// LARECOMP_VK_IMPORT_HEAP=1: import the whole heap at the first draw and serve everything from it (the old behaviour).
// LARECOMP_VK_NO_GMEM=1: no imports (global memory reads return zero, vertices / indices are copied).
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;
static int gmem_off = -1;  // 1: imports unavailable or switched off
static bool gmem_failed;   // the whole-heap import failed (chunks may still work: they are far smaller)

static bool gmem_disabled(void) {
    if (gmem_off < 0) gmem_off = getenv("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import;
    return gmem_off;
}

// Host memory [HEAP_BASE + lo, + size) as a buffer (lo and size page multiples). Nothing stays allocated on failure.
static bool host_import(uint64_t lo, uint64_t size, VkBuffer *buf, VkDeviceMemory *mem) {
    void *host = (void *)(uintptr_t)(HEAP_BASE + lo);
    plat_commit(HEAP_BASE + lo, size);  // An external host-memory import must cover committed pages.
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &mp) != VK_SUCCESS || !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = host};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, *mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, *buf, *mem, 0);
    return true;
}

static bool gmem_import(uint64_t size, const char *why) {
    VkBuffer buf;
    VkDeviceMemory mem;
    if (!host_import(0, size, &buf, &mem)) return false;
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] whole guest heap imported (%s): %llu MB resident\n", why, (unsigned long long)(size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (guest_src re-imports in the eager mode)

// The whole heap for a shader's global memory (why: for the log - what made the first import).
static VkDescriptorBufferInfo gmem_info(const char *why) {
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!gmem_disabled() && !gmem_failed && want > gmem_size && !gmem_import(want, why)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        gmem_failed = true;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Chunk c imports heap offsets [c * GCH_SIZE, + GCH_SIZE + GCH_TAIL). The tail overlaps the next chunk (its pages are
// shared, nothing is pinned twice), so a buffer starting near the end of a chunk is still served by it: only bindings
// longer than the tail that start late in a chunk fall back to copies. 32 MB: a smaller slice wastes less on pages the
// game never touched, a larger one imports less often (each import is an allocation the driver pins up front).
// Never destroyed before a reboot (vk_exec_reset): submitted work and vertex cache entries refer to them.
#define GCH_SHIFT 25
#define GCH_SIZE (1ull << GCH_SHIFT)
#define GCH_TAIL (8ull << 20)
#define GCH_N (HEAP_MAX >> GCH_SHIFT)
static struct { VkBuffer buf; VkDeviceMemory mem; bool bad; } gch[GCH_N];
static uint64_t gch_bytes;  // imported so far (the log)

static bool gch_import(uint64_t c) {
    uint64_t lo = c << GCH_SHIFT, len = GCH_SIZE + GCH_TAIL;
    if (len > HEAP_MAX - lo) len = HEAP_MAX - lo;
    if (!host_import(lo, len, &gch[c].buf, &gch[c].mem)) {
        gch[c].bad = true;  // (not retried at every draw)
        LOG("[vk] guest heap chunk %llu import failed: its vertices / indices are copied\n", (unsigned long long)c);
        return false;
    }
    gch_bytes += len;
    LOG("[vk] guest heap chunk %llu (offset %llu MB) imported for vertex / index data: %llu MB in chunks\n", (unsigned long long)c,
        (unsigned long long)(lo >> 20), (unsigned long long)(gch_bytes >> 20));
    return true;
}

// The zero-copy source of guest memory [addr, addr + size), addr >= HEAP_BASE: its buffer, and that buffer's heap
// offset *base. The whole-heap import when there is one, else the chunk holding addr (imported on first use). False:
// copy the data instead (outside the heap the game set up, a binding running past its chunk's tail, imports off or
// failed). Sources are page-aligned at both ends, so a page-rounded range inside the binding is inside the source too.
static bool guest_src(uint64_t addr, uint64_t size, VkBuffer *buf, uint64_t *base) {
    static int eager = -1;
    if (eager < 0) eager = getenv("LARECOMP_VK_IMPORT_HEAP") != NULL;
    if (gmem_disabled()) return false;
    if (eager && addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) {
        gmem_tried = kernel_heap_size();
        gmem_info("LARECOMP_VK_IMPORT_HEAP");
    }
    if (gmem_buf && addr + size <= HEAP_BASE + gmem_size) {  // (a shader made the whole import: one buffer for everything)
        *buf = gmem_buf;
        *base = 0;
        return true;
    }
    if (eager) return false;
    uint64_t lo = addr - HEAP_BASE, c = lo >> GCH_SHIFT, cb = c << GCH_SHIFT;
    if (lo >= HEAP_MAX || size > HEAP_MAX || lo + size > kernel_heap_size() || lo - cb + size > GCH_SIZE + GCH_TAIL) return false;
    if (!gch[c].buf && (gch[c].bad || !gch_import(c))) return false;
    *buf = gch[c].buf;
    *base = cb;
    return true;
}

// Compute dispatches that use global memory: the imported heap is host memory across PCIe on a discrete GPU, and a
// shader storing to it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~8 ms a frame on a GTX 1660
// Super and on an RTX 3080 Ti alike, which the game then waits for on its fence - 49-53 fps there on the 1660). Before
// such a dispatch its bound storage buffers (sorted, merged) are copied into a device-local buffer, the address table
// (binding GMAP_BUFFER_INDEX) tells the shader (translate.c, GLSL compute) where each range sits, loads and stores
// inside them use that copy, and a shader that stores gets the ranges copied back to the heap right after it (the
// game alternates its buffer sets per frame: the CPU isn't writing them meanwhile). Accesses outside the table use
// the heap as before. LARECOMP_VK_GMEM_FAST=0: no copy (empty table: everything goes to the heap, the old behaviour).
#define GFAST_SIZE (32ull << 20)
static VkBuffer gfast_buf;
static VmaAllocation gfast_mem;
static uint32_t gmap_tab[(GMAP_MAX + 1) * 4];  // this dispatch's table (stage_set copies it into binding 20)
static VkBufferCopy gfast_cp[GMAP_MAX];       // its copies heap -> gfast_buf (reversed after a storing dispatch)
static int gfast_n;

static VkDescriptorBufferInfo gfast_info(void) {
    if (!gfast_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GFAST_SIZE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gfast_buf, &gfast_mem, NULL));
    }
    return (VkDescriptorBufferInfo){gfast_buf, 0, GFAST_SIZE};
}

static void gfast_prepare(VkCommandBuffer cb) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_GMEM_FAST"); on = !e || atoi(e) != 0; }
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
    gmem_info("compute shader with global memory");  // (the whole-heap import, made on first use)
    gfast_info();
    if (!on || !gmem_buf) return;
    struct { uint64_t lo, hi; } r[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {  // heap offsets of the bound storage buffers, 256-aligned
        uint64_t a = st.sbuf[5][i].addr, sz = st.sbuf[5][i].size;
        if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE + sz > gmem_size) continue;
        uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
        if (hi > gmem_size) hi = gmem_size;
        int k = n++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }  // (insertion sort by start)
        r[k].lo = lo; r[k].hi = hi;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merge overlapping / nearby ranges (a small gap costs less than another table entry)
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    VkBufferCopy cp[16];
    uint64_t off = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        if (m > GMAP_MAX || off + len > GFAST_SIZE) return;  // (too much: the heap as before)
        cp[i] = (VkBufferCopy){r[i].lo, off, len};
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        off += (len + 255) & ~255ull;
    }
    if (!m) return;
    if (getenv("LARECOMP_LOG_GFAST")) {  // (debug) =<shader code address hex>: that shader's first tables
        static int nlog;
        if (st.program->stages[5].code_addr == strtoull(getenv("LARECOMP_LOG_GFAST"), NULL, 16) && nlog++ < 40) {
            LOG("[gfast] cs %llx: %d ranges, %llu KB:", (unsigned long long)st.program->stages[5].code_addr, m, (unsigned long long)(off >> 10));
            for (int i = 0; i < m; i++) LOG(" %llx+%llx", (unsigned long long)(r[i].lo + HEAP_BASE), (unsigned long long)(r[i].hi - r[i].lo));
            LOG("\n");
        }
    }
    vkCmdCopyBuffer(cb, gmem_buf, gfast_buf, (uint32_t)m, cp);
    vk_full_barrier(cb);
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
    memcpy(gfast_cp, cp, (size_t)m * sizeof cp[0]);
    gfast_n = m;
}

// After a storing dispatch: its ranges back to the heap (before anything else reads them: draws, the CPU after the fence).
static void gfast_finish(VkCommandBuffer cb) {
    if (!gfast_n) return;
    VkBufferCopy back[GMAP_MAX];
    for (int i = 0; i < gfast_n; i++) back[i] = (VkBufferCopy){gfast_cp[i].dstOffset, gfast_cp[i].srcOffset, gfast_cp[i].size};
    vk_full_barrier(cb);
    vkCmdCopyBuffer(cb, gfast_buf, gmem_buf, (uint32_t)gfast_n, back);
    vk_full_barrier(cb);
    gfast_n = 0;
}

// ---- vertex / index cache
// Reading vertex / index data in place from the imported heap crosses PCIe for every vertex fetched on a discrete GPU
// (outdoors at 3x internal resolution on an RTX 3080 Ti: ~0.7-1.9 ms of GPU a frame; more on PCIe 3.0), and copying it
// per draw instead moves about as much onto the CPU. So each range bound for drawing gets a device-local copy, refreshed
// only where the CPU wrote since (the OS's page write tracking, plat_written_pages, asked at submission: vc_flush): one
// sequential GPU copy of the written pages in the submission's upload buffer (which completes before its draws) instead
// of fetches across the bus. Overlapping ranges are separate entries; the per-page generation keeps them all current.
// Pages the GPU writes itself (a compute dispatch's copy-back, gfast_finish) are never cached: draws over them read the
// heap in place as before, and so does everything when the OS gives no write tracking. LARECOMP_VK_VCACHE=0: off.
#define VC_PAGE 4096ull
#define VC_TAB 8192                   // entries (open addressing)
#define VC_MAX_RANGE (64ull << 20)    // larger bound ranges: read in place
#define VC_BUDGET (768ull << 20)      // device-local bytes kept; the least recently used go first
// (src / sbase: the imported chunk the copies are made from and its heap offset; src null: the whole-heap import, base 0)
typedef struct { uint64_t lo, hi, used_seq, sbase; uint32_t gen, epoch; VkBuffer buf, src; VmaAllocation mem; bool gpu; } VcEnt;
static VcEnt vc_tab[VC_TAB];
static uint32_t vc_pend[VC_TAB];  // entries the pending submission draws from (vc_flush brings them up to date)
static int vc_npend;
static uint32_t *vc_page_gen;  // per heap page: the generation of its last CPU write seen (vc_gen when observed)
static uint8_t *vc_page_gpu;   // per heap page: written by the GPU
static uint32_t vc_gen = 1, vc_epoch = 1;  // (epoch: bumped when pages turn GPU-written: entries check again)
static uint64_t vc_bytes;
#define VC_DEAD 1024
static struct { VkBuffer b; VmaAllocation m; uint64_t seq; } vc_dead[VC_DEAD];
static int vc_ndead;
static int vc_on = -1;
// (profiler: draws served / read in place, bytes refreshed, write tracking queries, written pages seen, query time)
uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;

static void vc_free_dead(bool all) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < vc_ndead; i++) {
        if (!all && vc_dead[i].seq > done) { vc_dead[k++] = vc_dead[i]; continue; }
        vmaDestroyBuffer(g_vk.vma, vc_dead[i].b, vc_dead[i].m);
    }
    vc_ndead = k;
}

static bool vc_drop(VcEnt *e) {  // (freed once the submissions that may read it are done)
    if (vc_ndead == VC_DEAD) { vk_wait_seq(vk_pending_seq() - 1); vc_free_dead(false); }
    if (vc_ndead == VC_DEAD) return false;  // (all read by the pending submission: keep it)
    vc_dead[vc_ndead].b = e->buf; vc_dead[vc_ndead].m = e->mem; vc_dead[vc_ndead++].seq = e->used_seq;
    vc_bytes -= e->hi - e->lo;
    memset(e, 0, sizeof *e);
    return true;
}

// The page tables (vc_page_gen / vc_page_gpu) exist whenever the OS gives write tracking: the vertex cache and the
// texture upload checks (vk_pages_changed) share them. Each write tracking query resets the OS bits, so every query
// stamps the pages it saw written with a new generation, and each user compares against the generation it last saw.
static int pt_state = -1;  // 1: tracking, 0: none
static bool pt_init(void) {
    if (pt_state >= 0) return pt_state;
    uint64_t pg = VC_PAGE, one;
    int n = plat_written_pages(HEAP_BASE, VC_PAGE, &one, 1, &pg);  // (is there write tracking?)
    pt_state = n >= 0 && pg == VC_PAGE;
    if (!pt_state) { LOG("[vk] no page write tracking (%d, page %llu)\n", n, (unsigned long long)pg); return false; }
    vc_page_gen = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_gen);
    vc_page_gpu = calloc(HEAP_MAX / VC_PAGE, 1);
    vc_page_gen[0] = ++vc_gen;  // (the probe took page 0's tracking)
    return true;
}

// Texture upload checks (textures.c): may guest memory [addr, addr + size) have changed since generation `since`?
// 0: no CPU write since, 1: written, -1: unknown (no tracking, outside the heap, or pages the GPU writes).
// *now: the generation to pass next time.
int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (!size || addr < HEAP_BASE || addr - HEAP_BASE + size > HEAP_MAX || !pt_init()) return -1;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    static uint64_t pages[4096];
    uint64_t pg = VC_PAGE;
    int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
    if (n < 0) return -1;
    uint32_t g = ++vc_gen;
    if (n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;
    else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
    *now = g;
    int changed = 0;
    for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) {
        if (vc_page_gpu[p]) return -1;
        if (vc_page_gen[p] > since) changed = 1;
    }
    return changed;
}

// Heap pages [lo, hi) (offsets) written by the GPU: never cached from now on.
static void vc_mark_gpu(uint64_t lo, uint64_t hi) {
    if (!pt_init()) return;
    for (uint64_t p = lo / VC_PAGE; p < (hi + VC_PAGE - 1) / VC_PAGE && p < HEAP_MAX / VC_PAGE; p++) vc_page_gpu[p] = 1;
    vc_epoch++;
}

// A storing shader (stage 0 / 1 / 5) may write any of its bound storage buffers.
static void vc_mark_sbufs(int stage) {
    for (int i = 0; i < 16; i++) {
        uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
        if (a >= HEAP_BASE && sz && a - HEAP_BASE < HEAP_MAX) vc_mark_gpu(a - HEAP_BASE, a - HEAP_BASE + sz);
    }
}

static bool vcache_lookup(uint64_t addr, uint64_t size, VkBuffer src, uint64_t sbase, VkBuffer *buf, VkDeviceSize *off) {
    if (vc_on < 0) {
        const char *e = getenv("LARECOMP_VK_VCACHE");
        vc_on = !e || atoi(e) != 0;
        if (vc_on && !pt_init()) {
            LOG("[vk] vertex cache off: no page write tracking\n");
            vc_on = 0;
        }
    }
    if (!vc_on || size > VC_MAX_RANGE) return false;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    uint64_t pend = vk_pending_seq();
    uint64_t h = (lo * 0x9E3779B97F4A7C15ull) ^ (hi * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VC_TAB - 1), free_slot = ~0u, old_slot = ~0u;
    VcEnt *e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VC_TAB - 1)) {
        VcEnt *c = &vc_tab[slot];
        if (c->buf && c->lo == lo && c->hi == hi) { e = c; break; }
        if (!c->buf && free_slot == ~0u) free_slot = slot;
        if (c->buf && c->used_seq < pend && (old_slot == ~0u || c->used_seq < vc_tab[old_slot].used_seq)) old_slot = slot;
    }
    if (!e) {  // a new range: its own device-local buffer (gen 0: copied whole at the submission)
        if (vc_npend == VC_TAB) return false;
        if (vc_ndead) vc_free_dead(false);
        if (free_slot == ~0u) {
            if (old_slot == ~0u || !vc_drop(&vc_tab[old_slot])) return false;
            free_slot = old_slot;
        }
        while (vc_bytes + (hi - lo) > VC_BUDGET) {  // over budget: the least recently used entry goes
            int lru = -1;
            for (int i = 0; i < VC_TAB; i++)
                if (vc_tab[i].buf && vc_tab[i].used_seq < pend && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
            if (lru < 0 || !vc_drop(&vc_tab[lru])) return false;
        }
        e = &vc_tab[free_slot];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = hi - lo,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &e->buf, &e->mem, NULL) != VK_SUCCESS) { memset(e, 0, sizeof *e); return false; }
        e->lo = lo; e->hi = hi; e->sbase = sbase;
        e->src = src == gmem_buf ? VK_NULL_HANDLE : src;  // (null: the whole-heap import, which a grown heap replaces)
        vc_bytes += hi - lo;
    }
    if (e->epoch != vc_epoch) {  // (pages turned GPU-written since: is this range one of them?)
        e->gpu = false;
        for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE && !e->gpu; p++) e->gpu = vc_page_gpu[p];
        e->epoch = vc_epoch;
    }
    if (e->gpu) { prof_vc_bypass++; return false; }
    if (e->used_seq != pend) {  // first use in this submission: brought up to date by vc_flush before it runs
        e->used_seq = pend;
        vc_pend[vc_npend++] = (uint32_t)(e - vc_tab);
    }
    prof_vc_hits++;
    *buf = e->buf;
    *off = addr - HEAP_BASE - lo;
    return true;
}

static int vc_pend_cmp(const void *a, const void *b) {
    uint64_t x = vc_tab[*(const uint32_t *)a].lo, y = vc_tab[*(const uint32_t *)b].lo;
    return x < y ? -1 : x > y;
}

// At submission (vk_submit_extra, before the upload buffer closes): the pages the CPU wrote in the ranges this
// submission draws from - one write tracking query per run of nearby ranges (a query costs about as much as scanning
// ~64 pages: ranges closer than that share one) - and their copies into the cache, ahead of the draws.
void vc_flush(void) {
    if (!vc_npend) return;
    qsort(vc_pend, (size_t)vc_npend, sizeof vc_pend[0], vc_pend_cmp);
    uint32_t g = ++vc_gen;
    static uint64_t pages[4096];
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < vc_npend;) {
        uint64_t lo = vc_tab[vc_pend[i]].lo, hi = vc_tab[vc_pend[i]].hi;
        int j = i + 1;
        for (; j < vc_npend && vc_tab[vc_pend[j]].lo <= hi + 64 * VC_PAGE; j++)
            if (vc_tab[vc_pend[j]].hi > hi) hi = vc_tab[vc_pend[j]].hi;
        uint64_t pg = VC_PAGE;
        int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
        prof_vc_queries++;
        if (n < 0 || n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;  // (all)
        else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
        if (n > 0) prof_vc_scan_pages += (uint64_t)n;
        i = j;
    }
    prof_vc_scan_ns += SDL_GetTicksNS() - t0;
    VkCommandBuffer cb = vk_upload_cmd();
    for (int i = 0; i < vc_npend; i++) {
        VcEnt *e = &vc_tab[vc_pend[i]];
        VkBufferCopy cp[64];
        int nc = 0;
        for (uint64_t p = e->lo / VC_PAGE; p < e->hi / VC_PAGE; p++) {
            if (e->gen && vc_page_gen[p] <= e->gen) continue;
            uint64_t o = p * VC_PAGE - e->sbase;  // (offset in the source buffer)
            if (nc && cp[nc - 1].srcOffset + cp[nc - 1].size == o) { cp[nc - 1].size += VC_PAGE; continue; }
            if (nc == 64) { nc = 1; cp[0] = (VkBufferCopy){e->lo - e->sbase, 0, e->hi - e->lo}; break; }  // (scattered: the whole range)
            cp[nc++] = (VkBufferCopy){o, p * VC_PAGE - e->lo, VC_PAGE};
        }
        if (nc) {
            vkCmdCopyBuffer(cb, e->src ? e->src : gmem_buf, e->buf, (uint32_t)nc, cp);
            for (int k = 0; k < nc; k++) prof_vc_copied += cp[k].size;
        }
        e->gen = g;
    }
    vc_npend = 0;
}

// Vertex / index data from the imported guest heap (renderer.m bind_guest reads g_guest_buffer the same way): the
// device-local cache above, else read in place from the chunk of the heap (guest_src) holding it - no per-draw copy, no
// index scan for the copy size. The game fences its CPU writes as on the Switch. Falls back to copies outside the heap,
// past a chunk's tail or without the import; LARECOMP_VK_COPY_VERTICES=1 forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) {
        copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
#ifdef _WIN32
        MEMORYSTATUSEX memory = {.dwLength = sizeof memory};
        // WDDM charges another heap-sized commitment for this optional zero-copy import.
        if (!copy && GlobalMemoryStatusEx(&memory) && memory.ullAvailPageFile < kernel_heap_size() * 2 + (4ull << 30)) {
            copy = 1;
            LOG("[vk] using copied vertices to preserve Windows memory headroom\n");
        }
#endif
    }
    if (copy || addr < HEAP_BASE) return false;
    VkBuffer src;
    uint64_t base;
    if (!guest_src(addr, size, &src, &base)) return false;
    if (vcache_lookup(addr, size, src, base, buf, off)) return true;
    *buf = src;
    *off = addr - HEAP_BASE - base;
    return true;
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap imports (whole and chunks): their pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_cs_n = 0;
    gcopy_gen++;
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    gmem_size = gmem_tried = 0;
    gmem_failed = false;
    for (int i = 0; i < (int)GCH_N; i++) {
        if (gch[i].buf) {
            vkDestroyBuffer(g_vk.dev, gch[i].buf, NULL);
            vkFreeMemory(g_vk.dev, gch[i].mem, NULL);
        }
    }
    memset(gch, 0, sizeof gch);
    gch_bytes = 0;
    for (int i = 0; i < VC_TAB; i++)  // (the vertex cache: copies of the old pages)
        if (vc_tab[i].buf) vmaDestroyBuffer(g_vk.vma, vc_tab[i].buf, vc_tab[i].mem);
    memset(vc_tab, 0, sizeof vc_tab);
    vc_npend = 0;
    vc_free_dead(true);
    vc_bytes = 0;
    if (vc_page_gpu) memset(vc_page_gpu, 0, HEAP_MAX / VC_PAGE);
    vc_epoch++;
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
            // (eye: a VR eye-1 recording samples the eye's own images of rendered textures)
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view, eye; VkImageView view; } memo[1024];
            const uint8_t eye = (uint8_t)vk_vr_eye();
            // A deferred depth copy can change which image owns this texture's
            // contents within the same frame. Cache the resolved source, not
            // the destination object (otherwise a shadow lookup can reuse old depth).
            NvnTexture *etex = hud_sub(e->tex);
            NvnTexture *read_tex = naliases ? alias_read(etex) : etex;
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)read_tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull ^
                                      (uint64_t)eye << 9) & 1023);
            if (!nomemo && memo[ms].tex == read_tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view && memo[ms].eye == eye) {
                view = memo[ms].view;
            } else {
                view = vk_view(read_tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = read_tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].eye = eye; memo[ms].view = view;
            }
            renderer_texture_scale(etex, &sc[0], &sc[1]);
            wh[0] = etex->width; wh[1] = etex->height;
            for (int k = 0; k < nfeedback; k++)
                if (etex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = vk_vr_eye() && feedback_view1[k] ? feedback_view1[k] : feedback_view[k];
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
static float g_mirror_w;  // mirror mode's composite: its render target width (rscale[65].x; 0 otherwise)

// Descriptor contents repeat heavily across adjacent draws (especially the fragment set). Keep sets for the
// lifetime of one submission: its pool, transient-buffer offsets and deferred image views all remain valid for
// exactly that interval. Keys are compared byte-for-byte; the hash only selects an open-addressed table slot.
typedef struct {
    uint32_t binding, type;
    VkBuffer buffer;
    VkDeviceSize offset, range;
    VkSampler sampler;
    VkImageView view;
    VkImageLayout image_layout;
} DescriptorKeyItem;
typedef struct { uint64_t hash, seq; VkDescriptorSetLayout layout; uint32_t count, key_off; VkDescriptorSet set; } DescriptorCacheEntry;
#define DESCRIPTOR_CACHE_SIZE 8192
static DescriptorCacheEntry descriptor_cache[DESCRIPTOR_CACHE_SIZE];
static DescriptorKeyItem *descriptor_keys;
static uint32_t descriptor_keys_used, descriptor_keys_cap;
static uint64_t descriptor_cache_seq;

static uint64_t descriptor_hash(VkDescriptorSetLayout layout, const DescriptorKeyItem *key, uint32_t count) {
    uint64_t h = hash_words(0xD6E8FEB86659FD93ull, &layout, sizeof layout);
    h = hash_words(h, &count, sizeof count);
    return hash_words(h, key, count * sizeof *key) | 1;  // zero denotes an unused table entry
}

static VkDescriptorSet descriptor_cache_find(VkDescriptorSetLayout layout, const DescriptorKeyItem *key, uint32_t count,
                                             uint64_t hash) {
    const uint64_t seq = vk_pending_seq();
    if (descriptor_cache_seq != seq) {
        memset(descriptor_cache, 0, sizeof descriptor_cache);
        descriptor_keys_used = 0;
        descriptor_cache_seq = seq;
    }
    uint32_t slot = (uint32_t)(hash ^ (hash >> 32)) & (DESCRIPTOR_CACHE_SIZE - 1);
    for (uint32_t probe = 0; probe < DESCRIPTOR_CACHE_SIZE; probe++, slot = (slot + 1) & (DESCRIPTOR_CACHE_SIZE - 1)) {
        DescriptorCacheEntry *e = &descriptor_cache[slot];
        if (!e->hash) return VK_NULL_HANDLE;
        if (e->seq == seq && e->hash == hash && e->layout == layout && e->count == count &&
            (!count || !memcmp(descriptor_keys + e->key_off, key, count * sizeof *key)))
            return e->set;
    }
    return VK_NULL_HANDLE;
}

static void descriptor_cache_add(VkDescriptorSetLayout layout, const DescriptorKeyItem *key, uint32_t count,
                                 uint64_t hash, VkDescriptorSet set) {
    if (descriptor_keys_used + count > descriptor_keys_cap) {
        uint32_t cap = descriptor_keys_cap ? descriptor_keys_cap * 2 : 4096;
        while (cap < descriptor_keys_used + count) cap *= 2;
        DescriptorKeyItem *next = realloc(descriptor_keys, cap * sizeof *next);
        if (!next) return;  // caching is optional; the newly allocated descriptor set remains valid
        descriptor_keys = next;
        descriptor_keys_cap = cap;
    }
    uint32_t slot = (uint32_t)(hash ^ (hash >> 32)) & (DESCRIPTOR_CACHE_SIZE - 1);
    for (uint32_t probe = 0; probe < DESCRIPTOR_CACHE_SIZE; probe++, slot = (slot + 1) & (DESCRIPTOR_CACHE_SIZE - 1)) {
        DescriptorCacheEntry *e = &descriptor_cache[slot];
        if (e->hash) continue;
        if (count) memcpy(descriptor_keys + descriptor_keys_used, key, count * sizeof *key);
        *e = (DescriptorCacheEntry){hash, vk_pending_seq(), layout, count, descriptor_keys_used, set};
        descriptor_keys_used += count;
        return;
    }
}

// (VR multiview) Eye 1's data for a multiview draw, resolved as eye 1's replay would.
static void mv_eye1(bool on) {
    static int memo;
    if (on) memo = eye_memo_mode, eye_memo_mode = 0, vk_vr_set_eye(1);
    else vk_vr_set_eye(0), eye_memo_mode = memo;
}

// (VR multiview) A binding as both eyes' elements: element 0 is the one just resolved (eye 0), element 1 eye 1's -
// its own image of a rendered texture, its own camera - or the same.
static void mv_eye1_element(VkWriteDescriptorSet *w, const VkBindingInfo *b, int stage, ShaderInfo *si, uint32_t gx,
                            uint32_t gy, uint32_t gz, VkDescriptorBufferInfo bi[2], VkDescriptorImageInfo ii[2]) {
    if (w->pImageInfo) {
        ii[0] = ii[1] = *w->pImageInfo;
        const int ti = (int)b->binding - 32;
        if (ti >= 0 && ti < si->ntex && ti < 64) {
            NvnTexture *tt = shader_texture_object(stage, &si->tex[ti]);
            if (tt && naliases) tt = alias_read(tt);
            if (tt && mv_is_later(tt)) {
                const TexBackend *tb = tt->backend;
                char why[160];
                snprintf(why, sizeof why, "reads what eye 1 renders later: %ux%u fmt %#x scalable %d layered %d",
                         tt->width, tt->height, tt->format, tb ? tb->scalable : -1, tb ? tb->eye_layer : -1);
                mv_blacklist_late(mv_sig(), why, tt);
            }
            if (tt && !mv_shared(tt)) {
                float sc[2]; uint32_t wh[2];
                mv_eye1(true), texture_info(stage, si, ti, &ii[1], sc, wh), mv_eye1(false);
            }
        }
        w->pImageInfo = ii;
    } else if (w->pBufferInfo) {
        bi[0] = bi[1] = *w->pBufferInfo;
        if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17)
            mv_eye1(true), bi[1] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz), mv_eye1(false);
        w->pBufferInfo = bi;
    }
    w->descriptorCount = 2;
}

static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii) {
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    VkDescriptorBufferInfo bi2[MAX_BIND][2];  // (VR multiview: both eyes' elements of a binding)
    VkDescriptorImageInfo ii2[MAX_BIND][2];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    float rs[66][4] = {{1, 1, 0, 0}};  // ([65]: mirror mode, renderer.m)
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    rs[65][0] = g_mirror_w;
    rs[65][1] = st.alpha_ref;
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
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
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = b->binding == GFAST_BUFFER_INDEX ? gfast_info() : gmem_info(stage == 5 ? "compute shader with global memory" : "draw shader with global memory");
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        if (p->mv && b->count == 2) mv_eye1_element(&w[n], b, stage, si, gx, gy, gz, bi2[n], ii2[n]);
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            (b->binding != 18 && b->binding != 19 && b->binding != GMAP_BUFFER_INDEX)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == GMAP_BUFFER_INDEX) {  // (compute) this dispatch's fast-copy table: gfast_prepare
            bi[n] = ubo_span(vk_transient_copy(gmap_tab, sizeof gmap_tab, 256), sizeof gmap_tab);
        } else if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    DescriptorKeyItem key[MAX_BIND] = {0};
    for (int i = 0; i < n; i++) {
        key[i].binding = w[i].dstBinding;
        key[i].type = w[i].descriptorType;
        if (w[i].pBufferInfo) {
            key[i].buffer = w[i].pBufferInfo->buffer;
            key[i].offset = w[i].pBufferInfo->offset;
            key[i].range = w[i].pBufferInfo->range;
        }
        if (w[i].pImageInfo) {
            key[i].sampler = w[i].pImageInfo->sampler;
            key[i].view = w[i].pImageInfo->imageView;
            key[i].image_layout = w[i].pImageInfo->imageLayout;
        }
    }
    static int cache_enabled = -1;
    if (cache_enabled < 0) cache_enabled = getenv("LARECOMP_VK_NO_DESCRIPTOR_CACHE") ? 0 : 1;
    const bool cache = cache_enabled && !p->mv;  // (the key holds element 0 only)
    const uint64_t hash = cache ? descriptor_hash(layout, key, (uint32_t)n) : 0;
    VkDescriptorSet ds = cache ? descriptor_cache_find(layout, key, (uint32_t)n, hash) : VK_NULL_HANDLE;
    if (cache) {
        if (ds) {
            stat_descriptor_hits++;
            return ds;
        }
        stat_descriptor_misses++;
    }
    ds = vk_alloc_set(layout);
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    if (cache) descriptor_cache_add(layout, key, (uint32_t)n, hash, ds);
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
    VkResult r = vk_create_compute_pipelines(1, &ci, &cp->pipe);
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
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    if (draw_must_wait(NULL)) build_compute(cp);
    else run_job(build_compute, cp);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (cp && draw_must_wait(NULL)) wait_built(&cp->p.state);
    if (!cp || atomic_load(&cp->p.state) != 1) { stat_skipped++; return; }
    {   // LARECOMP_SKIP_CS=<code address hex> (debug): skip that compute shader's dispatches, to see what it does
        static uint64_t skip = ~0ull;
        if (skip == ~0ull) { const char *e = getenv("LARECOMP_SKIP_CS"); skip = e ? strtoull(e, NULL, 16) : 0; }
        if (skip && st.program->stages[5].code_addr == skip) { stat_skipped++; return; }
    }
    if (cp->si->uses_gmem && getenv("LARECOMP_LOG_CS_SBUF")) {  // (debug) the storage buffers bound to gmem dispatches
        static uint64_t seen_cs[16];
        static int seen_n[16];
        int k = 0;
        while (k < 15 && seen_cs[k] && seen_cs[k] != st.program->stages[5].code_addr) k++;
        seen_cs[k] = st.program->stages[5].code_addr;
        if (seen_n[k]++ < 24) {
            LOG("[cs] %llx %ux%ux%u:", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
            for (int i = 0; i < 16; i++)
                if (st.sbuf[5][i].addr) LOG(" sb%d %llx+%llx", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
            LOG("\n");
        }
    }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u) cs %llx gmem %d", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    nfeedback = 0;
    if (ct_on()) {
        LOG("[ct] dispatch cs %llx %ux%ux%u | tex", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < cp->si->ntex; i++) {
            NvnTexture *t = shader_texture_object(5, &cp->si->tex[i]);
            if (t) LOG(" %s%p/%#x", cp->si->tex[i].is_image ? "I:" : "", (void *)t, t->format);
        }
        LOG("\n");
    }
    if (cp->si->uses_gmem_map) gfast_prepare(cb);
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    if (cp->si->uses_gmem_map && cp->si->uses_gmem_store) gfast_finish(cb);
    if (cp->si->uses_gmem_store) vc_mark_sbufs(5);
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

// ---- VR HUD layer: the game's 2D layer (HUD, menus, title, file select, map) drawn once per frame into its own
// 16:9 texture instead of into each eye, for an OpenXR panel. Its draws are the last of a frame: an orthographic
// 192-byte camera block (vertex slot 0) and the presented image as target (measured on the title, file select,
// gameplay and map screens). On by default in VR; SKYRECOMP_VR_HUD_LAYER=0 turns it off.
static NvnTexture g_hud_tex, g_hud_depth, g_wrist_tex;  // (g_wrist_tex: the wrist panels' elements, vr.c)  // (its own depth / stencil: some 2D draws depth-test, e.g. the map islands)
static uint64_t g_hud_cleared = ~0ull;

bool vk_hud_layer_enabled(void) {  // (on by default; SKYRECOMP_VR_HUD_LAYER=0 draws the 2D layer into the eyes)
    static int on = -1;
    if (on < 0) { const char *e = getenv("SKYRECOMP_VR_HUD_LAYER"); on = !e || atoi(e) != 0; }
    return on;
}

bool vk_hud_layer_on(void) { return vk_hud_layer_enabled() && vr_stereo_active(); }

// The images the game presented lately (device.c backend_present): the screen this frame will present is one of
// them. (A texture's "presented" mark is for good: the game reuses images, the map draws its islands into one.)
static struct { NvnTexture *t; uint64_t frame; } g_recent_presented[4];
void vk_hud_note_presented(NvnTexture *t) {
    const uint64_t f = renderer_present_count();
    int slot = 0;
    for (int i = 0; i < 4; i++) {
        if (g_recent_presented[i].t == t) { slot = i; break; }
        if (g_recent_presented[i].frame < g_recent_presented[slot].frame) slot = i;
    }
    g_recent_presented[slot].t = t;
    g_recent_presented[slot].frame = f;
}
static bool recently_presented(const NvnTexture *t) {
    const uint64_t f = renderer_present_count();
    for (int i = 0; i < 4; i++)
        if (t && g_recent_presented[i].t == t && g_recent_presented[i].frame + 4 >= f) return true;
    return false;
}

static bool is_hud_draw(void) {
    NvnTexture *c = st.rt.ncolor ? st.rt.color[0] : NULL;
    const TexBackend *b = c ? (const TexBackend *)c->backend : NULL;
    const Range c0 = st.ubuf[0][0];
    if (!b || !recently_presented(c) || !c0.addr || c0.size != 192) return false;
    const float *f = (const float *)(uintptr_t)c0.addr;
    return fabsf(f[15] - 1) < 1e-4f && fabsf(f[12]) < 1e-4f && fabsf(f[13]) < 1e-4f && fabsf(f[14]) < 1e-4f;
}

// A cutscene's letterbox bar: a 2D quad wider than the screen, thin, against its top or bottom edge, not rotated.
// In VR the scene fills the view: the bars only hid its edges on a TV, and on the HUD panel they are two black
// stripes across the view. (The 2D camera block: the ortho projection's rows 0-1, then the quad's model rows; the
// quad spans 0..1 in its own x and y.)
static bool hud_letterbox_bar(void) {
    const Range c0 = st.ubuf[0][0];
    if (!c0.addr || c0.size != 192) return false;
    const float *f = (const float *)(uintptr_t)c0.addr;
    if (fabsf(f[17]) > 1e-3f || fabsf(f[20]) > 1e-3f) return false;  // (rotated or skewed)
    const float x0 = f[0] * f[19] + f[3], x1 = f[0] * (f[16] + f[19]) + f[3];
    const float y0 = f[5] * f[23] + f[7], y1 = f[5] * (f[21] + f[23]) + f[7];
    const float xl = fminf(x0, x1), xr = fmaxf(x0, x1), yb = fminf(y0, y1), yt = fmaxf(y0, y1);
    return xr - xl > 2.2f && xl <= -1.0f && xr >= 1.0f && yt - yb < 0.35f && (yt >= 0.98f || yb <= -0.98f);
}

// A 2D quad over the whole screen (a fade to / from black or white, a flash): drawn in the eyes rather than on the
// HUD panel, it covers the whole view - on the panel it is a coloured rectangle floating in a world left unfaded.
static bool hud_fullscreen_quad(void) {
    const Range c0 = st.ubuf[0][0];
    if (!c0.addr || c0.size != 192) return false;
    const float *f = (const float *)(uintptr_t)c0.addr;
    if (fabsf(f[17]) > 1e-3f || fabsf(f[20]) > 1e-3f) return false;
    const float x0 = f[0] * f[19] + f[3], x1 = f[0] * (f[16] + f[19]) + f[3];
    const float y0 = f[5] * f[23] + f[7], y1 = f[5] * (f[21] + f[23]) + f[7];
    return fminf(x0, x1) <= -0.98f && fmaxf(x0, x1) >= 0.98f && fminf(y0, y1) <= -0.98f && fmaxf(y0, y1) >= 0.98f;
}

// The HUD texture's backend (NULL before the first HUD draw).
TexBackend *vk_hud_backend(void) { return g_hud_tex.backend; }
static bool g_wrist_drawn;  // a wrist element was drawn since the last present (device.c: else no card)
TexBackend *vk_wrist_backend(void) { return g_wrist_drawn ? g_wrist_tex.backend : NULL; }

// The wrist texture: made like the HUD texture (same size and format), cleared with it.
static NvnTexture *wrist_texture(void) {
    if (!g_wrist_tex.width && g_hud_tex.width) {
        g_wrist_tex = g_hud_tex;
        g_wrist_tex.backend = NULL;
        snprintf(g_wrist_tex.label, sizeof g_wrist_tex.label, "vr-wrist");
    }
    return g_wrist_tex.width ? &g_wrist_tex : NULL;
}
static void wrist_clear(VkCommandBuffer cb) {
    if (!g_wrist_tex.backend) return;
    TexBackend *w = vk_tex(&g_wrist_tex, true);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, w->layer0, 1};
    vkCmdClearColorImage(cb, w->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
}

// A 2D draw of a wrist panel's element (vr.c WRIST_*_RECT): its layout anchor - the draw's model translation in the
// 1920x1080 layout - in the hearts' row (y < 100, x < 340; the area name's banner, also top left, is all below
// y 106 there) or by the rupee counter and Fi's prompt (x < 160, y > 740). Measured in Link's room.
static bool hud_wrist_element(void) {
    const Range c0 = st.ubuf[0][0];
    if (!c0.addr || c0.size != 192) return false;
    const float *f = (const float *)(uintptr_t)c0.addr;
    const float x = ((f[0] * f[19] + f[3]) * 0.5f + 0.5f) * 1920, y = (0.5f - (f[5] * f[23] + f[7]) * 0.5f) * 1080;
    return (y < 100 && x < 340) || (x < 160 && y > 740);
}

// The panel image is a per-present scratch surface, not retained UI state.  Clear it only after device.c has
// recorded every headset and mirror read from it.  This makes stale full-screen screens impossible even when the
// async replay thread's draw/present counters cross a frame boundary.
void vk_hud_discard_after_present(void) {
    if (!g_hud_tex.backend) return;
    vk_end_pass();
    TexBackend *b = vk_tex(&g_hud_tex, true);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, b->layer0, 1};
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    wrist_clear(cb);
    g_wrist_drawn = false;
    vk_full_barrier(cb);
    g_hud_cleared = ~0ull;
    st.rt_dirty = true;
}

// Made like the presented image but 1920x1080 and not resolution-scaled (the game lays its 2D layer out in those
// pixels), host-only (no guest memory: never uploaded).
static NvnTexture *hud_texture(const NvnTexture *like) {
    if (!g_hud_tex.width) {
        g_hud_tex = *like;
        g_hud_tex.width = 1920;
        g_hud_tex.height = 1080;
        g_hud_tex.depth = 1;
        g_hud_tex.levels = 1;
        g_hud_tex.samples = 1;
        g_hud_tex.target = 1;
        g_hud_tex.flags &= ~8u;  // (not a scaled render target)
        g_hud_tex.addr = 0;
        g_hud_tex.pool = NULL;
        g_hud_tex.pool_offset = 0;
        g_hud_tex.backend = NULL;
        g_hud_tex.pending_rgba = NULL;
        snprintf(g_hud_tex.label, sizeof g_hud_tex.label, "vr-hud");
    }
    return &g_hud_tex;
}

static NvnTexture *hud_depth(const NvnTexture *like) {
    if (!like) return NULL;
    if (!g_hud_depth.width) {
        g_hud_depth = *like;
        g_hud_depth.width = 1920;
        g_hud_depth.height = 1080;
        g_hud_depth.depth = 1;
        g_hud_depth.levels = 1;
        g_hud_depth.samples = 1;
        g_hud_depth.target = 1;
        g_hud_depth.flags &= ~8u;
        g_hud_depth.addr = 0;
        g_hud_depth.pool = NULL;
        g_hud_depth.pool_offset = 0;
        g_hud_depth.backend = NULL;
        g_hud_depth.pending_rgba = NULL;
        snprintf(g_hud_depth.label, sizeof g_hud_depth.label, "vr-hud-depth");
    }
    return &g_hud_depth;
}

// Once per frame, before the frame's first draw: the HUD texture becomes fully transparent (a frame without HUD
// draws leaves it empty).
static void hud_clear_for_frame(void) {
    const uint64_t frame = renderer_present_count();
    if (frame == g_hud_cleared || !g_hud_tex.backend || vk_vr_eye()) return;
    g_hud_cleared = frame;
    vk_end_pass();
    TexBackend *b = vk_tex(&g_hud_tex, true);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, b->layer0, 1};
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    wrist_clear(cb);
    if (g_hud_depth.backend) {  // (depth 0: the game's reversed-Z far plane; stencil 0)
        TexBackend *d = vk_tex(&g_hud_depth, true);
        VkClearDepthStencilValue dz = {0.0f, 0};
        VkImageSubresourceRange da = {d->aspect, 0, 1, d->layer0, 1};
        vkCmdClearDepthStencilImage(cb, d->image, VK_IMAGE_LAYOUT_GENERAL, &dz, 1, &da);
    }
    vk_full_barrier(cb);
    st.rt_dirty = true;  // (the next draw begins its pass again)
}

// Full-screen 2D (the map, menus with no 3D scene behind): the previous frame drew no scene. Nothing is redirected
// then; at present the screen's 16:9 box goes to the HUD texture whole (vk_hud_present), opaque.
static bool hud_full2d(void) { return renderer_present_count() > g_last_scene_frame + 1; }
// (hle/vr_world.c) A full-screen menu pauses the game while the world keeps drawing behind it: its 2D layer is
// redirected whole and shown opaque, drawn with the game's own blending (the map's parchment blends over its islands
// by the destination's alpha, which the coverage alpha below would replace).
extern bool vr_world_menu_active(void);
bool vk_hud_opaque(void) { return hud_full2d() || vr_world_menu_active(); }

static CmdRec *g_hud_rec;  // (the draw being routed)
static bool g_hud_candidate;  // this draw is drawn like the 2D layer (decided in draw_ once its program is known)
static bool g_hud_fullscreen_draw;
static VrFadeFrame g_view_fade;
static NvnTexture *g_view_fade_target;
// Lettering cancels a final fade, but does not make a title card's black backdrop a window onto the last scene.
bool vk_hud_black_backdrop(NvnTexture *screen) {
    return screen && ((g_black_clear_target == screen && g_black_clear_frame == renderer_present_count()) ||
           (g_view_fade_target == screen && g_view_fade.opaque[3] == 1 &&
            g_view_fade.opaque[0] == 0 && g_view_fade.opaque[1] == 0 && g_view_fade.opaque[2] == 0));
}
bool vk_take_view_fade(float rgba[4]) {
    g_view_fade_target = NULL;
    return vr_fade_frame_take(&g_view_fade,rgba);
}
// (device.c) A 1x1 texture of one premultiplied colour (the wrist card's backing).
TexBackend *vk_solid_texture(const float rgba[4]) {
    extern void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride);
    static NvnTexture texture = {.width = 1, .height = 1, .depth = 1, .levels = 1, .samples = 1, .target = 1, .format = 0x25};
    static uint8_t last[4] = {1, 2, 3, 4};
    uint8_t pixel[4];
    for (int i = 0; i < 4; i++) pixel[i] = (uint8_t)lroundf(fminf(1, fmaxf(0, rgba[i])) * 255);
    if (memcmp(pixel, last, 4)) renderer_upload_rgba(&texture, pixel, 1, 1, 4), memcpy(last, pixel, 4);
    return vk_tex(&texture, false);
}
TexBackend *vk_view_fade_mirror(const float rgba[4]) {
    extern void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride);
    static NvnTexture texture = {.width=1, .height=1, .depth=1, .levels=1, .samples=1, .target=1, .format=0x25};
    uint8_t pixel[4];
    for (int i=0;i<4;i++) pixel[i]=(uint8_t)lroundf(fminf(1, fmaxf(0,rgba[i]))*255);
    renderer_upload_rgba(&texture,pixel,1,1,4);
    return vk_tex(&texture,false);
}
static bool g_fullscreen_quad_frame;  // a full-screen 2D quad (a fade, a flash) since the last present
// (device.c, at present) Whether this frame drew a full-screen 2D quad - then the eyes carry it over the whole view
// and the panel is never opaque nor a copy of the frame (it showed as a black rectangle during fades); resets.
bool vk_hud_frame_had_fullscreen_quad(void) {
    const bool had = g_fullscreen_quad_frame;
    g_fullscreen_quad_frame = false;
    return had;
}  // geometry fills the screen; draw_ decides whether it is actually a solid fade
static bool g_hud_active;     // this draw renders into the HUD texture: draw() restores the targets afterwards
static bool g_hud_content_since_present;  // at least one real 2D-layer draw was routed since the last present
static RtState g_hud_saved_rt;

// (device.c, at present, eye 0's screen) Full-screen 2D frames: the screen's centred 16:9 box (where the game's
// layout sits; the eye image's taller or wider rest shows what a 16:9 screen would not) becomes the HUD texture.
// (device.c, at present) Whether the frame being presented drew a 3D scene; resets for the next frame.
// (device.c, at present) The frame being presented drew no 2D but full-screen quads (a fade, a flash, a blank screen);
// resets for the next frame.
static int g_2d_content;  // this frame's 2D-layer draws that are not full-screen quads
static bool g_fullscreen_2d_since_present;  // a fade/flash covers the eyes; never show a panel over it
// A full-screen menu over the live world (the map) fills the screen first: its backdrop, on the HUD panel - not a fade.
// The panel is then opaque (the menu's later draws leave the HUD's alpha as the fill set it, half).
static bool g_menu_backdrop, g_menu_backdrop_last;
bool vk_hud_menu_backdrop(void) { return g_menu_backdrop_last; }
bool vk_hud_frame_blank(void) {
    g_menu_backdrop_last = g_menu_backdrop, g_menu_backdrop = false;
    const bool blank = g_fullscreen_2d_since_present || g_2d_content == 0;
    g_2d_content = 0;
    g_fullscreen_2d_since_present = false;
    return blank;
}

bool vk_hud_frame_had_scene(void) {
    const bool had = g_scene_since_present;
    g_scene_since_present = false;
    return had;
}

bool vk_hud_frame_had_content(void) {
    const bool had = g_hud_content_since_present;
    g_hud_content_since_present = false;
    return had;
}

void vk_hud_present(NvnTexture *screen) {
    // (not a frame that drew a 3D scene: right after a scene-less stretch - black title cards, a fade - the first
    // scene frames still count as full-screen 2D, and the whole frame landed on the panel, a small copy of the view)
    // Do not resurrect the previous panel during a scene-less fade.  The HUD image is persistent, so merely having
    // a backend does not mean this frame supplied panel content.  Real full-screen 2D screens (map, file select,
    // menus) contain layout draws; an all-screen fade does not.
    if (!vk_hud_layer_on() || !hud_full2d() || g_scene_since_present || g_fullscreen_2d_since_present || g_fullscreen_quad_frame ||
        g_2d_content == 0 || !screen || !screen->backend) return;
    NvnTexture *hud = hud_texture(screen);
    TexBackend *d = vk_tex(hud, true), *s = (TexBackend *)screen->backend;
    float u = fminf(s->sx, s->sy);
    if (!(u > 0)) u = 1;
    const int bw = (int)lroundf(1920 * u), bh = (int)lroundf(1080 * u);
    const int x0 = ((int)s->width - bw) / 2, y0 = ((int)s->height - bh) / 2;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, s->layer0, 1}, .srcOffsets = {{x0, y0, 0}, {x0 + bw, y0 + bh, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, d->layer0, 1}, .dstOffsets = {{0, 0, 0}, {(int)d->width, (int)d->height, 1}}};
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    vk_full_barrier(cb);
    // device.c samples and resets this immediately after vk_hud_present().  Mark the blit just like routed HUD
    // draws so it is the only full-screen fallback allowed onto the OpenXR panel this frame.
    g_hud_content_since_present = true;
}

static void linear_rt_writeback(void);
static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    g_hud_rec = r;
    g_hud_candidate = vk_hud_layer_on() && st.program && (hud_clear_for_frame(), !hud_full2d()) && is_hud_draw();
    if (g_hud_candidate && hud_letterbox_bar()) {
        g_hud_candidate = false;
        stat_skipped++;
        PROF_ADD(prof_draw);
        return;
    }
    const bool hud_draw = vk_hud_layer_on() && st.program && !vk_vr_eye() && is_hud_draw();
    const bool fullscreen_2d = hud_draw && hud_fullscreen_quad();
    g_hud_fullscreen_draw = fullscreen_2d;
    if (fullscreen_2d) g_fullscreen_quad_frame = true;
    // (a fade: over the whole view, in the eyes - except behind a full-screen menu drawn over the live world: its own
    // full-screen background, the map's parchment, belongs on its panel; in the eyes it covered the world instead)
    if (g_hud_candidate && fullscreen_2d && !vr_world_menu_active()) g_hud_candidate = false;
    if (!fullscreen_2d && hud_draw) g_2d_content++;  // (vk_hud_frame_blank)
    // World behind a menu (hle/vr_world.c): the menu's own passes that read the screen back (the map's islands,
    // drawn there first, then captured into its map texture) read the HUD texture, where its 2D layer now is.
    const bool menu_sub = !g_hud_candidate && g_menu_screen && g_hud_tex.backend && vk_hud_layer_on() && vr_world_menu_active();
    if (menu_sub) g_hud_sub_from = g_menu_screen, g_hud_sub_to = &g_hud_tex;
    eye_memo_rec = r;
    draw_(r, indexed);
    g_aim_draw = false;  // (VR aiming: set by hud_route for this draw only)
    linear_rt_writeback();
    eye_memo_rec = NULL;
    if (menu_sub && !g_hud_active) g_hud_sub_from = g_hud_sub_to = NULL;
    if (g_hud_active) {
        st.rt = g_hud_saved_rt;
        st.rt_dirty = true;
        g_hud_sub_from = g_hud_sub_to = NULL;
        g_hud_active = false;
    }
    g_hud_candidate = false;
    PROF_ADD(prof_draw);
}

// VR aiming (runtime/hle/vr_aim.c): the left eye's depth along the aim line. At the reticle's first draw of a frame
// (eye 0), the depth at the points vr_aim_probe asks for is copied into a small host buffer of its own (two slots); a
// slot is read once its submission has completed - a frame or two later, never waiting for the GPU.
static void aim_probe(void) {
    extern int vr_aim_probe(int slot, float u[], float v[], int max, bool flip_y, bool minus_one);
    extern void vr_aim_result(int slot, const float scene[], int n);
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static struct { uint64_t seq; int n; VkFormat fmt; bool busy; } slot[2];
    static uint64_t frame = ~0ull;
    if (!buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1024, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info) != VK_SUCCESS) { buf = VK_NULL_HANDLE; return; }
        ptr = info.pMappedData;
    }
    const uint64_t done = vk_completed_seq();
    for (int s = 0; s < 2; s++) {  // results that have arrived
        if (!slot[s].busy || slot[s].seq > done) continue;
        vmaInvalidateAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
        float v[64];
        const uint8_t *q = ptr + s * 512;
        for (int i = 0; i < slot[s].n; i++) {
            uint32_t w;
            memcpy(&w, q + i * 4, 4);
            switch (slot[s].fmt) {
            case VK_FORMAT_D16_UNORM: v[i] = (float)(w & 0xffff) / 65535.0f; break;
            case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: v[i] = (float)(w & 0xffffff) / 16777215.0f; break;
            default: memcpy(&v[i], &w, 4);
            }
        }
        vr_aim_result(s, v, slot[s].n);
        slot[s].busy = false;
    }
    const uint64_t f = renderer_present_count();
    if (frame == f || g_aim_main_frame[0] != f || !g_aim_depth) return;  // (once a frame, with this frame's depth)
    const int s = !slot[0].busy ? 0 : !slot[1].busy ? 1 : -1;
    if (s < 0) return;
    TexBackend *b = vk_tex(g_aim_depth, true);
    if (!b || !b->image || b->samples > 1 || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    frame = f;
    float u[64], v[64];
    const int n = vr_aim_probe(s, u, v, 64, nvn_window_origin() == 0, nvn_depth_mode() == 0);
    if (n <= 0) return;
    VkBufferImageCopy rg[64];
    for (int i = 0; i < n; i++) {
        int x = (int)(u[i] * b->width), y = (int)(v[i] * b->height);
        x = x < 0 ? 0 : x >= (int)b->width ? (int)b->width - 1 : x;
        y = y < 0 ? 0 : y >= (int)b->height ? (int)b->height - 1 : y;
        rg[i] = (VkBufferImageCopy){.bufferOffset = (VkDeviceSize)(s * 512 + i * 4),
                                    .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, b->layer0, 1},
                                    .imageOffset = {x, y, 0}, .imageExtent = {1, 1, 1}};
    }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, buf, (uint32_t)n, rg);
    vk_full_barrier(cb);
    slot[s].seq = vk_pending_seq(), slot[s].n = n, slot[s].fmt = b->format, slot[s].busy = true;
}

// (hud_route) An aiming item's reticle piece in a frame with the 3D scene: true when it's drawn in each eye's own
// picture, moved to the shot's end point (cbuf_info, g_aim_draw) rather than on the HUD panel. Not in a multiview
// pass (one recording for both eyes: it can't move per eye) - it stays on the panel then.
static bool aim_piece(Prog *pr, bool mv) {
    extern bool vr_aim_enabled(void), vr_aim_ready(void);
    extern void vr_aim_center(float x, float y);
    extern const char *texpatch_aim_name(int id);
    if (mv || !vr_aim_enabled() || !g_scene_since_present || vr_world_menu_active()) return false;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        const char *nm = b && b->tp > 0 ? texpatch_aim_name(b->tp - 1) : NULL;
        if (!nm) continue;
        // (plain circles - the Slingshot's dot - only after a real reticle piece this frame: other screens use them)
        static uint64_t piece_frame[2] = {~0ull, ~0ull};
        const int eye = (int)vk_vr_eye() & 1;
        if (strncmp(nm, "th_circle", 9)) piece_frame[eye] = renderer_present_count();
        else if (piece_frame[eye] != renderer_present_count()) return false;
        const float *u = (const float *)(uintptr_t)st.ubuf[0][0].addr;  // (is_hud_draw: the 192-byte 2D block)
        if (!eye) {
            // its centre: an unrotated piece (the ring, the dot; the arrows are turned 45 degrees)
            if (fabsf(u[17]) < 1e-4f && fabsf(u[20]) < 1e-4f) vr_aim_center(u[0] * u[19] + u[3], u[5] * u[23] + u[7]);
            aim_probe();
        }
        if (!vr_aim_ready()) return false;  // (no end point or eye camera yet: on the panel, as before)
        g_aim_draw = true;
        static int said;
        if (!said++) LOG("[vr] aiming: the reticle (%s) is drawn at the shot's end point\n", nm);
        return true;
    }
    return false;
}

// (draw_, its program known) Where a 2D-layer draw goes. Some of the 2D layer's draws composite the 3D scene
// (bloom, the title's haze): they read a texture made from this frame's scene and stay
// in the eyes. The rest renders once, in eye 0, into the HUD texture; eye 1 skips it. False: skip the draw.
static bool hud_route(Prog *pr, bool mv) {
    if (aim_piece(pr, mv)) return true;  // (an aiming reticle: in the eyes, at the shot's end point)
    const uint64_t fr1 = renderer_present_count() + 1;
    const uint64_t fs = st.program ? st.program->stages[1].code_addr : 0;
    const uint64_t fs_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
    // Static layout layers include the void dialogue's patterned purple background. They must follow its text
    // onto the HUD; a shader-wide "void effect" exemption separated the box from its lettering. Screen-sized
    // backgrounds and scene-derived composites are classified by geometry/provenance instead.
    bool reads_scene = false;
    bool reads_screen_rt = false;
    for (int i = 0; i < pr->fi->ntex && i < 64 && !reads_scene; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        // Async replay may leave the producer and consumer one present generation apart.  Treat either adjacent
        // generation as the live scene; otherwise a cutscene composite can be mistaken for 2D UI and become a
        // floating rectangular card (notably the flare in the void dream sequence).
        reads_scene = b && b->scene_frame && b->scene_frame <= fr1 + 1 && b->scene_frame + 1 >= fr1 &&
                      !recently_presented(t);
        // Some cutscene bloom/flare chains use an offscreen render target whose scene_frame marker is lost while
        // copying or downsampling it.  It is still screen-derived imagery, not a static UI texture.  Without this
        // fallback the effect's rectangular intermediate becomes a floating HUD card in the void sequence.
        if (b && b->gpu_owned && t->width >= 160 && t->height >= 90 && !recently_presented(t)) reads_screen_rt = true;
    }
    // The selected save's parchment backing uses a layout shader with a static 256x512 mask and a small rendered
    // texture. That second input inherits scene provenance, but the quad is UI: leaving it in the eyes separates
    // it from the save details/buttons on the HUD panel. Measured on the selected-save screen, not the title.
    // Restrict this exception to that layout shader and its two-texture, non-fullscreen draw; scene composites,
    // including the measured void cutscene effects above, keep their existing stereo routing.
    const bool layout_backdrop = fs_hash == 0x47b40805275c9a4aull && pr->fi->ntex == 2 && !hud_fullscreen_quad();
    if ((reads_scene || reads_screen_rt) && !layout_backdrop) {
        // A live-scene composite is part of the stereo view even when its quad is not the canonical 0..1 shape.
        // The old unit-quad requirement routed cutscene bloom/flares to the HUD, exposing their rectangular bounds.
        // Only an intentionally active world-menu panel may use a scene sample as its frosted HUD backdrop.
        if (!vr_world_menu_active()) return true;
        const float *vb = st.vbuf[0].addr && st.vbuf[0].size >= 48 ? (const float *)(uintptr_t)st.vbuf[0].addr : NULL;
        bool unit = vb && g_hud_rec && g_hud_rec->x[2] <= 6;
        for (int i = 0; unit && i < 12; i++) unit = isfinite(vb[i]) && vb[i] >= -0.01f && vb[i] <= 1.01f;
        // full-screen menu composite: drawn normally in both eyes - unless the menu has its own backdrop on the panel
        // (the map's soft shadow of its islands, over its parchment: in the eyes, a ghost of the map over the world)
        if (unit && !g_menu_backdrop) return true;
    }
    if (vk_vr_eye()) return false;
    {   // Diagnostic capture for a user-reproduced floating cutscene card.  Log the actual draw routed to the HUD,
        // including every sampled texture's provenance; do not infer the offending effect from screenshots alone.
        static int trace = -1;
        if (trace < 0) trace = getenv("SKYRECOMP_VR_HUD_TRACE") && atoi(getenv("SKYRECOMP_VR_HUD_TRACE")) != 0;
        if (trace) {
            static uint64_t seen[512];
            static int nseen;
            int k = 0;
            while (k < nseen && seen[k] != fs_hash) k++;
            if (k == nseen && nseen < (int)(sizeof seen / sizeof seen[0])) {
                seen[nseen++] = fs_hash;
                LOG("[hud-route] first frame %llu fs %016llx draws %llu fullscreen %d scene %d screenrt %d ntex %d rt %ux%u\n",
                    (unsigned long long)renderer_present_count(), (unsigned long long)fs_hash,
                    (unsigned long long)(g_hud_rec ? g_hud_rec->x[2] : 0), hud_fullscreen_quad(), reads_scene,
                    reads_screen_rt, pr->fi->ntex, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0,
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0);
                for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
                    NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                    const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
                    LOG("[hud-route]   tex%d %ux%u gpu %d scene_frame %llu recent %d label %s\n", i,
                        t ? t->width : 0, t ? t->height : 0, b ? b->gpu_owned : 0,
                        (unsigned long long)(b ? b->scene_frame : 0), recently_presented(t), t ? t->label : "-");
                }
            }
        }
    }
    // Async replay can process the first draws of a new presented frame while renderer_present_count still names the
    // previous one. The frame-number guard may therefore think the HUD was already cleared. The first draw actually
    // routed after each present is an unambiguous boundary: force its transparent clear before preserving this draw.
    if (!g_hud_content_since_present) {
        g_hud_cleared = ~0ull;
        hud_clear_for_frame();
    }
    NvnTexture *hud = hud_texture(st.rt.color[0]);
    NvnTexture *hd = hud_depth(st.rt.depth);
    if (!hud->backend || (hd && !hd->backend)) {  // (first use: made now, cleared for this frame)
        vk_tex(hud, true);
        if (hd) vk_tex(hd, true);
        g_hud_cleared = ~0ull;
        hud_clear_for_frame();
    }
    // (vr.c, the wrist panels on: their elements go whole to the wrist texture, the rest stays in the HUD's)
    NvnTexture *wrist = vr_wrist_elements() && hud_wrist_element() ? wrist_texture() : NULL;
    if (wrist && !wrist->backend) {
        vk_tex(wrist, true);
        vk_end_pass();
        wrist_clear(vk_cmd());
        vk_full_barrier(vk_cmd());
    }
    g_hud_saved_rt = st.rt;
    st.rt = (RtState){0};
    st.rt.ncolor = 1;
    st.rt.color[0] = wrist ? wrist : hud;
    if (wrist) g_wrist_drawn = true;
    st.rt.depth = hd;
    st.rt_dirty = true;
    g_hud_sub_from = g_menu_screen = g_hud_saved_rt.color[0];
    g_hud_sub_to = hud;
    g_hud_active = true;
    g_hud_content_since_present = true;
    return true;
}

// (draw_) Marks the targets of a draw that renders this frame's scene, or something made from it.
static void hud_mark_scene_targets(Prog *pr) {
    const uint64_t fr1 = renderer_present_count() + 1;
    bool scene = false;
    const Range c0 = st.ubuf[0][0];
    if (c0.addr && c0.size >= 112) {
        extern bool sky_vr_is_scene_projection(const float *ubo, uint64_t bytes);
        scene = sky_vr_is_scene_projection((const float *)(uintptr_t)c0.addr, c0.size);
        if (scene) g_last_scene_frame = renderer_present_count(), g_scene_since_present = true;
    }
    for (int i = 0; i < pr->fi->ntex && i < 64 && !scene; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        scene = b && b->scene_frame == fr1;
    }
    if (!scene) return;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && st.rt.color[i] != &g_hud_tex && st.rt.color[i]->backend)
            ((TexBackend *)st.rt.color[i]->backend)->scene_frame = fr1;
}

static uint64_t prof_tp_;
static void draw_(CmdRec *r, bool indexed) {
    if (mv_skip()) { stat_mv_skipped++; return; }  // (VR multiview: eye 0's pass drew it in both eyes)
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { stat_skipped++; return; }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    fkey.alpha_func = st.color_state[2] < 8 ? st.color_state[2] : 0;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    // SRC1 factors consume a second fragment output at location 0, index 1.
    // Treating it as MRT location 1 leaves the blend mask undefined (SSHD eyes).
    if ((st.color_state[0] & 1) && st.rt.ncolor == 1) {
        const uint8_t *b = st.blend[0];
        for (int i = 1; i <= 4; i++)
            if (b[i] >= 0x10 && b[i] <= 0x13) fkey.dual_source = true;
        if (fkey.dual_source) fkey.rt_kind[1] = fkey.rt_kind[0];
    }
    const bool mv = mv_for_draw();
    bool wait = draw_must_wait(st.rt.ncolor && st.rt.color[0] ? st.rt.color[0] : st.rt.depth);
    Prog *pr = get_program(st.program, &vkey, &fkey, mv, wait);
    if (pr && wait) wait_built(&pr->state);
    if (!pr || atomic_load(&pr->state) != 1) {
        // (only a failure of the multiview rewrite itself: a shader that doesn't translate at all is skipped one eye
        // at a time too - blacklisting its targets cascaded into every pass reading them, the whole scene included)
        if (mv && pr && atomic_load(&pr->state) == 2 && pr->vi && pr->vi->msl && pr->fi && pr->fi->msl)
            mv_blacklist(mv_sig(), "shader");
        stat_skipped++;
        return;
    }
    bool view_fade_draw = false;
    // The native solid layout shader carries uniform colour in VS cb3[7], not at FS cbuf[0].
    // Handle alpha ramps and the opaque hold in the same full-view fade path. Keep native opaque fills until
    // present: a later content draw cancels promotion, preserving normal backgrounds behind menu text.
    if (vr_stereo_active() && !indexed && r->x[0] == 7 && r->x[2] == 4 &&
        pr->vi->attr_mask == 1 && pr->fi->ntex == 0 && is_hud_draw() && !st.ds[0] && !st.ds[3] && st.rt.ncolor == 1 &&
        st.rt.color[0] && renderer_texture_presented(st.rt.color[0]) &&
        (!(st.color_state[0] & 1) || (st.blend[0][1] == 5 && st.blend[0][2] == 6 && st.blend[0][5] == 1)) &&
        (st.channel_mask & 7) == 7 && st.ubuf[0][0].addr && st.ubuf[0][0].size == 192 &&
        st.attrib[0] == 0x22 && st.stream[0][0] == 12 && st.vbuf[0].addr &&
        r->x[1] <= st.vbuf[0].size / 12 && st.vbuf[0].size / 12 - r->x[1] >= 4 &&
        st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
        st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height &&
        st.scissor[0][0] == 0 && st.scissor[0][1] == 0 &&
        st.scissor[0][2] >= st.rt.color[0]->width && st.scissor[0][3] >= st.rt.color[0]->height) {
        const uint64_t fs_hash = hmix(1469598103934665603ull, (const void *)(uintptr_t)(pr->code[1] + 0x30), 0x200);
        float rgba[4];
        const float (*vertices)[3] = (const float (*)[3])(uintptr_t)(st.vbuf[0].addr + r->x[1] * 12);
        if (fs_hash == 0x8574d5dbe7039811ull && vr_fade_quad_color((const float *)(uintptr_t)st.ubuf[0][0].addr, vertices, rgba) && vr_view_fade_available()) {
            view_fade_draw = true;
            const bool keep_native = g_view_fade.opaque_last || !(st.color_state[0] & 1);
            if (!vk_vr_eye()) {
                g_view_fade_target = st.rt.color[0];
                vr_fade_frame_fill(&g_view_fade, rgba, (st.color_state[0] & 1) != 0);
                if (getenv("SKYRECOMP_VR_FADE_TRACE") && renderer_present_count() % 30 == 0)
                    LOG("[vr-fade] frame %llu native rgba %.4f %.4f %.4f %.4f %s -> full headset\n",
                        (unsigned long long)renderer_present_count(),rgba[0],rgba[1],rgba[2],rgba[3],
                        (st.color_state[0] & 1) ? "blend" : "opaque hold");
            }
            if (!keep_native) {
                stat_skipped++;
                return; // Alpha ramps are composited once. Opaque fills remain native unless final at present.
            }
        }
    }
    // Work on another image cannot reveal content behind this fill. Only later content on its own target
    // distinguishes an opaque menu background from a final full-view veil.
    if (!vk_vr_eye() && !view_fade_draw && g_view_fade_target) {
        for (int i = 0; i < st.rt.ncolor; i++)
            if (st.rt.color[i] == g_view_fade_target) vr_fade_frame_draw(&g_view_fade);
    }
    // A textured full-screen quad can be the title or file-select background.  Only a textureless black/white fill
    // owns the whole view as a fade; suppressing every full-screen quad makes those screens disappear in VR.
    // (behind a world menu, a textured full-screen quad goes on the panel only over the menu's own backdrop - the
    // map's soft shadow of its islands. Without one (the Gear menu) it is the menu's blurred picture of the screen:
    // in VR a flat image of the view in front of the live world behind the menu - not drawn, in either eye.)
    // (also its plain copy of the 3D view onto the 2D layer: a 2D draw sampling a screen-sized render target)
    if (vr_world_menu_active() && !g_menu_backdrop && pr->fi->ntex && vk_hud_layer_on() &&
        (hud_fullscreen_quad() || g_hud_candidate)) {
        const bool full = hud_fullscreen_quad();
        bool from_screen = false;
        for (int i = 0; i < pr->fi->ntex && i < 64 && !from_screen; i++) {
            NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
            const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
            from_screen = b && b->gpu_owned && (full || (t->width >= 1280 && t->height >= 720));
        }
        if (from_screen) {
            // (the game's world picture overwrote everything under it - the menu's fills. It then filters that flat,
            // mono picture into paper (these draws, skipped too); over the live world the panel darkens it instead)
            // (again at each of the filter's draws: the paper layer drawn between them is the filter's base)
            if (!full && !vk_vr_eye() && g_hud_tex.backend) {
                vk_end_pass();
                TexBackend *hb = vk_tex(&g_hud_tex, true);
                VkClearColorValue zero = {{0, 0, 0, 0.25f}};
                VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, hb->layer0, 1};
                vk_full_barrier(vk_cmd());
                vkCmdClearColorImage(vk_cmd(), hb->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
                vk_full_barrier(vk_cmd());
                st.rt_dirty = true;
            }
            stat_skipped++;
            return;
        }
    }
    if (g_hud_fullscreen_draw && pr->fi->ntex == 0 && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
        const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
        const bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f;
        const bool white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
        if ((black || white) && g_hud_candidate && vr_world_menu_active()) g_menu_backdrop = true;
        else if (black || white) g_fullscreen_2d_since_present = true;
    }
    // A recognized full-view fill stays in the eyes while we decide whether it is a final veil or an opaque
    // background followed by scene content. Routing that native hold to the HUD leaves a black card over the
    // world even after later scene draws correctly cancel promotion to the final compositor fade.
    if (g_hud_candidate && !view_fade_draw && !hud_route(pr, mv)) {
        stat_skipped++;
        return;
    }
    if (vk_hud_layer_on()) hud_mark_scene_targets(pr);
    {   // SKYRECOMP_VR_DRAW_LOG=<frame>: that frame's draws (eye 0)
        static long at = -2;
        if (at == -2) { const char *e = getenv("SKYRECOMP_VR_DRAW_LOG"); at = e ? atol(e) : -1; }
        if (at >= 0 && (long)renderer_present_count() == at && !vk_vr_eye()) {
            const Range c0 = st.ubuf[0][0];
            const float *f = c0.addr && c0.size >= 112 ? (const float *)(uintptr_t)c0.addr : NULL;
            NvnTexture *t = st.rt.ncolor ? st.rt.color[0] : NULL;
            extern bool sky_vr_is_scene_projection(const float *ubo, uint64_t bytes);
            LOG("[draw] c0 %llu scene %d rt %p %dx%d presented %d prog %p vs %llx fs %llx%s",
                (unsigned long long)c0.size, f ? sky_vr_is_scene_projection(f, c0.size) : -1, (void *)t, t ? (int)t->width : 0,
                t ? (int)t->height : 0, t ? renderer_texture_presented(t) : 0, (void *)st.program,
                (unsigned long long)pr->code[0], (unsigned long long)pr->code[1], f ? "" : "\n");
            if (f) LOG(" P %.3f %.3f %.3f %.3f %.3f V %.1f %.1f %.1f\n", f[0], f[5], f[10], f[11], f[14], f[19], f[23], f[27]);
            LOG("[draw]   mask %08x blend_on %x b %d %d %d %d %d %d alpha %d %.3f prim %llu n %llu tex", st.channel_mask,
                st.color_state[0], st.blend[0][1], st.blend[0][2], st.blend[0][3], st.blend[0][4], st.blend[0][5],
                st.blend[0][6], st.color_state[2], st.alpha_ref, (unsigned long long)r->x[0], (unsigned long long)r->x[2]);
            for (int i = 0; i < pr->fi->ntex && i < 8; i++) {
                NvnTexture *tt = shader_texture_object(1, &pr->fi->tex[i]);
                const TexBackend *tb = tt ? (const TexBackend *)tt->backend : NULL;
                LOG(" %p(%dx%d fmt %#x tp %d%s)", (void *)tt, tt ? (int)tt->width : 0, tt ? (int)tt->height : 0,
                    tt ? tt->format : 0, tb ? tb->tp : 0, tt && renderer_texture_presented(tt) ? " screen" : "");
            }
            LOG("\n");
        }
    }
    vr_rig_set_hand_draw(-1);
    if (vr_stereo_active() && pr->vi) {
        // VR 1st person: Link's own skinned draws are skipped (both eyes), like the game's own 1st-person view.
        // Only a bone slot the vertex shader reads counts: bindings stay bound for later, unskinned draws.
        const Range cam = st.ubuf[0][0];
        bool link = false, skinned = false;
        for (int idx = 3; idx <= 5 && !link; idx++) {
            if (!(pr->vi->cbuf_mask & (1u << (idx + 3)))) continue;
            const Range bones = st.ubuf[0][idx];
            skinned |= bones.addr != 0;
            link = bones.addr && cam.addr &&
                   vr_rig_is_link_draw((const float *)(uintptr_t)cam.addr, cam.size, (const float *)(uintptr_t)bones.addr,
                                       bones.size, r->x[2], st.vbuf[0].addr, st.vbuf[0].size);
        }
        // (rigid draws: his eyes - vr_rig_is_link_draw tells them by size and place)
        if (!skinned && cam.addr)
            link = vr_rig_is_link_draw((const float *)(uintptr_t)cam.addr, cam.size, NULL, 0, r->x[2], st.vbuf[0].addr,
                                       st.vbuf[0].size);
        if (link) { stat_skipped++; return; }
        {   // (RE, SKYRECOMP_DOOR_TRACE) draws near the door Link is at: which one is its leaf
            extern void vr_rig_door_probe(const float *slot0, uint64_t size0, bool skinned, const float *bones,
                                          uint64_t bone_bytes, uint64_t count, uint64_t vb, uint64_t vb_size);
            const Range b3 = st.ubuf[0][3];
            if (cam.addr && !vk_vr_eye())
                vr_rig_door_probe((const float *)(uintptr_t)cam.addr, cam.size, skinned, (const float *)(uintptr_t)b3.addr,
                                  b3.size, r->x[2], st.vbuf[0].addr, st.vbuf[0].size);
        }
        // (rigid draws: Link's hands, placed on the controllers by their camera - vr_rig.c)
        if (!skinned && cam.addr) {
            const int hand = vr_rig_hand_side((const float *)(uintptr_t)cam.addr, cam.size, r->x[2], st.vbuf[0].addr,
                                              st.vbuf[0].size);
            vr_rig_set_hand_draw(hand);
            // (the sword or shield he holds: with that hand - vr_rig.c)
            if (hand < 0) vr_rig_held_draw((const float *)(uintptr_t)cam.addr, cam.size, r->x[2]);
        }
        if (!skinned && cam.addr) {  // (the door held open in VR: its leaf turned - vr_rig.c)
            extern void vr_rig_door_draw(const float *slot0, uint64_t size0);
            vr_rig_door_draw((const float *)(uintptr_t)cam.addr, cam.size);
        }
    }
    g_mirror_w = 0;
    {   // mirror mode (runtime/hle/mirror.c; as renderer.m): the scene's composite into the window image is mirrored
        extern bool mirror_draw(bool composite);
        bool composite = false;
        if (st.rt.ncolor && st.rt.color[0] && renderer_texture_presented(st.rt.color[0]) && !(st.color_state[0] & 1) &&
            r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
            st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height && pr->fi->ntex) {
            NvnTexture *t0 = shader_texture_object(1, &pr->fi->tex[0]);
            composite = t0 && t0->width == st.rt.color[0]->width && t0->height == st.rt.color[0]->height;
        }
        if (mirror_draw(composite)) {
            vkey.flip_x = fkey.flip_x = true;
            pr = get_program(st.program, &vkey, &fkey, mv, wait);
            if (pr && wait) wait_built(&pr->state);
            if (!pr || atomic_load(&pr->state) != 1) { stat_skipped++; return; }
            g_mirror_w = (float)st.rt.color[0]->width;
        }
    }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
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
        {   // (test) LARECOMP_SKIP_FS_ADDR=<hex>,...: skip draws by fragment shader address (the draw log's "fs")
            static const char *skip_addr = (const char *)1;
            if (skip_addr == (const char *)1) skip_addr = getenv("LARECOMP_SKIP_FS_ADDR");
            if (skip_addr) {
                char hex[24];
                snprintf(hex, sizeof hex, "%llx", (unsigned long long)fs);
                if (strstr(skip_addr, hex)) { stat_skipped++; return; }
            }
        }
        extern bool pot_level_active(void);
        if (last_hash == 0xa2177dbb72197bd9ull && pot_level_active()) { stat_skipped++; return; }
        // Skyward Sword's outdoor water surface composites two full-resolution scene-colour textures, projected with
        // its own matrix: cbuf_info re-projects that per eye (screen-space texture projections). LARECOMP_VR_NO_WATER_SURFACE=1 omits
        // the surface in VR instead (the earlier workaround: transparent water without the surface layer).
        // Depth of field (cutscenes): the game's blur of what is out of focus, from a small low-resolution copy of the
        // scene - at the headset's resolution its edges turn blocky around characters, and in VR the eyes focus by
        // themselves. Skipping this pass (the first of its chain) leaves the scene sharp. VR Options: Depth of Field.
        if (last_hash == 0x3dffee7e4b168544ull && vr_stereo_active() && !g_settings.vr_dof) { stat_skipped++; return; }
        // The end of the void dream (before the bedroom) blends its own blurred low-resolution copy over the scene:
        // Link turned soft and pixelated in the headset. Measured: skipping this composite alone leaves him sharp.
        if (last_hash == 0xe9b84cc0aebb7adaull && vr_stereo_active() && !g_settings.vr_dof) { stat_skipped++; return; }
        if (last_hash == WATER_SURFACE_FS && vr_stereo_active() && getenv("LARECOMP_VR_NO_WATER_SURFACE")) {
            stat_skipped++;
            return;
        }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
        // The void cutscene's depth composite uses a perspective camera to place a screen image on a quad.
        // Its source already contains this eye's scene. Replacing that camera with the headset camera projects
        // the image a second time, adding a smaller Link/flare over the correctly rendered world.
        g_draw_screen_composite = false;
        if (vr_stereo_active() && g_cur_fs_hash == 0x5baac408d0a0e46eull && !indexed &&
            r->x[0] == 7 && r->x[2] == 4 && pr->fi->ntex == 1) {
            NvnTexture *source = shader_texture_object(1, &pr->fi->tex[0]);
            const TexBackend *image = source ? (const TexBackend *)source->backend : NULL;
            g_draw_screen_composite = image && image->gpu_owned && source->width >= 160 && source->height >= 90;
        }
        // This cutscene uses another depth-of-field composite. Skipping only the older blur shader
        // leaves its low-resolution image blended over Link even with Depth of Field disabled.
        if (g_draw_screen_composite && !g_settings.vr_dof) { stat_skipped++; return; }
        // Keep the glow's offscreen producers intact; only its final screen overlay softens the eye image.
        if (vr_stereo_active() && last_hash == 0x8e6c006313fee204ull && pr->fi->ntex == 1 &&
            st.rt.ncolor && recently_presented(st.rt.color[0]) && hud_fullscreen_quad() &&
            !g_settings.vr_bloom) {
            NvnTexture *source = shader_texture_object(1, &pr->fi->tex[0]);
            const TexBackend *image = source ? (const TexBackend *)source->backend : NULL;
            if (image && image->gpu_owned) { stat_skipped++; return; }
        }
        {   // Reverse-engineering capture for Skyward Sword's native bomb trajectory guide. Unlike the normal F9
            // summary, this preserves every ribbon segment and the complete landing mesh/instance stream so a
            // standalone VR renderer can reproduce the original geometry and material exactly.
            static int dump = -1, ribbon, marker;
            if (dump < 0) dump = getenv("LARECOMP_DUMP_BOMB_GUIDE") != NULL;
            if (dump && !vk_vr_eye() && last_hash == 0x02bee173fece2927ull && (r->x[2] == 12 || r->x[2] == 1116)) {
                int id = r->x[2] == 12 ? ribbon++ : marker++;
                if ((r->x[2] == 12 && id < 64) || (r->x[2] == 1116 && id < 4)) {
                    char path[128];
                    for (int stream = 0; stream < 2; stream++) {
                        if (!st.vbuf[stream].addr || !st.vbuf[stream].size) continue;
                        snprintf(path, sizeof path, "bomb-guide-%s-%02d-vbuf%d.bin",
                                 r->x[2] == 12 ? "ribbon" : "marker", id, stream);
                        FILE *f = fopen(path, "wb");
                        if (f) {
                            fwrite((const void *)(uintptr_t)st.vbuf[stream].addr, 1, (size_t)st.vbuf[stream].size, f);
                            fclose(f);
                        }
                    }
                    for (int slot = 0; slot < 18; slot++) {
                        const Range u = st.ubuf[0][slot];
                        if (!u.addr || !u.size || u.size > 0x100000) continue;
                        snprintf(path, sizeof path, "bomb-guide-%s-%02d-vubo%02d.bin",
                                 r->x[2] == 12 ? "ribbon" : "marker", id, slot);
                        FILE *f = fopen(path, "wb");
                        if (f) {
                            fwrite((const void *)(uintptr_t)u.addr, 1, (size_t)u.size, f);
                            fclose(f);
                        }
                    }
                    if (id == 0 && pr->fi->ntex) {
                        NvnTexture *tex = shader_texture_object(1, &pr->fi->tex[0]);
                        const TexBackend *backend = tex ? (const TexBackend *)tex->backend : NULL;
                        if (backend) {
                            snprintf(path, sizeof path, "bomb-guide-%s-texture.png", r->x[2] == 12 ? "ribbon" : "marker");
                            renderer_save_texture_tex((TexBackend *)backend, path);
                        }
                    }
                    LOG("[bomb-guide] %s %d: count %llu attrs %#x desc", 
                        r->x[2] == 12 ? "ribbon" : "marker", id, (unsigned long long)r->x[2], pr->vi->attr_mask);
                    for (int attr = 0; attr < st.nattrib; attr++) {
                        if (pr->vi->attr_mask & (1u << attr)) LOG(" a%d=%#x", attr, st.attrib[attr]);
                    }
                    LOG(" streams %#x/%#x sizes %llu/%llu vubo",
                        st.stream[0][0], st.stream[1][0], (unsigned long long)st.vbuf[0].size,
                        (unsigned long long)st.vbuf[1].size);
                    for (int slot = 0; slot < 18; slot++) {
                        if (st.ubuf[0][slot].addr) LOG(" u%d=%llu", slot, (unsigned long long)st.ubuf[0][slot].size);
                    }
                    LOG("\n");
                }
            }
        }
        extern void widescreen_backdrop_seen(uint64_t hash);
        if (last_hash) widescreen_backdrop_seen(last_hash);
        extern void widescreen_scene_ubo(const float *u, uint64_t size);  // (the camera's view-projection)
        if (st.ubuf[0][5].addr) widescreen_scene_ubo((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size);
    }
    if (g_debug_frame) debug_draw(pr, r, indexed);
    if (ct_on()) ct_draw(pr, "draw");
    alias_draw_check();
    pass_sample_hazard(pr);
    resolve_feedback(pr, mv);
    for (int k = 0; k < nfeedback && mv; k++)  // (a feedback read eye 1 can't have its own of: from the next frame on)
        if (!feedback_view1[k]) mv_blacklist(mv_sig(), "feedback");
    if (mv) stat_mv_draws++;
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    begin_pass();
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { stat_skipped++; return; }

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
    // (not over a world menu's own backdrop - the map's parchment blends by its destination alpha; a menu without one,
    // the Gear menu, is its pieces over the live world, like the HUD)
    if (g_hud_sub_from && k.ncolor && k.color[0] != VK_FORMAT_UNDEFINED && !(vr_world_menu_active() && g_menu_backdrop)) {
        // VR HUD layer: the game writes its 2D layer with RGB-only masks (the screen's alpha was never used). The HUD
        // texture accumulates coverage in alpha instead (alpha = src + dst * (1 - src)), colour blending unchanged:
        // premultiplied colour over transparent black, ready to be composited on the panel.
        if (k.write_mask[0] & 7) k.write_mask[0] |= 8;
        if (k.blend_on & 1) { k.blend[0][3] = 2; k.blend[0][4] = 6; }  // (NVN blend factors: ONE, ONE_MINUS_SRC_ALPHA)
    }
    k.a2c = st.ms[0xC] != 0;
    // NVN and Vulkan use the same logic-op values; OR preserves overlapping shadow-mask channels. A GPU without the
    // logicOp feature (device.c) gets OR as max blending, as on Metal (renderer.m): exact for channels that are 0 or 1 -
    // the shadow masks, where each caster ORs its own channel into a shared target (Link's red, an NPC's green).
    k.logic_op = g_vk.logic_op ? st.color_state[1] & 15 : VK_LOGIC_OP_COPY;
    if (!g_vk.logic_op && (st.color_state[1] & 15) == VK_LOGIC_OP_OR)
        for (int i = 0; i < st.rt.ncolor && i < 8; i++)
            if (st.rt.color[i] && !rt_kind(st.rt.color[i]->format)) k.or_max |= (uint8_t)(1u << i);
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1;
    k.depth_clamp = st.depth_clamp;
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
        if (wait) build_pipeline(pp);
        else run_job(build_pipeline, pp);
    }
    if (wait) wait_built(&pp->state);
    if (atomic_load(&pp->state) != 1) { stat_skipped++; return; }
    ss_enabled();
    if (ss_need(SS_PIPE, ss.pipe == pp->pipe)) { ss.pipe = pp->pipe; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe); }
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth && !pass_mask_injected;
    if (ss_need(SS_DT, ss.dt == (VkBool32)dt)) { ss.dt = dt; vkCmdSetDepthTestEnable(cb, dt); }
    VkBool32 dw = dt && st.ds[1];
    if (g_vk.depth_bounds && g_bounds_known != (int)pass_bounds_on) {  // (VR hidden area, begin_pass)
        g_bounds_known = pass_bounds_on;
        vkCmdSetDepthBoundsTestEnable(cb, pass_bounds_on);
        if (pass_bounds_on) vkCmdSetDepthBounds(cb, pass_bounds[0], pass_bounds[1]);
    }
    if (ss_need(SS_DW, ss.dw == dw)) { ss.dw = dw; vkCmdSetDepthWriteEnable(cb, dw); }
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
    if (ss_need(SS_DOP, ss.dop == dop)) { ss.dop = dop; vkCmdSetDepthCompareOp(cb, dop); }
    bool sten = st.ds[3] && pass_stencil;
    if (ss_need(SS_STEN, ss.sten == (VkBool32)sten)) { ss.sten = sten; vkCmdSetStencilTestEnable(cb, sten); }
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            uint32_t v[7] = {stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                             compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF), st.stencil_vmask[f] & 0xFF, st.stencil_mask[f] & 0xFF,
                             st.stencil_ref[f]};
            if (!ss_need(f ? SS_SFACE1 : SS_SFACE0, !memcmp(v, ss.sface[f], sizeof v))) continue;
            memcpy(ss.sface[f], v, sizeof v);
            vkCmdSetStencilOp(cb, face, (VkStencilOp)v[0], (VkStencilOp)v[1], (VkStencilOp)v[2], (VkCompareOp)v[3]);
            vkCmdSetStencilCompareMask(cb, face, v[4]);
            vkCmdSetStencilWriteMask(cb, face, v[5]);
            vkCmdSetStencilReference(cb, face, v[6]);
        }
    }
    if (ss_need(SS_BC, !memcmp(ss.bc, st.blend_color, sizeof ss.bc))) { memcpy(ss.bc, st.blend_color, sizeof ss.bc); vkCmdSetBlendConstants(cb, st.blend_color); }

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    if (vkey.flip_x) cull = 0;  // (mirror mode's composite: one full-screen quad, either winding)
    // (NVN Face: 1 front, 2 back, 3 front and back. 3 is how the game hides a material's polygons - the Remlit's night
    // face and claws by day (zeldaret/ss d_a_e_remly.cpp applyScaryMat) - and was drawn as none)
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT
                       : cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    if (ss_need(SS_CULL, ss.cull == cm)) { ss.cull = cm; vkCmdSetCullMode(cb, cm); }
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    VkFrontFace ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (ss_need(SS_FF, ss.ff == ff)) { ss.ff = ff; vkCmdSetFrontFace(cb, ff); }
    VkPrimitiveTopology tp = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (ss_need(SS_TOPO, ss.topo == tp)) { ss.topo = tp; vkCmdSetPrimitiveTopology(cb, tp); }
    VkBool32 pre = st.prim_restart && native && (mode == 3 || mode == 5);
    if (ss_need(SS_PRE, ss.pre == pre)) { ss.pre = pre; vkCmdSetPrimitiveRestartEnable(cb, pre); }
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        if (ss_need(SS_DBE, ss.dbe == (VkBool32)on)) { ss.dbe = on; vkCmdSetDepthBiasEnable(cb, on); }
        if (ss_need(SS_DB, !memcmp(ss.db, f, sizeof f))) { memcpy(ss.db, f, sizeof f); vkCmdSetDepthBias(cb, f[1], f[2], f[0]); }
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
    float tint[8] = {0};
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
    g_draw_samples_screen = false;  // (cbuf_info: screen-space texture projections are re-projected only for these)
    if (vr_stereo_active())
        for (int i = 0; i < pr->fi->ntex && !g_draw_samples_screen; i++) {
            NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
            const TexBackend *tb = t ? (const TexBackend *)t->backend : NULL;
            g_draw_samples_screen = tb && tb->gpu_owned && t->width >= 640 && t->height >= 360;
        }
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    extern bool g_tex_defer_ok, g_tex_deferred;
    g_tex_defer_ok = true, g_tex_deferred = false;  // (textures.c: new ones over this frame's budget wait a frame)
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    g_tex_defer_ok = false;
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (g_tex_deferred) { stat_skipped++; stat_tex_deferred++; return; }  // (drawn once its textures are ready)
    // Sky draws depth-tested scene geometry on the presented target; it must not be boxed like UI.
    if ((g_is_las || !st.ds[0]) && fabsf(pass_sx - pass_sy) > 0.01f * pass_sy &&
        st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
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
    VkDescriptorSet sets[2] = {vset, fset};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);  // (always new sets)
    if (pr->vi->uses_gmem_store) vc_mark_sbufs(0);
    if (pr->fi->uses_gmem_store) vc_mark_sbufs(1);
    if (pr->vi->uses_gmem_store || pr->fi->uses_gmem_store) g_outside_work = true;  // (later reads need a full barrier)

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
        // Primitive restart in a point / line / triangle LIST: the GPU drops the unfinished primitive and starts
        // counting again after the restart index. Vulkan lists cannot restart (and would read it as a vertex,
        // shifting every later triangle: Lanayru's sand floors lost whole stepped bands). Rebuilt here.
        const uint32_t per = mode == 0 ? 1 : mode == 1 ? 2 : mode == 4 ? 3 : 0;
        bool relisted = false;
        if (native && per > 1 && st.prim_restart && itype != 0 && count) {
            const uint32_t isz = itype == 1 ? 2 : itype == 2 ? 4 : 1;
            const uint32_t rv = isz == 2 ? (restart & 0xFFFFu) : isz == 1 ? (restart & 0xFFu) : restart;
            bool any = false;
            for (uint32_t i = 0; i < count && !any; i++) {
                const uint32_t v = isz == 2 ? ((const uint16_t *)src)[i] : isz == 4 ? ((const uint32_t *)src)[i] : ((const uint8_t *)src)[i];
                any = v == rv;
            }
            if (any) {
                ib = vk_transient((uint64_t)count * 4 + 4, 4);
                uint32_t *out = ib.ptr, n = 0, group[3], g = 0;
                for (uint32_t i = 0; i < count; i++) {
                    const uint32_t v = isz == 2 ? ((const uint16_t *)src)[i] : isz == 4 ? ((const uint32_t *)src)[i] : ((const uint8_t *)src)[i];
                    if (v == rv) { g = 0; continue; }
                    group[g++] = v;
                    if (g == per) {
                        for (uint32_t k2 = 0; k2 < per; k2++) {
                            out[n++] = group[k2];
                            if (group[k2] > max_index) max_index = group[k2];
                        }
                        g = 0;
                    }
                }
                itype_vk = VK_INDEX_TYPE_UINT32;
                icount = n;
                relisted = true;
            }
        }
        if (relisted) {
        } else if (native && itype != 0) {
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
    if (use_index && ss_need(SS_IB, ss.ib == ib.buf && ss.io == ib.off && ss.it == itype_vk)) {
        ss.ib = ib.buf; ss.io = ib.off; ss.it = itype_vk;
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

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
                if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == dbuf && ss.vo[s] == doff) continue;
                ss.vb[s] = dbuf; ss.vo[s] = doff; ss.vb_known |= 1u << s;
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
        if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == off) continue;
        ss.vb[s] = sp.buf; ss.vo[s] = off; ss.vb_known |= 1u << s;
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
            float rtint[8] = {0};
            extern void pot_gold_tint(float tint[8]);
            if (run_gold[ri]) pot_gold_tint(rtint);
            VkDescriptorSet rset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, rtint, 0, 0, 0, fii);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 1, 1, &rset, 0, NULL);
        }
        if (!use_index) vkCmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);
        else vkCmdDrawIndexed(cb, icount, run_n[ri], 0, base_vertex, base_inst + run_at[ri]);
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
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    if (getenv("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {
            if (!((pr->fi->cbuf_mask >> c) & 1) || !st.ubuf[1][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c].addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)st.ubuf[1][c].addr, (unsigned)st.ubuf[1][c].size);
            for (int k = 0; k < 96 && k * 4 < (int)st.ubuf[1][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "");
    }
}

// ================================================================== VR stereo: every command list recorded per eye
// With an OpenXR frame in stereo (runtime/vr.h), backend_execute records each list twice, right after each other:
// eye 0 as without VR, then eye 1 into eye-local render targets (textures.c vk_vr_set_eye) with the eye's scene
// camera (cbuf_info). Both eyes see the same inputs:
//   - renderer state: eye 1 starts from the `st` eye 0 started from;
//   - guest memory: the list's own CPU-side writes (inline uniform updates, buffer copies, readbacks) are
//     journaled during eye 0 and rolled back for eye 1, which repeats the updates and copies in order. Afterwards
//     every journaled range is set to eye 0's end state, so the game sees the writes of one execution only.
// Once-only work stays with eye 0: compute dispatches, fences, timing counters and GPU -> CPU readbacks.
// The rollback is safe against the game's CPU: these ranges are written by the command list itself, which the
// game does not touch again until the GPU work (this list) has completed.
typedef struct { uint64_t addr, size, old_off, end_off; } GuestWrite;
static struct {
    bool recording;  // eye 0 of a stereo list
    GuestWrite *writes;
    uint32_t n, cap;
    uint8_t *bytes;
    uint64_t used, bytes_cap;
} stereo;

static uint64_t stereo_save(uint64_t addr, uint64_t size) {
    if (stereo.used + size > stereo.bytes_cap) {
        uint64_t cap = stereo.bytes_cap ? stereo.bytes_cap : 1 << 16;
        while (cap < stereo.used + size) cap *= 2;
        uint8_t *bytes = realloc(stereo.bytes, cap);
        if (!bytes) FATAL("VR stereo journal: out of memory\n");
        stereo.bytes = bytes;
        stereo.bytes_cap = cap;
    }
    memcpy(stereo.bytes + stereo.used, (const void *)(uintptr_t)addr, size);
    uint64_t off = stereo.used;
    stereo.used += size;
    return off;
}

// [addr, addr + size) of guest memory is about to be written by the list (eye 0: remember what it held).
static void stereo_guest_write(uint64_t addr, uint64_t size) {
    if (!stereo.recording || !addr || !size) return;
    if (stereo.n == stereo.cap) {
        uint32_t cap = stereo.cap ? stereo.cap * 2 : 256;
        GuestWrite *w = realloc(stereo.writes, cap * sizeof *w);
        if (!w) FATAL("VR stereo journal: out of memory\n");
        stereo.writes = w;
        stereo.cap = cap;
    }
    stereo.writes[stereo.n++] = (GuestWrite){addr, size, stereo_save(addr, size), 0};
}

// After eye 0: keep its end state, then restore what the list started with (newest write first).
static void stereo_guest_rollback(void) {
    for (uint32_t i = 0; i < stereo.n; i++) stereo.writes[i].end_off = stereo_save(stereo.writes[i].addr, stereo.writes[i].size);
    for (uint32_t i = stereo.n; i-- > 0;) {
        const GuestWrite *w = &stereo.writes[i];
        memcpy((void *)(uintptr_t)w->addr, stereo.bytes + w->old_off, w->size);
    }
}

// After eye 1: guest memory as eye 0 left it.
static void stereo_guest_commit(void) {
    for (uint32_t i = 0; i < stereo.n; i++) {
        const GuestWrite *w = &stereo.writes[i];
        memcpy((void *)(uintptr_t)w->addr, stereo.bytes + w->end_off, w->size);
    }
    stereo.n = 0;
    stereo.used = 0;
}

// Caches that hold one eye's view of memory or images.
static void stereo_eye_switched(void) {
    guest_copy_flush();  // (shared constant-buffer copies: guest memory differs between the eyes' recordings)
    snap_reason = "vr eye";
    snapshots_invalidate(NULL);  // (feedback copies were made from the other eye's attachments)
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
    alias_before_write(t, 0, "upload");
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
                                                (uint32_t)level, (array ? (uint32_t)rg[2] : 0) + b->layer0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    t = alias_read(t);
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
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, b->layer0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
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
                                                (uint32_t)level, (array ? (uint32_t)rg[2] : 0) + (srcimg == b->image ? b->layer0 : 0), array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    wait_why = "texture -> buffer readback";
    renderer_flush();  // the CPU may read the result right after the fence
    stereo_guest_write(dst, total);
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

// Linear render targets (NVN flag 0x10) are ones the CPU reads straight from memory: the save files' thumbnails - the
// game draws the last presented frame into a 256x144 linear target and memcpy's it into save/<n>.screenshot, which was
// saved black (the texels stayed in the Vulkan image). So a draw into one is read back into guest memory, at the
// target's own row pitch, before the submit returns (renderer.m does the same on Metal). Only the thumbnails so far,
// so the wait costs nothing in play.
// ponytail: a readback per draw - batch per pass if a game draws a lot into linear targets.
static void linear_rt_writeback(void) {
    for (int i = 0; i < st.rt.ncolor && i < 8; i++) {
        NvnTexture *t = st.rt.color[i];
        const FormatInfo *fi = t ? nvn_format_info(t->format) : NULL;
        if (!t || !(t->flags & 0x10) || !fi || !fi->bpb || !t->stride || t->stride < (uint64_t)t->width * fi->bpb ||
            t->addr < HEAP_BASE || t->addr + (uint64_t)t->stride * t->height > HEAP_BASE + HEAP_MAX)
            continue;
        TexRefRec ref = {.tex = (uint64_t)(uintptr_t)t};
        uint64_t rs = st.copy_row_stride, is = st.copy_image_stride;
        st.copy_row_stride = t->stride, st.copy_image_stride = 0;
        copy_texture_to_buffer(&ref, t->addr);
        st.copy_row_stride = rs, st.copy_image_stride = is;
    }
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
    mv_note_later(dt);  // (VR multiview: eye 1 copies its own later)
    renderer_mark_gpu_owned(dt);
    if (s->scene_frame == renderer_present_count() + 1) d->scene_frame = s->scene_frame;  // (VR HUD layer: a scene copy)
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
    if (g_debug_frame || ct_on())
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    if (copy_opt() && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && sw == (int)s->width && sh == (int)s->height && dw == (int)d->width &&
        dh == (int)d->height && slv == 0 && dlv == 0 && sly == 0 && dly == 0 && alias_make(st_, dt, s, d)) {
        if (ct_on()) LOG("[ct] alias made %p -> %p\n", (void *)st_, (void *)dt);
        return;
    }
    alias_before_write(dt, 0, "copy dst");
    if (naliases) s = alias_read(st_)->backend;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly + s->layer0, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly + d->layer0, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly + s->layer0, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly + d->layer0, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly + s->layer0, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly + d->layer0, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline_srgb(VkFormat fmt, bool over, bool quad, int srgb);
static VkPipeline present_pipeline_ex(VkFormat fmt, bool over, bool quad) { return present_pipeline_srgb(fmt, over, quad, false); }
static VkPipeline present_pipeline(VkFormat fmt) { return present_pipeline_ex(fmt, false, false); }
static VkPipelineLayout quad_layout;  // (present's set layout; push: mat4 mvp + vec4 + vec4 uv rect, vertex stage)
// over: the premultiplied texture blended over what is there (the VR HUD on the desktop mirror); srgb: the texture is
// read through an sRGB view and encoded again (the VR mirror's layers: their own format, no reinterpreting view);
// 2: the texture's encoded values decoded for an sRGB target (the HUD panel drawn into the eye images)
static VkPipeline present_pipeline_srgb(VkFormat fmt, bool over, bool quad, int srgb) {
    static struct { VkFormat f; bool over, quad; int srgb; VkPipeline p; } cache[16];
    for (int i = 0; i < 16; i++)
        if (cache[i].p && cache[i].f == fmt && cache[i].over == over && cache[i].quad == quad && cache[i].srgb == srgb)
            return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
        VkPushConstantRange q = {VK_SHADER_STAGE_VERTEX_BIT, 0, 96};
        ci.pPushConstantRanges = &q;
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &quad_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader(quad ? "hud_quad.vert" : "present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(srgb == 2 ? "present_decode.frag" : srgb ? "present_srgb.frag" : over ? "present_over.frag" : "present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    if (over) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
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
                                       .pColorBlendState = &cb, .pDynamicState = &dy,
                                       .layout = quad ? quad_layout : present_layout};
    VkPipeline p;
    VK_CHECK(vk_create_graphics_pipelines(1, &pi, &p));
    for (int i = 0; i < 16; i++)
        if (!cache[i].p) { cache[i].f = fmt; cache[i].over = over; cache[i].quad = quad; cache[i].srgb = srgb; cache[i].p = p; break; }
    return p;
}

// present.vert's flip + crop (pv), then present.frag's colour matrix (Color blind mode: output_scale.c)
static void present_push(VkCommandBuffer cb, const float pv[8]) {
    float pc[20];
    memcpy(pc, pv, 8 * sizeof(float));
    output_color_matrix(pc + 8);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
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

// Optional performance mode: avoid the full-resolution AA/upscale chain; the
// final OpenXR blit still performs any required resize. It is opt-in so the
// default path retains identical visual quality.
static bool fast_post(void) {
    static bool env_read;
    if (!env_read) {
        env_read = true;
        const char *e = getenv("SKYRECOMP_VK_FAST_POST");
        if (e) {
            g_settings.vr_fast_post = atoi(e) != 0;
            LOG("[vk] initial fast post-processing from SKYRECOMP_VK_FAST_POST (VR Options remains live)\n");
        }
    }
    return g_settings.vr_fast_post;
}

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[16];
    for (int i = 0; i < 16; i++) if (cache[i].p && cache[i].fs == fs) return cache[i].p;
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
    VK_CHECK(vk_create_graphics_pipelines(1, &pi, &p));
    for (int i = 0; i < 16; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS. VR eye e (vk_vr_eye_post): 5 + 5 e + the same.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[16];  // (0-4 desktop, 5-14 VR eyes, 15 VR HUD + settings menu)
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        const bool copied = i == 2 || i == 4 || i == 7 || i == 9 || i == 12 || i == 14 || i == 15 || i == 16;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         (copied ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0)};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkResult vr = vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL);
        if (vr != VK_SUCCESS) {
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        }
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
        VkResult vr = vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL);
        if (vr != VK_SUCCESS) {
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        }
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
    if (fast_post()) return v;
    bool smaa = g_settings.antialias == AA_SMAA, fxaa = g_settings.antialias == AA_FXAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    bool nis = g_settings.upscaler == UPSCALE_NIS && (ow > b->width + 1 || oh > b->height + 1);
    // Supersampling (internal resolution above the window, e.g. 2x-4x): a filtered downscale (down.frag, as the VR
    // eyes), then SMAA at the window's size and a light RCAS. The present's bilinear sampling alone read 4 of the
    // 4-16 rendered pixels behind each window pixel (shimmering), and SMAA ran at the full rendered size.
    // LARECOMP_POST_DOWN=0: the bilinear present as before.
    static int down_on = -1;
    if (down_on < 0) { const char *e = getenv("LARECOMP_POST_DOWN"); down_on = !e || atoi(e) != 0; }
    const bool down = down_on && (b->width > ow + 1 || b->height > oh + 1) && b->width + 1 >= ow && b->height + 1 >= oh;
    if (!smaa && !fxaa && !fsr && !nis && !down) return v;
    if (vr_running()) return v;  // (VR: the mirror shows the eye image vk_vr_eye_post already made - RGP: SMAA at
                                 //  3138x3492 again cost ~1.2 ms per frame at 1.5x)
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    uint32_t sw = b->width, sh = b->height;  // (the size SMAA works at)
    if (down) {
        const float iw = (float)b->width, ih = (float)b->height;
        TexBackend *d = post_image(3, ow, oh);
        float pd[8] = {1.0f / iw, 1.0f / ih, iw / ow, ih / oh, (float)ow, (float)oh};
        VkImageView di[3] = {v};
        post_pass(cb, "down.frag", di, 1, d, pd, false);
        v = post_view(d);
        sw = ow, sh = oh;
    }
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)sw, h = (float)sh;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, sw, sh), *weights = post_image(1, sw, sh);
        TexBackend *out = post_image(2, sw, sh);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    } else if (fxaa) {
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "fxaa.frag", in, 1, out, m, false);
        v = post_view(out);
    }
    if (down) {
        TexBackend *r = post_image(4, ow, oh);
        float pr[8] = {exp2f(-0.5f), 0, 0, 0, (float)ow, (float)oh};  // (gentler than FSR's 0.2 stops: no upscale blur to undo)
        VkImageView ri_[3] = {v};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    } else if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    } else if (nis) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "nis_easu.frag", ei, 1, e, pe, false);
        float pr[8] = {0.35f, 0, 0, 0, (float)ow, (float)oh};  // (NVIDIA NIS sharpness: 0.35)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "nis_rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pup = -1;
        int up = down ? 3 : fsr ? 1 : nis ? 2 : 0, aa = smaa ? 1 : fxaa ? 2 : 0;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || aa != psm || up != pup)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : fxaa ? " fxaa" : "",
                down ? " downscale+rcas" : fsr ? " fsr1" : nis ? " nis" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = aa; pup = up;
    }
    return v;
}

// VR: eye `eye`'s game image -> the headset's eye size ow x oh (vr.c then copies the result 1:1). The Video tab's
// choices apply: rendered above the headset's size (supersampling), a filtered downscale (down.frag), SMAA
// ("Anti-aliasing") and a light RCAS sharpen; at native size, SMAA without resampling or sharpening; below it, SMAA then
// the "Upscaler" (FSR 1 / NIS) as on the desktop. Returns the image to copy. SKYRECOMP_VR_EYE_POST=0: none.
TexBackend *vk_vr_eye_post(int eye, TexBackend *b, uint32_t ow, uint32_t oh) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SKYRECOMP_VR_EYE_POST"); on = !e || atoi(e) != 0; }
    if (!on || !b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || eye < 0 || eye > 1) return b;
    const int base = 5 + 5 * eye;
    const bool fast = fast_post();
    const bool smaa = !fast && g_settings.antialias == AA_SMAA;
    const bool down = !fast && (b->width > ow + 1 || b->height > oh + 1);
    const bool up = !fast && !down && (b->width + 1 < ow || b->height + 1 < oh);
    const bool fsr = up && g_settings.upscaler == UPSCALE_FSR1, nis = up && g_settings.upscaler == UPSCALE_NIS;
    if (!smaa && !down && !fsr && !nis) return b;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    gp_mark(cb, "vr eye %d post %ux%u -> %ux%u%s%s", eye, b->width, b->height, ow, oh, smaa ? " smaa" : "",
            down ? " down" : fsr ? " fsr1" : nis ? " nis" : "");
    vk_label_begin(cb, eye ? "[VR] Eye 1 Post (SMAA / scale)" : "[VR] Eye 0 Post (SMAA / scale)");
    // SMAA runs at the smaller of the two sizes: after the downscale when supersampling (4.9 instead of 11 Mpixels
    // at 1.5x), before the upscale otherwise.
    VkImageView v = raw_view(b);
    TexBackend *res = b;
    const float iw = (float)b->width, ih = (float)b->height;
    if (down) {
        TexBackend *d = post_image(base + 3, ow, oh);
        float pd[8] = {1.0f / iw, 1.0f / ih, iw / ow, ih / oh, (float)ow, (float)oh};
        VkImageView di[3] = {v};
        post_pass(cb, "down.frag", di, 1, d, pd, false);
        v = post_view(d);
        res = d;
    }
    if (smaa) {
        const uint32_t sw = res->width, sh = res->height;
        float m[8] = {1.0f / sw, 1.0f / sh, (float)sw, (float)sh};
        TexBackend *edges = post_image(base, sw, sh), *weights = post_image(base + 1, sw, sh);
        TexBackend *out = post_image(base + 2, sw, sh);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
        res = out;
    }
    if (down) {
        TexBackend *r = post_image(base + 4, ow, oh);
        float pr[8] = {exp2f(-0.5f), 0, 0, 0, (float)ow, (float)oh};  // (gentler than FSR's 0.2 stops: no upscale blur to undo)
        VkImageView ri_[3] = {v};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        res = r;
    } else if (fsr || nis) {
        TexBackend *e = post_image(base + 3, ow, oh), *r = post_image(base + 4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, fsr ? "easu.frag" : "nis_easu.frag", ei, 1, e, pe, false);
        float pr[8] = {fsr ? exp2f(-0.2f) : 0.35f, 0, 0, 0, (float)ow, (float)oh};
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, fsr ? "rcas.frag" : "nis_rcas.frag", ri_, 1, r, pr, false);
        res = r;
    }
    vk_full_barrier(cb);
    vk_label_end(cb);
    static uint32_t pw, ph, pow_, poh;
    static int pmode = -1;
    const int mode = smaa | (down ? 2 : 0) | (fsr ? 4 : 0) | (nis ? 8 : 0);
    if (!eye && (b->width != pw || b->height != ph || ow != pow_ || oh != poh || mode != pmode))
        LOG("[post] VR eyes %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "",
            down ? " downscale+rcas" : fsr ? " fsr1" : nis ? " nis" : "", ow, oh);
    if (!eye) pw = b->width, ph = b->height, pow_ = ow, poh = oh, pmode = mode;
    return res;
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
    present_push(cb, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// (device.c, the VR desktop mirror) The HUD texture over the mirror in (vx, vy, vw, vh): blended (its premultiplied
// alpha), or opaque for a full-screen 2D frame (the map), as the headset shows it.
void vk_present_overlay(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8], bool opaque) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkPipeline p = present_pipeline_ex(g_vk.sc_format, !opaque, false);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
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

// (device.c, the VR desktop mirror) The HUD panel texture where the left eye sees it: mvp (column-major, as GLSL)
// takes the panel's (x, y) in -1..1 to the window's clip space; flip: a lower-left origin texture.
void vk_present_hud_quad(VkCommandBuffer cb, TexBackend *b, const float mvp[16], bool flip, bool opaque) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkPipeline p = present_pipeline_ex(g_vk.sc_format, !opaque, true);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, quad_layout, 0, 1, &ds, 0, NULL);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
    float pc[24];
    memcpy(pc, mvp, 16 * sizeof(float));
    pc[16] = flip ? 1.0f : 0.0f, pc[17] = pc[18] = pc[19] = 0;
    pc[20] = pc[21] = 0, pc[22] = pc[23] = 1;
    vkCmdPushConstants(cb, quad_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 6, 1, 0, 0);
}

// (device.c, VR) The HUD panel drawn into one eye's swapchain image (vr_eye_panel), over the world: src (the game's 2D
// layer, encoded values) decoded for the sRGB eye image; mvp: column-major clip-from-panel; rect: the part shown.
void vk_vr_panel_into_eye(VkCommandBuffer cb, void *image, uint32_t format, uint32_t w, uint32_t h, uint32_t layer,
                          TexBackend *src, const float mvp[16], const float rect[4], bool flip, bool opaque) {
    if (!image || !src || !src->image) return;
    enum { WRAPS = 8 };
    static TexBackend wrap[WRAPS];
    static int next;
    TexBackend *b = NULL;
    for (int i = 0; i < WRAPS && !b; i++)
        if (wrap[i].image == (VkImage)image && wrap[i].width == w && wrap[i].height == h && wrap[i].format == (VkFormat)format)
            b = &wrap[i];
    if (!b) {
        b = &wrap[next];
        next = (next + 1) % WRAPS;
        if (b->image) vk_views_forget(b);
        *b = (TexBackend){.image = (VkImage)image, .format = (VkFormat)format, .width = w, .height = h, .depth = 1,
                          .layers = 2, .levels = 1, .samples = 1, .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                          .base_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY};
    }
    static const VkComponentMapping id = {0};
    VkImageView target = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, layer, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    vk_end_pass();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = target,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline_srgb((VkFormat)format, !opaque, true, 2);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(src), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, quad_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pc[24] = {0};
    memcpy(pc, mvp, 16 * sizeof(float));
    pc[16] = flip ? 1.0f : 0.0f;
    memcpy(pc + 20, rect, 4 * sizeof(float));
    vkCmdPushConstants(cb, quad_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 6, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// (device.c, the VR desktop mirror) One composition layer as the left eye sees it (vr_mirror_layers): the very
// swapchain image the compositor gets, read in its own (sRGB) format and encoded again for the window.
void vk_present_layer(VkCommandBuffer cb, const VrMirrorLayer *l) {
    enum { WRAPS = 64 };
    static TexBackend wrap[WRAPS];
    static int next;
    TexBackend *b = NULL;
    for (int i = 0; i < WRAPS && !b; i++)
        if (wrap[i].image == (VkImage)l->image && wrap[i].width == l->width && wrap[i].height == l->height &&
            wrap[i].layers == l->layers && wrap[i].format == (VkFormat)l->format) b = &wrap[i];
    if (!b) {  // (a new swapchain image: its views are made on first use; one recycled after 64 others)
        b = &wrap[next];
        next = (next + 1) % WRAPS;
        if (b->image) vk_views_forget(b);
        *b = (TexBackend){.image = (VkImage)l->image, .format = (VkFormat)l->format, .width = l->width,
                          .height = l->height, .depth = 1, .layers = l->layers, .levels = 1, .samples = 1,
                          .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                          .base_type = l->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D};
    }
    const bool srgb = b->format == VK_FORMAT_R8G8B8A8_SRGB || b->format == VK_FORMAT_B8G8R8A8_SRGB;
    VkPipeline p = present_pipeline_srgb(g_vk.sc_format, l->blend, true, srgb ? 1 : 0);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    static const VkComponentMapping id = {0};
    VkImageView view = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, quad_layout, 0, 1, &ds, 0, NULL);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
    float pc[24] = {0};
    if (l->full) pc[0] = 1, pc[5] = -1, pc[10] = 1, pc[15] = 1;  // (the quad's -1..1 over the window, top row first)
    else memcpy(pc, l->mvp, 16 * sizeof(float));
    memcpy(pc + 20, l->rect, 4 * sizeof(float));
    vkCmdPushConstants(cb, quad_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 6, 1, 0, 0);
}

// ---- VR: the settings menu (ImGui) on the HUD panel. It is drawn into its own image (the window's format: the
// menu's pipeline is built for it), then over the HUD texture into post image 15, which the panel shows instead.
static TexBackend g_menu_img;
static TexBackend *menu_image(uint32_t w, uint32_t h) {
    TexBackend *d = &g_menu_img;
    if (d->image && (d->width != w || d->height != h || d->format != g_vk.sc_format)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = g_vk.sc_format, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT};  // (+ the VR menu panel's copy)
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = g_vk.sc_format; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (device.c, VR present, the menu open) The HUD panel's image with the settings menu over the game's 2D layer
// (hud: the HUD texture, or NULL), 1920 x 1080. flip: the HUD texture is bottom-up (the menu is drawn top-down).
TexBackend *vk_vr_menu_over_hud(TexBackend *hud, bool flip) {
    enum { W = 1920, H = 1080 };
    extern void menu_render(void *cmd, uint32_t target_w, uint32_t target_h);
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    TexBackend *m = menu_image(W, H);
    TexBackend *out = post_image(15, W, H);
    VkPipeline pipe = post_pipeline("hud_menu.frag");  // (created before any rendering begins: post_pass)
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(m),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    menu_render((void *)cb, W, H);
    vkCmdEndRendering(cb);
    (void)pipe;
    const VkImageView srcs[2] = {hud ? raw_view(hud) : post_view(m), post_view(m)};
    const float pc[8] = {(float)W, (float)H, flip ? 1.0f : 0.0f, hud ? 0.0f : 1.0f};
    post_pass(cb, "hud_menu.frag", srcs, 2, out, pc, false);
    vk_full_barrier(cb);
    return out;
}

// (device.c, VR present, the menu up) The settings menu / name entry alone, 1600 x 1000 (its own panel: vr.c),
// top-down, premultiplied (ImGui over a clear transparent image), layout GENERAL.
TexBackend *vk_vr_menu_panel(void) {
    enum { W = 1600, H = 1000 };
    extern void menu_render(void *cmd, uint32_t target_w, uint32_t target_h);
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    TexBackend *m = menu_image(W, H);
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(m),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    menu_render((void *)cb, W, H);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    return m;
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
    present_push(cb, pv);
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
        const uint64_t op_t0 = prof ? SDL_GetTicksNS() : 0;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_BARRIER:
            if (r->x[0]) {
                // NVN barriers can order attachment writes and subsequent texture reads.
                // A Vulkan memory barrier must be outside dynamic rendering.
                vk_end_pass_at(-1);  // (this full barrier also serves the next pass: begin_pass adds none)
                st.rt_dirty = true;
                vk_full_barrier(vk_cmd());
                g_outside_work = false;
                g_nwritten = 0;
                guest_copy_flush();
                static uint32_t seen_flags[16];  // (log: which NVN barrier kinds the game uses)
                for (int i = 0; i < 16; i++) {
                    if (seen_flags[i] == (uint32_t)r->x[0]) break;
                    if (!seen_flags[i]) { seen_flags[i] = (uint32_t)r->x[0]; LOG("[vk] game barrier flags %#x\n", seen_flags[i]); break; }
                }
            }
            break;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: {
            const uint8_t *next = x;
            // The fragment-rate attachment is selected at vkCmdBeginRendering.
            // Only a pass that actually has a fragment-rate attachment must
            // stop before its first alpha-tested draw. Splitting every ordinary
            // alpha-state transition produced hundreds of one-draw passes and
            // several milliseconds of attachment/barrier overhead per frame.
            if (in_pass && pass_vrs_attached && st.color_state[2] >= 8 && next[2] < 8) vk_end_pass();
            memcpy(st.color_state, next, 4);
            break;
        }
        case CMD_SET_ALPHA_REF: st.alpha_ref = r->f[0]; break;
        case CMD_SET_DEPTH_CLAMP: st.depth_clamp = r->x[0] != 0; break;
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
            mv_epoch++;
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
            {   // (vr_rig.c: which pass a camera block belongs to - planar reflection passes are recognised by pass)
                extern uint64_t g_vr_pass_id, g_vr_pass_epoch;
                g_vr_pass_id = (uint64_t)(uintptr_t)st.rt.color[0] * 31u ^ (uint64_t)(uintptr_t)st.rt.depth;
                g_vr_pass_epoch++;
            }
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
            if (ct_on() && index < st.rt.ncolor && st.rt.color[index])
                LOG("[ct] clear-color %p mask %#x sc %d %d %d %d\n", (void *)st.rt.color[index], (uint32_t)r->x[2], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            if (index < st.rt.ncolor && st.rt.color[index]) alias_before_write(st.rt.color[index], 0, "clear");
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t || t->video_uploaded) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            alias_before_write(t, 0, "clear");
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
            if (ct_on() && st.rt.depth)
                LOG("[ct] clear-depth %p d%d s%d sc %d %d %d %d\n", (void *)st.rt.depth, r->x[0] != 0, r->x[2] != 0, st.scissor[0][0],
                    st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            if (st.rt.depth && naliases) {  // (copy aliases: which aspects of the depth target this clear replaces whole)
                NvnTexture *d = st.rt.depth;
                bool whole = st.scissor[0][0] <= 0 && st.scissor[0][1] <= 0 && (int64_t)st.scissor[0][0] + st.scissor[0][2] >= (int64_t)d->width &&
                             (int64_t)st.scissor[0][1] + st.scissor[0][3] >= (int64_t)d->height &&
                             !(st.rt.has_dview && (((st.rt.dview.setmask & 1) && st.rt.dview.base_level) ||
                                                   ((st.rt.dview.setmask & 2) && st.rt.dview.min_layer)));
                for (int i = 0; i < st.rt.ncolor; i++)  // (the clear covers the smallest attachment only)
                    if (st.rt.color[i] && (st.rt.color[i]->width < d->width || st.rt.color[i]->height < d->height)) whole = false;
                VkImageAspectFlags full = 0;
                if (whole && r->x[0] != 0) full |= VK_IMAGE_ASPECT_DEPTH_BIT;
                if (whole && renderer_format_has_stencil(d->format) && (r->x[2] & 0xFF) == 0xFF) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (!renderer_format_has_stencil(d->format)) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (ct_on())
                    LOG("[ct] alias clear %p full %#x (whole %d, x0 %llu x1 %#llx x2 %#llx, dview %d/%#x, ncolor %d)\n", (void *)d, full, whole,
                        (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2], st.rt.has_dview,
                        st.rt.dview.setmask, st.rt.ncolor);
                alias_before_write(d, full, "clear");
            }
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
                NvnTexture *d = st.rt.depth;
                const bool whole = st.scissor[0][0] <= 0 && st.scissor[0][1] <= 0 &&
                                   (int64_t)st.scissor[0][0] + st.scissor[0][2] >= (int64_t)d->width &&
                                   (int64_t)st.scissor[0][1] + st.scissor[0][3] >= (int64_t)d->height;
                if (r->x[0] != 0 && whole) hidden_mask(r->f[0]);
            }
            break;
        case CMD_DISPATCH:  // (VR stereo: once, in eye 0; both eyes' draws read its results)
            if (!vk_vr_eye()) dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]);
            break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:  // (both VR eyes: later draws of the list read the values in order)
            stereo_guest_write(r->x[0] + r->x[2], r->x[3]);
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            if (vk_vr_eye()) break;  // (VR stereo: signalled once, by eye 0)
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            if (vk_vr_eye()) break;  // (VR stereo: the game times eye 0's work, as without VR)
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow: the
            // host clock now, replaced by the GPU's time at this point once the submission is done (cq_resolve).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            int mode = vk_counter_mode();
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (mode == 1 && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = ns_to_gpu_ticks(plat_now_ns()); }
            if (!out) break;
            out[0] = 0;
            out[1] = mode == 1 ? frame_ts : mode == 3 ? SDL_GetTicksNS() : ns_to_gpu_ticks(plat_now_ns());  // (3 legacy: ns, the old bug)
            static uint64_t *prev_out;  // (frame pacing: the pair's start)
            if (mode == 0 || mode == 2) out[1] = pace_end_counter(prev_out, out, out[1]);
            prev_out = out;
            {   // (test) LARECOMP_TEST_COUNTER_DELAY_MS=<ms>: stall the host after a frame's first counter - a slow CPU
                static double delay = -1;
                static uint64_t delayed_frame = ~0ull;
                if (delay < 0) delay = getenv("LARECOMP_TEST_COUNTER_DELAY_MS") ? atof(getenv("LARECOMP_TEST_COUNTER_DELAY_MS")) : 0;
                if (delay > 0 && vk_frame_counter() != delayed_frame) {
                    delayed_frame = vk_frame_counter();
                    SDL_DelayPrecise((Uint64)(delay * 1e6));
                }
            }
            if (mode == 2 && type == 0) {  // (type 0: timestamp)
                VkCommandBuffer cb = vk_cmd();  // (first: may begin the buffer, which resets the slot's queries)
                CounterQ *q = &cq[cur_sub];
                pthread_mutex_lock(&cq_lock);
                if (q->pool && q->n < CQ_MAX) {
                    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->pool, q->n);
                    q->out[q->n++] = out;
                }
                pthread_mutex_unlock(&cq_lock);
            }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER:  // (VR stereo: read back once, by eye 0)
            if (!vk_vr_eye()) copy_texture_to_buffer(x, r->x[3]);
            break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            // (VR eye 1: eye 0's wait already made the source current; the copy is repeated so the list's later
            // commands see the same memory in both eyes)
            if (!vk_vr_eye()) {
                wait_why = "buffer -> buffer copy";
                renderer_flush();
            }
            stereo_guest_write(r->x[1], r->x[2]);
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        if (prof && r->op < CMD_COUNT && r->op != CMD_CALL) op_ns[r->op][vk_vr_eye() ? 1 : 0] += SDL_GetTicksNS() - op_t0;
        p += r->size;
    }
}

// Eye 1 of a stereo list starts from the state eye 0 started from, and the game sees eye 0's end state.
static void exec_list_stereo(uint64_t handle) {
    static __typeof__(st) start, end;  // (static: the state is several KB)
    const uint64_t draws_before = stat_draws;
    start = st;
    vk_vr_set_stereo(true);
    stereo.recording = true;
    g_stereo_camera_patched = false;
    g_stereo_shadow_or_depth = false;
    stereo.n = 0;
    stereo.used = 0;
    g_mv_list = mv_enabled();
    mv_epoch = 0, mv_npass = 0, mv_nlater = 0, pass_mv_skip = false;
    vk_vr_list_begin();
    static int memo_on = -1;  // SKYRECOMP_VR_NO_EYE_MEMO=1: eye 1 copies its guest data again (A/B)
    if (memo_on < 0) memo_on = !getenv("SKYRECOMP_VR_NO_EYE_MEMO");
    eye_memo_list++;
    eye_memo_mode = memo_on ? 1 : 0;
    exec_list(handle, 0);
    eye_memo_mode = 0;
    stereo.recording = false;
    vk_end_pass();
    end = st;

    static int share_independent = -1;
    if (share_independent < 0) share_independent = getenv("SKYRECOMP_VR_NO_SHARE_INDEPENDENT") ? 0 : 1;
    static int min_draws = -1;
    if (min_draws < 0) {
        const char *e = getenv("SKYRECOMP_VR_SHARE_MIN_DRAWS");
        min_draws = e ? atoi(e) : 1;
        if (min_draws < 1) min_draws = 1;
    }
    if (share_independent && stat_draws - draws_before >= (uint64_t)min_draws &&
        !g_stereo_camera_patched && !g_stereo_shadow_or_depth && vk_vr_list_shareable()) {
        vk_vr_list_share_outputs();
        vk_vr_set_stereo(false);
        stereo.n = 0;
        stereo.used = 0;
        st = end;
        st.rt_dirty = true;
        stat_vr_shared_lists++;
        g_mv_list = false;
        return;
    }

    stereo_guest_rollback();

    // SKYRECOMP_VR_SPLIT_EYES=1: eye 0 goes to the GPU now, which works on it while eye 1 is replayed (RGP: the GPU
    // idled ~30% of a frame waiting for lists submitted once both eyes were recorded). Measured neutral in Skyloft:
    // at 1.0x the replay thread itself is the limit (~7.1 ms of CPU for a 6.9 ms frame), above it the GPU.
    static int split = -1, split_min = -1;
    if (split < 0) {
        const char *e = getenv("SKYRECOMP_VR_SPLIT_EYES");  // (opt-in: measured neutral - see below)
        split = e && atoi(e) != 0;
        e = getenv("SKYRECOMP_VR_SPLIT_MIN_DRAWS");
        split_min = e ? atoi(e) : 32;
    }
    if (split && stat_draws - draws_before >= (uint64_t)split_min) {
        renderer_submit();
        stat_eye_splits++;
    }

    st = start;
    st.rt_dirty = true;  // (a new pass: eye 1's attachments)
    mv_epoch = 0, pass_mv_skip = false;
    vk_vr_set_eye(1);
    stereo_eye_switched();
    const uint64_t e1_t0 = prof ? SDL_GetTicksNS() : 0, e1_draw = prof_draw, e1_sets = prof_sets, e1_vtx = prof_vtx,
                   e1_pre = prof_pre, e1_key = prof_key, e1_dyn = prof_dyn, e1_tex = prof_tex, e1_draws = stat_draws;
    // Depth/shadow lists deliberately upload their matrices again for eye 1.
    eye_memo_mode = memo_on && !g_stereo_shadow_or_depth ? 2 : 0;
    exec_list(handle, 0);
    eye_memo_mode = 0;
    vk_end_pass();
    if (prof) {
        prof_e1_exec += SDL_GetTicksNS() - e1_t0;
        prof_e1_draw += prof_draw - e1_draw; prof_e1_sets += prof_sets - e1_sets; prof_e1_vtx += prof_vtx - e1_vtx;
        prof_e1_pre += prof_pre - e1_pre; prof_e1_key += prof_key - e1_key; prof_e1_dyn += prof_dyn - e1_dyn;
        prof_e1_tex += prof_tex - e1_tex; prof_e1_draws += stat_draws - e1_draws;
    }
    vk_vr_set_eye(0);
    vk_vr_set_stereo(false);
    stereo_eye_switched();
    g_mv_list = false, pass_mv_skip = false;

    stereo_guest_commit();
    st = end;
    st.rt_dirty = true;
}

void backend_execute(uint64_t handle) {
    PROF_T0();
    // The first list of a frame begins the OpenXR frame: one pose sample for both eyes of every list in it.
    float vr_sx, vr_sy;
    if (vr_running() && vr_render_scale(&vr_sx, &vr_sy)) renderer_set_scale(vr_sx, vr_sy);
    const bool stereo_list = vr_running() && vr_frame_begin();  // (xrWaitFrame for the frame's first list)
    const uint64_t list_t0 = SDL_GetTicksNS();
    if (stereo_list) exec_list_stereo(handle);
    else exec_list(handle, 0);
    g_list_ns += SDL_GetTicksNS() - list_t0;
    g_lists++;
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int each = -1;
    if (each < 0) each = getenv("LARECOMP_VK_SUBMIT_EACH_LIST") != NULL || getenv("LARECOMP_SYNC_SUBMIT") != NULL;
    if (each) {
        if (getenv("LARECOMP_SYNC_SUBMIT")) renderer_flush();
        else renderer_submit();
    }
}

void backend_submit_commands(void) {
    static int each = -1;
    if (each < 0) each = getenv("LARECOMP_VK_SUBMIT_EACH_LIST") != NULL || getenv("LARECOMP_SYNC_SUBMIT") != NULL;
    if (!each) renderer_submit();
}

// (test) An RGBA8 image's bytes as they are, alpha included (renderer_save_texture_tex draws through the present
// shader, which writes alpha 1): the VR HUD layer's transparency.
void vk_save_texture_raw(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->format != VK_FORMAT_R8G8B8A8_UNORM && b->format != VK_FORMAT_R8G8B8A8_SRGB)) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)b->width * b->height * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, b->layer0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    extern void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
    vk_save_png(path, out.ptr, b->width, b->height);
}
