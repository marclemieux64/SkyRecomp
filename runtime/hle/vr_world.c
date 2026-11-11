// VR: the 3D world keeps being drawn while a full-screen menu (the map) pauses the game.
//
// On a flat screen the map covers everything, so the game stops drawing the world behind it; in a headset the world
// vanished around the player. Three things stop it (found 2026-10-25: draw-call chains, function profiles, snapshot
// diffs and watchpoints; docs/vr.md):
//  1. The map's "enter full screen" state callback (main+0xbce320) sets the pending draw mask (|= 0x10): skipped.
//  2. preDraw (vtable +0x38, called by the process-tree visitor main+0xf0fb3c) refuses while the draw mask
//     (main+0x1827374, copied each frame from main+0x1827378 by main+0xb9a3e4) is set and the node's properties don't
//     match it: the preDraws that read it run with it cleared (for menus that set it some other way).
//  3. The world's objects enter the 3D scene root each frame during their own execute (logic) phase, which a paused
//     game skips (preExecute main+0xb175c0 refuses while the execute mask main+0x1827370 is set): the scene is empty.
//     The objects still exist, frozen: the root's children of the last unpaused frame are re-entered (the root's own
//     insert) at the start of each paused frame's execute phase. Nothing runs that would move the game on.
// The renderer then shows the menu's 2D layer as an opaque panel in front of the live world (nvn/vulkan/exec.c).
//
// On with VR and the HUD panel (both by default) and VR Options > World Behind Menus; SKYRECOMP_VR_WORLD_IN_MENUS=0
// turns it off, =1 forces it on.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "../vr/vr.h"

#define DRAW_MASK (MAIN_BASE + 0x1827374)
#define EXEC_MASK (MAIN_BASE + 0x1827370)  // (the execute phase's: set while a menu pauses the game)

extern uint64_t renderer_present_count(void);

static bool world_in_menus(void) {
    static int env = -2, panel = -1;  // (env: -1 unset, else forced 0 / 1)
    if (env < -1) {
        const char *e = getenv("SKYRECOMP_VR_WORLD_IN_MENUS");
        env = e ? atoi(e) != 0 : -1;
        extern bool vk_hud_layer_enabled(void);  // (the panel: on by default, SKYRECOMP_VR_HUD_LAYER=0)
        panel = vk_hud_layer_enabled();
    }
    // (VR Options > World Behind Menus, read live: it takes effect from the next menu opened)
    const bool on = env >= 0 ? env : panel && g_settings.vr_world_in_menus;
    return on && vr_running();
}

// ---- the scene root: global at main+0x2bfe688; children: array +0xf0, capacity +0xf8, count +0xfc
#define SCENE_ROOT_PTR (MAIN_BASE + 0x2bfe688)
#define SCENE_ROOT_VTABLE (MAIN_BASE + 0x1699050ull)
enum { MAX_CHILDREN = 1024 };
static uint64_t g_snap[MAX_CHILDREN], g_snap_vt[MAX_CHILDREN];  // (children; their vtables when taken)
static uint32_t g_nsnap;
static uint64_t g_replay_frame = ~0ull;

static uint64_t scene_root(void) {
    const uint64_t r = *(const uint64_t *)(uintptr_t)SCENE_ROOT_PTR;
    return r >= (MAIN_BASE + 0x0ull) && r < 0x7800000000ull && *(const uint64_t *)(uintptr_t)r == SCENE_ROOT_VTABLE ? r : 0;
}

// (execute phase of an unpaused frame) The list fills during the phase and is emptied before the draw: the fullest
// list of the frame is kept - and the frame before's whole one (g_prev). The frame a menu opens is cut short: the
// pause begins part-way through its execute phase, the objects after that point never run and never enter, and those
// that enter after the last preExecute of any frame are not seen here at all - with this frame's list alone, those
// objects were missing behind the menu. A new frame is known by the list having been emptied (not by the present
// count: VR presents on its own clock, part-way through a phase).
static uint64_t g_prev[MAX_CHILDREN], g_prev_vt[MAX_CHILDREN];
static uint32_t g_nprev, g_snap_seen;  // (g_snap_seen: the list's count at the last look)
static bool g_was_running;             // (an unpaused frame since the last replay: the next replay is a menu opening)
static uint64_t g_menu_opened;         // (the frame the last menu over the world opened; 0: none yet)
uint64_t vr_world_menu_opened_frame(void) { return g_menu_opened; }
static void scene_snapshot(void) {
    const uint64_t r = scene_root();
    if (!r) return;
    g_was_running = true;
    const uint64_t *list = *(uint64_t *const *)(uintptr_t)(r + 0xf0);
    const uint32_t n = *(const uint32_t *)(uintptr_t)(r + 0xfc);
    if (!list || n > MAX_CHILDREN) return;
    if (n < g_snap_seen) {  // (emptied: a new frame - the one before is kept whole)
        memcpy(g_prev, g_snap, g_nsnap * sizeof *g_snap), memcpy(g_prev_vt, g_snap_vt, g_nsnap * sizeof *g_snap_vt);
        g_nprev = g_nsnap, g_nsnap = 0;
    }
    g_snap_seen = n;
    if (n <= g_nsnap) return;
    memcpy(g_snap, list, n * sizeof *g_snap);
    for (uint32_t i = 0; i < n; i++) g_snap_vt[i] = g_snap[i] ? *(const uint64_t *)(uintptr_t)g_snap[i] : 0;
    g_nsnap = n;
}

