// Joy-Con motion sensors (nn::hid SixAxisSensor) and the controller style the game sees.
//
// Desktop: the game sees one Pro Controller whose sensor rests level (its button controls). VR (or
// SKYRECOMP_JOYCON=1; =0 forces the Pro Controller): a pair of detached Joy-Cons (NpadJoyDual), the style Skyward
// Sword HD plays with motion controls; each Joy-Con's sensor is the matching VR controller (runtime/vr.h
// vr_joycon_sensor, runtime/nvn/vulkan/JoyConTranslator.h for the conventions). The game reads both sensors every
// frame, recognises swings / thrusts / shield bashes itself, and points with Joy-Con (R).
//
// The style follows the live VR setting; input.c signals the SDK's style-change event when VR is toggled.
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../frame_rate.h"
#include "../vr/vr.h"
#include "../vr/vr_settings.h"
#include "motion_calibration.h"

enum { STYLE_FULLKEY = 1u << 0, STYLE_JOY_DUAL = 1u << 2 };  // NpadStyleTag bits
enum { STYLE_INDEX_FULLKEY = 3, STYLE_INDEX_JOY_DUAL = 5 };   // NpadStyleIndex
enum { DEVICE_LEFT = 0, DEVICE_RIGHT = 1, DEVICE_NONE = 2 };  // SixAxisSensorHandle device index

// nn::hid::SixAxisSensorHandle (4 bytes)
typedef struct { uint8_t style_index, player, device, pad; } SensorHandle;

// nn::hid::SixAxisSensorState (0x60 bytes). Units: acceleration in g, angular velocity in revolutions per second,
// angle in revolutions, direction = the sensor's axes as rows.
typedef struct {
    int64_t delta_ns, sampling;
    float accel[3], gyro[3], angle[3], direction[9];
    uint32_t attributes, reserved;
} SensorState;
_Static_assert(sizeof(SensorState) == 0x60, "nn::hid::SixAxisSensorState");
_Static_assert(offsetof(SensorState, accel) == 0x10 && offsetof(SensorState, direction) == 0x34, "SixAxisSensorState");

static _Atomic int auto_joycon;

// This coordinator owns the game's pointer calibration: the "point at the screen and press Y" prompt, the
// recalibration, then teardown; the game's cursor stays off until it has run. It is called each frame while the
// prompt is up; its state (+24): 0 opening, 1 waiting for the prompt's opening animation (its object +8: +0x160b
// set when an animation is done), 2 Y pressed?, 3 closing requested, 4 waiting for the closing animation, 5
// teardown, then 0 (idle). State 3 waits for the prompt's status +0x15e0 to be 2 (open). In VR it runs, answered by
// the port, and is never seen or heard:
//  - states 0/1 go straight to 2, and 3/4 straight to 5: SYS_GYRO_00 is never opened or closed, so neither its
//    drawing nor its UI sound events are requested. State 2's calibration and state 5's cleanup still run;
//  - the centred-pose settling window starts during startup/name entry; state 2 confirms once it is ready;
//  - states 1-4: the right Joy-Con points at the screen's centre (the HUD panel's centre, where the menu pointer's
//    mapping starts: vr_controls.c);
//  - the HUD and background audio keep running normally; neither blanket HUD hiding nor master muting is needed.
// A run ends when the state returns to 0 after the prompt, or when the coordinator stops being called; it is never
// answered for more than CAL_MAX_FRAMES. SKYRECOMP_VR_MOTION_CALIBRATION=1: show the game's own prompt in VR too.
#define LEGACY_CALIBRATION_COORDINATOR (MAIN_BASE + 0xdd2a68ull)
enum { CAL_MAX_FRAMES = 1200, CAL_SETTLE_FRAMES = 45 };
static GuestFn legacy_calibration_coordinator;
static GuestFn legacy_calibration_begin, legacy_calibration_update;

