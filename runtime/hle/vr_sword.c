// VR: the sword's hits follow the blade in your hand (VR Options > Sword Hits Follow Your Hand; [VR] sword_hitbox).
//
// The game keeps Link's sword as a swept line inside his actor, at +0x66e4 (his actor: the pointer at main+0x18299d0):
// four world positions - the blade's tip (80 units along the sword model's +X from its hilt), a point near its base
// (12 units along), and both of last frame's. Its hits are taken from that line: moving it 2 m up (SKYRECOMP_SWORD_SHIFT
// below) stops a swing from cutting the grass in front of him. Link's update (main+0xab7cf4) writes it every frame;
// it is called through his vtable, so it can be wrapped (hle_override). Right after it runs, while VR runs in 1st
// person with the right controller tracked and the sword drawn, the line becomes the blade held by the right
// controller: along the grip's forward axis (-z, the way a held sword points), from BASE_AT to TIP_AT game units out,
// this frame's and the last frame's. The game still decides when Link swings (its motion controls); where the blade
// is when it does is now your hand's.
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
#define BASE_AT 12.0f   // game units along the blade from the grip: the game's own base point
#define TIP_AT 80.0f    // ... and its tip
#define DRAWN_AT 30.0f  // the line's base further than this from Link's eye point: drawn (sheathed: 19-20)
#define SHEATHED_AT 25.0f
#define SWING_SPEED 2.0f  // the controller's blade tip faster than this (m/s): its hits are on

extern bool vr_running(void);
extern uint64_t renderer_present_count(void);

static GuestFn g_player_update;
static _Atomic bool g_spatial;  // (the sword out in 1st person: the Joy-Con's turn is the blade's - JoyConTranslator)
bool vr_sword_spatial(void) { return atomic_load(&g_spatial); }
static float g_shift[3];
static bool g_shift_on;

static bool sword_drawn(const float *line, const float *e) {
    static bool drawn;
    static int count;
    const float d[3] = {line[3] - e[0], line[4] - e[1], line[5] - e[2]};
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const bool want = drawn ? !(dist < SHEATHED_AT) : dist > DRAWN_AT;  // (a margin, and some patience: a swing
    if (want != drawn) {                                                 // past the head doesn't sheathe it)
        if (++count >= (drawn ? 10 : 3)) drawn = want, count = 0;
    } else count = 0;
    return drawn;
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
    if (!player_eye(eye) && !vr_player_eye_cached(eye)) return;
    static int said;
    if (!said++) LOG("[sword] Link's update in play (called with %llx)\n", (unsigned long long)pl);
    float *line = (float *)(uintptr_t)(pl + SWORD_LINE);  // tip, base, last tip, last base
    if (g_shift_on) {
        for (int p = 0; p < 4; p++) for (int i = 0; i < 3; i++) line[p * 3 + i] += g_shift[i];
        return;
    }
    static float last[6];
    static bool have_last;
    float grip[12];
    const bool on = g_vr_config.sword_hitbox && g_vr_config.view_mode == 1 && vr_running() && sword_drawn(line, eye) &&
                    vr_rig_controller_pose(1, grip);
    atomic_store(&g_spatial, on && g_vr_config.sword_swing_hits);
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
    // When the game swings is the game's own slash: the sword's attack colliders (Link +0x12c8, +0x14b8, +0x16a8)
    // are only in its collision checks during one - setting their "attacking" bit outside a slash changed nothing
    // (measured headset-free). So the slash itself is made to follow the blade: while the sword is out the right
    // Joy-Con's turn the game reads is the blade's turn in space (g_spatial; runtime/nvn/vulkan/JoyConTranslator.cpp),
    // the arm's swing included, rather than the wrist's.
    {
        static uint64_t last_ns;
        const uint64_t now_ns = SDL_GetTicksNS();
        const float dt = last_ns && now_ns > last_ns ? (float)(now_ns - last_ns) * 1e-9f : 0.0f;
        last_ns = now_ns;
        const float dx = blade[0] - last[0], dy = blade[1] - last[1], dz = blade[2] - last[2];
        const float speed = dt > 0 && dt < 0.25f ? sqrtf(dx * dx + dy * dy + dz * dz) / dt / vr_rig_units_per_metre() : 0.0f;
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