// (execute phase of a paused frame, once - and again if the list was emptied since) The last unpaused frame's
// children go back in, and the frame before's that it lacks.
static void scene_replay(Cpu *c) {
    static uint32_t replayed_n;  // (the list's count after the last replay)
    const uint64_t f = renderer_present_count(), r = scene_root();
    if (!r || (!g_nsnap && !g_nprev)) return;
    if (f == g_replay_frame && *(const uint32_t *)(uintptr_t)(r + 0xfc) >= replayed_n) return;
    g_replay_frame = f;
    const uint64_t insert = *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)r + 0x60);  // (main+0xfecfe4)
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    int added = 0, added_prev = 0;
    const uint32_t before = *(const uint32_t *)(uintptr_t)(r + 0xfc);
    for (uint32_t j = 0; j < g_nsnap + g_nprev; j++) {
        const bool prev = j >= g_nsnap;
        const uint64_t child = prev ? g_prev[j - g_nsnap] : g_snap[j], vt = prev ? g_prev_vt[j - g_nsnap] : g_snap_vt[j];
        const uint64_t *list = *(uint64_t *const *)(uintptr_t)(r + 0xf0);
        const uint32_t n = *(const uint32_t *)(uintptr_t)(r + 0xfc);
        bool present = false;
        for (uint32_t k = 0; k < n && !present; k++) present = list[k] == child;
        if (present) continue;
        // (an object deleted since - an arrow that landed while the item wheel was open... - is freed memory:
        // handing it back made the game abort; its vtable no longer matches)
        if (!child || *(const uint64_t *)(uintptr_t)child != vt) continue;
        c->x[0] = r;  // insert(root, index, child) -> bool: refuses a child that has a parent (+8) or a full list
        c->x[1] = n;
        c->x[2] = child;
        guest_call(c, insert);
        const int ok = (uint32_t)c->x[0] & 1;
        added += ok, added_prev += ok && prev;
    }
    memcpy(c->x, saved, sizeof saved);
    replayed_n = *(const uint32_t *)(uintptr_t)(r + 0xfc);
    if (g_was_running) {  // (a menu opening: how much of the world each list held)
        g_was_running = false;
        g_menu_opened = f;
        LOG("[vr-world] menu at frame %llu (draw mask %#x): the scene held %u objects; %d frozen ones re-entered (this frame's "
            "list %u, the frame before's %u - %d only in the frame before's)\n", (unsigned long long)f,
            *(volatile uint32_t *)(uintptr_t)DRAW_MASK, before, added, g_nsnap, g_nprev, added_prev);
    }
}

// (RE) SKYRECOMP_VR_WORLD_TRACE=1, without VR: what a menu opening leaves of the scene - the lists are kept as in VR,
// nothing is re-entered; logged at the first paused preExecute: the list as the pause finds it, this frame's kept
// list, the frame before's, and the objects only the frame before's holds (their vtables)
// (and every object of the scene root's type in the heap, with the most children each held since the last pause: is
// there a list besides the one re-entered?)
static uint64_t g_tr_root[32];
static uint32_t g_tr_peak[32], g_tr_n;
static void scene_trace_roots(void) {
    static unsigned calls;
    if (calls++ % 4096 == 0) {
        uint64_t found[32];
        uint32_t nf = 0;
        for (uint64_t a = (MAIN_BASE + 0x5000000ull); a < (MAIN_BASE + 0x8000000ull) && nf < 32; a += 8)
            if (*(const uint64_t *)(uintptr_t)a == SCENE_ROOT_VTABLE) found[nf++] = a;
        uint32_t peaks[32];
        for (uint32_t i = 0; i < nf; i++) {  // (peaks kept for those already known)
            peaks[i] = 0;
            for (uint32_t k = 0; k < g_tr_n; k++)
                if (g_tr_root[k] == found[i]) peaks[i] = g_tr_peak[k];
        }
        memcpy(g_tr_root, found, sizeof found), memcpy(g_tr_peak, peaks, sizeof peaks), g_tr_n = nf;
    }
    for (uint32_t i = 0; i < g_tr_n; i++) {
        if (*(const uint64_t *)(uintptr_t)g_tr_root[i] != SCENE_ROOT_VTABLE) continue;
        const uint32_t n = *(const uint32_t *)(uintptr_t)(g_tr_root[i] + 0xfc);
        if (n > g_tr_peak[i] && n < 100000) g_tr_peak[i] = n;
    }
}

static void scene_trace(void) {
    const uint64_t r = scene_root();
    if (!r || !g_was_running) return;
    g_was_running = false;
    {
        char roots[900];
        int rl = 0;
        for (uint32_t i = 0; i < g_tr_n && rl < (int)sizeof roots - 48; i++) {
            rl += snprintf(roots + rl, sizeof roots - rl, " %llx%s:%u", (unsigned long long)g_tr_root[i], g_tr_root[i] == r ? "(the root)" : "",
                           g_tr_peak[i]);
            g_tr_peak[i] = 0;
        }
        LOG("[vr-world] trace: the root is %#llx; objects of its type and their most children:%s\n", (unsigned long long)r, rl ? roots : " none in the heap range");
    }
    const uint32_t n = *(const uint32_t *)(uintptr_t)(r + 0xfc);
    char line[700];
    int len = 0, only = 0;
    for (uint32_t i = 0; i < g_nprev; i++) {
        bool in = false;
        for (uint32_t k = 0; k < g_nsnap && !in; k++) in = g_snap[k] == g_prev[i];
        if (in || !g_prev[i] || *(const uint64_t *)(uintptr_t)g_prev[i] != g_prev_vt[i]) continue;
        if (only++ < 24 && len < (int)sizeof line - 16)
            len += snprintf(line + len, sizeof line - len, " %llx", (unsigned long long)(g_prev_vt[i] - MAIN_BASE));
    }
    LOG("[vr-world] trace: pause at frame %llu: the scene's list holds %u (%s), this frame's kept list %u, the frame before's %u, "
        "%d only in the frame before's%s%s\n", (unsigned long long)renderer_present_count(), n,
        n && n >= g_nsnap ? "part-way through the frame" : "emptied: at a frame's start", g_nsnap, g_nprev, only,
        only ? " - vtables main+" : "", only ? line : "");
}

// A menu pauses the game and the frozen world is re-entered (this frame or the last).
// It stays so for half a second after: a closing menu keeps fading out once the game runs again, and its layers
// must keep their routing (else, for those frames, its full-screen layers flashed flat over the whole view).
bool vr_world_menu_active(void) {
    static uint64_t last = ~0ull;
    const uint64_t now = renderer_present_count();
    if (g_replay_frame != ~0ull && g_replay_frame + 1 >= now && *(volatile uint32_t *)(uintptr_t)EXEC_MASK &&
        world_in_menus())
        last = now;
    return last != ~0ull && now - last < 30;
}

