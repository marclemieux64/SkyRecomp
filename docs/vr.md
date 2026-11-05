# Experimental SteamVR / OpenXR support

VR is opt-in and off by default. Use **Enable VR Mode** in VR Options to enter or leave VR live.
`SKYRECOMP_VR=0` / `=1` before launch overrides the saved setting.

The launcher always opens on desktop without starting OpenXR or asking for a play mode. After **Play**, if VR is
enabled, Windows requests SteamVR startup once if necessary. Desktop play continues while its server and compositor
start. Once both have been present for three seconds, the game connects to OpenXR and switches into VR when the
session is ready. Failed connections retry every ten seconds; SteamVR starting later is detected automatically.
Disabling VR stops automatic connection attempts. Returning to the launcher disconnects VR.

On Windows, name entry keeps the normal game update and stereo render loop running behind the keyboard panel.
Only the game's naming callback waits for the answer, on its own native and guest stacks; it resumes and writes
the name after confirmation. Fresh left/right views follow head movement while background animations continue.
This also applies when VR is enabled but SteamVR is still starting. Other platforms retain the synchronous
name-entry fallback for now.

Automatic VR pointer calibration runs without opening the game's `SYS_GYRO_00` message or triggering its UI sounds.
The direct gyro-reset routine uses its native silent branch, bypassing the separate `0x13bd` reset sound and
its sound-driven feedback. A temporary native gate rejects duplicate reset calls without holding that feedback
state across rendered frames. Desktop reset behavior remains unchanged.
The calibration and cleanup steps still execute, while background music keeps playing at the normal volume.
The centered sensor baseline settles during startup and while the name keyboard is open, before the native
calibration wait. If it is not ready, calibration still waits for the required samples rather than using an
unstable baseline.
Calibration callbacks preserve the native HUD layout visibility, and the renderer keeps drawing the HUD throughout
the reset. No captured menu image is substituted for the live layouts.

On Windows, runtime logs are formatted before writing to unbuffered stderr, so routine VR diagnostics issue
one write per message instead of many small writes on the render thread.

Standard solid-colour game fades use a final head-locked OpenXR quad overlay covering the headset view, including
the floating HUD, wrist panels, keyboard and pointer. The renderer reads the game's actual uniform RGBA and
projects its four vertices to identify a full-view fade; overlapping fades retain their alpha blending order.
Textured backgrounds, partial rectangles, and depth/stencil-clipped draws retain their native routing.
The desktop VR mirror shows the same full-view coverage. `SKYRECOMP_VR_FADE_TRACE=1` traces extraction and
composition; `SKYRECOMP_VR_FULL_VIEW_FADE=0` restores native fade routing for diagnosis.
Black-fade transmission is converted from the game's display blend space to OpenXR linear light, preventing
bright logos/effects from remaining conspicuous through near-black fades. The native mirror opacity is retained.
The veil uses a single-image overlay swapchain and is submitted after the HUD/menu overlays. SteamVR composited-eye
captures verify that the opaque startup fade covers the title logo, then reveals it as the fade opens.
The shared draw handler also covers non-blended opaque fills between fade-out and fade-in, including the black
hold after confirming a new save name. An opaque background followed by ordinary content remains local to its
panel; consecutive fade fills retain their native ordering. The opaque hold is not mistaken for the end of a fade.

For shortcuts, `skyrecomp.exe --desktop` forces desktop mode; `skyrecomp.exe --vr` enables automatic VR connection
after Play. These flags override `SKYRECOMP_VR` for that process. Other OpenXR runtimes use their normal API readiness
checks; automatic SteamVR process detection and launching currently apply to Windows.

This build covers VR rendering (stereo output, the head-tracked camera in 1st / 3rd person, culling, per-eye visual
fixes, performance), the HUD panel / world behind menus / wrist HUD, and the VR controllers as the game's Joy-Con
pair: buttons, sticks and motion (runtime/vr/vr_input.c bindings for Index, Touch, Vive, WMR and the simple
controller; runtime/hle/motion.c the motion sensors). Not here: Link's hands on the controllers, the Bow, item
aiming, the menu pointer, teleport and rumble. With VR on the game always uses the Joy-Con pair (motion controls); a
gamepad's buttons still work alongside.

