// VR camera rig (see vr_rig.h): the eye cameras, Link's own draws hidden in 1st person, and culling.
#include "vr_rig_internal.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vr_settings.h"

bool player_eye(float eye[3]);  // runtime/hle/player.c: Link's eye point (feet + 150 units)
bool player_feet_y(float *y);   // runtime/hle/player.c: the world height of Link's feet
#if !defined(__APPLE__)
bool sky_vr_camera_with_view(const float mono[28], const float game_view[12], const float view[12], const float fov[4],
                             float near_units, float out[28]);  // runtime/nvn/vulkan/StereoCameraHook.h
bool sky_vr_camera_direct(const float mono[28], const float view[12], const float fov[4], float near_units,
                          float out[28]);
bool sky_vr_is_scene_camera(const float *ubo, uint64_t bytes);
bool sky_vr_is_scene_projection(const float *ubo, uint64_t bytes);
bool game_camera_view(float view[12]);  // runtime/hle/player.c: the game camera's own view
#endif

// 1st person: geometry nearer than this is clipped (Link's own head around the eyes). SKYRECOMP_VR_NEAR overrides.
#define FIRST_PERSON_NEAR 15.0f
// 1st person -> theatre (3rd-person view) when the game camera is farther than this from Link's eye point, back
// below THEATRE_LEAVE. Walking, the camera trails Link by ~400 units.
#define THEATRE_ENTER 1500.0f
#define THEATRE_LEAVE 1000.0f
#define THEATRE_FRAMES 20  // frames the camera must stay far before switching
// A skinned draw is Link's when one of its bones lies this close to his eye point (measured: a few units standing,
// up to ~100 swimming / jumping, where the eye point stays at floor + 150; other characters' nearest bones stayed
// 150+ away).
#define LINK_BONE_RADIUS 110.0f
// His head's own meshes (measured in his room): palettes of 16..99 matrices (hair 24, face 44) with a bone this close
// to the eye point; his body's meshes use the whole skeleton (186..368); his eyes are rigid, tiny, at the eye point.
// Held items use small palettes (the Slingshot has 5), at his hands: never taken for Link.
#define LINK_HEAD_MIN_PALETTE 16
#define LINK_SKELETON_MIN_PALETTE 100
#define LINK_HEAD_RADIUS 30.0f
#define LINK_FACE_MIN_PALETTE 40  // (his face: 44 matrices - where his head is)
// Scripted moves (opening a door): the game animates Link through it while his eye point (and his actor's position)
// stay put, then jump. 1st person follows his animated head then: once it is this far (horizontally) from the eye
// point, the eye is the head plus its usual offset from the eye point (measured while they stay together).
#define LINK_HEAD_DETACH 45.0f
#define LINK_HEAD_ATTACHED 20.0f
#define LINK_EYE_PART_MAX_VERTS 96
#define LINK_EYE_PART_RADIUS 12.0f
#define LINK_BELT_RADIUS 18.0f
#define LINK_MESH_CACHE 64
// Swimming: the game keeps the eye point at the water surface (even while Link dives), which cuts the view in half.
// In water the eye point is more than SWIM_EYE_ABOVE_FEET above Link's feet (on land it is feet + 150, measured up to
// 151.2) and holds perfectly still (on stairs the gap can grow too, but the height keeps changing). Then, while Link
// himself is drawn there (not so in the knight's rescue, where the same numbers occur), the eye is kept at least
// SWIM_EYE_RAISE above the surface. This is a hard clearance while swimming: easing or room-scale head movement
// through the water plane makes its screen-space reflection singular (flat fans / streaks). The offset eases out.
// (Following his feet instead fails: swimming at the surface they sit 154..196 below it depending on his pose.)
#define SWIM_EYE_ABOVE_FEET 153.0f
#define SWIM_STILL_FRAMES 10
#define SWIM_EYE_RAISE 12.0f

// ================================================================== math
typedef struct { float x, y, z, w; } Quat;

