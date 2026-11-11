# Changelog

## [Unreleased] - VR

Experimental SteamVR / OpenXR support (off by default; VR Options > Enable VR Mode), from @marclemieux64 and @AL56201. See [docs/vr.md](docs/vr.md).

### 1st person
- **Hands and held items:** Link's hands, sword and shield stay on their controllers through swings, shield raises and bashes; the hands are on the correct sides, and the blade shows in both eyes.
- **Sword hits from your blade:** a slash hits with the game's three sword attack colliders, now placed along the blade in your right hand, so what gets cut is where you swing (Z-targeting no longer steers it).
- **Aiming with the controller:** aimed items go where the right controller points.
- **Slingshot:** a new Slingshot system shoots from the controller along where it points, following the game's own seed flight, with a 3D marker where the seed will land; the Slingshot model is held in the hand.
- **Link's body hidden:** his small pieces (eyes, belt, pouches) and his hair stay hidden when his body leaves the eye point (rolls, swings, climbing, swimming, doors). Enemies that come close are no longer mistaken for Link and hidden.
- **Beetle:** it launches from the right controller along where it points; the Beetle and its bracelet sit on your wrist; while it flies, the view switches to the game's own camera following it, and returns to 1st person when it's caught.
- **Boss-key puzzle:** while you examine a boss door's lock and turn the key, the view switches to the game's own close-up of the key and the lock, and returns to 1st person when you back out or the door opens.

### Rendering
- **Culling follows the headset:** objects are culled against the eyes' view, and their draw distance is measured along where you look.

### Packaging
- Windows packages bundle the Khronos OpenXR loader with its Apache 2.0 license (`LICENSE-OpenXR.txt`).

## [2.2.0] - 2026-11-10

## New in 2.2

