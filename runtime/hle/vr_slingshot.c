// VR: the Slingshot shoots where the right controller points (VR Options > Aim Items with the Controller).
//
// The game launches its seed along its own aim (the camera through its reticle, steered by the right Joy-Con as a
// pointer). Instead, right after the update that launched a seed, it is moved to the right controller (just ahead of
// it) and its velocity turned to where the controller points, its speed kept - straight from the controller's tracked
// pose, nothing from the game's Joy-Con simulation (which is held still meanwhile: vr_aim.c vr_aim_pointer). The game
// then flies it as its own - straight for a while, then falling - and its collision does the rest. vr_slingshot_path
// predicts that flight for the 3D marker (runtime/hle/vr_aim.c, the marker quad in runtime/vr/vr.c).
//
// Found in a headset-free build by following positions near Link after a shot (a steady, fast mover), then watching
// the functions touching it:
//  - the seed is an actor with vtable SEED_VTABLE; actors are updated through vtable slot 20 (Link's: main+0xab7cf4);
//    the seed doesn't override it: the shared update ACTOR_EXECUTE moves it (an indirect call: hle_override);
//  - in it (offsets from the actor): its launch point +0x120, position +0x144, gravity +0x1e0, velocity +0x1e8;
//  - its flight: speed 150 (game units per 1/30 s); straight (gravity 0) for 13 updates at 60 fps (975 units), then
//    gravity -5 and its velocity x 0.99 each update; each update moves it velocity x 0.5;
//  - its launch point: 21 units below Link's eye point (main+0x1829a30) and 22 ahead of it.
// Test (no headset): SKYRECOMP_SLINGSHOT_TEST_DIR="x y z" turns every shot to that world direction.
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../vr/vr_hud.h"

#define ACTOR_EXECUTE (MAIN_BASE + 0x295a4)
#define SEED_VTABLE (MAIN_BASE + 0x155fb60)
#define SEED_ORIGIN 0x120
#define SEED_POS 0x144
#define SEED_GRAVITY 0x1e0
#define SEED_VEL 0x1e8
#define SEED_SPEED 150.0f
#define SEED_STRAIGHT_UPDATES 13
#define SEED_GRAVITY_ON -5.0f
#define SEED_DRAG 0.99f
#define SEED_DT 0.5f
#define SEED_BELOW_EYE 21.0f
#define SEED_AHEAD_OF_EYE 22.0f

bool vr_running(void);
bool vr_rig_controller_aim(float o[3], float d[3]);
bool player_eye(float eye[3]);
extern uint64_t renderer_present_count(void);

static GuestFn g_actor_execute;
static float g_test_dir[3];
static bool g_test_on;

// The flight as measured on the seeds themselves (each one followed while it flies): its time step per update (in
// 1/30 s, the unit of its speed - it follows the frame rate), how far it flies straight, its gravity and its drag
// per 1/30 s. Starting values from the headset-free run (60 updates a second); every shot refines them, so the
// prediction follows the frame rate the game really runs at.
static float g_dt = SEED_DT, g_straight = SEED_STRAIGHT_UPDATES * SEED_SPEED * SEED_DT, g_gravity = SEED_GRAVITY_ON;
static float g_drag_unit = SEED_DRAG * SEED_DRAG;  // (x0.99 per 0.5: x0.9801 per 1/30 s)
static struct { uint64_t a; float origin[3], p[3], v[3]; int updates; bool falling; } g_fly;

