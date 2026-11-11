// The door Link stands at (Skyward Sword HD), for VR's physical door opening (runtime/vr/vr_input.c, vr_rig.c).
//
// A door's leaf has a world matrix, row-major 3x4 (rotation | translation), in the door object's heap block - kept in
// several copies, recomputed by the game every frame. Measured 2026-11-04 on Link's room's door: closed, the leaf's
// translation is the door's position on the floor (790.6, 0, -2183) and its axes are level (the y axis straight up);
// the game's door sequence turns it about ~60 degrees about the hinge. No pointer from Link's actor to the door was
// found, so the matrix is found by scanning the object heap when Link reaches a door (his action 0x6a, player.c) and
// followed from then on while it stays a level, orthonormal matrix near him.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

extern int player_action(void);
extern bool player_eye(float eye[3]);

#define HEAP_LO (MAIN_BASE + 0x5000000ull)
#define HEAP_HI (MAIN_BASE + 0x8000000ull)
#define DOOR_NEAR 160.0f  // (units, level: the door's origin from Link's eye point; measured ~60 at the door)

static uint64_t g_door;  // the matrix's address; 0: none
static uint64_t g_door_actor_found;  // (its door object, found with it - below)
// How far VR holds the door open (radians, about its vertical through its origin - the leaf's draw turns about it in
// the game's own sequence; measured -22.5 -> -82 degrees of its x axis). vr_input.c sets it; vr_rig.c draws it.
#include <stdatomic.h>
static _Atomic float g_open;
// The door's sound, opening (true) or shutting (false) - the game's own door sounds
// (any thread: played on the game thread - door_sound_tick). Measured 2026-11-04 from the game's own door sequence:
// f_7000f4b8c0(sound source, id, 1, 0); the wooden door's SE_Door_W_OPEN 0x9d6 / SE_Door_W_CLOSE 0x9d7 (WZSound.brsar)
static uint64_t g_door_snd;  // (the door's sound source - found with the door)
static _Atomic int g_sound_request;  // (0 none, else the sound id)
void door_sound(bool open) { atomic_store(&g_sound_request, open ? 0x9d6 : 0x9d7); }
void door_sound_locked(void) { atomic_store(&g_sound_request, 0x9d5); }  // (SE_Door_LOCKED)
void door_sound_tick(Cpu *c) {
    const int id = atomic_exchange(&g_sound_request, 0);
    if (!id || !g_door_snd || *(const uint64_t *)(uintptr_t)g_door_snd != (MAIN_BASE + 0x168ede0ull)) return;
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    const uint64_t sp = c->sp, pc = c->pc;
    c->x[0] = g_door_snd, c->x[1] = (uint64_t)id, c->x[2] = 1, c->x[3] = 0;
    guest_call(c, (MAIN_BASE + 0xf4b8c0ull));
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp, c->pc = pc;
    LOG("[door] sound %#x\n", id);
}
static _Atomic bool g_closing;  // (swinging shut by itself - door_close; a hand's set stops it)
void door_set_open(float radians) { atomic_store(&g_closing, false); atomic_store(&g_open, radians); }
static bool g_test_closed;  // (the test door, once shut by itself, is left to close)
void door_close(void) { if (atomic_load(&g_open) != 0.0f) atomic_store(&g_closing, true), g_test_closed = true; }
float door_open(void) { return atomic_load(&g_open); }
static float g_door_m[12];
static void door_find_tris(const float m[12]);
static void door_tris_set(bool flat);
static void door_find_prisms(const float m[12]);
// (the copy found is turned 180 degrees from the drawn leaf: its x and z axes are taken negated)
static bool g_door_flip;
static void take(const float *m) {
    memcpy(g_door_m, m, sizeof g_door_m);
    if (g_door_flip)
        for (int r = 0; r < 3; r++) g_door_m[r * 4 + 0] = -g_door_m[r * 4 + 0], g_door_m[r * 4 + 2] = -g_door_m[r * 4 + 2];
}

static bool level_matrix(const float *m) {
    for (int i = 0; i < 12; i++)
        if (!isfinite(m[i]) || fabsf(m[i]) > 1e6f) return false;
    // orthonormal rows, the y axis up
    for (int r = 0; r < 3; r++) {
        const float *a = m + r * 4, *b = m + ((r + 1) % 3) * 4;
        if (fabsf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] - 1) > 2e-3f) return false;
        if (fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 2e-3f) return false;
    }
    return fabsf(m[5] - 1) < 2e-3f;
}

static bool near_link(const float *m, const float eye[3]) {
    const float dx = m[3] - eye[0], dz = m[11] - eye[2];
    return dx * dx + dz * dz < DOOR_NEAR * DOOR_NEAR && eye[1] - m[7] > 100 && eye[1] - m[7] < 220;
}

static uint64_t scan(const float eye[3]) {
    const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);  // (his own matrices follow him)
    uint64_t best = 0;
    float best_d = 1e30f;
    for (uint64_t a = HEAP_LO; a < HEAP_HI; a += 4) {
        const float *m = (const float *)(uintptr_t)a;
        // cheap tests first: translation near, then the rotation
        const float dx = m[3] - eye[0], dz = m[11] - eye[2];
        if (!(dx * dx + dz * dz < DOOR_NEAR * DOOR_NEAR)) continue;
        if (a >= link - 0x1000 && a < link + 0x8000) continue;
        if (dx * dx + dz * dz < 25.0f * 25.0f) continue;  // (at Link himself: his own)
        if (!near_link(m, eye) || !level_matrix(m)) continue;
        if (fabsf(m[0] - 1) < 1e-6f && fabsf(m[10] - 1) < 1e-6f) continue;  // (an identity-turned helper, not a leaf)
        {   // (not one inside a door object - its own matrix at +0x1f4 faces the other way: the leaf's draw matrix is
            // in a block of its own)
            bool in_door = false;
            for (uint64_t o = (a & ~7ull); o + 0x900 > a && o > HEAP_LO && !in_door; o -= 8)
                in_door = *(const uint64_t *)(uintptr_t)o == (MAIN_BASE + 0x1600da0ull);
            if (in_door) continue;
        }
        const float d = dx * dx + dz * dz;
        if (d < best_d) best_d = d, best = a;
    }
    return best;
}

// (once per frame, vr_rig.c)
void door_test_open(void);
// The level's door objects (vtable DOOR_VTABLE, position at +0x144): found a slice of the heap per frame (a whole
// scan at once is a hitch in the headset), dropped once their vtable is gone (the room unloaded)
#define DOOR_VTABLE_ (MAIN_BASE + 0x1600da0ull)
#define DOOR_KEEP 500.0f  // (units: a door left open stays open until Link is this far from it)
#define DOOR_REACH 150.0f  // (units, level: Link's eye from the door's origin - ~60 standing at it)
static uint64_t g_doors[64];
static int g_ndoors;
static bool door_object_near(const float eye[3]) {
    static uint64_t cursor = HEAP_LO;
    const uint64_t end = cursor + (HEAP_HI - HEAP_LO) / 32;
    for (uint64_t a = cursor; a < end; a += 8)
        if (*(const uint64_t *)(uintptr_t)a == DOOR_VTABLE_) {
            bool known = false;
            for (int i = 0; i < g_ndoors && !known; i++) known = g_doors[i] == a;
            if (!known && g_ndoors < 64) {
                g_doors[g_ndoors++] = a;
                if (getenv("SKYRECOMP_DOOR_CENSUS")) {  // (RE: each door object's fields, to tell the locked ones)
                    const uint8_t *b = (const uint8_t *)(uintptr_t)a;
                    char line[1200];
                    int n = snprintf(line, sizeof line, "[door] census %#llx at %.0f %.0f %.0f:", (unsigned long long)a,
                                     *(const float *)(b + 0x144), *(const float *)(b + 0x148), *(const float *)(b + 0x14c));
                    static const int ranges[][2] = {{0x0, 0x40}, {0xb0, 0xd0}, {0x780, 0x800}};
                    for (int r = 0; r < 3; r++) {
                        n += snprintf(line + n, sizeof line - n, " |%x:", ranges[r][0]);
                        for (int o = ranges[r][0]; o < ranges[r][1]; o += 4) n += snprintf(line + n, sizeof line - n, " %08x", *(const uint32_t *)(b + o));
                    }
                    LOG("%s\n", line);
                }
            }
        }
    cursor = end >= HEAP_HI ? HEAP_LO : end;
    bool near = false;
    for (int i = 0; i < g_ndoors; i++) {
        if (*(const uint64_t *)(uintptr_t)g_doors[i] != DOOR_VTABLE_) { g_doors[i--] = g_doors[--g_ndoors]; continue; }
        const float *p = (const float *)(uintptr_t)(g_doors[i] + 0x144);
        const float dx = p[0] - eye[0], dz = p[2] - eye[2];
        near |= dx * dx + dz * dz < DOOR_REACH * DOOR_REACH && eye[1] - p[1] > 100 && eye[1] - p[1] < 220;
    }
    return near;
}

