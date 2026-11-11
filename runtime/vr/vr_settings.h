// User-facing VR configuration. This module does not depend on the VR renderer or rig.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VRConfig {
    float resolution_scale
#ifdef __cplusplus
        = 1.0f
#endif
        ;                                // fraction of OpenXR's recommended per-eye resolution (0.5..2.0)
    bool desktop_mirror
#ifdef __cplusplus
#ifdef __ANDROID__
        = false  // (a headset has no desktop window to mirror to)
#else
        = true
#endif
#endif
        ;                                // draw the left eye in the desktop window while VR is running
    bool dynamic_resolution
#ifdef __cplusplus
        = false
#endif
        ;                                // adapt VR resolution to the OpenXR display-time budget
    float dynamic_resolution_min
#ifdef __cplusplus
        = 0.7f
#endif
        ;                                // lowest adaptive resolution multiplier (0.5..resolution_scale)
    float world_scale
#ifdef __cplusplus
        = 100.0f
#endif
        ;                                // game units per metre (50..200)
    int view_mode
#ifdef __cplusplus
        = 0
#endif
        ;                                // 0: third-person follow, 1: first-person head
    float camera_height_offset
#ifdef __cplusplus
        = 0.0f
#endif
        ;                                // game units (-50..50)
    float camera_distance_offset
#ifdef __cplusplus
        = 0.0f
#endif
        ;                                // game units (-100..100)
    bool level_horizon
#ifdef __cplusplus
        = true
#endif
        ;
    int move_direction
#ifdef __cplusplus
        = 0
#endif
        ;                                // left stick forward: 0 where the head looks, 1 / 2 the left / right controller
    int turn_mode
#ifdef __cplusplus
        = 0
#endif
        ;                                // right stick, 1st person: 0 snap, 1 smooth, 2 off (the game's camera)
    float snap_turn_angle
#ifdef __cplusplus
        = 30.0f
#endif
        ;                                // degrees (15..90)
    float smooth_turn_speed
#ifdef __cplusplus
        = 120.0f
#endif
        ;                                // degrees per second at full tilt (30..360)
    int door_mode
#ifdef __cplusplus
        = 0
#endif
        ;                                // opening doors: 0 A or the grab-twist-pull gesture, 1 the gesture only, 2 A only
    bool sword_hitbox
#ifdef __cplusplus
        = true
#endif
        ;                                // 1st person: the sword's hits follow the blade in your right hand (hle/vr_sword.c)
    bool sword_swing_hits
#ifdef __cplusplus
        = true
#endif
        ;                                // ... and they're on whenever the blade moves fast, not only in the game's slashes
    bool sword_free_swings
#ifdef __cplusplus
        = true
#endif
        ;                                // 1st person: a fast swing hits by itself, in the direction swung (hle/vr_sword.c)
    float sword_swing_speed
#ifdef __cplusplus
        = 3.0f
#endif
        ;                                // ... once the blade tip moves faster than this (m/s)
} VRConfig;

// Always contains a validated configuration. LoadVRConfig resets it to defaults first.
extern VRConfig g_vr_config;

void ResetVRConfig(void);
void LoadVRConfig(const char *filepath);
void SaveVRConfig(const char *filepath);

#ifdef __cplusplus
} // extern "C"
#endif