static Quat q_mul(Quat a, Quat b) {
    return (Quat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Quat q_conj(Quat q) { return (Quat){-q.x, -q.y, -q.z, q.w}; }
static Quat q_norm(Quat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-6f) || !isfinite(n)) return (Quat){0, 0, 0, 1};
    return (Quat){q.x / n, q.y / n, q.z / n, q.w / n};
}
static void q_matrix(Quat q, float m[9]) {  // row-major rotation
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                        2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    memcpy(m, r, sizeof r);
}
static void yaw_matrix(float yaw, float m[9]) {  // rotation about +y taking +z to (sin yaw, 0, cos yaw)
    const float c = cosf(yaw), s = sinf(yaw);
    const float r[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    memcpy(m, r, sizeof r);
}

void vr_rig_yaw_only(float out[4], const float orientation[4]) {
    Quat q = q_norm((Quat){orientation[0], orientation[1], orientation[2], orientation[3]});
    float m[9], f[3];
    q_matrix(q, m);
    m_vec(m, (const float[3]){0, 0, -1}, f);
    float yaw = (fabsf(f[0]) < 1e-5f && fabsf(f[2]) < 1e-5f) ? 0.0f : atan2f(-f[0], -f[2]);  // (OpenXR: about +y)
    out[0] = 0; out[1] = sinf(yaw * 0.5f); out[2] = 0; out[3] = cosf(yaw * 0.5f);
}

void vr_rig_anchor_camera(VrRigAnchor *a, const float v[12], bool level, float height, float distance) {
    // camera -> world rotation = R^T; camera position = -R^T t
    const float rc[9] = {v[0], v[4], v[8], v[1], v[5], v[9], v[2], v[6], v[10]};
    const float t[3] = {v[3], v[7], v[11]};
    float pos[3];
    m_vec(rc, t, pos);
    for (int i = 0; i < 3; i++) pos[i] = -pos[i];
    const float f[3] = {-v[8], -v[9], -v[10]};  // forward (-z) in the world
    const float n = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (level && n > 1e-4f) yaw_matrix(atan2f(-f[0], -f[2]), a->rotation);
    else memcpy(a->rotation, rc, sizeof rc);
    pos[1] += height;
    if (n > 1e-4f) { pos[0] -= f[0] / n * distance; pos[2] -= f[2] / n * distance; }
    memcpy(a->origin, pos, sizeof pos);
}

void vr_rig_anchor_point(VrRigAnchor *a, const float position[3], float yaw) {
    yaw_matrix(yaw + (float)M_PI, a->rotation);  // the anchor's -z faces `yaw`
    memcpy(a->origin, position, sizeof a->origin);
}

void vr_rig_relative(const float reference[7], const float eye[7], float units, float rot[9], float offset[3]) {
    float ry[4];
    vr_rig_yaw_only(ry, reference);
    const Quat ref = {ry[0], ry[1], ry[2], ry[3]};
    const Quat q = q_norm((Quat){eye[0], eye[1], eye[2], eye[3]});
    q_matrix(q_mul(q_conj(ref), q), rot);
    float inv[9], d[3] = {eye[4] - reference[4], eye[5] - reference[5], eye[6] - reference[6]};
    q_matrix(q_conj(ref), inv);
    m_vec(inv, d, offset);
    for (int i = 0; i < 3; i++) offset[i] *= units;
}

bool vr_rig_view(const VrRigAnchor *a, const float reference[7], const float eye[7], float units, float view[12]) {
    if (!(units > 0) || !isfinite(units)) return false;
    float rel[9], off[3], rw[9], p[3];
    vr_rig_relative(reference, eye, units, rel, off);
    m_mul(a->rotation, rel, rw);
    m_vec(a->rotation, off, p);
    for (int i = 0; i < 3; i++) p[i] += a->origin[i];
    for (int r = 0; r < 3; r++) {  // view = [Rw^T | -Rw^T p]
        view[r * 4 + 0] = rw[0 * 3 + r];
        view[r * 4 + 1] = rw[1 * 3 + r];
        view[r * 4 + 2] = rw[2 * 3 + r];
        view[r * 4 + 3] = -(rw[0 * 3 + r] * p[0] + rw[1 * 3 + r] * p[1] + rw[2 * 3 + r] * p[2]);
    }
    for (int i = 0; i < 12; i++)
        if (!isfinite(view[i])) return false;
    return true;
}

float vr_rig_camera_yaw(const float v[12]) { return atan2f(-v[8], -v[10]); }



// ================================================================== state
// Renderer thread: everything but the values the game thread reads (atomics).
VrRigShared g_rig;
static bool g_yaw_set;          // 1st person: g_yaw holds the anchor's facing
static float g_yaw;             // 1st person anchor facing (where the game camera looked on entry)
static float g_swim_raise;      // current eye height added to the eye point (swimming), eased
static bool g_water;            // this frame: the eye point / feet gap says Link is in water
static int g_water_hits_now, g_water_hits_before;  // Link's draws found at the eye point in water: this frame, the last
// The game camera from game memory is trusted while draws show it is live: some draw's view equalled it this frame
// or the previous one (in a cutscene the address might hold another camera than the one drawing).
static bool g_memory_seen_now, g_memory_seen_before;
// The memory camera matched a draw within the last MEMORY_TRUST_FRAMES frames. On the title screen it matches only
// every other frame or so (it holds the next camera while the slowly panning one is drawn); deciding per frame
// switched the eye cameras' and the culling's source every 2-3 frames, and islands / clouds flickered.
enum { MEMORY_TRUST_FRAMES = 60 };
static _Atomic uint64_t g_memory_seen_frame;  // (present count + 1; 0 never)
static bool memory_recent(void) {
    extern uint64_t renderer_present_count(void);
    const uint64_t f = atomic_load(&g_memory_seen_frame);
    return f && renderer_present_count() + 1 <= f + MEMORY_TRUST_FRAMES;
}

float vr_rig_units_per_metre(void) {
    static float env = -1;
    if (env < 0) {
        // SKYRECOMP_VR_SCALE now means internal render resolution. Preserve
        // compatibility with its former world-scale meaning only for values
        // in the documented units-per-metre range.
        const char *value = getenv("SKYRECOMP_VR_WORLD_SCALE");
        if (!value) value = getenv("SKYRECOMP_VR_SCALE");
        env = value ? (float)atof(value) : 0.0f;
        if (env < 10.0f || env > 1000.0f) env = 0.0f;
    }
    if (env > 0) return env;
    const float s = g_vr_config.world_scale;
    return s >= 10.0f && s <= 1000.0f ? s : 100.0f;
}

static float g_near_clip = FIRST_PERSON_NEAR;  // (VR Options > Near Clip: vr_rig_set_near_clip, from vr.c)
void vr_rig_set_near_clip(float units) {
    if (units >= 1.0f && units <= 100.0f) g_near_clip = units;
}

static float near_units(void) {  // (SKYRECOMP_VR_NEAR overrides the setting)
    static float env = -2;
    if (env < -1) env = getenv("SKYRECOMP_VR_NEAR") ? (float)atof(getenv("SKYRECOMP_VR_NEAR")) : -1;
    return env >= 0 ? env : g_near_clip;
}

void vr_rig_recenter(void) { g_yaw_set = false; }

void vr_rig_toggle_view(void) {
    g_vr_config.view_mode = g_vr_config.view_mode ? 0 : 1;
    g_yaw_set = false;
}

// Spawning (an area loaded, Link placed): for the first frames his eye point still holds a stale position (measured
// in his room: the view stood back by the wall, still, then jumped to him ~60 frames later). In 1st person the
// headset view stays black until his own body is drawn around the eye point (a skeleton bone within reach of it:
// the eye point is his) for SETTLE_FRAMES frames in a row, at most SETTLE_MAX frames: the load's own black screen
// lasts a little longer instead of showing the wrong viewpoint.
#define SETTLE_FRAMES 5
#define SETTLE_MAX 150
static bool g_settling;
static float g_head_world[3], g_head_offset[3];  // (vr_rig_is_link_draw) his head; the eye point minus it, usually
static uint64_t g_head_frame;                      // (present count + 1 when his head was last drawn)
static bool g_head_offset_ok;
static bool g_scripted;  // (vr_rig_begin_frame) his head has left the eye point: a scripted move (a door) plays
static uint64_t g_body_at_eye;  // (present count + 1 when his body was last drawn around the eye point)
bool vr_rig_settling(void) { return g_settling; }
bool vr_rig_game_hand(int side, float w[3]);

void vr_rig_begin_frame(void) {
    g_water_hits_before = g_water_hits_now;
    g_water_hits_now = 0;
    g_memory_seen_before = g_memory_seen_now;
    g_memory_seen_now = false;
    int mode = 0;
    float pos[3];
    {   // (RE, hle/player.c) SKYRECOMP_PLAYER_TRACE / SKYRECOMP_PLAYER_SNAP, once per frame
        extern void player_trace_tick(uint64_t frame);
        extern uint64_t renderer_present_count(void);
        player_trace_tick(renderer_present_count());
        extern void dispatch_trace_tick(uint64_t frame);
        dispatch_trace_tick(renderer_present_count());
        extern void door_track(void);
        door_track();
        static int door_trace = -1;  // (RE) SKYRECOMP_DOOR_TRACE=1: the game's hands in the door's frame, every 5 frames
        if (door_trace < 0) door_trace = getenv("SKYRECOMP_DOOR_TRACE") != NULL;
        extern bool door_matrix(float m[12]);
        float dm[12], h[3];
        if (door_trace && renderer_present_count() % 5 == 0 && door_matrix(dm)) {
            char line[256];
            int n = snprintf(line, sizeof line, "[door] frame %llu door %.1f %.1f %.1f x-axis %.2f %.2f",
                             (unsigned long long)renderer_present_count(), dm[3], dm[7], dm[11], dm[0], dm[8]);
            for (int side = 0; side < 2; side++)
                if (vr_rig_game_hand(side, h)) {
                    const float d[3] = {h[0] - dm[3], h[1] - dm[7], h[2] - dm[11]};
                    float l[3];  // door local: R^T d
                    for (int i = 0; i < 3; i++) l[i] = dm[0 * 4 + i] * d[0] + dm[1 * 4 + i] * d[1] + dm[2 * 4 + i] * d[2];
                    n += snprintf(line + n, sizeof line - n, " | %s hand local %.1f %.1f %.1f", side ? "R" : "L", l[0], l[1], l[2]);
                }
            fprintf(stderr, "%s\n", line);
        }
    }
    // Theatre: 1st person drops to the 3rd-person view while the game camera is far from Link's eye point. Link's
    // eye point stops updating while he rides his Loftwing, and cinematic cameras frame scenes from far away; the
    // view comes back once the camera follows Link again. Hysteresis keeps it from flickering at the edge.
    static bool theatre;
    float camera_distance = -1;
    bool have_eye = g_vr_config.view_mode == 1 && player_eye(pos);
    {   // (scripted moves: the eye follows his animated head - LINK_HEAD_DETACH)
        extern uint64_t renderer_present_count(void);
        g_scripted = false;
        if (have_eye && g_head_frame && g_head_frame + 2 >= renderer_present_count() + 1) {
            const float dx = g_head_world[0] - pos[0], dz = g_head_world[2] - pos[2], d = sqrtf(dx * dx + dz * dz);
            if (d < LINK_HEAD_ATTACHED) {
                for (int i = 0; i < 3; i++)
                    g_head_offset[i] = g_head_offset_ok ? g_head_offset[i] + (pos[i] - g_head_world[i] - g_head_offset[i]) * 0.1f
                                                        : pos[i] - g_head_world[i];
                g_head_offset_ok = true;
            } else if (g_head_offset_ok) {  // (eased in between: no jump as the head moves off)
                const float w = fminf(1.0f, (d - LINK_HEAD_ATTACHED) / (LINK_HEAD_DETACH - LINK_HEAD_ATTACHED));
                g_scripted = w > 0.5f;
                for (int i = 0; i < 3; i++) pos[i] += (g_head_world[i] + g_head_offset[i] - pos[i]) * w;
            }
        }
    }
    if (have_eye && g_rig.have_game_view) {
        const float *v = g_rig.game_view;
        float d2 = 0;
        for (int i = 0; i < 3; i++) {
            const float c = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);  // camera position = -R^T t
            d2 += (c - pos[i]) * (c - pos[i]);
        }
        camera_distance = sqrtf(d2);
        static int far_frames;  // (a camera cut for a frame or two doesn't switch the view)
        far_frames = camera_distance > THEATRE_ENTER ? far_frames + 1 : 0;
        if (far_frames >= THEATRE_FRAMES) theatre = true;
        else if (camera_distance < THEATRE_LEAVE) theatre = false;
    }
    if (!have_eye) theatre = false;
    {   // (spawn settling, above)
        extern uint64_t renderer_present_count(void);
        static bool had_eye;
        static int since, found;
        if (have_eye && !had_eye) since = found = 0;
        if (have_eye) {
            found = g_body_at_eye + 1 >= renderer_present_count() + 1 && g_body_at_eye ? found + 1 : 0;
            since++;
        }
        had_eye = have_eye;
        g_settling = have_eye && g_vr_config.view_mode == 1 && found < SETTLE_FRAMES && since < SETTLE_MAX;
    }
    if (have_eye && !theatre) {
        // (entering 1st person: facing where the game camera looked, i.e. where you were already looking)
        if (!g_yaw_set) { g_yaw = g_rig.have_game_view ? vr_rig_camera_yaw(g_rig.game_view) : 0.0f; g_yaw_set = true; }
        static int follow = -1;  // (test) SKYRECOMP_VR_FP_FOLLOW_CAMERA=1: faces the game camera's yaw every frame
        if (follow < 0) follow = getenv("SKYRECOMP_VR_FP_FOLLOW_CAMERA") != NULL;
        if (follow && g_rig.have_game_view) g_yaw = vr_rig_camera_yaw(g_rig.game_view);
        memcpy(g_rig.link, pos, sizeof pos);
        mode = 1;
        static float last_eye_y;
        static int still;
        still = pos[1] == last_eye_y ? still + 1 : 0;
        last_eye_y = pos[1];
        float feet = 0;
        g_water = still >= SWIM_STILL_FRAMES && player_feet_y(&feet) && pos[1] - feet > SWIM_EYE_ABOVE_FEET;
        const bool swimming = g_water && g_water_hits_before > 0;
        // Do not ease towards the water clearance: the intermediate positions are precisely where the reflection
        // breaks. Leaving the water can stay smooth because every intermediate point is already above its plane.
        if (swimming) g_swim_raise = SWIM_EYE_RAISE;
        else g_swim_raise += (0.0f - g_swim_raise) * 0.15f;
    }
    if (mode != 1) { g_yaw_set = false; g_swim_raise = 0; g_water = false; }  // (re-entering 1st person faces Link's direction again)
    // SKYRECOMP_VR_CAM_TRACE=1: when the memory camera starts / stops matching the draws, and the rig mode changes
    static int trace = -1, last_seen = -1, last_mode = -1;
    if (trace < 0) trace = getenv("SKYRECOMP_VR_CAM_TRACE") != NULL;
    extern uint64_t renderer_present_count(void);
    if (trace && camera_distance >= 0 && renderer_present_count() % 120 == 0) {
        float feet = NAN;
        player_feet_y(&feet);
        fprintf(stderr, "[vr-rig] frame %llu: game camera %.0f units from Link's eye; eye %.1f above his feet, eye raised %.1f\n",
                (unsigned long long)renderer_present_count(), camera_distance, pos[1] - feet, g_swim_raise);
    }
    if (trace && (last_seen != g_memory_seen_before || last_mode != mode)) {
        fprintf(stderr, "[vr-rig] frame %llu: memory camera %s, %s person%s\n", (unsigned long long)renderer_present_count(),
                g_memory_seen_before ? "matches the draws" : "NOT seen in the draws (fallback)", mode ? "1st" : "3rd",
                theatre ? " (theatre: game camera far from Link)" : "");
        last_seen = g_memory_seen_before;
        last_mode = mode;
    }
    g_rig.mode = mode;
}