// The native calibration callbacks disable the meter/layout owner's draw flag (+0x1539), saving it at
// main+0x1939784. Keep that draw flag at its entry value while running automatic VR calibration; the real
// layouts then continue updating/drawing, rather than disappearing or being replaced with a captured image.
typedef struct { uint64_t hud; uint8_t visible; } CalibrationHud;
static CalibrationHud calibration_hud_before(void) {
    CalibrationHud result = {0};
    if (!vr_requested() || ENV("SKYRECOMP_VR_MOTION_CALIBRATION")) return result;
    result.hud = *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x1827388);
    if (result.hud) result.visible = *(const uint8_t *)(uintptr_t)(result.hud + 0x1539);
    return result;
}
static void calibration_hud_after(CalibrationHud before) {
    if (!before.hud || before.hud != *(const uint64_t *)(uintptr_t)(MAIN_BASE + 0x1827388)) return;
    volatile uint8_t *visible = (volatile uint8_t *)(uintptr_t)(before.hud + 0x1539);
    if (*visible != before.visible) {
        if (ENV("SKYRECOMP_MOTION_CAL_TRACE"))
            LOG("[motion] calibration preserves native HUD visibility %u (native callback requested %u)\n", before.visible, *visible);
        *visible = before.visible;
    }
}
static void vr_calibration_begin(Cpu *c) {
    CalibrationHud before = calibration_hud_before();
    legacy_calibration_begin(c);
    calibration_hud_after(before);
}
static void vr_calibration_update(Cpu *c) {
    CalibrationHud before = calibration_hud_before();
    legacy_calibration_update(c);
    calibration_hud_after(before);
}
static _Atomic uint64_t cal_last_call, cal_run_start;  // (present count + 1; 0 none)
static _Atomic uint32_t cal_state;
static _Atomic bool cal_in_run;
static _Atomic bool cal_initial_done;
static _Atomic uint64_t cal_warm_start;

// Prepare while the normal startup/name-entry loop is still running, before native calibration pauses it.
// Name entry releases preparation when its dialog closes; live sensor samples invalidate the prepared window.
void motion_prepare_auto_calibration(bool prepare) {
    if (!vr_requested() || ENV("SKYRECOMP_VR_MOTION_CALIBRATION")) return;
    atomic_store(&cal_initial_done, !prepare);
    if (prepare) atomic_store(&cal_warm_start, 0);
}
static uint64_t cal_now(void) {
    extern uint64_t renderer_present_count(void);
    return renderer_present_count() + 1;
}
static bool cal_running(void) {  // (a run is in progress: called lately, not past its limit)
    const uint64_t now = cal_now(), last = atomic_load(&cal_last_call), start = atomic_load(&cal_run_start);
    return atomic_load(&cal_in_run) && last && now <= last + 2 && now <= start + CAL_MAX_FRAMES;
}
static void vr_auto_calibration(Cpu *c) {
    static int keep = -1;
    if (keep < 0) keep = getenv("SKYRECOMP_VR_MOTION_CALIBRATION") != NULL;
    if (!vr_requested() || keep) {
        legacy_calibration_coordinator(c);
        return;
    }
    const uint64_t self = c->x[0], now = cal_now();
    if (!cal_running()) {  // (a new run)
        atomic_store(&cal_run_start, now);
        atomic_store(&cal_in_run, true);
        LOG("[motion] VR: calibrating in the background; native prompt and UI sounds are skipped\n");
    }
    atomic_store(&cal_last_call, now);
    if (self) {
        volatile uint32_t *state = (volatile uint32_t *)(uintptr_t)(self + 24);
        const uint32_t next = motion_calibration_background_state(*state);
        if (next != *state) {
            *state = next;
            *(volatile uint32_t *)(uintptr_t)(self + 0x20) = 0;
        }
        atomic_store(&cal_state, next);  // centered sensor samples apply before the first native calibration call
    }
    CalibrationHud hud_before = calibration_hud_before();
    legacy_calibration_coordinator(c);
    calibration_hud_after(hud_before);
    const uint32_t st = self ? *(const uint32_t *)(uintptr_t)(self + 24) : 0;
    const uint32_t before = atomic_exchange(&cal_state, st);
    if (st != before && getenv("SKYRECOMP_MOTION_CAL_TRACE")) {
        const uint64_t prompt = self ? *(const uint64_t *)(uintptr_t)(self + 8) : 0;
        LOG("[motion] calibration state %u at frame %llu (native prompt state %u, open request %u)\n", st,
            (unsigned long long)now, prompt ? *(const uint32_t *)(uintptr_t)(prompt + 0x15e0) : 0,
            prompt ? *(const uint32_t *)(uintptr_t)(prompt + 0x15f8) : 0);
    }
    if (st == 0 && before == 5) {  // (closed: the run is over)
        atomic_store(&cal_in_run, false);
        atomic_store(&cal_initial_done, true);
        atomic_store(&cal_warm_start, 0);
    }
}