**VR Resolution** in VR Options controls the game's render resolution relative to OpenXR's recommended per-eye size.
`1.0x` renders at the recommendation; values above it supersample for a sharper image at a substantial GPU cost.
The setting applies live and is saved as `resolution_scale` in the `[VR]` section of `skyrecomp.ini`.
Turn off **Desktop Mirror** to skip drawing the left eye into the desktop window. This leaves the headset image
unchanged and avoids the mirror presentation and its optional desktop post-processing. Opening the menu temporarily
restores the mirror so its settings remain visible.

**Adaptive VR Resolution** treats VR Resolution as a quality ceiling and lowers it only after sustained frame
pressure, down to Adaptive Minimum. It raises quality again gradually after sustained headroom and requires no eye
tracking, so it works with Valve Index.

Failures reported by the OpenXR API are logged (`[vr]`); desktop play continues while connection retries wait.
Waiting for SteamVR's compositor avoids entering the driver with only its server running. This is a readiness
check, not isolation from arbitrary driver crashes. `--desktop` bypasses OpenXR entirely.

## Build dependency

OpenXR is detected at compile time (`runtime/vr/vr.c`; define `SKYRECOMP_NO_OPENXR` to build without it). MSYS2 UCRT64:

```sh
pacman -S mingw-w64-ucrt-x86_64-openxr-sdk
```

A release ships `libopenxr_loader.dll` (MSYS2) or `openxr_loader.dll` (Khronos SDK) next to `skyrecomp.exe`; it is
loaded only when VR is on.

## Where the code is

All VR code is in `runtime/vr/`, except what has to live inside the renderer (`runtime/nvn/vulkan/`) or the game
hooks (`runtime/hle/`).

| File | What |
|---|---|
| `vr.c`, `vr_xr.h` | OpenXR core: instance, session, frame loop, eye images, poses, resolution scale |
| `vr_input.c`, `vr_controls.c` | The controllers as the game's Joy-Con pair (OpenXR actions, buttons, sticks, motion sensors), grip poses for the wrist HUD |
| `vr_rig.c` | Camera rig: eye cameras, 1st / 3rd person, hiding Link's body, culling field of view |
| `vr_stereo_cull.cpp` | Stereo culling helpers |
| `vr_settings.cpp` | The `[VR]` section of `skyrecomp.ini` |

Headers: `vr.h` and `vr_rig.h` are the public interfaces; `vr_internal.h` and `vr_rig_internal.h` (shared frame
state `g_rig`, matrix helpers) are internal. `tools/test_vr_rig.c` tests vr_rig.c alone.

## How a frame is rendered

| Step | Where | What |
|---|---|---|
| Frame begin | `vr_frame_begin` (first `backend_execute` of a frame) | `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews`: one pose sample for both eyes of the whole frame |
| Per command list | `exec_list_stereo` (`runtime/nvn/vulkan/exec.c`) | the list is recorded for eye 0, then again for eye 1 |
| Eye images | `vk_vr_set_eye` (`textures.c`) | in eye 1, every texture the GPU writes gets an eye-1 image of its own; textures only sampled stay shared |
| Eye camera | `cbuf_info` -> `vr_patch_scene_ubo` -> `StereoCameraHook` | the scene camera block of each draw becomes the eye's camera, in the draw's own copy |
| Present | `backend_present` (`device.c`) | both eyes' presented images are blitted into the two array layers of one OpenXR swapchain image; the desktop window mirrors the left eye |
| Frame end | `vr_frame_end`, after the queue submission | `xrReleaseSwapchainImage`, `xrEndFrame` with one projection layer |

### The two eyes see the same inputs

Eye 1 records a list right after eye 0 recorded it, from the same starting point:

- the renderer state (`st` in `exec.c`) is restored to what eye 0 started with;
- guest memory written by the list itself (inline uniform updates, buffer copies, readbacks) is journaled during
  eye 0 and rolled back for eye 1, which repeats the updates and copies in order. Afterwards every journaled range is
  set back to eye 0's end state, so the game sees exactly one execution;