void vr_rig_current_anchor(VrRigAnchor *a, const float *game_view) {
    if (g_rig.mode == 1) {
        const float eye[3] = {g_rig.link[0], g_rig.link[1] + g_vr_config.camera_height_offset + g_swim_raise, g_rig.link[2]};
        vr_rig_anchor_point(a, eye, g_yaw);
    } else {
        vr_rig_anchor_camera(a, game_view, g_vr_config.level_horizon, g_vr_config.camera_height_offset,
                             g_vr_config.camera_distance_offset);
    }
}

static bool same_view(const float a[12], const float b[12]) {
    for (int i = 0; i < 12; i++)
        if (fabsf(a[i] - b[i]) > ((i & 3) == 3 ? 1e-3f * (1.0f + fabsf(a[i])) : 1e-5f)) return false;
    return true;
}

static float det3(const float v[12]) {  // the 3x3 part of a row-major 3x4
    return v[0] * (v[5] * v[10] - v[6] * v[9]) - v[1] * (v[4] * v[10] - v[6] * v[8]) + v[2] * (v[4] * v[9] - v[5] * v[8]);
}

// How far a model matrix's 3x3 is from upright (its y axis the world's): the game places objects with yaw and
// scale only, so the model matrix that explains a block's view should be upright.
uint64_t g_vr_pass_id, g_vr_pass_epoch;  // (exec.c, CMD_SET_RENDER_TARGETS)

static float tilt(const float m[9]) {
    float s = 0;
    for (int i = 0; i < 9; i++) s = fmaxf(s, fabsf(m[i]));
    if (!(s > 0)) return 1e9f;
    return (fabsf(m[1]) + fabsf(m[3]) + fabsf(m[5]) + fabsf(m[7])) / s;
}

// A planar reflection block (mirror floors, lakes): view Vm = F Vg D M - the game camera Vg reflected in a
// horizontal plane (D = diag(1,-1,1) on world axes), the image turned upside down (F, the same on view axes), times
// a model matrix M. A normal block is Vm = Vg M. Whichever reading gives the upright M wins.
// Passes where a block's view was the game camera itself: the scene seen normally, never a reflection pass (whose
// camera is mirrored). Link's head turning in the void dream gave its block an orientation that read as "upright
// only when mirrored": the scene pass flipped to reflection for a few frames and his head was drawn above his body.
enum { DIRECT_PASSES = 16 };
static uint64_t g_direct_pass[DIRECT_PASSES];
static int g_direct_next;
static void note_direct_pass(void) {
    for (int i = 0; i < DIRECT_PASSES; i++)
        if (g_direct_pass[i] == g_vr_pass_id) return;
    g_direct_pass[g_direct_next] = g_vr_pass_id;
    g_direct_next = (g_direct_next + 1) % DIRECT_PASSES;
}
static bool direct_pass(void) {
    for (int i = 0; i < DIRECT_PASSES; i++)
        if (g_vr_pass_id && g_direct_pass[i] == g_vr_pass_id) return true;
    return false;
}

static bool planar_reflection(const float v[12], const float g[12]) {
    static int off = -1;  // SKYRECOMP_VR_NO_REFLECTION_FIX=1: the earlier behaviour
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_REFLECTION_FIX") != NULL;
    if (off || direct_pass()) return false;
    float plain[9], mirror[9];  // Vg^-1 Vm and D Vg^-1 F Vm (rotation parts; Vg^-1 = Vg^T)
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float p = 0, m = 0;
            for (int k = 0; k < 3; k++) {
                p += g[k * 4 + r] * v[k * 4 + c];
                m += g[k * 4 + r] * v[k * 4 + c] * (k == 1 ? -1.0f : 1.0f);
            }
            plain[r * 3 + c] = p;
            mirror[r * 3 + c] = m * (r == 1 ? -1.0f : 1.0f);
        }
    const float tp = tilt(plain), tm = tilt(mirror);
    // A block that tells (one of the readings upright, the other not) decides for its whole pass: the draws whose
    // model matrix isn't upright (or a level camera, where both readings agree) follow it. The pass's targets
    // remember the answer for the next frame, for the draws before the first deciding one.
    enum { KNOWN = 16 };
    static uint64_t known_id[KNOWN];
    static int8_t known_refl[KNOWN];
    static int next_slot;
    static uint64_t epoch = ~0ull;
    static int state;  // this pass: -1 plain, 1 reflection, 0 not decided
    int slot = -1;
    for (int i = 0; i < KNOWN; i++)
        if (known_id[i] == g_vr_pass_id) slot = i;
    if (epoch != g_vr_pass_epoch) { epoch = g_vr_pass_epoch; state = slot >= 0 ? known_refl[slot] : 0; }
    const int decided = tm < 0.02f && tp > 0.05f && tm < 0.25f * tp ? 1 : tp < 0.02f && tm > 0.05f && tp < 0.25f * tm ? -1 : 0;
    if (decided && decided != state) {
        state = decided;
        if (slot < 0 && decided > 0) slot = next_slot, next_slot = (next_slot + 1) % KNOWN, known_id[slot] = g_vr_pass_id;
        if (slot >= 0) known_refl[slot] = (int8_t)decided;
    }
    return state > 0;
}

