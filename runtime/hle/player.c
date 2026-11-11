// Link's eye point (Skyward Sword HD), for the VR camera rig's 1st-person anchor.
//
// main+0x1829a30  vec3: Link's position at eye height (x, z of Link; y = the floor under him + 150), no animation
//                 bob. A fixed global in the main module, verified 2026-10-25 in the Knight Academy: Link's room and
//                 the hallway (a different area load), standing, walking and running (LARECOMP_FIND_MOVE,
//                 LARECOMP_MEMDUMP). The same block holds pointers into Link's actor (main+0x18299d0 and its
//                 neighbours), which stays at one address across those areas; the pointer is checked as a sign the
//                 block is live.
// (Superseded: main+0x182a9b0 tracked Link in his room only; after the next area load it pointed at a static
// object.)
// SKYRECOMP_PLAYER_TRACE=1 logs it every 30 frames.
//
// 0x7002bfda90    the game camera's view matrix, row-major 3x4 (world -> camera), stored three times in a row
//                 (+0x30, +0x60). In the game's arena (allocated at boot, same address every run); no pointer to it
//                 from main's static data was found. Found 2026-10-25 by searching memory for the scene block view
//                 whose camera sat ~390 units behind Link in Skyloft. Draw blocks carry this view times a model
//                 matrix, so it is the only reliable source of the camera itself. The VR rig (runtime/vr_rig.c)
//                 trusts it only while some draw's view equals it (this frame or the last).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#define PLAYER_EYE (MAIN_BASE + 0x1829a30)
#define PLAYER_ACTOR_REF (MAIN_BASE + 0x18299d0)

static bool heap_ptr(uint64_t p) { return p > MAIN_BASE + 0x2000000 && p < 0x7400000000ull && !(p & 3); }

// A game just loaded: until Link first moves, that whole block is unset (the actor's pointer 0, the eye point 0 -
// measured 2026-11-10: a save loaded and left alone stayed so for a minute, and was filled the frame the stick was
// pushed) - the VR view stayed the game's 3rd-person camera, and the controls took it for the title screen, until he
// took a step. Then Link himself is read: the player as the game's own objects find him, *(main+0x2bf9940) (vtable
// main+0x1637d28), his position at +0x144 (x, the floor under him, z - the eye point is that + 150 once the block is
// filled: the same numbers). Not on the title screen, which also has him (stage F000, the byte at main+0x2bf98fb
// 0x1c there; 0 / 1 in a game).
#define PLAYER_PTR (MAIN_BASE + 0x2bf9940ull)
#define PLAYER_VTABLE (MAIN_BASE + 0x1637d28ull)
static const float *player_position_live(void) {
    const uint64_t p = *(const uint64_t *)(uintptr_t)PLAYER_PTR;
    if (!heap_ptr(p) || (p & 7) || *(const uint64_t *)(uintptr_t)p != PLAYER_VTABLE) return NULL;
    if (!memcmp((const void *)(uintptr_t)(MAIN_BASE + 0x2bf98d8ull), "F000", 5) && *(const uint8_t *)(uintptr_t)(MAIN_BASE + 0x2bf98fbull) == 0x1c)
        return NULL;  // (the title screen's)
    const float *q = (const float *)(uintptr_t)(p + 0x144);
    for (int i = 0; i < 3; i++)
        if (!isfinite(q[i]) || fabsf(q[i]) > 1e6f) return NULL;
    return q[0] == 0 && q[1] == 0 && q[2] == 0 ? NULL : q;
}

static bool player_yaw_live(float *yaw) {
    if (!g_is_skyward_100) return false;
    const uint64_t p = *(const uint64_t *)(uintptr_t)PLAYER_PTR;
    if (!heap_ptr(p) || (p & 7) || *(const uint64_t *)(uintptr_t)p != PLAYER_VTABLE) return false;
    if (yaw) *yaw = *(const int16_t *)(uintptr_t)(p + 0x13e) * (float)(M_PI / 32768.0);
    return true;
}