// Link is at the door (within reach of it, either side): the gesture's
// The door is locked: asked of the game's own test (the door's vtable entry 31, f_700078c9a0(door) - door_lock_tick,
// below). The lock type (+0x7b9) alone does not say: disassembled 2026-11-10, by type (+0x7ba its scene flag)
//   0, 2: never locked          1, 3: locked until the flag is set (no flag, 0xff: always)
//   4: locked while the flag is set (no flag: always)
//   5: locked while the byte at 0x7002bf98fd is 0          6: locked while it is 1
// and a door with bit 6 of +0xc set is locked while it holds something at +0x760. Its handle still turns; it does not
// open.
static _Atomic bool g_locked;
bool door_locked(void) { return g_door && atomic_load(&g_locked); }
bool door_at(void) {
    float eye[3];
    if (!g_door || !player_eye(eye)) return false;
    const float dx = g_door_m[3] - eye[0], dz = g_door_m[11] - eye[2];
    return dx * dx + dz * dz < DOOR_REACH * DOOR_REACH;
}

// The searches for a door (its leaf, its object, its collision - ~40 ms, and the first look for the rooms' .kcl
// files ~1.7 s) run on a thread of their own: on the frame's thread they stopped the headset's picture
#include <pthread.h>
static uint64_t g_door_job;           // (the leaf found - adopted by door_track)
static _Atomic int g_job_state;       // 0 idle, 1 running, 2 done
static float g_job_eye[3];
static void *door_job(void *arg) {
    (void)arg;
    float eye[3];
    memcpy(eye, g_job_eye, sizeof eye);
    const uint64_t t0 = plat_now_ns();
    g_door_job = scan(eye);
    LOG("[door] timing: leaf scan %.1f ms\n", (plat_now_ns() - t0) / 1e6);
    if (g_door_job) {
        g_door_flip = false;
        take((const float *)(uintptr_t)g_door_job);
        extern uint64_t door_find_actor(const float m[12]);
        g_door_actor_found = door_find_actor(g_door_m);
        const uint64_t t1 = plat_now_ns();
        door_tris_set(false), door_find_tris(g_door_m);
        const uint64_t t2 = plat_now_ns();
        door_find_prisms(g_door_m);
        LOG("[door] timing: object %.1f ms, triangles %.1f ms, kcl %.1f ms\n", (t1 - t0) / 1e6, (t2 - t1) / 1e6, (plat_now_ns() - t2) / 1e6);
        if (getenv("SKYRECOMP_DOOR_KCL")) {  // (RE: the rooms' collision in memory - the doorway's prisms 914 / 8771)
            static const uint8_t pat[2][16] = {
                {0x43, 0x86, 0x97, 0x0a, 0x01, 0x2a, 0x08, 0x6d, 0x08, 0x72, 0x08, 0x6f, 0x00, 0x7a, 0x00, 0x08},
                {0x43, 0x86, 0x97, 0x0a, 0x09, 0xbb, 0x06, 0xb2, 0x61, 0xfb, 0x06, 0xb3, 0x00, 0x04, 0x00, 0x09}};
            uint8_t le[2][16];
            for (int k = 0; k < 2; k++) {  // (little-endian: the float and each u16 swapped)
                le[k][0] = pat[k][3], le[k][1] = pat[k][2], le[k][2] = pat[k][1], le[k][3] = pat[k][0];
                for (int j = 4; j < 16; j += 2) le[k][j] = pat[k][j + 1], le[k][j + 1] = pat[k][j];
            }
            extern uint64_t kernel_heap_size(void);
            extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);
            uint64_t ra[64], rs[64];
            const int nr = kernel_data_regions(ra, rs, 64);
            for (int r = 0; r < nr; r++) LOG("[door] region %#llx + %#llx\n", (unsigned long long)ra[r], (unsigned long long)rs[r]);
            for (int r = 0; r < nr; r++)
                for (uint64_t a = ra[r]; a + 16 <= ra[r] + rs[r]; a += 4)
                    for (int k = 0; k < 2; k++) {
                        if (!memcmp((const void *)(uintptr_t)a, pat[k], 16)) LOG("[door] kcl prism %s (big-endian) at %#llx\n", k ? "8771" : "914", (unsigned long long)a);
                        if (!memcmp((const void *)(uintptr_t)a, le[k], 16)) LOG("[door] kcl prism %s (little-endian) at %#llx\n", k ? "8771" : "914", (unsigned long long)a);
                    }
            {   // the kcl2 copy: room 1's triangle 913 (the doorway's), 36 bytes - flattened with its neighbour 914 if
                // SKYRECOMP_DOOR_KCL_OFF, every triangle of the room if SKYRECOMP_DOOR_KCL_ALL
                static const uint8_t tri[36] = {0x85, 0x2b, 0x57, 0x44, 0x0a, 0x97, 0x86, 0x43, 0x9a, 0xed, 0x0a, 0xc5,
                                                0x85, 0x2b, 0x57, 0x44, 0x00, 0x00, 0x00, 0x00, 0x9a, 0xed, 0x0a, 0xc5,
                                                0xf8, 0x36, 0x32, 0x44, 0x00, 0x00, 0x00, 0x00, 0xea, 0x19, 0x07, 0xc5};
                for (int r = 0; r < nr; r++)
                    for (uint64_t a = ra[r]; a + 36 <= ra[r] + rs[r]; a += 4)
                        if (!memcmp((const void *)(uintptr_t)a, tri, 36)) {
                            LOG("[door] kcl2 triangle 913 at %#llx\n", (unsigned long long)a);
                            const char *only = getenv("SKYRECOMP_DOOR_KCL_COPY");  // (test: only the copy in this range)
                            if (only && (a >> 28) != strtoull(only, NULL, 16)) continue;  // (e.g. 700 or 701)
                            if (getenv("SKYRECOMP_DOOR_KCL_OFF")) {
                                float *t = (float *)(uintptr_t)a;
                                const int count = getenv("SKYRECOMP_DOOR_KCL_ALL") ? 929 : 2;
                                float *t0 = getenv("SKYRECOMP_DOOR_KCL_ALL") ? t - 9 * 913 : t;
                                for (int i = 0; i < count; i++)
                                    for (int j = 3; j < 9; j++) t0[9 * i + j] = t0[9 * i + j % 3];  // (all three corners at the first)
                                LOG("[door] kcl2: %d triangles flattened\n", count);
                            }
                        }
            }
            LOG("[door] kcl search done (heap %#llx bytes)\n", (unsigned long long)kernel_heap_size());
            if (getenv("SKYRECOMP_DOOR_KCL_OFF"))  // (test: the found prisms and the next one flattened)
                for (int r = 0; r < nr; r++)
                    for (uint64_t a = ra[r]; a + 16 <= ra[r] + rs[r]; a += 4)
                        for (int k = 0; k < 2; k++)
                            if (!memcmp((const void *)(uintptr_t)a, le[k], 16)) {
                                *(float *)(uintptr_t)a = 0.0f, *(float *)(uintptr_t)(a + 16) = 0.0f;
                                if (getenv("SKYRECOMP_DOOR_KCL_ALL") && k == 0)  // (test: every prism of Link's room)
                                    for (int i = 1; i < 930; i++) *(float *)(uintptr_t)(a - 0x10 * 914 + 0x10 * i) = 0.0f;
                                LOG("[door] flattened prisms at %#llx\n", (unsigned long long)a);
                            }
        }
        if (getenv("SKYRECOMP_DOOR_REFS"))  // (RE: the colliders' words, as hex and float)
            for (uint64_t o = 0x670; o < 0x738; o += 0x10) {
                const uint32_t *w = (const uint32_t *)(uintptr_t)(g_door_actor_found + o);
                LOG("[door] +%03llx: %08x %08x %08x %08x | %.2f %.2f %.2f %.2f\n", (unsigned long long)o, w[0], w[1], w[2], w[3],
                    *(const float *)&w[0], *(const float *)&w[1], *(const float *)&w[2], *(const float *)&w[3]);
            }
        if (getenv("SKYRECOMP_DOOR_REFS")) {  // (RE: the vtables' entries)
            static const uint64_t vts[] = {(MAIN_BASE + 0x1600da0ull), (MAIN_BASE + 0x166e508ull), (MAIN_BASE + 0x164ce50ull), (MAIN_BASE + 0x164d6a0ull), (MAIN_BASE + 0x1649878ull)};
            for (int v = 0; v < 5; v++) {
                char line[1600];
                int n = snprintf(line, sizeof line, "[door] vtable %#llx:", (unsigned long long)vts[v]);
                for (int e = 0; e < 40; e++) n += snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)*(const uint64_t *)(uintptr_t)(vts[v] + 8 * e));
                LOG("%s\n", line);
            }
        }
        if (getenv("SKYRECOMP_DOOR_REFS"))  // (RE: matrices at the door inside the door object)
            for (uint64_t o = 0; o < 0x900; o += 4) {
                const float *mm = (const float *)(uintptr_t)(g_door_actor_found + o);
                if (fabsf(mm[3] - g_door_m[3]) < 2 && fabsf(mm[11] - g_door_m[11]) < 2 && level_matrix(mm))
                    LOG("[door] matrix in the door object at +%#llx (x axis %.3f %.3f)\n", (unsigned long long)o, mm[0], mm[8]);
            }
        if (getenv("SKYRECOMP_DOOR_REFS"))  // (RE: what points at the door object - its collision body)
            for (uint64_t a = HEAP_LO; a < HEAP_HI; a += 8)
                for (int k = 0; k < 1 && g_door_actor_found; k++)
                    if (*(const uint64_t *)(uintptr_t)a == g_door_actor_found) {
                        uint64_t vt = 0, at = 0;
                        for (uint64_t o = a & ~7ull; o + 0x400 > a && !vt; o -= 8) {
                            const uint64_t v = *(const uint64_t *)(uintptr_t)o;
                            if (v >= (MAIN_BASE + 0x1400000ull) && v < (MAIN_BASE + 0x1800000ull)) vt = v, at = o;
                        }
                        LOG("[door] ref to %#llx at %#llx (object %#llx +%#llx, vtable %#llx)\n",
                            (unsigned long long)g_door_actor_found, (unsigned long long)a, (unsigned long long)at,
                            (unsigned long long)(a - at), (unsigned long long)vt);
                    }
        g_door_snd = 0;  // (its sound source: vtable 0x700168ede0, 0x720 before the leaf's matrix in its block)
        for (uint64_t o = g_door_job & ~7ull; o + 0x2000 > g_door_job && !g_door_snd; o -= 8)
            if (*(const uint64_t *)(uintptr_t)o == (MAIN_BASE + 0x168ede0ull)) g_door_snd = o;
        if (g_door_actor_found) {
            // (the door object faces the other way from the drawn leaf: its yaw, s16 at +0x132, minus 180 degrees,
            // is the leaf's - a copy turned the other way is turned back)
            const float yaw = *(const int16_t *)(uintptr_t)(g_door_actor_found + 0x132) * (float)(M_PI / 32768) - (float)M_PI;
            const float leaf = atan2f(-g_door_m[8], g_door_m[0]);
            float d = leaf - yaw;
            while (d > (float)M_PI) d -= 2 * (float)M_PI;
            while (d < -(float)M_PI) d += 2 * (float)M_PI;
            if (fabsf(d) > (float)(M_PI / 2)) g_door_flip = true, take((const float *)(uintptr_t)g_door_job);
        }
        LOG("[door] door at %.1f %.1f %.1f (matrix %#llx, x axis %.3f %.3f)\n", g_door_m[3], g_door_m[7], g_door_m[11],
            (unsigned long long)g_door_job, g_door_m[0], g_door_m[8]);
    }
    atomic_store(&g_job_state, 2);
    return NULL;
}

