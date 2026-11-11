# Quest smoke-test v0.1.0-pre.3

This is a hardware-unverified developer pre-release. It does **not** contain or
run Skyward Sword. Its purpose is to validate SkyRecomp's new Android/Quest
packaging and the native OpenXR/Vulkan path before the game runtime is ported.

Expected result on Quest 2, Quest Pro, Quest 3, or Quest 3S:

1. The APK installs and appears under **Unknown Sources** as
   **Sky Recomp Quest Smoke Test**.
2. Launching it enters immersive mode.
3. OpenXR creates a stereo Vulkan session.
4. The left eye shows dark blue and the right eye shows dark purple. The image
   remains stable as the headset moves because poses are submitted every frame.

Install with SideQuest or:

```text
adb install -r SkyRecomp-Quest-SmokeTest-0.1.0-smoketest.3.apk
```

Capture diagnostics while launching:

```text
adb logcat -c
adb logcat -s SkyRecompQuest OpenXR-Loader
```

Please report the headset model and Horizon OS version, whether immersive mode
started, what each eye displayed, and the complete filtered log. Comfort and
performance have not been evaluated. Exit immediately if the display flickers,
is disorienting, or does not respond normally to the system button.
