#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Sets up, configures, builds and packages Nemyu on Linux. Run with --help.
# POSIX sh, so tools/shellcheck.sh can check it like the other scripts.

set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)

# Pinned like tools/linux/ci-appimage.sh and tools/windows/ci-release.ps1.
cmake_pin=3.31.6
ninja_pin=1.13.2
aqt_pin=3.3.0
# The AppImage's Qt (tools/linux/ci-appimage.sh); Windows releases use 6.10.2.
qt_version=6.8.3
min_cmake=3.31
min_gcc=13
min_clang=17
# Older Qt (Ubuntu 24.04 ships 6.4) is untested.
min_qt=6.5

venv="$root/.cache/linux-python"

usage() {
    cat <<EOF
Usage: sh tools/linux/build.sh [actions] [options] [-- extra CMake arguments]

Actions (run in this order; without any, --configure --build):
  --clean             Delete the build directory
  --setup             Check (or install) the packages, then create the pinned
                      CMake $cmake_pin / Ninja $ninja_pin environment in .cache
  --configure         Configure the build directory with Ninja
  --build             Build the targets; configures first when needed
  --package           Make a validated AppImage with tools/linux/package-appimage.sh
                      in out/linux-x86_64. Refuses a build that is stale, not
                      configured at HEAD, or made from uncommitted changes

Options:
  --fresh             Configure from scratch (cmake --fresh); alone, the build follows
  --build-type TYPE   Release (default), RelWithDebInfo, Debug or MinSizeRel
  --build-dir DIR     Build directory, relative to the repository (default: build-linux)
  --targets LIST      Comma-separated gui, cli, tests (default: gui,cli)
  --jobs N, -j N      Parallel jobs (default: from CPUs and available memory)
  --lto, --no-lto     Enable or disable link-time optimization
  --ccache, --no-ccache
                      Use ccache (cache in .cache/ccache-linux-amd64)
  --tests             Build the unit tests and run them with ctest
  --allow-dirty       Let --package use a build of uncommitted changes ("-dirty")
  --compiler NAME     gcc or clang (default: CMake's choice)
  --install-deps      With --setup: install missing packages (uses sudo)
  --distro FAMILY     Override detection: debian, fedora, arch or suse
  --aqt-qt            With --setup: install Qt $qt_version with aqtinstall into .cache/qt-linux
  --qt-version VER    Qt version for --aqt-qt and to find it later (default: $qt_version)
  --qt-dir DIR        Use the Qt installation at DIR
  --system-qt         Use the distribution's Qt even when an aqt Qt is installed
  --dry-run, -n       Print the commands without running them
  --help, -h          Show this help

Configure options are kept in the CMake cache, so a later --build reuses them.
Examples:
  sh tools/linux/build.sh --setup --install-deps
  sh tools/linux/build.sh --build-type RelWithDebInfo --ccache
  sh tools/linux/build.sh --build --package
  sh tools/linux/build.sh --fresh --build -- -DENABLE_LSFG=OFF
EOF
}

say() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# Quotes one argument for printing and for eval.
quote() {
    case "$1" in
        ''|*[!A-Za-z0-9_./:=+,@%-]*) printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")" ;;
        *) printf '%s' "$1" ;;
    esac
}

run() {
    printf '+'
    for arg do printf ' %s' "$(quote "$arg")"; done
    printf '\n'
    [ "$dry_run" = 1 ] || "$@"
}

# True when version $1 >= $2.
version_ge() {
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1)" = "$2" ]
}

actions=''
add_action() { case " $actions " in *" $1 "*) ;; *) actions="$actions $1" ;; esac; }
has_action() { case " $actions " in *" $1 "*) return 0 ;; esac; return 1; }

build_type=''
build_dir=build-linux
targets=gui,cli
jobs=''
lto=''
ccache=''
run_tests=0
allow_dirty=0
compiler=''
install_deps=0
family=''
aqt_qt=0
qt_dir=''
system_qt=0
dry_run=0
fresh=0
# Set by options that change the CMake cache, so --build reconfigures first.
reconfigure=0