void door_track(void) {
    door_test_open();
    {   // (diagnostics) reaching / leaving a door
        static bool was;
        const bool at = door_at();
        if (at != was) {
            float e[3] = {0, 0, 0};
            player_eye(e);
            float h[3] = {0, 0, 0};
            extern bool door_handle(float w[3]);
            if (at) door_handle(h);
            LOG("[door] %s the door (eye %.1f %.1f, handle %.1f %.1f %.1f)\n", at ? "at" : "left", e[0], e[2], h[0], h[1],
                h[2]);
            was = at;
        }
    }
    float eye[3];
    if (!player_eye(eye)) { g_door = 0; return; }
    if (g_door) {
        const float *m = (const float *)(uintptr_t)g_door;
        const float kx = m[3] - eye[0], kz = m[11] - eye[2];  // (kept while Link is within DOOR_KEEP of it)
        if (level_matrix(m) && kx * kx + kz * kz < DOOR_KEEP * DOOR_KEEP) take(m);
        else g_door = 0, door_set_open(0);  // (Link far from it: the door left open shuts)
    }
    static uint64_t next_scan;
    extern uint64_t renderer_present_count(void);
    const uint64_t now = renderer_present_count();
    // (Link's +0x118 is not his action but the collision polygon he touches - 0x6a was one wall of his room - so a
    // door is found from the door objects instead: from either side, any door)
    if (!g_door && atomic_load(&g_job_state) == 2) {  // (a search finished: its door adopted)
        g_door = g_door_job, atomic_store(&g_job_state, 0);
    }
    if (!g_door && door_object_near(eye) && now >= next_scan && atomic_load(&g_job_state) == 0) {
        next_scan = now + 30;
        memcpy(g_job_eye, eye, sizeof g_job_eye);
        atomic_store(&g_job_state, 1);
        pthread_t th;
        if (pthread_create(&th, NULL, door_job, NULL) == 0) pthread_detach(th);
        else atomic_store(&g_job_state, 0);
    }
}

// (test) SKYRECOMP_DOOR_TEST_OPEN=<degrees>: the door Link is at, held open that far
void door_test_open(void) {
    static int on = -1;
    static float deg;
    if (on < 0) on = getenv("SKYRECOMP_DOOR_TEST_OPEN") != NULL, deg = on ? (float)atof(getenv("SKYRECOMP_DOOR_TEST_OPEN")) : 0;
    if (on && !g_test_closed) door_set_open(g_door ? deg * (float)(M_PI / 180) : 0.0f);
    {   // (test) SKYRECOMP_DOOR_TEST_GRAB=<0 left / 1 right>: that hand holds the handle while at a door
        static int grab = -2;
        if (grab == -2) grab = getenv("SKYRECOMP_DOOR_TEST_GRAB") ? atoi(getenv("SKYRECOMP_DOOR_TEST_GRAB")) : -1;
        extern void vr_rig_door_grab(int side);
        if (grab >= 0) vr_rig_door_grab(g_door ? grab : -1);
    }
    static bool requested;  // (and the room beyond, once - as a pulled door does)
    void door_request_room(void);
    if (on && g_door && !requested) requested = true, door_request_room(), door_sound(true);
    static int step_at = -1;  // (SKYRECOMP_DOOR_TEST_STEP=<frames>: and Link stepped through that long after)
    if (step_at < 0) step_at = getenv("SKYRECOMP_DOOR_TEST_STEP") ? atoi(getenv("SKYRECOMP_DOOR_TEST_STEP")) : 0;
    static int since;
    void door_request_step(void);
    if (on && requested && step_at > 0 && ++since == step_at) door_request_step();
}

// The leaf's world matrix (row-major 3x4) of the door Link is at. False: none.
bool door_matrix(float m[12]) {
    if (!g_door) return false;
    memcpy(m, g_door_m, sizeof g_door_m);
    return true;
}

