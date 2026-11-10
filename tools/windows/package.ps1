# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Zips a local Windows build to test or share, with the same rules as publish-gitlab-release.ps1 but
# without tagging or uploading anything:
#   tools/windows/package.ps1 [-BuildDir build] [-AllowDirty]
# Writes <BuildDir>\release\nemyu-<commit>-windows-x86_64.zip and a .sha256 next to it. A build that
# was not configured at HEAD, or is not up to date with the sources, is refused. -AllowDirty packages
# a build of uncommitted changes, and marks the zip name with "-dirty".

param(
    [string]$BuildDir = "build",
    [switch]$AllowDirty
)

$ErrorActionPreference = "Stop"

function Invoke-Git {
    $output = & git -C $root @args
    if ($LASTEXITCODE -ne 0) {
        throw "git $args failed"
    }
    $output
}

$root = & git -C $PSScriptRoot rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0) {
    throw "Run this from a Nemyu checkout."
}
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$bin = Join-Path $build "bin"
$head = Invoke-Git rev-parse HEAD

# Only what goes into the build: the tree can hold other work (CI, docs) that does not matter here.
$dirty = Invoke-Git status --porcelain --untracked-files=no -- src dist externals CMakeModules .patch CMakeLists.txt cpmfile.json LICENSE.txt LICENSES
if ($dirty -and -not $AllowDirty) {
    throw "The sources have uncommitted changes, so the build may not match $head. Commit them, or pass -AllowDirty."
}

$scmRev = Join-Path $build "src\common\scm_rev.cpp"
if (-not (Test-Path $scmRev)) {
    throw "$scmRev was not found. Configure and build $BuildDir first."
}
$builtRev = [regex]::Match((Get-Content $scmRev -Raw), '#define GIT_REV "([0-9a-f]+)"').Groups[1].Value
if ($builtRev -ne $head) {
    throw "$BuildDir was configured at '$builtRev', not at HEAD ($head). Re-run the CMake configure step and build again."
}
foreach ($exe in "nemyu.exe", "nemyu-cli.exe") {
    if (-not (Test-Path (Join-Path $bin $exe))) {
        throw "$exe was not built in $BuildDir."
    }
}
foreach ($runtime in "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Charts.dll", "plugins\platforms\qwindows.dll") {
    if (-not (Test-Path (Join-Path $bin $runtime))) {
        throw "Qt runtime $runtime is missing from $bin. Build with dynamic Qt (windeployqt runs after the link)."
    }
}

# Also catches sources edited or reverted since the last build, which a clean tree does not show.
# A dry run of build.ninja itself always stops at CMake's glob check, so go through a manifest that
# only includes it.
$ninja = [regex]::Match((Get-Content (Join-Path $build "CMakeCache.txt") -Raw), '(?m)^CMAKE_MAKE_PROGRAM:FILEPATH=(.+?)\r?$').Groups[1].Value
if (-not $ninja -or -not (Test-Path $ninja)) {
    throw "The ninja $BuildDir was configured with ('$ninja') was not found, so the build cannot be checked."
}
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

# Only the programs, Qt and its plugins, and the Visual C++ runtime go in the zip. Anything else in
# bin, such as the PDBs or a portable user folder holding keys and saves, must not ship.
# windeployqt also copies Windows' own icuuc.dll, which must come from the system it runs on. A build
# configured with -Tests also has test programs, which the release never ships.
$binPath = (Resolve-Path $bin).ProviderPath
$files = @(Get-ChildItem $binPath -File | Where-Object { $_.Extension -in ".exe", ".dll" -or $_.Name -eq "qt.conf" } |
    Where-Object { $_.Name -notmatch '(^|_)tests?\.exe$' } |
    Where-Object {
        $system = Join-Path "$env:SystemRoot\System32" $_.Name
        -not ((Test-Path $system) -and (Get-FileHash $system).Hash -eq (Get-FileHash $_.FullName).Hash)
    })
$files += Get-ChildItem (Join-Path $binPath "plugins") -Recurse -File -Filter *.dll

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found, so the Visual C++ runtime cannot be located."
}
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
$suffix = if ($dirty) { "-dirty" } else { "" }
$filename = "nemyu-$($head.Substring(0, 10))$suffix-windows-x86_64.zip"
$zip = Join-Path $build "release\$filename"
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

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$zip.sha256", "$hash  $filename`n", (New-Object Text.UTF8Encoding $false))
"Packaged $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MiB, $($files.Count) program files + $($crtFiles.Count) runtime)"
