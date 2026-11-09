// Object culling: the headset's view in VR, and the Draw distance setting.
//
// Skyward Sword culls its objects (actors: characters, doors, items, grass, birds, effects...) against the game
// camera's frustum: 60 degrees high at 16:9, and each object's own draw distance. In a headset the eyes look
// elsewhere (a turned head, a wider field of view), and objects popped in as they came into the game camera's view
// rather than the eyes'. The level's own geometry is not culled this way (a head turned 160 degrees still sees it).
//
// Found 2026-10-26: the cull test main+0xdf5ab4 (box, distance) reads the culling camera through the global
// main+0x2bf9958: a world-space box around the frustum (+0x168 min, +0x174 max) and its planes (+0x180 .. +0x1dc,
// normal + offset, a box on the positive side of any plane is culled). The plane at +0x1b0 is the camera's forward
// axis, tested against the object's draw distance (actor +0x23c, checked by main+0xb114e0) plus +0x164:
// n . p + d - (c + dist) > 0 is culled. main+0xf261a8 rebuilds the planes from the camera's field of view (camera
// +0xd8) during the camera's update (main+0xb653f4, called by these virtual functions); after it:
//  - VR: the box is opened and the side planes are rebuilt from the eyes, and the forward plane too: objects'
//    draw distances are measured along where you look, from your eye (SKYRECOMP_VR_CULL_GAME_DISTANCE=1: the game
//    camera's, as before);
//  - Draw distance k (g_settings.draw_distance): the forward plane is scaled, n / k and d / k + c (1 - 1/k), so the
//    test becomes (depth - c) / k > dist: every object's draw distance times k.
// Nothing else changes - the projection the game renders with is the camera's own, and the objects' own fields.
//
// VR culling: on with VR; SKYRECOMP_VR_WIDE_CULL=0 turns it off (=2: opened fully, also without VR, for tests).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "../vr/vr.h"
#include "../vr/vr_rig.h"

#define CULL_CAMERA_PTR (MAIN_BASE + 0x2bf9958)

static int mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("SKYRECOMP_VR_WIDE_CULL");
        m = e ? atoi(e) : 1;
    }
    return m;
}

#define MARGIN_DEGREES 10.0f  // (the head turns between this update and the frame's rendering; the other eye)
#define APEX_BACK_METRES 0.3f  // (the planes meet behind the eye: the other eye, head movement)

static float widen(float t, float sign) {  // a field edge's tangent, MARGIN_DEGREES further out, below 85 degrees
    const float a = atanf(t) + sign * MARGIN_DEGREES * (float)(M_PI / 180.0), lim = 85.0f * (float)(M_PI / 180.0);
    return tanf(fmaxf(-lim, fminf(lim, a)));
}

// The side planes from the eyes' view: plane (n, d) in the game world, a box on its positive side is culled. In eye
// space (-z forward) the left edge x = l * -z has the outside normal (-1, 0, -l); the apex sits `back` units behind
// the eye. World: n_w = R^T n, d_w = n . t - back * n.z (view = [R | t]).
static void eye_plane(float *out, const float v[12], float nx, float ny, float nz, float back) {
    for (int k = 0; k < 3; k++) out[k] = v[0 * 4 + k] * nx + v[1 * 4 + k] * ny + v[2 * 4 + k] * nz;
    out[3] = nx * v[3] + ny * v[7] + nz * v[11] - back * nz;
}

// The forward plane (+0x1b0: each object's draw distance is tested along it) from the eye instead of the game camera:
// turned toward something, its distance is measured the way you look, not the way the game camera faces. The game's
// plane is n . p + d with n the camera's forward axis (either sign) and n . campos + d its offset at the camera; the
// eye's keeps that offset, along the eye's forward axis from the eye's position. Only a plane the game rebuilt is
// replaced (`fresh`): never one already replaced.
static void eye_forward_plane(float *f, const float eye_view[12]) {
    extern bool game_camera_view(float view[12]);
    float g[12];
    if (!game_camera_view(g)) return;
    float *p = f + 0x1b0 / 4;
    const float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (!(len > 0.5f && len < 2.0f)) return;
    const float gf[3] = {-g[8], -g[9], -g[10]};  // (the game camera's forward in the world: -z)
    const float s = p[0] * gf[0] + p[1] * gf[1] + p[2] * gf[2];
    if (fabsf(s) < 0.9f * len) return;  // (not its forward axis: left alone)
    float cam[3], eye[3];  // positions: -R^T t
    for (int k = 0; k < 3; k++) {
        cam[k] = -(g[0 * 4 + k] * g[3] + g[1 * 4 + k] * g[7] + g[2 * 4 + k] * g[11]);
        eye[k] = -(eye_view[0 * 4 + k] * eye_view[3] + eye_view[1 * 4 + k] * eye_view[7] + eye_view[2 * 4 + k] * eye_view[11]);
    }
    const float offset = p[0] * cam[0] + p[1] * cam[1] + p[2] * cam[2] + p[3];
    {   // (the plane at the camera it was built from sits near it - the near distance. Far off: the camera read isn't
        // the one culled with, and moving the plane by it would cull what's in front of the eyes. Left as the game's.)
        static int said;
        if (said < 3 || !(fabsf(offset) < 200.0f * len)) {
            if (said < 3 || said % 600 == 0)
                LOG("[vr-cull] forward plane: %.1f from the game camera%s\n", offset / len,
                    fabsf(offset) < 200.0f * len ? "" : " - not that camera's: left as the game's");
            said++;
        }
        if (!(fabsf(offset) < 200.0f * len)) return;
    }
    const float sign = s > 0 ? len : -len;
    for (int k = 0; k < 3; k++) p[k] = sign * -eye_view[8 + k];
    p[3] = offset - (p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2]);
}

