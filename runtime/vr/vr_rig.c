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
#define LINK_HAIR_RADIUS 45.0f    // (a head-sized palette whose bones centre this close to his face's: his hair)
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
    // The game's own camera, as theatre, while it shows what 1st person can't (runtime/hle/player.c); 1st person again
    // after:
    //  - the Beetle in flight: its camera follows it - until it is caught (SKYRECOMP_VR_BEETLE_FP=1 stays in 1st person);
    //  - the boss door's key puzzle: its close-up of the key and the lock - until he backs out or the door opens
    //    (SKYRECOMP_VR_BOSS_LOCK_FP=1 stays in 1st person).
    {
        extern bool player_beetle_flying(void), player_boss_lock(void);
        static int keep_beetle = -1, keep_lock = -1;
        if (keep_beetle < 0) keep_beetle = getenv("SKYRECOMP_VR_BEETLE_FP") != NULL;
        if (keep_lock < 0) keep_lock = getenv("SKYRECOMP_VR_BOSS_LOCK_FP") != NULL;
        const bool now_beetle = have_eye && !keep_beetle && player_beetle_flying();
        const bool now_lock = have_eye && !keep_lock && player_boss_lock();
        // (held through a few frames that read it as over: one bad read of Link's actor didn't bring 1st person back
        // mid-flight)
        static int miss;
        static bool kept, kept_lock;
        if (now_beetle || now_lock) kept = true, kept_lock = now_lock, miss = 0;
        else if (kept && ++miss > 4) kept = false;
        const bool beetle = kept && have_eye;
        static bool was;
        if (beetle != was)
            fprintf(stderr, "[vr-rig] %s: %s\n",
                    kept_lock ? (beetle ? "the boss door's lock examined" : "the boss door's lock left")
                              : (beetle ? "the Beetle launched" : "the Beetle caught"),
                    beetle ? "the game's camera" : "1st person again");
        was = beetle;
        static bool beetle_theatre;  // (leaving: back to 1st person unless the camera is far for its own reasons)
        if (beetle) theatre = true, beetle_theatre = true;
        else if (beetle_theatre) {
            if (camera_distance < THEATRE_ENTER) theatre = false;
            beetle_theatre = false;
        }
    }
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
static bool g_skin_on;  // (vr_rig_held_skin_draw: the draw being recorded is the Slingshot, moved with its hand)
static float g_skin_T[12];
static float g_sling_b0[12], g_sling_rel[12];  // (the Slingshot's first bone this frame; it relative to its hand)
static uint64_t g_sling_frame;
static int g_sling_side = -1;
static bool g_sling_rel_ok;
static void beetle_measure(void);  // (the Beetle on his wrist: vr_rig_beetle_draw)
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
    if (g_skin_on) {  // (the Slingshot: its view block moved with its hand too - its bones are, in exec.c)
        memcpy(hand_mono, mono, 16 * sizeof(float));
        a_mul(g_skin_T, mono + 16, hand_mono + 16);
        memcpy(g_hand_orig, mono + 16, sizeof g_hand_orig);
        memcpy(g_hand_repl, hand_mono + 16, sizeof g_hand_repl);
        g_hand_blocks = true;
        mono = hand_mono;
    } else if (g_hand_draw >= 0 && hand_block(&a, reference, game, hand_mono + 16)) {
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

// A remembered mesh drawn this far from his eye point is not his: an enemy that once came close enough to be taken for
// him (every enemy of its kind shares the mesh, so all of them vanished, even looked at). Forgotten; his own is learnt
// again the next time it is drawn at him. (Mirrored draws - reflections - don't count.)
#define LINK_FORGET_DISTANCE 500.0f
// His body's and head's bones centre on him: their centre within this far sideways of his eye point. An enemy close
// enough to have a bone within LINK_BONE_RADIUS of his eye still centres a body-width or more away.
#define LINK_BODY_CENTRE 40.0f
#define LINK_HEAD_CENTRE 45.0f

static void forget_link_mesh(uint64_t hash, uint64_t count, uint64_t size, float distance) {
    for (int i = 0; i < LINK_MESH_CACHE; i++)
        if (g_link_mesh[i].hash == hash && g_link_mesh[i].count == count && g_link_mesh[i].size == size) {
            g_link_mesh[i] = (LinkMesh){0, 0, 0};
            fprintf(stderr, "[link-log] not his after all (drawn %.0f from his eye): %llu indices, vb %llu bytes\n", distance,
                    (unsigned long long)count, (unsigned long long)size);
        }
}

static bool mirrored_block(const float *slot0) {  // (the draw's view block turns the world inside out: a reflection)
    const float *m = slot0 + 16;
    const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    return det < 0;
}

// The centre of a palette's bones in the world, minus his eye point: its sideways (horizontal) distance from him.
static float palette_sideways(const float *bones, int n, const float *v) {
    float c[3] = {0, 0, 0}, w[3];
    for (int i = 0; i < n; i++) c[0] += bones[i * 12 + 3], c[1] += bones[i * 12 + 7], c[2] += bones[i * 12 + 11];
    for (int r = 0; r < 3; r++) c[r] = c[r] / n - v[r * 4 + 3];
    for (int i = 0; i < 3; i++) w[i] = v[0 * 4 + i] * c[0] + v[1 * 4 + i] * c[1] + v[2 * 4 + i] * c[2];
    const float dx = w[0] - g_rig.link[0], dz = w[2] - g_rig.link[2];
    return sqrtf(dx * dx + dz * dz);
}

// His small rigid pieces (eyes, belt, pouches) are only told by where they sit - at the eye point, on his centre line.
// When his body leaves the eye point (a roll, a swing, climbing, swimming, a door) they left that place and showed.
// So, like his body's meshes, they are remembered: once one has kept its offset from the eye point while he walked
// (LINK_PIECE_WALK units in the world, offset within LINK_PIECE_KEPT - a world mesh he stands in stays behind), that
// mesh is his wherever it is drawn. His hands and the sword and shield are never taken (they have their own handling).
#define LINK_PIECE_CANDIDATES 16
#define LINK_PIECE_WALK 40.0f
#define LINK_PIECE_KEPT 12.0f
#define LINK_PIECE_BODY_RADIUS 30.0f
typedef struct { uint64_t hash, count, size; float eye[3], off[3]; } LinkPiece;
static LinkPiece g_piece[LINK_PIECE_CANDIDATES];

// Meshes drawn on the sword's or shield's own joint (the blade, the hilt's parts): theirs, never Link's. (The blade
// sheathed on his back moves with him like his pieces do; taken for one, it vanished from his hand too.)
#define NOT_LINK_MAX 16
static LinkMesh g_not_link[NOT_LINK_MAX];
static bool not_link_mesh(uint64_t hash, uint64_t count, uint64_t size) {
    for (int i = 0; i < NOT_LINK_MAX; i++)
        if (g_not_link[i].hash == hash && g_not_link[i].count == count && g_not_link[i].size == size) return true;
    return false;
}
static void mark_not_link(uint64_t hash, uint64_t count, uint64_t size) {
    if (!hash || not_link_mesh(hash, count, size)) return;
    static int next;
    g_not_link[next] = (LinkMesh){hash, count, size};
    next = (next + 1) % NOT_LINK_MAX;
    for (int i = 0; i < LINK_MESH_CACHE; i++)  // (forgotten as his, if it had been taken)
        if (g_link_mesh[i].hash == hash && g_link_mesh[i].count == count && g_link_mesh[i].size == size)
            g_link_mesh[i] = (LinkMesh){0, 0, 0};
    fprintf(stderr, "[link-log] on the sword's or shield's joint, never Link: %llu indices, vb %llu bytes\n",
            (unsigned long long)count, (unsigned long long)size);
}

static bool link_piece_excluded(uint64_t count, uint64_t vb_size) {
    return count == 1056 || count == 2352 || vb_size == 28672 ||  // (the sword, the shield; his hands: HAND_MESH_VB)
           (count == 330 && vb_size == 12288);                     // (the Beetle on his wrist: vr_rig_beetle_draw)
}
#define LINK_PIECE_MIN_COUNT 24  // (smaller: quads, glints, effects - shared by much of the world; not learnt)
#define LINK_JOINT_MAX_COUNT 48  // (on the same joint as a piece of his: only meshes this small - the bits by his head)

static void link_piece_seen(uint64_t hash, uint64_t count, uint64_t size, const float off[3]) {
    if (!hash || count < LINK_PIECE_MIN_COUNT || known_link_mesh(hash, count, size) || not_link_mesh(hash, count, size)) return;
    for (int i = 0; i < LINK_PIECE_CANDIDATES; i++) {
        LinkPiece *p = &g_piece[i];
        if (p->hash != hash || p->count != count || p->size != size) continue;
        const float wx = g_rig.link[0] - p->eye[0], wz = g_rig.link[2] - p->eye[2];
        const float ox = off[0] - p->off[0], oy = off[1] - p->off[1], oz = off[2] - p->off[2];
        if (ox * ox + oy * oy + oz * oz > LINK_PIECE_KEPT * LINK_PIECE_KEPT) {  // (left behind: start over from here)
            memcpy(p->eye, g_rig.link, sizeof p->eye), memcpy(p->off, off, sizeof p->off);
        } else if (wx * wx + wz * wz > LINK_PIECE_WALK * LINK_PIECE_WALK) {
            remember_link_mesh(hash, count, size), p->hash = 0;
            fprintf(stderr, "[link-log] a piece of Link remembered: %llu indices, vb %llu bytes\n",
                    (unsigned long long)count, (unsigned long long)size);
        }
        return;
    }
    static int next;
    g_piece[next] = (LinkPiece){hash, count, size, {g_rig.link[0], g_rig.link[1], g_rig.link[2]}, {off[0], off[1], off[2]}};
    next = (next + 1) % LINK_PIECE_CANDIDATES;
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
static bool g_aim_ok;                 // (the right controller's aim pose, local space; and its ray in the game world)
static float g_aim_pose[7], g_aim_world[6];
static uint64_t g_aim_world_frame;
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
    if (g_aim_ok) {  // (the right controller's pointing ray, the same way)
        float rel[9], off[3], rc[9], p[3];
        vr_rig_relative(reference, g_aim_pose, vr_rig_units_per_metre(), rel, off);
        m_mul(a->rotation, rel, rc);
        m_vec(a->rotation, off, p);
        for (int i = 0; i < 3; i++) g_aim_world[i] = p[i] + a->origin[i], g_aim_world[3 + i] = -rc[i * 3 + 2];
        g_aim_world_frame = renderer_present_count() + 1;
    }
    pthread_mutex_unlock(&g_ctrl_pose_lock);
}

// (vr_input.c, each frame) The right controller's aim pose (OpenXR local space: x y z w, position): where it points.
void vr_rig_set_aim(bool ok, const float pose[7]) {
    pthread_mutex_lock(&g_ctrl_pose_lock);
    g_aim_ok = ok && pose;
    if (g_aim_ok) memcpy(g_aim_pose, pose, sizeof g_aim_pose);
    pthread_mutex_unlock(&g_ctrl_pose_lock);
}
// The right controller's pointing ray in the game world: its origin and its direction (unit), refreshed with the eyes.
bool vr_rig_controller_aim(float o[3], float d[3]) {
    extern uint64_t renderer_present_count(void);
    pthread_mutex_lock(&g_ctrl_pose_lock);
    const bool ok = g_aim_world_frame && g_aim_world_frame + 3 >= renderer_present_count() + 1;
    if (ok) memcpy(o, g_aim_world, 12), memcpy(d, g_aim_world + 3, 12);
    pthread_mutex_unlock(&g_ctrl_pose_lock);
    return ok;
}

// Where the game puts Link's hand (game world, the wrist) - last drawn within 2 frames
bool vr_rig_game_hand(int side, float w[3]) {
    extern uint64_t renderer_present_count(void);
    if (side < 0 || side > 1 || !g_hand_last_frame[side] || g_hand_last_frame[side] + 2 < renderer_present_count() + 1)
        return false;
    memcpy(w, g_hand_last[side], sizeof g_hand_last[side]);
    return true;
}

// A controller's pose can drop out for a few frames (held close to the face - the shield raised - the headset's
// cameras lose it): its hand, and what it holds, went back to the game's pose for those frames. The last good pose is
// kept for GRIP_HOLD frames (SKYRECOMP_VR_HAND_LOG logs each dropout).
#define GRIP_HOLD 60
void vr_rig_set_grips(bool left_ok, const float left[7], bool right_ok, const float right[7]) {
    extern uint64_t renderer_present_count(void);
    static uint64_t good[2];
    static int lost[2];
    static int log = -1;
    if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
    const uint64_t f = renderer_present_count() + 1;
    const bool ok[2] = {left_ok && left, right_ok && right};
    const float *pose[2] = {left, right};
    for (int s = 0; s < 2; s++) {
        if (ok[s]) {
            if (log && lost[s]) fprintf(stderr, "[hand-log] frame %llu controller %d pose back after %d frames\n", (unsigned long long)f, s, lost[s]);
            memcpy(g_grip[s], pose[s], sizeof g_grip[s]);
            good[s] = f, lost[s] = 0, g_grip_ok[s] = true;
        } else {
            if (log && !lost[s]) fprintf(stderr, "[hand-log] frame %llu controller %d pose lost\n", (unsigned long long)f, s);
            lost[s]++;
            g_grip_ok[s] = good[s] && good[s] + GRIP_HOLD >= f;  // (the last good pose, for a moment)
        }
    }
}
// ---- 1st person: what his hands hold (the sword, the shield). They're rigid draws of their own, posed by the game
// on his hand's joint; placed with the hand, they'd stay where the game animates them while the hand drawn goes to
// its controller. So each is drawn through the same world transform as the hand holding it: C K (H^-1 M), H^-1 M
// being the item relative to the game's hand - the game's own grip, nothing tuned. Held or not is read from the
// game's placement (measured in Skyloft, first person: a drawn sword's hilt sits 10.3-10.6 units from the hand
// holding it, a readied shield 10.8 from its hand; sheathed, on his back, both are 50+ units from either hand).
// SKYRECOMP_VR_NO_HELD=1: they stay where the game puts them.
#define HELD_REACH 25.0f
#define HELD_REACH_SHIELD 40.0f
#define HELD_NOT_HIS 300.0f   // (an item's model this far from his eye point is someone else's)
#define HELD_KEEP 70.0f
#define HELD_PART_NEAR 40.0f
#define HELD_MEMORY 60        // (frames)
#define HELD_CONTINUITY 80.0f // (how far the held item may have moved since it was last drawn)
#define HELD_PART_MIN_COUNT 24
#define HELD_BACK 30.0f       // (closer to his eye point than this, with its hand not drawn: on his back)
#define HELD_BACK_FAR 45.0f   // (the same with its hand drawn far from it)
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

static void sling_measure(void) {  // (the Slingshot and its hand drawn this frame: Hg^-1 B0 again)
    float hi[12];
    const int s = g_sling_side;
    if (s < 0 || g_hand_game_frame[s] != g_sling_frame || !a_inverse(g_hand_game[s], hi)) return;
    a_mul(hi, g_sling_b0, g_sling_rel);
    g_sling_rel_ok = true;
}

static void held_measure(int k) {    // (the item and its hand drawn this frame: H^-1 M again)
    float hi[12];
    const int s = g_held[k].side;
    if (s < 0 || g_hand_game_frame[s] != g_held[k].frame || !a_inverse(g_hand_game[s], hi)) return;
    a_mul(hi, g_held[k].block, g_held[k].rel);
    g_held[k].rel_frame = g_held[k].frame;
}

// (g_hand_orig, g_hand_repl: this hand draw's game view block and the one replacing it)
void vr_rig_set_hand_draw(int side) {
    g_hand_draw = side, g_hand_blocks = false, g_door_blocks = false, g_held_on = false, g_skin_on = false;
}
// (exec.c) This hand draw's game view block and its replacement: other uniform blocks holding the same matrix (the
// shader's normals, for its lighting) get the replacement too.
bool vr_rig_hand_blocks(float orig[12], float repl[12]) {
    if (!g_hand_blocks || (g_hand_draw < 0 && !g_door_blocks && !g_skin_on)) return false;
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
    {   // (diagnostics: SKYRECOMP_VR_HAND_LOG=1 - rigid draws turned down near where a hand just was: each kind once)
        static int log = -1;
        if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
        const bool kind_ok = vb_size == HAND_MESH_VB && count >= 600 && count <= 900;
        const float ex = slot0[19] - e[0], ey = slot0[23] - e[1], ez = slot0[27] - e[2];
        const float eye_d = sqrtf(ex * ex + ey * ey + ez * ez);
        if (log && (!kind_ok || eye_d > HAND_REACH)) {
            extern uint64_t renderer_present_count(void);
            const uint64_t f = renderer_present_count() + 1;
            const float *v = g_rig.game_view;
            float w[3];
            for (int i = 0; i < 3; i++)
                w[i] = v[0 * 4 + i] * (slot0[19] - v[3]) + v[1 * 4 + i] * (slot0[23] - v[7]) + v[2 * 4 + i] * (slot0[27] - v[11]);
            for (int s = 0; s < 2; s++) {
                if (!g_hand_last_frame[s] || g_hand_last_frame[s] + 3 < f) continue;
                const float a = w[0] - g_hand_last[s][0], b = w[1] - g_hand_last[s][1], c = w[2] - g_hand_last[s][2];
                const float d = sqrtf(a * a + b * b + c * c);
                if (d > 40) continue;
                static struct { uint64_t count, size; } seen[48];
                static int nseen;
                bool known = false;
                for (int i = 0; i < nseen; i++) known |= seen[i].count == count && seen[i].size == vb_size;
                if (known || nseen >= 48) break;
                seen[nseen].count = count, seen[nseen++].size = vb_size;
                fprintf(stderr, "[hand-log] frame %llu turned down near hand %d (%.1f away): count %llu vb size %llu, %.1f from the eye%s\n",
                        (unsigned long long)f, s, d, (unsigned long long)count, (unsigned long long)vb_size, eye_d,
                        kind_ok ? " (too far)" : "");
                break;
            }
        }
    }
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
        // camera it hangs on: in 1st person the camera faces his way, so the camera's left is his left hand.
        // (Measured with SKYRECOMP_VR_HAND_LOG in Skyloft: the mesh holding the sword - his right hand - hangs on the
        // camera's right; the other ~30 units to its left. Taken the other way round, each controller showed the
        // other hand - mirrored - and the sword went to the left controller. SKYRECOMP_VR_HAND_SWAP=1: the old way.)
        const bool known[2] = {g_hand_last_frame[0] + 30 >= now && g_hand_last_frame[0], g_hand_last_frame[1] + 30 >= now && g_hand_last_frame[1]};
        const bool drawn[2] = {g_hand_last_frame[0] == now, g_hand_last_frame[1] == now};
        if (known[0] && known[1]) {
            float d2[2];
            for (int h = 0; h < 2; h++) {
                const float a0 = w[0] - g_hand_last[h][0], a1 = w[1] - g_hand_last[h][1], a2 = w[2] - g_hand_last[h][2];
                d2[h] = a0 * a0 + a1 * a1 + a2 * a2;
            }
            side = drawn[0] != drawn[1] ? (drawn[0] ? 1 : 0) : d2[0] <= d2[1] ? 0 : 1;
        } else {
            static int swap = -1;
            if (swap < 0) swap = getenv("SKYRECOMP_VR_HAND_SWAP") != NULL;
            side = (dx < 0) == !swap ? 0 : 1;
        }
        static int next;
        g_hand_mesh[next] = (__typeof__(g_hand_mesh[0])){vb, count, side};
        next = (next + 1) % 8;
    }
    memcpy(g_hand_last[side], w, sizeof w);
    g_hand_last_frame[side] = now;
    g_hand_count_cur = count;
    memcpy(g_hand_game[side], slot0 + 16, sizeof g_hand_game[side]);
    g_hand_game_frame[side] = now;
    {   // (diagnostics: SKYRECOMP_VR_HAND_LOG=1)
        static int log = -1;
        static uint64_t last[2];
        if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
        if (log && now >= last[side] + 60) {
            last[side] = now;
            fprintf(stderr, "[hand-log] frame %llu side %d count %llu vb %llx det %.3f cam x %.1f\n", (unsigned long long)now,
                    side, (unsigned long long)count, (unsigned long long)vb, det3(slot0 + 16), dx);
        }
    }
    for (int k = 0; k < HELD_KINDS; k++)  // (an item drawn before its hand this frame: measured now, used next frame)
        if (g_held[k].frame == now && g_held[k].side == side) held_measure(k);
    if (g_sling_frame == now && g_sling_side == side) sling_measure();  // (the Slingshot too)
    if (side == 1) beetle_measure();  // (and the Beetle, if drawn this frame: it checks)
    return side;
}