RT_ON_RESET(motion_calibration_reset) {
    atomic_store(&cal_in_run, false);
    atomic_store(&cal_state, 0);
    atomic_store(&cal_last_call, 0);
    atomic_store(&cal_run_start, 0);
    atomic_store(&cal_initial_done, false);
    atomic_store(&cal_warm_start, 0);
}
static bool auto_calibrating(void) {  // (the right Joy-Con points at the centre)
    if (!vr_requested() || ENV("SKYRECOMP_VR_MOTION_CALIBRATION")) return false;
    const uint32_t st = atomic_load(&cal_state);
    const bool centered = !atomic_load(&cal_initial_done) || (cal_running() && st >= 1 && st <= 4);
    if (centered && !atomic_load(&cal_warm_start)) atomic_store(&cal_warm_start, cal_now());
    return centered;
}

// (input.c, each pad poll) Confirm only after the game's motion filter has received a stable centred pose. Completing
// the hidden prompt in 2-3 frames left its baseline dependent on the controller pose from before the prompt, which
// could restrict the cursor to part of the screen even though the absolute VR ray covered the whole HUD panel.
uint64_t motion_auto_buttons(void) {
    const uint32_t st = atomic_load(&cal_state);
    const uint64_t start = atomic_load(&cal_warm_start);
    return cal_running() && st == 2 && start && cal_now() >= start + CAL_SETTLE_FRAMES ? 1u << 3 : 0;  // (Npad Y)
}

// TEMP (RE) SKYRECOMP_TEMP_MOTION_GATE=1: f_7000a17aa8 / f_7000a17be0 (player: "motion input active?") logged on change
// with the player's action (+0x468) and its controller block (+0x638c: +0x64..+0x6f, +0x78)
static GuestFn temp_gate_aa8, temp_gate_be0;
static void temp_gate_log(const char *which, Cpu *c, uint64_t player) {
    static char last[2][160];
    char line[160];
    const uint8_t *k = (const uint8_t *)(uintptr_t)(player + 0x638c);
    snprintf(line, sizeof line, "ret %llu action %#x b64 %02x%02x%02x%02x b68 %02x%02x%02x%02x b6c %02x%02x u78 %u",
             (unsigned long long)(c->x[0] & 1), *(const uint32_t *)(uintptr_t)(player + 0x468), k[0x64], k[0x65], k[0x66],
             k[0x67], k[0x68], k[0x69], k[0x6a], k[0x6b], k[0x6c], k[0x6d], *(const uint16_t *)(k + 0x78));
    const int i = which[0] == 'a';
    if (strcmp(line, last[i])) { LOG("[gate] %s %s\n", which, line); strcpy(last[i], line); }
}
static void temp_gate_aa8_hook(Cpu *c) { const uint64_t p = c->x[0]; temp_gate_aa8(c); temp_gate_log("aa8", c, p); }
static void temp_gate_be0_hook(Cpu *c) { const uint64_t p = c->x[0]; temp_gate_be0(c); temp_gate_log("be0", c, p); }

void motion_vr_init(void) {
    if (getenv("SKYRECOMP_TEMP_MOTION_GATE")) {
        temp_gate_aa8 = dispatch_lookup((MAIN_BASE + 0xa17aa8ull));
        temp_gate_be0 = dispatch_lookup((MAIN_BASE + 0xa17be0ull));
        if (temp_gate_aa8) hle_override((MAIN_BASE + 0xa17aa8ull), temp_gate_aa8_hook, "temp_gate_aa8");
        if (temp_gate_be0) hle_override((MAIN_BASE + 0xa17be0ull), temp_gate_be0_hook, "temp_gate_be0");
    }
    legacy_calibration_coordinator = dispatch_lookup(LEGACY_CALIBRATION_COORDINATOR);
    if (legacy_calibration_coordinator)
        hle_override(LEGACY_CALIBRATION_COORDINATOR, vr_auto_calibration, "vr_auto_motion_calibration");
    else
        LOG("[motion] warning: legacy calibration hook unavailable\n");
    legacy_calibration_begin = dispatch_lookup(MAIN_BASE + 0xdd25c0);
    legacy_calibration_update = dispatch_lookup(MAIN_BASE + 0xdd273c);
    if (legacy_calibration_begin)
        hle_override(MAIN_BASE + 0xdd25c0, vr_calibration_begin, "vr_calibration_begin");
    if (legacy_calibration_update)
        hle_override(MAIN_BASE + 0xdd273c, vr_calibration_update, "vr_calibration_update");
}