static int g_hand_draw = -1;
static uint64_t g_hand_count_cur;  // (the hand draw's index count: which pose's mesh)
static bool hand_block(const VrRigAnchor *a, const float reference[7], const float game[12], float out[12]);
static void update_controller_poses(const VrRigAnchor *a, const float reference[7]);
static float g_hand_orig[12], g_hand_repl[12];
static bool g_hand_blocks;
static bool g_door_blocks;  // (vr_rig_door_draw) this draw is the door's leaf, turned: g_hand_orig -> g_hand_repl
static bool eye_camera(const float mono[28], const float reference[7], const float eye[7], const float fov[4],
                       float out[28], bool direct) {
#if defined(__APPLE__)
    (void)mono; (void)reference; (void)eye; (void)fov; (void)out;
    return false;
#else
    if (!mono || !sky_vr_is_scene_projection(mono, 28 * sizeof(float))) return false;
    // The game camera Vg. A block's view is Vg times the draw's model matrix (terrain chunks, the sky dome, Link's
    // skinned parts), so Vg comes from game memory; mirrored blocks (a reflection pass: its own camera) and frames
    // where memory doesn't match any draw fall back to the block's own view when that is a plain camera.
    float camera[12];
    const float *game;
    int source;
    static int no_memory = -1;  // SKYRECOMP_VR_NO_GAME_CAMERA=1: only the block's own view (the earlier behaviour)
    if (no_memory < 0) no_memory = getenv("SKYRECOMP_VR_NO_GAME_CAMERA") != NULL;
    const bool memory = !no_memory && game_camera_view(camera);
    if (memory && same_view(camera, mono + 16)) {
        extern uint64_t renderer_present_count(void);
        g_memory_seen_now = true;
        atomic_store(&g_memory_seen_frame, renderer_present_count() + 1);
        note_direct_pass();
    }
    // Without the memory camera (the title screen, file select, cutscenes: another camera draws), a plain camera
    // block of the same render pass (its targets; this frame or kept from an earlier one) stands for Vg in that
    // pass's blocks that are Vg times a model matrix: otherwise those (the sky dome, the cloud ring and sea) kept
    // the mono camera, stayed fixed to the head, and flickered whenever the memory camera matched for a frame.
    // (Other passes draw with other cameras: reflections, glare.)
    enum { PASS_CAMERAS = 16 };
    static struct { uint64_t pass; float view[12]; } pass_camera[PASS_CAMERAS];
    static int pass_camera_next;
    int pc = -1;
    for (int i = 0; i < PASS_CAMERAS; i++)
        if (pass_camera[i].pass == g_vr_pass_id) pc = i;
    if (memory && memory_recent() && det3(mono + 16) > 0) game = camera, source = 0;
    else if (sky_vr_is_scene_camera(mono, 28 * sizeof(float))) {
        if (!direct && pc >= 0 && fabsf(mono[5] - 5.0f) < .01f &&
            getenv("SKYRECOMP_VR_MODEL_CAMERA_TRACE") && !same_view(pass_camera[pc].view, mono + 16)) {
            extern uint64_t renderer_present_count(void);
            static uint64_t logged_frame = ~0ull;
            static int logged;
            const uint64_t frame_no = renderer_present_count();
            if (logged_frame != frame_no) { logged_frame = frame_no; logged = 0; }
            if (logged++ < 60) {
                fprintf(stderr, "[vr-model-camera] frame %llu pass %llu prior", (unsigned long long)frame_no,
                        (unsigned long long)g_vr_pass_id);
                for (int j = 0; j < 12; j++) fprintf(stderr, " %.8g", pass_camera[pc].view[j]);
                fprintf(stderr, " next");
                for (int j = 0; j < 12; j++) fprintf(stderr, " %.8g", mono[16 + j]);
                fprintf(stderr, "\n");
            }
        }
        game = mono + 16, source = 1;
        if (det3(mono + 16) > 0 && g_vr_pass_id) {
            if (pc < 0) pc = pass_camera_next, pass_camera_next = (pass_camera_next + 1) % PASS_CAMERAS;
            pass_camera[pc].pass = g_vr_pass_id;
            memcpy(pass_camera[pc].view, mono + 16, sizeof pass_camera[pc].view);
        }
    } else if (pc >= 0 && det3(mono + 16) > 0) game = pass_camera[pc].view, source = 2;
    else return false;
    static int trace = -1;  // SKYRECOMP_VR_CAM_TRACE=1: how often each source is used
    static long counts[3];
    if (trace < 0) trace = getenv("SKYRECOMP_VR_CAM_TRACE") != NULL;
    if (trace && ++counts[source] && (counts[0] + counts[1] + counts[2]) % 4000 == 0)
        fprintf(stderr, "[vr-rig] camera from game memory %ld, from the block %ld, from the last camera block %ld\n",
                counts[0], counts[1], counts[2]);
    memcpy(g_rig.game_view, game, sizeof g_rig.game_view);
    g_rig.have_game_view = true;
    VrRigAnchor a;
    vr_rig_current_anchor(&a, game);
    if (g_rig.mode == 1 && g_swim_raise > 0.0f) {
        // Room-scale tracking is applied after the anchor. A crouch can therefore cancel the fixed anchor raise and
        // put the real headset eye back through the surface. Move the anchor just enough that this eye (including
        // its tracked vertical offset) never gets closer than SWIM_EYE_RAISE to the game's surface point.
        float rel[9], off[3], world_off[3];
        vr_rig_relative(reference, eye, vr_rig_units_per_metre(), rel, off);
        m_vec(a.rotation, off, world_off);
        const float min_y = g_rig.link[1] + SWIM_EYE_RAISE;
        if (a.origin[1] + world_off[1] < min_y) a.origin[1] = min_y - world_off[1];
    }
    float view[12];
    if (!vr_rig_view(&a, reference, eye, vr_rig_units_per_metre(), view)) return false;
    if (source == 0) update_controller_poses(&a, reference);  // (on the real game camera only: vr_rig_controller_pose)
    // Link's hand: on its controller - its own view block replaced (game camera x the controller's hand frame), so
    // its lighting turns with it too
    float hand_mono[28];
    if (g_hand_draw >= 0 && g_scripted && getenv("SKYRECOMP_DOOR_HANDREL")) {
        // (RE) the game's own hand on the door's handle during its door sequence: the hand mesh's world matrix
        // (game view^-1 x its block) in the door leaf's frame (the leaf's matrix, animated by the game) - logged
        static uint64_t last;
        extern uint64_t renderer_present_count(void);
        extern bool door_matrix(float m[12]);
        float vi[12], w[12], dm[12], di[12], h[12];
        if (renderer_present_count() >= last + 5 && door_matrix(dm) && a_inverse(game, vi)) {
            last = renderer_present_count();
            a_mul(vi, mono + 16, w);
            a_rigid_inverse(dm, di);
            a_mul(di, w, h);
            fprintf(stderr, "[door-hand] frame %llu side %d mesh %llu:", (unsigned long long)last, g_hand_draw,
                    (unsigned long long)g_hand_count_cur);
            for (int j = 0; j < 12; j++) fprintf(stderr, " %.3f", h[j]);
            fprintf(stderr, "\n");
        }
    }
    if (g_hand_draw >= 0 && hand_block(&a, reference, game, hand_mono + 16)) {
        memcpy(hand_mono, mono, 16 * sizeof(float));
        memcpy(g_hand_orig, mono + 16, sizeof g_hand_orig);
        memcpy(g_hand_repl, hand_mono + 16, sizeof g_hand_repl);
        g_hand_blocks = true;
        mono = hand_mono;
    } else if (g_door_blocks) {  // (the door held open: its leaf's block turned - vr_rig_door_draw)
        memcpy(hand_mono, mono, 16 * sizeof(float));
        memcpy(hand_mono + 16, g_hand_repl, sizeof g_hand_repl);
        mono = hand_mono;
    }
    const float near = g_rig.mode == 1 ? near_units() : 0.0f;
    if (!direct && source != 1 && planar_reflection(mono + 16, game)) {
        // The block draws the reflection in a horizontal mirror: Vm = F Vg R (R the world's reflection in the plane,
        // F = diag(1,-1,1) turning the image upside down, as the game samples it). The eye's version is F Ve R, so
        // C = F Ve R Vm^-1 = (F Ve) (F Vg)^-1. Plain C = Ve Vg^-1 cancelled the mirror: the pass drew an ordinary eye
        // view, so the floor showed what lies under it (B400's dark water wedge) and lakes an offset panel.
        float fview[12], fgame[12];
        memcpy(fview, view, sizeof fview);
        memcpy(fgame, game, sizeof fgame);
        for (int i = 4; i < 8; i++) fview[i] = -fview[i], fgame[i] = -fgame[i];
        // (the image is upside down: the headset's uneven up / down field of view swaps, or the reflection's
        // bottom would lack what the floor samples there)
        const float ffov[4] = {fov[0], fov[1], -fov[3], -fov[2]};
        return sky_vr_camera_with_view(mono, fgame, fview, ffov, near, out);
    }
    return direct ? sky_vr_camera_direct(mono, view, fov, near, out)
                  : sky_vr_camera_with_view(mono, game, view, fov, near, out);
#endif
}

bool vr_rig_eye_camera(const float mono[28], const float reference[7], const float eye[7], const float fov[4],
                       float out[28]) {
    return eye_camera(mono, reference, eye, fov, out, false);
}

bool vr_rig_eye_camera_direct(const float mono[28], const float reference[7], const float eye[7], const float fov[4],
                              float out[28]) {
    return eye_camera(mono, reference, eye, fov, out, true);
}

typedef struct { uint64_t hash, count, size; } LinkMesh;
static LinkMesh g_link_mesh[LINK_MESH_CACHE];

// A cheap stable identity for immutable vertex data. Addresses alone may be recycled after an area load. Sampling
// the beginning, middle and end distinguishes meshes without hashing megabytes of vertices every draw.
static uint64_t mesh_hash(uint64_t addr, uint64_t size) {
    if (!addr || !size) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    uint64_t h = 1469598103934665603ull;
    const uint64_t sample = size < 64 ? size : 64;
    const uint64_t start[3] = {0, size > sample ? (size - sample) / 2 : 0, size > sample ? size - sample : 0};
    for (int s = 0; s < 3; s++)
        for (uint64_t i = 0; i < sample; i++) h = (h ^ p[start[s] + i]) * 1099511628211ull;
    return h;
}

static bool known_link_mesh(uint64_t hash, uint64_t count, uint64_t size) {
    for (int i = 0; i < LINK_MESH_CACHE; i++)
        if (g_link_mesh[i].hash == hash && g_link_mesh[i].count == count && g_link_mesh[i].size == size) return true;
    return false;
}

static void remember_link_mesh(uint64_t hash, uint64_t count, uint64_t size) {
    if (!hash || known_link_mesh(hash, count, size)) return;
    static int next;
    g_link_mesh[next] = (LinkMesh){hash, count, size};
    next = (next + 1) % LINK_MESH_CACHE;
}

// ---- 1st person: Link's hands on the controllers. His hands are their own rigid meshes (one per hand and pose,
// rooted at the wrist; measured: 744 vertices relaxed, a 28672-byte vertex buffer, within reach of his eye). Each
// hand draw is placed on its controller instead of the game's hand pose H: drawn through the world transform
// X = C K H^-1 (C: the controller's grip pose in the game world, K: the mesh in that grip - its fingers along the
// controller, its palm inward, the wrist behind the grip's centre). Only the drawing moves: the game's own hand and
// what it holds are unchanged. SKYRECOMP_VR_NO_HANDS=1: the game's hands as it places them.
#define HAND_MESH_VB 28672
#define HAND_REACH 130.0f
static bool g_grip_ok[2];
static float g_grip[2][7];     // (render thread) each controller's grip pose (OpenXR local space: x y z w, position)
// (g_hand_draw: the draw being recorded is Link's left (0) / right (1) hand, -1 none)
static struct { uint64_t vb, count; int side; } g_hand_mesh[8];  // (hand meshes learned: which side each is)
static float g_hand_last[2][3];   // each hand's last game position (game world) and the frame it was drawn
static uint64_t g_hand_last_frame[2];

// Physical doors (vr_input.c): each controller's grip in the game world (as drawn), and the hand holding a door's
// handle - drawn on the handle (hle/door.c) wherever the controller is.
static float g_ctrl_world[2][3];
static uint64_t g_ctrl_world_frame[2];
static _Atomic int g_door_grab = -1;
void vr_rig_door_grab(int side) { atomic_store(&g_door_grab, side); }
// (vr_input.c) a hand near enough to take a handle: drawn on it in the holding pose - the face (+1 / -1), 0: not
static _Atomic float g_door_hover[2];
void vr_rig_door_hover(int side, float face) { if (side >= 0 && side < 2) atomic_store(&g_door_hover[side], face); }
bool vr_rig_controller_world(int side, float w[3]) {
    extern uint64_t renderer_present_count(void);
    if (side < 0 || side > 1 || !g_ctrl_world_frame[side] || g_ctrl_world_frame[side] + 3 < renderer_present_count() + 1)
        return false;
    memcpy(w, g_ctrl_world[side], sizeof g_ctrl_world[side]);
    return true;
}