- once-only work stays with eye 0: compute dispatches, fences, the game's GPU timing counters and GPU-to-CPU readbacks.

Depth-copy aliasing (`alias_make`) is off while VR runs: it swaps textures' own images, which would mix the eyes.

### Camera (`runtime/nvn/vulkan/StereoCameraHook.*`)

The game's scene camera is the vertex stage's uniform slot 0 (864 bytes); its first 28 floats, both matrices row-major:

| Floats | Contents |
|---|---|
| 0..15 | projection, 4x4: perspective with `w = -z` (`P[3][2] = -1`), `P[2][3] = 1` |
| 16..27 | view, 3x4 affine `[R | t]`; eye space right-handed, -Z forward, +Y up |

Per eye, the view is composed with the inverse of the eye's pose relative to the reference head pose (taken on the
first tracked frame, and again after a SteamVR recentre), measured in the reference's own axes and scaled by
100 game units per metre (`SKYRECOMP_VR_WORLD_SCALE`; `SKYRECOMP_VR_SCALE` is now the render scale, 0.5..2). The projection's X/Y rows become the headset's asymmetric frustum;
its depth rows and Y sign stay the game's. Guest memory always keeps the game's mono camera.

Head rotation goes through the camera only. The game itself still aims with its own camera.

### Culling (`runtime/hle/vr_cull.c`)

The game culls its objects (characters, doors, flags, grass, birds, effects) against its camera's 60-degree frustum;
in a headset, those outside it popped in as the game camera turned towards them, not the eyes. The level's own
geometry is not culled that way (a head turned 160 degrees still sees it). The cull test (main+0xdf5ab4) reads the
culling camera through main+0x2bf9958: a box around the frustum and six planes, rebuilt by the camera's update. After
each update, the box is opened and four side planes are rebuilt from eye 0's view and both eyes' field
(`vr_rig_cull_view`), 10 degrees wider per edge, meeting 0.3 m behind the eye. The plane along the camera's forward
axis stays: it carries each object's draw distance (actor +0x23c), so draw distances are the game's. Skyloft: about
10-20% more draws. `SKYRECOMP_VR_WIDE_CULL=0` keeps the game's culling; `=2` opens it fully, also without VR (test:
desktop draws double).

**Draw distance** (Display > Rendering, also without VR; `draw_distance` in the ini, 1 to 4) multiplies every
object's draw distance: the forward plane is scaled by 1/k after each camera update, so the game's test
`depth - c > dist` becomes `(depth - c) / k > dist`. Skyloft on desktop: about +35% draws at 2x, +60% at 4x. The
level's scenery is always drawn; model level-of-detail switches are the game's own. Measured 2026-10-26:
the game's textures already use 16x anisotropic trilinear filtering, and its post chain has no depth-of-field in
gameplay (the full-screen pass after the 480x270 blur chain is bloom: edge detail is the same without it).

Many blocks don't hold the camera itself: their view is the camera's view times the draw's model matrix (terrain
chunks with their own origin, the sky dome scaled x2500, Link's skinned parts). The eye goes into the projection,
P' = Pe * Ve * Vg^-1, with Vg the game camera read from game memory (`game_camera_view`, `runtime/hle/player.c`), so
every block with the game's scene projection gets the same correction and keeps its own view. That memory is trusted
while some draw's view equals it (this frame or the last); otherwise, and for mirrored (reflection) blocks, a block
that is a plain camera falls back to its own view. Without this, 1st person showed chunks shifted by their origin
(a bright sky panel, a floating strip of grass) and 3rd person drew Link upside down in the air.

