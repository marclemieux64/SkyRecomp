#!/usr/bin/env bash
# Builds the personal Quest APK from the player's extracted game files on Linux, and installs it on a
# connected headset (build-personal-apk.ps1, for Linux). Lines starting with "== " are step headings and
# "HEADSET: " lines say what happened with the headset; the builder window shows both.
#
#   build-personal-apk.sh --game <dir> --sdk <android sdk> --java <jdk> [--install | --install-only]
set -euo pipefail
export PYTHONUNBUFFERED=1  # progress lines arrive as they are printed, not in bursts

game_root='' android_sdk="${ANDROID_HOME:-}" java_home="${JAVA_HOME:-}" install=0 install_only=0
while [ $# -gt 0 ]; do
    case "$1" in
        --game) game_root="$2"; shift 2 ;;
        --sdk) android_sdk="$2"; shift 2 ;;
        --java) java_home="$2"; shift 2 ;;
        --install) install=1; shift ;;
        --install-only) install=1; install_only=1; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
fail() { echo "$*" >&2; exit 1; }
total_steps=5
[ "$install" -eq 1 ] && total_steps=6
step() { echo "== Step $1/$total_steps: $2"; }

step 1 'Checking the game files and build tools'
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
[ -n "$game_root" ] && [ -d "$game_root" ] || fail 'Select the extracted game folder first.'
game="$(cd "$game_root" && pwd)"
exefs="$game/exefs"
romfs="$game/romfs"
for name in main sdk subsdk0 subsdk1 main.npdm; do
    [ -f "$exefs/$name" ] || fail "Missing extracted game file: exefs/$name"
done
[ -d "$romfs" ] || fail 'Missing extracted game directory: romfs'
[ -n "$android_sdk" ] || fail 'Set ANDROID_HOME or pass --sdk.'
[ -n "$java_home" ] || fail 'Set JAVA_HOME or pass --java.'

ndk_version='27.1.12297006'
cmake_version='3.31.6'
ndk="$android_sdk/ndk/$ndk_version"
cmake="$android_sdk/cmake/$cmake_version/bin/cmake"
ninja="$android_sdk/cmake/$cmake_version/bin/ninja"
adb="$android_sdk/platform-tools/adb"
for tool in "$cmake" "$ninja"; do
    [ -x "$tool" ] || fail "Android build tool not found: $tool"
done

if [ "$install_only" -eq 0 ]; then
    generated="$repo/build/quest-personal/generated"
    native_build="$repo/build/quest-personal/native"
    private_jni="$repo/android/app/src/personal/jniLibs/arm64-v8a"
    mkdir -p "$generated" "$native_build" "$private_jni"

    command -v python3 >/dev/null || fail 'Python 3 was not found. Install the python3 package and try again.'
    python_env="$repo/build/quest-personal/python-env"
    python="$python_env/bin/python"
    if [ ! -x "$python" ]; then
        echo 'Preparing the private code-generation environment...'
        if ! python3 -m venv "$python_env" >/dev/null 2>&1; then
            # (Debian / Ubuntu without python3-venv: a bare environment, then pip from its own installer)
            rm -rf "$python_env"
            python3 -m venv --without-pip "$python_env" || fail 'Could not create the private Python environment.'
            curl --location --fail --silent --show-error https://bootstrap.pypa.io/get-pip.py | "$python" - --quiet ||
                fail 'Could not install pip in the private Python environment.'
        fi
    fi
    if ! "$python" -c 'import unicorn, capstone, lz4.block' >/dev/null 2>&1; then
        echo 'Installing the pinned code-generation dependencies...'
        "$python" -m pip install --disable-pip-version-check 'unicorn==2.1.4' 'capstone==5.0.9' 'lz4==4.4.5' ||
            fail 'Could not install the private Python code-generation environment.'
    fi

    step 2 'Translating the game code to C (4 parts, several minutes)'
    while read -r module base; do
        echo "Analysing $module... (the first progress line can take a minute)"
        args=("$repo/tools/gen.py" "$module" "$exefs/$module" "$base" "$generated/$module" --simd asm)
        [ "$module" = sdk ] && args+=(--hle "$repo/runtime/hle/hle_symbols.txt")
        "$python" "${args[@]}" || fail "Game-code generation failed for $module."
    done <<'MODULES'
