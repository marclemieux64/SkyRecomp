// VR: an aiming item's reticle (the Slingshot's, the Bow's, the Clawshots', the Gust Bellows') drawn where the shot
// will land, at its depth in the world, instead of on the flat HUD panel (VR Options > Aiming Reticle in 3D;
// [VR] aim_reticle_3d).
//
// The game keeps no "where the shot lands" point (a memory search finds only points at fixed distances along the aim
// line), so it is found from the left eye's depth. NS points along the game camera's aim line (through the reticle's
// centre), 20 to 5000 units ahead, are projected into the left eye's picture. exec.c reads the scene's depth there
// (aim_probe: copied out at the reticle's first draw, read a frame or two later, never waiting). vr_aim_result takes
// the first place where the line passes behind a surface that lies between two neighbouring samples; a surface merely
// in front of the line (your hand) doesn't count. With no surface, it takes the end of the range. The reticle's
// pieces are then drawn in each eye's own picture rather than on the HUD panel, moved so they sit at that point
// (vr_aim_shift). Each eye uses its own view, so they appear at its depth.
//
// The reticle's pieces are known by their texture names (runtime/nvn/texpatch.c R_AIM). The plain circle (the
// Slingshot's dot) counts only after a real piece in the same frame: other screens draw it too.
// SKYRECOMP_AIM_LOG=1: the end point and its depth samples every 60 frames.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "../vr/vr_hud.h"

bool game_camera_view(float view[12]);  // runtime/hle/player.c: the game camera (world -> camera), as this frame's
bool vr_rig_controller_aim(float o[3], float d[3]);  // runtime/vr/vr_rig.c: the right controller's ray, game world
extern uint64_t renderer_present_count(void);

#define NS 48
#define RANGE_NEAR 20.0f
#define RANGE_FAR 5000.0f
static float g_eye[2][16], g_game[16];  // eye k's camera (game camera space -> its clip space), the game's projection
static uint64_t g_eye_frame[2] = {~0ull, ~0ull};
static float g_center[2];               // the reticle's centre on the game's screen (NDC), from its unrotated pieces
static uint64_t g_center_frame = ~0ull;
static struct { float w[NS][3], d[NS]; bool ok[NS]; int n; float o[3], dir[3]; } g_probe[2];  // per probe slot (exec.c)
static float g_P[3];                    // the end point, world
static uint64_t g_P_frame = ~0ull;

bool vr_aim_enabled(void) {
    static int off = -1;
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_AIM") != NULL;
    return !off && g_vr_hud_config.aim_reticle_3d;
}

// (exec.c, a main-view scene draw) Eye `eye`'s camera block and the game's, both 28 floats (a projection, then the
// draw's view block: camera x model). The model cancels: C = eye view block x game view block^-1 takes game camera
// space to the eye's, so the eye's clip from game camera space is its projection x C.
void vr_aim_camera(int eye, const float eye_block[28], const float game_block[28]) {
    if (eye < 0 || eye > 1) return;
    const float *m = game_block + 16, *e = eye_block + 16;
    // inverse of the game's view block (3x4 affine: [A | t] -> [A^-1 | -A^-1 t])
    const float a = m[0], b = m[1], c = m[2], d = m[4], f = m[5], g = m[6], h = m[8], i = m[9], k = m[10];
    const float det = a * (f * k - g * i) - b * (d * k - g * h) + c * (d * i - f * h);
    if (!(fabsf(det) > 1e-12f)) return;
    const float inv[9] = {(f * k - g * i) / det, (c * i - b * k) / det, (b * g - c * f) / det,
                          (g * h - d * k) / det, (a * k - c * h) / det, (c * d - a * g) / det,
                          (d * i - f * h) / det, (b * h - a * i) / det, (a * f - b * d) / det};
    float mi[12];
    for (int r = 0; r < 3; r++) {
        for (int q = 0; q < 3; q++) mi[r * 4 + q] = inv[r * 3 + q];
        mi[r * 4 + 3] = -(inv[r * 3] * m[3] + inv[r * 3 + 1] * m[7] + inv[r * 3 + 2] * m[11]);
    }
    float C[16] = {0};
    for (int r = 0; r < 3; r++)
        for (int q = 0; q < 4; q++)
            C[r * 4 + q] = e[r * 4] * mi[q] + e[r * 4 + 1] * mi[4 + q] + e[r * 4 + 2] * mi[8 + q] + (q == 3 ? e[r * 4 + 3] : 0);
    C[15] = 1;
    for (int r = 0; r < 4; r++)
        for (int q = 0; q < 4; q++)
            g_eye[eye][r * 4 + q] = eye_block[r * 4] * C[q] + eye_block[r * 4 + 1] * C[4 + q] + eye_block[r * 4 + 2] * C[8 + q] +
                                    eye_block[r * 4 + 3] * C[12 + q];
    if (eye == 0) memcpy(g_game, game_block, sizeof g_game);
    g_eye_frame[eye] = renderer_present_count();
}

