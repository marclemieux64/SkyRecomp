#!/usr/bin/env bash
# Installs the private Java and Android build tools for the Linux APK builder (the PowerShell
# install-toolchain.ps1, for Linux). Needs only bash, curl, tar and python3.
set -euo pipefail

destination="${1:-${XDG_DATA_HOME:-$HOME/.local/share}/skyrecomp-vr-apk-builder/toolchain}"
jdk_root="$destination/jdk"
sdk_root="$destination/android-sdk"
downloads="$destination/downloads"
android_tools_url='https://dl.google.com/android/repository/commandlinetools-linux-15859902_latest.zip'
android_tools_sha256='4e4c464f145a7512b57d088ac6c278c03c9eea610886b35a5e0804e74eedf583'
jdk_url='https://aka.ms/download-jdk/microsoft-jdk-21-linux-x64.tar.gz'

if [ "$(uname -m)" != x86_64 ]; then
    echo 'The Android build tools only exist for 64-bit Intel/AMD Linux.' >&2
    exit 1
fi
for tool in curl tar python3; do
    command -v "$tool" >/dev/null || { echo "Missing required program: $tool" >&2; exit 1; }
done

download() {  # url, file
    curl --location --fail --retry 3 --progress-bar --output "$2" "$1" || { echo "Download failed: $1" >&2; exit 1; }
}

mkdir -p "$jdk_root" "$sdk_root" "$downloads"

java_bin="$(find "$jdk_root" -type f -path '*/bin/java' 2>/dev/null | head -n 1)"
if [ -z "$java_bin" ]; then
    echo '== Tools 1/3: Downloading Java (about 200 MB)'
    download "$jdk_url" "$downloads/jdk.tar.gz"
    tar -xzf "$downloads/jdk.tar.gz" -C "$jdk_root"
    java_bin="$(find "$jdk_root" -type f -path '*/bin/java' | head -n 1)"
    [ -n "$java_bin" ] || { echo 'The JDK archive did not contain bin/java.' >&2; exit 1; }
fi
java_home="$(dirname "$(dirname "$java_bin")")"
export JAVA_HOME="$java_home"

sdkmanager="$sdk_root/cmdline-tools/latest/bin/sdkmanager"
if [ ! -x "$sdkmanager" ]; then
    echo '== Tools 2/3: Downloading the Android command-line tools'
    download "$android_tools_url" "$downloads/android-command-line-tools.zip"
    actual="$(sha256sum "$downloads/android-command-line-tools.zip" | cut -d' ' -f1)"
    [ "$actual" = "$android_tools_sha256" ] || { echo "Android tools checksum mismatch: $actual" >&2; exit 1; }
    unpack="$destination/android-tools-unpack"
    rm -rf "$unpack"
    mkdir -p "$unpack" "$sdk_root/cmdline-tools/latest"
    python3 -m zipfile -e "$downloads/android-command-line-tools.zip" "$unpack"  # (unzip is not always installed)
    cp -a "$unpack/cmdline-tools/." "$sdk_root/cmdline-tools/latest/"
    chmod +x "$sdk_root/cmdline-tools/latest/bin/"*
    rm -rf "$unpack"
    [ -x "$sdkmanager" ] || { echo 'Android sdkmanager was not installed correctly.' >&2; exit 1; }
fi

echo 'Accepting Android SDK licenses...'
set +o pipefail  # (yes ends with SIGPIPE once sdkmanager has read enough)
yes | "$sdkmanager" "--sdk_root=$sdk_root" --licenses >/dev/null
licenses=${PIPESTATUS[1]}
set -o pipefail
[ "$licenses" -eq 0 ] || { echo 'Android SDK license acceptance failed.' >&2; exit 1; }

echo '== Tools 3/3: Downloading the Android compiler and SDK (about 2 GB, several minutes)'
"$sdkmanager" "--sdk_root=$sdk_root" \
    'platforms;android-36' 'build-tools;35.0.0' 'platform-tools' \
    'ndk;27.1.12297006' 'cmake;3.31.6' || { echo 'Android SDK package installation failed.' >&2; exit 1; }

cat > "$destination/toolchain.json" <<EOF
{
    "androidSdk": "$sdk_root",
    "javaHome": "$java_home"
}
EOF
echo "Android SDK: $sdk_root"
echo "Java home: $java_home"