main 0x7100004000
sdk 0x712E74B000
subsdk0 0x712D037000
subsdk1 0x712D9FF000
MODULES

    step 3 'Compiling the game code for the headset (about 2000 files, the longest step)'
    "$cmake" -S "$repo/android/game" -B "$native_build" -G Ninja \
        "-DCMAKE_MAKE_PROGRAM=$ninja" \
        "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
        "-DSKYRECOMP_ROOT=$repo" "-DSKYRECOMP_GENERATED_DIR=$generated" ||
        fail 'Android game-library configuration failed.'
    "$cmake" --build "$native_build" --target skygame --parallel || fail 'Android game-library build failed.'
    cp -f "$native_build/libskygame.so" "$private_jni/"

    step 4 'Building the game engine (the first build downloads its libraries and takes a few minutes)'
    export JAVA_HOME="$java_home" ANDROID_HOME="$android_sdk" ANDROID_SDK_ROOT="$android_sdk"
    # (sh: the payload is unpacked from a zip, which does not keep the wrapper's executable bit)
    sh "$repo/android/gradlew" -p "$repo/android" --console=plain assemblePersonalRelease ||
        fail 'Personal Quest APK build failed.'
fi

apk="$(ls -t "$repo"/android/app/build/outputs/apk/personal/release/*.apk 2>/dev/null | head -n 1 || true)"
[ -n "$apk" ] || fail 'No personal APK has been built yet. Build it first.'
step 5 'The APK is ready'
echo "Personal APK: $apk"

if [ "$install" -eq 1 ]; then
    package='io.github.skyrecomp.quest'
    remote="/sdcard/Android/data/$package/files/extracted"
    [ -x "$adb" ] || fail "adb not found: $adb"
    devices="$("$adb" devices | tail -n +2 | grep -v '^[[:space:]]*$' || true)"
    ready="$(printf '%s\n' "$devices" | grep -c "$(printf '\t')device\$" || true)"
    if [ "$ready" -eq 0 ]; then
        if printf '%s' "$devices" | grep -q unauthorized; then
            echo 'HEADSET: put the headset on and accept "Allow USB debugging", then click Install on headset.'
        elif printf '%s' "$devices" | grep -q 'no permissions'; then
            echo 'HEADSET: Linux blocks access to the USB device. Install the "android-udev-rules" package (or add yourself to the plugdev group), replug the headset, then click Install on headset.'
        else
            echo 'HEADSET: none connected. Plug the Quest in by USB (developer mode on), then click Install on headset.'
        fi
        exit 0
    fi
    [ "$ready" -eq 1 ] || fail 'More than one Android device is connected. Unplug the others and try again.'

    step 6 'Installing on the headset'
    echo 'Installing the app...'
    "$adb" install -r "$apk" || fail 'APK installation failed.'

    # The game data (about 7 GB) only needs copying once: a marker on the headset records what was copied.
    fingerprint="$(find "$romfs" "$exefs" -type f -printf '%s\n' | awk '{n++; s+=$1} END {printf "%d files, %.0f bytes", n, s}')"
    present="$("$adb" shell "cat '$remote/.complete' 2>/dev/null" | tr -d '\r' || true)"
    if [ "$present" = "$fingerprint" ]; then
        echo 'The game files are already on the headset.'
    else
        echo 'Copying the game files to the headset (about 7 GB, this takes several minutes)...'
        "$adb" shell "mkdir -p '$remote' && rm -f '$remote/.complete'"
        "$adb" push "$romfs" "$remote/" || fail 'Copying the game files failed. Check the headset has enough free space.'
        "$adb" push "$exefs" "$remote/" || fail 'Copying the game files failed. Check the headset has enough free space.'
        "$adb" shell "echo '$fingerprint' > '$remote/.complete'"
    fi
    echo 'HEADSET: installed. Start "SkyRecomp Quest Personal Build" from Unknown Sources in the headset library.'
fi