void vr_aim_center(float x, float y) { g_center[0] = x, g_center[1] = y, g_center_frame = renderer_present_count(); }

static bool to_clip(const float M[16], const float c[3], float o[4]) {
    for (int i = 0; i < 4; i++) o[i] = M[i * 4] * c[0] + M[i * 4 + 1] * c[1] + M[i * 4 + 2] * c[2] + M[i * 4 + 3];
    return o[3] > 1e-3f;
}
static void to_cam(const float V[12], const float p[3], float c[3]) {
    for (int i = 0; i < 3; i++) c[i] = V[i * 4] * p[0] + V[i * 4 + 1] * p[1] + V[i * 4 + 2] * p[2] + V[i * 4 + 3];
}

// The aim line this frame: from the game camera through the reticle's centre (the screen's centre without one)
static bool aim_line(const float V[12], float o[3], float d[3]) {
    for (int i = 0; i < 3; i++) o[i] = -(V[i] * V[3] + V[4 + i] * V[7] + V[8 + i] * V[11]);  // -R^T t
    uint64_t f = renderer_present_count();
    float nx = 0, ny = 0;
    if (g_center_frame != ~0ull && g_center_frame + 2 >= f) nx = g_center[0], ny = g_center[1];
    if (!(fabsf(g_game[0]) > 1e-4f && fabsf(g_game[5]) > 1e-4f)) return false;
    const float c[3] = {(nx + g_game[2]) / g_game[0], (ny + g_game[6]) / g_game[5], -1};  // (camera space, at z = -1)
    for (int i = 0; i < 3; i++) d[i] = V[i] * c[0] + V[4 + i] * c[1] + V[8 + i] * c[2];  // R^T c
    const float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(n > 0)) return false;
    for (int i = 0; i < 3; i++) d[i] /= n;
    return true;
}

// exec.c: where to read the left eye's depth for probe slot `slot`: u, v in 0..1 of its picture (v down). Returns
// how many. flip_y: the picture's rows run the other way from GL's; minus_one: depth from GL's -1..1
int vr_aim_probe(int slot, float u[], float v[], int max, bool flip_y, bool minus_one) {
    uint64_t f = renderer_present_count();
    float o[3], d[3], V[12];
    if (slot < 0 || slot > 1 || g_eye_frame[0] == ~0ull || g_eye_frame[0] + 1 < f || !game_camera_view(V))
        return 0;
    // Aiming with the controller: along its ray - it starts at the hand, in the left eye's view from the start. The
    // game camera's line starts wherever that camera is (in VR not at the eyes): its first stretch fell outside the
    // left eye's picture, so where it went behind a surface there was missed ("nothing hit", the reticle far off).
    // The game is then aimed at the point found (vr_aim_pointer): the reticle and the shot meet there.
    if (!(g_vr_hud_config.aim_with_controller && vr_rig_controller_aim(o, d)) && !aim_line(V, o, d)) return 0;
    int n = NS < max ? NS : max;
    memcpy(g_probe[slot].o, o, sizeof o), memcpy(g_probe[slot].dir, d, sizeof d);
    for (int i = 0; i < n; i++) {
        float t = RANGE_NEAR * powf(RANGE_FAR / RANGE_NEAR, (float)i / (float)(n - 1)), c[3], k[4];
        float *w = g_probe[slot].w[i];
        for (int j = 0; j < 3; j++) w[j] = o[j] + d[j] * t;
        to_cam(V, w, c);
        bool ok = to_clip(g_eye[0], c, k);
        float x = ok ? k[0] / k[3] : 0, y = ok ? k[1] / k[3] : 0, z = ok ? k[2] / k[3] : 0;
        ok &= fabsf(x) < 1 && fabsf(y) < 1;
        g_probe[slot].ok[i] = ok;
        g_probe[slot].d[i] = minus_one ? (z + 1) * 0.5f : z;
        u[i] = ok ? x * 0.5f + 0.5f : 0;
        v[i] = ok ? (flip_y ? -y : y) * 0.5f + 0.5f : 0;
    }
    g_probe[slot].n = n;
    return n;
}