// The handle (its knob) of the door Link is at, in the world, on his side of it. The door's frame (measured on his
// room's door): the leaf spans x -70 (the hinge, found from how the leaf turns) .. +70, its plane is z = 0, y up; the
// knob sits near the free edge at hand height.
// (measured 2026-11-04: the controllers held on the knob in VR sat at x 42..62, y 115, z 17..21 from the door's
// side - the grip's centre, in the hand around the knob)
#define HANDLE_X 52.0f
#define DOOR_HINGE_X (-70.0f)  // (the leaf turns about its edge away from the knob)
float door_hinge_x(void) { return DOOR_HINGE_X; }
#define HANDLE_Y 115.0f
#define HANDLE_Z 19.0f
// The door has a handle on each face (z +-HANDLE_Z in its frame, turning with the leaf). The one held: g_handle_face
// (vr_input.c sets it at the grab - the one nearer the hand); 0: the face on Link's side (his z in the door's frame)
static float g_handle_face;
void door_set_handle_face(float face) { g_handle_face = face; }
static bool handle_face_at(float face, float out_z, float w[3]) {
    float m[12], eye[3];
    if (!door_matrix(m) || !player_eye(eye)) return false;
    const float d[3] = {eye[0] - m[3], eye[1] - m[7], eye[2] - m[11]};
    const float side = face != 0.0f ? face : m[2] * d[0] + m[6] * d[1] + m[10] * d[2] < 0 ? -1.0f : 1.0f;
    // (turned with the leaf: Ry(open) in the door's frame - x' = c x + s z, z' = -s x + c z)
    // (about the hinge, x = DOOR_HINGE_X: l = h + Ry (l0 - h))
    const float o = door_open(), c = cosf(o), s = sinf(o), lx = HANDLE_X - DOOR_HINGE_X, lz = out_z * side;
    const float l[3] = {DOOR_HINGE_X + c * lx + s * lz, HANDLE_Y, -s * lx + c * lz};
    for (int r = 0; r < 3; r++) w[r] = m[r * 4 + 0] * l[0] + m[r * 4 + 1] * l[1] + m[r * 4 + 2] * l[2] + m[r * 4 + 3];
    return true;
}
static bool handle_at(float out_z, float w[3]) { return handle_face_at(g_handle_face, out_z, w); }
bool door_handle(float w[3]) { return handle_at(HANDLE_Z, w); }
// The held handle's knob and the way its face looks (out of the door, turned with the leaf), in the world - for the hand
// put on it (vr_rig.c): palm to the door, fingers up, as the game's own door sequence holds it
bool door_handle_frame(float knob[3], float out_n[3]) {
    float m[12], eye[3];
    if (!door_matrix(m) || !player_eye(eye) || !handle_face_at(g_handle_face, HANDLE_Z, knob)) return false;
    const float d[3] = {eye[0] - m[3], eye[1] - m[7], eye[2] - m[11]};
    const float face = g_handle_face != 0.0f ? g_handle_face : m[2] * d[0] + m[6] * d[1] + m[10] * d[2] < 0 ? -1.0f : 1.0f;
    const float o = door_open(), c = cosf(o), s = sinf(o);
    const float nl[3] = {s * face, 0, c * face};  // (Ry(open) of the face's z)
    for (int r = 0; r < 3; r++) out_n[r] = m[r * 4 + 0] * nl[0] + m[r * 4 + 1] * nl[1] + m[r * 4 + 2] * nl[2];
    return true;
}
// A handle's knob (face +1 / -1; 0: the held one) in the world, the face resolved, and the swung leaf's axes in the world
// (columns: the door's x, y, z turned by the open angle)
bool door_handle_pose(float face, float knob[3], float *face_out, float axes[9]) {
    float m[12], eye[3];
    if (!door_matrix(m) || !player_eye(eye)) return false;
    if (face == 0.0f) face = g_handle_face;
    if (face == 0.0f) {
        const float d[3] = {eye[0] - m[3], eye[1] - m[7], eye[2] - m[11]};
        face = m[2] * d[0] + m[6] * d[1] + m[10] * d[2] < 0 ? -1.0f : 1.0f;
    }
    if (!handle_face_at(face, HANDLE_Z, knob)) return false;
    const float o = door_open(), c = cosf(o), s = sinf(o);
    const float ry[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) axes[r * 3 + k] = m[r * 4 + 0] * ry[0 * 3 + k] + m[r * 4 + 1] * ry[1 * 3 + k] + m[r * 4 + 2] * ry[2 * 3 + k];
    *face_out = face;
    return true;
}
// The handle on one face (+1 / -1) - for reaching either
bool door_handle_on(float face, float w[3]) { return handle_face_at(face, HANDLE_Z, w); }
// A point in the world -> the door's frame (its closed frame: R^T (p - t)); false: no door
bool door_local(const float p[3], float l[3]) {
    float m[12];
    if (!door_matrix(m)) return false;
    const float d[3] = {p[0] - m[3], p[1] - m[7], p[2] - m[11]};
    for (int i = 0; i < 3; i++) l[i] = m[0 * 4 + i] * d[0] + m[1 * 4 + i] * d[1] + m[2 * 4 + i] * d[2];
    return true;
}
// The handle in the door's closed frame, on Link's side (x, z)
bool door_handle_local(float *x, float *z) {
    float m[12], eye[3];
    if (!door_matrix(m) || !player_eye(eye)) return false;
    const float d[3] = {eye[0] - m[3], eye[1] - m[7], eye[2] - m[11]};
    const float side = m[2] * d[0] + m[6] * d[1] + m[10] * d[2] < 0 ? -1.0f : 1.0f;
    *x = HANDLE_X, *z = HANDLE_Z * side;
    return true;
}
// Where the hand holding the handle is drawn: the grip's centre further out from the door, the hand's mesh clear of it
#define HANDLE_HAND_Z 30.0f
bool door_handle_hand(float w[3]) { return handle_at(HANDLE_HAND_Z, w); }

// ---- the room beyond the door (no door event). Measured 2026-11-04 (Link's room -> the academy hallway):
// a door object (vtable 0x7001600da0) keeps its position at +0x144 / +0x148 / +0x14c and the two rooms it joins at
// +0x7bc / +0x7bd (bytes; 1 and 0 there). The door event activates the far room with f_7000e0854c(rooms, to, from):
// rooms = *(0x7002bf9d90), its room objects at +0xf0 + 8 * index; it clears the room's inactive bits (+0x9f0, 0xa
// when inactive) and marks the rooms around it - the far room's objects are then built, as when the door opens.
#define DOOR_VTABLE (MAIN_BASE + 0x1600da0ull)
#define ROOMS_PTR (MAIN_BASE + 0x2bf9d90ull)
#define g_door_actor g_door_actor_found
static _Atomic int g_room_request;

// (several objects of that type can stand at one door: all kept; door_rooms takes the one joining an active room to
// an inactive one)
static uint64_t g_door_actors[8];
static int g_door_actor_count;
uint64_t door_find_actor(const float m[12]) {
    g_door_actor_count = 0;
    for (uint64_t a = HEAP_LO; a < HEAP_HI && g_door_actor_count < 8; a += 8) {
        if (*(const uint64_t *)(uintptr_t)a != DOOR_VTABLE) continue;
        const float *p = (const float *)(uintptr_t)(a + 0x144);
        // (same floor too: the door straight above, on the next floor, is another room's - y 600 over Link's room's)
        if (fabsf(p[0] - m[3]) < 5 && fabsf(p[2] - m[11]) < 5 && fabsf(p[1] - m[7]) < 50) {
            g_door_actors[g_door_actor_count++] = a;
            LOG("[door] door object %#llx at %.1f %.1f %.1f yaw %04x rooms %d %d\n", (unsigned long long)a, p[0], p[1], p[2],
                *(const uint16_t *)(uintptr_t)(a + 0x132), *(const uint8_t *)(uintptr_t)(a + 0x7bc),
                *(const uint8_t *)(uintptr_t)(a + 0x7bd));
        }
    }
    return g_door_actor_count ? g_door_actors[0] : 0;
}

static uint64_t room_object(int index) {
    const uint64_t rooms = *(const uint64_t *)(uintptr_t)ROOMS_PTR;
    if (rooms < (MAIN_BASE + 0x0ull) || rooms >= 0x7800000000ull || index < 0 || index > 63) return 0;
    const uint64_t r = *(const uint64_t *)(uintptr_t)(rooms + 0xf0 + 8 * (uint64_t)index);
    return r >= (MAIN_BASE + 0x0ull) && r < 0x7800000000ull ? r : 0;
}