static bool player_eye_live(float eye[3]) {
    if (!g_is_skyward_100) return false;  // (game 1.0.0's addresses: not another version's - runtime/main.c)
    if (!heap_ptr(*(const uint64_t *)(uintptr_t)PLAYER_ACTOR_REF)) {  // (no Link - title, loading - or not moved yet)
        const float *q = player_position_live();
        if (!q) return false;
        if (eye) eye[0] = q[0], eye[1] = q[1] + 150.0f, eye[2] = q[2];
        return true;
    }
    const float *e = (const float *)(uintptr_t)PLAYER_EYE;
    for (int i = 0; i < 3; i++)
        if (!isfinite(e[i]) || fabsf(e[i]) > 1e6f) return false;
    if (e[0] == 0 && e[1] == 0 && e[2] == 0) return false;
    if (eye) memcpy(eye, e, 12);
    return true;
}

// Link's actor (the pointer at main+0x18299d0) +0x154: float, the world height of his feet (on land the eye point is
// this + 150; in water the eye point stays at the surface while this goes below it, deep when diving). Measured
// 2026-10-25 in Skyloft's lake (SKYRECOMP_PLAYER_SNAP).
static bool player_feet_y_live(float *y) {
    if (!g_is_skyward_100) return false;  // (game 1.0.0's addresses: not another version's - runtime/main.c)
    const uint64_t actor = *(const uint64_t *)(uintptr_t)PLAYER_ACTOR_REF;
    if (!heap_ptr(actor)) {  // (not moved yet since the load - above)
        const float *q = player_position_live();
        if (q && y) *y = q[1];
        return q != NULL;
    }
    const float v = *(const float *)(uintptr_t)(actor + 0x154);
    if (!isfinite(v) || fabsf(v) > 1e6f) return false;
    if (y) *y = v;
    return true;
}

// Link's actor +0x118: u32, his current action. Measured 2026-11-04 in his room: 0x58 standing, 0x57 / 0x1b / 0x14 /
// 0x15 starting and walking, 0x9 pushed against a wall, 0x6a at a door he can open (A opens it; with it the actor's
// +0x14c and +0x314 change too, which a wall leaves alone).
int player_action(void) {
    if (!g_is_skyward_100) return -1;
    const uint64_t actor = *(const uint64_t *)(uintptr_t)PLAYER_ACTOR_REF;
    if (!heap_ptr(actor)) return -1;
    return (int)*(const volatile uint32_t *)(uintptr_t)(actor + 0x118);
}

// The Beetle in flight, launch to catch: Link's actor +0x74 = 0x0f (his item state: 0x1a aiming it, 0x2f standing)
// and +0x8d4c = 1 (set at the launch, cleared ~100 frames after the catch). Measured 2026-11-09 headset-free with
// SKYRECOMP_LINK_DIFF / SKYRECOMP_WORD_WATCH (two launches: both words change on the launch frame, +0x74 goes back to
// 0x1a the frame the Beetle is in his hand again, return flight included). (main+0x1829ea8 bit 16 is the game camera
// following it: it ends ~200 frames in, before the return.)
// Also - the main sign, since a release was missed sometimes in VR (motion controls: the item state needn't be 0x0f) -
// Link's actor +0x7d38 points at the Beetle actor (vtable main+0x1570498) exactly from the launch to the catch (held,
// it is at +0x7d98 instead and +0x7d38 is 0). Measured 2026-11-09 the same way, through steering, piloting and a recall.
#define BEETLE_VTABLE (MAIN_BASE + 0x1570498)
static bool player_beetle_flying_live(void) {
    if (!g_is_skyward_100) return false;  // (game 1.0.0's addresses: not another version's - runtime/main.c)
    const uint64_t actor = *(const uint64_t *)(uintptr_t)PLAYER_ACTOR_REF;
    if (!heap_ptr(actor)) return false;
    const uint64_t b = *(const volatile uint64_t *)(uintptr_t)(actor + 0x7d38);
    if (heap_ptr(b) && *(const volatile uint64_t *)(uintptr_t)b == BEETLE_VTABLE) return true;
    return *(const volatile uint32_t *)(uintptr_t)(actor + 0x74) == 0x0f &&
           *(const volatile uint32_t *)(uintptr_t)(actor + 0x8d4c) == 1;
}