In 1st person Link's own draws are filtered (`vr_rig_is_link_draw`): a large player-skeleton draw (26 matrices or
more) with a bone within 110 units of his eye confirms that mesh as Link; its sampled vertex-data identity then stays
hidden through animations that move the bones away. Small palettes are held items and rigid draws can be equipment
or effects, so neither is hidden by proximity. The near plane clips remaining head intersections; his shadow stays.
His head is always hidden: head-only palettes (16..99 matrices: hair 24, face 44) with a bone within 30 units of
the eye, and tiny rigid meshes (his eyes, <= 96 vertices) within 12 units, and his belt's rigid pieces (centred
under him, 15..110 units below the eye). His hands (rigid, to the sides) stay. VR Options >
Movement Direction turns the left stick by the head's (or a controller's) yaw from the game camera, during gameplay,
so forward walks where the player looks (or points). `SKYRECOMP_VR_SHOW_LINK=1` keeps all of Link. Theatre: while the game camera is more than 1500 units from Link's
eye point (Loftwing flight, where that point stops updating; far cinematic cameras) 1st person shows the 3rd-person
view after 20 frames (a one-off camera cut doesn't switch it), and returns below 1000 units.

Swimming: the game keeps Link's eye point at the water surface, which would cut the view in half (and the water's
screen-space reflection breaks into flat fans right at its plane). In water the eye point is more than 153 units above
his feet (Link's actor +0x154; on land it is feet + 150, up to 151.2 measured) and holds perfectly still for 10
frames. Then, while Link himself is drawn there, the eye is raised 12 units above the surface, eased in and out.
The knight's rescue gives the same height numbers, but Link isn't drawn there. Following his feet instead fails
(swimming at the surface they sit 154..196 below it); diving (an item later in the game) isn't handled yet: the
game's eye point stays at the surface.
`SKYRECOMP_PLAYER_SNAP=<file>` records Link's actor and the player globals every 30 frames for this kind of search.

1st person moves the near plane to 15 units (`SKYRECOMP_VR_NEAR`) so Link's own head is clipped. Checked in Skyloft
against near = 1: only the head pieces differ (no fog, depth-of-field or depth-fighting change).

### The sword and shield in his hands (`runtime/vr/vr_rig.c`, `vr_rig_held_draw`)

Link's hands go to the controllers (`hand_block`), but what he holds is drawn on its own: the sword (index count
1056) and the shield (2352) are rigid draws the game poses on his hand's joint. Left alone they stay where the game
animates them while the hand drawn goes to its controller. Each one in a hand is drawn through the same transform as
that hand, followed by its offset from the game's hand (`H^-1 M`, from the two view blocks: the game camera cancels).
So the grip is the game's own and nothing is tuned.

Held or not is read from where the game puts it. Measured in Skyloft, in first person: a drawn sword's hilt sits
10.3 to 10.6 units from the hand holding it, and a readied shield 10.8 units from its hand. Sheathed on his back, both
are 50 or more units from either hand, so the cut-off is 25. The offset is measured in a frame where both the item and
its hand are drawn. When the item comes first, the hand's draw measures it and the next frame uses it. Another draw
with exactly the held item's block (a part on the same joint) goes with it. `SKYRECOMP_VR_NO_HELD=1` leaves them where
the game puts them.
### The aiming reticle at the shot's end point (`runtime/hle/vr_aim.c`)

On the flat HUD panel, the Slingshot's, Bow's, Clawshots' and Gust Bellows' reticle sits at the panel's distance,
which doesn't match where the shot goes. With **Aiming Reticle in 3D** (`aim_reticle_3d`, on), its pieces are drawn
in each eye's own picture instead, moved to where the shot will land:

