# GitLab tag releases

Creating a Git tag on GitLab, or pushing one from Git, starts the release pipeline.
The tagged commit must contain `.gitlab-ci.yml` and already belong to `origin/nemyu`.
Branch pushes and merge requests do not start release builds.

For example, after committing and pushing the release changes to `nemyu`:

```sh
git tag v0.2.0
git push origin v0.2.0
```

On the site, use **Code > Tags > New tag**, enter the tag name and select `nemyu`
as the ref. Release notes are filled by CI. Monitor **Build > Pipelines**.

The pipeline runs these jobs:

1. `windows_build`: installs Vulkan SDK 1.4.341.1 and Qt 6.10.2, loads MSVC,
   builds GUI and CLI with CMake/Ninja, and packages the existing Windows ZIP.
2. `linux_appimage`: builds GUI and CLI on Ubuntu 24.04 with Qt 6.8.3 and validates
   the [Linux AppImage](linux/README.md).
3. `publish_release`: waits for both jobs, validates their metadata, and creates
   **Nemyu <tag>** with both packages and SHA-256 checksums.

Windows and Linux build in parallel. PGO is not enabled. The binaries are uploaded
to the Generic Package Registry using `CI_JOB_TOKEN`; only small manifests and
diagnostics are transferred through job artifacts. Package versions include the
commit and job ID, so a retry does not overwrite a previous job's package.
Registry files remain available after job artifacts expire.

The release job uses the official glab `v1.126.0` image with CI auto-login.
It references the existing tag and never explicitly creates a tag. Detached HEAD
is supported. It checks that the remote tag still points at `CI_COMMIT_SHA` and
that the commit belongs to `origin/nemyu`. Repeating the release job updates named
asset links rather than creating duplicates. A failed build prevents release publication.
No personal access token or additional secret is required. The registry and hosted
runners must be enabled, and the initiating user must have publishing permission.

Windows' hosted runner has 2 CPUs and 7.5 GB RAM and a three-hour job limit.
The build stops before that limit to save its sccache and retries once automatically
with exit code 75 if the cold build needs more time. A retry uses cached objects.
Caches are an optimization, so external downloads, quota and runner availability
can still affect a build. Other compiler failures are not automatically retried.

The existing Windows manual command remains available:

```powershell
.\tools\windows\publish-gitlab-release.ps1 -Tag v0.2.0 -BuildDir build
```

Its normal mode still creates the tag through `glab release create` and uploads
the ZIP with `--use-package-registry`. CI instead uses `-TagAlreadyExists -PackageOnly`
to reuse its validation and packaging, then publishes both platforms
in the final job. `-DryRun` remains available for the manual publishing command.

## References

- [Tag-triggered releases](https://docs.gitlab.com/user/project/releases/release_cicd_examples/)
- [Windows hosted runner](https://docs.gitlab.com/ci/runners/hosted_runners/windows/)
- [Hosted runner limits](https://docs.gitlab.com/ci/runners/hosted_runners/)
- [Generic Package Registry](https://docs.gitlab.com/user/packages/generic_packages/)
- [glab release creation and CI authentication](https://docs.gitlab.com/cli/release/create/)
- [glab v1.126.0 image source](https://gitlab.com/gitlab-org/cli/-/blob/v1.126.0/Dockerfile)
