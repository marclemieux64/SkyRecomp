// VR controller input: one OpenXR action set whose actions are a virtual pair of Joy-Cons, suggested bindings for
// each supported controller, and the per-frame sample handed to the Joy-Con translator
// (runtime/nvn/vulkan/JoyConTranslator.h). Players can rebind every action in SteamVR's controller bindings.
//
// Default layout, Valve Index (primary):
//   left   thumbstick: stick (move), click: L-stick | trigger: ZL (target) | grip: L | A: Minus | B: Plus
//          trackpad press: d-pad (direction on the pad)
//   right  thumbstick: stick | trigger: ZR | grip: R | A: A | B: B | trackpad press: X
//   Pointing (aim pose) of each hand is that Joy-Con's orientation: swing, thrust, point.
#include "vr.h"

#if !defined(__APPLE__) && !defined(SKYRECOMP_NO_OPENXR) && __has_include(<openxr/openxr.h>) && \
    __has_include(<openxr/openxr_platform.h>)

#include <SDL3/SDL_timer.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "vr_xr.h"
#include "vr_internal.h"
#include "vr_settings.h"
#include "nvn/vulkan/JoyConTranslator.h"

enum {
    ACT_LEFT_STICK, ACT_RIGHT_STICK, ACT_DPAD,                      // vector2
    ACT_ZL, ACT_ZR, ACT_L, ACT_R,                                   // float
    ACT_A, ACT_B, ACT_X, ACT_PLUS, ACT_MINUS, ACT_LSTICK, ACT_RSTICK, ACT_DPAD_PRESS,  // boolean
    ACT_LEFT_AIM, ACT_RIGHT_AIM,                                    // pose used by the Joy-Con motion emulation
    ACT_LEFT_GRIP, ACT_RIGHT_GRIP,                                  // grip poses: the wrist HUD
    ACT_LEFT_HAPTIC, ACT_RIGHT_HAPTIC,                              // vibration (the settings menu's clicks)
    ACT_COUNT
};

