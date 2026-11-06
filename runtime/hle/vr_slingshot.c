// VR: the Slingshot shoots where the right controller points (VR Options > Aim Items with the Controller).
//
// The game launches its seed along its own aim (the camera through its reticle, steered by the right Joy-Con as a
// pointer). Instead, right after the update that launched a seed, its velocity is turned to the right controller's
// pointing direction, its speed kept (and its first step with it); the game then flies it as its own - straight for a
// while, then falling - and its collision does the rest. vr_slingshot_path predicts that flight for the 3D marker
// (runtime/hle/vr_aim.c, the marker quad in runtime/vr/vr.c).
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

// The direction to launch along (world, unit): the right controller's, or the test's. False: leave the game's.
static bool launch_dir(float d[3]) {
    if (g_test_on) { memcpy(d, g_test_dir, sizeof g_test_dir); return true; }
    float o[3];
    return g_vr_hud_config.aim_with_controller && vr_running() && vr_rig_controller_aim(o, d);
}

static void actor_execute_hook(Cpu *c) {
    const uint64_t a = c->x[0];
    g_actor_execute(c);
    if (!a || *(const uint64_t *)(uintptr_t)a != SEED_VTABLE) return;
    float *o = (float *)(uintptr_t)(a + SEED_ORIGIN), *p = (float *)(uintptr_t)(a + SEED_POS), *v = (float *)(uintptr_t)(a + SEED_VEL);
    const float sx = p[0] - o[0], sy = p[1] - o[1], sz = p[2] - o[2], step = sqrtf(sx * sx + sy * sy + sz * sz);
    const float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    static uint64_t aimed_at;  // (the seed last turned and its launch point: each shot once - the actor is reused)
    static float aimed_origin[3];
    if (aimed_at == a && !memcmp(aimed_origin, o, sizeof aimed_origin)) return;
    if (!(speed > 1) || step > speed) return;  // (not flying, or past its first step: not this shot's launch)
    aimed_at = a, memcpy(aimed_origin, o, sizeof aimed_origin);
    float d[3];
    if (!launch_dir(d)) return;
    for (int k = 0; k < 3; k++) v[k] = d[k] * speed, p[k] = o[k] + d[k] * step;
    static int said;
    if (said++ < 3 || getenv("SKYRECOMP_SLINGSHOT_LOG"))
        LOG("[slingshot] seed launched along the controller: %.3f %.3f %.3f, speed %.1f (gravity %.1f)\n", d[0], d[1], d[2],
            speed, *(const float *)(uintptr_t)(a + SEED_GRAVITY));
}

// The seed's flight from where a shot now would start, along the controller: n points (world), one per update,
// up to max_len units of path. Returns how many. False start (no direction / no eye point): 0.
int vr_slingshot_path(float (*pts)[3], int n, float max_len) {
    float d[3], eye[3];
    if (n < 2 || !launch_dir(d) || !player_eye(eye)) return 0;
    float h[2] = {d[0], d[2]};
    const float hl = sqrtf(h[0] * h[0] + h[1] * h[1]);
    if (hl > 1e-4f) h[0] /= hl, h[1] /= hl;
    else h[0] = h[1] = 0;
    float p[3] = {eye[0] + h[0] * SEED_AHEAD_OF_EYE, eye[1] - SEED_BELOW_EYE, eye[2] + h[1] * SEED_AHEAD_OF_EYE};
    float v[3] = {d[0] * SEED_SPEED, d[1] * SEED_SPEED, d[2] * SEED_SPEED};
    float len = 0;
    int k = 0;
    memcpy(pts[k++], p, sizeof p);
    for (int u = 0; k < n && len < max_len; u++) {
        if (u >= SEED_STRAIGHT_UPDATES) {
            v[1] += SEED_GRAVITY_ON * SEED_DT;
            for (int i = 0; i < 3; i++) v[i] *= SEED_DRAG;
        }
        for (int i = 0; i < 3; i++) p[i] += v[i] * SEED_DT;
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
