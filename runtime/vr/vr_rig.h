// VR camera rig: where the headset's eyes are in the game world (runtime/vr_rig.c).
//
//   3rd person (default)  the anchor is the game's own camera; the head moves relative to it. Level horizon keeps
//                         its yaw and position but drops its pitch and roll; camera offsets move it up / back.
//   1st person            the anchor is Link's eye point (runtime/hle/player.c), facing where the game camera looked
//                         on entry (not turned by Link turning); falls back to 3rd person
//                         while Link's eye point is unknown (title, loading)
// Both: the head pose is taken relative to the reference (the head when tracking started or was recentred, yaw
// only), scaled by g_vr_config.world_scale game units per metre.
//
// Yaws are in the game's convention (runtime/hle/player.c): radians, 0 = +z, +pi/2 = +x, y up.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- renderer thread
void vr_rig_begin_frame(void);  // once per OpenXR frame: Link's pose and the view mode for this frame
// The eye's scene camera block (28 floats, StereoCameraHook.h). reference / eye: {qx,qy,qz,qw,px,py,pz} in the
// OpenXR local space; fov {left,right,up,down}. mono: any block with the game's scene projection; its view may be
// the camera's view times a model matrix (see game_camera_view, runtime/hle/player.c).
bool vr_rig_eye_camera(const float mono[28], const float reference[7], const float eye[7], const float fov[4],
                       float out[28]);
bool vr_rig_eye_camera_direct(const float mono[28], const float reference[7], const float eye[7], const float fov[4],
                              float out[28]);
// 1st person: true for a skinned draw of Link himself (hidden, as in the game's own 1st-person view). slot0: the
// draw's vertex slot 0 (the scene projection, size0 bytes); bones: its bone matrices (3x4 row-major each, already in
// game-camera space), bone_bytes long. vb/count identify the immutable mesh: once a large player-skeleton draw is
// confirmed as Link, that identity remains Link through animations that move every bone away from the eye. Small
// palettes (held items) and rigid draws are never classified as the body by proximity.
// SKYRECOMP_VR_SHOW_LINK=1 keeps him.
bool vr_rig_is_link_draw(const float *slot0, uint64_t size0, const float *bones, uint64_t bone_bytes,
                         uint64_t count, uint64_t vb, uint64_t vb_size);
// 1st person: Link's hands on the controllers (vr_rig.c). vr.c: the controllers' grip poses (OpenXR local space:
// x y z w, position) each frame; exec.c: which hand a rigid draw is (0 left, 1 right, -1 none) and that the draw
// being recorded is one (its camera then places it on the controller).
void vr_rig_set_grips(bool left_ok, const float left[7], bool right_ok, const float right[7]);
int vr_rig_hand_side(const float *slot0, uint64_t size0, uint64_t count, uint64_t vb, uint64_t vb_size);
void vr_rig_set_hand_draw(int side);
// (after vr_rig_set_hand_draw(-1)) a rigid draw that is the sword or shield in his hand: drawn with that hand
bool vr_rig_held_draw(const float *slot0, uint64_t size0, uint64_t count);
bool vr_rig_settling(void);  // 1st person, just spawned: Link's eye point not settled yet (the view is held black)
bool vr_rig_hand_blocks(float orig[12], float repl[12]);  // (exec.c) this hand draw's view block: game's, replacement
// A controller's grip pose in the game world (row-major 3x4: x right, y up, -z forward; game units), as of the last
// eye camera drawn with the real game camera (within 3 frames). side: 0 left, 1 right. Any thread.
bool vr_rig_controller_pose(int side, float c[12]);
// Once per stereo frame: both eyes (pose, fov) -> the field of view the game culls with (vr_rig_cull_fov).
void vr_rig_update_cull(const float reference[7], const float eye0[7], const float fov0[4], const float eye1[7],
                        const float fov1[4]);
void vr_rig_recenter(void);   // the next frame re-anchors (1st person: facing Link's direction again)
void vr_rig_toggle_view(void);  // 1st <-> 3rd person (F6, the menu)
float vr_rig_units_per_metre(void);

// ---- game thread: the camera's culling field of view while VR renders (runtime/hle/widescreen.c rt_fovy and
// rt_cull_aspect); false outside VR.
bool vr_rig_cull_fov(float *fovy_radians, float *aspect);
// ---- game thread: eye 0's view in the game world (row-major 3x4) and both eyes' field as tangents (left, right,
// down, up) while VR renders (runtime/hle/vr_cull.c); false outside VR.
bool vr_rig_cull_view(float view[12], float tangents[4]);
bool vr_rig_cull_trusted(void);  // the cull view is built on the real game camera (false: title, file select)
float vr_rig_view_yaw(void);  // the eyes' yaw from the game camera's (radians; 0 without the real game camera)
// 1st person: geometry nearer than this many game units is clipped (VR Options > Near Clip; default 15).
void vr_rig_set_near_clip(float units);

// ---- the math (tests: tools/test_vr_rig.c)
typedef struct VrRigAnchor {
    float rotation[9];  // anchor axes -> world, row-major (OpenXR-style anchor axes: x right, y up, -z forward)
    float origin[3];    // world
} VrRigAnchor;
// 3rd person: the game camera (its view, row-major 3x4 world -> camera), optionally levelled and offset (game units).
void vr_rig_anchor_camera(VrRigAnchor *a, const float game_view[12], bool level, float height, float distance);
// 1st person: a point and a yaw.
void vr_rig_anchor_point(VrRigAnchor *a, const float position[3], float yaw);
// The world -> eye view (row-major 3x4) for an eye pose relative to the reference, scaled into game units.
bool vr_rig_view(const VrRigAnchor *a, const float reference[7], const float eye[7], float units_per_metre,
                 float view[12]);
float vr_rig_camera_yaw(const float game_view[12]);  // yaw of the camera's forward direction
void vr_rig_yaw_only(float out[4], const float orientation[4]);  // the yaw part of an OpenXR orientation

#ifdef __cplusplus
}
#endif