// ---- the Slingshot in his hand. Unlike the sword and shield it is a skinned draw (index count 762, 4 bones; measured:
// its first bone 10.9 units from his right hand, the game holding it there): its bones are in game-camera space, and
// each eye's projection carries the eye's own offset (Pe Ve Vg^-1), so moving its view block - as the hands are moved -
// would not move it. Instead its bones (and view block, for its lighting) are taken along with the hand holding it:
// T = Vg C K Hg^-1, the hand's game block Hg (camera space) to where the hand is drawn - the controller's grip C (world,
// vr_rig_controller_pose) and the hand mesh in it K (hand_k) - so it keeps the game's own grip.
static void hand_k(int side, float k[12]);
#define SLINGSHOT_COUNT 762
#define SKIN_REACH 40.0f
bool vr_rig_skin_transform(float T[12]) {  // (exec.c: its bones' slot)
    if (!g_skin_on) return false;
    memcpy(T, g_skin_T, sizeof g_skin_T);
    return true;
}
bool vr_rig_held_skin_draw(const float *slot0, uint64_t size0, const float *bones, uint64_t bone_bytes, uint64_t count) {
    static int off = -1;
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_HELD") != NULL;
    g_skin_on = false;
    if (off || count != SLINGSHOT_COUNT || !slot0 || !bones || bone_bytes < 48 || bone_bytes / 48 > 16 || !hands_on() ||
        !g_rig.have_game_view || !sky_vr_is_scene_projection(slot0, size0))
        return false;
    extern uint64_t renderer_present_count(void);
    const uint64_t now = renderer_present_count() + 1;
    int side = -1;
    float best = SKIN_REACH * SKIN_REACH;
    for (int s = 0; s < 2; s++) {
        if (!g_hand_game_frame[s] || g_hand_game_frame[s] + 2 < now || !g_grip_ok[s]) continue;
        const float *h = g_hand_game[s];
        const float dx = bones[3] - h[3], dy = bones[7] - h[7], dz = bones[11] - h[11], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) best = d2, side = s;
    }
    // (the hand holding it is kept: walking, turning and the game's aiming moved his hand away from it for frames)
    if (side < 0 && g_sling_side >= 0 && g_sling_frame && g_sling_frame + 60 >= now) side = g_sling_side;
    {   // (it is out: the game's Joy-Con is held still meanwhile - runtime/hle/vr_aim.c)
        extern void vr_aim_slingshot_seen(void);
        vr_aim_slingshot_seen();
    }
    memcpy(g_sling_b0, bones, sizeof g_sling_b0);
    g_sling_frame = now, g_sling_side = side;
    if (side >= 0 && g_hand_game_frame[side] == now) sling_measure();  // (its hand already drawn this frame)
    float C[12], K[12], ck[12], target[12], bi[12];
    // (why a draw of it is left where the game puts it, counted: SKYRECOMP_VR_HAND_LOG, once a second)
    static uint64_t log_frame, n_seen, n_placed, n_noside, n_scripted, n_nopose, n_door;
    static int log = -1;
    if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
    if (log && now >= log_frame + 120) {
        if (n_seen)
            fprintf(stderr, "[skin-log] Slingshot draws %llu: placed %llu; no hand %llu, scripted move %llu, no controller pose %llu, "
                            "door %llu\n", (unsigned long long)n_seen, (unsigned long long)n_placed, (unsigned long long)n_noside,
                    (unsigned long long)n_scripted, (unsigned long long)n_nopose, (unsigned long long)n_door);
        log_frame = now, n_seen = n_placed = n_noside = n_scripted = n_nopose = n_door = 0;
    }
    n_seen++;
    extern bool door_at(void);
    if (side < 0) { n_noside++; return false; }
    if (g_scripted && door_at()) { n_scripted++; return false; }  // (only at a door: a walk or a turn moved his head too)
    if (!vr_rig_controller_pose(side, C) || !a_inverse(bones, bi)) { n_nopose++; return false; }
    if (atomic_load(&g_door_grab) == side || atomic_load(&g_door_hover[side]) != 0.0f) { n_door++; return false; }
    n_placed++;
    // Its first bone where the game holds it in the hand (g_sling_rel = Hg^-1 B0, measured whenever both are drawn in
    // one frame), with the hand on its controller: B0' = Vg C K rel; every bone moved the same: T = B0' B0^-1. (From
    // its own bone of this very draw: with the hand's game block instead - a frame older when the hand is drawn
    // after it, and swinging with his walk - it came loose whenever he moved or turned.)
    if (!g_sling_rel_ok) {  // (not measured yet: from the hand's block as it is)
        float hi[12];
        if (!a_inverse(g_hand_game[side], hi)) return false;
        a_mul(hi, bones, g_sling_rel);
    }
    hand_k(side, K);
    a_mul(C, K, ck);
    float vck[12];
    a_mul(g_rig.game_view, ck, vck);
    a_mul(vck, g_sling_rel, target);
    a_mul(target, bi, g_skin_T);
    g_skin_on = true;
    static int said;
    if (!said++) fprintf(stderr, "[vr] first person: the Slingshot is in the %s hand on its controller\n", side ? "right" : "left");
    return true;
}