static void shadow_trace(const char *where);

// ---- the shadows behind a menu. The models' shadows (the frame's first pass: each caster's models drawn from the
// light, blurred into a 303x303 texture, laid over the scene - main+0xe23d70 at +0x21a8) come from a list the objects
// fill anew every frame during their execute (main+0xb11aa4, called by 147 of them: main+0xf1eb40(list, handle,
// priority, 1) takes a slot, main+0xf1ed78(slot, model, matrix) adds a model to it) and the pass empties: a paused
// game leaves it empty (measured 2026-11-10: the pass was the one missing behind the Gear menu). The list: the object
// at *(main+0x2bfe6a8); its slots (0x180 bytes each) from [+0x98], [+0xba] in use; the list's first / last at +0x80 /
// +0x88, its count +0x90; a slot's handle at +0x20 (the handle's +8: its slot), its models' count at +0x179.
// As for the scene: the state as the last running frame's objects left it is put back in each paused frame - the list
// object's fields, the slots in use and the handles' slot pointers, as they were (everything in them is frozen).
#define SHADOW_LIST_PTR (MAIN_BASE + 0x2bfe6a8ull)
enum { SH_MAX_SLOTS = 96, SH_SLOT = 0x180, SH_HEAD = 0x80, SH_HEAD_BYTES = 0x40 };
typedef struct {
    uint64_t list, pool;
    uint32_t used, models;                 // (slots in use; their models' counts summed)
    uint8_t head[SH_HEAD_BYTES];           // (the list object's +0x80 .. +0xc0)
    uint8_t slots[SH_MAX_SLOTS * SH_SLOT];
    uint64_t handle[SH_MAX_SLOTS], handle_first[SH_MAX_SLOTS];  // (each slot's handle; its first word then)
} ShadowSnap;
static ShadowSnap g_sh_cur, g_sh_last;     // (the frame being filled; the last one kept whole)
static bool g_sh_have;
static bool guest_ptr(uint64_t p) { return p >= MAIN_BASE && p < 0x7800000000ull && !(p & 7); }
static uint64_t shadow_list(uint64_t *pool, uint32_t *used) {
    const uint64_t l = *(const uint64_t *)(uintptr_t)SHADOW_LIST_PTR;
    if (!guest_ptr(l)) return 0;
    *pool = *(const uint64_t *)(uintptr_t)(l + 0x98);
    *used = *(const uint8_t *)(uintptr_t)(l + 0xba);
    return guest_ptr(*pool) && *used <= SH_MAX_SLOTS ? l : 0;
}
// (an unpaused frame, before and after each object's execute) The fullest state of the frame is kept.
static void shadow_sample(void) {
    uint64_t pool;
    uint32_t used;
    const uint64_t l = shadow_list(&pool, &used);
    if (!l) return;
    if (used < g_sh_cur.used || l != g_sh_cur.list) {  // (emptied: a new frame - the one before is kept whole)
        if (g_sh_cur.used) g_sh_last = g_sh_cur, g_sh_have = true;
        g_sh_cur.used = g_sh_cur.models = 0, g_sh_cur.list = l;
    }
    if (!used) return;
    uint32_t models = 0;
    for (uint32_t i = 0; i < used; i++) models += *(const uint8_t *)(uintptr_t)(pool + i * SH_SLOT + 0x179);
    if (used == g_sh_cur.used && models == g_sh_cur.models) return;
    g_sh_cur.pool = pool, g_sh_cur.used = used, g_sh_cur.models = models;
    memcpy(g_sh_cur.head, (const void *)(uintptr_t)(l + SH_HEAD), SH_HEAD_BYTES);
    memcpy(g_sh_cur.slots, (const void *)(uintptr_t)pool, (size_t)used * SH_SLOT);
    for (uint32_t i = 0; i < used; i++) {
        const uint64_t h = *(const uint64_t *)(g_sh_cur.slots + i * SH_SLOT + 0x20);
        g_sh_cur.handle[i] = guest_ptr(h) ? h : 0;
        g_sh_cur.handle_first[i] = guest_ptr(h) ? *(const uint64_t *)(uintptr_t)h : 0;
    }
}
// (a paused frame, before and after each object's execute) The kept state goes back while the list is empty.
static void shadow_restore(void) {
    static int on = -1;
    if (on < 0) on = getenv("SKYRECOMP_VR_WORLD_SHADOWS") ? atoi(getenv("SKYRECOMP_VR_WORLD_SHADOWS")) : 3;
    if (!(on & 1)) return;
    if (g_sh_cur.used) g_sh_last = g_sh_cur, g_sh_have = true, g_sh_cur.used = g_sh_cur.models = 0;  // (the pause: the frame is whole)
    uint64_t pool;
    uint32_t used;
    const uint64_t l = shadow_list(&pool, &used);
    if (!l || used || !g_sh_have || l != g_sh_last.list || pool != g_sh_last.pool) return;
    // (a handle gone since - its object deleted behind the menu: nothing is put back, rather than a slot of freed memory)
    for (uint32_t i = 0; i < g_sh_last.used; i++)
        if (!g_sh_last.handle[i] || *(const uint64_t *)(uintptr_t)g_sh_last.handle[i] != g_sh_last.handle_first[i]) {
            g_sh_have = false;
            LOG("[vr-world] menu: a shadow's owner is gone - the shadows are left out\n");
            return;
        }
    memcpy((void *)(uintptr_t)pool, g_sh_last.slots, (size_t)g_sh_last.used * SH_SLOT);
    memcpy((void *)(uintptr_t)(l + SH_HEAD), g_sh_last.head, SH_HEAD_BYTES);
    for (uint32_t i = 0; i < g_sh_last.used; i++) *(uint64_t *)(uintptr_t)(g_sh_last.handle[i] + 8) = pool + (uint64_t)i * SH_SLOT;
    static uint64_t said;
    if (renderer_present_count() > said + 600) {
        said = renderer_present_count();
        LOG("[vr-world] menu: %u shadow casters (%u models) put back\n", g_sh_last.used, g_sh_last.models);
    }
}

