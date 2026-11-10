#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

case "${1:-build}" in
    build|configure) mode=${1:-build} ;;
    *) echo 'Usage: sh tools/linux/ci-appimage.sh [build|configure]' >&2; exit 1 ;;
esac

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
[ "$(uname -sm)" = 'Linux x86_64' ] || { echo 'Linux x86_64 is required.' >&2; exit 1; }
# This matches the CI image and the documented minimum glibc version of the package.
# shellcheck source=/dev/null
. /etc/os-release
[ "$ID:$VERSION_ID" = 'ubuntu:24.04' ] || { echo 'Use the ubuntu:24.04 container image.' >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo 'Run inside the container as root to install build dependencies.' >&2; exit 1; }
jobs=${BUILD_JOBS:-4}
case "$jobs" in ''|*[!0-9]*|0) echo 'BUILD_JOBS must be a positive integer.' >&2; exit 1 ;; esac
if [ "${GITLAB_CI:-false}" = true ]; then
    : "${CI_JOB_TOKEN:?CI_JOB_TOKEN is required for the package upload}"
    : "${CI_API_V4_URL:?}" "${CI_PROJECT_ID:?}" "${CI_JOB_ID:?}" "${CI_COMMIT_SHA:?}" "${CI_PIPELINE_ID:?}" "${CI_COMMIT_TAG:?}"
    printf '%s\n' "$CI_COMMIT_TAG" | grep -Eq '^[A-Za-z0-9]+([._-][A-Za-z0-9]+)*$'
fi

out="$root/out/linux-x86_64"
mkdir -p "$out"
finish() {
    result=$?
    trap - 0
    printf '\nExit status: %s\n' "$result" >> "$out/BUILDINFO.txt"
    if command -v ccache >/dev/null 2>&1; then ccache --show-stats || true; fi
    exit "$result"
}
trap finish 0
printf 'Nemyu Linux x86_64 build\nOS: %s\n' "$PRETTY_NAME" > "$out/BUILDINFO.txt"

export DEBIAN_FRONTEND=noninteractive
apt-get -o Acquire::Retries=3 update
apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
    autoconf automake binutils build-essential ca-certificates ccache curl file git \
    glslang-tools libasound2-dev libavcodec-dev libavfilter-dev libavutil-dev \
    libdbus-1-dev libegl1-mesa-dev libgl1-mesa-dev libglu1-mesa-dev libhidapi-dev \
    libopengl0 libopus-dev libpulse-dev libssl-dev libswresample-dev libswscale-dev \
    libtool libudev-dev libusb-1.0-0-dev libva-dev libvdpau-dev libvulkan-dev \
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
    libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxext-dev \
    libxkbcommon-dev libxkbcommon-x11-0 mesa-common-dev nasm patch pkg-config python3 python3-venv \
    shellcheck squashfs-tools xdg-utils zlib1g-dev
shellcheck -s sh "$root/tools/linux/ci-appimage.sh" "$root/tools/linux/package-appimage.sh" "$root/tools/publish-gitlab-release.sh"

# Do not use Noble's CMake 3.28: the project requires at least 3.31.
python3 -m venv "$root/.cache/linux-ci-python"
python="$root/.cache/linux-ci-python/bin/python"
"$python" -m pip install --disable-pip-version-check --retries 3 --timeout 120 \
    cmake==3.31.6 ninja==1.13.2 aqtinstall==3.3.0
PATH="$root/.cache/linux-ci-python/bin:$PATH"
export PATH
qt="$root/.cache/qt-linux/6.8.3/gcc_64"
if [ ! -f "$qt/.nemyu-install-complete" ] || [ ! -f "$qt/lib/cmake/Qt6Charts/Qt6ChartsConfig.cmake" ] || [ ! -x "$qt/bin/qmake" ]; then
    "$python" -m aqt install-qt linux desktop 6.8.3 linux_gcc_64 \
        --outputdir "$root/.cache/qt-linux" --modules qtcharts
    touch "$qt/.nemyu-install-complete"