void vr_aim_result(int slot, const float scene[], int n) {
    if (slot < 0 || slot > 1 || n != g_probe[slot].n || n < 2) return;
    const float *d = g_probe[slot].d;
    const bool *ok = g_probe[slot].ok;
    int first = -1, final = -1;  // (reversed depth - far is 0 - told by the line's own on-screen samples)
    for (int i = 0; i < n; i++) if (ok[i]) { if (first < 0) first = i; final = i; }
    bool rev = first >= 0 && final > first && d[final] < d[first];
    int hit = -1;
    float frac = 1;
    for (int i = 1; i < n && hit < 0; i++) {
        if (!ok[i] || !ok[i - 1]) continue;
        float a = rev ? 1 - d[i - 1] : d[i - 1], b = rev ? 1 - d[i] : d[i], s = rev ? 1 - scene[i] : scene[i];
        if (!(s < b)) continue;              // (this sample is in front of what's there)
        if (s + 1e-6f < a) continue;         // (behind something nearer than the line here: not on the line)
        hit = i, frac = b > a ? (s - a) / (b - a) : 1;
    }
    // The distance along the line, steadied: the end point flipped between a surface a few hundred units off and
    // "nothing hit" (the range's end) from one probe to the next, and every flip moved the reticle and re-aimed the
    // game. After a hit, "nothing" counts from its third time in a row; small changes are eased, big ones (onto
    // another surface) taken at once.
    static float steady_t = -1;
    static int misses;
    float t_new;
    if (hit < 0) {
        t_new = steady_t > 0 && steady_t < RANGE_FAR * 0.99f && ++misses < 3 ? steady_t : RANGE_FAR;
    } else {
        misses = 0;
        const float t0 = RANGE_NEAR * powf(RANGE_FAR / RANGE_NEAR, (float)(hit - 1) / (float)(n - 1));
        const float t1 = RANGE_NEAR * powf(RANGE_FAR / RANGE_NEAR, (float)hit / (float)(n - 1));
        t_new = t0 + (t1 - t0) * fminf(1, fmaxf(0, frac));
    }
    const float t = steady_t > 0 && t_new < steady_t * 1.5f && steady_t < t_new * 1.5f
                        ? expf(logf(steady_t) * 0.6f + logf(t_new) * 0.4f) : t_new;
    steady_t = t;
    for (int j = 0; j < 3; j++) g_P[j] = g_probe[slot].o[j] + g_probe[slot].dir[j] * t;
    g_P_frame = renderer_present_count();
    static int log = -1;
    if (log < 0) log = getenv("SKYRECOMP_AIM_LOG") != NULL;
    static uint64_t last;
    if (log && g_P_frame >= last + 60) {
        last = g_P_frame;
        LOG("[aim] end point %.1f %.1f %.1f: %s\n", g_P[0], g_P[1], g_P[2], hit < 0 ? "nothing hit" : "hits");
        for (int i = 0; i < n; i += 4)
            LOG("[aim]   sample %d%s: line depth %.7f, scene %.7f\n", i, ok[i] ? "" : " (off screen)", d[i], scene[i]);
    }
}