// (hle/vr_world.c, the game thread, every frame) Is the door Link is at locked: the game's own test, on each door
// object standing there - locked if any says so (wrongly shut is the safe side: the game's own door prompt still works)
void door_lock_tick(Cpu *c) {
    bool locked = false;
    char said[160] = "";
    int len = 0;
    if (g_door && atomic_load(&g_job_state) != 1)  // (not while a search is filling the list)
        for (int i = 0; i < g_door_actor_count; i++) {
            const uint64_t d = g_door_actors[i];
            if (*(const uint64_t *)(uintptr_t)d != DOOR_VTABLE) continue;  // (gone with its room)
            uint64_t saved[31];
            memcpy(saved, c->x, sizeof saved);
            const uint64_t sp = c->sp, pc = c->pc;
            c->x[0] = d;
            guest_call(c, (MAIN_BASE + 0x78c9a0ull));
            const bool l = (c->x[0] & 0xff) != 0;
            memcpy(c->x, saved, sizeof saved);
            c->sp = sp, c->pc = pc;
            locked |= l;
            if (len < (int)sizeof said - 40)
                len += snprintf(said + len, sizeof said - len, " [type %d flag %#x: %s]", *(const uint8_t *)(uintptr_t)(d + 0x7b9),
                                *(const uint8_t *)(uintptr_t)(d + 0x7ba), l ? "locked" : "open");
        }
    static uint64_t was_door;
    static bool was_locked;
    if (g_door && len && (g_door != was_door || locked != was_locked)) {
        LOG("[door] lock: %s%s (time byte %d)\n", locked ? "locked" : "not locked", said, *(const uint8_t *)(uintptr_t)(MAIN_BASE + 0x2bf98fdull));
        was_door = g_door, was_locked = locked;
    }
    if (!g_door) was_door = 0;
    atomic_store(&g_locked, locked);
    static int census = -1;  // (RE, SKYRECOMP_DOOR_CENSUS: the game's answer for every door object of the level, as it changes)
    if (census < 0) census = getenv("SKYRECOMP_DOOR_CENSUS") != NULL;
    if (census) {
        // (its own list, found here every few seconds - a hitch: g_doors is the frame thread's, and VR's only)
        static uint64_t list[64], seen[64];
        static int nlist;
        static int8_t seen_locked[64];
        static unsigned tick;
        if (tick++ % 300 == 0) {
            nlist = 0;
            for (uint64_t a = HEAP_LO; a < HEAP_HI && nlist < 64; a += 8)
                if (*(const uint64_t *)(uintptr_t)a == DOOR_VTABLE) list[nlist++] = a;
        }
        for (int i = 0; i < nlist; i++) {
            const uint64_t d = list[i];
            if (*(const uint64_t *)(uintptr_t)d != DOOR_VTABLE) continue;
            uint64_t saved[31];
            memcpy(saved, c->x, sizeof saved);
            const uint64_t sp = c->sp, pc = c->pc;
            c->x[0] = d;
            guest_call(c, (MAIN_BASE + 0x78c9a0ull));
            const int8_t l = (c->x[0] & 0xff) != 0;
            memcpy(c->x, saved, sizeof saved);
            c->sp = sp, c->pc = pc;
            int k = 0;
            while (k < 64 && seen[k] && seen[k] != d) k++;
            if (k == 64) k = 0, memset(seen, 0, sizeof seen);
            if (seen[k] == d && seen_locked[k] == l) continue;
            seen[k] = d, seen_locked[k] = l;
            const uint8_t *b = (const uint8_t *)(uintptr_t)d;
            LOG("[door] census lock %#llx at %.0f %.0f %.0f: type %d flag %#x room %d rooms %d %d bit6 %d -> %s (time byte %d)\n",
                (unsigned long long)d, *(const float *)(b + 0x144), *(const float *)(b + 0x148), *(const float *)(b + 0x14c),
                b[0x7b9], b[0x7ba], (int8_t)b[0x17c], b[0x7bc], b[0x7bd], (b[0xc] >> 6) & 1, l ? "locked" : "not locked",
                *(const uint8_t *)(uintptr_t)(MAIN_BASE + 0x2bf98fdull));
        }
    }
}

// The door's far room (the one of its two that is inactive) and the near one. False: no door object, or both active.
bool door_rooms(int *to, int *from) {
    if (!g_door_actor) return false;
    for (int i = 0; i < g_door_actor_count; i++) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)g_door_actors[i];
        const int a = d[0x7bd], b = d[0x7bc];
        const uint64_t ra = room_object(a), rb = room_object(b);
        if (!ra || !rb || a == b) continue;
        const bool a_off = *(const uint8_t *)(uintptr_t)(ra + 0x9f0) & 0x1e, b_off = *(const uint8_t *)(uintptr_t)(rb + 0x9f0) & 0x1e;
        if (a_off == b_off) continue;
        *to = a_off ? a : b, *from = a_off ? b : a;
        return true;
    }
    return false;
}

// (vr_input.c, any thread) Activate the room beyond the door: done on the game thread (door_room_tick).
void door_request_room(void) { atomic_store(&g_room_request, 1); }
// The two rooms the door joins, as activated: the one opened (to) and the one he stood in (from), and the side of the
// door he stood on then (his z in its frame). While the door is open both stay active; shut, the one behind him goes.
static int g_room_to = -1, g_room_from = -1;
static float g_room_from_side;
static _Atomic int g_unload_request;
static float link_side(void) {
    float m[12];
    const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);
    if (link < (MAIN_BASE + 0x0ull) || link >= 0x7800000000ull || !door_matrix(m)) return 0;
    const float *p = (const float *)(uintptr_t)(link + 0x148);
    const float lz = m[2] * (p[0] - m[3]) + m[6] * (p[1] - m[7]) + m[10] * (p[2] - m[11]);
    return lz < 0 ? -1.0f : 1.0f;
}
static bool room_active(int index) {
    const uint64_t r = room_object(index);
    return r && !(*(const uint8_t *)(uintptr_t)(r + 0x9f0) & 0x1e);
}
static void activate_room(Cpu *c, int to, int from) {
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    const uint64_t sp = c->sp, pc = c->pc;
    c->x[0] = *(const uint64_t *)(uintptr_t)ROOMS_PTR, c->x[1] = (uint64_t)to, c->x[2] = (uint64_t)from;
    const uint64_t ta = plat_now_ns();
    guest_call(c, (MAIN_BASE + 0xe0854cull));
    LOG("[door] timing: room activation %.1f ms\n", (plat_now_ns() - ta) / 1e6);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp, c->pc = pc;
}

// (hle/vr_world.c, the game thread, inside an object's update)
void door_room_tick(Cpu *c) {
    if (g_room_to >= 0 && *(const uint64_t *)(uintptr_t)ROOMS_PTR) {
        const float side = link_side();
        const int here = side == g_room_from_side ? g_room_from : g_room_to, behind = here == g_room_from ? g_room_to : g_room_from;
        if (atomic_exchange(&g_unload_request, 0)) {
            // shut: the room he is in made current again, as the game's own room change does - the one behind goes
            LOG("[door] shut: room %d current, room %d behind released\n", here, behind);
            activate_room(c, here, behind);
            {   // (that keeps it active - its neighbour: marked inactive as the game had it, 0x0a at +0x9f0)
                const uint64_t r = room_object(behind);
                if (r) *(volatile uint8_t *)(uintptr_t)(r + 0x9f0) |= 0x0a;
            }
            LOG("[door]   room %d now %s\n", behind, room_active(behind) ? "still active" : "inactive");
            g_room_to = g_room_from = -1;
        } else if (door_open() != 0.0f && side != 0 && !room_active(behind)) {
            // (open: the game, seeing him in the new room, let the one behind go - kept while it can be seen)
            static uint64_t said;
            extern uint64_t renderer_present_count(void);
            if (renderer_present_count() > said + 60) LOG("[door] room %d behind kept while the door is open\n", behind), said = renderer_present_count();
            activate_room(c, behind, here);
        }
    }
    if (!atomic_exchange(&g_room_request, 0)) return;
    int to, from;
    const uint64_t rooms = *(const uint64_t *)(uintptr_t)ROOMS_PTR;
    if (!door_rooms(&to, &from) || !rooms) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)g_door_actor;
        const uint64_t ra = d ? room_object(d[0x7bd]) : 0, rb = d ? room_object(d[0x7bc]) : 0;
        LOG("[door] room request: no far room to activate (door object %#llx rooms %d %d -> %#llx %#llx flags %x %x)\n",
            (unsigned long long)g_door_actor, d ? d[0x7bd] : -1, d ? d[0x7bc] : -1, (unsigned long long)ra, (unsigned long long)rb,
            ra ? *(const uint8_t *)(uintptr_t)(ra + 0x9f0) : 0xff, rb ? *(const uint8_t *)(uintptr_t)(rb + 0x9f0) : 0xff);
        return;
    }
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    const uint64_t sp = c->sp, pc = c->pc;
    c->x[0] = rooms, c->x[1] = (uint64_t)to, c->x[2] = (uint64_t)from;
    const uint64_t ta = plat_now_ns();
    guest_call(c, (MAIN_BASE + 0xe0854cull));
    LOG("[door] timing: room activation %.1f ms\n", (plat_now_ns() - ta) / 1e6);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp, c->pc = pc;
    LOG("[door] activated room %d beyond the door (from room %d)\n", to, from);
    g_room_to = to, g_room_from = from, g_room_from_side = link_side();
    extern void door_mark_activated(void);
    door_mark_activated();
}