// Each controller's whole grip pose in the game world (row-major 3x4: the grip's axes - x right, y up, -z forward,
// the way a held sword points - and its position), refreshed by every eye camera (eye_camera below) from the same
// anchor the eyes use. For game hooks that need the hand as an object (runtime/hle/vr_sword.c).
static float g_ctrl_pose[2][12];
static uint64_t g_ctrl_pose_frame[2];
static pthread_mutex_t g_ctrl_pose_lock = PTHREAD_MUTEX_INITIALIZER;  // (written by the replay thread, read by the game)
bool vr_rig_controller_pose(int side, float c[12]) {
    extern uint64_t renderer_present_count(void);
    if (side < 0 || side > 1) return false;
    pthread_mutex_lock(&g_ctrl_pose_lock);
    const bool ok = g_ctrl_pose_frame[side] && g_ctrl_pose_frame[side] + 3 >= renderer_present_count() + 1;
    if (ok) memcpy(c, g_ctrl_pose[side], sizeof g_ctrl_pose[side]);
    pthread_mutex_unlock(&g_ctrl_pose_lock);
    return ok;
}
static void update_controller_poses(const VrRigAnchor *a, const float reference[7]) {
    extern uint64_t renderer_present_count(void);
    pthread_mutex_lock(&g_ctrl_pose_lock);
    for (int side = 0; side < 2; side++) {
        if (!g_grip_ok[side]) continue;
        float rel[9], off[3], rc[9], p[3];
        vr_rig_relative(reference, g_grip[side], vr_rig_units_per_metre(), rel, off);
        m_mul(a->rotation, rel, rc);
        m_vec(a->rotation, off, p);
        for (int i = 0; i < 3; i++) {
            g_ctrl_pose[side][i * 4 + 0] = rc[i * 3 + 0], g_ctrl_pose[side][i * 4 + 1] = rc[i * 3 + 1];
            g_ctrl_pose[side][i * 4 + 2] = rc[i * 3 + 2], g_ctrl_pose[side][i * 4 + 3] = p[i] + a->origin[i];
        }
        g_ctrl_pose_frame[side] = renderer_present_count() + 1;
    }
    pthread_mutex_unlock(&g_ctrl_pose_lock);
}

// Where the game puts Link's hand (game world, the wrist) - last drawn within 2 frames
bool vr_rig_game_hand(int side, float w[3]) {
    extern uint64_t renderer_present_count(void);
    if (side < 0 || side > 1 || !g_hand_last_frame[side] || g_hand_last_frame[side] + 2 < renderer_present_count() + 1)
        return false;
    memcpy(w, g_hand_last[side], sizeof g_hand_last[side]);
    return true;
}

void vr_rig_set_grips(bool left_ok, const float left[7], bool right_ok, const float right[7]) {
    g_grip_ok[0] = left_ok && left, g_grip_ok[1] = right_ok && right;
    if (g_grip_ok[0]) memcpy(g_grip[0], left, sizeof g_grip[0]);
    if (g_grip_ok[1]) memcpy(g_grip[1], right, sizeof g_grip[1]);
}
// ---- 1st person: what his hands hold (the sword, the shield). They're rigid draws of their own, posed by the game
// on his hand's joint; placed with the hand, they'd stay where the game animates them while the hand drawn goes to
// its controller. So each is drawn through the same world transform as the hand holding it: C K (H^-1 M), H^-1 M
// being the item relative to the game's hand - the game's own grip, nothing tuned. Held or not is read from the
// game's placement (measured in Skyloft, first person: a drawn sword's hilt sits 10.3-10.6 units from the hand
// holding it, a readied shield 10.8 from its hand; sheathed, on his back, both are 50+ units from either hand).
// SKYRECOMP_VR_NO_HELD=1: they stay where the game puts them.
#define HELD_REACH 25.0f
static const uint64_t HELD_COUNTS[] = {1056, 2352};  // (index counts: the sword, the shield)
enum { HELD_KINDS = sizeof HELD_COUNTS / sizeof HELD_COUNTS[0] };
static float g_hand_game[2][12];     // each hand's game view block (game camera x its model) and its frame
static uint64_t g_hand_game_frame[2];
static struct {
    float block[12];                 // this item's game view block, the frame it was drawn and the hand it's in
    uint64_t frame;
    int side;
    float rel[12];                   // H^-1 M: the item relative to that hand, and the frame it was measured
    uint64_t rel_frame;
} g_held[HELD_KINDS];
static bool g_held_on;               // (the draw being recorded is a held item: g_hand_draw its hand, g_held_k H^-1 M)
static float g_held_k[12];

static void held_measure(int k) {    // (the item and its hand drawn this frame: H^-1 M again)
    float hi[12];
    const int s = g_held[k].side;
    if (s < 0 || g_hand_game_frame[s] != g_held[k].frame || !a_inverse(g_hand_game[s], hi)) return;
    a_mul(hi, g_held[k].block, g_held[k].rel);
    g_held[k].rel_frame = g_held[k].frame;
}

// (g_hand_orig, g_hand_repl: this hand draw's game view block and the one replacing it)
void vr_rig_set_hand_draw(int side) { g_hand_draw = side, g_hand_blocks = false, g_door_blocks = false, g_held_on = false; }
// (exec.c) This hand draw's game view block and its replacement: other uniform blocks holding the same matrix (the
// shader's normals, for its lighting) get the replacement too.
bool vr_rig_hand_blocks(float orig[12], float repl[12]) {
    if (!g_hand_blocks || (g_hand_draw < 0 && !g_door_blocks)) return false;
    memcpy(orig, g_hand_orig, sizeof g_hand_orig);
    memcpy(repl, g_hand_repl, sizeof g_hand_repl);
    return true;
}
static int hand_side(const float *slot0, uint64_t count, uint64_t vb, uint64_t vb_size, const float e[3]);
int vr_rig_hand_side(const float *slot0, uint64_t size0, uint64_t count, uint64_t vb, uint64_t vb_size);

static bool hands_on(void) {
    static int off = -1;
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_HANDS") != NULL;
    return !off && g_rig.mode == 1 && g_rig.have_game_view;
}

// Which hand a rigid draw is (-1: not a hand). slot0 + 16: its view (game camera x its model); e: his eye, same space.
static int hand_side(const float *slot0, uint64_t count, uint64_t vb, uint64_t vb_size, const float e[3]) {
    if (vb_size != HAND_MESH_VB || count < 600 || count > 900) return -1;
    const float dx = slot0[19] - e[0], dy = slot0[23] - e[1], dz = slot0[27] - e[2];
    if (dx * dx + dy * dy + dz * dz > HAND_REACH * HAND_REACH) return -1;
    extern uint64_t renderer_present_count(void);
    const uint64_t now = renderer_present_count() + 1;
    const float *v = g_rig.game_view;
    float w[3];  // (its game position in the world: R^T (o - t))
    for (int i = 0; i < 3; i++)
        w[i] = v[0 * 4 + i] * (slot0[19] - v[3]) + v[1 * 4 + i] * (slot0[23] - v[7]) + v[2 * 4 + i] * (slot0[27] - v[11]);
    int side = -1;
    for (int i = 0; i < 8; i++)
        if (g_hand_mesh[i].vb == vb && g_hand_mesh[i].count == count) side = g_hand_mesh[i].side;
    if (side < 0) {
        // A new mesh (taking out an item or pulling a trigger swaps in another pose's mesh): the side not drawn yet
        // this frame, else the one whose last position is nearer; with neither known yet, the side of the game
        // camera it hangs on (measured: the mesh first seen on the camera's left is his right hand).
        const bool known[2] = {g_hand_last_frame[0] + 30 >= now && g_hand_last_frame[0], g_hand_last_frame[1] + 30 >= now && g_hand_last_frame[1]};
        const bool drawn[2] = {g_hand_last_frame[0] == now, g_hand_last_frame[1] == now};
        if (known[0] && known[1]) {
            float d2[2];
            for (int h = 0; h < 2; h++) {
                const float a0 = w[0] - g_hand_last[h][0], a1 = w[1] - g_hand_last[h][1], a2 = w[2] - g_hand_last[h][2];
                d2[h] = a0 * a0 + a1 * a1 + a2 * a2;
            }
            side = drawn[0] != drawn[1] ? (drawn[0] ? 1 : 0) : d2[0] <= d2[1] ? 0 : 1;
        } else side = dx < 0 ? 1 : 0;
        static int next;
        g_hand_mesh[next] = (__typeof__(g_hand_mesh[0])){vb, count, side};
        next = (next + 1) % 8;
    }
    memcpy(g_hand_last[side], w, sizeof w);
    g_hand_last_frame[side] = now;
    g_hand_count_cur = count;
    memcpy(g_hand_game[side], slot0 + 16, sizeof g_hand_game[side]);
    g_hand_game_frame[side] = now;
    for (int k = 0; k < HELD_KINDS; k++)  // (an item drawn before its hand this frame: measured now, used next frame)
        if (g_held[k].frame == now && g_held[k].side == side) held_measure(k);
    return side;
}