// VR always plays with motion controls (user: button controls must not be an option in VR - they lose the
// motion): the Joy-Con pair whenever VR is on, whatever was used last. g_vr_config.control_source (the old "VR
// Control Source" choice) is no longer read.
static int forced_mode(void) { return vr_running() ? 1 : -1; }

bool motion_joycon_mode(void) {
    static int override = -2, last = -1;
    if (override == -2) {
        const char *e = getenv("SKYRECOMP_JOYCON");
        override = e ? atoi(e) != 0 : -1;
    }
    const int forced = forced_mode();
    const int on = forced >= 0 ? forced : override >= 0 ? override : atomic_load(&auto_joycon);  // (VR first)
    if (on != last) {
        LOG("[motion] controller style: %s\n", on ? "Joy-Con pair (motion controls)" : "Pro Controller");
        last = on;
    }
    return on;
}

// Auto mode follows the last input source that the player actually used. SDL pads call this with their database
// classification; OpenXR calls it when tracked motion-controller input becomes active.
void motion_select_input(bool joycon) {
    if (forced_mode() >= 0 || getenv("SKYRECOMP_JOYCON")) return;
    if (atomic_exchange(&auto_joycon, joycon) == joycon) return;
    (void)motion_joycon_mode();
    extern void input_controller_style_changed(void);
    input_controller_style_changed();
}

void motion_input_config_changed(void) {
    (void)motion_joycon_mode();
    extern void input_controller_style_changed(void);
    input_controller_style_changed();
}

// Each distinct call shape logged once (SKYRECOMP_JOYCON_TRACE=1).
static void note(const char *what, uint32_t a, uint32_t b, uint32_t c) {
    static struct { const char *what; uint32_t a, b, c; } seen[64];
    static int n, on = -1;
    if (on < 0) on = getenv("SKYRECOMP_JOYCON_TRACE") != NULL;
    if (!on) return;
    for (int i = 0; i < n; i++)
        if (seen[i].what == what && seen[i].a == a && seen[i].b == b && seen[i].c == c) return;
    if (n < 64) seen[n++] = (typeof(seen[0])){what, a, b, c};
    LOG("[motion] %s %#x %#x %#x\n", what, a, b, c);
}

