// Virtual 3D surface used to present the game's 2D HUD in VR.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VRHUDConfig {
    float hud_distance
#ifdef __cplusplus
        = 2.0f
#endif
        ;                         // metres in front of the viewer (0.5..10)
    float hud_scale
#ifdef __cplusplus
        = 1.0f
#endif
        ;                         // uniform surface scale (0.25..3)
    float hud_vertical_offset
#ifdef __cplusplus
        = 0.0f
#endif
        ;                         // metres (-2..2)
    bool curvature
#ifdef __cplusplus
        = false
#endif
        ;                         // flat quad or a subtle cylindrical surface
    bool lock_to_head
#ifdef __cplusplus
        = false
#endif
        ;                         // rigid head lock or smooth world-space follow
    bool wrist_hud
#ifdef __cplusplus
        = true
#endif
        ;                         // gameplay edge HUD follows the motion-controller hands
    bool wrist_hud_glance
#ifdef __cplusplus
        = false
#endif
        ;                         // status panel appears only while looking at the back of the wrist
    bool wrist_hud_swap_hands
#ifdef __cplusplus
        = false
#endif
        ;                         // status on right and item/action elements on left
    float wrist_hud_size
#ifdef __cplusplus
        = 0.20f
#endif
        ;                         // width of each flat wrist panel in metres
    float wrist_hud_side
#ifdef __cplusplus
        = 0.00f
#endif
        ;                         // outward from each grip pose, metres
    float wrist_hud_up
#ifdef __cplusplus
        = 0.04f
#endif
        ;                         // above each grip pose, metres
    float wrist_hud_forward
#ifdef __cplusplus
        = 0.06f
#endif
        ;                         // towards the fingers from each grip pose, metres
    float wrist_hud_tilt
#ifdef __cplusplus
        = -15.0f
#endif
        ;                         // wrist-relative panel pitch in degrees
    float wrist_hud_background
#ifdef __cplusplus
        = 0.46f
#endif
        ;                         // dark backing opacity (0..0.85)
    // The settings menu's own panel (vr.c): placed level in front of the gaze when it opens, then still.
    float menu_distance
#ifdef __cplusplus
        = 1.0f
#endif
        ;                         // metres in front of the head (0.4..3)
    float menu_width
#ifdef __cplusplus
        = 1.2f
#endif
        ;                         // metres (0.4..3); 16:10
    float menu_height
#ifdef __cplusplus
        = -0.1f
#endif
        ;                         // metres above (+) / below (-) eye level (-1..1)
    bool aim_reticle_3d
#ifdef __cplusplus
        = true
#endif
        ;                         // an aiming item's reticle at the shot's end point in the world (hle/vr_aim.c)
    bool aim_with_controller
#ifdef __cplusplus
        = true
#endif
        ;                         // an aimed item goes where the right controller points (hle/vr_aim.c)
} VRHUDConfig;

extern VRHUDConfig g_vr_hud_config;

void ResetVRHUDConfig(void);
extern bool g_vr_hud_hold;  // the panel holds still (the menu pointer is in use: vr.c sets it each frame)
void ValidateVRHUDConfig(void);

// Matrices use the runtime's convention: row-major, multiplied by column vectors. head_pose is tracking-space-from-
// head and camera_pose is world-from-tracking-space. The output is world-from-HUD-local-space. A HUD quad spans
// local x/y; its +z normal faces the viewer. NULL poses are treated as identity matrices.
void ComputeHUDQuadMatrix(float out_matrix[16], const float head_pose[16], const float camera_pose[16]);

// Deterministic variant for render loops and tests. delta_seconds is clamped to avoid jumps after a pause.
void ComputeHUDQuadMatrixDelta(float out_matrix[16], const float head_pose[16], const float camera_pose[16],
                               float delta_seconds);
void ResetHUDLazyFollow(void);

// Converts normalized quad coordinates (-1..1) to a local surface point. Curvature bends x around a subtle cylinder;
// callers can tessellate the HUD and transform each result by ComputeHUDQuadMatrix's output.
void ComputeHUDSurfacePoint(float out_position[3], float normalized_x, float normalized_y);

// Draws controls into the current ImGui window; it does not Begin/End a window.
void RenderVRHUDControls(void);

#ifdef __cplusplus
} // extern "C"
#endif
