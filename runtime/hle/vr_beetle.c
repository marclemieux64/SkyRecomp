// VR: the Beetle launches from the right controller, along where it points (VR Options > Aim Items with the Controller).
//
// The Beetle is an actor of its own (vtable BEETLE_VTABLE) with its own update (BEETLE_UPDATE, vtable slot 20, called
// through the vtable: hle_override). Its fields, measured headset-free (SKYRECOMP_BEETLE_SNAP, in a test build) over a
// launch and its first second of flight:
//  - position +0x144 (the usual actor position), with copies of past positions at +0x1b0, +0x1bc, +0x1c8;
//  - heading +0x13c pitch and +0x13e yaw, 16-bit angles (0x10000 a turn; pitch negative upward). Checked against its
//    path: frames 3170..3260 it flew at yaw -26.1 deg (0xed76) and about 17 deg up (0xf30a..0xf4bf); +0x1d4 / +0x1d6
//    hold -pitch / yaw;
//  - velocity +0x1e8 (vec3, about 9 units an update), recomputed from the heading every update, so the heading steers.
// Link's actor says when it flies (runtime/hle/player.c player_beetle_flying: +0x74 0x0f, launch to catch).
//
// On the Beetle's first update in flight its position becomes the right controller's (a little ahead along it) and
// its heading and velocity where the controller points, its speed kept; the game flies it from there (steering, its
// range, its return). Straight from the controller's tracked pose, nothing from the game's Joy-Con simulation.
// Test (no headset): SKYRECOMP_BEETLE_TEST_DIR="x y z" launches every Beetle along that world direction from Link's eye.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../vr/vr_hud.h"

#define BEETLE_VTABLE (MAIN_BASE + 0x1570498)
#define BEETLE_UPDATE (MAIN_BASE + 0x27e688)
#define BEETLE_POS 0x144
#define BEETLE_PITCH 0x13c
#define BEETLE_YAW 0x13e
#define BEETLE_PITCH2 0x1d4
#define BEETLE_YAW2 0x1d6
#define BEETLE_VEL 0x1e8
static const int k_past_pos[] = {0x1b0, 0x1bc, 0x1c8};
#define FROM_HAND_AHEAD 8.0f  // (units along the controller: out of the hand)

bool vr_running(void);
bool vr_rig_controller_aim(float o[3], float d[3]);
bool player_eye(float eye[3]);
bool player_beetle_flying(void);

static GuestFn g_update;
static float g_test_dir[3];
static bool g_test_on;

static bool launch_ray(float o[3], float d[3]) {
    if (g_test_on) {
        if (!player_eye(o)) return false;
        memcpy(d, g_test_dir, sizeof g_test_dir);
        for (int k = 0; k < 3; k++) o[k] += d[k] * 30.0f;
        return true;
    }
    if (!(g_vr_hud_config.aim_with_controller && vr_running() && vr_rig_controller_aim(o, d))) return false;
    for (int k = 0; k < 3; k++) o[k] += d[k] * FROM_HAND_AHEAD;
    return true;
}

static int16_t angle16(float radians) { return (int16_t)lrintf(radians * (32768.0f / (float)M_PI)); }

static void beetle_update_hook(Cpu *c) {
    const uint64_t a = c->x[0];
    g_update(c);
    if (!a || *(const uint64_t *)(uintptr_t)a != BEETLE_VTABLE) return;
    static uint64_t launched;  // (the Beetle this flight was turned for: once a flight)
    if (!player_beetle_flying()) { launched = 0; return; }
    if (launched == a) return;
    launched = a;
    float o[3], d[3];
    if (!launch_ray(o, d)) return;
    float *p = (float *)(uintptr_t)(a + BEETLE_POS), *v = (float *)(uintptr_t)(a + BEETLE_VEL);
    const float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const int16_t yaw = angle16(atan2f(d[0], d[2]));
    const int16_t pitch = angle16(-atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2])));
    *(int16_t *)(uintptr_t)(a + BEETLE_PITCH) = pitch, *(int16_t *)(uintptr_t)(a + BEETLE_YAW) = yaw;
    *(int16_t *)(uintptr_t)(a + BEETLE_PITCH2) = (int16_t)-pitch, *(int16_t *)(uintptr_t)(a + BEETLE_YAW2) = yaw;
    for (int k = 0; k < 3; k++) p[k] = o[k], v[k] = d[k] * speed;
    for (unsigned i = 0; i < sizeof k_past_pos / sizeof k_past_pos[0]; i++)  // (no sweep from the game's start to here)
        memcpy((void *)(uintptr_t)(a + k_past_pos[i]), o, 12);
    static int said;
    if (said++ < 3 || getenv("SKYRECOMP_BEETLE_LOG"))
        LOG("[beetle] launched along the controller: %.3f %.3f %.3f (pitch %04x yaw %04x), speed %.1f\n", d[0], d[1], d[2],
            (uint16_t)pitch, (uint16_t)yaw, speed);
}

void vr_beetle_init(void) {  // (main.c, after the game code is loaded)
    const char *e = getenv("SKYRECOMP_BEETLE_TEST_DIR");
    if (e && sscanf(e, "%f %f %f", &g_test_dir[0], &g_test_dir[1], &g_test_dir[2]) == 3) {
        const float n = sqrtf(g_test_dir[0] * g_test_dir[0] + g_test_dir[1] * g_test_dir[1] + g_test_dir[2] * g_test_dir[2]);
        if (n > 0) {
            for (int k = 0; k < 3; k++) g_test_dir[k] /= n;
            g_test_on = true;
        }
    }
    g_update = dispatch_lookup(BEETLE_UPDATE);
    if (g_update) hle_override(BEETLE_UPDATE, beetle_update_hook, "vr_beetle update");
    LOG("[beetle] update %s%s\n", g_update ? "hooked" : "not found (the Beetle keeps the game's aim)",
        g_test_on ? "; test: every launch along SKYRECOMP_BEETLE_TEST_DIR" : "");
}
