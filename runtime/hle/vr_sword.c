// VR: the sword's hits follow the blade in your hand (VR Options > Sword Hits Follow Your Hand; [VR] sword_hitbox).
//
// The game keeps Link's sword as a swept line inside his actor, at +0x66e4 (his actor: the pointer at main+0x18299d0):
// four world positions - the blade's tip (80 units along the sword model's +X from its hilt), a point near its base
// (12 units along), and both of last frame's. A slash's hits are the three attack colliders the update builds from it
// (SWORD_COLLIDER below). Link's update (main+0xab7cf4) writes both every frame; it is called through his vtable, so it
// can be wrapped (hle_override). Right after it runs, while VR runs in 1st person with the right controller tracked and
// the sword drawn, the line and the colliders become the blade held by the right controller: along the grip's forward
// axis (-z, the way a held sword points), this update's and the last one's. The game still decides when Link swings
// (its motion controls); where the blade is when it does - what it hits - is now your hand's, Z-targeting or not.
//
// Swings of their own (VR Options > Swings Hit on Their Own; [VR] sword_free_swings): the colliders only hit while
// Link's update hands them to the collision manager (cCcS, the pointer at main+0x2c24548) for that frame - Set
// (main+0xfd5f7c: into its attack list, if the attack bit, bit 0 of the collider's first word, is set) and its priority
// list (main+0xfd3ddc on manager +0x6068) - which it does only during its own slash. So when the blade in your hand moves
// fast and the game isn't slashing, the hook does it itself, every update until the blade slows down: the swing starts
// like the game's own (main+0xa6d158: the attack's direction, damage and radius, its hits on targets reset), and each
// update registers the three colliders. The direction is the blade's own path across your view, one of the game's eight
// (a bit in +0x0c of each collider and Link +0x41e, 0x1 down, then counter-clockwise: 0x2 down-right, 0x4 right ...
// 0x80 down-left; with an angle in +0x0e, 0 = up, 0x4000 = left), so guarding enemies see the swing you made - in the
// game's own slashes too. Found headset-free: a slash's collider dumped update by update (the direction and angle
// written at its start, the radius 20 -> 45, the swept vector), the registration read off Link's slash code, and a
// collider armed by hand outside a slash processed by the manager only once it is Set (its bounds follow it).
//
// Drawn or sheathed comes from the game's own line: sheathed, its base point sits 19-20 units from Link's eye point
// (main+0x1829a30, runtime/hle/player.c) on his back; drawn, further out in his hand. (Measured in the original
// implementation: the hilt on his back is ~25 units from his head, held 40 or more.)
//
// Found with a memory search for positions that keep their place in the drawn hilt's own frame while the sword swings,
// and the update by watching who writes them (marclemieux64/SkyRecomp PR; first done in a separate VR build:
// "sword hit line"). Diagnostics: SKYRECOMP_SWORD_LOG=1 logs what is written every 120 frames.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_timer.h>

#include "../hle.h"
#include "../vr/vr_rig.h"
#include "../vr/vr_settings.h"

float vr_rig_units_per_metre(void);

