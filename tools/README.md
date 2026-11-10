# Tools

Tools for Nemyu and its subprojects. When adding new scripts please use `#!/bin/sh -e` or `#!/usr/bin/env <interpreter>` (for `.py`, `.rb`, or `.perl`). Keep scripts POSIX compliant (i.e not require hard `bash` to run, just plain old `sh`).

## Third-Party

- [CPMUtil Scripts](./cpm)

## Binaries

- `maxwell-spirv`: Converts Maxwell shaders (dumped from `.ash` files) into SPIR-V code (emitted into STDOUT).
- `maxwell-disas`: Dumb raw Maxwell disassembler.
- `maxwell-ir`: Dump generated IR of Maxwell shaders.

## Scripts

- `linux/build.sh`: Sets up, configures, builds and packages a local Linux build. See [Linux builds](linux/README.md#local-builds).
- `linux/ci-appimage.sh`: Builds and validates a Linux x86_64 AppImage in GitLab's tag pipeline. See [Linux builds](linux/README.md).
- `publish-gitlab-release.sh`: Publishes both platform packages after the Windows and Linux jobs succeed. See [Releasing](RELEASING.md).
- `generate_converters.py`: Generates converters for given formats of textures (C++ helper).
- `svc_generator.py`: Generates the files `src/core/hle/kernel/svc.cpp` and `src/core/hle/kernel/svc.h` based off prototypes.
- `switch2_titles.py`: Regenerates `dist/switch2_titles.json`, the Switch 2 program ID to name table (from the Switchbrew wiki) embedded into core for dumps without control data.
- `shellcheck.sh`: Ensure POSIX compliance (and syntax sanity) for all tools in this directory and subdirectories.
- `windows/build.ps1`: Sets up the Windows toolchain (Visual Studio 2022, a venv with the CI's CMake/Ninja/aqtinstall, Qt, the Vulkan SDK) in `.cache`, then configures, builds and packages. Run it with `-Help`.
- `windows/package.ps1`: Zips a local Windows build (no PDBs or user data, with Qt and the Visual C++ runtime) into `<build>/release` with a `.sha256`, refusing a stale build.
- `windows/publish-gitlab-release.ps1`: Publishes the Windows build in `build` as a GitLab release, packaged with Qt and the Visual C++ runtime.
- `llvmpipe-run.sh`: Sets environment variables needed to run any command (or Nemyu) with llvmpipe.
- `optimize-assets.sh`: Optimizes PNG assets with OptiPng.
- `update-cpm.sh`: Updates CPM.cmake to the latest version.
- `update-icons.sh`: Rebuild all icons (macOS, Windows, bitmaps) based on the master SVG file (`dist/dev.eden_emu.eden.svg`)
    * Also optimizes the master SVG
    * Requires: `png2icns` (libicns), ImageMagick, [`svgo`](https://github.com/svg/svgo)
- `dtrace-tool.sh`
- `lanczos-gen.pl`: Generates constants for the Lanczos filter.
- `clang-format.sh`: Runs `clang-format` on the entire codebase.
    * Requires: clang
- `find-unused-strings.sh`: Find any unused strings in the Android app (XML -> Kotlin).
- `cpp-lint.sh`: Homemade dumb C++ linter.
- `fuzzsettings.cpp`: Fuzz settings files.

## Android

It's recommended to run these scripts after almost any Android change, as they are relatively fast and important both for APK bloat and CI.

- `unused-strings.sh`: Finds unused strings in `strings.xml` files.
- `stale-translations.sh`: Finds translated strings that aren't present in the source `strings.xml` file.

## Translations

- [Translation Scripts](./translations)
