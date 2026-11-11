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

The `personal` APK carries the game runtime itself: `android/engine` builds
`runtime/` as `libmain.so` (SDL3's Android activity, the Vulkan renderer, the
OpenXR code of the PC VR build) and `libskygame.so` is linked against it. The
`publicRuntime` flavor is still the smoke test. State of the personal build:

- It compiles, links and packages; it has **not been run on a headset yet**.
- It needs Vulkan 1.3 and Android 14 (`minSdk 34`): Quest 3 / 3S.
- The game's fixed address range (`0x7000000000`-`0x7320000000`) must be free
  in the app process; if it is not, logcat (`adb logcat -s SkyRecomp`) lists
  what is in the way.
- The two pre-rendered movies (prologue, ending) are not decoded: FFmpeg is not
  part of the Android build (`runtime/platform/android_stubs.c`).
- The first native build downloads SDL3, shaderc and VMA sources.

Windows players download and run the single `Skyrecomp-VR APK Builder.exe`.
The application embeds and privately extracts its build payload automatically.
Its GUI only asks for the extracted game folder; on the first build it offers
to download Microsoft OpenJDK 21 and the official Android command-line tools,
then installs the pinned Platform, Build Tools, NDK, CMake, and ADB packages.
The player does not need to configure Java or Android SDK paths; Python 3
must be installed, because the code generator runs on it. The
**Remove installed tools** button deletes only that private toolchain; it does
not touch extracted game data or built APKs.
The PowerShell script remains the internal build
backend so the same operation can be automated and diagnosed from its log.

Linux players (x86-64) download `Skyrecomp-VR-APK-Builder-x86_64.AppImage`,
make it executable and run it. It is the same builder with the same steps
(`tools/quest/gui-linux`, scripts in `tools/quest/linux/`); it keeps its
sources, private tools and the built APK in
`~/.local/share/skyrecomp-vr-apk-builder/`. It needs `bash`, `curl`, `tar` and
`python3`, and for the USB install the usual Android udev rules.
`tools/quest/build-appimage.sh` builds it (needs the .NET 8 SDK).

Maintainers can create the standalone executable with
`tools/quest/build-packager.ps1`. Tags matching `quest-packager-v*` compile and
self-test the real Windows executable before attaching the standalone EXE to a GitHub
pre-release.

The finished APK is placed in
`android/app/build/outputs/apk/personal/release/` (under
`%LOCALAPPDATA%\Skyrecomp-VR APK Builder\payload\<version>\` when built
with the GUI; **Open output folder** goes there). It contains generated code
derived from that player's game executable, so it is a personal artifact and
must not be uploaded or shared.

## Engine work still required

1. Run the personal build on a headset and fix what the first boots show
   (address-space reservation, Adreno Vulkan features and texture formats,
   the Activity's pause / resume lifecycle).
2. Present only through OpenXR: the Android window surface is still created
   as on desktop, with the mirror off by default.
3. Package ARM64 FFmpeg or replace the movie HLE with Android MediaCodec.
4. Measure memory use and frame pacing on Quest hardware.

No game files or generated Nintendo code belong in a distributable APK.