static void follow_flight(uint64_t a) {
    const float *o = (const float *)(uintptr_t)(a + SEED_ORIGIN), *p = (const float *)(uintptr_t)(a + SEED_POS);
    const float *v = (const float *)(uintptr_t)(a + SEED_VEL), g = *(const float *)(uintptr_t)(a + SEED_GRAVITY);
    if (g_fly.a != a || memcmp(g_fly.origin, o, sizeof g_fly.origin)) return;  // (not the shot being followed)
    const float sx = p[0] - g_fly.p[0], sy = p[1] - g_fly.p[1], sz = p[2] - g_fly.p[2], step = sqrtf(sx * sx + sy * sy + sz * sz);
    const float sp = sqrtf(g_fly.v[0] * g_fly.v[0] + g_fly.v[1] * g_fly.v[1] + g_fly.v[2] * g_fly.v[2]);
    const float hv = sqrtf(v[0] * v[0] + v[2] * v[2]), hv0 = sqrtf(g_fly.v[0] * g_fly.v[0] + g_fly.v[2] * g_fly.v[2]);
    if (!(step > 0) || g_fly.updates > 600) { g_fly.a = 0; return; }  // (it hit something, or is gone)
    float dt = 0;
    if (sp > 1) {
        dt = step / sp;
        if (dt > 0.05f && dt < 2.0f) g_dt += (dt - g_dt) * 0.5f;
    }
    if (g != 0 && !g_fly.falling) {  // (gravity on: how far it flew straight)
        g_fly.falling = true;
        const float dx = g_fly.p[0] - o[0], dy = g_fly.p[1] - o[1], dz = g_fly.p[2] - o[2];
        const float d0 = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d0 > 100 && d0 < 5000) g_straight += (d0 - g_straight) * 0.5f;
        g_gravity = g;
    } else if (g != 0 && hv0 > 1 && dt > 0.05f) {  // (falling: the drag, per 1/30 s)
        const float r = hv / hv0;
        if (r > 0.5f && r <= 1.0f) g_drag_unit += (powf(r, 1.0f / dt) - g_drag_unit) * 0.3f;
    }
    memcpy(g_fly.p, p, sizeof g_fly.p), memcpy(g_fly.v, v, sizeof g_fly.v);
    g_fly.updates++;
    static uint64_t logged;
    if (getenv("SKYRECOMP_SLINGSHOT_LOG") && g_fly.updates % 4 == 0 && renderer_present_count() != logged)
        logged = renderer_present_count(), LOG("[slingshot] flight: update %d, step %.1f (dt %.3f), gravity %.1f -> model dt %.3f, "
                                               "straight %.0f, drag %.4f per 1/30 s\n", g_fly.updates, step, dt, g, g_dt,
                                               g_straight, g_drag_unit);
}

// The shot's start and direction (world): the right controller's tracked pose as it is - its position (a little
// ahead along it, out of the hand) and where it points; nothing from the game's Joy-Con simulation. The test's
// direction from Link's eye point instead. False: leave the game's shot alone.
#define FROM_HAND_AHEAD 5.0f
static bool launch_ray(float o[3], float d[3]) {
    if (g_test_on) {
        memcpy(d, g_test_dir, sizeof g_test_dir);
        float eye[3];
        if (!player_eye(eye)) return false;
        for (int k = 0; k < 3; k++) o[k] = eye[k] + d[k] * SEED_AHEAD_OF_EYE;
        o[1] -= SEED_BELOW_EYE;
        return true;
    }
    if (!(g_vr_hud_config.aim_with_controller && vr_running() && vr_rig_controller_aim(o, d))) return false;
    for (int k = 0; k < 3; k++) o[k] += d[k] * FROM_HAND_AHEAD;
    return true;
}
static bool launch_dir(float d[3]) { float o[3]; return launch_ray(o, d); }

