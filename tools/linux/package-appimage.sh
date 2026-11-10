#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
build=${1:-$root/build-linux}
: "${QT_ROOT_DIR:?Set QT_ROOT_DIR to the Qt installation used to build Nemyu}"
[ "$(uname -sm)" = 'Linux x86_64' ] || { echo 'Linux x86_64 is required.' >&2; exit 1; }
for binary in nemyu nemyu-cli; do
    test -x "$build/bin/$binary"
    file "$build/bin/$binary" | grep -q 'ELF 64-bit.*x86-64'
done
test -x "$QT_ROOT_DIR/bin/qmake"
short_sha=$(git -C "$root" rev-parse --short=10 HEAD)
version=${CI_COMMIT_TAG:-$short_sha}
printf '%s\n' "$version" | grep -Eq '^[A-Za-z0-9]+([._-][A-Za-z0-9]+)*$'
case "$version" in HEAD|*.lock) echo 'Invalid release tag.' >&2; exit 1 ;; esac
out="$root/out/linux-x86_64"
tools="$root/.cache/appimage-tools"
mkdir -p "$out" "$tools"

download() {
    url=$1 destination=$2 checksum=$3
    if [ ! -f "$destination" ] || ! printf '%s  %s\n' "$checksum" "$destination" | sha256sum --check --status; then
        curl --fail --location --retry 3 --retry-all-errors --retry-delay 5 \
            --connect-timeout 30 --max-time 900 --output "$destination.part" "$url"
        printf '%s  %s\n' "$checksum" "$destination.part" | sha256sum --check --status
        mv "$destination.part" "$destination"
    fi
    chmod +x "$destination"
}

download 'https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage' \
    "$tools/linuxdeploy-x86_64.AppImage" \
    'c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d'
# The plugin is pinned independently of linuxdeploy; see tools/linux/README.md.
download 'https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage' \
    "$tools/linuxdeploy-plugin-qt-x86_64.AppImage" \
    '15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724'

# A fresh staging tree prevents old binaries, saves or keys from entering the package.
stage=$(mktemp -d "$root/.cache/appimage-stage.XXXXXX")
trap 'rm -rf -- "$stage"' 0
appdir="$stage/AppDir"
doc="$appdir/usr/share/doc/nemyu"
mkdir -p "$doc"
cp "$root/LICENSE.txt" "$doc/"
cp -R "$root/LICENSES" "$doc/"
tr -d '\r' < "$root/dist/dev.eden_emu.eden.desktop" > "$stage/io.gitlab.nemyu_emulator.nemyu.desktop"
cp "$root/dist/nemyu.png" "$stage/io.gitlab.nemyu_emulator.nemyu.png"

# Extraction mode works in unprivileged Docker runners without /dev/fuse.
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$QT_ROOT_DIR/bin/qmake"
export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
export ARCH=x86_64
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export LDAI_OUTPUT="$out/nemyu-$version-linux-x86_64.AppImage"
"$tools/linuxdeploy-x86_64.AppImage" --appdir "$appdir" \
    --executable "$build/bin/nemyu" --executable "$build/bin/nemyu-cli" \
    --desktop-file "$stage/io.gitlab.nemyu_emulator.nemyu.desktop" \
    --icon-file "$stage/io.gitlab.nemyu_emulator.nemyu.png" --plugin qt --output appimage
test -s "$LDAI_OUTPUT"
chmod +x "$LDAI_OUTPUT"

# Inspect the final SquashFS, then start both programs without a display or a game.
mkdir -p "$stage/check" "$stage/home"
cd "$stage/check"
"$LDAI_OUTPUT" --appimage-extract >/dev/null
extracted="$stage/check/squashfs-root"
test -f "$extracted/usr/share/doc/nemyu/LICENSE.txt"
xcb_plugin=$(find "$extracted/usr" -name libqxcb.so -print -quit)
test -n "$xcb_plugin"
for binary in "$extracted/usr/bin/nemyu" "$extracted/usr/bin/nemyu-cli" "$xcb_plugin"; do
    dependencies=$(LD_LIBRARY_PATH="$extracted/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$binary")
    case "$dependencies" in *'not found'*) printf '%s\n' "$dependencies" >&2; exit 1 ;; esac
done
export HOME="$stage/home"
export XDG_CONFIG_HOME="$HOME/config"
export XDG_CACHE_HOME="$HOME/cache"
export XDG_RUNTIME_DIR="$HOME/runtime"
mkdir -m 700 "$XDG_RUNTIME_DIR"
export QT_QPA_PLATFORM=offscreen
# The Qt frontend has no --version option. Bound its headless startup instead.
if timeout --kill-after=5s 10s "$extracted/AppRun" > "$out/GUI-SMOKE.log" 2>&1; then
    gui_status=0
else
    gui_status=$?
fi
case "$gui_status" in
    0|124) ;;
    *) cat "$out/GUI-SMOKE.log" >&2; exit "$gui_status" ;;
esac
if LD_LIBRARY_PATH="$extracted/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout --kill-after=5s 10s "$extracted/usr/bin/nemyu-cli" --version > "$out/CLI-SMOKE.log" 2>&1; then
    cli_status=0
else
    cli_status=$?
fi
# The current CLI prints --version, then returns SDL_APP_FAILURE for a missing ROM.
if [ "$cli_status" = 1 ] && grep -Fq 'Failed to load ROM: No ROM specified' "$out/CLI-SMOKE.log"; then
    cli_status=0
fi
if [ "$cli_status" != 0 ] || ! grep -Fq 'Nemyu ' "$out/CLI-SMOKE.log"; then
    cat "$out/CLI-SMOKE.log" >&2
    echo 'The packaged CLI did not pass its version check.' >&2
    exit 1
fi
printf 'CLI version check passed.\n'
cd "$out"
sha256sum "$(basename "$LDAI_OUTPUT")" > SHA256SUMS
printf 'Validated AppImage: %s\n' "$LDAI_OUTPUT"