#define PLAYER_UPDATE (MAIN_BASE + 0xab7cf4)
#define PLAYER_ACTOR_REF (MAIN_BASE + 0x18299d0)
#define PLAYER_EYE (MAIN_BASE + 0x1829a30)
#define SWORD_LINE 0x66e4
#define SWORD_TRAIL_ON 0x3bc  // (in the line's structure: u32, the slash trail drawn while set)
#define BASE_AT 12.0f   // game units along the blade from the grip: the game's own base point
#define TIP_AT 80.0f    // ... and its tip
#define DRAWN_AT 30.0f  // the line's base further than this from Link's eye point: drawn (sheathed: 19-20)
#define SHEATHED_AT 25.0f
#define DRAWN_MAX 200.0f
// Swings of their own only once play has settled: Link's own eye point (not a cached one), the 1st-person view placed,
// the game not paused, and the sword in your hand this many updates. (Allowed from the start, one fired while the game
// was still spawning him - a slash started on him then: the hands were learned the wrong way round, the HUD yellow.)
#define FREE_READY_UPDATES 60
#define SWING_SPEED 2.0f  // the controller's blade tip faster than this (m/s): its hits are on
// The sword's three attack colliders (capsules, 0x1f0 apart from Link +0x12c8): what a slash actually hits with. Each
// holds its capsule's two ends in the world at +0x178 and +0x184 (radius ~45): the first from the blade's base to a point
// REACH out along it, the second from last update's end point to this one's, the third from the base to last update's
// end - together the arc the blade swept. The game builds them from its own sword during Link's update and takes their
// bounds from them only afterwards (the bounds read one update behind right after it), so ends written after the
// update are the ones its hits use. Measured headset-free: moving them 2 m up keeps a bush in front of Link whole
// through three slashes; moving only the line at +0x66e4 doesn't (it has already been read by then).
#define SWORD_COLLIDER 0x12c8
#define SWORD_COLLIDER_STRIDE 0x1f0
#define CAPSULE_START 0x178
#define CAPSULE_END 0x184
#define REACH_DEFAULT 128.0f  // the game's own: its first capsule ~128 units from the base along the blade
// Swings of their own (see the top). The collider objects start 0x40 before SWORD_COLLIDER (their vtables).
#define SWORD_OBJECT (SWORD_COLLIDER - 0x40)
#define CC_MANAGER_REF (MAIN_BASE + 0x2c24548)
#define CC_SET (MAIN_BASE + 0xfd5f7c)       // (manager, collider)
#define CC_PRIORITY (MAIN_BASE + 0xfd3ddc)  // (manager + CC_PRIORITY_AT, collider, 1)
#define CC_PRIORITY_AT 0x6068
#define CC_AT_COUNT 0x5a08                  // u16; the attack list's pointers from +0x8 (at most 0x200)
#define SWORD_ATTACK_START (MAIN_BASE + 0xa6d158)  // (Link, 0, direction; s0 60, s1 45: the game's slash)
#define LINK_ATTACK_DIR 0x41e               // u16, the attack's direction bit (also each collider +0x0c; angle +0x0e)
// A swing of its own starts when the blade tip moves faster than VR Options > Swing Speed to Hit ([VR]
// sword_swing_speed, m/s), measured from the controller's own motion in the room (vr_input.c: walking and turning
// don't count), and ends after FREE_END_UPDATES updates below FREE_END of that.
#define FREE_END 0.6f
#define FREE_END_UPDATES 3
#define DIR_HISTORY 3     // updates of the tip's path its direction is read from

extern bool vr_running(void);
extern uint64_t renderer_present_count(void);

static GuestFn g_player_update;
static _Atomic bool g_spatial;  // (the sword out in 1st person: the Joy-Con's turn is the blade's - JoyConTranslator)
bool vr_sword_spatial(void) { return atomic_load(&g_spatial); }
static _Atomic bool g_out;  // the sword drawn in 1st person, in your hand (vr_rig.c: the view doesn't follow his slashes)
bool vr_sword_out(void) { return atomic_load(&g_out); }
static float g_shift[3];
static bool g_shift_on;

static bool sword_drawn(const float *line, const float *e) {
    static bool drawn;
    static int count;
    const float d[3] = {line[3] - e[0], line[4] - e[1], line[5] - e[2]};
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    // (a margin, and some patience: a swing past the head doesn't sheathe it. Farther than DRAWN_MAX: not a sword in
    // his hand but a line not set up yet - spawning, loading)
    const bool want = dist < DRAWN_MAX && (drawn ? !(dist < SHEATHED_AT) : dist > DRAWN_AT);
    if (want != drawn) {
        if (++count >= (drawn ? 10 : 3)) drawn = want, count = 0;
    } else count = 0;
    return drawn;
}

// The tip's path (world) across the view: the game's direction bit and angle; false while it barely crosses the view
// (a thrust, or too short to tell).
static bool swing_direction(const float d[3], uint16_t *bit, uint16_t *angle) {
    float view[12], t[4];
    if (!vr_rig_cull_view(view, t)) return false;
    float r = 0, u = 0, nr = 0, nu = 0;
    for (int i = 0; i < 3; i++) r += d[i] * view[i], u += d[i] * view[4 + i], nr += view[i] * view[i], nu += view[4 + i] * view[4 + i];
    if (nr <= 0 || nu <= 0) return false;
    r /= sqrtf(nr), u /= sqrtf(nu);
    const float across = sqrtf(r * r + u * u), len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (across < 0.5f * len || across < 4.0f) return false;
    const int k = ((int)lroundf(atan2f(-r, u) / (float)(M_PI / 4)) % 8 + 8) % 8;  // (0 up, counter-clockwise)
    *bit = (uint16_t)(1u << ((k + 4) & 7));
    *angle = (uint16_t)(k * 0x2000);
    return true;
}