#define PRE_EXECUTE (MAIN_BASE + 0xb175c0ull)
#define POST_EXECUTE (MAIN_BASE + 0xb175f8ull)
static GuestFn g_orig_post_execute;
static GuestFn g_orig_pre_execute;
// (RE) SKYRECOMP_RE_ACTORS=<frame>: the scene root's children that frame - address, vtable, and the floats where
// Link's actor keeps his position (+0x148 x, +0x150 z) - to find objects near him (a door) and their fields
static void re_actors(void) {
    {   // (RE) SKYRECOMP_RE_POKE=<hex main offset>,<hex u32>,<from frame>: written every frame from then on
        static int init;
        static uint64_t off, from;
        static uint32_t val;
        if (!init) {
            init = 1;
            const char *e = getenv("SKYRECOMP_RE_POKE");
            if (!e || sscanf(e, "%llx,%x,%llu", (unsigned long long *)&off, &val, (unsigned long long *)&from) != 3) off = 0;
        }
        if (off && renderer_present_count() >= from) *(volatile uint32_t *)(uintptr_t)(MAIN_BASE + off) = val;
    }
    {   // (RE) SKYRECOMP_RE_CANCEL_DOOR=<hex u32>: once the scene's list more than doubles (a room activated by the door
        // sequence), Link's action (actor +0x118) is set to that value once
        static int on = -1;
        static uint32_t val, base;
        if (on < 0) on = getenv("SKYRECOMP_RE_CANCEL_DOOR") != NULL, val = on ? (uint32_t)strtoul(getenv("SKYRECOMP_RE_CANCEL_DOOR"), NULL, 16) : 0;
        const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);
        const bool have_link = link >= (MAIN_BASE + 0x0ull) && link < 0x7800000000ull;
        static bool armed;  // (Link stood at a door: the doubling that follows is the door's room)
        {
            static int said;
            if (on == 1 && !said++) LOG("[re] cancel: watching (link %d)\n", have_link);
        }
        if (have_link && *(const uint32_t *)(uintptr_t)(link + 0x118) == 0x6a) {
            if (!armed && on == 1) LOG("[re] cancel: armed at frame %llu, %u nodes\n", (unsigned long long)renderer_present_count(), g_nsnap);
            armed = true, base = g_nsnap;
        }
        if (on == 1 && armed && renderer_present_count() % 20 == 0)
            LOG("[re] cancel watch: frame %llu nodes %u base %u action %#x\n", (unsigned long long)renderer_present_count(),
                g_nsnap, base, have_link ? *(const uint32_t *)(uintptr_t)(link + 0x118) : 0);
        if (on == 1 && armed && have_link && g_nsnap) {
            if (g_nsnap > 2 * base) {
                LOG("[re] scene %u -> %u nodes at frame %llu: Link's action %#x -> %#x\n", base, g_nsnap,
                    (unsigned long long)renderer_present_count(), *(const uint32_t *)(uintptr_t)(link + 0x118), val);
                *(volatile uint32_t *)(uintptr_t)(link + 0x118) = val;
                on = 2;
            }
        }
    }
    {   // (RE) SKYRECOMP_RE_POKE_ONCE=<hex address>,<hex u32>,<frame>: written once, that frame
        static int init;
        static uint64_t addr, at, val;  // (a value over 32 bits is written as a u64)
        if (!init) {
            init = 1;
            const char *e = getenv("SKYRECOMP_RE_POKE_ONCE");
            if (!e || sscanf(e, "%llx,%llx,%llu", (unsigned long long *)&addr, (unsigned long long *)&val, (unsigned long long *)&at) != 3) addr = 0;
        }
        if (addr && renderer_present_count() >= at) {
            if (val >> 32) *(volatile uint64_t *)(uintptr_t)addr = val;
            else *(volatile uint32_t *)(uintptr_t)addr = (uint32_t)val;
            LOG("[re] poked %#llx = %#llx at frame %llu\n", (unsigned long long)addr, (unsigned long long)val,
                (unsigned long long)renderer_present_count());
            addr = 0;
        }
    }
    {   // (RE) SKYRECOMP_RE_DUMP=<hex address>,<hex length>,<file> - appended every 10 frames: frame, then the bytes
        static FILE *f;
        static uint64_t addr, len;
        static int init;
        static uint64_t last = ~0ull;
        if (!init) {
            init = 1;
            const char *e = getenv("SKYRECOMP_RE_DUMP");
            char path[512];
            if (e && sscanf(e, "%llx,%llx,%511s", (unsigned long long *)&addr, (unsigned long long *)&len, path) == 3)
                f = fopen(path, "wb");
        }
        const uint64_t fr = renderer_present_count();
        if (f && fr != last && fr % 10 == 0) {
            last = fr;
            fwrite(&fr, 8, 1, f);
            fwrite((const void *)(uintptr_t)addr, len, 1, f);
            fflush(f);
        }
    }
    static long at = -2;
    if (at == -2) at = getenv("SKYRECOMP_RE_ACTORS") ? atol(getenv("SKYRECOMP_RE_ACTORS")) : -1;
    if (at >= 0 && (long)renderer_present_count() >= at) {
        static int said;
        if (!said++) LOG("[re] at frame %llu: %u objects saved\n", (unsigned long long)renderer_present_count(), g_nsnap);
    }
    if (at < 0 || (long)renderer_present_count() < at || !g_nsnap) return;
    at = -1;
    LOG("[re] room controller *(0x7002bfece8) = %#llx\n", (unsigned long long)*(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x2bfece8ull));
    {   // (every object at the door's exact spot: a vtable in main at +0, (790.6, *, -2183) at +0x144 or +0x148)
        for (uint64_t a = (MAIN_BASE + 0x5000000ull); a < (MAIN_BASE + 0x8000000ull); a += 8) {
            const uint64_t vt = *(const uint64_t *)(uintptr_t)a;
            if (vt < (MAIN_BASE + 0x1000000ull) || vt >= (MAIN_BASE + 0x2000000ull)) continue;
            for (int o = 0x144; o <= 0x148; o += 4) {
                const float *p = (const float *)(uintptr_t)(a + o);
                if (fabsf(p[0] - 790.6f) < 0.5f && fabsf(p[2] + 2183.0f) < 0.5f)
                    LOG("[re] at the door: %#llx vtable %#llx (position at +%#x) angles +0x130: %04x %04x %04x\n",
                        (unsigned long long)a, (unsigned long long)vt, o, ((const uint16_t *)(uintptr_t)(a + 0x130))[0],
                        ((const uint16_t *)(uintptr_t)(a + 0x130))[1], ((const uint16_t *)(uintptr_t)(a + 0x130))[2]);
            }
        }
    }
    {   // (the door object: 16-bit values from +0x130 - its angles, 22.5 degrees = 0x1000)
        const uint16_t *h = (const uint16_t *)(uintptr_t)((MAIN_BASE + 0x6229168ull) + 0x130);
        char line[512];
        int len = snprintf(line, sizeof line, "[re] door +0x130..:");
        for (int k = 0; k < 48; k++) len += snprintf(line + len, sizeof line - len, " %04x", h[k]);
        LOG("%s\n", line);
    }
    {   // (the door object - type 0x7001600da0 - before A: its position and room bytes)
        const uint8_t *d = (const uint8_t *)(uintptr_t)(MAIN_BASE + 0x6229168ull);
        LOG("[re] door 0x7006229168 vtable %#llx pos %.1f %.1f %.1f bytes +0x7b8..: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            (unsigned long long)*(const uint64_t *)d, *(const float *)(d + 0x144), *(const float *)(d + 0x148),
            *(const float *)(d + 0x14c), d[0x7b8], d[0x7b9], d[0x7ba], d[0x7bb], d[0x7bc], d[0x7bd], d[0x7be], d[0x7bf]);
    }
    {   // (the door object that activated the hallway: where it keeps its position - floats near (790.6, *, -2183))
        const float *f = (const float *)(uintptr_t)(MAIN_BASE + 0x622a908ull);
        for (int k = 0; k < 0x900 / 4; k++)
            if (fabsf(f[k] - 790.6f) < 1.0f || fabsf(f[k] + 2183.0f) < 1.0f)
                LOG("[re] door object +%#x = %.2f\n", k * 4, f[k]);
        LOG("[re] door object +0x7bc..: %02x %02x %02x %02x\n", ((const uint8_t *)f)[0x7bb], ((const uint8_t *)f)[0x7bc],
            ((const uint8_t *)f)[0x7bd], ((const uint8_t *)f)[0x7be]);
    }
    {   // (the hallway's room object: its create phases - f_7000f0f19c runs table[index] until done)
        const uint64_t room = (MAIN_BASE + 0x622b0e8ull), ph = *(const uint64_t *)(uintptr_t)(room + 0x98);
        LOG("[re] room %#llx vtable %#llx phase record %#llx\n", (unsigned long long)room,
            (unsigned long long)*(const uint64_t *)(uintptr_t)room, (unsigned long long)ph);
        if (ph >= (MAIN_BASE + 0x0ull) && ph < 0x7800000000ull) {
            const uint64_t table = *(const uint64_t *)(uintptr_t)(ph + 0x10);
            const int idx = *(const int16_t *)(uintptr_t)(ph + 0xa);
            LOG("[re] phase state %u index %d table %#llx\n", *(const uint16_t *)(uintptr_t)(ph + 8), idx, (unsigned long long)table);
            for (int k = 0; table >= (MAIN_BASE + 0x0ull) && table < 0x7800000000ull && k < 12; k++) {
                const uint64_t f = *(const uint64_t *)(uintptr_t)(table + k * 8);
                LOG("[re]   phase %d: %#llx%s\n", k, (unsigned long long)f, k == idx ? "  <- current" : "");
                if (!f) break;
            }
        }
    }
    const uint64_t r = scene_root();  // (the fullest list of the last frame: scene_snapshot)
    const uint64_t *list = g_snap;
    const uint32_t n = g_nsnap;
    LOG("[re] scene root %#llx: %u children\n", (unsigned long long)r, n);
    {   // (RE) SKYRECOMP_RE_VTABLE=<hex>: every object in the heap with that vtable at +0 (that frame)
        const char *e = getenv("SKYRECOMP_RE_VTABLE");
        const uint64_t vt = e ? strtoull(e, NULL, 16) : 0;
        int found = 0;
        for (uint64_t a = (MAIN_BASE + 0x5000000ull); vt && a < (MAIN_BASE + 0x8000000ull); a += 0x10)
            if (*(const uint64_t *)(uintptr_t)a == vt) {
                bool in_root = false;
                for (uint32_t k = 0; k < g_nsnap && !in_root; k++) in_root = g_snap[k] == a;
                LOG("[re] vt-object %#llx%s\n", (unsigned long long)a, in_root ? " (in the scene)" : "");
                found++;
            }
        if (vt) LOG("[re] %d objects with vtable %#llx\n", found, (unsigned long long)vt);
    }
    {   // objects positioned near Link: the position triplet at +0x148 (as his actor) within 150 units of his,
        // their vtable (in main) at +0
        const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);
        const float *lp = (const float *)(uintptr_t)(link + 0x148);
        LOG("[re] Link actor %#llx at %.1f %.1f %.1f\n", (unsigned long long)link, lp[0], lp[1], lp[2]);
        for (uint64_t a = (MAIN_BASE + 0x5000000ull); a < (MAIN_BASE + 0x8000000ull); a += 0x8) {
            const float *p = (const float *)(uintptr_t)(a + 0x148);
            if (!(fabsf(p[0] - lp[0]) < 150 && fabsf(p[2] - lp[2]) < 150 && fabsf(p[1] - lp[1]) < 400)) continue;
            const uint64_t vt = *(const uint64_t *)(uintptr_t)a;
            if (vt < MAIN_BASE || vt >= MAIN_BASE + 0x3000000 || a == link) continue;
            LOG("[re] near %#llx vt main+%#llx pos %.1f %.1f %.1f | +0x158..: %08x %08x %08x %08x\n", (unsigned long long)a,
                (unsigned long long)(vt - MAIN_BASE), p[0], p[1], p[2], ((const uint32_t *)p)[4], ((const uint32_t *)p)[5],
                ((const uint32_t *)p)[6], ((const uint32_t *)p)[7]);
        }
    }
    for (uint32_t i = 0; list && i < n && i < 2048; i++) {
        const uint64_t a = list[i];
        if (a < (MAIN_BASE + 0x0ull) || a >= 0x7800000000ull) continue;
        const float *f = (const float *)(uintptr_t)(a + 0x148);
        LOG("[re] %4u %#llx vt %#llx pos %.1f %.1f %.1f\n", i, (unsigned long long)a,
            (unsigned long long)*(const uint64_t *)(uintptr_t)a, f[0], f[1], f[2]);
    }
}