// The Beetle on his right wrist (held, before a launch): three draws on one model matrix - the Beetle actor's (its
// position +0x144) - rigid 330 indices (12288-byte vertex buffer), skinned 1464 (61440, 5 bones) and 96 (4096, 7
// bones): the Beetle and the launcher it sits on. Measured headset-free with SKYRECOMP_HELD_NEAR=beetle (a test build:
// every draw at the held Beetle's position). All three go with his right hand onto the right controller, as the
// Slingshot does: rel = Hg^-1 M (the model relative to the hand the game draws, measured whenever both are drawn in a
// frame), then target = Vg C K rel and T = target M^-1 - for the rigid one its view block, for the skinned ones their
// bones too (exec.c). So it sits where the game puts it, on the wrist, behind the hand. SKYRECOMP_VR_BEETLE_SHIFT="x y z"
// (game units, in the controller's grip frame: x right, y up, -z forward) moves it from there.
#define BEETLE_ROLL_DEGREES 270.0f  // (counterclockwise about the forearm, looking down it toward the hand; user's pick)
// (the shift is along the controller's grip axes as the game world has them - tilted with the handle, so +z goes back
// and down. User 2026-11-09: y -4, z 0 is in line with the hand; z +8 put it lower and still over the hand - back
// to y -4, z 0.)
#define BEETLE_BACK 0.0f
#define BEETLE_UP -7.0f  // (negative is up: user 2026-11-09, -4 in line with the hand, then "a bit up")
// (game units back along the forearm - against the controller's pointing direction. User 2026-11-09 screenshot at 6:
// the bracelet still round the middle of the hand, the fingers out past it - about half a hand further back: 12;
// then "a bit further": 16, 20)
#define BEETLE_ARM_BACK 20.0f
// (game units straight up in the world - about 100 a metre. User 2026-11-09: 4 didn't show; 20 back along a slightly
// raised forearm also drops it a few units)
#define BEETLE_LIFT 10.0f
// The launcher bracelet isn't one of the three: (user 2026-11-09, turning each in turn) 1464 is part of the Beetle, the
// rigid 330 its blade and 96 its wings. So nothing is swung by default (SKYRECOMP_VR_BANGLE_MESH=<index count> and
// SKYRECOMP_VR_BANGLE_YAW swing one, for tests).
#define BANGLE_COUNT_DEFAULT 0
// The bracelet is its own rigid draw: 768 indices in a 28672-byte vertex buffer - the size of his hands' - so the hands
// code took it for his right hand and put it on the controller as one, its model turned to a hand's: sideways along
// the arm (found in VR with vr_rig_beetle_near_note: both 750, his hand, and 768 taken as his right hand while the
// Beetle was out). While the Beetle is on his wrist it is not a hand (vr_rig_hand_side) but goes with the Beetle: the
// same move as the Beetle's rigid body (its T, camera space), so it keeps the game's place against it.
#define BRACELET_COUNT 768
#define BRACELET_VB 28672
static float g_beetle_T[12];          // (the Beetle's rigid body's move this frame or the last)
static uint64_t g_beetle_T_frame;
// The Beetle is placed against the bracelet, not his hand: (user 2026-11-09) its 270-degree turn and its place on the
// wrist were tuned while the bracelet was taken for his right hand (drawn after his real hand, so the hand the Beetle
// was measured against) - against his real hand (750) it sat out at the fingers, blade upright. rel = B^-1 M, B the
// bracelet's game block, M the Beetle's, measured whenever both are drawn in one frame.
static float g_bracelet_block[12], g_beetle_bracelet_rel[12];
static uint64_t g_bracelet_frame;
static bool g_beetle_bracelet_ok;
static float g_beetle_m_game[12];     // (the Beetle's rigid body's game block, and its frame)
static uint64_t g_beetle_m_frame;
static void beetle_bracelet_measure(void) {
    float bi[12];
    if (!g_bracelet_frame || g_bracelet_frame != g_beetle_m_frame || !a_inverse(g_bracelet_block, bi)) return;
    a_mul(bi, g_beetle_m_game, g_beetle_bracelet_rel);
    g_beetle_bracelet_ok = true;
}
static bool beetle_out(void) {        // (the Beetle drawn on his wrist this frame or the last)
    extern uint64_t renderer_present_count(void);
    return g_beetle_T_frame && g_beetle_T_frame + 2 >= renderer_present_count() + 1;
}
#define BANGLE_YAW_DEGREES 90.0f
// w (3x4, world) turned by `degrees` about the axis (ax, ay, az) through its own position (counterclockwise looking
// against the axis)
static void beetle_turn(float w[12], float ax, float ay, float az, float degrees) {
    const float len = sqrtf(ax * ax + ay * ay + az * az), th = degrees * (float)(M_PI / 180.0);
    if (!(len > 0.5f) || th == 0.0f) return;
    const float x = ax / len, y = ay / len, z = az / len, co = cosf(th), si = sinf(th), t = 1.0f - co;
    const float R[9] = {t * x * x + co, t * x * y - si * z, t * x * z + si * y,
                        t * x * y + si * z, t * y * y + co, t * y * z - si * x,
                        t * x * z - si * y, t * y * z + si * x, t * z * z + co};
    float r[12];
    memcpy(r, w, sizeof r);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) w[i * 4 + j] = R[i * 3] * r[j] + R[i * 3 + 1] * r[4 + j] + R[i * 3 + 2] * r[8 + j];
}
static bool beetle_mesh(uint64_t count, uint64_t vb_size) {
    return (count == 330 && vb_size == 12288) || (count == 1464 && vb_size == 61440) || (count == 96 && vb_size == 4096);
}
static float g_beetle_m[12], g_beetle_rel[12];
static uint64_t g_beetle_frame;
static bool g_beetle_rel_ok;
static void beetle_measure(void) {  // (the Beetle and his right hand drawn this frame: Hg^-1 M again)
    float hi[12];
    if (!g_beetle_frame || g_hand_game_frame[1] != g_beetle_frame || !a_inverse(g_hand_game[1], hi)) return;
    a_mul(hi, g_beetle_m, g_beetle_rel);
    g_beetle_rel_ok = true;
}
bool vr_rig_beetle_draw(const float *slot0, uint64_t size0, uint64_t count, uint64_t vb_size) {
    static int off = -1;
    // (user 2026-11-09: turned 90 degrees counterclockwise - a roll about the forearm, seen looking down it toward the
    // hand - and a few units back toward the elbow from where the game holds it. SKYRECOMP_VR_BEETLE_ROLL (degrees) and
    // SKYRECOMP_VR_BEETLE_SHIFT override.)
    static float shift[3] = {0.0f, BEETLE_UP, BEETLE_BACK}, roll = BEETLE_ROLL_DEGREES, bangle_yaw = BANGLE_YAW_DEGREES;
    static uint64_t bangle_count = BANGLE_COUNT_DEFAULT;
    if (off < 0) {
        off = getenv("SKYRECOMP_VR_NO_HELD") != NULL;
        const char *e = getenv("SKYRECOMP_VR_BEETLE_SHIFT");
        if (e) sscanf(e, "%f %f %f", &shift[0], &shift[1], &shift[2]);
        e = getenv("SKYRECOMP_VR_BEETLE_ROLL");
        if (e) roll = (float)atof(e);
        e = getenv("SKYRECOMP_VR_BANGLE_YAW");
        if (e) bangle_yaw = (float)atof(e);
        e = getenv("SKYRECOMP_VR_BANGLE_MESH");
        if (e) bangle_count = (uint64_t)atoll(e);
    }
    const float yaw = count == bangle_count ? bangle_yaw : 0.0f;  // (the bracelet: also swung to run along the arm)
    extern uint64_t renderer_present_count(void);
    const uint64_t now = renderer_present_count() + 1;
    if (count == BRACELET_COUNT && vb_size == BRACELET_VB) {  // (the bracelet: with the Beetle's own move)
        if (off || !slot0 || !hands_on() || !beetle_out() || !sky_vr_is_scene_projection(slot0, size0)) return false;
        memcpy(g_bracelet_block, slot0 + 16, sizeof g_bracelet_block), g_bracelet_frame = now;
        beetle_bracelet_measure();
        memcpy(g_skin_T, g_beetle_T, sizeof g_skin_T);
        g_skin_on = true;
        static int said;
        if (!said++) fprintf(stderr, "[vr] first person: the Beetle's bracelet goes with the Beetle\n");
        return true;
    }
    if (off || !beetle_mesh(count, vb_size) || !slot0 || !hands_on() || !g_rig.have_game_view ||
        !sky_vr_is_scene_projection(slot0, size0))
        return false;
    const float *m = slot0 + 16;
    // (in his right hand: within reach of it; in flight it is far from it and stays the game's)
    if (!g_hand_game_frame[1] || g_hand_game_frame[1] + 2 < now) return false;
    const float *h = g_hand_game[1];
    const float dx = m[3] - h[3], dy = m[7] - h[7], dz = m[11] - h[11];
    if (dx * dx + dy * dy + dz * dz > SKIN_REACH * SKIN_REACH) return false;
    extern bool door_at(void);
    if (g_scripted && door_at()) return false;
    float C[12], K[12], ck[12], vck[12], target[12], mi[12];
    if (!vr_rig_controller_pose(1, C) || !a_inverse(m, mi)) return false;
    memcpy(g_beetle_m, m, sizeof g_beetle_m);
    g_beetle_frame = now;
    if (g_hand_game_frame[1] == now) beetle_measure();
    if (!g_beetle_rel_ok) {  // (not measured yet: from the hand's block as it is)
        float hi[12];
        if (!a_inverse(g_hand_game[1], hi)) return false;
        a_mul(hi, m, g_beetle_rel);
    }
    hand_k(1, K);
    a_mul(C, K, ck);
    if (count == 330) {  // (its rigid body: against the bracelet when known - above)
        memcpy(g_beetle_m_game, m, sizeof g_beetle_m_game), g_beetle_m_frame = now;
        beetle_bracelet_measure();
    }
    float w[12], rel_b[12];  // (the Beetle in the world: against the bracelet as a hand on the controller - or the hand)
    if (g_beetle_bracelet_ok && count == 330) a_mul(ck, g_beetle_bracelet_rel, w);
    else if (g_beetle_bracelet_ok && g_bracelet_frame + 2 >= now) {
        // (its skinned parts share the rigid body's move: B^-1 of this draw's block, through the bracelet's place)
        float bi[12];
        if (!a_inverse(g_bracelet_block, bi)) return false;
        a_mul(bi, m, rel_b);
        a_mul(ck, rel_b, w);
    } else a_mul(ck, g_beetle_rel, w);
    // Turned about the forearm (the grip's z: roll), the bracelet then swung about the grip's up axis (y: yaw) - each
    // through the item's own place - then shifted in the grip's frame.
    beetle_turn(w, C[2], C[6], C[10], roll);
    beetle_turn(w, C[1], C[5], C[9], yaw);
    // Its place: the wrist (the hand mesh's root on the controller, ck's translation), then shifted in the grip's frame
    // - not where the game's grip puts it: against his real hand (750) that is out at the fingers (user 2026-11-09: once
    // the bracelet stopped being taken for his hand, the pair left the wrist). SKYRECOMP_VR_BEETLE_AT_GRIP=1: the game's.
    // (superseded: measured against the bracelet it is on the wrist already. SKYRECOMP_VR_BEETLE_AT_WRIST=1 centres it
    // at the wrist point anyway.)
    static int at_wrist = -1;
    if (at_wrist < 0) at_wrist = getenv("SKYRECOMP_VR_BEETLE_AT_WRIST") != NULL;
    if (at_wrist) w[3] = ck[3], w[7] = ck[7], w[11] = ck[11];
    for (int i = 0; i < 3; i++) w[i * 4 + 3] += C[i * 4 + 0] * shift[0] + C[i * 4 + 1] * shift[1] + C[i * 4 + 2] * shift[2];
    {   // back along the forearm: against where the controller points (its aim ray), level with where it is - the
        // grip's z runs along the tilted handle and took it down too (user 2026-11-09). SKYRECOMP_VR_BEETLE_ARM_BACK.
        static float arm_back = -1;
        if (arm_back < 0) {
            const char *e = getenv("SKYRECOMP_VR_BEETLE_ARM_BACK");
            arm_back = e ? (float)atof(e) : BEETLE_ARM_BACK;
        }
        float o[3], d[3];
        if (arm_back != 0 && vr_rig_controller_aim(o, d))
            for (int i = 0; i < 3; i++) w[i * 4 + 3] -= d[i] * arm_back;
        // and straight up in the world (the grip's y is tilted with the handle: a few units along it barely lifted
        // it - user 2026-11-09). SKYRECOMP_VR_BEETLE_LIFT.
        static float lift = -1e9f;
        if (lift < -1e8f) {
            const char *e = getenv("SKYRECOMP_VR_BEETLE_LIFT");
            lift = e ? (float)atof(e) : BEETLE_LIFT;
        }
        w[7] += lift;
    }
    a_mul(g_rig.game_view, w, target);
    (void)vck;
    a_mul(target, mi, g_skin_T);
    g_skin_on = true;
    if (count == 330) memcpy(g_beetle_T, g_skin_T, sizeof g_beetle_T), g_beetle_T_frame = now;  // (its rigid body: the bracelet's move)
    static int said;
    if (!said++) fprintf(stderr, "[vr] first person: the Beetle on the right controller, on his wrist\n");
    return true;
}

