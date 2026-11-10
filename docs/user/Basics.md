# User Handbook - The Basics

## Introduction

Nemyu is a very complicated piece of software, there are many knobs and toggles that can be configured. Most of these are invisible to normal users, however, power users may be able to leverage them to their advantage.

This handbook primarily describes such knobs and toggles. Normal configuration options are described within the emulator itself and will not be covered in detail.

## Requirements

The emulator is very demanding on hardware and requires a decent mid-range computer.

Eden's [requirements page](https://archive.is/sv83h), written for Switch 1 games, gives a rough idea of recommended and minimum specs.

The CPU must support FMA for an optimal gameplay experience. The GPU needs to support OpenGL 4.6 ([compatibility list](https://opengl.gpuinfo.org/)), or Vulkan 1.1 ([compatibility list](https://vulkan.gpuinfo.org/)). Switch 2 games need the Vulkan backend and a GPU with Vulkan 1.3; see the [system requirements](../../README.md#system-requirements) in the README.

If your GPU doesn't support or is just behind by a minor version, see the [Mesa environment variable hacks](./Graphics.md#mesa-environment-variable-hacks) (*nix only).

## Releases

Nemyu releases are published on the [Releases page](https://gitlab.com/nemyu-emulator/nemyu/-/releases). Each release has a Windows x86-64 zip built with MSVC that bundles Qt, its plugins, the Visual C++ runtime and the license texts: extract it and run `nemyu.exe`, or `nemyu-cli.exe` from a terminal.

New tag releases also include an experimental Linux x86-64 AppImage, which needs glibc 2.39 or newer (Ubuntu 24.04). There is no prebuilt release for macOS and no Android release; on macOS, build Nemyu from source by following the [build guide](../Build.md).

Releases are made from the `nemyu` branch. Building the latest `nemyu` branch yourself gets you changes that are not in a release yet.

## User configuration

### Configuration directories

Nemyu will store configuration files and data in the following directories:

- **Windows**: `%APPDATA%\nemyu` (for example `C:\Users\<USER>\AppData\Roaming\nemyu`), with the configuration files in its `config` subfolder.
- **Linux, macOS, FreeBSD, Solaris, OpenBSD**: `$XDG_DATA_HOME/nemyu`, `$XDG_CACHE_HOME/nemyu` and `$XDG_CONFIG_HOME/nemyu` (by default `~/.local/share/nemyu`, `~/.cache/nemyu` and `~/.config/nemyu`).
- **HaikuOS**: `/boot/home/config/settings/nemyu`

If a `user` directory is present next to the executable (Windows) or in the current working directory (other platforms), that will override all global configuration directories and the emulator will use the `user` directory instead. This makes the installation portable.

If you used Eden (or an earlier Nemyu build) before and there is no `nemyu` directory yet, Nemyu copies your keys, firmware, saves, mods, SD card, shader caches and settings from the `eden` directory into `nemyu` the first time it starts, and leaves the original untouched. Installed updates and DLC are not copied; install them again.

### Environment variables

Throughout the handbook, environment variables are mentioned. These are often either global (system wide) or local (set in a script, bound only to the current session). It's heavily recommended to use them in a local context only, as this allows you to rollback changes easily (if for example, there are regressions setting them).

On Windows, the recommended way is to create a `.bat` file next to `nemyu.exe`; contents of which could resemble something like:

```bat
set "__GL_THREADED_OPTIMIZATIONS=1"
set "SOME_OTHER_VAR=1"
nemyu.exe
```

For other platforms, the recommended method is using a shell script:

```sh
export __GL_THREADED_OPTIMIZATIONS=1
export SOME_OTHER_VAR=1
./nemyu
```

Then just running `chmod +x script.sh && source script.sh`.

## Compatibility list

Nemyu doesn't maintain a full compatibility list yet. Its goal is to run every Switch 2 game; the games tested regularly, with their status and measured performance, are listed in the [Compatibility](../../README.md#compatibility) section of the README.

For Switch 1 games, [EmuReady](https://www.emuready.com/) has fine-grained compatibility information for multiple emulators and forks.
