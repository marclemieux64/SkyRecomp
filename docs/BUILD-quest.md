# Meta Quest base APK

The `android/` project is the first Android/Quest porting layer. It produces an
ARM64 APK that is recognized as an immersive Quest application, enters through
Android's native lifecycle, initializes the Khronos Android OpenXR loader,
creates an OpenXR instance, requests an HMD system, lets OpenXR create the
required Vulkan instance/device, and creates an XR session. It is an integration
build, not yet a playable Skyward Sword build: it currently stays on the
headset loading screen and writes bootstrap/lifecycle messages to logcat.

This layout follows the working standalone approach used by Shipwright-VR:
an ARM64-only Android package, Quest manifest metadata, a native activity, and
the Khronos OpenXR loader packaged through Gradle Prefab.

## Requirements

- JDK 17 or newer
- Android SDK Platform 36 and Build Tools 35.0.0 or newer
- Android NDK `27.1.12297006`
- CMake 3.31.6
- Android Studio, or the included Gradle 8.11.1 wrapper

Set `ANDROID_HOME` to the SDK directory, then build from the repository root:

```powershell
cd android
.\gradlew.bat assembleDebug
```

The debug-signed APK is written below
`android/app/build/outputs/apk/debug/`. Install it with:

```powershell
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -s SkyRecompQuest
```

Tags matching `quest-smoketest-v*` run the Quest workflow, verify the release
APK, retain it as an Actions artifact, and publish it as a GitHub pre-release.
See [the smoke-test procedure](QUEST-SMOKETEST.md) before distributing one.

## Personal game-data build (work in progress)

`tools/quest/build-personal-apk.ps1` accepts the player's existing extracted
game directory (`exefs/` plus `romfs/`). It generates AArch64 code, builds the
private `libskygame.so`, and packages it only into a local `personalRelease`
APK. With `-Install`, it also installs the APK and copies the extracted data to
the app-specific Quest directory through ADB. Generated code and game data are
ignored by Git and must never be uploaded to a release.

This packaging path is not playable yet: the Android runtime still needs to
load `libskygame.so`, mount the copied data, and connect the game renderer to
the OpenXR frame loop.

Windows players can double-click `tools/quest/SkyRecomp Quest Packager.cmd` for
a graphical wrapper with folder pickers, build progress, optional installation,
and an output-folder shortcut. The PowerShell script remains the build backend
so the same operation can be automated and diagnosed from its log.

The finished APK is placed in
`android/app/build/outputs/apk/personal/release/`. It contains generated code
derived from that player's game executable, so it is a personal artifact and
must not be uploaded or shared.

## Engine work still required

The desktop executable cannot simply be copied into this APK. A playable port
needs these boundaries moved to Android:

1. Add `runtime/platform/android.c`, including guest address reservation,
   monotonic timing, fault handling, and app-private storage paths. Re-exec is
   not available to an Android Activity.
2. Build the generated AArch64 game code into an APK native library. Android
   cannot build or replace that library on the headset, so game extraction and
   code generation remain a host-side step.
3. Merge the Android OpenXR/Vulkan bootstrap supplied here into
   `runtime/vr/vr.c`, then create swapchains from the Activity lifecycle.
4. Adapt the Vulkan presenter to an OpenXR-only Android path (no desktop mirror
   window) and package shaderc for ARM64, because Maxwell shaders are translated
   at runtime.
5. Package ARM64 FFmpeg or replace the movie HLE with Android MediaCodec, and
   provide SDL3's Android Java/native integration for audio and input.
6. Replace the desktop launcher and file picker with Android storage access,
   then test memory use and frame pacing on Quest hardware.

No game files or generated Nintendo code belong in a distributable APK.
