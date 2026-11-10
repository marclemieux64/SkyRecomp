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
static uint64_t g_snap_frame = ~0ull, g_replay_frame = ~0ull;

static uint64_t scene_root(void) {
    const uint64_t r = *(const uint64_t *)(uintptr_t)SCENE_ROOT_PTR;
    return r >= (MAIN_BASE + 0x0ull) && r < 0x7800000000ull && *(const uint64_t *)(uintptr_t)r == SCENE_ROOT_VTABLE ? r : 0;
}

// (execute phase of an unpaused frame) The list fills during the phase and is emptied before the draw: the fullest
// list of the frame is kept.
static void scene_snapshot(void) {
    const uint64_t f = renderer_present_count(), r = scene_root();
    if (!r) return;
    if (f != g_snap_frame) g_snap_frame = f, g_nsnap = 0;
    const uint64_t *list = *(uint64_t *const *)(uintptr_t)(r + 0xf0);
    const uint32_t n = *(const uint32_t *)(uintptr_t)(r + 0xfc);
    if (!list || n > MAX_CHILDREN || n <= g_nsnap) return;
    memcpy(g_snap, list, n * sizeof *g_snap);
    for (uint32_t i = 0; i < n; i++) g_snap_vt[i] = g_snap[i] ? *(const uint64_t *)(uintptr_t)g_snap[i] : 0;
    g_nsnap = n;
}

// (execute phase of a paused frame, once) The last unpaused frame's children go back in.
static void scene_replay(Cpu *c) {
    const uint64_t f = renderer_present_count(), r = scene_root();
    if (f == g_replay_frame || !r || !g_nsnap) return;
    g_replay_frame = f;
    const uint64_t insert = *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)r + 0x60);  // (main+0xfecfe4)
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    int added = 0;
    for (uint32_t i = 0; i < g_nsnap; i++) {
        const uint64_t *list = *(uint64_t *const *)(uintptr_t)(r + 0xf0);
        const uint32_t n = *(const uint32_t *)(uintptr_t)(r + 0xfc);
        bool present = false;
        for (uint32_t k = 0; k < n && !present; k++) present = list[k] == g_snap[i];
        if (present) continue;
        // (an object deleted since - an arrow that landed while the item wheel was open... - is freed memory:
        // handing it back made the game abort; its vtable no longer matches)
        if (!g_snap[i] || *(const uint64_t *)(uintptr_t)g_snap[i] != g_snap_vt[i]) continue;
        c->x[0] = r;  // insert(root, index, child) -> bool: refuses a child that has a parent (+8) or a full list
        c->x[1] = n;
        c->x[2] = g_snap[i];
        guest_call(c, insert);
        added += (uint32_t)c->x[0] & 1;
    }
    memcpy(c->x, saved, sizeof saved);
    static int logged;
    if (!logged++) LOG("[vr-world] menu: %d frozen world objects re-entered into the scene each frame\n", added);
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

#define PRE_EXECUTE (MAIN_BASE + 0xb175c0ull)
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
        if (!*(volatile uint32_t *)(uintptr_t)EXEC_MASK) scene_snapshot();
        else scene_replay(c);
    }
    g_orig_pre_execute(c);
}

// preDraw implementations reading the draw mask (watchpoint), called through vtables (so through the dispatcher,
// where overrides apply): the base node's, the world (stage) node's, and the actor base's.
static const uint64_t k_pre_draw[] = {(MAIN_BASE + 0xec620cull), (MAIN_BASE + 0xaaf4acull), (MAIN_BASE + 0xb1756cull)};
static GuestFn g_orig[sizeof k_pre_draw / sizeof k_pre_draw[0]];
static void pre_draw(Cpu *c, int i) {
    volatile uint32_t *mask = (volatile uint32_t *)(uintptr_t)DRAW_MASK;
    const uint32_t saved = *mask;
    const bool clear = saved && world_in_menus();
    if (clear) *mask = 0;
    g_orig[i](c);
    if (clear) *mask = saved;
}
static void pre_draw0(Cpu *c) { pre_draw(c, 0); }
static void pre_draw1(Cpu *c) { pre_draw(c, 1); }
static void pre_draw2(Cpu *c) { pre_draw(c, 2); }

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
    if ((g_orig_map_draw_off = dispatch_lookup(MAP_DRAW_OFF))) hle_override(MAP_DRAW_OFF, map_draw_off, "vr_world_map_draw_off"), n++;
    static const HostFn wrap[] = {pre_draw0, pre_draw1, pre_draw2};
    for (size_t i = 0; i < sizeof k_pre_draw / sizeof k_pre_draw[0]; i++) {
        g_orig[i] = dispatch_lookup(k_pre_draw[i]);
        if (g_orig[i]) hle_override(k_pre_draw[i], wrap[i], "vr_world_pre_draw"), n++;
    }
    LOG("[vr-world] %d hooks (the world behind menus: with VR and the HUD panel)\n", n);
}
