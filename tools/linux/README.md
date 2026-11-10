# Linux AppImage builds

The GitLab tag pipeline builds Linux x86_64 on the `saas-linux-medium-amd64` runner
(4 CPUs, 16 GB RAM), in parallel with Windows. Creating or pushing a tag starts
both builds and publishes them in one release after both succeed. See
[Releasing](../RELEASING.md) for tag creation and the complete workflow.

This build job does not create a tag or a release. After the package passes validation,
the job uploads the AppImage, `SHA256SUMS` and `BUILDINFO.txt` to the project's
Generic Package Registry using `CI_JOB_TOKEN`. Find them in **Deploy > Package
registry**, under `nemyu-linux-x86_64`; the version is `<commit>-<job-id>`.
Each job attempt gets a distinct version, including retries.
The job artifacts contain `release.json` for the final publishing job and
`DOWNLOAD.md` with the direct download link, and retain
configuration diagnostics for 30 days. The actual AppImage is kept in the
Package Registry, independently of artifact expiration and artifact size limits.
Package uploads require the registry to be enabled and the initiating user to
have permission to publish packages. No personal access token is needed.

## Build environment

The build uses Ubuntu 24.04, GCC 13, CMake 3.31.6, Ninja 1.13.2 and Qt 6.8.3 with
Qt Charts. CMake and Ninja come from a Python virtual environment because Ubuntu
24.04's CMake 3.28 does not satisfy the project's minimum. The project targets
are still `yuzu` and `yuzu-cmd`; the executables are `nemyu` and `nemyu-cli`.
PGO is not enabled. The build retains the project's default runtime features.

CPM downloads, Qt, packaging tools and ccache objects are cached separately from
Windows. A missing cache is supported. The build tree and Python environment
are not cached. `BUILD_JOBS` defaults to 4; reduce it if changing to a smaller
runner. Jobs are serialized to avoid concurrent writes to the same cache.
The script has 2 hours 40 minutes, leaving time within the 3-hour job timeout
for uploading caches and diagnostics. A failed job does not publish a release.

Packaging uses linuxdeploy `1-alpha-20251107-1` and its Qt plugin
`1-alpha-20250213-1`, both checked against SHA-256 hashes in
`package-appimage.sh`. Update the URL and checksum together when upgrading.
AppImages run in extraction mode while packaging, so the runner needs no FUSE
device, privileged Docker container, GPU, keys, firmware or game files.

The final AppImage is extracted and checked for the two executables, Qt's XCB
plugin, dependencies and licenses. The CLI must print its version without a crash
or timeout. Its current missing-ROM exit status is accepted only with the expected
version and missing-ROM messages. The Qt frontend, which has no version flag,
must start headlessly without crashing during a bounded 10-second run.
This is a packaging check, not a game compatibility test.
Only release binaries, dependencies and licenses enter the staging directory;
user data and `Lossless.dll` are not bundled.

The expected minimum system is x86_64 Linux with glibc 2.39 (Ubuntu 24.04) or
newer, X11 or XWayland, and suitable graphics drivers. AppImage does not bundle
the host graphics driver or guarantee compatibility with older distributions.
Test the resulting package on a real Linux desktop before attaching it to a public release.

## Local use

From a Linux checkout, run in the same container as CI:

```sh
docker run --rm -v "$PWD:/source" -w /source ubuntu:24.04 \
    sh tools/linux/ci-appimage.sh
```

The output is `out/linux-x86_64/nemyu-<tag>-linux-x86_64.AppImage` in CI and
`nemyu-<commit>-linux-x86_64.AppImage` locally when `CI_COMMIT_TAG` is not set.
Without GitLab's environment variables, the script does not upload anything.
Use `sh tools/linux/ci-appimage.sh configure` inside the container to install
dependencies and configure CMake without compiling or uploading. To package an
existing Linux build, set `QT_ROOT_DIR` to its Qt prefix and run
`sh tools/linux/package-appimage.sh /absolute/path/to/build`.

## Local builds

`tools/linux/build.sh` sets up, configures, builds and packages a local build on
Debian/Ubuntu, Fedora, Arch and openSUSE. It uses the CI configuration and pins
(CMake 3.31.6 and Ninja 1.13.2 in `.cache/linux-python`), builds into `build-linux`
and picks the job count from the CPUs and available memory:

```sh
sh tools/linux/build.sh --setup                 # check packages; prints the install command
sh tools/linux/build.sh --setup --install-deps  # install them with the distribution's package manager
sh tools/linux/build.sh                         # configure and build nemyu and nemyu-cli
sh tools/linux/build.sh --build --package       # build, then make a validated AppImage
```

If the distribution's Qt is older than 6.5, add `--aqt-qt` to `--setup` to install
Qt 6.8.3 (the AppImage's version) into `.cache/qt-linux`; later configures use it
automatically. `--package` refuses a build that is stale, not configured at HEAD or
made from uncommitted changes; `--allow-dirty` packages the latter with a `-dirty`
name. Other options include `--build-type`, `--targets gui,cli,tests`, `--tests`,
`--lto`, `--ccache`, `--compiler clang`, `--fresh`, `--clean` and `--dry-run`.
Switching compilers needs `--fresh`. Arguments after `--` go to CMake. See
`sh tools/linux/build.sh --help`.

## References

- [Upstream release workflow](https://github.com/Djihads80/Eglantine/blob/nx2/.github/workflows/release-binaries.yml)
- [GitLab Linux runner sizes](https://docs.gitlab.com/ci/runners/hosted_runners/linux/)
- [Pipeline workflow rules](https://docs.gitlab.com/ci/yaml/workflow/)
- [GitLab cache behavior](https://docs.gitlab.com/ci/caching/)
- [Generic Package Registry and job-token authentication](https://docs.gitlab.com/user/packages/generic_packages/)
- [linuxdeploy packaging guide](https://docs.appimage.org/packaging-guide/from-source/linuxdeploy-user-guide.html)
- [Qt deployment plugin](https://github.com/linuxdeploy/linuxdeploy-plugin-qt)
- [AppImage output plugin](https://github.com/linuxdeploy/linuxdeploy-plugin-appimage)
