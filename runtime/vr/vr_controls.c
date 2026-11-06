// The VR controllers as the game's Joy-Con pair: buttons and sticks, and the motion sensors (vr_input.c feeds the
// translator; runtime/hle/input.c and runtime/hle/motion.c read these on the game thread).
#include "vr_internal.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <SDL3/SDL_timer.h>
#include <stdlib.h>
#include <string.h>

#if SKYRECOMP_HAS_OPENXR
#include "hle.h"
#include "nvn/vulkan/JoyConTranslator.h"

static bool controllers_off(void) {  // (test) SKYRECOMP_VR_NO_INPUT=1: controllers ignored, for scripted runs
    static int off = -1;
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_INPUT") != NULL;
    return off;
}

_Static_assert(sizeof(VrJoyConState) == sizeof(SkyJoyConState), "the same layout as JoyConTranslator.h");
_Static_assert(sizeof(VrJoyConSensor) == sizeof(SkyJoyConSensor), "the same layout as JoyConTranslator.h");

// ---- the menu pointer: on the title / file select screens (no Link in the world) and while a menu pauses the game
// (the gear menu, the map...), the game's cursor goes where the right controller's ray meets the floating HUD panel.
// The game turns the right Joy-Con's orientation, relative to its canonical centre, into a cursor like a flat screen:
// offset = K tan(angle), with K = 1.298 screen widths horizontally (yaw about the Joy-Con's z) and 2.316 screen
// heights vertically (pitch about x; down positive) - measured 2026-10-26 with SKYRECOMP_JOYCON_TEST=point. The
// panel's hit (u, v) gives those angles back, clamped to the panel's edges (panel_uv). Outside the title / file
// select and paused menus the real orientation goes to the game (the sword follows it).
// SKYRECOMP_VR_PANEL_POINTER=0 turns it off.
#define POINTER_KX 1.298f
#define POINTER_KY 2.316f
static pthread_mutex_t g_ptr_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_ptr_aim[16], g_ptr_hud[16];  // (row-major world-from-local: the right aim pose; the panel, half extents)
static bool g_ptr_aim_ok, g_ptr_hud_ok;

// The Joy-Con direction (rows: its axes in the player's frame) that puts the game's cursor at (u, v) of the screen
// (0..1, top left): a turn about its own z then x from the canonical centre.
void vr_pointer_direction(float u, float v, float out[9]);  // (also motion.c tests)
void vr_pointer_direction(float u, float v, float out[9]) {
    const float a = atanf((u - 0.5f) / POINTER_KX), b = atanf((v - 0.5f) / POINTER_KY);
    const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
    const float yaw[9] = {ca, -sa, 0, sa, ca, 0, 0, 0, 1}, pitch[9] = {1, 0, 0, 0, cb, -sb, 0, sb, cb};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            out[r * 3 + k] = pitch[r * 3] * yaw[k] + pitch[r * 3 + 1] * yaw[3 + k] + pitch[r * 3 + 2] * yaw[6 + k];
}

// (vr_input.c, once per frame) The right controller's aim pose in the local space.
void vr_pointer_set_aim(bool tracked, const float q[4], const float p[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float m[16] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p[0],
                         2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p[1],
                         2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), p[2],
                         0, 0, 0, 1};
    pthread_mutex_lock(&g_ptr_lock);
    g_ptr_aim_ok = tracked;
    if (tracked) memcpy(g_ptr_aim, m, sizeof m);
    pthread_mutex_unlock(&g_ptr_lock);
}

// (vr.c, each frame the HUD panel is placed) The panel the menu pointer aims at (world-from-panel, half extents).
void vr_controls_set_hud_panel(const float m[16]) {
    pthread_mutex_lock(&g_ptr_lock);
    memcpy(g_ptr_hud, m, sizeof g_ptr_hud);
    g_ptr_hud_ok = true;
    pthread_mutex_unlock(&g_ptr_lock);
}

bool vr_game_paused(void) { return *(volatile uint32_t *)(uintptr_t)(MAIN_BASE + 0x1827370) != 0; }  // (vr.c)
// (vr.c) Paused, or within half a second after: a closing menu keeps fading out once the game runs again - the panel
// holds still for it (it swung back to follow the head mid-fade: a flash of the menu at another angle).
bool vr_game_paused_recently(void) {
    extern uint64_t renderer_present_count(void);
    static uint64_t last = ~0ull;
    const uint64_t now = renderer_present_count();
    if (vr_game_paused()) last = now;
    return last != ~0ull && now - last < 30;
}
static bool panel_pointer_on(void) {
    static int env = -1;
    if (env < 0) env = getenv("SKYRECOMP_VR_PANEL_POINTER") ? atoi(getenv("SKYRECOMP_VR_PANEL_POINTER")) != 0 : 1;
    extern bool vk_hud_layer_enabled(void);
    extern bool player_eye(float eye[3]);
    if (!env || !vk_hud_layer_enabled()) return false;
    return *(volatile uint32_t *)(uintptr_t)(MAIN_BASE + 0x1827370) != 0  // (a menu pauses the game)
           || !player_eye(NULL);  // (no Link: the title, file select, loading)
}