- Added recognition of prepared **Skyward Sword HD v1.0.1** game files, alongside v1.0.0.
- Added **early, experimental Archipelago mod support**, contributed by [@Wesley-Playz](https://github.com/Wesley-Playz) in [#90](https://github.com/Apolinario-coder/SkyRecomp/pull/90). This is an initial compatibility step, not a claim that every seed, connection or playthrough has been tested.
- Enabled mods to supply replacement game modules through `mods/<mod name>/exefs/`, including the additional `subsdk8` module used by the mod. The first enabled mod in the configured order wins when multiple mods replace the same module.
- Updated the game code loader and generator to use the module layout required by this integration, including code that enters a mod function partway through it and jump tables with out-of-line targets.
- Added version-specific timing locations for the existing experimental 120 FPS option. 120 FPS remains experimental.

## Integration fixes

- Preserved the current generator's optimized normal path while adding the mod's alternate-entry behavior.
- Kept verified v1.0.0 gameplay patches working at the new module addresses. Address-specific v1.0.0 cheats are not applied to v1.0.1, whose corresponding locations have not been verified.
- Changed the generated-library identity so libraries from older releases are rebuilt for the new layout. Locally generated libraries also track the selected main-module build ID, the generator and enabled module overrides.
- Extended host crash-symbol lookup to include the additional mod module, and bounded alternate entry to aligned instructions in that module's text section.
- Added generator regression checks for timing locations, alternate entry, relocated patches and external jump-table targets.

## Setup and updating

Back up your saves before updating and extract into a fresh folder. Supply your own prepared game files: `exefs/` containing `main`, `sdk`, `subsdk0`, `subsdk1`, plus `romfs.bin` or `romfs/`. For v1.0.1, the selected update must already be applied to those prepared files; the launcher does not decrypt or apply encrypted update packages.

When using the Archipelago mod, follow the mod author's setup instructions and put its prepared replacement modules under `mods/<mod name>/exefs/`. Enable the mod before preparing the game code and restart after changing executable mods. Setup may rebuild the game code; do not reuse an old `skygame.dll`, `.so` or `.dylib` from 2.1 or earlier.

This release follows the current automated package format: Windows, Linux and macOS packages include the application and preparation tools, not precompiled game code, game assets, the 4K texture pack or ReShade. Existing optional graphics features remain platform dependent. The Windows package is named **SkyRecomp-v2.2.0-Windows.zip**. The launcher uses **v2.2.0** for the requested **2.2** release.

## Thanks

Thank you to **Wesley-Playz** for the game-version and Archipelago contribution, and to all contributors and users reporting issues and testing fixes. Existing upstream and artwork credits remain applicable.

## Known limits and reporting problems

Support for v1.0.1 and Archipelago is experimental. A complete game playthrough and a live Archipelago multiworld session have not been verified for this release. Not all address-specific gameplay options have verified v1.0.1 equivalents; the v1.0.0 cheat patches are deliberately left out of that build.

Report problems through [GitHub Issues](https://github.com/Apolinario-coder/SkyRecomp/issues). Include v2.2.0, the game version, operating system, controller, mods and their order, and steps to reproduce. On Windows, attach `skyrecomp.log` from beside the executable. If the game freezes, wait at least 15 seconds before copying the log, and preserve it before relaunching. On Linux/macOS, also include terminal output or the available system crash report. Never attach game executables or console keys.


## [2.1.0] - 2026-10-31

This release brings major stability, performance, graphics, and quality-of-life improvements rolled up from @BlAnK182C-bak (Issue #92).

### Performance & Compilation
- **Optimized game code (-O1):** Game code is now compiled with `-O1` optimization across all platforms, offering substantial performance and frame pacing gains while keeping build times controlled via LLVM pass caps.
- **Pipeline Cache & Shader Compilation:** Added configurable shader compilation settings (hybrid, wait, or async) and persistent pipeline cache saving to minimize stutter.

### Graphics & Rendering
- **Bindless Shaders & Gather:** Translated bindless texture lookups and texture gathers across both Vulkan and Metal backends.
- **Enhanced Video Settings:** Added FXAA, supersampling with proper averaging, and anisotropic filtering settings.
- **Hardware Compatibility:** Vulkan now safely queries and activates only supported device features, fixing crashes on varied GPU architectures (#46, #59, #60). Guest heap is now allocated in 32 MB chunks.
- **Metal Improvements:** Fixed NVN alpha comparisons, depth test behavior on passes lacking depth buffers (fixing dark Fi rendering), OR logic op blending, and thumbnail saving.
- **Texture Fixes:** Corrected channel ordering for BGR5 formats and low-bit handling for 16-bit texture formats.

### Controls & Input
- **Stick Circle Clamping:** Kept both analog sticks constrained to the Nintendo Switch physical circle bounds (#16, #31).
- **Free Camera Right Stick:** Right stick click now passes through correctly while using free camera (#86).
- **Mouse Combat:** Sword swings can now be driven directly with mouse motion while holding left click.
- **Prompt Icons:** Added crisp HUD and in-dialogue keyboard/controller prompt icons that automatically reflect the active input device.

### Audio
- Fixed aux reverb, high-pitched notes, missing PCM format decoders, and audio pop/click artifacts. Turning room reverb off now properly disables aux reverb.

### Gameplay & Cheats
- **Virtual Amiibo:** Added virtual Zelda & Loftwing amiibo support from the player's own dump in the Gameplay tab.
- **Flight & Speed Tweaks:** Added Loftwing dash gauge, customizable flight and walking speed multipliers, and drop toggles.
- **Cheats Tab:** Added options for Hero Mode from the start, custom damage taken multipliers, and color blind mode.
- **Performance Overlay:** Built-in real-time overlay showing CPU, GPU, RAM, thermals, and frametime graphs.

### Setup & Packaging
- Setup now avoids folder dialog lockups and never attempts to copy game files over themselves (#81, #87).
- Automated multi-platform release packaging for Windows, Linux AppImage, and macOS 14/15.

## [2.0.2] - 2026-10-25

- **Language fixes and regional support:** Added automatic regional fallback so game data with either European (`en-GB`, `es-ES`, `fr-FR`) or North American (`en-US`, `es-419`, `fr-CA`) files correctly displays in English or the chosen language, preventing unwanted fallbacks.
- **Fixed game code linking error during setup:** Added `-fno-sanitize=all` to prevent undefined compiler runtime references during initial setup build.
- **Fixed introductory video hang:** Prevented controller skip action on non-skippable introductory cutscenes to ensure audio and video remain in sync.

## [2.0.0] - 2026-10-25

This update focuses on stability, eliminating unexpected crashes and freezes during gameplay (especially at 60 and 120 FPS).

### Crash and freeze fixes
- **Fixed random gameplay freezes:** Solved the lost condition variable wakeup race condition where the game screen hangs while background music continues playing. Special thanks and credit to @Taktloss (PR #58) and @ColdincD (PR #55) for identifying the root cause and providing fixes.
- **Fixed random gameplay crashes:** The game no longer crashes unexpectedly during gameplay or in scenes with heavy draw activity at 60 and 120 FPS.
- **Better GPU compatibility:** Resolved video memory issues that caused crashes on graphics cards with limited memory or without hardware features like Resizable BAR.
- **Texture stability:** Safer loading for custom texture packs, preventing freezes when loading new areas.
- **Setup fix:** Fixed a crash that could occur during the initial ROM setup on Windows.

### Windows v1.0.6 setup hotfix - 2026-10-22

- Fixed the packaged game-code generator terminating before writing a log during game setup. The Windows compatibility adjustment now runs automatically whenever the helper is packaged.
- Failed generator reports now include the exit code and diagnostic log path, including when the helper produces no output.

## [1.0.6] - 2026-10-21

**Sharper visuals. Freer camera control. A new way to play with keyboard and mouse.** This release brings three major upgrades: Henriko Magnifico's 4K texture pack, an optional free camera, and mouse camera/combat controls.

## The highlight: 4K Textures by Henriko Magnifico

The **Windows visual package includes 568 replacement textures by [Henriko Magnifico](https://www.henrikomagnifico.com/zelda-skyward-sword-4k)**, his ReShade presets, and the effects they need. All credit for the texture artwork and supplied presets belongs to Henriko Magnifico — thank you for making this visual upgrade possible! Please visit his page to see and support his work.

The pack improves matching textures throughout the game, rather than only the opening room. It does **not** replace every original texture; anything absent from the pack keeps its original appearance.

- Extract the entire Windows ZIP and open **skyrecomp.exe**. No separate ReShade download or installation is needed.
- Your own prepared **Skyward Sword HD v1.0.0** game files are still required. Game data and console keys are not included.
- Open **ESC > Video** to enable or disable **4K Textures** and **ReShade**. Texture changes and already-loaded effects apply while playing and are saved.
- **Ultra without motion blur** is the default ReShade preset. Unwanted motion-blur effects were removed from the supplied no-motion-blur presets.
- If the application started with ReShade disabled, enable it and close/reopen the application to load it.
- 4K replacements currently use Vulkan. The bundled ReShade setup is Windows-only; the macOS package does not include the 4K/ReShade visual pack.

## Highlight: Free Camera

Camera control has been a recurring point of frustration for players. The new optional **Free Camera** lets you look around with the right stick without holding L, making exploration more comfortable. Thank you to **[@kyrill-bo](https://github.com/kyrill-bo)** for the contribution in [#45](https://github.com/Apolinario-coder/SkyRecomp/pull/45).

- Added an optional **free camera**, adjustable camera sensitivity, a camera-mode indicator and a smaller FPS display.
- Tap **L** on a controller to switch camera mode; hold it temporarily for sword-stick controls. Existing aiming and item controls remain available.
## Highlight: Keyboard & Mouse Controls

Enable **Free camera look** and **Mouse camera and combat** in **ESC > Controls**, and use the game's button-only control mode. Move the mouse to look around, explore with WASD, and use mouse buttons for combat and items:

- **Left click:** sword slash. Move the mouse before clicking to choose a direction.
- **Right click:** hold target lock; pressing also sends a shield bash when a shield is equipped.
- **Middle click:** sword thrust.
- **Hold Left Alt or Mouse 4:** aim the selected item with the mouse. With the bow ready, hold left click to draw and release to fire.
- Adjust **Mouse sensitivity** in the menu. Tap **Q** to switch free camera mode; hold it temporarily for the original sword-stick behavior.
- Mouse controls release the cursor in the ESC menu and clear pending actions when focus is lost. Explicit mouse bindings take priority over shortcuts.
- See the [complete keyboard/mouse controls](https://github.com/Apolinario-coder/SkyRecomp/blob/v1.0.6/docs/mouse-controls.md), including item selection and bow aiming. Use the game's button-only control mode.

## Fixes and improvements

- ReShade now obeys its saved Off choice, switches loaded effects immediately, and keeps the choice when returning to the launcher or recreating the window.
- Texture replacement can be toggled during play, including textures already displayed. Replacements load as needed, with original textures retained when a replacement cannot be loaded.
- Reduced Windows memory use during startup and texture loading; corrected buffer handling and fallback behavior when memory is limited.
- Fixed a Windows memory allocation race and error handling for failed game thread creation. These address identified failure cases; other crashes may still occur.
- Made input sampling consistent across repeated reads and independent of stalled rendering.
- Fixed Windows configuration files causing language and other text settings to fall back to defaults.
- Improved automatic system-language detection, including **Simplified Chinese (`zh-Hans` / `zh-CN`)** and Traditional Chinese variants.
- Changing language in the ESC menu now shows when a restart is needed and offers **Apply language and restart...**. Save first: unsaved progress will be lost when restarting. The port's settings interface remains in English.
- Fixed identified macOS screen-clearing crashes and shader transparency handling, and improved game setup on Apple Silicon.
- Interrupted game setup can resume completed work. Reused work is checked against the current compiler, sources and headers.
- Added Linux AppImage packaging with the required video libraries, alongside the existing Linux archives.
- Corrected font packaging and included missing third-party license notices.
- Updated the launcher version to **1.0.6**.

## Crash and freeze reports: please include your log

Windows now opens a separate **SkyRecomp diagnostics** console and writes **skyrecomp.log next to skyrecomp.exe**. Closing that console does not close the game. It remains available after a crash.

If the game freezes:

1. Wait **at least 15 seconds** so the automatic diagnostic report can be written. Long loading can also produce a possible-freeze report; tell us what was happening.
2. Copy **skyrecomp.log before launching again** — the next normal launch replaces it.
3. [Open an issue](https://github.com/Apolinario-coder/SkyRecomp/issues/new) and attach the log. Include **v1.0.6**, your operating system, CPU/GPU, graphics driver, location, action, controller, language and frame-rate setting, plus whether **4K Textures** and **ReShade** were enabled.
4. Add a screenshot and a save near the problem if available. Check logs for personal paths before sharing. Linux/macOS users can attach captured terminal output and system crash reports.

Logs are not uploaded automatically. [More diagnostic details](https://github.com/Apolinario-coder/SkyRecomp/blob/v1.0.6/docs/diagnostics.md).

## A cleaner main branch

- Removed compiled programs, DLLs, obsolete extraction tools and local data placeholders from the current source tree.
- Grouped fonts under **fonts/** and build helpers under **tools/**, and removed redundant resource/header copies.
- Retired the old image-specific extractor containing a fixed key and clarified the prepared-game-file setup instructions.
- Added source checks to help keep generated binaries, game files and embedded keys out of future changes.
- Preserved local game files and saves. Historical commits and older releases are unchanged.

## Thank you to the contributors

- **[@kyrill-bo](https://github.com/kyrill-bo)** — free camera, sensitivity, toggle/hold controls, mode display and graphics contributions in [#45](https://github.com/Apolinario-coder/SkyRecomp/pull/45).
- **[@LiamPalmqvist](https://github.com/LiamPalmqvist)** — macOS graphics fixes, Apple Silicon setup and resumable game preparation in [#43](https://github.com/Apolinario-coder/SkyRecomp/pull/43), [#41](https://github.com/Apolinario-coder/SkyRecomp/pull/41) and [#42](https://github.com/Apolinario-coder/SkyRecomp/pull/42).
- **[@BlAnK182C-bak](https://github.com/BlAnK182C-bak)** — Linux AppImage packaging in [#29](https://github.com/Apolinario-coder/SkyRecomp/pull/29).
- **Henriko Magnifico** — the texture artwork and supplied ReShade presets highlighted above.
- **[peiikai and LASRecomp contributors](https://github.com/peiikai/LASRecomp)** — the foundation of this port; **Patrick Mours and ReShade contributors** — ReShade.

Thank you also to everyone reporting bugs, sending logs, testing fixes and submitting improvements. Reviewed PR contributions have been adapted into this release; this wording does not imply that every original PR has been merged unchanged.

## Status and updating

This is still an **experimental project**. The reported inventory/map freezes and visual differences involving Mia and the Loftwing-area trees have not yet been confirmed fixed. Full-game stability has not been established. **120 FPS remains experimental**, carried over from v1.0.5; try 60 FPS when investigating a problem.

Back up **save/** before updating. Extract into a fresh folder, then copy your saves and prepared game files from your previous installation. Keep logs from a failed run before reopening the game. Do not copy an older executable or DLLs over the new release. Native macOS and Linux gameplay has not been verified on the Windows build machine.

## [1.0.5] - 2026-10-20

### Added

- **120 FPS (Experimental)**: Enjoy smoother gameplay with the new option in **Video > Frame rate**. Game speed and audio timing adjust independently of the higher frame rate. 60 FPS remains the default; performance depends on your hardware and settings.

- Included the tools needed to prepare game files on a new installation.

### Fixed

- Fixed Link's shadow disappearing and reappearing when moving through certain spots.
- Fixed objects appearing to slide when rotating the camera with **Aspect Unlocked** on ultrawide displays.
- Prevented small controller stick drift from sending unwanted movement or sword commands.
- Fixed the causes identified in crash reports involving the door-repairing NPC after rescuing the pet and entering the cave.
- Improved game setup so older prepared game files are rebuilt when needed for the latest fixes.
- Error reports now display the correct **SkyRecomp** application name.

### Notes

- The 120 FPS option is experimental. A turning animation followed a different path during testing; combat, flight and all cutscenes have not yet been fully checked. Switch back to 60 FPS if you encounter problems.
- The reported NPC and cave crashes could not be replayed with the original player's save. Other random crashes and objects disappearing outside the usual screen width may still occur.
- Includes the previous fixes for oversized eyes, newly entered names appearing as squares, slow item sounds and the short distorted sound in the left audio channel.

## [1.0.4] - 2026-10-19

### Fixed

- **Character eye rendering**: Eyes no longer appear exaggeratedly large. Dual-source fragment outputs are now mapped to location 0 with indices 0 and 1 when attachment 0 uses SRC1 blend factors (Vulkan). NVN alpha comparisons and alpha reference are implemented through fragment-shader discard, with the reference dynamic in `rscale[65].y`. NVN `SetDepthClamp` is now recorded and applied to the Vulkan pipeline key, rasterization state, and Metal depth clip mode.

- **Typed names in dialogs**: New names no longer appear as squares. The keyboard HLE (`swkbd`) now observes `KeyboardConfig::IsUseUtf8` (offset `0x3bd`) to select UTF-8 or UTF-16LE output encoding. Initial text preserves Unicode, output is bounds-checked, and surrogate pairs are handled.

- **Sound effects and short music**: Playback is no longer slow, including the sound when receiving a rupee. Audio voices honor `SkipPitchAndSampleRateConversion` (bypass copies samples directly instead of stretching them). The renderer clock is independent of output-device queue consumption, using a periodic deadline with prequeued silence. Windows guest waits now use high-resolution waitable timers instead of `nanosleep`/`pthread_cond_timedwait` (which rounded 3 ms waits to ~15.6 ms). Splitter destination updates skip variable-length `SNDI` state records before parsing `SNDD` destinations, fixing clipped left-only audio. Diagnostic WAV recordings use the renderer's actual sample rate instead of a hardcoded 48 kHz.