- **Which draws are the reticle:** the pieces are known by their texture names (`th_sling*`, `th_arrowCursor*`,
  `th_hook*`, `th_vaccume*`: texpatch role `R_AIM`). The plain `th_circle*` (the Slingshot's dot) counts only after a
  real piece in the same frame, because other screens draw it too.
- **The end point:** the game keeps no "where it lands" point. 48 points along the game camera's aim line, through
  the reticle's centre and 20 to 5000 units ahead, are projected into eye 0's picture. `exec.c aim_probe` copies the
  scene's depth at those points into a small host buffer and reads it a frame or two later, without waiting.
  `vr_aim_result` takes the first place where the line passes behind a surface between two neighbouring samples; a
  surface merely in front of the line (a hand) doesn't count. With no surface, it takes the end of the range.
- **Each eye's camera:** `vr_aim_camera` gets each eye's main-view camera from its scene draws into the frame's largest
  depth target. The eye block times the inverse of the game block cancels the draw's model, and hand and door draws are
  skipped, since their blocks were replaced. `vr_aim_shift` then moves each piece by the difference between where the
  point falls in that eye and on the game's screen, in the 2D block's translation (floats 19 and 23).

The reticle stays on the panel as before when there's no end point or eye camera yet, in a multiview pass (one
recording for both eyes can't move it per eye), and behind menus. `SKYRECOMP_VR_NO_AIM=1` turns it off and
`SKYRECOMP_AIM_LOG=1` logs the end point and its depth samples.

### The sword's hits follow your hand (`runtime/hle/vr_sword.c`)

VR Options > Sword Hits Follow Your Hand (`sword_hitbox` in `[VR]`, on by default; 1st person). The game keeps Link's
sword as a swept line in his actor at +0x66e4: the blade's tip (80 units along the sword model's +X from the hilt), a
point 12 units along, and both of last frame's, in world space. Its hits come from that line: with the line moved
2 m up (`SKYRECOMP_SWORD_SHIFT="0 200 0"`, a test) a swing at grass in front of Link cuts nothing. Link's update
(main+0xab7cf4) writes it every frame and is called through his vtable, so it is wrapped with `hle_override`; after
it runs, the line is replaced by the blade held by the right controller - along the grip's -z, 12 to 80 units out
(`vr_rig_controller_pose`: the grip in the game world from the eyes' own anchor, updated by every eye camera drawn
with the real game camera) - this frame's and the last frame's. The game still decides when Link swings (its motion
controls); where the blade is when it does is the player's hand.

Drawn or sheathed comes from the game's own line: its base point sits 19-20 units from Link's eye point
(main+0x1829a30) while sheathed, 47-125 while drawn and swinging (measured in Faron Woods); above 30 for 3 frames it
counts as drawn, below 25 for 10 frames as sheathed. Only Link's actor (`*(main+0x18299d0)`) is changed: the update
also serves the title screen's Link. `SKYRECOMP_SWORD_LOG=1` logs the line written every 120 frames.

The line, the update and the hit test were found with a memory search for positions that keep their place in the
drawn hilt's frame while the sword swings, and by watching which functions write them; the effect was confirmed in
play (grass and the training logs cut where the controller's blade passes).

### Frame rate and pacing

- The frame rate setting (Video > Frame rate) offers 60, 120 and 144 FPS. The Skyward Sword simulation step follows
  elapsed time, so any rate keeps game speed. In VR, 120 or 144 means the headset's own refresh (frame_rate.c
  `g_rt_headset_hz`: the shortest period xrWaitFrame has predicted; SteamVR lengthens it while it throttles) and the
  game's own limiter steps aside: xrWaitFrame paces.
- The desktop window is only a mirror in VR and must never wait for the monitor: MAILBOX, else IMMEDIATE (device.c
  `present_mode`). With FIFO - AMD on Windows has no MAILBOX - a 60 Hz monitor held the headset's frames to 60 Hz
  once the mirror's queue filled up; SteamVR saw late frames and throttled between 72 and 48 Hz (~57 fps). Fixed:
  a 144 Hz Index holds 144 in Skyloft.
- Asynchronous replay (nvn/async.c; on by default in and out of VR: Display > Multithreaded Rendering, next launch;
  `SKYRECOMP_ASYNC_REPLAY=0` / `1` overrides. Desktop at 144 FPS in Skyloft: ~108-120 fps without it, a steady 144
  with it): the game thread queues
  its command lists, presents and texture releases to one replay thread and builds the next frame meanwhile (one
  frame ahead at most). Fences carry the queue position of the item that signals them, so nvnSyncWait waits for the
  replay thread to pass it, then for the GPU. What the draws read from game memory for the VR camera (the game
  camera, Link's eye and feet; hle/player.c) is snapshotted with each list. The per-frame hooks that act on the game
  (scripted input, warps, tests) stay on the game thread (device.c `present_game_hooks`). Head tracking is sampled on
  the replay thread at xrWaitFrame, so it gains no latency; game input reaches the screen one frame later at most.
  Measured: a frame's critical path became the longer of game and replay instead of their sum (a heavy Skyloft view:
  10.9 -> 7.7 ms).