// ---------------------------------------------------------------- aiming with the controller
// The game aims an item with the right Joy-Con's orientation, as a pointer: the reticle sits where it points on the
// screen, the camera turning when it points past an edge, relative to a centre it sets itself - hard to hit with
// in VR, where the screen is the game camera and not where the controller points. Instead the game is told the
// orientation that puts its reticle where the right controller's ray meets the game's screen (vr_controls.c
// aim_pointer, with the menu pointer's mapping vr_pointer_direction): the shot goes where the controller points.
// Off the screen the reticle is sent past that edge and the game turns its camera there. The mapping's centre may
// sit elsewhere (the game sets its own when the item comes out), so where the reticle really is (vr_aim_center)
// is compared with where it should be and the difference fed back, slowly, while both are on screen.
// VR Options > Aim Items with the Controller ([VR] aim_with_controller); SKYRECOMP_AIM_LOG=1 logs it.
#define AIM_FAR 3000.0f     // (the ray's point that is put on the screen: far enough that its origin hardly matters)
#define AIM_GAIN 0.12f      // (of the reticle's error fed back per game frame)
#define AIM_BIAS_MAX 0.6f
bool vr_aim_pointer(float *u_out, float *v_out) {
    static float bias[2];
    static uint64_t last;
    const uint64_t f = renderer_present_count();
    if (!g_vr_hud_config.aim_with_controller || g_center_frame == ~0ull || g_center_frame + 10 < f) {
        bias[0] = bias[1] = 0;  // (no reticle on screen: the next item starts afresh)
        return false;
    }
    float o[3], d[3], V[12], c[3], k[4];
    if (!vr_rig_controller_aim(o, d) || !game_camera_view(V) || !(fabsf(g_game[0]) > 1e-4f && fabsf(g_game[5]) > 1e-4f))
        return false;
    // (where the controller's ray meets the world - the 3D reticle's point, from its ray: vr_aim_probe - else far along it)
    float p[3] = {o[0] + d[0] * AIM_FAR, o[1] + d[1] * AIM_FAR, o[2] + d[2] * AIM_FAR};
    if (g_P_frame != ~0ull && g_P_frame + 10 >= f) memcpy(p, g_P, sizeof p);
    to_cam(V, p, c);
    float x, y;
    if (to_clip(g_game, c, k)) x = k[0] / k[3], y = k[1] / k[3];
    else x = c[0] > 0 ? 4.0f : -4.0f, y = c[1] > 0 ? 4.0f : -4.0f;  // (behind the camera: past the edge it's on)
    extern int nvn_window_origin(void);
    const bool flip = nvn_window_origin() == 0;  // (the screen's rows run down; clip y up unless the origin is the top)
    const float want[2] = {x * 0.5f + 0.5f, (flip ? -y : y) * 0.5f + 0.5f};
    const float now[2] = {g_center[0] * 0.5f + 0.5f, (flip ? -g_center[1] : g_center[1]) * 0.5f + 0.5f};
    const bool inside = want[0] > 0.05f && want[0] < 0.95f && want[1] > 0.05f && want[1] < 0.95f &&
                        now[0] > 0.02f && now[0] < 0.98f && now[1] > 0.02f && now[1] < 0.98f;
    if (f != last && inside && g_center_frame + 2 >= f)
        for (int i = 0; i < 2; i++) bias[i] = fmaxf(-AIM_BIAS_MAX, fminf(AIM_BIAS_MAX, bias[i] + AIM_GAIN * (want[i] - now[i])));
    last = f;
    *u_out = fmaxf(-0.4f, fminf(1.4f, want[0] + bias[0]));
    *v_out = fmaxf(-0.4f, fminf(1.4f, want[1] + bias[1]));
    static int log = -1;
    static uint64_t logged;
    if (log < 0) log = getenv("SKYRECOMP_AIM_LOG") != NULL;
    if (log && f >= logged + 60)
        logged = f, LOG("[aim] controller: wants %.3f %.3f, reticle at %.3f %.3f, correction %.3f %.3f\n", want[0], want[1],
                        now[0], now[1], bias[0], bias[1]);
    return true;
}

// (exec.c) An end point and both eyes' cameras, recent: the reticle can go to the eyes (else it stays on the panel,
// the same in both eyes)
bool vr_aim_ready(void) {
    const uint64_t f = renderer_present_count();
    return g_P_frame != ~0ull && g_P_frame + 10 >= f && g_eye_frame[0] != ~0ull && g_eye_frame[0] + 1 >= f &&
           g_eye_frame[1] != ~0ull && g_eye_frame[1] + 1 >= f;
}

// The reticle in eye `eye`: where its centre goes in that eye's picture (clip x, y) and where its centre is now on
// the game's screen (clip x, y; once per frame, so every piece of a frame moves together - with each piece moved by
// the game's own projection of the end point instead, the game's reticle lagging the controller put the pieces off
// it, and a centre changing mid-frame pulled them apart). False: no end point yet.
bool vr_aim_target(int eye, float target[2], float centre[2]) {
    static uint64_t latch_frame = ~0ull;
    static float latch[2];
    const uint64_t f = renderer_present_count();
    float V[12], c[3], ke[4];
    if (eye < 0 || eye > 1 || g_P_frame == ~0ull || g_P_frame + 10 < f || g_eye_frame[eye] == ~0ull || g_eye_frame[eye] + 1 < f ||
        g_center_frame == ~0ull || g_center_frame + 10 < f || !game_camera_view(V))
        return false;
    if (latch_frame != f) latch_frame = f, latch[0] = g_center[0], latch[1] = g_center[1];
    to_cam(V, g_P, c);
    if (!to_clip(g_eye[eye], c, ke)) return false;
    target[0] = ke[0] / ke[3], target[1] = ke[1] / ke[3];
    centre[0] = latch[0], centre[1] = latch[1];
    static int log = -1;
    static uint64_t logged[2];
    if (log < 0) log = getenv("SKYRECOMP_AIM_LOG") != NULL;
    if (log && f >= logged[eye] + 60)
        logged[eye] = f, LOG("[aim] frame %llu eye %d: end point at %.4f %.4f in its picture, %.1f deep; reticle centre %.4f %.4f\n",
                             (unsigned long long)f, eye, target[0], target[1], ke[3], centre[0], centre[1]);
    return isfinite(target[0]) && isfinite(target[1]);
}