// (test) SKYRECOMP_JOYCON_TEST=sweep: Joy-Con (R) turns +-20 degrees about its x, y, then z axis, 120 presented
// frames each (the pointer moves: z horizontally, x vertically); =gyro: gyro only, +0.1 rev/s about z for frames
// 2700..2759 (the pointer stays: the game points with `direction`).
static bool test_sample(uint8_t device, SensorState *s) {
    static const char *test_r = (const char *)1, *test_l = (const char *)1;  // (=_LEFT: the same for Joy-Con (L))
    if (test_r == (const char *)1) test_r = getenv("SKYRECOMP_JOYCON_TEST"), test_l = getenv("SKYRECOMP_JOYCON_TEST_LEFT");
    const char *test = device == DEVICE_RIGHT ? test_r : device == DEVICE_LEFT ? test_l : NULL;
    if (!test) return false;
    extern uint64_t renderer_present_count(void);
    const uint64_t f = renderer_present_count();
    if (!strcmp(test, "gyro")) {
        if (f >= 2700 && f < 2760) s->gyro[2] = 0.1f;
        return true;
    }
    float pu, pv, pfrom = 0, ptilt = 0;  // (test) =panel:<u>,<v>[,<frame>[,<tilt>]]: the menu pointer's mapping
    // (vr_controls.c) aimed at (u, v); tilt: gravity as from a controller pitched down that many degrees
    if (sscanf(test, "panel:%f,%f,%f,%f", &pu, &pv, &pfrom, &ptilt) >= 2) {
        if ((float)f < pfrom) return true;
        extern void vr_pointer_direction(float u, float v, float out[9]);
        vr_pointer_direction(pu, pv, s->direction);
        const float t = ptilt * (float)(M_PI / 180.0);
        s->accel[0] = 0, s->accel[1] = sinf(t), s->accel[2] = -cosf(t);
        return true;
    }
    // (test) =point:<yaw>,<pitch>[,<frame>]: held turned that many degrees about z, then about x (from that frame:
    // straight before it, so the game's Y calibration can be made first)
    // =seq:<yaw>,<pitch>,<frame>[,<gx>,<gy>,<gz>[,<ax>,<ay>,<az>]]/...: each from its frame (straight before the
    // first), with an optional gyroscope rate (revolutions per second about the Joy-Con's x, y, z) and acceleration
    // (g; a swing) during it
    float yaw_deg = 0, pitch_deg = 0, from = 0, gyro[3] = {0, 0, 0}, accel[3] = {0, 0, -1};
    bool has_accel = false;
    if (!strncmp(test, "seq:", 4)) {
        bool any = false;
        for (const char *p = test + 4; p && *p; p = strchr(p, '/') ? strchr(p, '/') + 1 : NULL) {
            float y, pi, fr, g[3] = {0, 0, 0}, ac[3] = {0, 0, -1};
            const int n = sscanf(p, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &y, &pi, &fr, &g[0], &g[1], &g[2], &ac[0], &ac[1], &ac[2]);
            if (n >= 3 && (float)f >= fr) {
                yaw_deg = y, pitch_deg = pi, any = true, memcpy(gyro, g, sizeof g), memcpy(accel, ac, sizeof ac);
                has_accel = n >= 9;
            }
        }
        if (!any) return true;
        memcpy(s->gyro, gyro, sizeof gyro);
        if (has_accel) memcpy(s->accel, accel, sizeof accel);
        from = 0;
    }
    if (!strncmp(test, "seq:", 4) || sscanf(test, "point:%f,%f,%f", &yaw_deg, &pitch_deg, &from) >= 2) {
        if ((float)f < from) return true;
        const float a = yaw_deg * (float)(M_PI / 180.0), b = pitch_deg * (float)(M_PI / 180.0);
        const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        const float yaw[9] = {ca, -sa, 0, sa, ca, 0, 0, 0, 1}, pitch[9] = {1, 0, 0, 0, cb, -sb, 0, sb, cb};
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++)
                s->direction[r * 3 + k] = pitch[r * 3 + 0] * yaw[0 * 3 + k] + pitch[r * 3 + 1] * yaw[1 * 3 + k] +
                                          pitch[r * 3 + 2] * yaw[2 * 3 + k];
        s->angle[0] = b / (float)(2 * M_PI);  // (the integrated rotation, revolutions, as the sweep test)
        s->angle[2] = a / (float)(2 * M_PI);
        return true;
    }
    const int axis = (int)(f / 120 % 3), i = (axis + 1) % 3, j = (axis + 2) % 3;
    const double w = 2 * M_PI / 120.0, amp = 20.0 * M_PI / 180.0, ph = w * (double)(f % 120);
    const float a = (float)(amp * sin(ph)), c = cosf(a), sn = sinf(a);
    s->direction[i * 3 + i] = c;
    s->direction[i * 3 + j] = -sn;
    s->direction[j * 3 + i] = sn;
    s->direction[j * 3 + j] = c;
    s->gyro[axis] = (float)(amp * w * 60.0 * cos(ph) / (2 * M_PI));
    s->angle[axis] = a / (float)(2 * M_PI);
    return true;
}

// One Joy-Con sensor sample: the VR controller, else lying level at rest.
static void motion_sample(uint8_t device, SensorState *s) {
    s->accel[2] = -1.0f;
    s->direction[0] = s->direction[4] = s->direction[8] = 1.0f;
    if (device == DEVICE_RIGHT && auto_calibrating()) {
        // Bias the hidden baseline slightly toward the top of the panel.  The game's accepted pointer cone is
        // asymmetric after calibration; using its geometric centre left a narrow unreachable strip at the bottom.
        // Moving the baseline (rather than amplifying live poses) preserves the cone and shifts its usable range.
        extern void vr_pointer_direction(float u, float v, float out[9]);
        vr_pointer_direction(0.5f, 0.46f, s->direction);
        for (int i = 0; i < 3; i++) s->accel[i] = -s->direction[i * 3 + 2];
        return;
    }
    if (device == DEVICE_RIGHT) atomic_store(&cal_warm_start, 0);
    if (test_sample(device, s)) return;
    VrJoyConSensor v;
    if (device > DEVICE_RIGHT) return;
    // The settings menu has the controllers (its pointer, menu.mm): the game keeps their last pose, held still
    // (its cursor and sword don't follow the menu's pointing).
    static VrJoyConSensor last[2];
    static bool have_last[2];
    extern bool menu_input_captured(void);
    if (menu_input_captured()) {
        if (!have_last[device]) return;
        v = last[device];
        memset(v.gyro, 0, sizeof v.gyro);
    } else {
        if (!vr_joycon_sensor(device, &v)) return;
        last[device] = v, have_last[device] = true;
    }
    memcpy(s->accel, v.accel, sizeof s->accel);
    memcpy(s->gyro, v.gyro, sizeof s->gyro);
    memcpy(s->angle, v.angle, sizeof s->angle);
    memcpy(s->direction, v.direction, sizeof s->direction);
    if (v.delta_ns > 0) s->delta_ns = v.delta_ns;
}