#define GAME_CAMERA_VIEW (MAIN_BASE + 0x2bfda90ull)

// The boss door's key puzzle: from the A that examines the lock to the B that backs out (or the door opening), Link's
// actor +0x9478 points at the boss door's lock object (vtable main+0x1601018); otherwise it is 0 (and the same object
// sits at +0x9360 while he only stands at it). Measured 2026-11-10 headset-free at Skyview's boss door, two visits
// (SKYRECOMP_WORD_WATCH "p9478": set on the frame of the A, cleared on the frame of the B, both times).
#define BOSS_LOCK_VTABLE (MAIN_BASE + 0x1601018)
static bool player_boss_lock_live(void) {
    if (!g_is_skyward_100) return false;  // (game 1.0.0's addresses: not another version's - runtime/main.c)
    const uint64_t actor = *(const uint64_t *)(uintptr_t)PLAYER_ACTOR_REF;
    if (!heap_ptr(actor)) return false;
    const uint64_t d = *(const volatile uint64_t *)(uintptr_t)(actor + 0x9478);
    return heap_ptr(d) && *(const volatile uint64_t *)(uintptr_t)d == BOSS_LOCK_VTABLE;
}

static bool game_camera_view_live(float view[12]) {
    if (!g_is_skyward_100) return false;  // (game 1.0.0's addresses: not another version's - runtime/main.c)
    const float *v = (const float *)(uintptr_t)GAME_CAMERA_VIEW;
    float m[12];
    memcpy(m, v, sizeof m);
    for (int i = 0; i < 12; i++)
        if (!isfinite(m[i]) || fabsf(m[i]) > 1e7f) return false;
    for (int r = 0; r < 3; r++) {  // orthonormal rotation rows
        const float *a = m + r * 4, *b = m + ((r + 1) % 3) * 4;
        if (fabsf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] - 1) > 1e-3f || fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 1e-3f)
            return false;
    }
    if (view) memcpy(view, m, sizeof m);
    return true;
}

// ---- snapshots (nvn/async.c). With the command replay on its own thread, the game thread is already building the
// next frame while a frame is drawn: what the draws need from game memory is taken when their list is submitted, and
// the replay thread reads that instead of live memory.
typedef struct PlayerSnap {
    bool eye_ok, feet_ok, cam_ok, beetle, boss_lock, yaw_ok;
    float eye[3], feet, cam[12], yaw;
} PlayerSnap;
static __thread const PlayerSnap *tl_snap;

size_t player_snapshot_size(void) { return sizeof(PlayerSnap); }
void player_snapshot_take(void *out) {
    PlayerSnap *s = out;
    s->eye_ok = player_eye_live(s->eye);
    s->feet_ok = player_feet_y_live(&s->feet);
    s->cam_ok = game_camera_view_live(s->cam);
    s->beetle = player_beetle_flying_live();
    s->boss_lock = player_boss_lock_live();
    s->yaw_ok = player_yaw_live(&s->yaw);
}
void player_snapshot_use(const void *snap) { tl_snap = snap; }  // (this thread; NULL: live memory again)

bool player_eye(float eye[3]) {
    if (!tl_snap) return player_eye_live(eye);
    if (tl_snap->eye_ok && eye) memcpy(eye, tl_snap->eye, sizeof tl_snap->eye);
    return tl_snap->eye_ok;
}

// The way Link faces (radians about the vertical; 0: along +z, a quarter turn: along +x): the player's angle, s16 at
// +0x13e of *(main+0x2bf9940) (a door's code compares its own +0x13e with the direction to him). Read 2026-11-11 from a
// running game with the camera in front of him: with it, the hand the game draws on his right is the one holding
// the sword.
bool player_yaw(float *yaw) {
    if (!tl_snap) return player_yaw_live(yaw);
    if (tl_snap->yaw_ok && yaw) *yaw = tl_snap->yaw;
    return tl_snap->yaw_ok;
}

bool player_feet_y(float *y) {
    if (!tl_snap) return player_feet_y_live(y);
    if (tl_snap->feet_ok && y) *y = tl_snap->feet;
    return tl_snap->feet_ok;
}
bool player_beetle_flying(void) { return tl_snap ? tl_snap->beetle : player_beetle_flying_live(); }
bool player_boss_lock(void) { return tl_snap ? tl_snap->boss_lock : player_boss_lock_live(); }

