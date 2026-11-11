#!/usr/bin/env bash
# Removes the Linux APK builder's private Java and Android tools (and nothing else).
set -euo pipefail

expected="$(realpath -m "${XDG_DATA_HOME:-$HOME/.local/share}/skyrecomp-vr-apk-builder/toolchain")"
target="$(realpath -m "${1:-$expected}")"

# Never allow this cleanup helper to target a system-wide SDK or JDK.
if [ "$target" != "$expected" ]; then
    echo "Refusing to remove a non-private toolchain path: $target" >&2
    exit 1
fi
if [ ! -d "$target" ]; then
    echo 'The private toolchain is already absent.'
    exit 0
fi

# A Gradle daemon or adb server started from these tools would keep running from deleted files.
for exe in /proc/[0-9]*/exe; do
    path="$(readlink "$exe" 2>/dev/null || true)"
    case "$path" in
        "$target"/*)
            pid="${exe#/proc/}"; pid="${pid%/exe}"
            echo "Stopping private tool process: $(basename "$path") ($pid)"
            kill -9 "$pid" 2>/dev/null || true
            ;;
    esac
done
rm -rf "$target"
echo "Removed private toolchain: $target"