uint32_t motion_style_set(void) { return motion_joycon_mode() ? STYLE_JOY_DUAL : STYLE_FULLKEY; }

// GetNpadStyleSet(NpadId): the styles attached to a player.
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    note("GetNpadStyleSet", id, 0, 0);
    c->x[0] = id == 0 ? motion_style_set() : 0;
}

// GetSixAxisSensorHandles(out, count, NpadId, style): one sensor for a Pro Controller, two for a Joy-Con pair.
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_sixaxis_handles) {
    SensorHandle *out = (SensorHandle *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    uint32_t style = (uint32_t)c->x[3];
    note("GetSixAxisSensorHandles", (uint32_t)count, id, style);
    int n = 0;
    if (id == 0 && out) {
        if ((style & STYLE_JOY_DUAL) && motion_joycon_mode()) {
            if (n < count) out[n++] = (SensorHandle){STYLE_INDEX_JOY_DUAL, 0, DEVICE_LEFT, 0};
            if (n < count) out[n++] = (SensorHandle){STYLE_INDEX_JOY_DUAL, 0, DEVICE_RIGHT, 0};
        } else if (count >= 1) {
            out[n++] = (SensorHandle){STYLE_INDEX_FULLKEY, 0, DEVICE_NONE, 0};
        }
    }
    c->x[0] = (uint64_t)n;
}

HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_start_sixaxis) {
    const SensorHandle *h = (const SensorHandle *)(uintptr_t)c->x[0];
    note("StartSixAxisSensor", h->style_index, h->player, h->device);
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_stop_sixaxis) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", hid_enable_sixaxis_fusion) {
    note("EnableSixAxisSensorFusion", (uint32_t)c->x[1], 0, 0);
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", hid_set_sixaxis_fusion) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", hid_set_gyro_drift) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", hid_get_sixaxis_state) {
    SensorState *s = (SensorState *)(uintptr_t)c->x[0];
    const SensorHandle *h = (const SensorHandle *)(uintptr_t)c->x[1];
    static int64_t sampling;
    note("GetSixAxisSensorState", h->style_index, h->player, h->device);
    memset(s, 0, sizeof *s);
    s->delta_ns = (int64_t)rt_frame_period_ns();
    s->sampling = ++sampling;
    s->attributes = 1;  // IsConnected
    if (motion_joycon_mode()) {
        motion_sample(h->device, s);
    } else {
        s->accel[1] = -1.0f;
        s->direction[0] = s->direction[4] = s->direction[8] = 1.0f;
    }
}

// SixAxisSensorState::GetQuaternion: the orientation of `direction` as a quaternion (x, y, z, w).
HLE_FUNC("_ZNK2nn3hid18SixAxisSensorState13GetQuaternionEPNS_4util4neon12Vector4fTypeE", hid_sixaxis_quat) {
    const SensorState *s = (const SensorState *)(uintptr_t)c->x[0];
    float *q = (float *)(uintptr_t)c->x[1];
    if (q) {
        const float *m = s->direction;
        const float w = sqrtf(fmaxf(0.0f, 1.0f + m[0] + m[4] + m[8])) * 0.5f;
        const float k = w > 1e-4f ? 0.25f / w : 0.0f;
        q[0] = (m[5] - m[7]) * k;
        q[1] = (m[6] - m[2]) * k;
        q[2] = (m[1] - m[3]) * k;
        q[3] = w > 1e-4f ? w : 1.0f;
    }
    c->x[0] = 0;
}