// ---- stepping through (the door's collision stays where the closed leaf is: the game's own sequence walks Link
// through by animation). Link's actor keeps his position x / z at +0x148 / +0x150, with copies at +0x15c / +0x164 (the
// previous frame's) - measured as the ones that follow him (snapshot diff, his room -> the door).
static _Atomic int g_step_request;
void door_request_step(void) { atomic_store(&g_step_request, 1); }

// (game thread) Link moved through the door: to the far side of its plane, `dist` units beyond it, at the doorway's
// centre line (the door's origin is the doorway's middle - its leaf spans x -70 .. 70)
// The closed door's collision: triangles in the rooms' live collision (the kcl2 triangle lists, 9 floats each - the
// game collides against these; the .kcl copy in memory is not used) lying in the doorway: every corner within the
// leaf's width and a few units of the door's plane, between the floor and the lintel, facing across the doorway.
// Measured 2026-11-05 on Link's room's door: two in the bedroom's list (10 units in), two in the hallway's. Found for
// whichever door Link is at; flattened (all three corners at the first) while the door is open, put back once shut.
#define DOOR_TRIS_MAX 32
static uint64_t g_tri[DOOR_TRIS_MAX];
static float g_tri_orig[DOOR_TRIS_MAX][9];
static int g_ntri;
static bool g_tris_flat;
static void door_find_tris(const float m[12]) {
    g_ntri = 0;
    for (uint64_t a = HEAP_LO; a + 36 <= HEAP_HI && g_ntri < DOOR_TRIS_MAX; a += 4) {
        const float *t = (const float *)(uintptr_t)a;
        // (cheap first: the first corner near the door, level with its floor or above)
        const float dx = t[0] - m[3], dz = t[2] - m[11];
        if (!(dx * dx + dz * dz < 100.0f * 100.0f) || !(t[1] - m[7] > -5.0f && t[1] - m[7] < 285.0f)) continue;
        bool ok = true;
        float l[3][3];
        for (int v = 0; v < 3 && ok; v++) {
            const float px = t[3 * v] - m[3], py = t[3 * v + 1] - m[7], pz = t[3 * v + 2] - m[11];
            if (!isfinite(px) || !isfinite(py) || !isfinite(pz)) { ok = false; break; }
            l[v][0] = m[0] * px + m[4] * py + m[8] * pz;   // (along the leaf)
            l[v][1] = py;
            l[v][2] = m[2] * px + m[6] * py + m[10] * pz;  // (across the doorway)
            ok = fabsf(l[v][0]) < 86.0f && fabsf(l[v][2]) < 20.0f && py > -5.0f && py < 285.0f;  // (measured: +-80, 0..269)
        }
        if (!ok) continue;
        // facing across the doorway (its normal along the door's z), and a real triangle, not a sliver
        const float e1[3] = {l[1][0] - l[0][0], l[1][1] - l[0][1], l[1][2] - l[0][2]};
        const float e2[3] = {l[2][0] - l[0][0], l[2][1] - l[0][1], l[2][2] - l[0][2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 200.0f || fabsf(n[2]) < 0.9f * nl) continue;
        g_tri[g_ntri] = a;
        memcpy(g_tri_orig[g_ntri], t, sizeof g_tri_orig[0]);
        g_ntri++;
        a += 32;  // (past this triangle)
    }
    LOG("[door] the closed door's collision: %d triangles\n", g_ntri);
}
// The same doorway in the rooms' .kcl collision (also used: with only the triangle lists taken out Link still stopped in
// the doorway). Every loaded .kcl is found by its header (little-endian: 0x38, the positions', normals', prisms' and
// octree's offsets in order, a positive thickness - files in archives are 32-byte aligned), its prisms decoded to
// triangles (KCL: the corner, then the others along the edge normals' crossings by the height) and those in the
// doorway flattened by a height of 0.
#define DOOR_PRISMS_MAX 32
static uint64_t g_prism[DOOR_PRISMS_MAX];
static float g_prism_h[DOOR_PRISMS_MAX];
static int g_nprism;
static bool tri_in_doorway(const float m[12], const float c[3][3]) {
    float l[3][3];
    for (int v = 0; v < 3; v++) {
        const float px = c[v][0] - m[3], py = c[v][1] - m[7], pz = c[v][2] - m[11];
        if (!isfinite(px) || !isfinite(py) || !isfinite(pz)) return false;
        l[v][0] = m[0] * px + m[4] * py + m[8] * pz, l[v][1] = py, l[v][2] = m[2] * px + m[6] * py + m[10] * pz;
        if (!(fabsf(l[v][0]) < 86.0f && fabsf(l[v][2]) < 20.0f && py > -5.0f && py < 285.0f)) return false;
    }
    const float e1[3] = {l[1][0] - l[0][0], l[1][1] - l[0][1], l[1][2] - l[0][2]};
    const float e2[3] = {l[2][0] - l[0][0], l[2][1] - l[0][1], l[2][2] - l[0][2]};
    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    return nl >= 200.0f && fabsf(n[2]) >= 0.9f * nl;
}
static void door_find_prisms(const float m[12]) {
    g_nprism = 0;
    // the loaded .kcl files: found once (a full look takes ~1.7 s) and kept while every one still has its header; looked
    // for again when one is gone (another stage)
    static uint64_t kcl[512];
    static int nkcl = -1;
    bool valid = false;  // (through the rooms every time - ~2 ms, and rooms loaded since are in it)
    for (int i = 0; i < nkcl && valid; i++) valid = *(const uint32_t *)(uintptr_t)kcl[i] == 0x38;
    if (!valid) {
        nkcl = 0;
        // first through the rooms: a room object's pointers (and theirs, one level on) to a .kcl's header - only ones
        // into mapped memory are read
        extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);
        uint64_t mra[64], mrs[64];
        const int mnr = kernel_data_regions(mra, mrs, 64);
        #define MAPPED(p, n) ({ bool ok_ = false; for (int q_ = 0; q_ < mnr && !ok_; q_++) ok_ = (p) >= mra[q_] && (p) < mra[q_] + mrs[q_] && (n) <= mra[q_] + mrs[q_] - (p); ok_; })
        const uint64_t t0 = plat_now_ns();
        for (int ri = 0; ri < 64; ri++) {
            const uint64_t room = room_object(ri);
            if (!room) continue;
            for (uint64_t o = 0; o < 0x2000; o += 8) {
                const uint64_t p1 = *(const uint64_t *)(uintptr_t)(room + o);
                if ((p1 & 3) || !MAPPED(p1, 0x400)) continue;
                for (int lvl = 0; lvl < 2; lvl++) {
                    // (level 0: the pointer itself; level 1: the pointers in what it points at)
                    const int count = lvl ? 0x80 : 1;
                    for (int j = 0; j < count; j++) {
                        const uint64_t c = lvl ? *(const uint64_t *)(uintptr_t)(p1 + 8 * j) : p1;
                        if ((c & 3) || !MAPPED(c, 0x40)) continue;
                        const uint32_t *h = (const uint32_t *)(uintptr_t)c;
                        if (h[0] != 0x38 || !(h[1] > 0x38 && h[2] > h[1] && h[3] > h[2] && h[3] < 0x2000000)) continue;
                        bool known = false;
                        for (int k = 0; k < nkcl && !known; k++) known = kcl[k] == c;
                        if (!known && nkcl < 512) kcl[nkcl++] = c;
                    }
                }
            }
        }
        LOG("[door] .kcl collision files through the rooms: %d (%.1f ms)\n", nkcl, (plat_now_ns() - t0) / 1e6);
        if (nkcl) goto have_list;  // (the rooms' own: ~2 ms - the full look below only if none was found that way)
        nkcl = 0;
        extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);
        uint64_t ra[64], rs[64];
        const int nr = kernel_data_regions(ra, rs, 64);
        for (int r = 0; r < nr; r++)
            for (uint64_t a = (ra[r] + 31) & ~31ull; a + 0x40 <= ra[r] + rs[r] && nkcl < 512; a += 32) {
                const uint32_t *h = (const uint32_t *)(uintptr_t)a;
                if (h[0] != 0x38 || !(h[1] > 0x38 && h[2] > h[1] && h[3] > h[2] && h[3] < 0x2000000)) continue;
                const float thick = *(const float *)&h[4];
                if (!(thick > 0.0f && thick < 1000.0f) || a + h[3] > ra[r] + rs[r]) continue;
                kcl[nkcl++] = a;
            }
        LOG("[door] looked for the .kcl collision files: %d\n", nkcl);
    }