bool game_camera_view(float view[12]) {
    if (!tl_snap) return game_camera_view_live(view);
    if (tl_snap->cam_ok && view) memcpy(view, tl_snap->cam, sizeof tl_snap->cam);
    return tl_snap->cam_ok;
}

// (game thread, once per frame) SKYRECOMP_PLAYER_TRACE=1
void player_trace_tick(uint64_t frame) {
    static int on = -1;
    if (on < 0) on = getenv("SKYRECOMP_PLAYER_TRACE") != NULL;
    {   // (RE) SKYRECOMP_PLAYER_SNAP=<file>: every 30 frames, Link's actor (0x3000 bytes from its pointer - 0x800) and
        // the player globals (main+0x1829800, 0x800 bytes), to find state flags (e.g. swimming) by diffing
        static FILE *snap;
        static int init;
        if (!init) {
            init = 1;
            const char *p = getenv("SKYRECOMP_PLAYER_SNAP");
            if (p) snap = fopen(p, "wb");
        }
        float e[3];
        if (snap && frame % 30 == 0 && player_eye(e)) {
            const uint64_t actor = *(const uint64_t *)(uintptr_t)PLAYER_ACTOR_REF;
            const uint64_t hdr[2] = {frame, actor};
            fwrite(hdr, sizeof hdr, 1, snap);
            fwrite(e, sizeof e, 1, snap);
            fwrite((const void *)(uintptr_t)(actor - 0x800), 0x3000, 1, snap);
            fwrite((const void *)(uintptr_t)(MAIN_BASE + 0x1829800), 0x800, 1, snap);
            fflush(snap);
        }
    }
    if (!on || frame % 30) return;
    float e[3];
    {   // (and what it is read from: the actor's pointer, the global as it is, the actor's own position - x +0x148,
        // feet +0x154, z +0x150)
        const uint64_t actor = *(const uint64_t *)(uintptr_t)PLAYER_ACTOR_REF;
        const float *g = (const float *)(uintptr_t)PLAYER_EYE;
        const float *p = heap_ptr(actor) ? (const float *)(uintptr_t)(actor + 0x148) : NULL;
        if (player_eye(e)) LOG("[player] frame %llu: eye %.1f %.1f %.1f", (unsigned long long)frame, e[0], e[1], e[2]);
        else LOG("[player] frame %llu: none", (unsigned long long)frame);
        LOG(" | actor %#llx global %.1f %.1f %.1f actor's %.1f %.1f(+0x14c) %.1f, feet %.1f\n", (unsigned long long)actor, g[0], g[1], g[2],
            p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, p ? p[3] : 0);
        // (the player as the game's objects find him - the door's code reads *(main+0x2bf9940) + 0x144 / +0x14c)
        const uint64_t pl = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x2bf9940ull);
        const float *q = heap_ptr(pl) ? (const float *)(uintptr_t)(pl + 0x140) : NULL;
        {   // (the stage's name - the door's create compares the string at main+0x2bf98d8 - and the words around it)
            char name[17] = {0};
            memcpy(name, (const void *)(uintptr_t)(MAIN_BASE + 0x2bf98d8ull), 16);
            for (int i = 0; i < 16; i++)
                if (name[i] && (name[i] < 32 || name[i] > 126)) name[i] = '?';
            const uint8_t *b = (const uint8_t *)(uintptr_t)(MAIN_BASE + 0x2bf98f0ull);
            LOG("[player]   stage '%s' +0x2bf98f0: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", name, b[0], b[1],
                b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }
        LOG("[player]   *(main+0x2bf9940) = %#llx vtable %#llx | +0x140: %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n", (unsigned long long)pl,
            (unsigned long long)(q ? *(const uint64_t *)(uintptr_t)pl : 0), q ? q[0] : 0, q ? q[1] : 0, q ? q[2] : 0, q ? q[3] : 0, q ? q[4] : 0,
            q ? q[5] : 0, q ? q[6] : 0);
    }
}