while [ $# -gt 0 ]; do
    case "$1" in
        --*=*)
            # Split --option=value into two arguments.
            option=${1%%=*} value=${1#*=}
            shift
            set -- "$option" "$value" "$@"
            continue
            ;;
        -j[0-9]*)
            # -j8, like make and ninja.
            value=${1#-j}
            shift
            set -- -j "$value" "$@"
            continue
            ;;
    esac
    case "$1" in
        --build-type|--build-dir|--targets|--jobs|-j|--compiler|--distro|--qt-version|--qt-dir)
            [ $# -ge 2 ] || die "$1 needs a value."
            ;;
    esac
    case "$1" in
        --setup|--clean|--configure|--build|--package) add_action "${1#--}" ;;
        --fresh) fresh=1; reconfigure=1 ;;
        --build-type)
            case "$2" in
                Release|RelWithDebInfo|Debug|MinSizeRel) build_type=$2 ;;
                *) die "Unknown build type: $2" ;;
            esac
            reconfigure=1; shift ;;
        --build-dir) build_dir=$2; shift ;;
        --targets)
            for target in $(printf '%s' "$2" | tr ',' ' '); do
                case "$target" in
                    gui|cli) ;;
                    tests) reconfigure=1 ;;
                    *) die "Unknown target: $target (use gui, cli or tests)" ;;
                esac
            done
            [ -n "$(printf '%s' "$2" | tr -d ', ')" ] || die '--targets needs at least one target.'
            targets=$2; shift ;;
        --jobs|-j)
            case "$2" in ''|*[!0-9]*|0) die '--jobs needs a positive integer.' ;; esac
            jobs=$2; shift ;;
        --lto) lto=ON; reconfigure=1 ;;
        --no-lto) lto=OFF; reconfigure=1 ;;
        --ccache) ccache=ON; reconfigure=1 ;;
        --no-ccache) ccache=OFF; reconfigure=1 ;;
        --tests) run_tests=1; reconfigure=1 ;;
        --allow-dirty) allow_dirty=1 ;;
        --compiler)
            case "$2" in gcc|clang) compiler=$2 ;; *) die "Unknown compiler: $2" ;; esac
            reconfigure=1; shift ;;
        --install-deps) install_deps=1 ;;
        --distro)
            case "$2" in debian|fedora|arch|suse) family=$2 ;; *) die "Unknown distribution family: $2" ;; esac
            shift ;;
        --aqt-qt) aqt_qt=1 ;;
        --qt-version) qt_version=$2; shift ;;
        --qt-dir) qt_dir=$2; reconfigure=1; shift ;;
        --system-qt) system_qt=1; reconfigure=1 ;;
        --dry-run|-n) dry_run=1 ;;
        --help|-h) usage; exit 0 ;;
        --) shift; break ;;
        *) die "Unknown option: $1 (see --help)" ;;
    esac
    shift
done

# Everything after -- goes to CMake, quoted for eval.
extra=''
for arg do
    extra="$extra $(quote "$arg")"
    reconfigure=1
done