static void actor_execute_hook(Cpu *c) {
    const uint64_t a = c->x[0];
    g_actor_execute(c);
    if (!a || *(const uint64_t *)(uintptr_t)a != SEED_VTABLE) return;
    float *o = (float *)(uintptr_t)(a + SEED_ORIGIN), *p = (float *)(uintptr_t)(a + SEED_POS), *v = (float *)(uintptr_t)(a + SEED_VEL);
    const float sx = p[0] - o[0], sy = p[1] - o[1], sz = p[2] - o[2], step = sqrtf(sx * sx + sy * sy + sz * sz);
    const float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    static uint64_t aimed_at;  // (the seed last turned and its launch point: each shot once - the actor is reused)
    static float aimed_origin[3];
    if (aimed_at == a && !memcmp(aimed_origin, o, sizeof aimed_origin)) { follow_flight(a); return; }
    if (!(speed > 1) || step > speed) return;  // (not flying, or past its first step: not this shot's launch)
    float start[3], d[3];
    if (!launch_ray(start, d)) { aimed_at = a, memcpy(aimed_origin, o, sizeof aimed_origin); return; }
    // (from the hand: its launch point moved there too - the game measures its range from it)
    for (int k = 0; k < 3; k++) o[k] = start[k], v[k] = d[k] * speed, p[k] = start[k] + d[k] * step;
    aimed_at = a, memcpy(aimed_origin, o, sizeof aimed_origin);
    // (followed from here: vr_slingshot_path's flight measured on it)
    g_fly.a = a, g_fly.updates = 0, g_fly.falling = false;
    memcpy(g_fly.origin, o, sizeof g_fly.origin), memcpy(g_fly.p, p, sizeof g_fly.p), memcpy(g_fly.v, v, sizeof g_fly.v);
    static int said;
    if (said++ < 3 || getenv("SKYRECOMP_SLINGSHOT_LOG"))
        LOG("[slingshot] seed launched along the controller: %.3f %.3f %.3f, speed %.1f (gravity %.1f)\n", d[0], d[1], d[2],
            speed, *(const float *)(uintptr_t)(a + SEED_GRAVITY));
}

// The seed's flight from where a shot now would start, along the controller: n points (world), one per update,
// up to max_len units of path. Returns how many. False start (no direction / no eye point): 0.
int vr_slingshot_path(float (*pts)[3], int n, float max_len) {
    float d[3], p[3];
    if (n < 2 || !launch_ray(p, d)) return 0;
    float v[3] = {d[0] * SEED_SPEED, d[1] * SEED_SPEED, d[2] * SEED_SPEED};
    // (in steps of 1/30 s's half - fine enough for the marker - with the measured flight: straight for g_straight,
    // then gravity and drag per unit of time)
    const float dt = 0.5f, drag = powf(g_drag_unit, dt);
    float len = 0, flown = 0;
    int k = 0;
    memcpy(pts[k++], p, sizeof p);
    for (int u = 0; k < n && len < max_len; u++) {
        if (flown >= g_straight) {
            v[1] += g_gravity * dt;
            for (int i = 0; i < 3; i++) v[i] *= drag;
        }
        flown += sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * dt;
        for (int i = 0; i < 3; i++) p[i] += v[i] * dt;
        const float *q = pts[k - 1];
        len += sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
        memcpy(pts[k++], p, sizeof p);
    }
    return k;
}

// (exec.c) The Slingshot's own reticle is replaced (hidden, the marker shown instead) - aiming it with the controller
bool vr_slingshot_on(void) { float d[3]; return launch_dir(d); }

void vr_slingshot_init(void) {
    const char *e = getenv("SKYRECOMP_SLINGSHOT_TEST_DIR");
    if (e && sscanf(e, "%f %f %f", &g_test_dir[0], &g_test_dir[1], &g_test_dir[2]) == 3) {
        const float n = sqrtf(g_test_dir[0] * g_test_dir[0] + g_test_dir[1] * g_test_dir[1] + g_test_dir[2] * g_test_dir[2]);
        if (n > 0) {
            for (int k = 0; k < 3; k++) g_test_dir[k] /= n;
            g_test_on = true;
        }
    }
    g_actor_execute = dispatch_lookup(ACTOR_EXECUTE);
    if (g_actor_execute) hle_override(ACTOR_EXECUTE, actor_execute_hook, "vr_slingshot actor update");
    LOG("[slingshot] actor update %s%s\n", g_actor_execute ? "hooked" : "not found (the Slingshot keeps the game's aim)",
        g_test_on ? "; test: every shot along SKYRECOMP_SLINGSHOT_TEST_DIR" : "");
}