static void headset_frustum(float *f, bool fresh) {
    float view[12], t[4];  // (t: left, right, down, up)
    const bool have = vr_rig_cull_view(view, t);
    if (!have && mode() != 2) return;  // (no stereo frame yet: the game's own)
    const bool eyes = have && vr_rig_cull_trusted();  // (else - title, file select - nothing is culled: below)
    for (int i = 0; i < 3; i++) {
        f[0x168 / 4 + i] = -1e30f;  // (the box around the frustum: everything)
        f[0x174 / 4 + i] = 1e30f;
    }
    static const int planes[] = {0x180, 0x190, 0x1a0, 0x1c0, 0x1d0};  // (not +0x1b0: the draw distance)
    for (unsigned i = 0; i < sizeof planes / sizeof planes[0]; i++) {
        float *p = f + planes[i] / 4;
        p[0] = p[1] = p[2] = 0;
        p[3] = -1;  // (0 . box - 1 < 0: inside)
    }
    if (eyes) {
        const float back = APEX_BACK_METRES * vr_rig_units_per_metre();
        const float l = widen(t[0], -1), r = widen(t[1], 1), d = widen(t[2], -1), u = widen(t[3], 1);
        eye_plane(f + 0x180 / 4, view, -1, 0, -l, back);
        eye_plane(f + 0x190 / 4, view, 1, 0, r, back);
        eye_plane(f + 0x1a0 / 4, view, 0, -1, -d, back);
        eye_plane(f + 0x1c0 / 4, view, 0, 1, u, back);
        if (fresh && getenv("SKYRECOMP_VR_CULL_GAME_DISTANCE") == NULL) eye_forward_plane(f, view);
    }
    static int logged;
    if (!logged++)
        LOG("[vr-cull] objects are culled with the headset's view and its distance, not the game camera's%s\n",
            eyes ? "" : " (test: no culling)");
}

// The forward plane scaled for draw distance k (a plane the game rebuilt since the last call: `fresh`).
static void scale_draw_distance(float *f, float k) {
    float *p = f + 0x1b0 / 4;
    const float c = f[0x164 / 4];
    p[0] /= k, p[1] /= k, p[2] /= k;
    p[3] = p[3] / k + c * (1.0f - 1.0f / k);
    static float logged;
    if (logged != k) LOG("[cull] draw distance x%.2f\n", k), logged = k;
}

static const uint64_t k_update[] = {0x7000b64260ull, 0x7000b645f0ull, 0x7000e1a120ull};
static GuestFn g_orig[sizeof k_update / sizeof k_update[0]];
static void update(Cpu *c, int i) {
    g_orig[i](c);
    const uint64_t cam = *(const uint64_t *)(uintptr_t)CULL_CAMERA_PTR;
    if (cam < 0x7000000000ull || cam >= 0x7800000000ull) return;
    float *f = (float *)(uintptr_t)cam;
    // (the forward plane as we left it last: unchanged, the game didn't rebuild it - replacing or scaling it again
    // would compound, whichever of the three camera updates ran)
    static float last[4];
    const bool fresh = memcmp(f + 0x1b0 / 4, last, sizeof last) != 0;
    const int m = mode();
    if (m == 2 || (m && vr_running())) headset_frustum(f, fresh);
    const float k = g_settings.draw_distance;
    if (fresh && k > 1.001f && k <= 4.0f) scale_draw_distance(f, k);
    memcpy(last, f + 0x1b0 / 4, sizeof last);
}
static void update0(Cpu *c) { update(c, 0); }
static void update1(Cpu *c) { update(c, 1); }
static void update2(Cpu *c) { update(c, 2); }

// (main.c, after dispatch_init and before the game runs)
void vr_cull_init(void) {
    extern bool g_is_skyward;
    if (!g_is_skyward) return;
    static const HostFn wrap[] = {update0, update1, update2};
    int n = 0;
    for (size_t i = 0; i < sizeof k_update / sizeof k_update[0]; i++)
        if ((g_orig[i] = dispatch_lookup(k_update[i]))) hle_override(k_update[i], wrap[i], "cull_camera_update"), n++;
    LOG("[cull] %d hooks (headset culling in VR, draw distance)\n", n);
}