fi
export QT_ROOT_DIR="$qt"
export CCACHE_DIR="${CCACHE_DIR:-$root/.cache/ccache-linux-amd64}"
export CCACHE_BASEDIR="$root"
export CCACHE_COMPILERCHECK=content
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
ccache --zero-stats
{
    printf 'Commit: %s\n' "$(git rev-parse HEAD)"
    printf 'Pipeline: %s\n' "${CI_PIPELINE_URL:-local}"
    printf 'Job: %s\n' "${CI_JOB_URL:-local}"
    gcc --version
    cmake --version
    ninja --version
    "$qt/bin/qmake" -query QT_VERSION
} >> "$out/BUILDINFO.txt"

cmake -S "$root" -B "$root/build-linux" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DDYNARMIC_TESTS=OFF \
    -DENABLE_QT=ON -DYUZU_CMD=ON -DYUZU_USE_BUNDLED_QT=OFF \
    -DYUZU_USE_BUNDLED_SDL3=ON -DVulkanHeaders_FORCE_BUNDLED=ON \
    -DVulkanUtilityLibraries_FORCE_BUNDLED=ON -DSPIRV-Headers_FORCE_BUNDLED=ON \
    -DBoost_FORCE_BUNDLED=ON -Dfmt_FORCE_BUNDLED=ON -Dzstd_FORCE_BUNDLED=ON -DENABLE_WERROR=OFF \
    -DUSE_CCACHE=ON "-DCMAKE_PREFIX_PATH=$qt" "-DQt6_DIR=$qt/lib/cmake/Qt6"
if [ "$mode" = configure ]; then exit 0; fi
cmake --build "$root/build-linux" --target yuzu yuzu-cmd --parallel "$jobs"
sh "$root/tools/linux/package-appimage.sh" "$root/build-linux"

if [ "${GITLAB_CI:-false}" = true ]; then
    name="nemyu-$CI_COMMIT_TAG-linux-x86_64.AppImage"
    package_url="$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/nemyu-linux-x86_64/$CI_COMMIT_SHA-$CI_JOB_ID"
    for artifact in "$name" SHA256SUMS BUILDINFO.txt; do
        status=$(curl --fail-with-body --location --no-progress-meter \
            --retry 3 --retry-all-errors --retry-delay 5 --connect-timeout 30 --max-time 900 \
            --header "JOB-TOKEN: $CI_JOB_TOKEN" --upload-file "$out/$artifact" \
            --output "$out/.upload-response.json" --write-out '%{http_code}' "$package_url/$artifact")
        if [ "$status" != 201 ]; then
            printf 'Package upload returned HTTP %s for %s.\n' "$status" "$artifact" >&2
            exit 1
        fi
    done
    "$python" - "$out/release.json" "$CI_COMMIT_TAG" "$CI_COMMIT_SHA" "$CI_PIPELINE_ID" "$name" "$package_url" <<'PY'
import json
import sys
path, tag, commit, pipeline_id, name, url = sys.argv[1:]
with open(path, "w", encoding="utf-8") as manifest:
    json.dump({"tag": tag, "commit": commit, "pipeline_id": pipeline_id, "assets": [
        {"name": name, "url": f"{url}/{name}", "link_type": "package"},
        {"name": f"{name}.sha256", "url": f"{url}/SHA256SUMS", "link_type": "other"},
    ]}, manifest)
PY
    printf '# Nemyu Linux build\n\n[Download AppImage](%s/%s)\n\n[SHA-256](%s/SHA256SUMS)\n' \
        "$package_url" "$name" "$package_url" > "$out/DOWNLOAD.md"
    printf 'AppImage uploaded: %s/%s\n' "$package_url" "$name"
else
    printf 'AppImage ready in %s\n' "$out"
fi