// (RE) SKYRECOMP_RE_ROOM=<hex manager>,<room>: once Link stands at a door, f_7000f0f258(manager, room) is called once
// (game thread) - what the door event's room switch calls (f_7000f0faac, on the door event)
static void re_room(Cpu *c) {
    static int init;
    static uint64_t mgr;
    static unsigned room;
    static bool on;
    if (!init) {
        init = 1;
        const char *e = getenv("SKYRECOMP_RE_ROOM");
        on = e && sscanf(e, "%llx,%u", (unsigned long long *)&mgr, &room) == 2;
    }
    if (!on) return;
    const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);
    if (link < (MAIN_BASE + 0x0ull) || link >= 0x7800000000ull || *(const uint32_t *)(uintptr_t)(link + 0x118) != 0x6a) return;
    static int wait;
    if (++wait < 120) return;  // (standing at the door a while)
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    const uint64_t sp = c->sp;
    // what an object's update calls as the hallway is activated (f_7000789748 at 0x7000789d50): f_7000e0854c(
    // *(0x7002bf9d90), mgr (its +0x7bd), room (its +0x7bc)) - measured (0, 1)
    c->x[0] = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x2bf9d90ull);
    c->x[1] = (uint32_t)mgr;
    c->x[2] = room;
    LOG("[re] calling f_7000e0854c(%#llx, %u, %u) at frame %llu\n", (unsigned long long)c->x[0], (unsigned)mgr, room,
        (unsigned long long)renderer_present_count());
    guest_call(c, (MAIN_BASE + 0xe0854cull));
    LOG("[re] returned %#llx\n", (unsigned long long)c->x[0]);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
    on = false;
}

