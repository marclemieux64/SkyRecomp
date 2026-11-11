#!/usr/bin/env bash
# Builds the Linux personal APK builder: build/quest-packager-release/Skyrecomp-VR-APK-Builder-x86_64.AppImage
# (build-packager.ps1, for Linux). Needs the .NET 8 SDK, git, python3 and curl. No game data goes in: the
# payload is the worktree files Git tracks or would track.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
output="${1:-$repo/build/quest-packager-release}"
work="$repo/build/quest-appimage"
appdir="$work/AppDir"
payload="$work/payload.zip"
appimage="$output/Skyrecomp-VR-APK-Builder-x86_64.AppImage"
appimagetool_url='https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage'

mkdir -p "$output" "$work"
rm -rf "$appdir" "$payload"

# ---- the embedded sources: only the files tools/quest/payload-files.txt lists, and of those only what Git
# tracks or would track, so ignored game data can never be included (shell scripts keep Unix line ends
# whatever the checkout used)
git -c safe.directory='*' -C "$repo" ls-files --cached --others --exclude-standard -z > "$work/files.list"
python3 - "$repo" "$work/files.list" "$payload" <<'PY'
import os, sys, zipfile
repo, listing, out = sys.argv[1:4]
wanted = [l.strip() for l in open(os.path.join(repo, 'tools/quest/payload-files.txt'), encoding='utf-8')]
wanted = [l for l in wanted if l and not l.startswith('#')]
names = [n for n in open(listing, 'rb').read().decode().split('\0') if n]
done = set()
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for name in names:
        if not any(name.startswith(w) if w.endswith('/') else name == w for w in wanted):
            continue
        path = os.path.join(repo, name)
        if not os.path.isfile(path):
            continue
        data = open(path, 'rb').read()
        if name.endswith('.sh') or name == 'android/gradlew':
            data = data.replace(b'\r\n', b'\n')
        z.writestr(name, data)
        done.add(name)
missing = [w for w in wanted if not w.endswith('/') and w not in done]
if missing:
    sys.exit('Files listed in payload-files.txt are missing: ' + ', '.join(missing))
print(f'Payload: {len(done)} source files')
PY

# ---- the application
dotnet publish "$repo/tools/quest/gui-linux/SkyRecompQuestBuilder.Linux.csproj" \
    -c Release -r linux-x64 --self-contained true \
    -p:DebugType=None -p:DebugSymbols=false "-p:PayloadPath=$payload" -o "$appdir/usr/bin"

# ---- the X session libraries the window toolkit loads at startup, which minimal systems lack. From this
# system, else unpacked from the distribution's packages (no installation needed).
libs="$work/xlibs"
for lib in libICE.so.6 libSM.so.6; do
    found="$(ldconfig -p | awk -v name="$lib" '$1 == name && /x86-64/ { print $NF; exit }')"
    if [ -z "$found" ]; then
        if [ ! -d "$libs/root" ]; then
            mkdir -p "$libs"
            (cd "$libs" && apt-get download libice6 libsm6 >/dev/null && for deb in *.deb; do dpkg -x "$deb" root; done)
        fi
        found="$(find "$libs/root" -name "$lib*" -type f | head -n 1)"
    fi
    [ -n "$found" ] || { echo "Missing $lib: install the libice6 and libsm6 packages." >&2; exit 1; }
    cp -L "$found" "$appdir/usr/bin/$lib"
done

cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
here="$(dirname "$(readlink -f "$0")")"
exec "$here/usr/bin/skyrecomp-vr-apk-builder" "$@"
EOF
chmod +x "$appdir/AppRun" "$appdir/usr/bin/skyrecomp-vr-apk-builder"
cat > "$appdir/skyrecomp-vr-apk-builder.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Skyrecomp-VR APK Builder
Comment=Builds your personal Sky Recomp APK for Meta Quest from your own game files
Exec=skyrecomp-vr-apk-builder
Icon=skyrecomp-vr-apk-builder
Categories=Game;Utility;
Terminal=false
EOF
python3 - "$repo/docs/icon/icon_1024.png" "$appdir/skyrecomp-vr-apk-builder.png" <<'PY'
import shutil, sys
shutil.copyfile(sys.argv[1], sys.argv[2])  # (AppImage accepts the icon at any size)
PY
ln -sf skyrecomp-vr-apk-builder.png "$appdir/.DirIcon"

# ---- the AppImage
tool="$work/appimagetool-x86_64.AppImage"
if [ ! -x "$tool" ]; then
    curl --location --fail --silent --show-error --output "$tool" "$appimagetool_url"
    chmod +x "$tool"
fi
rm -f "$appimage"
# (extract-and-run: works without FUSE, e.g. in containers and CI)
ARCH=x86_64 "$tool" --appimage-extract-and-run --no-appstream "$appdir" "$appimage"
rm -f "$payload"
echo "$appimage"