have_list:;
    int files = 0;
    for (int f = 0; f < nkcl; f++)
        {
            const uint64_t a = kcl[f];
            const uint32_t *h = (const uint32_t *)(uintptr_t)a;
            files++;
            const float *pos = (const float *)(uintptr_t)(a + h[0]), *nrm = (const float *)(uintptr_t)(a + h[1]);
            const uint32_t npos = (h[1] - h[0]) / 12, nnrm = (h[2] - h[1]) / 12, npr = (h[3] - h[2]) / 16;
            for (uint32_t i = 1; i < npr && g_nprism < DOOR_PRISMS_MAX; i++) {
                const uint8_t *pr = (const uint8_t *)(uintptr_t)(a + h[2] + 16ull * i);
                const float height = *(const float *)pr;
                const uint16_t *ix = (const uint16_t *)(pr + 4);  // (position, face normal, three edge normals)
                if (ix[0] >= npos || ix[1] >= nnrm || ix[2] >= nnrm || ix[3] >= nnrm || ix[4] >= nnrm) continue;
                const float *P = pos + 3 * ix[0], *fn = nrm + 3 * ix[1], *n1 = nrm + 3 * ix[2], *n2 = nrm + 3 * ix[3], *n3 = nrm + 3 * ix[4];
                // (quick: the corner near the door)
                const float qx = P[0] - m[3], qz = P[2] - m[11];
                if (!(qx * qx + qz * qz < 100.0f * 100.0f)) continue;
                const float cA[3] = {fn[1] * n1[2] - fn[2] * n1[1], fn[2] * n1[0] - fn[0] * n1[2], fn[0] * n1[1] - fn[1] * n1[0]};
                const float cB[3] = {fn[1] * n2[2] - fn[2] * n2[1], fn[2] * n2[0] - fn[0] * n2[2], fn[0] * n2[1] - fn[1] * n2[0]};
                const float dA = cA[0] * n3[0] + cA[1] * n3[1] + cA[2] * n3[2], dB = cB[0] * n3[0] + cB[1] * n3[1] + cB[2] * n3[2];
                if (fabsf(dA) < 1e-6f || fabsf(dB) < 1e-6f) continue;
                const float c[3][3] = {{P[0], P[1], P[2]},
                                       {P[0] + cB[0] * height / dB, P[1] + cB[1] * height / dB, P[2] + cB[2] * height / dB},
                                       {P[0] + cA[0] * height / dA, P[1] + cA[1] * height / dA, P[2] + cA[2] * height / dA}};
                if (!tri_in_doorway(m, c)) continue;
                g_prism[g_nprism] = (uint64_t)(uintptr_t)pr, g_prism_h[g_nprism] = height, g_nprism++;
            }
        }
    LOG("[door] the closed door in the .kcl collision: %d prisms (%d collision files)\n", g_nprism, files);
}
static void door_tris_set(bool flat) {
    if (flat == g_tris_flat) return;
    for (int i = 0; i < g_nprism; i++) {
        float *h = (float *)(uintptr_t)g_prism[i];
        if (flat && *h == g_prism_h[i]) *h = 0.0f;
        else if (!flat && *h == 0.0f) *h = g_prism_h[i];
    }
    for (int i = 0; i < g_ntri; i++) {
        float *t = (float *)(uintptr_t)g_tri[i];
        if (flat) {
            if (memcmp(t, g_tri_orig[i], sizeof g_tri_orig[0])) continue;  // (not ours any more: left alone)
            for (int j = 3; j < 9; j++) t[j] = t[j % 3];
        } else if (t[3] == t[0] && t[4] == t[1] && t[5] == t[2]) memcpy(t, g_tri_orig[i], sizeof g_tri_orig[0]);
    }
    g_tris_flat = flat;
}

// Trace (always, near a door that is open): build/door_trace.csv - a frame per line, Link's feet in the door's frame
// (x along the leaf, z across), the open angle, in the doorway, and what held him: 1 the frame's side stopped a step,
// 2 the leaf pushed him. Plotted to see what is in the way.
static int g_trace_why;
float g_door_stick;  // (the left stick's tilt, 0..1 - vr_input.c)
static void door_trace(uint64_t frame, float lx, float lz, bool inside) {
    static FILE *f;
    if (!f) {
        char path[600] = "door_trace.csv";  // (next to the exe, as the log: the working folder depends on the launch)
#if defined(_WIN32)
        extern unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
        char exe[512];
        const unsigned long n = GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = n && n < sizeof exe ? strrchr(exe, '\\') : NULL;
        if (slash) slash[1] = 0, snprintf(path, sizeof path, "%sdoor_trace.csv", exe);
#endif
        f = fopen(path, "a");
        if (!f) return;
        fprintf(f, "frame,x,z,open_deg,inside,why,stick\n");
    }
    fprintf(f, "%llu,%.2f,%.2f,%.1f,%d,%d,%.2f\n", (unsigned long long)frame, lx, lz, door_open() * 57.2958f, inside, g_trace_why,
            g_door_stick);
    fflush(f);
}

void door_step_tick(void) {
    {   // (RE test) SKYRECOMP_DOOR_TEST_RADIUS=<hex offset from Link>,<float>: that float of Link's written every frame
        static int on = -1;
        static long off;
        static float val;
        if (on < 0) {
            const char *e = getenv("SKYRECOMP_DOOR_TEST_RADIUS");
            on = e && sscanf(e, "%lx,%f", &off, &val) == 2;
        }
        const uint64_t lk = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);
        if (on && lk >= (MAIN_BASE + 0x0ull) && lk < 0x7800000000ull) *(volatile float *)(uintptr_t)(lk + (int64_t)off) = val;
    }
    {   // (RE test) SKYRECOMP_DOOR_TEST_LIFTM=<offset hex>: the door object's 3x4 matrix at that offset lifted 500 every
        // frame - does its collision follow it?
        static long off = -2;
        if (off == -2) off = getenv("SKYRECOMP_DOOR_TEST_LIFTM") ? strtol(getenv("SKYRECOMP_DOOR_TEST_LIFTM"), NULL, 16) : -1;
        if (off >= 0 && g_door_actor_found) {
            float *mm = (float *)(uintptr_t)(g_door_actor_found + (uint64_t)off);
            static float y0 = 1e9f;
            if (y0 == 1e9f) y0 = mm[7], LOG("[door] lifting the matrix at +%#lx (y %.1f, x %.1f z %.1f)\n", off, mm[7], mm[3], mm[11]);
            mm[7] = y0 + 500.0f;
        }
    }
    // The door open: Link walks through its frame by the game's own collision, the closed door's triangles taken out
    // of it (door_tris_set). Crossed and out of the doorway on the other side, the door shuts behind him.
    extern uint64_t renderer_present_count(void);
    static uint64_t last_frame;
    const uint64_t frame = renderer_present_count();
    if (frame == last_frame) return;
    last_frame = frame;
    atomic_store(&g_step_request, 0);  // (no scripted step)
    static const int32_t copies[] = {-0x254, -0x44, 0x1c, 0x48, 0x148, 0x15c, 0x220, 0x234, 0x2d8, 0x1634, 0x2940 + 0x144, 0x2940 + 0x1bc};
    enum { NCOPIES = sizeof copies / sizeof *copies };
    static bool inside;
    static float entry_side;
    static int close_in;  // (frames until the door shuts behind him; 0: none)
    float m[12];
    const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);
    if (link < (MAIN_BASE + 0x0ull) || link >= 0x7800000000ull || !door_matrix(m)) { inside = false, door_tris_set(false); return; }
    if (close_in && !--close_in) door_close();
    if (atomic_load(&g_closing)) {  // (swinging shut, easing in, then latched)
        const float o = atomic_load(&g_open), step = 0.01f + 0.03f * fabsf(o);  // (radians a frame)
        if (fabsf(o) <= step) {
            atomic_store(&g_open, 0.0f), atomic_store(&g_closing, false), door_sound(false);
            atomic_store(&g_unload_request, 1);  // (shut: the room behind him can go - door_room_tick)
        }
        else atomic_store(&g_open, o - (o > 0 ? step : -step));
    }
    // his feet in the door's closed frame (x along the leaf, z across the doorway)
    const float *p = (const float *)(uintptr_t)(link + 0x148);
    const float d[3] = {p[0] - m[3], p[1] - m[7], p[2] - m[11]};
    float lx = m[0] * d[0] + m[4] * d[1] + m[8] * d[2]; float lz = m[2] * d[0] + m[6] * d[1] + m[10] * d[2];
    g_trace_why = 0;
    const bool open = fabsf(door_open()) > 3.0f * (float)(M_PI / 180);  // (the doorway is free once it opens)
    if (getenv("SKYRECOMP_DOOR_SPEED") && fabsf(lx) < 150 && fabsf(lz) < 200) {  // (test: his speed near the door)
        static float plx, plz;
        LOG("[door] speed %.1f at %.0f %.0f%s\n", sqrtf((lx - plx) * (lx - plx) + (lz - plz) * (lz - plz)), lx, lz, inside ? " (in)" : "");
        plx = lx, plz = lz;
    }
    {   // the leaf is solid to walk into from the doorway's side - never a push. Its face toward the opening (normal n,
        // +x in the door's frame) takes the part of his step that goes into it (to within R of it); the leaf swung
        // over him moves nothing (he was already there); its face toward the wall's corner, and its ends, let him by
        // - so he is never shut in behind it
        static float pl_x, pl_z;
        static uint64_t pl_frame;
        // (both faces, at about Link's own width - 34, the game's walls hold his centre that far - so his eyes never
        // go into it; the leaf swung over him still moves nothing: from inside R a step is left alone)
        const float o = door_open(), hx = door_hinge_x(), len = -2 * hx, R = 30.0f;
        const float ex = cosf(o), ez = -sinf(o);  // (the leaf's direction in the door's frame: Ry(open) of x)
        float nx = -ez, nz = ex;
        if (fabsf(o) > 0.02f && pl_frame && frame - pl_frame < 4) {
            const float t = (lx - hx) * ex + lz * ez, tp = (pl_x - hx) * ex + pl_z * ez;
            const float along_p0 = (pl_x - hx) * nx + pl_z * nz;
            if (along_p0 < 0) nx = -nx, nz = -nz;  // (the face he was on)
            const float along = (lx - hx) * nx + lz * nz, along_p = fabsf(along_p0);
            if (t > -R && t < len + R && tp > -R && tp < len + R && along_p >= R && along < R) {
                const float back = R - along;  // (no more than his own step brought him in)
                g_trace_why |= 2;
                for (unsigned i = 0; i < NCOPIES; i++) {
                    float *q = (float *)(uintptr_t)(link + (int64_t)copies[i]);
                    q[0] += back * (m[0] * nx + m[2] * nz), q[2] += back * (m[8] * nx + m[10] * nz);
                }
                lx += back * nx, lz += back * nz;
            }
        }
        pl_x = lx, pl_z = lz, pl_frame = frame;
    }
    // the closed door's collision - its triangles in the rooms' collision, found with the door - is taken away while
    // the door is open, and put back once it is shut: he walks through by the game's own collision, nothing else
    door_tris_set(open);
    // through: in the doorway, then out of it on the other side - the door shuts behind him
    const bool in_frame = fabsf(lx) < 110.0f && fabsf(lz) < 90.0f;
    if (in_frame && !inside) inside = true, entry_side = lz < 0 ? -1.0f : 1.0f;
    else if (!in_frame && inside) {
        inside = false;
        const bool crossed = (lz < 0 ? -1.0f : 1.0f) != entry_side && fabsf(lz) >= 90.0f;
        if (crossed) LOG("[door] Link through the doorway (%.0f %.0f): the door shuts behind him\n", lx, lz), close_in = 60;
    }
    if (door_open() != 0.0f && fabsf(lx) < 250 && fabsf(lz) < 250) door_trace(frame, lx, lz, inside);
}