- Eye 1 reuses eye 0's guest-memory copies (vertex data, uniform buffers) by command (exec.c `eye_memo`,
  `SKYRECOMP_VR_NO_EYE_MEMO=1`): uploads 2.7 -> 1.5 GB per 2 s.

### The image in the headset

- Each eye goes through exec.c `vk_vr_eye_post` before the copy into the swapchain (`SKYRECOMP_VR_EYE_POST=0`: the
  plain blit). Rendered above the headset's size (VR Resolution > 1): a filtered downscale (shaders.c `down.frag`, a
  tent over one output pixel, 16 bilinear taps - a bilinear blit skipped most of a 1.5-2x image, so supersampling
  kept its aliasing), SMAA at the headset's size, a light RCAS sharpen. Below it: SMAA, then the Video tab's upscaler
  (FSR 1 / NIS). At native resolution: optional SMAA without sharpening, to avoid amplifying subpixel shimmer on
  thin geometry such as Skyloft's ladders and fences. Fast VR Post-Processing bypasses these optional passes.
  SMAA follows the Video tab's "Anti-aliasing". Cost at 1.5x: 0.66 ms per eye.
  The desktop window does the same when the internal resolution is above the window's size (exec.c
  `post_process`; 2x into 1280x720: jagged edges before, ~0.15 ms; `LARECOMP_POST_DOWN=0`: the old bilinear present).
- Depth targets a draw samples while they are attached, without writing them (title screen, file select), are read
  in place instead of copied (exec.c `g_depth_direct`; `LARECOMP_FEEDBACK_COPY=1`: the copy). Same image; the
  title's GPU frame at 1.5x: 12.4 -> 9.6 ms.
- Skyloft at 144 Hz: 1.0x 5.5 ms GPU (~132 fps), 1.3x 8.8 ms and 1.5x 9.9 ms (a steady 72, each frame shown twice).
- Fixed foveated shading (off by default: VR Options > Foveated Rendering, live; `SKYRECOMP_VR_FOVEATION=on`,
  `<inner>,<outer>` or `0` overrides; exec.c `vrs_view`): passes that
  render the eye-sized images get a VK_KHR_fragment_shading_rate image - 1x1 inside 0.6 of the half field of view
  from each eye's optical axis, 2x1 / 1x2 to 0.8, 2x2 beyond (RDNA2 has no 4x4; 8x8 texels). It works (2x2 blocks
  show), but measured in Skyloft at 1.5x it saves ~0.4 ms of ~10 even with everything at 2x2: the eye passes are
  bound by geometry (~185 draws per eye) and bandwidth, not pixel shading. The largest row of a capture (~2 ms, the
  frame's first pass on eye 0 only) is the SteamVR compositor sharing the GPU.
- Hidden area (opt-in, `SKYRECOMP_VR_HIDDEN_MASK=1`; `=2` paints it red): the Index hides 24.9% of each eye image
  (XR_KHR_visibility_mask, vr.c `vr_hidden_mesh`). After a whole-target depth clear on an eye-sized target the mesh
  is drawn into the depth at the far side from the clear value (exec.c `hidden_mask`); passes without depth get a
  D16 mask of their own and every eye pass rejects the hidden pixels with the depth bounds test (`hidden_mask_view`).
  Measured in Skyloft at 1.5x: -0.35 ms of ~10.3, and the game's blurs (bloom, haze) then read stale hidden pixels
  into the visible edge - so it stays off.
- Where the time goes at 1.5x (3 captures of ~12.5 ms): full-screen passes (1-19 draws: lighting, fog, post) ~7 ms,
  scene passes 3.3-4.7 ms, the eyes' post and copies ~0.7-1 ms. GPU averages over 300 frames (`SKYRECOMP_VR_PACE_TRACE=1`
  prints them): 1.0x 6.2 ms, 1.2x 7.7, 1.5x 10.3. Neither fewer shaded pixels (foveation, -0.4 ms), nor rejected
  pixels (hidden area, -0.35), nor colour compression hints (`LARECOMP_VK_FORMAT_LIST=1`, 0) nor dropping the barrier
  between passes (`LARECOMP_VK_EXP_NOBARRIER=1`, wrong images possible: -0.5) moves it much: the cost that grows
  with resolution is not explained yet - a Radeon GPU Profiler capture would show it.