static void pre_execute(Cpu *c) {
    extern void door_room_tick(Cpu *c);
    door_room_tick(c);  // (physical doors: the room beyond, activated - hle/door.c)
    extern void door_sound_tick(Cpu *c);
    door_sound_tick(c);  // (and its sounds)
    extern void door_lock_tick(Cpu *c);
    door_lock_tick(c);  // (and whether it is locked - the game's own test)
    extern void door_step_tick(void);
    door_step_tick();
    extern void door_yaw_tick(void);
    door_yaw_tick();
    extern void door_pose_tick(void);
    door_pose_tick();
    extern void door_lift_tick(void);
    door_lift_tick();
    extern void door_link_tick(void);
    door_link_tick();
    re_actors();
    re_room(c);
    if (world_in_menus()) {
        if (!*(volatile uint32_t *)(uintptr_t)EXEC_MASK) scene_snapshot(), shadow_sample();
        else scene_replay(c), shadow_restore();
    } else {
        static int trace = -1;
        if (trace < 0) trace = getenv("SKYRECOMP_VR_WORLD_TRACE") != NULL;
        if (trace) {
            {   // (the masks, as they change: execute, draw, the draw's pending one)
                static uint32_t was[3] = {~0u, ~0u, ~0u};
                const uint32_t now[3] = {*(volatile uint32_t *)(uintptr_t)EXEC_MASK, *(volatile uint32_t *)(uintptr_t)DRAW_MASK,
                                         *(volatile uint32_t *)(uintptr_t)(MAIN_BASE + 0x1827378)};
                if (memcmp(was, now, sizeof was)) {
                    LOG("[vr-world] trace: frame %llu masks: execute %#x draw %#x pending draw %#x\n",
                        (unsigned long long)renderer_present_count(), now[0], now[1], now[2]);
                    memcpy(was, now, sizeof was);
                }
            }
            if (!*(volatile uint32_t *)(uintptr_t)EXEC_MASK) scene_snapshot(), scene_trace_roots();
            else scene_trace();
        }
    }
    // (test) SKYRECOMP_VR_WORLD_SHADOWS bit 2: the shadow manager (vtable main+0x1678d18; its execute main+0xe1d388)
    // also keeps running behind a menu - its preExecute run with the execute mask cleared. Not needed for the models'
    // shadows (shadow_restore, above), and off by default.
    volatile uint32_t *exec_mask = (volatile uint32_t *)(uintptr_t)EXEC_MASK;
    const uint64_t node = c->x[0];
    if (*exec_mask && guest_ptr(node) && *(const uint64_t *)(uintptr_t)node == MAIN_BASE + 0x1678d18ull && world_in_menus()) {
        static int on = -1;
        if (on < 0) on = getenv("SKYRECOMP_VR_WORLD_SHADOWS") ? atoi(getenv("SKYRECOMP_VR_WORLD_SHADOWS")) : 1;
        if (on & 2) {
            const uint32_t saved = *exec_mask;
            *exec_mask = 0;
            g_orig_pre_execute(c);
            *exec_mask = saved;
            return;
        }
    }
    g_orig_pre_execute(c);
}

// (after each object's execute: the shadow casters' list, kept or put back - as before each one's)
static void post_execute(Cpu *c) {
    g_orig_post_execute(c);
    if (!world_in_menus()) return;
    if (!*(volatile uint32_t *)(uintptr_t)EXEC_MASK) shadow_sample();
    else shadow_restore();
}