[ -n "$actions" ] || actions=' configure build'
# --fresh implies a configure, like -Fresh in tools/windows/build.ps1.
[ "$fresh" = 0 ] || add_action configure
case "$build_dir" in /*) ;; *) build_dir="$root/$build_dir" ;; esac
# Resolves ., .. and trailing slashes for the safety checks. CMake folds a missing
# directory's .. lexically, so such a path could point anywhere.
if [ -d "$build_dir" ]; then
    build_dir=$(CDPATH='' cd -- "$build_dir" && pwd)
else
    case "/$build_dir/" in */../*|*/./*) die "Use a build directory without . or .. components: $build_dir" ;; esac
fi
case "$qt_dir" in ''|/*) ;; *) qt_dir="$root/$qt_dir" ;; esac
qt_aqt="$root/.cache/qt-linux/$qt_version/gcc_64"
cache="$build_dir/CMakeCache.txt"

[ "$(uname -s)" = Linux ] || die 'This script builds for Linux; use tools/windows on Windows.'
case "$root" in
    /mnt/[a-z]/*) warn 'The repository is on a Windows drive; WSL builds are much faster on the Linux filesystem.' ;;
esac

# Reads one value from the CMake cache. Caches written on Windows use CRLF.
cache_value() {
    if [ -f "$cache" ]; then
        tr -d '\r' < "$cache" | sed -n "s/^$1:[A-Z]*=//p" | head -n 1
    fi
}

# Never use the repository, one of its parents, a Windows build directory or
# another source tree's.
check_build_dir() {
    case "$root/" in "${build_dir%/}"/*) die "Refusing to use $build_dir as the build directory." ;; esac
    case "$(cache_value CMAKE_CXX_COMPILER)" in
        [A-Za-z]:/*|*.exe|*.EXE) die "$build_dir was configured on Windows; use another --build-dir." ;;
    esac
    source_dir=$(cache_value CMAKE_HOME_DIRECTORY)
    if [ -n "$source_dir" ] && [ "$source_dir" != "$root" ] && [ "$source_dir" != "$(cd -P -- "$root" && pwd)" ]; then
        die "$build_dir belongs to another source tree ($source_dir)."
    fi
}

# Detects the distribution family from /etc/os-release.
detect_distro() {
    [ -r /etc/os-release ] || die 'Cannot read /etc/os-release; pass --distro.'
    # shellcheck source=/dev/null
    os_name=$(. /etc/os-release && printf '%s' "${PRETTY_NAME:-$ID}")
    if [ -z "$family" ]; then
        # shellcheck source=/dev/null
        for id in $(. /etc/os-release && printf '%s %s' "$ID" "${ID_LIKE:-}"); do
            case "$id" in
                debian|ubuntu) family=debian ;;
                fedora|rhel|centos) family=fedora ;;
                arch) family=arch ;;
                opensuse*|suse|sles) family=suse ;;
                *) continue ;;
            esac
            break
        done
    fi
    [ -n "$family" ] || die "Unsupported distribution: $os_name. Pass --distro, or see docs/Deps.md."
    say "Distribution: $os_name ($family family)"
}

# Prints the packages the build needs. The configure step bundles SDL3, Boost,
# fmt, zstd and the Vulkan/SPIR-V headers like CI, so they are not listed.
packages() {
    use_system_qt=1
    if [ -n "$qt_dir" ] || [ "$aqt_qt" = 1 ]; then use_system_qt=0; fi
    case "$family" in
        debian)
            # The same list as tools/linux/ci-appimage.sh.
            printf '%s\n' autoconf automake binutils ca-certificates ccache curl file git \
                glslang-tools libasound2-dev libavcodec-dev libavfilter-dev libavutil-dev \
                libdbus-1-dev libegl1-mesa-dev libgl1-mesa-dev libglu1-mesa-dev libhidapi-dev \
                libopengl0 libopus-dev libpulse-dev libssl-dev libswresample-dev libswscale-dev \
                libtool libudev-dev libusb-1.0-0-dev libva-dev libvdpau-dev libvulkan-dev \
                libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
                libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxext-dev \
                libxkbcommon-dev libxkbcommon-x11-0 mesa-common-dev nasm patch pkg-config \
                python3 python3-venv squashfs-tools xdg-utils zlib1g-dev
            case "$compiler" in clang) printf '%s\n' clang lld ;; *) printf '%s\n' build-essential g++ ;; esac
            [ "$use_system_qt" = 0 ] || printf '%s\n' qt6-base-dev qt6-base-dev-tools \
                qt6-base-private-dev qt6-charts-dev qt6-svg-dev
            ;;
        fedora|suse)
            # Both resolve pkgconfig() capabilities, whichever package provides them.
            printf '%s\n' autoconf automake binutils ccache curl file git libtool nasm patch \
                python3 xdg-utils 'pkgconfig(alsa)' 'pkgconfig(dbus-1)' 'pkgconfig(egl)' \
                'pkgconfig(gl)' 'pkgconfig(glu)' 'pkgconfig(hidapi-hidraw)' \
                'pkgconfig(libavcodec)' 'pkgconfig(libavfilter)' 'pkgconfig(libavutil)' \
                'pkgconfig(libpulse)' 'pkgconfig(libswresample)' 'pkgconfig(libswscale)' \
                'pkgconfig(libudev)' 'pkgconfig(libusb-1.0)' 'pkgconfig(libva)' \
                'pkgconfig(openssl)' 'pkgconfig(opus)' 'pkgconfig(vdpau)' 'pkgconfig(vulkan)' \
                'pkgconfig(xcb-cursor)' 'pkgconfig(xcb-icccm)' 'pkgconfig(xcb-image)' \
                'pkgconfig(xcb-keysyms)' 'pkgconfig(xcb-renderutil)' 'pkgconfig(xext)' \
                'pkgconfig(xkbcommon)' 'pkgconfig(xkbcommon-x11)' 'pkgconfig(zlib)'
            if [ "$family" = fedora ]; then
                # Fedora's FFmpeg is ffmpeg-free; RPM Fusion's ffmpeg-devel also works.
                printf '%s\n' glslang pkgconf-pkg-config squashfs-tools
                case "$compiler" in clang) printf '%s\n' clang lld ;; *) printf '%s\n' gcc gcc-c++ ;; esac
                [ "$use_system_qt" = 0 ] || printf '%s\n' qt6-qtbase-devel \
                    qt6-qtbase-private-devel qt6-qtcharts-devel qt6-qtsvg-devel
            else
                printf '%s\n' glslang-devel pkgconf squashfs
                case "$compiler" in clang) printf '%s\n' clang lld ;; *) printf '%s\n' gcc gcc-c++ ;; esac
                [ "$use_system_qt" = 0 ] || printf '%s\n' 'cmake(Qt6Charts)' \
                    'cmake(Qt6Concurrent)' 'cmake(Qt6Core)' 'cmake(Qt6DBus)' 'cmake(Qt6Svg)' \
                    'cmake(Qt6Widgets)' qt6-gui-private-devel
            fi
            ;;
        arch)
            printf '%s\n' alsa-lib base-devel ccache curl dbus ffmpeg file git glslang glu \
                hidapi libglvnd libpulse libusb libva libvdpau libxext libxkbcommon \
                libxkbcommon-x11 mesa nasm openssl opus patch pkgconf python squashfs-tools \
                systemd-libs vulkan-headers vulkan-icd-loader xcb-util-cursor xcb-util-image \
                xcb-util-keysyms xcb-util-renderutil xcb-util-wm xdg-utils zlib
            [ "$compiler" != clang ] || printf '%s\n' clang lld
            [ "$use_system_qt" = 0 ] || printf '%s\n' qt6-base qt6-charts qt6-svg
            ;;
    esac
}

is_installed() {
    case "$family" in
        debian) dpkg-query -W -f='${db:Status-Abbrev}' "$1" 2>/dev/null | grep -q '^ii' ;;
        fedora|suse) rpm -q --whatprovides "$1" >/dev/null 2>&1 ;;
        arch) pacman -T "$1" >/dev/null 2>&1 ;;
    esac
}

sudo=''
[ "$(id -u)" = 0 ] || sudo=sudo

# Prints the command that installs the given packages.
install_command() {
    printf '%s' "${sudo:+$sudo }"
    case "$family" in
        debian) printf 'apt-get install --no-install-recommends' ;;
        fedora) printf 'dnf install' ;;
        arch) printf 'pacman -S --needed' ;;
        suse) printf 'zypper install' ;;
    esac
    for package do printf ' %s' "$(quote "$package")"; done
}

check_compiler() {
    case "$compiler" in
        clang) cxx=clang++ minimum=$min_clang ;;
        gcc) cxx=g++ minimum=$min_gcc ;;
        *) cxx=${CXX:-c++} minimum='' ;;
    esac
    if ! command -v "$cxx" >/dev/null 2>&1; then
        warn "$cxx was not found."
        return 1
    fi
    version=$("$cxx" -dumpfullversion 2>/dev/null || "$cxx" -dumpversion)
    if [ -z "$minimum" ]; then
        case "$("$cxx" --version 2>/dev/null)" in *clang*) minimum=$min_clang ;; *) minimum=$min_gcc ;; esac
    fi
    if ! version_ge "$version" "$minimum"; then
        warn "$cxx $version is too old: GCC $min_gcc+ or Clang $min_clang+ is required."
        return 1
    fi
    say "Compiler: $cxx $version"
}

# Prints the system Qt version, if any.
system_qt_version() {
    for qmake in qmake6 /usr/lib/qt6/bin/qmake /usr/lib64/qt6/bin/qmake; do
        if command -v "$qmake" >/dev/null 2>&1; then
            "$qmake" -query QT_VERSION 2>/dev/null && return
        fi
    done
    pkg-config --modversion Qt6Core 2>/dev/null
}

setup_env() {
    detect_distro
    status=0
    missing=''
    for package in $(packages); do
        is_installed "$package" || missing="$missing $package"
    done
    if [ -n "$missing" ]; then
        # shellcheck disable=SC2086 # Package names have no spaces.
        command=$(install_command $missing)
        if [ "$install_deps" = 1 ]; then
            say 'Installing the missing packages'
            if [ "$family" = debian ]; then run ${sudo:+"$sudo"} apt-get update; fi
            eval "run $command"
        else
            say "Missing packages:$missing"
            printf 'Install them with:\n  %s\nor rerun with --install-deps.\n' "$command"
            status=1
        fi
    else
        say 'All the required packages are installed'
    fi

    check_compiler || status=1

    if [ -n "$qt_dir" ]; then
        [ -f "$qt_dir/lib/cmake/Qt6/Qt6Config.cmake" ] || die "No Qt 6 installation in $qt_dir"
        say "Qt: $qt_dir"
    elif [ "$aqt_qt" = 0 ]; then
        qt=$(system_qt_version || true)
        if [ -z "$qt" ]; then
            warn 'No system Qt 6 was found.'
            status=1
        elif ! version_ge "$qt" "$min_qt"; then
            warn "System Qt $qt is older than $min_qt; rerun with --aqt-qt to install Qt $qt_version."
            status=1
        else
            say "Qt: system $qt"
        fi
    fi

    if [ -n "$missing" ] && [ "$install_deps" = 0 ]; then
        die 'Install the missing packages, then run --setup again.'
    fi

    # Pinned CMake and Ninja, like CI; distribution CMake is often older than 3.31.
    # A venv whose Python was upgraded or removed no longer runs, so start it over.
    if ! "$venv/bin/python" -c '' >/dev/null 2>&1; then
        run python3 -m venv --clear "$venv"
    fi
    run "$venv/bin/python" -m pip install --disable-pip-version-check --retries 3 --timeout 120 \
        "cmake==$cmake_pin" "ninja==$ninja_pin" "aqtinstall==$aqt_pin"

    if [ "$aqt_qt" = 1 ]; then
        if [ -f "$qt_aqt/.nemyu-install-complete" ] && [ -f "$qt_aqt/lib/cmake/Qt6Charts/Qt6ChartsConfig.cmake" ] &&
            [ -x "$qt_aqt/bin/qmake" ]; then
            say "Qt $qt_version is already installed in $qt_aqt"
        else
            run "$venv/bin/python" -m aqt install-qt linux desktop "$qt_version" linux_gcc_64 \
                --outputdir "$root/.cache/qt-linux" --modules qtcharts
            run touch "$qt_aqt/.nemyu-install-complete"
        fi
    fi
    [ "$status" = 0 ] || die 'Setup is incomplete; see the warnings above.'
    say 'Setup complete'
}

# Puts the pinned tools first in PATH and checks CMake and Ninja.
resolve_tools() {
    if [ -x "$venv/bin/cmake" ]; then
        PATH="$venv/bin:$PATH"
        export PATH
    fi
    problem=''
    if ! command -v cmake >/dev/null 2>&1; then
        problem='CMake was not found'
    elif ! command -v ninja >/dev/null 2>&1; then
        problem='Ninja was not found'
    else
        version=$(cmake --version | sed -n 's/^cmake version //p')
        version_ge "$version" "$min_cmake" || problem="CMake $version is older than $min_cmake"
    fi
    [ -n "$problem" ] || return 0
    # A dry run only prints the commands, so it can go on without the tools.
    if [ "$dry_run" = 1 ]; then warn "$problem."; else die "$problem; run --setup first."; fi
}

# Prints the Qt prefix to configure with, or nothing for the system Qt.
qt_prefix() {
    if [ -n "$qt_dir" ]; then
        printf '%s' "$qt_dir"
    elif [ "$system_qt" = 0 ] && [ -f "$qt_aqt/.nemyu-install-complete" ]; then
        printf '%s' "$qt_aqt"
    fi
}

# Exports ccache's settings when the build directory uses it.
setup_ccache() {
    if [ "$ccache" = ON ] || [ "$(cache_value USE_CCACHE)" = ON ]; then
        CCACHE_DIR=${CCACHE_DIR:-$root/.cache/ccache-linux-amd64}
        CCACHE_BASEDIR=$root
        CCACHE_COMPILERCHECK=content
        CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-5G}
        export CCACHE_DIR CCACHE_BASEDIR CCACHE_COMPILERCHECK CCACHE_MAXSIZE
    fi
}

configure_project() {
    check_build_dir
    resolve_tools
    setup_ccache
    # Changing the compiler makes CMake throw the cache away, CI options included.
    if [ -n "$compiler" ] && [ -f "$cache" ] && [ "$fresh" = 0 ]; then
        case "$compiler" in gcc) wanted=g++ ;; *) wanted=clang++ ;; esac
        cached=$(cache_value CMAKE_CXX_COMPILER)
        resolved=$(command -v "$wanted" || true)
        if [ -z "$resolved" ] || [ "$(readlink -f -- "$cached")" != "$(readlink -f -- "$resolved")" ]; then
            die "$build_dir was configured with $cached; add --fresh to switch to $wanted, or use another --build-dir."
        fi
    fi
    first=1
    if [ -f "$cache" ] && [ "$fresh" = 0 ]; then first=0; fi
    args=''
    add() { for arg do args="$args $(quote "$arg")"; done; }
    add -S "$root" -B "$build_dir" -G Ninja
    [ "$fresh" = 0 ] || add --fresh
    if [ "$first" = 1 ]; then
        build_type=${build_type:-Release} lto=${lto:-OFF} ccache=${ccache:-OFF}
        # The CI configuration; extra arguments after -- override it.
        add -DENABLE_QT=ON -DYUZU_CMD=ON -DYUZU_USE_BUNDLED_QT=OFF -DYUZU_USE_BUNDLED_SDL3=ON \
            -DVulkanHeaders_FORCE_BUNDLED=ON -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
            -DSPIRV-Headers_FORCE_BUNDLED=ON -DBoost_FORCE_BUNDLED=ON -Dfmt_FORCE_BUNDLED=ON \
            -Dzstd_FORCE_BUNDLED=ON -DENABLE_WERROR=OFF -DDYNARMIC_TESTS=OFF
    fi
    [ -z "$build_type" ] || add "-DCMAKE_BUILD_TYPE=$build_type"
    [ -z "$lto" ] || add "-DENABLE_LTO=$lto"
    [ -z "$ccache" ] || add "-DUSE_CCACHE=$ccache"
    case "$run_tests,$targets" in
        1,*|*tests*) add -DBUILD_TESTING=ON -DYUZU_TESTS=ON ;;
        *) [ "$first" = 0 ] || add -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF ;;
    esac
    case "$compiler" in
        gcc) add -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ ;;
        # Clang's LTO needs lld, which USE_FASTER_LINKER selects.
        clang) add -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DUSE_FASTER_LINKER=ON ;;
    esac
    qt=$(qt_prefix)
    # Switching Qt must also forget the Qt6Core_DIR, Qt6Widgets_DIR... of the old one.
    if [ "$first" = 0 ]; then
        if [ "$system_qt" = 1 ]; then
            add '-UQt6*' -UCMAKE_PREFIX_PATH
        elif [ -n "$qt" ] && [ "$(cache_value Qt6_DIR)" != "$qt/lib/cmake/Qt6" ]; then
            add '-UQt6*'
        fi
    fi
    [ -z "$qt" ] || add "-DCMAKE_PREFIX_PATH=$qt" "-DQt6_DIR=$qt/lib/cmake/Qt6"
    say "Configuring $build_dir"
    eval "run cmake $args $extra"
}

# Each GCC job needs about 2 GiB at its peak; LTO links need more.
default_jobs() {
    cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
    memory=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo 2>/dev/null || echo 0)
    per_job=2097152
    if [ "$lto" = ON ] || [ "$(cache_value ENABLE_LTO)" = ON ]; then per_job=3145728; fi
    by_memory=$((${memory:-0} / per_job))
    [ "$by_memory" -ge 1 ] || by_memory=1
    if [ "$by_memory" -lt "$cpus" ]; then echo "$by_memory"; else echo "$cpus"; fi
}

build_project() {
    check_build_dir
    resolve_tools
    setup_ccache
    [ -f "$cache" ] || [ "$dry_run" = 1 ] || die "$build_dir is not configured; run --configure first."
    set --
    for target in $(printf '%s' "$targets" | tr ',' ' '); do
        case "$target" in
            gui) set -- "$@" yuzu ;;
            cli) set -- "$@" yuzu-cmd ;;
            tests) set -- "$@" tests ;;
        esac
    done
    [ "$run_tests" = 0 ] || case " $* " in *' tests '*) ;; *) set -- "$@" tests ;; esac
    jobs=${jobs:-$(default_jobs)}
    say "Building $* with $jobs jobs"
    run cmake --build "$build_dir" --parallel "$jobs" --target "$@"
    if [ "$run_tests" = 1 ]; then
        run ctest --test-dir "$build_dir" --output-on-failure --parallel "$jobs"
    fi
}

# Prints the Qt prefix used by the build, which has bin/qmake for linuxdeploy.
package_qt_root() {
    if [ -n "${QT_ROOT_DIR:-}" ]; then
        printf '%s' "$QT_ROOT_DIR"
        return
    fi
    qt=$(cache_value Qt6_DIR)
    qt=${qt%/lib/cmake/Qt6}
    if [ -n "$qt" ] && [ -x "$qt/bin/qmake" ]; then
        printf '%s' "$qt"
        return
    fi
    for qmake in qmake6 /usr/lib/qt6/bin/qmake /usr/lib64/qt6/bin/qmake; do
        if command -v "$qmake" >/dev/null 2>&1; then
            bins=$("$qmake" -query QT_HOST_BINS)
            if [ -x "$bins/qmake" ]; then
                dirname -- "$bins"
                return
            fi
        fi
    done
    [ "$dry_run" = 1 ] || die 'Cannot find the Qt installation; set QT_ROOT_DIR.'
    printf '<Qt prefix>'
}

# Refuses to package a build that does not match HEAD, like tools/windows/package.ps1.
check_package_build() {
    head=$(git -C "$root" rev-parse HEAD)
    # Only what goes into the build: the tree can hold other work (CI, docs).
    dirty=$(git -C "$root" status --porcelain --untracked-files=no -- src dist externals CMakeModules \
        .patch CMakeLists.txt cpmfile.json LICENSE.txt LICENSES)
    if [ -n "$dirty" ] && [ "$allow_dirty" = 0 ]; then
        die "The sources have uncommitted changes, so the build may not match $head. Commit them, or pass --allow-dirty."
    fi
    # The configure step stamps the commit into the build.
    built=$(sed -n 's/^#define GIT_REV "\([0-9a-f]*\)".*/\1/p' "$build_dir/src/common/scm_rev.cpp" 2>/dev/null || true)
    [ "$built" = "$head" ] || die "$build_dir was configured at '$built', not at HEAD ($head). Run --configure --build first."
    for binary in nemyu nemyu-cli; do
        [ -x "$build_dir/bin/$binary" ] || die "$build_dir/bin/$binary is missing; build gui and cli first."
    done
    # Also catches sources edited since the last build. A dry run of build.ninja itself
    # stops at CMake's glob check, so go through a file that only includes it.
    ninja=$(cache_value CMAKE_MAKE_PROGRAM)
    [ -x "$ninja" ] || die "The ninja $build_dir was configured with ('$ninja') was not found."
    wrapper=$(mktemp)
    printf 'include build.ninja\n' > "$wrapper"
    pending=$("$ninja" -C "$build_dir" -f "$wrapper" -n yuzu yuzu-cmd 2>&1) || pending="failed: $pending"
    rm -f -- "$wrapper"
    case "$pending" in
        *'no work to do'*) ;;
        *) die "$build_dir is not up to date with the sources. Run --build first." ;;
    esac
}