- Barriers between passes (exec.c begin_pass): a full barrier used to start every pass (~119 per VR frame). Now the
  next pass gets one only after work outside passes; the game's own barriers (NVN `0x70`, `0x12`, ~52 per frame)
  count as that pass's; a pass drawing on to an image rendered since then gets an attachment barrier; a draw sampling
  such an image ends its pass behind a full barrier first. Full barriers at pass starts: ~56 -> ~2 per frame;
  GPU ~-0.1 ms. `LARECOMP_VK_FULL_BARRIERS=1`: the old way.

### Profiling with Radeon GPU Profiler

A capture (Skyloft, 1.5x, 2026-10-26; `RadeonDeveloperPanelCLI -m profiling -p skyrecomp --rgp-auto-capture=frame:2400`
with the Radeon Developer Service stopped): RGP calls the application CPU bound - the GPU idles ~30% of the frame,
waiting for the replay thread's submissions (both eyes of a list are recorded before they are submitted); barriers
take 1.5% of the frame. The three most expensive events were the desktop mirror's SMAA over the 3138x3492 eye image
(~1.2 ms): the mirror now shows eye 0 as vk_vr_eye_post made it. GPU averages since: 1.0x 5.6 ms (141-144 fps),
1.5x 9.0 ms.

Submitting eye 0 before eye 1 is replayed (`SKYRECOMP_VR_SPLIT_EYES=1`, opt-in) changes nothing measurable: at 1.0x
the replay thread's own CPU time is the limit (xrBeginFrame -> xrEndFrame 7.0-7.2 ms for a 6.9 ms frame; the GPU's
idle time was its consequence), above 1.0x the GPU. Eye 1's reuse of eye 0's copies (`eye_memo`) holds across
submissions: a copy is reused while its transient chunk was not recycled (a generation per chunk), and the chunk is
then kept until the pending submission is done (`vk_transient_hold`).

The VR present's CPU time by part (`SKYRECOMP_VR_PACE_TRACE=1`, "present CPU ms/frame"), Skyloft at 1.0x / 144 Hz:
vr_poll 0.2, eye post + copies + HUD quad 0.15, xrEndFrame 0.55 - 0.9 ms in all and a steady 144 fps with the
desktop mirror off. Each mirror frame added ~0.3 ms and doubled xrEndFrame (~1.1 ms): 137 fps. The mirror is now
refreshed at most 60 times a second (VR Options > Mirror Refresh Rate: 30 / 60 / 90 Hz / Headset rate;
`SKYRECOMP_VR_MIRROR_HZ` overrides, 0: every frame): 142-144 fps with it on.

`SKYRECOMP_GPU_MARKERS=1` labels the GPU work (VK_EXT_debug_utils): each pass `[VR] Eye N Scene WxH` (an eye-sized
target with depth), `[VR] Eye N Post Processing WxH` (without), `[VR] Eye N Offscreen WxH`, `[VR] HUD`, then
`[VR] Eye N Post (SMAA / scale)`, `[VR] Eye copies` and `[VR] HUD Quad` at present. With the Radeon Developer Panel
connected (Radeon Developer Tool Suite), launch the game with that variable and capture a profile: RGP's timeline
and event list group the work under those names.

## Diagnostics

- `SKYRECOMP_VR_EYE_SHOT` also saves eye 0 as the headset gets it (`build/vr_post0*.png`).
- `SKYRECOMP_VR_PACE_TRACE=1`: every 300 frames the headset refresh, xrWaitFrame time, frame intervals and the time
  per OpenXR stage; `=2` adds a line per frame (lists, draws, passes, replay idle, GPU waits, predicted period,
  time waited, work) for 120 frames of every 1000. `SKYRECOMP_ASYNC_TRACE=1`: where the game and replay threads waited
  for each other. `LARECOMP_VK_PROF` adds eye 1's share, time per command type and eye, present and xrWaitFrame time
  (it costs ~4 ms per frame itself). `LARECOMP_FRAME_RATE_AT=<frame>:<rate>` (tests) changes the frame rate setting
  at a frame: scripted routes are recorded at 60 FPS.