static void set_direction(uint64_t pl, uint16_t bit, uint16_t angle) {
    *(uint16_t *)(uintptr_t)(pl + LINK_ATTACK_DIR) = bit;
    for (int k = 0; k < 3; k++) {
        uint8_t *col = (uint8_t *)(uintptr_t)(pl + SWORD_COLLIDER + SWORD_COLLIDER_STRIDE * k);
        *(uint16_t *)(col + 0x0c) = bit;
        *(uint16_t *)(col + 0x0e) = angle;
    }
}

// The game handed the colliders to the manager this update (its own slash).
static bool game_registered(uint64_t pl) {
    const uint64_t m = *(const uint64_t *)(uintptr_t)CC_MANAGER_REF;
    if (!m) return false;
    const unsigned n = *(const uint16_t *)(uintptr_t)(m + CC_AT_COUNT);
    for (unsigned i = 0; i < n && i < 0x200; i++) {
        const uint64_t p = *(const uint64_t *)(uintptr_t)(m + 8 + 8 * i);
        if (p >= pl + SWORD_OBJECT && p < pl + SWORD_OBJECT + 3 * SWORD_COLLIDER_STRIDE) return true;
    }
    return false;
}

static void guest_calls_start(Cpu *c, uint64_t saved[31], uint64_t *sp, uint64_t *pc) {
    memcpy(saved, c->x, 31 * sizeof(uint64_t)), *sp = c->sp, *pc = c->pc;
}
static void guest_calls_end(Cpu *c, const uint64_t saved[31], uint64_t sp, uint64_t pc) {
    memcpy(c->x, saved, 31 * sizeof(uint64_t)), c->sp = sp, c->pc = pc;
}

// The game's slash start, with the direction swung (resets the hits: one swing hits each target once).
static void free_swing_start(Cpu *c, uint64_t pl, uint16_t bit) {
    uint64_t saved[31], sp, pc;
    const __uint128_t v0 = c->v[0], v1 = c->v[1];
    guest_calls_start(c, saved, &sp, &pc);
    c->x[0] = pl, c->x[1] = 0, c->x[2] = bit;
    c->v[0] = 0x42700000u, c->v[1] = 0x42340000u;  // (60, 45: as the game passes them)
    guest_call(c, SWORD_ATTACK_START);
    c->v[0] = v0, c->v[1] = v1;
    guest_calls_end(c, saved, sp, pc);
}

static void free_swing_register(Cpu *c, uint64_t pl) {
    const uint64_t m = *(const uint64_t *)(uintptr_t)CC_MANAGER_REF;
    if (!m) return;
    uint64_t saved[31], sp, pc;
    guest_calls_start(c, saved, &sp, &pc);
    for (int k = 0; k < 3; k++) {
        const uint64_t obj = pl + SWORD_OBJECT + SWORD_COLLIDER_STRIDE * k;
        *(uint32_t *)(uintptr_t)(pl + SWORD_COLLIDER + SWORD_COLLIDER_STRIDE * k) |= 1u;  // (the attack bit)
        c->x[0] = m, c->x[1] = obj;
        guest_call(c, CC_SET);
        c->x[0] = m + CC_PRIORITY_AT, c->x[1] = obj, c->x[2] = 1;
        guest_call(c, CC_PRIORITY);
    }
    guest_calls_end(c, saved, sp, pc);
}