package_project() {
    check_build_dir
    version=$(git -C "$root" rev-parse --short=10 HEAD)
    if [ "$dry_run" = 0 ]; then
        check_package_build
        [ -z "$dirty" ] || version="$version-dirty"
    fi
    say 'Packaging the AppImage'
    # package-appimage.sh names the AppImage after CI_COMMIT_TAG, or the commit without it.
    run env "QT_ROOT_DIR=$(package_qt_root)" "CI_COMMIT_TAG=$version" \
        sh "$root/tools/linux/package-appimage.sh" "$build_dir"
}

clean_project() {
    check_build_dir
    [ -e "$build_dir" ] || return 0
    # Only delete what looks like a build directory.
    if [ ! -f "$cache" ] && [ -n "$(ls -A -- "$build_dir")" ]; then
        die "$build_dir has no CMakeCache.txt; refusing to delete it."
    fi
    say "Deleting $build_dir"
    run rm -rf -- "$build_dir"
}

if has_action clean; then clean_project; fi
if has_action setup; then setup_env; fi
# --build configures first when the cache is missing or a configure option was given.
if has_action configure || { has_action build && { [ ! -f "$cache" ] || [ "$reconfigure" = 1 ]; }; }; then
    configure_project
fi
if has_action build; then build_project; fi
if has_action package; then package_project; fi