- `SKYRECOMP_VR_TRACE=1`: every 300 frames, the stereo state, camera draws patched per eye, and each eye's pose and FOV;
  also the first few slot-0 blocks rejected as a scene camera.
- `SKYRECOMP_VR_EYE_SHOT=<frame>`: saves both eyes' presented images to `build/vr_eye0.png` and `build/vr_eye1.png`.
- `SKYRECOMP_VR_PASS_SHOT=<frame>`: saves every render pass's first target for both eyes (`build/pass_NNN[_eye1].png`).
- `SKYRECOMP_VR_NO_CAMERA=1`: both eyes keep the game's camera (isolates the camera patch).
- `SKYRECOMP_VR_NO_GAME_CAMERA=1`: the camera correction comes from each block's own view, not game memory (the old
  behaviour, for A/B checks). `SKYRECOMP_VR_CAM_TRACE=1` counts which source each draw used.
- `SKYRECOMP_VR_CAM_TRACE=1` also logs when the memory camera stops / starts matching the draws, the rig mode
  (1st / 3rd / theatre), and every 120 frames the game camera's distance from Link's eye.
- `SKYRECOMP_VR_EYE_SHOT=<frame>,<frame>,...` saves `build/vr_eye{0,1}_<frame>.png` for each listed frame;
  `SKYRECOMP_VR_EYE_SHOT_EVERY=<n>` saves eye 0 every n frames (`build/vr_live_<frame>.png`) during a live session.
- `SKYRECOMP_VR_FP_FOLLOW_CAMERA=1`: 1st person faces the game camera's yaw (playing on the desktop without the
  headset).
- Replays (`LARECOMP_INPUT_FILE`) drift after a few seconds of free movement (the game isn't frame-deterministic);
  for long routes use a live session with periodic shots instead.
- `SKYRECOMP_VR_PIN_HEAD=<degrees>`: the eyes stay at the reference head pose, turned left by that many degrees
  (`1`: straight ahead), so eye shots don't depend on the headset moving.
- Scripted runs on Windows: a dev build is a console program. Start it with no visible console (PowerShell
  `Start-Process -WindowStyle Hidden`), otherwise a click in its console window pauses the game until Enter.
- The freeze watchdog names the OpenXR call in progress (`vr_debug_stage`).
- `tools/test_stereo_camera_hook.cpp`: unit test (build command at its top).

## Known limits

- Objects appear at their draw distances (per object, e.g. 70 m or 140 m for small objects in Skyloft, times the
  Draw distance setting), measured along the game camera's forward axis rather than from the eyes.
- The scene camera block (slot 0) is patched for the vertex and the fragment stage: fragment shaders read it too
  (water reflections, fog, specular), and must use the same camera as the geometry. Slot 1 also carries a view
  rotation and is left mono; effects that read a camera from another block still see the mono camera. The game has
  no planar reflection pass in Skyloft (its only mirrored draws are orthographic effects).
- The game camera's address (0x7002bfda90) was found in Skyloft; cutscenes and other areas fall back as above if it
  doesn't match the draws there. Water surfaces at the 1st-person near plane are not checked yet.
- The game's TAA jitter in the projection is replaced by the headset frustum.
- A texture uploaded from the CPU and then rendered into during eye 1 starts its eye-1 image empty.
- If the game stops presenting (some static screens), no OpenXR frames are submitted while it waits; SteamVR may
  show its loading environment.
- GPU work doubles. Lower the internal resolution if the headset reprojects.
- 1st person: the view faces where the game camera looked on entry and doesn't turn with Link (no snap turns without
  VR controllers; turn your head, or use Recenter View). Link's rigid pieces (hands, hat, sword hilt) stay visible;
  only his skinned body is hidden.