// ---- the door object turned (its collision with it): its rotation, s16 x / y / z at +0x130 and a copy at +0x13c
// (y 0x9000 = 202.5 degrees for Link's room's door, whose leaf faces 22.5). (test) SKYRECOMP_DOOR_TEST_YAW=<degrees>:
// the door object at the door Link is at turned that much, every frame (game thread)
void door_yaw_tick(void) {
    static int on = -1;
    static float deg;
    if (on < 0) on = getenv("SKYRECOMP_DOOR_TEST_YAW") != NULL, deg = on ? (float)atof(getenv("SKYRECOMP_DOOR_TEST_YAW")) : 0;
    if (!on || !g_door_actor_count) return;
    int to, from;
    if (!door_rooms(&to, &from)) {}  // (keeps none: any door object at the door turns)
    static uint16_t base[8];
    static uint64_t based[8];
    for (int i = 0; i < g_door_actor_count; i++) {
        const uint64_t a = g_door_actors[i];
        uint16_t *y0 = (uint16_t *)(uintptr_t)(a + 0x132), *y1 = (uint16_t *)(uintptr_t)(a + 0x13e);
        if (based[i] != a) based[i] = a, base[i] = *y0;
        const uint16_t y = (uint16_t)(base[i] + (int)(deg * 65536.0f / 360.0f));
        *y0 = y, *y1 = y;
    }
}

// (test) SKYRECOMP_DOOR_TEST_POSE=<file of "hex address hex u32" lines>: written every object update - the leaf's
// matrices at the game's fully open pose, to see whether the door's collision follows them
void door_pose_tick(void) {
    static int n = -1;
    static uint64_t addr[256];
    static uint32_t val[256];
    if (n < 0) {
        n = 0;
        const char *p = getenv("SKYRECOMP_DOOR_TEST_POSE");
        FILE *f = p ? fopen(p, "r") : NULL;
        unsigned long long a;
        unsigned v;
        while (f && n < 256 && fscanf(f, "%llx %x", &a, &v) == 2) addr[n] = a, val[n] = v, n++;
        if (f) fclose(f);
        if (n) LOG("[door] test pose: %d values\n", n);
    }
    if (!n || !g_door_actor_count) return;
    for (int i = 0; i < n; i++) *(volatile uint32_t *)(uintptr_t)addr[i] = val[i];
}

// (test) SKYRECOMP_DOOR_TEST_LIFT=<units>: the door objects at the door lifted that far every object update (their
// position y, +0x148) - is Link's "at a door" stop (and the door's collision) theirs?
void door_lift_tick(void) {
    static int on = -1;
    static float up;
    if (on < 0) on = getenv("SKYRECOMP_DOOR_TEST_LIFT") != NULL, up = on ? (float)atof(getenv("SKYRECOMP_DOOR_TEST_LIFT")) : 0;
    if (!on) return;
    static float base[8];
    static uint64_t based[8];
    for (int i = 0; i < g_door_actor_count; i++) {
        float *y = (float *)(uintptr_t)(g_door_actors[i] + 0x148);
        if (based[i] != g_door_actors[i]) based[i] = g_door_actors[i], base[i] = *y;
        *y = base[i] + up;
    }
}

// (test) SKYRECOMP_DOOR_TEST_LINK=<hex offset>,<hex u32>: Link's actor field held at that value every object update
// once a door's room was activated - which of the fields the game's door sequence changes lets him walk through?
static bool g_room_activated;
void door_link_tick(void) {
    static int on = -1;
    static unsigned off, val;
    if (on < 0) on = getenv("SKYRECOMP_DOOR_TEST_LINK") && sscanf(getenv("SKYRECOMP_DOOR_TEST_LINK"), "%x,%x", &off, &val) == 2;
    if (!on || !g_room_activated) return;
    const uint64_t link = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x18299d0);
    if (link >= (MAIN_BASE + 0x0ull) && link < 0x7800000000ull) *(volatile uint32_t *)(uintptr_t)(link + off) = val;
}

void door_mark_activated(void) { g_room_activated = true; }

// ---- recording the player's door use, to see what they see: while Link is at a door or one is open, the headset's
// eye 0 every 20 frames -> door_rec/<frame>.png next to the exe (device.c), the frame matching door_trace.csv's.
// (frames accumulate: the folder is cleared between sessions by hand)
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif
bool door_rec_path(uint64_t frame, char *out, size_t n) {
    static int on = -1;  // (off unless SKYRECOMP_DOOR_REC: saving a frame is a stutter)
    if (on < 0) on = getenv("SKYRECOMP_DOOR_REC") != NULL;
    if (!on || frame % 20 || !g_door || !(door_open() != 0.0f || door_at())) return false;
    static char dir[600];
    if (!dir[0]) {
        snprintf(dir, sizeof dir, "door_rec");
#if defined(_WIN32)
        extern unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
        char exe[512];
        const unsigned long k = GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = k && k < sizeof exe ? strrchr(exe, '\\') : NULL;
        if (slash) slash[1] = 0, snprintf(dir, sizeof dir, "%sdoor_rec", exe);
        _mkdir(dir);
#else
        mkdir(dir, 0755);
#endif
    }
    snprintf(out, n, "%s/%06llu.png", dir, (unsigned long long)frame);
    return true;
}
