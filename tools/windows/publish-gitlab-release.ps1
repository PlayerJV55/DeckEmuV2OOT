# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Publishes the Windows build in the build folder as a GitLab release. It does not build anything: commit,
# re-run the CMake configure step (it stamps the commit into the build), build, push, then run
#   tools/windows/publish-gitlab-release.ps1 -Tag nemyu-1
# Needs glab logged in (glab auth login). Set NEMYU_DISCORD_WEBHOOK to announce the release, and
# NEMYU_DISCORD_ROLE to a role id to ping it.
# Tag pipelines use -TagAlreadyExists and glab's CI_JOB_TOKEN auto-login.
# -PackageOnly prepares the ZIP without creating a tag or publishing a release.

param(
    [Parameter(Mandatory)] [string]$Tag,
    [string]$Name = "Nemyu $Tag",
    [string]$Notes = "",
    [string]$BuildDir = "build",
    [string]$Remote = "origin",
    [string]$Branch = "nemyu",
    [string]$Project = "nemyu-emulator/nemyu",
    [switch]$TagAlreadyExists,
    [switch]$PackageOnly,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Invoke-Git {
    $output = & git -C $root @args
    if ($LASTEXITCODE -ne 0) {
        throw "git $args failed"
    }
    $output
}

if ($Tag -notmatch '^[A-Za-z0-9]+([._-][A-Za-z0-9]+)*$' -or $Tag.EndsWith(".lock") -or $Tag -eq "HEAD") {
    throw "'$Tag' is not a usable tag name: use letters and digits joined by '.', '_' or '-', like nemyu-1."
}
# Windows PowerShell passes these to glab wrongly.
if (-not $Name.Trim() -or $Name.Contains('"') -or $Name.EndsWith("\")) {
    throw "The release name must not be empty, contain double quotes or end with a backslash."
}

$root = & git -C $PSScriptRoot rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0) {
    throw "Run this from a Nemyu checkout."
}
if (-not $PackageOnly) {
    $glab = Get-Command glab -ErrorAction SilentlyContinue
    if ($glab) {
        $glab = $glab.Source
    } elseif (Test-Path "$env:ProgramFiles\glab\glab.exe") {
        $glab = "$env:ProgramFiles\glab\glab.exe"
    } else {
        throw "glab was not found. Install it and run 'glab auth login' once."
    }
}

# The build must be exactly the commit being released.
$head = Invoke-Git rev-parse HEAD
if ($TagAlreadyExists) {
    if ($env:CI_COMMIT_TAG -cne $Tag) {
        throw "-TagAlreadyExists requires CI_COMMIT_TAG to match '$Tag'."
    }
    if ($env:CI_COMMIT_SHA -and $env:CI_COMMIT_SHA -ne $head) {
        throw "CI_COMMIT_SHA does not match HEAD ($head)."
    }
}
if (Invoke-Git status --porcelain --untracked-files=no) {
    throw "The working tree has uncommitted changes, so the build may not match $head."
}
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$bin = Join-Path $build "bin"
$scmRev = Join-Path $build "src\common\scm_rev.cpp"
if (-not (Test-Path $scmRev)) {
    throw "$scmRev was not found. Configure and build $BuildDir first."
}
$stamp = Get-Content $scmRev -Raw
$builtRev = [regex]::Match($stamp, '#define GIT_REV "([0-9a-f]+)"').Groups[1].Value
$version = [regex]::Match($stamp, '#define BUILD_VERSION "([^"]*)"').Groups[1].Value
if ($builtRev -ne $head) {
    throw "$BuildDir was configured at '$builtRev', not at HEAD ($head). Re-run the CMake configure step and build again."
}
foreach ($exe in "nemyu.exe", "nemyu-cli.exe") {
    if (-not (Test-Path (Join-Path $bin $exe))) {
        throw "$exe was not built in $BuildDir."
    }
}
# Also catches sources edited or reverted since the last build, which a clean tree does not show.
$ninja = [regex]::Match((Get-Content (Join-Path $build "CMakeCache.txt") -Raw), '(?m)^CMAKE_MAKE_PROGRAM:FILEPATH=(.+?)\r?$').Groups[1].Value
if (-not $ninja -or -not (Test-Path $ninja)) {
    throw "The ninja $BuildDir was configured with ('$ninja') was not found, so the build cannot be checked."
}
# A dry run of build.ninja itself always stops at CMake's glob check, so go through a manifest
# that only includes it.
$wrapper = [IO.Path]::GetTempFileName()
try {
    [IO.File]::WriteAllText($wrapper, "include build.ninja`n")
    $pending = & $ninja -C $build -f $wrapper -n yuzu yuzu-cmd
    if ($LASTEXITCODE -ne 0 -or -not ($pending -match "no work to do")) {
        throw "$BuildDir is not up to date with the sources. Build yuzu and yuzu-cmd again."
    }
} finally {
    Remove-Item $wrapper -ErrorAction SilentlyContinue
}

# The commit must already belong to the remote release branch, including in a detached checkout.
$remoteUrl = Invoke-Git remote get-url $Remote
if ($remoteUrl -notmatch "gitlab\.com[:/]$([regex]::Escape($Project))(\.git)?/?$") {
    throw "Remote '$Remote' ($remoteUrl) is not gitlab.com/$Project."
}
& git -C $root fetch --quiet $Remote "+refs/heads/${Branch}:refs/remotes/$Remote/$Branch"
if ($LASTEXITCODE -ne 0) {
    throw "Could not fetch $Branch from $Remote. Push it first: git push $Remote $Branch"
}
& git -C $root merge-base --is-ancestor $head "refs/remotes/$Remote/$Branch"
if ($LASTEXITCODE -ne 0) {
    throw "$head is not on $Remote/$Branch. Push it first: git push $Remote $Branch"
}
if ($TagAlreadyExists) {
    # Peel annotated tags; lightweight tags point directly at the commit.
    $remoteTag = @(Invoke-Git ls-remote --tags $Remote "refs/tags/$Tag" "refs/tags/$Tag^{}")
    $tagCommit = $remoteTag | Where-Object { $_.EndsWith("refs/tags/$Tag^{}") } | Select-Object -First 1
    if (-not $tagCommit) {
        $tagCommit = $remoteTag | Where-Object { $_.EndsWith("refs/tags/$Tag") } | Select-Object -First 1
    }
    if (-not $tagCommit -or ($tagCommit -split '\s+')[0] -ne $head) {
        throw "Remote tag $Tag does not exist at HEAD ($head)."
    }
} else {
    if (Invoke-Git ls-remote --tags $Remote "refs/tags/$Tag") {
        throw "Tag $Tag already exists on $Remote. Pick a new tag."
    }
    # This clone also has the upstream's tags, which a fetch of the new tag would clash with.
    & git -C $root rev-parse --quiet --verify "refs/tags/$Tag" | Out-Null
    if ($LASTEXITCODE -eq 0) {
        throw "A local tag $Tag already exists (probably an upstream one). Pick another name, like nemyu-1."
    }
}

# Only the programs, Qt and its plugins, and the Visual C++ runtime go in the zip. Anything else in
# bin, such as the PDBs or a portable user folder holding keys and saves, must not ship.
# windeployqt also copies Windows' own icuuc.dll, which must come from the system it runs on.
$binPath = (Resolve-Path $bin).ProviderPath
if ($TagAlreadyExists) {
    foreach ($runtime in "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Charts.dll", "plugins\platforms\qwindows.dll") {
        if (-not (Test-Path (Join-Path $binPath $runtime))) {
            throw "Qt runtime $runtime is missing. Build with dynamic Qt and windeployqt before publishing."
        }
    }
}
$files = @(Get-ChildItem $binPath -File | Where-Object { $_.Extension -in ".exe", ".dll" -or $_.Name -eq "qt.conf" } |
    Where-Object {
        $system = Join-Path "$env:SystemRoot\System32" $_.Name
        -not ((Test-Path $system) -and (Get-FileHash $system).Hash -eq (Get-FileHash $_.FullName).Hash)
    })
$files += Get-ChildItem (Join-Path $binPath "plugins") -Recurse -File -Filter *.dll

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Parent.Parent.Name -match '^\d+(\.\d+)+$' } |
    Sort-Object { [version]$_.Parent.Parent.Name } -Descending | Select-Object -First 1