// (exec.c, a rigid draw that isn't a hand) The sword or shield in one of his hands: drawn with that hand, on its
// controller (vr_rig_set_hand_draw's state, the item's own offset kept). False: not held - the game's placement.
bool vr_rig_held_draw(const float *slot0, uint64_t size0, uint64_t count) {
    static int off = -1;
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_HELD") != NULL;
    if (off || !slot0 || !hands_on() || !sky_vr_is_scene_projection(slot0, size0)) return false;
    extern uint64_t renderer_present_count(void);
    const uint64_t now = renderer_present_count() + 1;
    const float *m = slot0 + 16;
    int k = -1;
    for (int i = 0; i < HELD_KINDS; i++)
        if (count == HELD_COUNTS[i]) k = i;
    if (k < 0) {  // (another part drawn on the held item's own joint - its exact block - goes with it)
        for (int i = 0; i < HELD_KINDS && k < 0; i++) {
            if (g_held[i].frame != now || g_held[i].side < 0) continue;
            bool same = true;
            for (int j = 0; same && j < 12; j++) same = fabsf(m[j] - g_held[i].block[j]) <= 1e-4f * (1.0f + fabsf(m[j]));
            if (same) k = i;
        }
        if (k < 0) return false;
    } else if (g_held[k].frame != now) {
        // which hand it's in: the nearest one drawn this frame or the last, within reach (block translations are
        // camera space: distances as in the world)
        int side = -1;
        float best = HELD_REACH * HELD_REACH;
        for (int s = 0; s < 2; s++) {
            if (!g_hand_game_frame[s] || g_hand_game_frame[s] + 1 < now) continue;
            const float dx = m[3] - g_hand_game[s][3], dy = m[7] - g_hand_game[s][7], dz = m[11] - g_hand_game[s][11];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) best = d2, side = s;
        }
        memcpy(g_held[k].block, m, sizeof g_held[k].block);
        g_held[k].frame = now;
        if (side != g_held[k].side) g_held[k].rel_frame = 0;  // (another hand: its offset measured afresh)
        g_held[k].side = side;
        held_measure(k);
    }
    const int side = g_held[k].side;
    if (side < 0 || !g_held[k].rel_frame || g_held[k].rel_frame + 2 < now) return false;
    if (atomic_load(&g_door_grab) == side || atomic_load(&g_door_hover[side]) != 0.0f) return false;
    float rel[12] = {0};
    if (count == HELD_COUNTS[k]) memcpy(rel, g_held[k].rel, sizeof rel);
    else {  // (a part on the item's joint: the same offset, its own block's relation to the item kept)
        float ii[12], part[12];
        if (!a_inverse(g_held[k].block, ii)) return false;
        a_mul(ii, m, part);
        a_mul(g_held[k].rel, part, rel);
    }
    g_hand_draw = side, g_hand_blocks = false, g_door_blocks = false, g_held_on = true;
    memcpy(g_held_k, rel, sizeof g_held_k);
    static int said;
    if (!said++) fprintf(stderr, "[vr] first person: Link's %s is on the %s controller\n", k ? "shield" : "sword", side ? "right" : "left");
    return true;
}

// K: the hand mesh in its controller's grip (as on the vr-port branch, measured from the game's rest pose). Both hand
// meshes are right-handed frames: fingers along +x, the thumb along +y (the right palm faces +z, the left -z), the
// wrist at the origin. OpenXR grip: -z forward, +y up. So for both hands: fingers forward, thumb up; the wrist 5.5 cm
// behind the grip's palm point. SKYRECOMP_VR_HAND_K=<rx>,<ry>,<rz>,<back cm>: an extra turn (degrees about the
// grip's x, y, z) and wrist distance - for tuning.
static void hand_k(int side, float k[12]) {
    (void)side;
    static const float grip[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};  // (columns: mesh x -> -z, y -> y, z -> x)
    static float adj[4] = {-90, 0, 0, 5.5f};  // (measured live: a quarter turn forward about x on top of vr-port's grip)
    static int read = -1;
    if (read < 0) {
        read = 1;
        const char *e = getenv("SKYRECOMP_VR_HAND_K");
        if (e) sscanf(e, "%f,%f,%f,%f", &adj[0], &adj[1], &adj[2], &adj[3]);
    }
    const float ax = adj[0] * (float)(M_PI / 180.0), ay = adj[1] * (float)(M_PI / 180.0), az = adj[2] * (float)(M_PI / 180.0);
    const float rx[9] = {1, 0, 0, 0, cosf(ax), -sinf(ax), 0, sinf(ax), cosf(ax)};
    const float ry[9] = {cosf(ay), 0, sinf(ay), 0, 1, 0, -sinf(ay), 0, cosf(ay)};
    const float rz[9] = {cosf(az), -sinf(az), 0, sinf(az), cosf(az), 0, 0, 0, 1};
    float t1[9], t2[9], r[9];
    m_mul(rz, ry, t1);
    m_mul(t1, rx, t2);
    m_mul(t2, grip, r);
    const float back = adj[3] * 0.01f * vr_rig_units_per_metre();
    for (int i = 0; i < 3; i++) k[i * 4 + 0] = r[i * 3 + 0], k[i * 4 + 1] = r[i * 3 + 1], k[i * 4 + 2] = r[i * 3 + 2];
    k[3] = 0, k[7] = 0, k[11] = back;
}

// (exec.c) Which hand a rigid draw is: 0 left, 1 right, -1 none
int vr_rig_hand_side(const float *slot0, uint64_t size0, uint64_t count, uint64_t vb, uint64_t vb_size) {
    if (!slot0 || !hands_on() || !sky_vr_is_scene_projection(slot0, size0)) return -1;
    const float *v = g_rig.game_view;
    float e[3];
    for (int r = 0; r < 3; r++) e[r] = v[r * 4] * g_rig.link[0] + v[r * 4 + 1] * g_rig.link[1] + v[r * 4 + 2] * g_rig.link[2] + v[r * 4 + 3];
    return hand_side(slot0, count, vb, vb_size, e);
}

// The view block a hand draw is lit and placed by: game camera x C K (its normals then turn with the controller;
// the projection stays the eye's, as for every draw). False: leave the draw as the game places it.
static bool hand_block(const VrRigAnchor *a, const float reference[7], const float game[12], float out[12]) {
    const int side = g_hand_draw;
    // (during a scripted move his hands stay where the game animates them: on the door's handle)
    if (side < 0 || side > 1 || !g_grip_ok[side] || !hands_on() || g_scripted) return false;

    float rel[9], off[3], rc[9], c[12], k[12], ck[12];
    vr_rig_relative(reference, g_grip[side], vr_rig_units_per_metre(), rel, off);
    m_mul(a->rotation, rel, rc);
    float p[3];
    m_vec(a->rotation, off, p);
    for (int i = 0; i < 3; i++) {
        c[i * 4 + 0] = rc[i * 3 + 0], c[i * 4 + 1] = rc[i * 3 + 1], c[i * 4 + 2] = rc[i * 3 + 2];
        c[i * 4 + 3] = p[i] + a->origin[i];
    }
    {
        extern uint64_t renderer_present_count(void);
        for (int i = 0; i < 3; i++) g_ctrl_world[side][i] = c[i * 4 + 3];
        g_ctrl_world_frame[side] = renderer_present_count() + 1;
        extern bool door_handle_hand(float w[3]);
        float h[3];
        if (atomic_load(&g_door_grab) == side && door_handle_hand(h))  // (holding the door: the hand on its handle)
            for (int i = 0; i < 3; i++) c[i * 4 + 3] = h[i];
    }
    if (g_held_on) {  // (a held item: the hand's frame, then the item where the game has it in that hand)
        float ckh[12];
        hand_k(side, k);
        a_mul(c, k, ck);
        a_mul(ck, g_held_k, ckh);
        a_mul(game, ckh, out);
        return true;
    }
    {   // holding the door's handle, or near enough to take it (hovering): the hand as the game's own door sequence
        // holds it - its rotation measured there (the left hand on the face Link stood at, in the door's frame), mirrored
        // for the other face (half a turn about the door's vertical) and for the right hand (the door's x mirrored, and
        // the mesh's z - the right mesh is the left one mirrored) - turned with the leaf; the fingers at the knob
        static const float R_LEFT[9] = {0.133f, -0.407f, -0.903f, -0.948f, 0.215f, -0.236f, 0.290f, 0.887f, -0.357f};
        const int grab = atomic_load(&g_door_grab);
        const float hover = atomic_load(&g_door_hover[side]);
        extern bool door_handle_pose(float face, float knob[3], float *face_out, float axes[9]);
        float knob[3], face, ax[9];
        if ((grab == side || hover != 0.0f) && door_handle_pose(grab == side ? 0.0f : hover, knob, &face, ax)) {
            // (the mesh's axes - SKYRECOMP_DOOR_HAND_AXES=<fingers>,<palm>: 1 x, 2 y, 3 z, negative for the other way
            // - default the mesh as hand_k describes it: fingers +x, the right palm +z, the left -z): fingers up, the palm
            // to the door; the third axis completes a right-handed frame
            static int fa = 0, pa = 0;
            if (!fa) {
                fa = 1, pa = 3;
                const char *e = getenv("SKYRECOMP_DOOR_HAND_AXES");
                if (e) sscanf(e, "%d,%d", &fa, &pa);
            }
            (void)R_LEFT;
            float nface[3];
            for (int q = 0; q < 3; q++) nface[q] = ax[q * 3 + 2] * face;
            const float upw[3] = {0, 1, 0};
            float tow[3] = {-nface[0], -nface[1], -nface[2]};  // (toward the door)
            const int pal = side == 1 ? pa : -pa;              // (the left palm is the mirrored axis)
            float col[3][3] = {{0}};
            const int fi = abs(fa) - 1, pi = abs(pal) - 1, ti = 3 - fi - pi;
            for (int q = 0; q < 3; q++) col[fi][q] = upw[q] * (fa < 0 ? -1.0f : 1.0f), col[pi][q] = tow[q] * (pal < 0 ? -1.0f : 1.0f);
            {   // (the third: the cross product of the next two in order, x = y z, y = z x, z = x y)
                const float *a1 = col[(ti + 1) % 3], *b1 = col[(ti + 2) % 3];
                col[ti][0] = a1[1] * b1[2] - a1[2] * b1[1], col[ti][1] = a1[2] * b1[0] - a1[0] * b1[2], col[ti][2] = a1[0] * b1[1] - a1[1] * b1[0];
            }
            float wr[9];
            for (int q = 0; q < 3; q++) wr[q * 3 + 0] = col[0][q], wr[q * 3 + 1] = col[1][q], wr[q * 3 + 2] = col[2][q];
            const float u = vr_rig_units_per_metre();
            float n[3];  // (out of the held face, in the world)
            for (int q = 0; q < 3; q++) n[q] = ax[q * 3 + 2] * face;
            float w[12];
            for (int q = 0; q < 3; q++) {
                w[q * 4 + 0] = wr[q * 3 + 0], w[q * 4 + 1] = wr[q * 3 + 1], w[q * 4 + 2] = wr[q * 3 + 2];
                w[q * 4 + 3] = knob[q] - (q == 1 ? 0.07f * u : 0.0f) + n[q] * 0.03f * u;
            }
            a_mul(game, w, out);
            return true;
        }
    }
    hand_k(side, k);
    a_mul(c, k, ck);
    a_mul(game, ck, out);
    return true;
}