static const struct { const char *name, *label; XrActionType type; uint32_t button; } action_defs[ACT_COUNT] = {
    [ACT_LEFT_STICK] = {"left_stick", "Left stick (move)", XR_ACTION_TYPE_VECTOR2F_INPUT, 0},
    [ACT_RIGHT_STICK] = {"right_stick", "Right stick", XR_ACTION_TYPE_VECTOR2F_INPUT, 0},
    [ACT_DPAD] = {"dpad", "D-pad direction", XR_ACTION_TYPE_VECTOR2F_INPUT, 0},
    [ACT_ZL] = {"zl", "ZL (target)", XR_ACTION_TYPE_FLOAT_INPUT, 0},
    [ACT_ZR] = {"zr", "ZR", XR_ACTION_TYPE_FLOAT_INPUT, 0},
    [ACT_L] = {"l", "L", XR_ACTION_TYPE_FLOAT_INPUT, 0},
    [ACT_R] = {"r", "R", XR_ACTION_TYPE_FLOAT_INPUT, 0},
    [ACT_A] = {"a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_A},
    [ACT_B] = {"b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_B},
    [ACT_X] = {"x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_X},
    [ACT_PLUS] = {"plus", "Plus", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_PLUS},
    [ACT_MINUS] = {"minus", "Minus", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_MINUS},
    [ACT_LSTICK] = {"left_stick_click", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_LSTICK},
    [ACT_RSTICK] = {"right_stick_click", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_RSTICK},
    [ACT_DPAD_PRESS] = {"dpad_press", "D-pad press", XR_ACTION_TYPE_BOOLEAN_INPUT, 0},
    [ACT_LEFT_AIM] = {"left_joycon", "Joy-Con (L) pose", XR_ACTION_TYPE_POSE_INPUT, 0},
    [ACT_RIGHT_AIM] = {"right_joycon", "Joy-Con (R) pose", XR_ACTION_TYPE_POSE_INPUT, 0},
    [ACT_LEFT_GRIP] = {"left_hand", "Left hand grip pose", XR_ACTION_TYPE_POSE_INPUT, 0},
    [ACT_RIGHT_GRIP] = {"right_hand", "Right hand grip pose", XR_ACTION_TYPE_POSE_INPUT, 0},
    [ACT_LEFT_HAPTIC] = {"left_haptic", "Left vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, 0},
    [ACT_RIGHT_HAPTIC] = {"right_haptic", "Right vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, 0},
};

typedef struct { int action; const char *path; } Binding;
#define L(p) "/user/hand/left/input/" p
#define R(p) "/user/hand/right/input/" p
#define HAPTICS {ACT_LEFT_HAPTIC, "/user/hand/left/output/haptic"}, {ACT_RIGHT_HAPTIC, "/user/hand/right/output/haptic"}

static const Binding index_bindings[] = {
    {ACT_LEFT_STICK, L("thumbstick")}, {ACT_LSTICK, L("thumbstick/click")}, {ACT_ZL, L("trigger/value")},
    {ACT_L, L("squeeze/value")}, {ACT_MINUS, L("a/click")}, {ACT_PLUS, L("b/click")}, {ACT_DPAD, L("trackpad")},
    {ACT_DPAD_PRESS, L("trackpad/force")}, {ACT_LEFT_AIM, L("aim/pose")}, {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_RIGHT_STICK, R("thumbstick")}, {ACT_ZR, R("trigger/value")},
    {ACT_R, R("squeeze/value")}, {ACT_A, R("a/click")}, {ACT_B, R("b/click")}, {ACT_X, R("trackpad/force")},
    {ACT_RIGHT_AIM, R("aim/pose")}, {ACT_RIGHT_GRIP, R("grip/pose")}, HAPTICS,
};
static const Binding touch_bindings[] = {
    {ACT_LEFT_STICK, L("thumbstick")}, {ACT_LSTICK, L("thumbstick/click")}, {ACT_ZL, L("trigger/value")},
    {ACT_L, L("squeeze/value")}, {ACT_MINUS, L("x/click")}, {ACT_PLUS, L("y/click")}, {ACT_LEFT_AIM, L("aim/pose")},
    {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_RIGHT_STICK, R("thumbstick")}, {ACT_ZR, R("trigger/value")},
    {ACT_R, R("squeeze/value")}, {ACT_A, R("a/click")}, {ACT_B, R("b/click")}, {ACT_RIGHT_AIM, R("aim/pose")},
    {ACT_RIGHT_GRIP, R("grip/pose")}, HAPTICS,
};
static const Binding vive_bindings[] = {
    {ACT_LEFT_STICK, L("trackpad")}, {ACT_LSTICK, L("trackpad/click")}, {ACT_ZL, L("trigger/value")},
    {ACT_L, L("squeeze/click")}, {ACT_PLUS, L("menu/click")}, {ACT_LEFT_AIM, L("aim/pose")}, {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_RIGHT_STICK, R("trackpad")}, {ACT_A, R("trackpad/click")}, {ACT_ZR, R("trigger/value")},
    {ACT_R, R("squeeze/click")}, {ACT_RIGHT_AIM, R("aim/pose")}, {ACT_RIGHT_GRIP, R("grip/pose")}, HAPTICS,
};
static const Binding wmr_bindings[] = {
    {ACT_LEFT_STICK, L("thumbstick")}, {ACT_LSTICK, L("thumbstick/click")}, {ACT_ZL, L("trigger/value")},
    {ACT_L, L("squeeze/click")}, {ACT_PLUS, L("menu/click")}, {ACT_DPAD, L("trackpad")},
    {ACT_DPAD_PRESS, L("trackpad/click")}, {ACT_LEFT_AIM, L("aim/pose")}, {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_RIGHT_STICK, R("thumbstick")}, {ACT_ZR, R("trigger/value")},
    {ACT_R, R("squeeze/click")}, {ACT_MINUS, R("menu/click")}, {ACT_A, R("trackpad/click")},
    {ACT_RIGHT_AIM, R("aim/pose")}, {ACT_RIGHT_GRIP, R("grip/pose")}, HAPTICS,
};
static const Binding simple_bindings[] = {
    {ACT_ZL, L("select/click")}, {ACT_PLUS, L("menu/click")}, {ACT_LEFT_AIM, L("aim/pose")}, {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_A, R("select/click")}, {ACT_MINUS, R("menu/click")}, {ACT_RIGHT_AIM, R("aim/pose")}, {ACT_RIGHT_GRIP, R("grip/pose")}, HAPTICS,
};
#undef L
#undef R

static const struct { const char *profile; const Binding *bindings; uint32_t count; } profiles[] = {
    {"/interaction_profiles/valve/index_controller", index_bindings, sizeof index_bindings / sizeof *index_bindings},
    {"/interaction_profiles/oculus/touch_controller", touch_bindings, sizeof touch_bindings / sizeof *touch_bindings},
    {"/interaction_profiles/htc/vive_controller", vive_bindings, sizeof vive_bindings / sizeof *vive_bindings},
    {"/interaction_profiles/microsoft/motion_controller", wmr_bindings, sizeof wmr_bindings / sizeof *wmr_bindings},
    {"/interaction_profiles/khr/simple_controller", simple_bindings, sizeof simple_bindings / sizeof *simple_bindings},
};

static XrSession session = XR_NULL_HANDLE;
static XrActionSet action_set = XR_NULL_HANDLE;
static XrAction actions[ACT_COUNT];
static XrSpace aim_space[2], grip_space[2];
static _Atomic int g_pointer_hand = -1;   // (menu_input) the hand the menu pointer follows, -1 none
static _Atomic bool g_pointer_held;       // the trigger is being pulled: the pointer stays where it was
static _Atomic uint64_t g_last_space;     // (vr_input_sync) the space hands are located in
static _Atomic float g_blade_tip_speed;   // (vr_input_sync) the right hand's sword tip, m/s in the room
static _Atomic uint64_t g_blade_tip_at;   // ... when (SDL ns)
// (hle/vr_sword.c, game thread) the sword tip's speed from the controller's own motion; false if not measured lately
bool vr_input_blade_speed(float *mps) {
    const uint64_t at = atomic_load(&g_blade_tip_at);
    if (!at || SDL_GetTicksNS() - at > 100000000ull) return false;
    *mps = atomic_load(&g_blade_tip_speed);
    return true;
}
static void locate_hand(XrSpace hand, XrSpace space, XrTime time, SkyVrHand *out);

static bool ok(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    LOG("[vr] %s failed (%d)\n", what, (int)r);
    return false;
}

static bool suggest(XrInstance instance, uint32_t p) {
    XrActionSuggestedBinding b[32];
    uint32_t n = 0;
    for (uint32_t i = 0; i < profiles[p].count && n < 32; i++) {
        if (!ok(p_xrStringToPath(instance, profiles[p].bindings[i].path, &b[n].binding), profiles[p].bindings[i].path))
            return false;
        b[n++].action = actions[profiles[p].bindings[i].action];
    }
    XrPath profile;
    if (!ok(p_xrStringToPath(instance, profiles[p].profile, &profile), profiles[p].profile)) return false;
    XrInteractionProfileSuggestedBinding sb = {.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                               .interactionProfile = profile, .countSuggestedBindings = n,
                                               .suggestedBindings = b};
    return ok(p_xrSuggestInteractionProfileBindings(instance, &sb), profiles[p].profile);
}

bool vr_input_create(XrInstance instance, XrSession s) {
    XrActionSetCreateInfo si = {.type = XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(si.actionSetName, sizeof si.actionSetName, "joycons");
    snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Joy-Cons (Skyward Sword)");
    if (!ok(p_xrCreateActionSet(instance, &si, &action_set), "xrCreateActionSet")) return false;
    for (int i = 0; i < ACT_COUNT; i++) {
        XrActionCreateInfo ai = {.type = XR_TYPE_ACTION_CREATE_INFO, .actionType = action_defs[i].type};
        snprintf(ai.actionName, sizeof ai.actionName, "%s", action_defs[i].name);
        snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", action_defs[i].label);
        if (!ok(p_xrCreateAction(action_set, &ai, &actions[i]), action_defs[i].name)) return false;
    }
    int suggested = 0;
    for (uint32_t p = 0; p < sizeof profiles / sizeof *profiles; p++) suggested += suggest(instance, p);
    if (!suggested) return false;
    XrSessionActionSetsAttachInfo at = {.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1,
                                        .actionSets = &action_set};
    if (!ok(p_xrAttachSessionActionSets(s, &at), "xrAttachSessionActionSets")) return false;
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo as = {.type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
                                      .poseInActionSpace.orientation.w = 1};
        as.action = actions[h ? ACT_RIGHT_AIM : ACT_LEFT_AIM];
        if (!ok(p_xrCreateActionSpace(s, &as, &aim_space[h]), "xrCreateActionSpace(aim)")) return false;
        as.action = actions[h ? ACT_RIGHT_GRIP : ACT_LEFT_GRIP];
        if (!ok(p_xrCreateActionSpace(s, &as, &grip_space[h]), "xrCreateActionSpace(grip)")) return false;
    }
    session = s;
    LOG("[vr] controller actions ready (%d of %d controller profiles bound)\n", suggested,
        (int)(sizeof profiles / sizeof *profiles));
    return true;
}

void vr_input_destroy(void) {
    sky_joycon_reset();
    for (int h = 0; h < 2; h++) {
        if (aim_space[h] && p_xrDestroySpace) p_xrDestroySpace(aim_space[h]);
        if (grip_space[h] && p_xrDestroySpace) p_xrDestroySpace(grip_space[h]);
        aim_space[h] = grip_space[h] = XR_NULL_HANDLE;
    }
    if (action_set && p_xrDestroyActionSet) p_xrDestroyActionSet(action_set);  // (its actions with it)
    action_set = XR_NULL_HANDLE;
    memset(actions, 0, sizeof actions);
    session = XR_NULL_HANDLE;
}

// A short vibration on one hand (amplitude 0..1).
static void haptic(int hand, float amplitude, float ms) {
    if (!session || !p_xrApplyHapticFeedback) return;
    XrHapticActionInfo hi = {.type = XR_TYPE_HAPTIC_ACTION_INFO, .action = actions[hand ? ACT_RIGHT_HAPTIC : ACT_LEFT_HAPTIC]};
    XrHapticVibration v = {.type = XR_TYPE_HAPTIC_VIBRATION, .duration = (XrDuration)(ms * 1e6f),
                           .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = amplitude};
    p_xrApplyHapticFeedback(session, &hi, (const XrHapticBaseHeader *)&v);
}

// The settings menu / name entry on the VR panel (ui/menu.mm), from the controllers (after ShipWright VR):
//  - holding the left stick click 0.6 s opens / closes the settings menu; a tap still reaches the game, as a short
//    press when released (at press time a tap can't be told from the start of a hold);
//  - the pointer is the hand that last pulled its trigger (the right one first); the trigger clicks past 0.55 and
//    lets go under 0.35; that hand's stick scrolls; B closes the menu; a click on the panel buzzes;
//  - whatever is held when the menu closes stays hidden from the game until released (the closing click doesn't
//    swing the sword).
static void menu_input(SkyVrControllers *s) {
    extern bool menu_is_open(void), menu_input_captured(void);
    extern void menu_vr_close(void), menu_vr_toggle(void);
    extern bool vr_menu_ray(float *u, float *v, bool *click);
    extern _Atomic bool g_vr_right_trigger;   // (vr_controls.c: the menu's click)
    extern _Atomic float g_vr_right_stick_y;  // (vr_controls.c: the menu's scrolling)
    const double now = SDL_GetTicksNS() / 1e9;
    static bool stick_was, stick_pending, b_was;
    static double stick_down_at, pulse_until;
    static int hand = 1;
    static bool down, was_captured;
    static uint32_t latch_buttons;
    static bool latch_trigger[2], latch_grip[2], latch_stick[2];

    // Left stick click: hold toggles the settings menu, a tap goes to the game on release.
    const bool stick = (s->buttons & SKY_JC_LSTICK) != 0;
    if (stick && !stick_was) stick_pending = true, stick_down_at = now;
    if (stick_pending && stick && now - stick_down_at >= 0.6) {
        stick_pending = false;
        menu_vr_toggle();
        haptic(0, 0.4f, 30);
    } else if (stick_pending && !stick) {
        stick_pending = false;
        if (menu_is_open()) menu_vr_close();
        else pulse_until = now + 0.12;  // (over a few game frames: it can't be missed)
    }
    stick_was = stick;
    s->buttons &= ~SKY_JC_LSTICK;
    if (now < pulse_until) s->buttons |= SKY_JC_LSTICK;

    const bool b = (s->buttons & SKY_JC_B) != 0;  // (OpenXR bypasses SDL, where the menu gets its Back)
    if (b && !b_was && menu_is_open()) menu_vr_close();
    b_was = b;

    const bool captured = menu_input_captured();
    if (captured) {
        const float trig[2] = {s->zl, s->zr};
        for (int h = 0; h < 2; h++)  // (the pointer follows the hand that pulls its trigger)
            if (h != hand && !down && trig[h] > 0.55f && s->hand[h].tracked) hand = h;
        const bool was_down = down;
        if (!down && trig[hand] > 0.55f) down = true;
        else if (down && trig[hand] < 0.35f) down = false;
        float u, v;
        bool click;
        if (down && !was_down && vr_menu_ray(&u, &v, &click)) haptic(hand, 0.25f, 15);
        atomic_store(&g_vr_right_trigger, down);
        atomic_store(&g_vr_right_stick_y, s->stick[hand][1]);
    } else {
        down = false;
        atomic_store(&g_vr_right_trigger, false);
        atomic_store(&g_vr_right_stick_y, 0.0f);
        if (was_captured) {  // (just closed: hold back what is still pressed)
            latch_buttons = s->buttons;
            latch_trigger[0] = s->zl > 0.2f, latch_trigger[1] = s->zr > 0.2f;
            latch_grip[0] = s->l > 0.2f, latch_grip[1] = s->r > 0.2f;
            for (int h = 0; h < 2; h++) latch_stick[h] = fabsf(s->stick[h][0]) > 0.25f || fabsf(s->stick[h][1]) > 0.25f;
        }
        latch_buttons &= s->buttons;
        s->buttons &= ~latch_buttons;
        float *trig[2] = {&s->zl, &s->zr}, *grip[2] = {&s->l, &s->r};
        for (int h = 0; h < 2; h++) {
            if (latch_trigger[h]) latch_trigger[h] = *trig[h] > 0.2f, *trig[h] = 0;
            if (latch_grip[h]) latch_grip[h] = *grip[h] > 0.2f, *grip[h] = 0;
            if (latch_stick[h]) {
                latch_stick[h] = fabsf(s->stick[h][0]) > 0.25f || fabsf(s->stick[h][1]) > 0.25f;
                s->stick[h][0] = s->stick[h][1] = 0;
            }
        }
    }
    was_captured = captured;
    // The pointer is held still while the trigger is being pulled (pulling it tilts the controller: the press would
    // land beside where it aimed), and its small tremor is smoothed: tiny turns are followed gradually, quick ones
    // at once.
    static float aim_q[4];
    static int aim_hand = -1;
    const SkyVrHand *ah = &s->hand[hand];
    if (!s->active || !ah->tracked) {
        aim_hand = -1;
        atomic_store(&g_pointer_hand, -1);
        vr_pointer_set_aim(false, ah->orientation, ah->position);
        return;
    }
    const float pull = hand ? s->zr : s->zl;
    const bool held = captured && !down && pull > 0.08f && aim_hand == hand;
    atomic_store(&g_pointer_held, held);
    atomic_store(&g_pointer_hand, hand);
    if (held) return;  // (held: the last aim stays)
    const float *q = ah->orientation;
    if (aim_hand != hand) memcpy(aim_q, q, sizeof aim_q), aim_hand = hand;
    float dot = aim_q[0] * q[0] + aim_q[1] * q[1] + aim_q[2] * q[2] + aim_q[3] * q[3];
    const float sign = dot < 0 ? -1.0f : 1.0f;
    dot = fminf(1.0f, fabsf(dot));
    const float angle = 2 * acosf(dot);  // (radians turned since the smoothed aim)
    const float a = fminf(1.0f, fmaxf(0.12f, angle / 0.035f));
    float l = 0;
    for (int i = 0; i < 4; i++) aim_q[i] += (q[i] * sign - aim_q[i]) * a, l += aim_q[i] * aim_q[i];
    l = sqrtf(l);
    for (int i = 0; i < 4; i++) aim_q[i] /= l;
    vr_pointer_set_aim(true, aim_q, ah->position);
}

// (vr_controls.c, game thread) The pointing hand where it will be at `time` (this session's last space): the menu
// pointer is placed per game frame, not from whichever render-thread sample happens to be latest.
bool vr_input_pointer_at(int64_t time, float q[4], float p[3]) {
    const int hand = atomic_load(&g_pointer_hand);
    const XrSpace space = (XrSpace)(uintptr_t)atomic_load(&g_last_space);
    if (!session || !space || hand < 0 || hand > 1 || time <= 0 || atomic_load(&g_pointer_held)) return false;
    SkyVrHand h;
    locate_hand(aim_space[hand], space, (XrTime)time, &h);
    if (!h.tracked) return false;
    memcpy(q, h.orientation, 4 * sizeof(float));
    memcpy(p, h.position, 3 * sizeof(float));
    return true;
}

static bool get_bool(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = actions[a]};
    XrActionStateBoolean st = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(p_xrGetActionStateBoolean(session, &gi, &st)) && st.isActive && st.currentState;
}

static float get_float(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = actions[a]};
    XrActionStateFloat st = {.type = XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(p_xrGetActionStateFloat(session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}

static XrVector2f get_vec2(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = actions[a]};
    XrActionStateVector2f st = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(p_xrGetActionStateVector2f(session, &gi, &st)) && st.isActive) return st.currentState;
    return (XrVector2f){0, 0};
}

static void locate_hand(XrSpace hand, XrSpace space, XrTime time, SkyVrHand *out) {
    XrSpaceVelocity vel = {.type = XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION, .next = &vel};
    memset(out, 0, sizeof *out);
    if (!hand || XR_FAILED(p_xrLocateSpace(hand, space, time, &loc))) return;
    const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & valid) != valid) return;
    out->tracked = true;
    out->orientation[0] = loc.pose.orientation.x; out->orientation[1] = loc.pose.orientation.y;
    out->orientation[2] = loc.pose.orientation.z; out->orientation[3] = loc.pose.orientation.w;
    out->position[0] = loc.pose.position.x; out->position[1] = loc.pose.position.y; out->position[2] = loc.pose.position.z;
    const XrSpaceVelocityFlags both = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    out->velocity_valid = (vel.velocityFlags & both) == both;
    out->linear_velocity[0] = vel.linearVelocity.x; out->linear_velocity[1] = vel.linearVelocity.y;
    out->linear_velocity[2] = vel.linearVelocity.z;
    out->angular_velocity[0] = vel.angularVelocity.x; out->angular_velocity[1] = vel.angularVelocity.y;
    out->angular_velocity[2] = vel.angularVelocity.z;
}

void vr_input_sync(XrSpace space, XrTime time, const float reference[7]) {
    SkyVrControllers s = {.reference = {0, 0, 0, 1}};
    atomic_store(&g_last_space, (uint64_t)(uintptr_t)space);
    if (session && space && time > 0) {
        XrActiveActionSet active = {.actionSet = action_set};
        XrActionsSyncInfo sync = {.type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active};
        s.active = XR_SUCCEEDED(p_xrSyncActions(session, &sync));
    }
    if (s.active) {
        s.time_ns = time;
        if (reference) memcpy(s.reference, reference, sizeof s.reference);
        // (+ / − open the Gear menu and the map: not while a menu already pauses the game - pressed there, they
        // invoked the menu again; B closes it)
        extern bool vr_game_paused(void);
        const bool paused = vr_game_paused();
        for (int a = ACT_A; a <= ACT_RSTICK; a++)
            if (get_bool(a) && !(paused && (a == ACT_PLUS || a == ACT_MINUS))) s.buttons |= action_defs[a].button;
        if (get_bool(ACT_DPAD_PRESS)) {  // the pressed trackpad's direction as one d-pad button
            const XrVector2f d = get_vec2(ACT_DPAD);
            if (d.x * d.x + d.y * d.y > 0.09f)
                s.buttons |= fabsf(d.x) > fabsf(d.y) ? (d.x > 0 ? SKY_JC_RIGHT : SKY_JC_LEFT)
                                                     : (d.y > 0 ? SKY_JC_UP : SKY_JC_DOWN);
        }
        s.zl = get_float(ACT_ZL);
        s.zr = get_float(ACT_ZR);
        s.l = get_float(ACT_L);
        s.r = get_float(ACT_R);
        const XrVector2f ls = get_vec2(ACT_LEFT_STICK), rs = get_vec2(ACT_RIGHT_STICK);
        s.stick[0][0] = ls.x; s.stick[0][1] = ls.y;
        { extern float g_door_stick; g_door_stick = sqrtf(ls.x * ls.x + ls.y * ls.y); }  // (hle/door.c trace)
        s.stick[1][0] = rs.x; s.stick[1][1] = rs.y;
        for (int h = 0; h < 2; h++) locate_hand(aim_space[h], space, time, &s.hand[h]);
        // the sword's tip in the right hand, its speed in the room (m/s) - the hand's own motion, not walking or
        // turning (hle/vr_sword.c): v + w x r, r the blade (0.8 m along the controller's -z)
        const SkyVrHand *r = &s.hand[1];
        if (r->tracked && r->velocity_valid) {
            const float *q = r->orientation, x = q[0], y = q[1], z = q[2], w = q[3];
            const float b[3] = {-0.8f * 2 * (x * z + w * y), -0.8f * 2 * (y * z - w * x), -0.8f * (1 - 2 * (x * x + y * y))};
            const float *v = r->linear_velocity, *a = r->angular_velocity;
            const float t[3] = {v[0] + a[1] * b[2] - a[2] * b[1], v[1] + a[2] * b[0] - a[0] * b[2], v[2] + a[0] * b[1] - a[1] * b[0]};
            atomic_store(&g_blade_tip_speed, sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]));
            atomic_store(&g_blade_tip_at, SDL_GetTicksNS());
        }
    }
    {   // (the right controller's pointing ray for aiming items: runtime/vr/vr_rig.c, runtime/hle/vr_aim.c)
        extern void vr_rig_set_aim(bool ok, const float pose[7]);
        const SkyVrHand *r = &s.hand[1];
        const float pose[7] = {r->orientation[0], r->orientation[1], r->orientation[2], r->orientation[3],
                               r->position[0], r->position[1], r->position[2]};
        vr_rig_set_aim(s.active && r->tracked, pose);
    }
    menu_input(&s);
    if (s.active) {  // The right stick's up / down is the d-pad's (pushed past 0.6, let go below 0.4); not the game's.
        static int held;  // (1 up, -1 down)
        const float x = s.stick[1][0], y = s.stick[1][1];
        if (!held && fabsf(y) > 0.6f && fabsf(y) > fabsf(x)) held = y > 0 ? 1 : -1;
        else if (held && held * y < 0.4f) held = 0;
        if (held) s.buttons |= held > 0 ? SKY_JC_UP : SKY_JC_DOWN;
        s.stick[1][1] = 0;
    }
    {   // The game walks Link relative to its own camera; in the headset the player looks around with the head.
        // During gameplay the left stick is turned by how far the eyes look from the game camera: forward walks
        // (and turns Link) where the player looks. Not in menus or cutscenes (the stick picks there).
        // SKYRECOMP_VR_STICK_FOLLOWS=camera: the game's own (as before).
        static int mode = -1;
        if (mode < 0) { const char *e = getenv("SKYRECOMP_VR_STICK_FOLLOWS"); mode = e && !strcmp(e, "camera") ? 0 : 1; }
        extern bool player_eye(float eye[3]);
        extern float vr_rig_view_yaw(void);
        const bool paused = *(const volatile uint32_t *)(uintptr_t)(MAIN_BASE + 0x1827370) != 0;
        if (mode && s.active && !paused && player_eye(NULL)) {
            // VR Options > Movement Direction: the head, or where the left / right controller points (the head
            // while that controller points too steeply up or down, or isn't tracked)
            extern bool vr_rig_pointing_yaw(const float reference[4], const float q[4], float *yaw);
            float yaw = vr_rig_view_yaw();
            const int h = g_vr_config.move_direction - 1;
            if (h >= 0 && h <= 1 && s.hand[h].tracked) vr_rig_pointing_yaw(s.reference, s.hand[h].orientation, &yaw);
            const float phi = -yaw;  // (toward the right: positive)
            const float c = cosf(phi), sn = sinf(phi), x = s.stick[0][0], y = s.stick[0][1];
            s.stick[0][0] = x * c + y * sn;
            s.stick[0][1] = -x * sn + y * c;
        }
        // (a menu over the live world - Gear, map: the left stick walked Link behind it; menus pick with the pointer)
        extern bool vr_world_menu_active(void);
        if (paused && vr_world_menu_active()) s.stick[0][0] = s.stick[0][1] = 0;
        // VR Options > Door Opening: at a door (Link's action 0x6a, hle/player.c), grab with a grip, turn the wrist at
        // least 90 degrees and pull the hand back 12 cm: A is pressed for the game, which plays its own door sequence
        // (the next room loads, the door closes behind him). Buzz on the grab and when the turn is far enough.
        {
            extern int player_action(void);
            static int hand = -1, press = 0;
            static bool opening;  // (the game's door sequence started under the hand: it keeps holding the door)
            static float q0[4], p0[3];
            static bool turned;
            static int loose;          // (frames the grip has been nearly let go: a held grip's force wavers)
            static float best_turn, best_pull;
            extern bool door_at(void);
            const bool at_door = !paused && door_at();
            const bool gesture = g_vr_config.door_mode != 2;
            {   // (diagnostics) Link's action as it changes, and each grip squeeze
                static int last_action = -2;
                static bool last_grip[2];
                const int action = player_action();
                if (action != last_action) LOG("[door] action %#x\n", action), last_action = action;
                const float g[2] = {s.l, s.r};
                for (int h = 0; h < 2; h++)
                    if ((g[h] > 0.6f) != last_grip[h]) {
                        last_grip[h] = g[h] > 0.6f;
                        LOG("[door] %s grip %s (%.2f) at door %d, tracked %d\n", h ? "right" : "left",
                            last_grip[h] ? "squeezed" : "released", g[h], at_door, s.hand[h].tracked);
                    }
            }
            if ((!at_door && !opening) || !gesture) hand = -1;
            if (hand < 0) opening = false;
            const float grip[2] = {s.l, s.r};
            // the handle (hle/door.c): a hand within reach of it hovers (a light tick), and only such a hand grabs
            extern bool door_handle(float w[3]);
            extern bool vr_rig_controller_world(int side, float w[3]);
            extern float vr_rig_units_per_metre(void);
            float handle[3], cw[3];
            const bool have_handle = (at_door || opening) && gesture && door_handle(handle);
            const float reach = 0.40f * vr_rig_units_per_metre();
            bool near[2] = {!have_handle, !have_handle};  // (no handle found: any grab, as before)
            static bool hovering[2];
            float near_face[2] = {0, 0};  // (which of the door's two handles each hand is nearer - either can be held)
            extern bool door_handle_on(float face, float w[3]);
            for (int h = 0; h < 2; h++) {
                if (have_handle && vr_rig_controller_world(h, cw)) {
                    float best = 1e30f;
                    for (int f = 0; f < 2; f++) {
                        float hw[3];
                        const float face = f ? 1.0f : -1.0f;
                        if (!door_handle_on(face, hw)) continue;
                        const float dx = cw[0] - hw[0], dy = cw[1] - hw[1], dz = cw[2] - hw[2], d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < best) best = d2, near_face[h] = face;
                    }
                    near[h] = best < reach * reach;
                }
                {   // (diagnostics) every half second at a door: the handle, each controller and its distance
                    static uint64_t next[2];
                    const uint64_t t = SDL_GetTicksNS();
                    if (at_door && t >= next[h]) {
                        next[h] = t + 500000000ull;
                        const bool cok = vr_rig_controller_world(h, cw);
                        LOG("[door] %s: handle %d (%.0f %.0f %.0f) controller %d (%.0f %.0f %.0f) reach %.0f\n",
                            h ? "right" : "left", have_handle, handle[0], handle[1], handle[2], cok, cw[0], cw[1], cw[2], reach);
                    }
                }
                if (near[h] && have_handle && !hovering[h] && hand < 0) haptic(h, 0.15f, 10);
                hovering[h] = near[h] && have_handle;
                {   // (vr_rig.c) hovering: the hand shown on the handle in the holding pose - it can be taken
                    extern void vr_rig_door_hover(int side, float face);
                    vr_rig_door_hover(h, hovering[h] && hand < 0 ? near_face[h] : 0.0f);
                }
            }
            if (at_door && gesture && hand < 0) {
                for (int h = 0; h < 2 && hand < 0; h++)
                    if (grip[h] > 0.5f && s.hand[h].tracked && near[h]) {
                        extern float door_open(void);  // (an open door needs no turn: it is grabbed as it is)
                        extern void door_set_handle_face(float face);
                        door_set_handle_face(near_face[h]);  // (the handle this hand took - either face's)
                        hand = h, turned = door_open() != 0.0f, loose = 0, best_turn = best_pull = 0;
                        LOG("[door] grab (%s hand)\n", h ? "right" : "left");
                        memcpy(q0, s.hand[h].orientation, sizeof q0), memcpy(p0, s.hand[h].position, sizeof p0);
                        haptic(h, 0.3f, 20);
                    }
            } else if (hand >= 0 && ((loose = grip[hand] < 0.1f ? loose + 1 : 0) > 20 || !s.hand[hand].tracked)) {
                LOG("[door] let go: turned %.0f deg, pulled %.0f cm\n", best_turn * 57.2958f, best_pull * 100);
                hand = -1;  // (let go: a third of a second under 10% - the door stays as it is)
            } else if (hand >= 0) {
                // the turn about the controller's own pointing axis since the grab: conj(q0) * q, its z (-forward) part
                const float *q = s.hand[hand].orientation;
                const float rw = q0[3] * q[3] + q0[0] * q[0] + q0[1] * q[1] + q0[2] * q[2];
                const float rz = q0[3] * q[2] - q0[2] * q[3] - q0[0] * q[1] + q0[1] * q[0];
                const float turn = fabsf(2.0f * atan2f(rz, rw));
                // a locked door: the handle turns, the door does not - the game's locked rattle, once per turn (turned back
                // near the start, it can rattle again)
                extern bool door_locked(void);
                static bool rattled;
                const bool locked = door_locked();
                if (locked && !rattled && turn > (float)(M_PI / 2)) {
                    extern void door_sound_locked(void);
                    LOG("[door] locked: the handle turns, the door stays shut\n");
                    door_sound_locked(), haptic(hand, 0.9f, 60), rattled = true;
                } else if (rattled && turn < (float)(M_PI / 6)) rattled = false;
                if (!locked && !turned && turn > (float)(M_PI / 2)) turned = true, haptic(hand, 0.5f, 30);
                // turned: the door follows the hand about its origin (hle/door.c) - the angle that keeps the
                // handle under it, measured as atan2(-z, x) in the door's closed frame; open wide enough, the game's
                // own sequence takes over (the next room loads)
                extern bool door_local(const float p[3], float l[3]);
                extern bool door_handle_local(float *x, float *z);
                extern void door_set_open(float radians);
                float hl[3], kx, kz;
                // (from where the hand is once the turn is done: grabbed up to 40 cm off the handle, and moved by the
                // turn itself, the hand alone read as the door already well open)
                static float a0, open0;
                static bool a0_set;
                static int a0_hand = -1;
                extern float door_open(void);
                extern float door_hinge_x(void);
                if (!turned || a0_hand != hand) a0_set = false;  // (each grab measures from itself)
                (void)kx, (void)kz;
                if (turned && have_handle && vr_rig_controller_world(hand, cw) && door_local(cw, hl)) {
                    // (about the hinge, not the door's middle; carried on from the angle it already had)
                    const float hx = hl[0] - door_hinge_x();
                    if (!a0_set) a0 = atan2f(-hl[2], hx), open0 = door_open(), a0_set = true, a0_hand = hand;
                    float d = atan2f(-hl[2], hx) - a0;
                    while (d > (float)M_PI) d -= 2 * (float)M_PI;
                    while (d < -(float)M_PI) d += 2 * (float)M_PI;
                    // (gain - ShipWright VR's door used 1.75, raised to 3: a comfortable arm arc swings the full leaf - the hand's own
                    // arc about the hinge is too small to keep up)
                    const float a = open0 + 3.0f * d;
                    const float lim = 90.0f * (float)(M_PI / 180);
                    door_set_open(fmaxf(-lim, fminf(lim, a)));
                    // as it starts to open, the room beyond is activated (hle/door.c - no door event: the door stays in
                    // the hand), so beyond the opening is never void
                    if (!opening && fabsf(a) > 3.0f * (float)(M_PI / 180)) {
                        extern void door_request_room(void);
                        LOG("[door] swung open %.0f deg: activating the room beyond\n", a * 57.2958f);
                        extern void door_sound(bool open);
                        if (fabsf(open0) < 0.01f) door_sound(true);  // (not again for a door grabbed already open)
                        door_request_room();
                        opening = true, haptic(hand, 0.6f, 40);
                    }
                }
                // the pull: how far the hand moved (level) since the grab - after the turn, at a door, any pull opens it
                const float *pp = s.hand[hand].position;
                const float pull = sqrtf((pp[0] - p0[0]) * (pp[0] - p0[0]) + (pp[2] - p0[2]) * (pp[2] - p0[2]));
                best_turn = fmaxf(best_turn, turn), best_pull = fmaxf(best_pull, pull);
                // (the door opens by being swung, above - no longer by a set pull distance: the turn alone moved
                // the controller that far)
                (void)pull;
            }
            {   // (vr_rig.c) the hand holding the handle is drawn on it
                extern void vr_rig_door_grab(int side);
                vr_rig_door_grab(have_handle ? hand : -1);
            }
            if (g_vr_config.door_mode == 1 && at_door) s.buttons &= ~(uint32_t)SKY_JC_A;  // (gesture only)
            if (press > 0) press--, s.buttons |= SKY_JC_A;
            // (open wide, Link walks through the doorway by himself - hle/door.c door_step_tick - and it shuts behind him;
            // otherwise the door stays as it was left until he is far from it)
            // (at a door the grips are the gesture's, not the game's L / R: squeezed, they made Link leave the door)
            if ((at_door && gesture) || hand >= 0) s.l = s.r = 0;
        }
        // VR Options > Turning: in 1st person the right stick turns the player (snap: a step per flick, smooth:
        // continuously) instead of the game's camera.
        if (g_vr_config.turn_mode != 2 && g_vr_config.view_mode == 1 && !paused && player_eye(NULL)) {
            extern void vr_turn(float radians);
            static bool armed = true;
            static XrTime last;
            const float x = s.stick[1][0];
            const float dt = last && time > last ? fminf((float)(time - last) * 1e-9f, 0.1f) : 0.0f;
            last = time;
            const float deg = (float)(M_PI / 180.0);
            if (g_vr_config.turn_mode == 0) {
                if (armed && fabsf(x) > 0.7f) vr_turn((x > 0 ? 1.0f : -1.0f) * g_vr_config.snap_turn_angle * deg), armed = false;
                else if (fabsf(x) < 0.3f) armed = true;
            } else if (fabsf(x) > 0.15f) {
                const float t = (fabsf(x) - 0.15f) / 0.85f * (x > 0 ? 1.0f : -1.0f);
                vr_turn(t * g_vr_config.smooth_turn_speed * deg * dt);
            }
            s.stick[1][0] = s.stick[1][1] = 0;
        }
    }
    // The game's motion sensors follow the aim poses (where each controller points); the grip poses (the controller
    // in the palm) place the wrist HUD.
    SkyVrHand grip[2] = {0};
    if (s.active)
        for (int h = 0; h < 2; h++) locate_hand(grip_space[h], space, time, &grip[h]);
    sky_joycon_update(&s);
    // In Auto mode, meaningful OpenXR use selects motion controls. Merely enabling VR never prevents using an SDL
    // gamepad in the headset; whichever source the player uses most recently wins.
    static bool motion_ready;
    const bool ready = s.active && s.hand[0].tracked && s.hand[1].tracked;
    float speed = 0.0f;
    for (int h = 0; h < 2; h++)
        if (s.hand[h].velocity_valid)
            for (int i = 0; i < 3; i++) speed += fabsf(s.hand[h].linear_velocity[i]) + .12f * fabsf(s.hand[h].angular_velocity[i]);
    const bool used = s.buttons || s.zl > .2f || s.zr > .2f || s.l > .2f || s.r > .2f ||
                      fabsf(s.stick[0][0]) > .25f || fabsf(s.stick[0][1]) > .25f ||
                      fabsf(s.stick[1][0]) > .25f || fabsf(s.stick[1][1]) > .25f || speed > .35f;
    if (ready && (!motion_ready || used)) {
        extern void motion_select_input(bool joycon);
        motion_select_input(true);
    }
    motion_ready = ready;
    float pose[2][7];
    for (int h = 0; h < 2; h++) {
        memcpy(pose[h], grip[h].orientation, 4 * sizeof(float));
        memcpy(pose[h] + 4, grip[h].position, 3 * sizeof(float));
    }
    vr_set_grip_poses(s.active && grip[0].tracked, pose[0], s.active && grip[1].tracked, pose[1]);

}

#endif