if (-not $crt) {
    throw "The Visual C++ runtime DLLs (VC\Redist\MSVC\<version>\x64\Microsoft.VC*.CRT) were not found."
}
$crtFiles = Get-ChildItem $crt.FullName -File -Filter *.dll

# Entries are added one by one: ZipFile.CreateFromDirectory on Windows PowerShell's .NET Framework
# writes backslashes into the entry names.
$package = "nemyu-$Tag-windows-x86_64"
$zip = Join-Path $build "release\$package.zip"
New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
if (Test-Path $zip) {
    Remove-Item $zip -Force
}
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in $files) {
        $entry = $file.FullName.Substring($binPath.Length + 1).Replace("\", "/")
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, "Optimal") | Out-Null
    }
    foreach ($file in $crtFiles) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $file.Name, "Optimal") | Out-Null
    }
    # The GPL and the licenses of the bundled libraries (Qt's LGPL, OpenSSL's Apache 2.0, ...).
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $root "LICENSE.txt"), "LICENSE.txt", "Optimal") | Out-Null
    foreach ($file in Get-ChildItem (Join-Path $root "LICENSES") -File) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, "LICENSES/$($file.Name)", "Optimal") | Out-Null
    }
} finally {
    $archive.Dispose()
}
"Packaged $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MiB)"