bool vr_rig_is_link_draw(const float *slot0, uint64_t size0, const float *bones, uint64_t bone_bytes,
                         uint64_t count, uint64_t vb, uint64_t vb_size) {
#if defined(__APPLE__)
    (void)slot0; (void)size0; (void)bones; (void)bone_bytes; (void)count; (void)vb; (void)vb_size;
    return false;
#else
    static int show = -1;
    if (show < 0) show = getenv("SKYRECOMP_VR_SHOW_LINK") != NULL;
    if (show || g_rig.mode != 1 || !g_rig.have_game_view || !slot0) return false;
    if (!sky_vr_is_scene_projection(slot0, size0)) return false;  // (the shadow pass keeps Link: his shadow stays)
    // Link's eye in game-camera space; his bones sit around it (head to feet), any other character's farther away.
    const float *v = g_rig.game_view;
    float e[3];
    for (int r = 0; r < 3; r++) e[r] = v[r * 4] * g_rig.link[0] + v[r * 4 + 1] * g_rig.link[1] + v[r * 4 + 2] * g_rig.link[2] + v[r * 4 + 3];
    // Rigid draws: only the tiny ones at the eye point are Link's (his eyes, measured in his room: four of 42 vertices
    // there); equipment and effects elsewhere can't be told from Link by proximity and stay.
    if (!bones || bone_bytes < 48) {
        const float dx = slot0[19] - e[0], dy = slot0[23] - e[1], dz = slot0[27] - e[2];
        if (count <= LINK_EYE_PART_MAX_VERTS && dx * dx + dy * dy + dz * dz < LINK_EYE_PART_RADIUS * LINK_EYE_PART_RADIUS)
            return true;
        // His belt's rigid pieces (measured: 12 and 216 vertices, 27 and 48 units below the eye point): on his
        // centre line, between chest and knees - part of his body. (His hands hang ~25 units to the sides.)
        float w[3];  // the offset in the world (view = R | t: R^T d)
        for (int i = 0; i < 3; i++) w[i] = v[0 * 4 + i] * dx + v[1 * 4 + i] * dy + v[2 * 4 + i] * dz;
        const bool belt = w[0] * w[0] + w[2] * w[2] < LINK_BELT_RADIUS * LINK_BELT_RADIUS && w[1] < -15.0f && w[1] > -110.0f;
        return belt;
    }
    const int n = (int)(bone_bytes / 48);
    float nearest = 1e30f;
    for (int i = 0; i < n; i++) {
        const float *b = bones + i * 12;
        const float dx = b[3] - e[0], dy = b[7] - e[1], dz = b[11] - e[2];
        nearest = fminf(nearest, dx * dx + dy * dy + dz * dz);
    }
    // Head-only palettes (his face: 44 matrices, his hair: 24), all at the eye point: hidden in both settings, they
    // would fill the view. (Held items have small palettes too, but at his hands, not his eye.)
    if (n < LINK_SKELETON_MIN_PALETTE) {
        if (n < LINK_HEAD_MIN_PALETTE) return false;
        // (remembered: in scripted moves - a door - his animated head leaves the eye point, which stays put)
        const bool near = nearest < LINK_HEAD_RADIUS * LINK_HEAD_RADIUS;
        const uint64_t hash = mesh_hash(vb, vb_size);
        const bool known = known_link_mesh(hash, count, vb_size);
        if (near && !known) remember_link_mesh(hash, count, vb_size);
        if (!near && !known) return false;
        if (n >= LINK_FACE_MIN_PALETTE) {  // where his head is (his face's bones, in the world: R^T (c - t))
            float c[3] = {0, 0, 0};
            for (int i = 0; i < n; i++) c[0] += bones[i * 12 + 3], c[1] += bones[i * 12 + 7], c[2] += bones[i * 12 + 11];
            for (int r = 0; r < 3; r++) c[r] = c[r] / n - v[r * 4 + 3];
            for (int i = 0; i < 3; i++) g_head_world[i] = v[0 * 4 + i] * c[0] + v[1 * 4 + i] * c[1] + v[2 * 4 + i] * c[2];
            extern uint64_t renderer_present_count(void);
            g_head_frame = renderer_present_count() + 1;
        }
        return true;
    }
    // His body: the whole skeleton's palette with a bone near his eye
    const uint64_t hash = mesh_hash(vb, vb_size);
    bool body = known_link_mesh(hash, count, vb_size);
    if (!body && nearest < LINK_BONE_RADIUS * LINK_BONE_RADIUS) remember_link_mesh(hash, count, vb_size), body = true;
    if (nearest < LINK_BONE_RADIUS * LINK_BONE_RADIUS) {  // (his body at the eye point: spawn settling)
        extern uint64_t renderer_present_count(void);
        g_body_at_eye = renderer_present_count() + 1;
    }
    if (body && g_water) g_water_hits_now++;
    return body;
#endif
}

// ---- culling: the game culls with its own camera; in VR it has to cover the union of both eyes' frusta, widened by
// how far the eyes point away from the game camera (a turned head). Kept below 80 degrees
// per side (the culling math needs a finite tangent); turning further than that can still cull at the edges.
static _Atomic float g_cull_fovy, g_cull_aspect;
static _Atomic uint64_t g_cull_frame;
static pthread_mutex_t g_cull_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_cull_view[12], g_cull_tan[4];  // (under g_cull_lock)
static uint64_t g_cull_view_frame;

// Whether the cull view stands on the real game camera (from game memory, matched by the draws: gameplay). On the
// title / file select another camera draws and the rig's camera is a substitute (a pass's plain camera block): the
// headset frustum built on it culled objects the eyes were looking at (islands flickering, vanishing).
bool vr_rig_cull_trusted(void) { return memory_recent(); }

// How far the eyes look turned from the game camera, about the vertical (radians, the sense of atan2(x, z) of the
// forward directions): the left stick is turned by it (JoyConTranslator), so "forward" walks where the player looks.
static _Atomic float g_view_yaw;
float vr_rig_view_yaw(void) { return memory_recent() ? atomic_load(&g_view_yaw) : 0.0f; }

// The same for a controller (its OpenXR orientation q, in the space of the head reference): where it points, turned
// from the game camera. False when it points too steeply up or down for a heading (or without the game camera).
bool vr_rig_pointing_yaw(const float reference[4], const float q[4], float *yaw) {
    if (!memory_recent() || !g_rig.have_game_view) return false;
    VrRigAnchor a;
    vr_rig_current_anchor(&a, g_rig.game_view);
    const float ref7[7] = {reference[0], reference[1], reference[2], reference[3], 0, 0, 0};
    const float q7[7] = {q[0], q[1], q[2], q[3], 0, 0, 0};
    float rel[9], off[3], rw[9], f[3];
    vr_rig_relative(ref7, q7, 1.0f, rel, off);
    m_mul(a.rotation, rel, rw);
    m_vec(rw, (const float[3]){0, 0, -1}, f);  // (the aim pose points along its -z)
    const float *v = g_rig.game_view;
    const float c[3] = {-v[8], -v[9], -v[10]};
    if (f[0] * f[0] + f[2] * f[2] < 0.1f || c[0] * c[0] + c[2] * c[2] < 0.01f) return false;
    *yaw = remainderf(atan2f(f[0], f[2]) - atan2f(c[0], c[2]), 2.0f * (float)M_PI);
    return true;
}

bool vr_rig_cull_view(float view[12], float tangents[4]) {
    extern uint64_t renderer_present_count(void);
    pthread_mutex_lock(&g_cull_lock);
    const bool ok = g_cull_view_frame && renderer_present_count() + 1 <= g_cull_view_frame + 30;
    if (ok) memcpy(view, g_cull_view, sizeof g_cull_view), memcpy(tangents, g_cull_tan, sizeof g_cull_tan);
    pthread_mutex_unlock(&g_cull_lock);
    return ok;
}