// (RE, exec.c: every draw, eye 0) While the Beetle is on his wrist, each other kind of draw near it - its model or a
// bone within 45 units - once, to find the launcher bracelet: its index count, vertex buffer, bones, what the hands
// code made of it, and its axes against the Beetle's ([beetle-near] in the log).
void vr_rig_beetle_near_note(const float *slot0, uint64_t size0, const float *bones, uint64_t bone_bytes, uint64_t count,
                             uint64_t vb_size, int hand, bool held) {
    extern uint64_t renderer_present_count(void);
    const uint64_t now = renderer_present_count() + 1;
    if (!slot0 || !g_beetle_frame || g_beetle_frame + 2 < now || beetle_mesh(count, vb_size) ||
        !sky_vr_is_scene_projection(slot0, size0))
        return;
    const float *m = slot0 + 16, *b = g_beetle_m;
    float d2 = (m[3] - b[3]) * (m[3] - b[3]) + (m[7] - b[7]) * (m[7] - b[7]) + (m[11] - b[11]) * (m[11] - b[11]);
    const int n = bones && bone_bytes >= 48 ? (int)(bone_bytes / 48) : 0;
    for (int i = 0; i < n && i < 512; i++) {
        const float *q = bones + i * 12;
        d2 = fminf(d2, (q[3] - b[3]) * (q[3] - b[3]) + (q[7] - b[7]) * (q[7] - b[7]) + (q[11] - b[11]) * (q[11] - b[11]));
    }
    if (d2 > 45.0f * 45.0f) return;
    static uint64_t seen[48];
    static int ns;
    const uint64_t key = count * 65536 + vb_size / 256 * 8 + (uint64_t)(n > 0);
    for (int i = 0; i < ns; i++) if (seen[i] == key) return;
    if (ns >= 48) return;
    seen[ns++] = key;
    fprintf(stderr, "[beetle-near] %llu indices, vb %llu bytes, %s%d bones, %.1f from the Beetle; %s; x axis %.2f %.2f %.2f "
            "(the Beetle's %.2f %.2f %.2f)\n", (unsigned long long)count, (unsigned long long)vb_size, n ? "skinned, " : "rigid, ",
            n, sqrtf(d2), hand >= 0 ? (hand ? "taken as his right hand" : "taken as his left hand") : held ? "a held item" : "left to the game",
            m[0], m[4], m[8], b[0], b[4], b[8]);
}