static void player_update_hook(Cpu *c) {
    const uint64_t pl = c->x[0];
    g_player_update(c);
    // (only in play, not the title screen's Link the update also serves: his eye point is valid then. Compared with
    // the actor main+0x18299d0 points to, the hook never acted: in play the update is called with another pointer,
    // and that one read 0 at the time)
    // (his eye point - read as the game has it; failing that, as the other actors' updates saw it this frame,
    // runtime/hle/vr_slingshot.c. With VR off this build's player pointer and eye point read 0)
    extern bool player_eye(float eye[3]), vr_player_eye_cached(float eye[3]);
    float eye[3];
    const bool live_eye = player_eye(eye);
    if (!live_eye && !vr_player_eye_cached(eye)) return;
    static int said;
    if (!said++) LOG("[sword] Link's update in play (called with %llx)\n", (unsigned long long)pl);
    float *line = (float *)(uintptr_t)(pl + SWORD_LINE);  // tip, base, last tip, last base
    float *cap[3];  // the three capsules' ends: start xyz, end xyz
    for (int k = 0; k < 3; k++) cap[k] = (float *)(uintptr_t)(pl + SWORD_COLLIDER + SWORD_COLLIDER_STRIDE * k + CAPSULE_START);
    if (g_shift_on) {  // (test: the game's own hits moved by that much - a slash then misses what's in front of Link)
        for (int p = 0; p < 4; p++) for (int i = 0; i < 3; i++) line[p * 3 + i] += g_shift[i];
        for (int k = 0; k < 3; k++) for (int i = 0; i < 6; i++) cap[k][i] += g_shift[i % 3];
        return;
    }
    static float last[6], last_end[3];
    static bool have_last;
    float grip[12];
    const bool on = g_vr_config.sword_hitbox && g_vr_config.view_mode == 1 && vr_running() && sword_drawn(line, eye) &&
                    vr_rig_controller_pose(1, grip);
    atomic_store(&g_spatial, on && g_vr_config.sword_swing_hits);
    atomic_store(&g_out, on);
    static int ready_updates;  // (FREE_READY_UPDATES)
    {
        extern bool vr_rig_settling(void), vr_game_paused(void);
        ready_updates = on && live_eye && !vr_rig_settling() && !vr_game_paused() ? ready_updates + 1 : 0;
    }
    if (!on) { have_last = false; return; }
    float blade[6];  // tip, base: along the grip's -z
    for (int i = 0; i < 3; i++) {
        blade[i] = grip[i * 4 + 3] - grip[i * 4 + 2] * TIP_AT;
        blade[3 + i] = grip[i * 4 + 3] - grip[i * 4 + 2] * BASE_AT;
    }
    if (!have_last) memcpy(last, blade, sizeof last);
    float game_tip[3];  // (the game's own tip, for the log: how far the blade in your hand is from it)
    memcpy(game_tip, line, sizeof game_tip);
    memcpy(line, blade, sizeof blade);
    memcpy(line + 6, last, sizeof last);
    // The slash's trail (the white swoosh) is drawn from this line's structure - a history of the blade's points
    // further in (+0x90, +0x144..+0x168, +0x228) - while its word at +0x3bc is set (1 during a slash). Cleared after
    // the update, no trail is drawn; the rest of the slash (its sounds, its hits) is untouched. Found headset-free,
    // screenshots mid-slash: moving the line 3 m changed nothing, collapsing the history thinned the trail to a streak,
    // and clearing the structure's last 12 bytes, then only +0x3bc, left none. SKYRECOMP_VR_SWORD_TRAIL=1 keeps it.
    static int trail = -1;
    if (trail < 0) trail = getenv("SKYRECOMP_VR_SWORD_TRAIL") != NULL;
    if (!trail) *(uint32_t *)((uint8_t *)line + SWORD_TRAIL_ON) = 0;
    {   // the hits: the three capsules from the blade in your hand, the game's own reach along it
        static float reach = REACH_DEFAULT;
        const float gx = cap[0][3] - cap[0][0], gy = cap[0][4] - cap[0][1], gz = cap[0][5] - cap[0][2];
        const float g = sqrtf(gx * gx + gy * gy + gz * gz);  // (the game's own, while it slashes)
        if (g > 60.0f && g < 250.0f) reach = g;
        float end[3];
        for (int i = 0; i < 3; i++) end[i] = blade[3 + i] - grip[i * 4 + 2] * reach;
        if (!have_last) memcpy(last_end, end, sizeof end);
        memcpy(cap[0], blade + 3, 3 * sizeof(float)), memcpy(cap[0] + 3, end, sizeof end);
        memcpy(cap[1], last_end, sizeof end), memcpy(cap[1] + 3, end, sizeof end);
        memcpy(cap[2], blade + 3, 3 * sizeof(float)), memcpy(cap[2] + 3, last_end, sizeof end);
        memcpy(last_end, end, sizeof end);
    }
    // The game's own slashes come from its motion controls: while the sword is out the right Joy-Con's turn the game
    // reads is the blade's turn in space (g_spatial; runtime/nvn/vulkan/JoyConTranslator.cpp), the arm's swing
    // included, rather than the wrist's. Swings of their own (below) hit without one.
    float speed;
    {
        static uint64_t last_ns;
        const uint64_t now_ns = SDL_GetTicksNS();
        const float dt = last_ns && now_ns > last_ns ? (float)(now_ns - last_ns) * 1e-9f : 0.0f;
        last_ns = now_ns;
        const float dx = blade[0] - last[0], dy = blade[1] - last[1], dz = blade[2] - last[2];
        speed = dt > 0 && dt < 0.25f ? sqrtf(dx * dx + dy * dy + dz * dz) / dt / vr_rig_units_per_metre() : 0.0f;
        static int log = -1;
        static uint64_t logged, fast, frames;
        if (log < 0) log = getenv("SKYRECOMP_SWORD_LOG") != NULL;
        frames++, fast += speed > SWING_SPEED;
        if (log && renderer_present_count() >= logged + 120) {
            logged = renderer_present_count();
            LOG("[sword] blade tip %.2f m/s now; faster than %.1f m/s in %llu of %llu updates\n", speed, SWING_SPEED,
                (unsigned long long)fast, (unsigned long long)frames);
            fast = frames = 0;
        }
    }
    {   // the swing's direction, and swings of their own (see the top)
        static float path[DIR_HISTORY + 1][3];  // (the tip, the last few updates; path[0] now)
        static int have_path;
        if (!have_last) have_path = 0;
        memmove(path[1], path[0], sizeof path - sizeof path[0]);
        for (int i = 0; i < 3; i++) path[0][i] = blade[i] - eye[i];  // (from his eye point: walking doesn't count)
        if (have_path <= DIR_HISTORY) have_path++;
        uint16_t bit = 0, angle = 0;
        bool dir = false;
        if (have_path > DIR_HISTORY) {
            const float d[3] = {path[0][0] - path[DIR_HISTORY][0], path[0][1] - path[DIR_HISTORY][1],
                                path[0][2] - path[DIR_HISTORY][2]};
            dir = swing_direction(d, &bit, &angle);
        }
        static bool swinging;
        static int slow;
        static uint64_t swings, logged_swings;
        const bool game = game_registered(pl);
        extern bool vr_input_blade_speed(float *mps);
        float hand = 0;
        const bool measured = vr_input_blade_speed(&hand);
        const float start = g_vr_config.sword_swing_speed, end = FREE_END * start;
        if (!g_vr_config.sword_free_swings || !have_last || !measured || ready_updates < FREE_READY_UPDATES) swinging = false;
        else if (!swinging && hand > start && !game) {
            swinging = true, slow = 0, swings++;
            free_swing_start(c, pl, dir ? bit : *(const uint16_t *)(uintptr_t)(pl + LINK_ATTACK_DIR));
        } else if (swinging) {
            slow = hand < end ? slow + 1 : 0;
            if (slow >= FREE_END_UPDATES) swinging = false;
        }
        if ((swinging || game) && dir && measured && hand > end && ready_updates >= FREE_READY_UPDATES) set_direction(pl, bit, angle);  // (the game's own too)
        if (swinging && !game) free_swing_register(c, pl);
        static int log = -1;
        if (log < 0) log = getenv("SKYRECOMP_SWORD_LOG") != NULL;
        if (log && swings != logged_swings && dir) {
            logged_swings = swings;
            LOG("[sword] a swing of its own (%llu): %.1f m/s, direction %#x (angle %#06x)%s\n", (unsigned long long)swings,
                hand, bit, angle, game ? "; the game slashing too" : "");
        }
    }
    memcpy(last, blade, sizeof last);
    have_last = true;
    static int log = -1;
    static uint64_t logged;
    if (log < 0) log = getenv("SKYRECOMP_SWORD_LOG") != NULL;
    if (log && renderer_present_count() >= logged + 120) {
        logged = renderer_present_count();
        const float gx = blade[0] - game_tip[0], gy = blade[1] - game_tip[1], gz = blade[2] - game_tip[2];
        LOG("[sword] hit line from the right controller: tip %.1f %.1f %.1f, base %.1f %.1f %.1f; the game's tip %.1f %.1f %.1f "
            "(%.0f away); his eye point %.1f %.1f %.1f\n", blade[0], blade[1], blade[2], blade[3], blade[4], blade[5],
            game_tip[0], game_tip[1], game_tip[2], sqrtf(gx * gx + gy * gy + gz * gz), eye[0], eye[1], eye[2]);
    }
}

void vr_sword_init(void) {  // (main.c, after the game code is loaded)
    const char *s = getenv("SKYRECOMP_SWORD_SHIFT");  // (test) "x y z": the game's own line moved by that much instead
    g_shift_on = s && sscanf(s, "%f %f %f", &g_shift[0], &g_shift[1], &g_shift[2]) == 3;
    g_player_update = dispatch_lookup(PLAYER_UPDATE);
    if (g_player_update) hle_override(PLAYER_UPDATE, player_update_hook, "vr_sword_player_update");
    LOG("[sword] Link's update %s%s\n", g_player_update ? "hooked" : "not found (the sword's hits stay the game's)",
        g_shift_on ? "; test: his sword's hit line moved" : "");
}