void vr_rig_update_cull(const float reference[7], const float eye0[7], const float fov0[4], const float eye1[7],
                        const float fov1[4]) {
    (void)eye1;
    if (!g_rig.have_game_view) return;
    const float tl = fminf(tanf(fov0[0]), tanf(fov1[0])), tr = fmaxf(tanf(fov0[1]), tanf(fov1[1]));
    const float tu = fmaxf(tanf(fov0[2]), tanf(fov1[2])), td = fminf(tanf(fov0[3]), tanf(fov1[3]));
    float half_h = atanf(fmaxf(-tl, tr)), half_v = atanf(fmaxf(tu, -td));
    VrRigAnchor a;
    vr_rig_current_anchor(&a, g_rig.game_view);
    float rel[9], off[3], rw[9], f[3];
    vr_rig_relative(reference, eye0, 1.0f, rel, off);
    m_mul(a.rotation, rel, rw);
    m_vec(rw, (const float[3]){0, 0, -1}, f);
    const float c[3] = {-g_rig.game_view[8], -g_rig.game_view[9], -g_rig.game_view[10]};
    const float yaw_signed = remainderf(atan2f(f[0], f[2]) - atan2f(c[0], c[2]), 2.0f * (float)M_PI);
    const float yaw_off = fabsf(yaw_signed);
    if (f[0] * f[0] + f[2] * f[2] > 0.01f && c[0] * c[0] + c[2] * c[2] > 0.01f) atomic_store(&g_view_yaw, yaw_signed);
    const float pitch_off = fabsf(asinf(fmaxf(-1, fminf(1, f[1]))) - asinf(fmaxf(-1, fminf(1, c[1]))));
    const float margin = 5.0f * (float)(M_PI / 180.0), limit = 80.0f * (float)(M_PI / 180.0);
    half_h = fminf(half_h + yaw_off + margin, limit);
    half_v = fminf(half_v + pitch_off + margin, limit);
    {   // eye 0's view in the game world and both eyes' field (vr_rig_cull_view, runtime/hle/vr_cull.c)
        float view[12];
        if (vr_rig_view(&a, reference, eye0, vr_rig_units_per_metre(), view)) {
            extern uint64_t renderer_present_count(void);
            pthread_mutex_lock(&g_cull_lock);
            memcpy(g_cull_view, view, sizeof view);
            g_cull_tan[0] = tl, g_cull_tan[1] = tr, g_cull_tan[2] = td, g_cull_tan[3] = tu;
            g_cull_view_frame = renderer_present_count() + 1;
            pthread_mutex_unlock(&g_cull_lock);
        }
    }
    if (!isfinite(half_h) || !isfinite(half_v)) return;
    extern uint64_t renderer_present_count(void);
    atomic_store(&g_cull_fovy, 2.0f * half_v);
    atomic_store(&g_cull_aspect, tanf(half_h) / tanf(half_v));
    atomic_store(&g_cull_frame, renderer_present_count() + 1);
}

bool vr_rig_cull_fov(float *fovy_radians, float *aspect) {
    extern uint64_t renderer_present_count(void);
    const uint64_t at = atomic_load(&g_cull_frame);
    if (!at || renderer_present_count() + 1 > at + 30) return false;  // (not in VR, or no stereo frame lately)
    if (fovy_radians) *fovy_radians = atomic_load(&g_cull_fovy);
    if (aspect) *aspect = atomic_load(&g_cull_aspect);
    return true;
}

// (RE, exec.c) SKYRECOMP_DOOR_TRACE=1: every 60 frames at a door, the draws whose model (rigid: its translation;
// skinned: its first bone) lies within 150 units of the door's origin - to tell its leaf's draw
void vr_rig_door_probe(const float *slot0, uint64_t size0, bool skinned, const float *bones, uint64_t bone_bytes,
                       uint64_t count, uint64_t vb, uint64_t vb_size) {
    static int on = -1;
    if (on < 0) on = getenv("SKYRECOMP_DOOR_TRACE") != NULL;
    extern uint64_t renderer_present_count(void);
    extern bool door_matrix(float m[12]);
    float dm[12];
    if (!on || renderer_present_count() % 20 || !g_rig.have_game_view || size0 < 112 || !door_matrix(dm)) return;
    const float *v = g_rig.game_view;
    if (skinned && bones && bone_bytes >= 48) {  // (any bone near the door: its nearest, and how many bones)
        const int nb = (int)(bone_bytes / 48);
        float best = 1e30f, bw[3] = {0, 0, 0};
        for (int k = 0; k < nb && k < 512; k++) {
            const float *bk = bones + k * 12;
            float ww[3];
            for (int i = 0; i < 3; i++)
                ww[i] = v[0 * 4 + i] * (bk[3] - v[3]) + v[1 * 4 + i] * (bk[7] - v[7]) + v[2 * 4 + i] * (bk[11] - v[11]);
            const float ex = ww[0] - dm[3], ey = ww[1] - dm[7], ez = ww[2] - dm[11], d2 = ex * ex + ey * ey + ez * ez;
            if (d2 < best) best = d2, memcpy(bw, ww, sizeof bw);
        }
        if (best < 150 * 150 && nb < 100)
            fprintf(stderr, "[door-draw] frame %llu skinned count %llu vb %#llx size %llu bones %d nearest %.1f\n",
                    (unsigned long long)renderer_present_count(), (unsigned long long)count, (unsigned long long)vb,
                    (unsigned long long)vb_size, nb, sqrtf(best));
        return;
    }
    const float *t = NULL;
    const float c[3] = {t ? t[3] : slot0[19], t ? t[7] : slot0[23], t ? t[11] : slot0[27]};  // (camera space)
    float w[3];
    for (int i = 0; i < 3; i++) w[i] = v[0 * 4 + i] * (c[0] - v[3]) + v[1 * 4 + i] * (c[1] - v[7]) + v[2 * 4 + i] * (c[2] - v[11]);
    const float dx = w[0] - dm[3], dy = w[1] - dm[7], dz = w[2] - dm[11];
    if (dx * dx + dy * dy + dz * dz > 150 * 150) return;
    if (count != 276) return;
    // its rotation in the world (R_view^T x the block's 3x3): the x axis' level components
    float rx[3];
    for (int i = 0; i < 3; i++) rx[i] = v[0 * 4 + i] * slot0[16] + v[1 * 4 + i] * slot0[20] + v[2 * 4 + i] * slot0[24];
    fprintf(stderr, "[door-draw] frame %llu leaf? x-axis %.3f %.3f %.3f size0 %llu\n", (unsigned long long)renderer_present_count(),
            rx[0], rx[1], rx[2], (unsigned long long)size0);
    fprintf(stderr, "[door-draw] frame %llu %s count %llu vb %#llx size %llu bones %llu at %.1f %.1f %.1f (from door %.1f %.1f %.1f)\n",
            (unsigned long long)renderer_present_count(), skinned ? "skinned" : "rigid", (unsigned long long)count,
            (unsigned long long)vb, (unsigned long long)vb_size, (unsigned long long)(bone_bytes / 48), w[0], w[1], w[2], dx, dy, dz);
}

// (exec.c, rigid draws) The door Link holds open (hle/door.c door_open): its leaf's draw - a rigid draw at the door's
// origin, turning about it in the game's own sequence - turned by the open angle about its own vertical: the block
// (game view x model) times Ry. Where the game's sequence has it open wider, the game's stays.
void vr_rig_door_draw(const float *slot0, uint64_t size0) {
    extern bool door_matrix(float m[12]);
    extern float door_open(void);
    float dm[12];
    const float open = door_open();
    if (open == 0.0f || !g_rig.have_game_view || size0 < 112 || !door_matrix(dm)) return;
    const float *v = g_rig.game_view, *b = slot0 + 16;
    float w[3], x[3];  // its origin and x axis in the world: R_view^T (t - t_view), R_view^T x
    for (int i = 0; i < 3; i++) {
        w[i] = v[0 * 4 + i] * (b[3] - v[3]) + v[1 * 4 + i] * (b[7] - v[7]) + v[2 * 4 + i] * (b[11] - v[11]);
        x[i] = v[0 * 4 + i] * b[0] + v[1 * 4 + i] * b[4] + v[2 * 4 + i] * b[8];
    }
    const float dx = w[0] - dm[3], dy = w[1] - dm[7], dz = w[2] - dm[11];
    if (dx * dx + dy * dy + dz * dz > 4.0f || fabsf(x[1]) > 0.01f) return;
    // the game's own angle (its x axis from the closed door's): kept when wider
    // (the closed door's x axis in the world is its matrix's first column; angles as atan2(-z, x), as Ry turns them)
    float game = atan2f(-x[2], x[0]) - atan2f(-dm[8], dm[0]);
    while (game > (float)M_PI) game -= 2 * (float)M_PI;
    while (game < -(float)M_PI) game += 2 * (float)M_PI;
    {   // (diagnostics) the leaf's own angle against the door's, as it changes
        static float last = 1e9f;
        if (fabsf(game - last) > 0.05f) fprintf(stderr, "[door] leaf draw at %.0f deg from the door's matrix\n", game * 57.2958f), last = game;
    }
    if (fabsf(game) >= fabsf(open)) return;
    const float c = cosf(open - game), s = sinf(open - game);
    extern float door_hinge_x(void);
    const float hx = door_hinge_x();
    float r[12];
    for (int row = 0; row < 3; row++) {
        const float *br = b + row * 4;
        r[row * 4 + 0] = br[0] * c - br[2] * s;  // B * Ry: [c 0 s; 0 1 0; -s 0 c]
        r[row * 4 + 1] = br[1];
        r[row * 4 + 2] = br[0] * s + br[2] * c;
        // (about the hinge h = (hx, 0, 0) in the leaf's frame, not its origin: t += B (h - Ry h))
        r[row * 4 + 3] = br[3] + br[0] * hx * (1.0f - c) + br[2] * hx * s;
    }
    memcpy(g_hand_orig, b, sizeof g_hand_orig);
    memcpy(g_hand_repl, r, sizeof g_hand_repl);
    g_hand_blocks = g_door_blocks = true;
}