// (RE, exec.c: each skinned draw, eye 0; SKYRECOMP_VR_HAND_LOG=1) Skinned draws with a small bone palette (held items:
// the Slingshot's has 5) near one of his hands: each kind once - its index count, palette size, how far its first
// bone and its view block are from each hand (camera space, as the hands' blocks), its first bone and its view block.
void vr_rig_skinned_note(const float *slot0, uint64_t size0, const float *bones, uint64_t bone_bytes, uint64_t count) {
    static int log = -1;
    if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
    if (!log || !slot0 || !bones || !hands_on() || !sky_vr_is_scene_projection(slot0, size0)) return;
    const uint64_t nb = bone_bytes / 48;
    if (nb < 1 || nb > 16) return;
    extern uint64_t renderer_present_count(void);
    const uint64_t now = renderer_present_count() + 1;
    float db[2] = {-1, -1}, dv[2] = {-1, -1};
    bool near = false;
    for (int s = 0; s < 2; s++) {
        if (!g_hand_game_frame[s] || g_hand_game_frame[s] + 2 < now) continue;
        const float *h = g_hand_game[s];
        const float bx = bones[3] - h[3], by = bones[7] - h[7], bz = bones[11] - h[11];
        const float vx = slot0[19] - h[3], vy = slot0[23] - h[7], vz = slot0[27] - h[11];
        db[s] = sqrtf(bx * bx + by * by + bz * bz), dv[s] = sqrtf(vx * vx + vy * vy + vz * vz);
        near |= db[s] < 80 || dv[s] < 80;
    }
    if (!near) return;
    static struct { uint64_t count, nb; } seen[48];
    static int ns;
    for (int i = 0; i < ns; i++) if (seen[i].count == count && seen[i].nb == nb) return;
    if (ns < 48) seen[ns].count = count, seen[ns++].nb = nb;
    const float *v = slot0 + 16;
    fprintf(stderr, "[skin-log] frame %llu count %llu bones %llu: first bone %.1f / %.1f from the hands, view block %.1f / %.1f\n"
                    "[skin-log]   bone0 %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f\n"
                    "[skin-log]   view  %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f\n",
            (unsigned long long)now, (unsigned long long)count, (unsigned long long)nb, db[0], db[1], dv[0], dv[1],
            bones[0], bones[1], bones[2], bones[3], bones[4], bones[5], bones[6], bones[7], bones[8], bones[9], bones[10], bones[11],
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
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
    // Another part drawn on the held item's own joint (its exact block: the blade) goes with it. Such a part is known
    // by its index count once seen after the item in a frame: drawn before it, the item's block of this frame isn't
    // known yet - in the left eye; the right eye's replay comes after, so only that eye moved it.
    static uint64_t part_count[HELD_KINDS][4];
    int k = -1;
    bool part = false;  // (a known part, not the item itself)
    for (int i = 0; i < HELD_KINDS; i++) {
        if (count == HELD_COUNTS[i]) k = i;
        for (int p = 0; p < 4 && k < 0; p++) if (part_count[i][p] && count == part_count[i][p]) k = i, part = true;
    }
    if (k >= 0 && !part && g_rig.have_game_view) {
        // Not his: another object with the item's model far from Link (a shield 1760 units off, drawn before his own
        // every frame, took the item's place in the frame and his own shield was turned down as a second copy)
        const float *v = g_rig.game_view;
        float e[3];
        for (int r = 0; r < 3; r++) e[r] = v[r * 4] * g_rig.link[0] + v[r * 4 + 1] * g_rig.link[1] + v[r * 4 + 2] * g_rig.link[2] + v[r * 4 + 3];
        const float ex = m[3] - e[0], ey = m[7] - e[1], ez = m[11] - e[2];
        if (ex * ex + ey * ey + ez * ez > HELD_NOT_HIS * HELD_NOT_HIS) return false;
    }
    if (k < 0) {
        for (int i = 0; i < HELD_KINDS && k < 0; i++) {
            if (g_held[i].frame != now || g_held[i].side < 0) continue;
            bool same = true;
            for (int j = 0; same && j < 12; j++) same = fabsf(m[j] - g_held[i].block[j]) <= 1e-4f * (1.0f + fabsf(m[j]));
            if (!same) continue;
            k = i;
            int p = 0;
            while (p < 4 && part_count[i][p]) p++;
            // (tiny counts - 4, 8: flat quads, a glint - are ones a lot of the world shares: not remembered)
            if (p < 4 && count >= HELD_PART_MIN_COUNT) {
                part_count[i][p] = count;
                static int log = -1;
                if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
                if (log) fprintf(stderr, "[held-log] kind %d: a part with index count %llu\n", i, (unsigned long long)count);
            }
        }
        if (k < 0) return false;
    } else if (part && g_held[k].frame == now) {  // (a known part after the item: only on the item's own joint)
        bool same = true;
        for (int j = 0; same && j < 12; j++) same = fabsf(m[j] - g_held[k].block[j]) <= 1e-4f * (1.0f + fabsf(m[j]));
        if (!same) return false;
    } else if (part) {
        // A known part before the item this frame: its count may be one world objects share too, so it counts only
        // drawn right where the item was last frame (a swing moves it less than HELD_PART_NEAR in a frame), and it
        // takes the item's offset - the same joint - without touching the item's state. (Without this, world draws
        // sharing the part's count went to the controller while a swing hid his hand: geometry vanished.)
        const float dx = m[3] - g_held[k].block[3], dy = m[7] - g_held[k].block[7], dz = m[11] - g_held[k].block[11];
        const int side = g_held[k].side;
        if (side < 0 || !g_held[k].rel_frame || !g_held[k].frame || g_held[k].frame + 1 < now ||
            dx * dx + dy * dy + dz * dz > HELD_PART_NEAR * HELD_PART_NEAR)
            return false;
        if (atomic_load(&g_door_grab) == side || atomic_load(&g_door_hover[side]) != 0.0f) return false;
        g_hand_draw = side, g_hand_blocks = false, g_door_blocks = false, g_held_on = true;
        memcpy(g_held_k, g_held[k].rel, sizeof g_held_k);
        return true;
    } else if (g_held[k].frame == now) {
        // Another draw with the item's own count this frame: the same item again (another pass: its exact block) or
        // something else sharing the count - Skyloft's tower top shares the sword hilt's - left where it is.
        bool same = true;
        for (int j = 0; same && j < 12; j++) same = fabsf(m[j] - g_held[k].block[j]) <= 1e-4f * (1.0f + fabsf(m[j]));
        if (!same) return false;
    }
    if (g_held[k].frame != now) {
        // which hand it's in: the nearest one drawn this frame or the last, within reach (block translations are
        // camera space: distances as in the world)
        // The hand it was in last frame keeps it within HELD_KEEP: the item is often drawn before its hand, so it is
        // compared with the hand's last frame, and in a swing the hand moves further than HELD_REACH in a frame (the
        // sword went back to the game's swinging hand). Sheathed, on his back, it is 50+ units from either hand and
        // the hand moves away from it.
        // (the hand holding it is remembered across frames the item isn't drawn in - up to HELD_MEMORY: with only the
        // last frame's, a raised shield missing a frame or two had to be taken again within HELD_REACH of his left
        // hand, which the game draws 80-160 units from it then, and it went back to the game's pose)
        const int was = g_held[k].side >= 0 && g_held[k].frame && g_held[k].frame + HELD_MEMORY >= now ? g_held[k].side : -1;
        int side = -1;
        // (the shield sits further from the hand the game draws than the sword does: 25-31 units here, readied)
        const float reach = k == 1 ? HELD_REACH_SHIELD : HELD_REACH;
        float best = reach * reach, d2s[2] = {-1, -1};
        for (int s = 0; s < 2; s++) {
            if (!g_hand_game_frame[s] || g_hand_game_frame[s] + 1 < now) continue;
            const float dx = m[3] - g_hand_game[s][3], dy = m[7] - g_hand_game[s][7], dz = m[11] - g_hand_game[s][11];
            const float d2 = dx * dx + dy * dy + dz * dz;
            d2s[s] = d2;
            if (d2 < best) best = d2, side = s;
        }
        // Its distance from his eye point: on his back (sword sheathed, shield away) it sits close to it (the sheathed
        // hilt ~20 units: vr_sword.c measured the blade's base 19-20 sheathed, 47-125 drawn).
        float eye_d = -1;
        if (g_rig.have_game_view) {
            const float *v = g_rig.game_view;
            float e[3];
            for (int r = 0; r < 3; r++) e[r] = v[r * 4] * g_rig.link[0] + v[r * 4 + 1] * g_rig.link[1] + v[r * 4 + 2] * g_rig.link[2] + v[r * 4 + 3];
            const float ex = m[3] - e[0], ey = m[7] - e[1], ez = m[11] - e[2];
            eye_d = sqrtf(ex * ex + ey * ey + ez * ez);
        }
        // The held item, drawn first this frame, follows on from where it was: another object sharing its count (the
        // tower top) drawn first is left where it is, and leaves the held item's state alone for the real one.
        if (side < 0 && was >= 0) {
            const float cx = m[3] - g_held[k].block[3], cy = m[7] - g_held[k].block[7], cz = m[11] - g_held[k].block[11];
            if (cx * cx + cy * cy + cz * cz > HELD_CONTINUITY * HELD_CONTINUITY) return false;
        }
        if (side < 0 && was >= 0 && d2s[was] >= 0 && d2s[was] < HELD_KEEP * HELD_KEEP) side = was;
        // Once in a hand it stays on that controller, with the offset it had (rigid on the hand's joint: it doesn't
        // change), until it is on his back. The game doesn't always draw the hand holding it near it: mid-swing the
        // swinging arm, hand included, is part of his body (hidden in 1st person), and raising or bashing with the
        // shield drew his left hand 80-160 units from it - both snapped the item back to the game's own pose. With its
        // hand not drawn it is let go only within HELD_BACK of his eye point; with the hand drawn far, HELD_BACK_FAR.
        if (side < 0 && was >= 0 && eye_d >= 0 && eye_d > (d2s[was] < 0 ? HELD_BACK : HELD_BACK_FAR)) side = was;
        {   // (diagnostics: SKYRECOMP_VR_HAND_LOG=1 - a held item let go, and how far its hands were)
            static int log = -1;
            if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
            if (log && was >= 0 && side < 0 && !part)
                fprintf(stderr, "[held-log] frame %llu kind %d let go: hands %.1f / %.1f away (drawn %llu / %llu), %.1f from the eye\n",
                        (unsigned long long)now, k, d2s[0] >= 0 ? sqrtf(d2s[0]) : -1.0f, d2s[1] >= 0 ? sqrtf(d2s[1]) : -1.0f,
                        (unsigned long long)g_hand_game_frame[0], (unsigned long long)g_hand_game_frame[1], eye_d);
            if (log && was < 0 && side < 0 && !part && g_held[k].side >= 0)
                fprintf(stderr, "[held-log] frame %llu kind %d let go after not being drawn since %llu, %.1f from the eye\n",
                        (unsigned long long)now, k, (unsigned long long)g_held[k].frame, eye_d);
            static uint64_t last_free[HELD_KINDS];
            if (log && side < 0 && !part && now >= last_free[k] + 120)
                last_free[k] = now, fprintf(stderr, "[held-log] frame %llu kind %d not held: hands %.1f / %.1f away, %.1f from the eye\n",
                                            (unsigned long long)now, k, d2s[0] >= 0 ? sqrtf(d2s[0]) : -1.0f,
                                            d2s[1] >= 0 ? sqrtf(d2s[1]) : -1.0f, eye_d);
            static uint64_t last_eye[HELD_KINDS];
            if (log && side >= 0 && !part && now >= last_eye[k] + 60)
                last_eye[k] = now, fprintf(stderr, "[held-log] frame %llu kind %d held, %.1f from the eye\n", (unsigned long long)now, k, eye_d);
        }
        if (part && side < 0) return false;  // (a known part's count drawn elsewhere: leaves the item's state alone)
        memcpy(g_held[k].block, m, sizeof g_held[k].block);
        g_held[k].frame = now;
        if (side != g_held[k].side) g_held[k].rel_frame = 0;  // (another hand: its offset measured afresh)
        g_held[k].side = side;
        held_measure(k);
    }
    const int side = g_held[k].side;
    // (the offset: measured whenever the item and its hand are drawn in one frame; kept while its hand isn't drawn -
    // rel_frame goes back to 0 when it changes hands)
    if (side < 0 || !g_held[k].rel_frame) return false;
    if (atomic_load(&g_door_grab) == side || atomic_load(&g_door_hover[side]) != 0.0f) {
        static int log = -1;
        static uint64_t last;
        if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
        if (log && now >= last + 120) last = now, fprintf(stderr, "[held-log] frame %llu kind %d left to the game: a door's handle\n", (unsigned long long)now, k);
        return false;
    }
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
    {   // (diagnostics: SKYRECOMP_VR_HAND_LOG=1)
        static int log = -1;
        static uint64_t last;
        if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
        if (log && now >= last + 60) {
            last = now;
            fprintf(stderr, "[held-log] frame %llu kind %d count %llu side %d det item %.3f rel %.3f hand %.3f\n",
                    (unsigned long long)now, k, (unsigned long long)count, side, det3(m), det3(rel), det3(g_hand_game[side]));
        }
    }
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
    if (count == BRACELET_COUNT && vb_size == BRACELET_VB && beetle_out()) return -1;  // (the Beetle's bracelet, not a hand)
    const float *v = g_rig.game_view;
    float e[3];
    for (int r = 0; r < 3; r++) e[r] = v[r * 4] * g_rig.link[0] + v[r * 4 + 1] * g_rig.link[1] + v[r * 4 + 2] * g_rig.link[2] + v[r * 4 + 3];
    return hand_side(slot0, count, vb, vb_size, e);
}

// The view block a hand draw is lit and placed by: game camera x C K (its normals then turn with the controller;
// the projection stays the eye's, as for every draw). False: leave the draw as the game places it.
static bool hand_block(const VrRigAnchor *a, const float reference[7], const float game[12], float out[12]) {
    const int side = g_hand_draw;
    // (during a scripted move at a door his hands stay where the game animates them: on the door's handle. Only at a
    // door: a big sword swing lunges his head that far from the eye point too, and the hands, the sword and the shield
    // went back to the game's swinging pose for its frames)
    extern bool door_at(void);
    {   // (diagnostics: SKYRECOMP_VR_HAND_LOG=1 - why a held item was left to the game, at most once a second)
        static int log = -1;
        static uint64_t last;
        if (log < 0) log = getenv("SKYRECOMP_VR_HAND_LOG") != NULL;
        extern uint64_t renderer_present_count(void);
        const uint64_t f = renderer_present_count();
        if (log && g_held_on && side >= 0 && side < 2 && f >= last + 120 && (!g_grip_ok[side] || (g_scripted && door_at())))
            last = f, fprintf(stderr, "[held-log] frame %llu held item on %d left to the game: %s\n", (unsigned long long)f, side,
                              !g_grip_ok[side] ? "controller pose lost" : "scripted move at a door");
    }
    if (side < 0 || side > 1 || !g_grip_ok[side] || !hands_on() || (g_scripted && door_at())) return false;

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
    // Link's eye in game-camera space; his bones sit around it (head to feet), any other character's farther away.
    const float *v = g_rig.game_view;
    float e[3];
    for (int r = 0; r < 3; r++) e[r] = v[r * 4] * g_rig.link[0] + v[r * 4 + 1] * g_rig.link[1] + v[r * 4 + 2] * g_rig.link[2] + v[r * 4 + 3];
    if (!sky_vr_is_scene_projection(slot0, size0)) {  // (the shadow pass keeps Link: his shadow stays)
        if (bones && bone_bytes >= 48) {  // (log: skinned draws at him in another pass - once each)
            float nearest = 1e30f;
            for (int i = 0; i < (int)(bone_bytes / 48); i++) {
                const float *b = bones + i * 12;
                const float dx = b[3] - e[0], dy = b[7] - e[1], dz = b[11] - e[2];
                nearest = fminf(nearest, dx * dx + dy * dy + dz * dz);
            }
            static uint64_t said[32];
            static int ns;
            const uint64_t h = mesh_hash(vb, vb_size) ^ count ^ (size0 << 40);
            bool seen = false;
            for (int i = 0; i < ns; i++) seen |= said[i] == h;
            if (nearest < 60.0f * 60.0f && !seen && ns < 32) {
                said[ns++] = h;
                fprintf(stderr, "[link-log] skinned %llu indices, %d bones, %.0f from his eye, in another pass (camera block "
                        "%llu bytes): shown\n", (unsigned long long)count, (int)(bone_bytes / 48), sqrtf(nearest),
                        (unsigned long long)size0);
            }
        }
        return false;
    }
    // Rigid draws: only the tiny ones at the eye point are Link's (his eyes, measured in his room: four of 42 vertices
    // there); equipment and effects elsewhere can't be told from Link by proximity and stay.
    if (!bones || bone_bytes < 48) {
        if (link_piece_excluded(count, vb_size)) return false;
        const uint64_t hash = mesh_hash(vb, vb_size);
        {   // (on the sword's or shield's own joint this frame: the item's, never his)
            extern uint64_t renderer_present_count(void);
            const uint64_t now = renderer_present_count() + 1;
            const float *m = slot0 + 16;
            for (int k = 0; k < HELD_KINDS; k++) {
                if (g_held[k].frame != now) continue;
                bool same = true;
                for (int j = 0; same && j < 12; j++) same = fabsf(m[j] - g_held[k].block[j]) <= 1e-4f * (1.0f + fabsf(m[j]));
                if (same) { mark_not_link(hash, count, vb_size); return false; }
            }
            if (not_link_mesh(hash, count, vb_size)) return false;
        }
        {   // (on the same joint as a piece of his drawn this frame or the last: his too, however small - the bits next
            // to his head drawn with the pieces there)
            static float blocks[8][12];
            static uint64_t block_frame[8];
            static int nb;
            extern uint64_t renderer_present_count(void);
            const uint64_t now = renderer_present_count() + 1;
            const float *m = slot0 + 16;
            if (known_link_mesh(hash, count, vb_size)) {
                const float px = slot0[19] - e[0], py = slot0[23] - e[1], pz = slot0[27] - e[2];
                const float d2 = px * px + py * py + pz * pz;
                if (d2 > LINK_FORGET_DISTANCE * LINK_FORGET_DISTANCE && !mirrored_block(slot0)) {
                    forget_link_mesh(hash, count, vb_size, sqrtf(d2));  // (drawn far from him: someone else's)
                    return false;
                }
            // (only a real joint: a block that is just the game camera's view - a model at the world's origin, as many
            // static props are drawn - is shared by everything drawn that way. A piece of his once seen with it had
            // every such prop taken for his and hidden: a tester's dormitory lost its furniture, 153 meshes, 329k log
            // lines, 2026-11-10)
            bool plain = g_rig.have_game_view;
            for (int j = 0; plain && j < 12; j++) plain = fabsf(m[j] - g_rig.game_view[j]) <= 1e-3f * (1.0f + fabsf(m[j]));
            if (!plain) memcpy(blocks[nb], m, sizeof blocks[nb]), block_frame[nb] = now, nb = (nb + 1) % 8;
            return true;  // (a piece of his, wherever his body has it now)
        }
        // (only the tiny bits it was for - next to his head, 6 and 12 indices - and not remembered: hidden this frame)
        for (int b = 0; b < 8 && count <= LINK_JOINT_MAX_COUNT; b++) {
            if (!block_frame[b] || block_frame[b] + 1 < now) continue;
            bool same = true;
            for (int j = 0; same && j < 12; j++) same = fabsf(m[j] - blocks[b][j]) <= 1e-4f * (1.0f + fabsf(m[j]));
            if (same) {
                static uint64_t said[32];
                static int ns;
                const uint64_t h = hash ^ count;
                bool seen = false;
                for (int i = 0; i < ns; i++) seen |= said[i] == h;
                if (!seen && ns < 32) {
                    said[ns++] = h;
                    fprintf(stderr, "[link-log] on a joint of his, his: %llu indices, vb %llu bytes\n",
                            (unsigned long long)count, (unsigned long long)vb_size);
                }
                return true;
            }
        }
        }
        const float dx = slot0[19] - e[0], dy = slot0[23] - e[1], dz = slot0[27] - e[2];
        float w[3];  // the offset in the world (view = R | t: R^T d)
        for (int i = 0; i < 3; i++) w[i] = v[0 * 4 + i] * dx + v[1 * 4 + i] * dy + v[2 * 4 + i] * dz;
        // His eyes (tiny, at the eye point) and his belt's rigid pieces (measured: 12 and 216 vertices, 27 and 48 units
        // below the eye point): on his centre line, between chest and knees - part of his body. (His hands hang ~25
        // units to the sides.)
        const bool eyes = count <= LINK_EYE_PART_MAX_VERTS && dx * dx + dy * dy + dz * dz < LINK_EYE_PART_RADIUS * LINK_EYE_PART_RADIUS;
        const bool belt = w[0] * w[0] + w[2] * w[2] < LINK_BELT_RADIUS * LINK_BELT_RADIUS && w[1] < -15.0f && w[1] > -110.0f;
        // (learnt from a wider place - his hips, back, pouches - but hidden there only once it moved with him)
        const float r2 = w[0] * w[0] + w[2] * w[2];
        const bool body = r2 < LINK_PIECE_BODY_RADIUS * LINK_PIECE_BODY_RADIUS && w[1] < -5.0f && w[1] > -140.0f;
        if (eyes || belt || body) link_piece_seen(hash, count, vb_size, w);
        if (!eyes && !belt && !body &&dx * dx + dy * dy + dz * dz < 70.0f * 70.0f) {  // (log: rigid meshes at him left shown - once each)
            static uint64_t said[64];
            static int ns;
            const uint64_t h = hash ^ count;
            bool known = false;
            for (int i = 0; i < ns; i++) known |= said[i] == h;
            if (!known && ns < 64) {
                said[ns++] = h;
                fprintf(stderr, "[link-log] rigid %llu indices, vb %llu bytes, at %+.0f %+.0f %+.0f from his eye "
                        "(%.0f off his centre line): shown\n", (unsigned long long)count, (unsigned long long)vb_size,
                        w[0], w[1], w[2], sqrtf(w[0] * w[0] + w[2] * w[2]));
            }
        }
        return eyes || belt;
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
        if (n < LINK_HEAD_MIN_PALETTE) {
            if (nearest < 70.0f * 70.0f) {  // (log: small skinned meshes drawn at him, left shown - once each)
                static uint64_t said[32];
                static int ns;
                const uint64_t h = mesh_hash(vb, vb_size) ^ count;
                bool known = false;
                for (int i = 0; i < ns; i++) known |= said[i] == h;
                if (!known && ns < 32) {
                    said[ns++] = h;
                    fprintf(stderr, "[link-log] skinned %llu indices, %d bones, vb %llu bytes, %.0f from his eye: shown\n",
                            (unsigned long long)count, n, (unsigned long long)vb_size, sqrtf(nearest));
                }
            }
            return false;
        }
        // (remembered: in scripted moves - a door - his animated head leaves the eye point, which stays put)
        bool near = nearest < LINK_HEAD_RADIUS * LINK_HEAD_RADIUS;
        float head_d = -1;
        {   // (or around his head as his face has it this frame: hair hanging further from the eye point - the back of
            // his head, a swing or a roll moving his head - was left shown)
            extern uint64_t renderer_present_count(void);
            const uint64_t now = renderer_present_count() + 1;
            if (g_head_frame && g_head_frame + 1 >= now) {
                float c[3] = {0, 0, 0}, w[3];
                for (int i = 0; i < n; i++) c[0] += bones[i * 12 + 3], c[1] += bones[i * 12 + 7], c[2] += bones[i * 12 + 11];
                for (int r = 0; r < 3; r++) c[r] = c[r] / n - v[r * 4 + 3];
                for (int i = 0; i < 3; i++) w[i] = v[0 * 4 + i] * c[0] + v[1 * 4 + i] * c[1] + v[2 * 4 + i] * c[2];
                const float hx = w[0] - g_head_world[0], hy = w[1] - g_head_world[1], hz = w[2] - g_head_world[2];
                head_d = sqrtf(hx * hx + hy * hy + hz * hz);
                near |= head_d < LINK_HAIR_RADIUS;
            }
        }
        const uint64_t hash = mesh_hash(vb, vb_size);
        const bool known = known_link_mesh(hash, count, vb_size);
        if (!near && !known && nearest < 90.0f * 90.0f) {  // (log: head-sized meshes at him left shown - once each)
            static uint64_t said[32];
            static int ns;
            const uint64_t h = hash ^ count;
            bool seen = false;
            for (int i = 0; i < ns; i++) seen |= said[i] == h;
            if (!seen && ns < 32) {
                said[ns++] = h;
                fprintf(stderr, "[link-log] head-sized %llu indices, %d bones, vb %llu bytes, %.0f from his eye, %.0f from his "
                        "head: shown\n", (unsigned long long)count, n, (unsigned long long)vb_size, sqrtf(nearest), head_d);
            }
        }
        bool known_now = known;
        if (known && nearest > LINK_FORGET_DISTANCE * LINK_FORGET_DISTANCE && !mirrored_block(slot0))
            forget_link_mesh(hash, count, vb_size, sqrtf(nearest)), known_now = false;
        // (learnt only centred on him: an enemy's face or hair close to his isn't)
        if (near && !known_now && palette_sideways(bones, n, v) < LINK_HEAD_CENTRE) remember_link_mesh(hash, count, vb_size), known_now = true;
        if (!known_now && !(near && nearest < LINK_HEAD_RADIUS * LINK_HEAD_RADIUS)) return false;
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
    if (body && nearest > LINK_FORGET_DISTANCE * LINK_FORGET_DISTANCE && !mirrored_block(slot0))
        forget_link_mesh(hash, count, vb_size, sqrtf(nearest)), body = false;
    // (learnt only centred on him: an enemy in sword reach has a bone this near his eye too, but centres away from him)
    const bool centred = nearest < LINK_BONE_RADIUS * LINK_BONE_RADIUS && palette_sideways(bones, n, v) < LINK_BODY_CENTRE;
    if (!body && centred) {
        remember_link_mesh(hash, count, vb_size), body = true;
        fprintf(stderr, "[link-log] his body: %llu indices, %d bones, vb %llu bytes\n", (unsigned long long)count, n,
                (unsigned long long)vb_size);
    }
    if (centred) {  // (his body at the eye point: spawn settling)
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