bool vr_menu_pointer_active(void);
bool vr_menu_pointer_active(void) { return panel_pointer_on(); }  // (vr.c: the HUD panel holds still meanwhile)

// Where the ray points on the panel, (u, v) in 0..1 (top left), clamped to its edges: the panel follows the head's
// yaw, so a turned head can leave the controller pointing past it - the cursor then stays on the nearest edge
// instead of leaving the screen (the game hides it off screen). A ray pointing away from the panel's plane goes to
// the edge in its direction along the panel. False only for unusable matrices.
static bool panel_uv_ex(const float aim[16], const float hud[16], float *u, float *v, bool *inside) {
    const float o[3] = {aim[3], aim[7], aim[11]}, d[3] = {-aim[2], -aim[6], -aim[10]};
    const float c[3] = {hud[3], hud[7], hud[11]}, x[3] = {hud[0], hud[4], hud[8]}, y[3] = {hud[1], hud[5], hud[9]};
    float n[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const float xx = x[0] * x[0] + x[1] * x[1] + x[2] * x[2], yy = y[0] * y[0] + y[1] * y[1] + y[2] * y[2];
    if (!(nl > 1e-6f) || !(xx > 1e-9f) || !(yy > 1e-9f)) return false;
    for (int i = 0; i < 3; i++) n[i] /= nl;
    const float dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    const float co[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    const float t = fabsf(dn) > 1e-6f ? (co[0] * n[0] + co[1] * n[1] + co[2] * n[2]) / dn : -1.0f;
    float lx, ly;
    if (t > 0) {  // (the plane is ahead: the hit, in panel units)
        const float r[3] = {o[0] + d[0] * t - c[0], o[1] + d[1] * t - c[1], o[2] + d[2] * t - c[2]};
        lx = (r[0] * x[0] + r[1] * x[1] + r[2] * x[2]) / xx;
        ly = (r[0] * y[0] + r[1] * y[1] + r[2] * y[2]) / yy;
    } else {  // (pointing away from it: the direction along the panel, pushed to the edge)
        lx = (d[0] * x[0] + d[1] * x[1] + d[2] * x[2]) / sqrtf(xx) * 1e3f;
        ly = (d[0] * y[0] + d[1] * y[1] + d[2] * y[2]) / sqrtf(yy) * 1e3f;
    }
    if (!isfinite(lx) || !isfinite(ly)) return false;
    if (inside) *inside = t > 0 && fabsf(lx) <= 1.0f && fabsf(ly) <= 1.0f;
    lx = fminf(1.0f, fmaxf(-1.0f, lx));
    ly = fminf(1.0f, fmaxf(-1.0f, ly));
    *u = lx * 0.5f + 0.5f;
    *v = 0.5f - ly * 0.5f;  // (+y: the top of the screen)
    return true;
}

static bool panel_uv(const float aim[16], const float hud[16], float *u, float *v) {
    return panel_uv_ex(aim, hud, u, v, NULL);
}

// (menu.mm, the settings menu on the HUD panel) Where the right controller's ray meets the panel, (u, v) in 0..1 from
// the top left; false when it misses the panel. click: the right trigger is held.
_Atomic bool g_vr_right_trigger;  // (vr_input.c)
_Atomic float g_vr_right_stick_y;  // (vr_input.c: up +)
float vr_menu_scroll(void) { return atomic_load(&g_vr_right_stick_y); }  // (menu.mm: the stick scrolls the menu)
// (vr.c, the menu's beam) The pointing controller's ray: origin, direction (unit), and its length to the panel
// (1.5 m when it misses).
bool vr_menu_beam(float o[3], float d[3], float *len);
bool vr_menu_beam(float o[3], float d[3], float *len) {
    pthread_mutex_lock(&g_ptr_lock);
    const bool ok = g_ptr_aim_ok && g_ptr_hud_ok;
    float aim[16], hud[16];
    memcpy(aim, g_ptr_aim, sizeof aim);
    memcpy(hud, g_ptr_hud, sizeof hud);
    pthread_mutex_unlock(&g_ptr_lock);
    if (!ok) return false;
    o[0] = aim[3], o[1] = aim[7], o[2] = aim[11];
    d[0] = -aim[2], d[1] = -aim[6], d[2] = -aim[10];
    *len = 1.5f;
    float u, v;
    bool inside = false;
    if (panel_uv_ex(aim, hud, &u, &v, &inside) && inside) {  // (the hit: back to metres along the ray)
        const float c[3] = {hud[3], hud[7], hud[11]}, x[3] = {hud[0], hud[4], hud[8]}, y[3] = {hud[1], hud[5], hud[9]};
        const float n[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
        const float dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
        if (fabsf(dn) > 1e-9f) *len = ((c[0] - o[0]) * n[0] + (c[1] - o[1]) * n[1] + (c[2] - o[2]) * n[2]) / dn;
    }
    return *len > 0.01f;
}

bool vr_menu_ray(float *u, float *v, bool *click) {
    pthread_mutex_lock(&g_ptr_lock);
    const bool ok = g_ptr_aim_ok && g_ptr_hud_ok;
    float aim[16], hud[16];
    memcpy(aim, g_ptr_aim, sizeof aim);
    memcpy(hud, g_ptr_hud, sizeof hud);
    pthread_mutex_unlock(&g_ptr_lock);
    bool inside = false;
    if (click) *click = atomic_load(&g_vr_right_trigger);
    return ok && panel_uv_ex(aim, hud, u, v, &inside) && inside;
}

static bool panel_pointer(SkyJoyConSensor *s) {
    pthread_mutex_lock(&g_ptr_lock);
    const bool on = panel_pointer_on() && g_ptr_aim_ok && g_ptr_hud_ok;
    float aim[16], hud[16];
    memcpy(aim, g_ptr_aim, sizeof aim);
    memcpy(hud, g_ptr_hud, sizeof hud);
    pthread_mutex_unlock(&g_ptr_lock);
    if (!on) return false;
    {   // The controller where it is when the game reads it (once per game frame, evenly spaced), with the runtime's
        // display lead - not the render thread's latest sample: the game thread reads that on its own schedule, so
        // some game frames got the same pose twice and the next one a double step (the cursor juddered).
        extern int64_t vr_display_time_now(void);
        extern bool vr_input_pointer_at(int64_t time, float q[4], float p[3]);
        extern bool nvn_async_on(void);
        extern uint64_t nvn_async_game_frame(void), renderer_present_count(void);
        const int64_t when = vr_display_time_now();
        const uint64_t g = (nvn_async_on() ? nvn_async_game_frame() : renderer_present_count()) + 1;
        if (when > 0) {
            float q[4], p[3];
            if (vr_input_pointer_at(when, q, p)) {
                // Tremor (and the prediction's noise): small turns eased in, once per game frame (the same frame
                // read twice gets the same pose); larger, deliberate turns followed at once.
                static float sq[4];
                static uint64_t sg;
                static bool sq_ok;
                static float out[4];
                if (!sq_ok || g < sg || g > sg + 8) memcpy(sq, q, sizeof sq), sq_ok = true;
                else if (g != sg) {
                    float dot = sq[0] * q[0] + sq[1] * q[1] + sq[2] * q[2] + sq[3] * q[3];
                    const float sign = dot < 0 ? -1.0f : 1.0f;
                    const float angle = 2 * acosf(fminf(1.0f, fabsf(dot)));
                    const float a = fminf(1.0f, fmaxf(0.25f, angle / 0.02f));  // (fully eased below ~1 degree)
                    float l = 0;
                    for (int i = 0; i < 4; i++) sq[i] += (q[i] * sign - sq[i]) * a, l += sq[i] * sq[i];
                    l = sqrtf(l);
                    for (int i = 0; i < 4; i++) sq[i] /= l;
                }
                sg = g;
                memcpy(out, sq, sizeof out);
                const float x = out[0], y = out[1], z = out[2], w = out[3];
                const float m[16] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p[0],
                                     2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p[1],
                                     2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), p[2],
                                     0, 0, 0, 1};
                memcpy(aim, m, sizeof aim);
            }
        }
    }
    float u, v;
    if (!panel_uv(aim, hud, &u, &v)) return false;
    {   // (diagnostics) every half second while the pointer is active: the panel point, the ray, the panel
        static uint64_t next;
        const uint64_t now = SDL_GetTicksNS();
        if (now >= next) {
            next = now + 500000000ull;
            LOG("[vr-pointer] u %.2f v %.2f | ray from (%.2f %.2f %.2f) dir (%.2f %.2f %.2f) | panel centre (%.2f %.2f %.2f) "
                "x (%.2f %.2f %.2f) y (%.2f %.2f %.2f)\n", u, v, aim[3], aim[7], aim[11], -aim[2], -aim[6], -aim[10],
                hud[3], hud[7], hud[11], hud[0], hud[4], hud[8], hud[1], hud[5], hud[9]);
        }
    }
    vr_pointer_direction(u, v, s->direction);
    // Gravity as the Joy-Con would feel it held that way (its axes' z, the player's up, negated: (0, 0, -1) level),
    // not as the controller is really held: the game also reads the tilt from it, and the real controller is held
    // lower and pitched down to aim at the panel - the cursor could not rise past the middle of the screen.
    for (int i = 0; i < 3; i++) s->accel[i] = -s->direction[i * 3 + 2];
    static int logged;
    if (!logged++) LOG("[vr] menu pointer: the game's cursor follows the right controller's ray on the HUD panel\n");
    return true;
}

// Aiming an item (a reticle on screen): the Joy-Con orientation that puts the game's reticle where the right
// controller points (runtime/hle/vr_aim.c vr_aim_pointer), with the menu pointer's mapping.
static bool aim_pointer(SkyJoyConSensor *s) {
    extern bool vr_aim_pointer(float *u, float *v);
    float u, v;
    if (!vr_aim_pointer(&u, &v)) return false;
    vr_pointer_direction(u, v, s->direction);
    for (int i = 0; i < 3; i++) s->accel[i] = -s->direction[i * 3 + 2];  // (gravity as it would be held: see above)
    static int logged;
    if (!logged++) LOG("[vr] aiming: the item aims where the right controller points\n");
    return true;
}

bool vr_joycon_state(VrJoyConState *state) {
    SkyJoyConState s;
    extern bool motion_joycon_mode(void);
    if (!state || controllers_off() || !motion_joycon_mode() || !sky_joycon_state(&s)) return false;
    memcpy(state, &s, sizeof s);
    return true;
}

// Skyward Strike asks for a nearly upright, steady Joy-Con. Real VR grips have controller-specific pitch offsets
// and considerably more resting jitter than a calibrated Joy-Con. Once the sword is already raised mostly upward,
// report the intended cardinal pose and remove only low-rate jitter; swings below this cone remain completely raw.
static void skyward_strike_assist(SkyJoyConSensor *s) {
    if (s->direction[5] < 0.72f) return;  // Joy-Con +Y follows the controller's pointing axis.
    float x = s->direction[0], y = s->direction[1];
    const float n = sqrtf(x * x + y * y);
    if (n > 1e-4f) x /= n, y /= n; else x = 1.0f, y = 0.0f;
    // Rows are the Joy-Con x/y/z axes in player space; x cross blade gives the remaining orthogonal axis.
    s->direction[0] = x;    s->direction[1] = y;    s->direction[2] = 0.0f;
    s->direction[3] = 0.0f; s->direction[4] = 0.0f; s->direction[5] = 1.0f;
    s->direction[6] = y;    s->direction[7] = -x;   s->direction[8] = 0.0f;
    s->accel[0] = 0.0f; s->accel[1] = -1.0f; s->accel[2] = 0.0f;
    const float jitter = 0.08f;  // revolutions/s; preserve deliberate rotations and every real swing.
    for (int i = 0; i < 3; i++) if (fabsf(s->gyro[i]) < jitter) s->gyro[i] = 0.0f;
}

bool vr_joycon_sensor(int device, VrJoyConSensor *sensor) {
    SkyJoyConSensor s;
    extern bool motion_joycon_mode(void);
    if (!sensor || controllers_off() || !motion_joycon_mode() || !sky_joycon_sensor(device, &s)) return false;
    // (the right Joy-Con: an aimed item, the menu pointer, the sword)
    if (device == 1 && !aim_pointer(&s) && !panel_pointer(&s)) skyward_strike_assist(&s);
    memcpy(sensor, &s, sizeof s);
    return true;
}

#else
bool vr_menu_beam(float o[3], float d[3], float *len) { (void)o; (void)d; (void)len; return false; }
float vr_menu_scroll(void) { return 0; }
bool vr_menu_ray(float *u, float *v, bool *click) { (void)u; (void)v; if (click) *click = false; return false; }
bool vr_menu_pointer_active(void) { return false; }
void vr_pointer_direction(float u, float v, float out[9]) {
    (void)u; (void)v;
    for (int i = 0; i < 9; i++) out[i] = i % 4 == 0;
}
void vr_controls_set_hud_panel(const float m[16]) { (void)m; }
bool vr_joycon_state(VrJoyConState *state) { (void)state; return false; }
bool vr_joycon_sensor(int device, VrJoyConSensor *sensor) { (void)device; (void)sensor; return false; }
#endif