// preDraw implementations reading the draw mask (watchpoint), called through vtables (so through the dispatcher,
// where overrides apply): the base node's, the world (stage) node's, and the actor base's.
// And every preDraw that reaches the base node's by a direct call (a call the dispatcher never sees: the base's own
// override does nothing there) - the actors' (main+0xb11124, in 398 vtables) and the 30 that call it in turn (found
// 2026-11-10: the callers of main+0xec620c in the code, followed up to the functions vtables point at). The Gear menu's
// draw mask is 1 (measured; the map's is 0x10): an actor whose +0xb8 lacks that bit is refused there. Listed, but
// hooked only with SKYRECOMP_VR_WORLD_GATES=33 (vr_world_init).
static const uint64_t k_pre_draw[] = {
    (MAIN_BASE + 0xec620cull), (MAIN_BASE + 0xaaf4acull), (MAIN_BASE + 0xb1756cull), (MAIN_BASE + 0xb11124ull), (MAIN_BASE + 0xe84a04ull), (MAIN_BASE + 0x49bf78ull), (MAIN_BASE + 0x4aa2d4ull),
    (MAIN_BASE + 0x4fb3e8ull), (MAIN_BASE + 0x548f94ull), (MAIN_BASE + 0x55bd48ull), (MAIN_BASE + 0x55da78ull), (MAIN_BASE + 0x59c0c0ull), (MAIN_BASE + 0x785510ull), (MAIN_BASE + 0x7e73a8ull),
    (MAIN_BASE + 0x7f8e28ull), (MAIN_BASE + 0x8ba9ecull), (MAIN_BASE + 0xa05ba4ull), (MAIN_BASE + 0xaf9584ull), (MAIN_BASE + 0xddc85cull), (MAIN_BASE + 0x162110ull), (MAIN_BASE + 0x1941c8ull),
    (MAIN_BASE + 0x1bed6cull), (MAIN_BASE + 0x31cd74ull), (MAIN_BASE + 0x373b28ull), (MAIN_BASE + 0x3f52e0ull), (MAIN_BASE + 0x48718cull), (MAIN_BASE + 0x561708ull), (MAIN_BASE + 0x56fd64ull),
    (MAIN_BASE + 0x5b9768ull), (MAIN_BASE + 0x602558ull), (MAIN_BASE + 0x6bf570ull), (MAIN_BASE + 0xaf7ddcull), (MAIN_BASE + 0x6191f8ull)};
enum { N_PRE_DRAW = sizeof k_pre_draw / sizeof k_pre_draw[0] };
static GuestFn g_orig[N_PRE_DRAW];
static void pre_draw(Cpu *c, int i) {
    volatile uint32_t *mask = (volatile uint32_t *)(uintptr_t)DRAW_MASK;
    const uint32_t saved = *mask;
    const bool clear = saved && world_in_menus();
    if (clear) *mask = 0;
    if (i == 1) shadow_trace("the stage's preDraw");
    g_orig[i](c);
    if (clear) *mask = saved;
}
#define PD(i) static void pre_draw##i(Cpu *c) { pre_draw(c, i); }
PD(0) PD(1) PD(2) PD(3) PD(4) PD(5) PD(6) PD(7) PD(8) PD(9) PD(10) PD(11) PD(12) PD(13) PD(14) PD(15) PD(16)
PD(17) PD(18) PD(19) PD(20) PD(21) PD(22) PD(23) PD(24) PD(25) PD(26) PD(27) PD(28) PD(29) PD(30) PD(31) PD(32)
#undef PD
_Static_assert(N_PRE_DRAW == 33, "one wrapper per preDraw");

// The frame's own routine (main+0xb9a3e4, through a vtable): at its end it calls main+0xe14790 (a direct call), which
// does nothing while the draw mask has a menu's bits (& 0x19: the Gear menu's 1, the map's 0x10) - and it reads the
// mask itself at three places (the frame before's: it copies the pending one in as it ends). With the menu's bits
// taken out for the call: (test) SKYRECOMP_VR_WORLD_FRAME=1 - are the shadows, gone behind the Gear menu, back?
// (RE) The shadow pass's casters: the list the frame's render (main+0xe23d70, at +0x21a8) walks - the object at
// *(main+0x2bfe6a8), its first at +0x80, each one's next at [node + u16(object + 0x92) + 8]. How many, -1: no object.
static int shadow_casters(void) {
    const uint64_t o = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x2bfe6a8ull);
    if (o < MAIN_BASE || o >= 0x7800000000ull) return -1;
    const uint16_t link = *(const uint16_t *)(uintptr_t)(o + 0x92);
    int n = 0;
    for (uint64_t p = *(const uint64_t *)(uintptr_t)(o + 0x80); p >= MAIN_BASE && p < 0x7800000000ull && n < 4096;
         p = *(const uint64_t *)(uintptr_t)(p + link + 8))
        n++;
    return n;
}
// (RE) SKYRECOMP_VR_SHADOW_TRACE=1: the list's length at points of the frame, every 90th frame
static void shadow_trace(const char *where) {
    static int on = -1;
    if (on < 0) on = getenv("SKYRECOMP_VR_SHADOW_TRACE") != NULL;
    if (!on || renderer_present_count() % 90) return;
    static uint64_t frame;
    static int n;
    if (frame != renderer_present_count()) frame = renderer_present_count(), n = 0;
    if (n++ >= 12) return;
    LOG("[vr-world] shadow trace: frame %llu %s: %d casters (execute mask %#x, draw mask %#x)\n", (unsigned long long)frame, where,
        shadow_casters(), *(volatile uint32_t *)(uintptr_t)EXEC_MASK, *(volatile uint32_t *)(uintptr_t)DRAW_MASK);
}