$body = @(
    $Notes
    "Build of $Branch at $head. Nemyu shows it as $version."
    "Windows: extract the zip and run nemyu.exe, or nemyu-cli.exe from a terminal. The Visual C++ runtime is included."
) | Where-Object { $_ }
$notesFile = Join-Path $build "release\$package.md"
[IO.File]::WriteAllText($notesFile, ($body -join "`n`n"), (New-Object Text.UTF8Encoding $false))
if ($PackageOnly) { return }

# The package registry has no 100 MiB cap, unlike release uploads.
# In CI, reference the tag itself so a deleted tag cannot be recreated from a commit SHA.
$releaseRef = if ($TagAlreadyExists) { $Tag } else { $head }
$glabArgs = @("release", "create", $Tag, $zip, "--repo", $Project, "--ref", $releaseRef, "--name", $Name,
              "--notes-file", $notesFile, "--no-update", "--use-package-registry")
if ($DryRun) {
    "Dry run, not published. Would run: glab $glabArgs"
    return
}
# With output redirected, Windows PowerShell turns glab's progress on stderr into errors, which
# would stop the script after the release exists.
$ErrorActionPreference = "Continue"
& $glab @glabArgs
$glabExit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($glabExit -ne 0) {
    throw "glab release create failed."
}
$url = "https://gitlab.com/$Project/-/releases/$Tag"
"Published $url"

if ($env:NEMYU_DISCORD_WEBHOOK) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    # NEMYU_DISCORD_ROLE, a role id, gets pinged like the old GitHub workflow did.
    $role = $env:NEMYU_DISCORD_ROLE
    $ping = if ($role) { "<@&$role> " } else { "" }
    $message = @{
        content = "## $ping$Name ($($head.Substring(0, 7)))`n$url"
        allowed_mentions = @{ parse = @(); roles = @($role | Where-Object { $_ }) }
    } | ConvertTo-Json -Compress
    try {
        Invoke-RestMethod -Method Post -Uri $env:NEMYU_DISCORD_WEBHOOK -ContentType "application/json; charset=utf-8" `
            -Body ([Text.Encoding]::UTF8.GetBytes($message)) | Out-Null
        "Announced on Discord."
    } catch {
        Write-Warning "The release is published, but the Discord post failed: $($_.Exception.Message)"
    }
}