// (RE) the shadow manager's execute (main+0xe1d388), traced around: its two lists' counts (the object at
// *(main+0x2c24548): u16 at +0x5a08 and +0x5a0a)
#define SHADOW_EXECUTE (MAIN_BASE + 0xe1d388ull)
static GuestFn g_orig_shadow_execute;
static void shadow_execute(Cpu *c) {
    static int on = -1;
    if (on < 0) on = getenv("SKYRECOMP_VR_SHADOW_TRACE") != NULL;
    const uint64_t m = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x2c24548ull);
    const bool log = on && renderer_present_count() % 90 == 0 && m >= MAIN_BASE && m < 0x7800000000ull;
    const int before = shadow_casters();
    const uint16_t a0 = log ? *(const uint16_t *)(uintptr_t)(m + 0x5a08) : 0, b0 = log ? *(const uint16_t *)(uintptr_t)(m + 0x5a0a) : 0;
    g_orig_shadow_execute(c);
    if (log)
        LOG("[vr-world] shadow trace: frame %llu the shadow manager's execute: casters %d -> %d, lists %u %u -> %u %u (execute mask %#x)\n",
            (unsigned long long)renderer_present_count(), before, shadow_casters(), a0, b0, *(const uint16_t *)(uintptr_t)(m + 0x5a08),
            *(const uint16_t *)(uintptr_t)(m + 0x5a0a), *(volatile uint32_t *)(uintptr_t)EXEC_MASK);
}

#define FRAME_ROUTINE (MAIN_BASE + 0xb9a3e4ull)
static GuestFn g_orig_frame_routine;
static void frame_routine(Cpu *c) {
    shadow_trace("frame routine");
    static int on = -1;
    if (on < 0) on = getenv("SKYRECOMP_VR_WORLD_FRAME") ? atoi(getenv("SKYRECOMP_VR_WORLD_FRAME")) : 0;
    volatile uint32_t *mask = (volatile uint32_t *)(uintptr_t)DRAW_MASK;
    if (on && (*mask & 0x19) && world_in_menus()) {
        static int said;
        if (!said++) LOG("[vr-world] frame routine: draw mask %#x -> %#x for the call\n", *mask, *mask & ~0x19u);
        *mask &= ~0x19u;  // (the routine writes the pending mask over it before it returns)
    }
    static int trace = -1;
    if (trace < 0) trace = getenv("SKYRECOMP_VR_SHADOW_TRACE") != NULL;
    const uint64_t m = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x2c24548ull);
    const bool log = trace && renderer_present_count() % 90 == 0 && m >= MAIN_BASE && m < 0x7800000000ull;
    const int before = log ? shadow_casters() : 0;
    const uint16_t a0 = log ? *(const uint16_t *)(uintptr_t)(m + 0x5a08) : 0, b0 = log ? *(const uint16_t *)(uintptr_t)(m + 0x5a0a) : 0;
    const uint32_t mask0 = *mask;
    g_orig_frame_routine(c);
    if (log)
        LOG("[vr-world] shadow trace: frame %llu the frame routine (draw mask %#x): casters %d -> %d, lists %u %u -> %u %u, state %u\n",
            (unsigned long long)renderer_present_count(), mask0, before, shadow_casters(), a0, b0, *(const uint16_t *)(uintptr_t)(m + 0x5a08),
            *(const uint16_t *)(uintptr_t)(m + 0x5a0a), *(const uint32_t *)(uintptr_t)(MAIN_BASE + 0x1587bc0ull));
    return;
}

// The map's "enter full screen" state callback (called once through a state table when the map opens): pending
// draw mask |= 0x10, and [[main+0x183c9a0] + 0x2188c] = 0.
#define MAP_DRAW_OFF (MAIN_BASE + 0xbce320ull)
static GuestFn g_orig_map_draw_off;
static void map_draw_off(Cpu *c) {
    if (world_in_menus()) return;
    g_orig_map_draw_off(c);
}

// (main.c, after dispatch_init and before the game runs: the dispatcher's caches must not hold these yet)
void vr_world_init(void) {
    int n = 0;
    if ((g_orig_pre_execute = dispatch_lookup(PRE_EXECUTE))) hle_override(PRE_EXECUTE, pre_execute, "vr_world_pre_execute"), n++;
    if ((g_orig_post_execute = dispatch_lookup(POST_EXECUTE))) hle_override(POST_EXECUTE, post_execute, "vr_world_post_execute"), n++;
    if ((g_orig_map_draw_off = dispatch_lookup(MAP_DRAW_OFF))) hle_override(MAP_DRAW_OFF, map_draw_off, "vr_world_map_draw_off"), n++;
    if ((g_orig_frame_routine = dispatch_lookup(FRAME_ROUTINE))) hle_override(FRAME_ROUTINE, frame_routine, "vr_world_frame_routine"), n++;
    if (getenv("SKYRECOMP_VR_SHADOW_TRACE") && (g_orig_shadow_execute = dispatch_lookup(SHADOW_EXECUTE)))
        hle_override(SHADOW_EXECUTE, shadow_execute, "vr_world_shadow_execute"), n++;
    static const HostFn wrap[N_PRE_DRAW] = {
        pre_draw0,  pre_draw1,  pre_draw2,  pre_draw3,  pre_draw4,  pre_draw5,  pre_draw6,  pre_draw7,  pre_draw8,
        pre_draw9,  pre_draw10, pre_draw11, pre_draw12, pre_draw13, pre_draw14, pre_draw15, pre_draw16, pre_draw17,
        pre_draw18, pre_draw19, pre_draw20, pre_draw21, pre_draw22, pre_draw23, pre_draw24, pre_draw25, pre_draw26,
        pre_draw27, pre_draw28, pre_draw29, pre_draw30, pre_draw31, pre_draw32};
    // Only the first three by default. (test) SKYRECOMP_VR_WORLD_GATES=33: all of them - in the Sparring Hall the Gear
    // menu (VR, 2026-11-10) showed the same actors either way, so the others stay off until a scene shows they matter
    const size_t gates = getenv("SKYRECOMP_VR_WORLD_GATES") ? (size_t)atoi(getenv("SKYRECOMP_VR_WORLD_GATES")) : 3;
    for (size_t i = 0; i < N_PRE_DRAW && i < gates; i++) {
        g_orig[i] = dispatch_lookup(k_pre_draw[i]);
        if (g_orig[i]) hle_override(k_pre_draw[i], wrap[i], "vr_world_pre_draw"), n++;
    }
    LOG("[vr-world] %d hooks (the world behind menus: with VR and the HUD panel)\n", n);
}
