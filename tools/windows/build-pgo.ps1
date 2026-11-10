# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Optional tuned builds of Nemyu for Windows: MSVC with link-time code generation, or clang-cl with
# ThinLTO and profile-guided optimization. The release and build.ps1 defaults stay as they are;
# each mode builds into its own directory through build.ps1 and tuned-build.cmake.
# Run tools\windows\build-pgo.ps1 -Help for the steps.

param(
    [ValidateSet("msvc-ltcg", "clang-lto", "clang-pgo-generate", "merge", "clang-pgo-use")]
    [string]$Mode,
    [ValidateSet("v3", "generic")]
    [string]$Arch = "v3",
    [string]$LlvmRoot = "",
    [string]$ProfileDir = "",
    [string]$Profile = "",
    [string]$BuildDir = "",
    [string[]]$Targets = @("gui", "cli"),
    [int]$Jobs = 0,
    [int]$LinkJobs = 0,
    [switch]$Fresh,
    [switch]$DryRun,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

$Usage = @"
Usage: tools\windows\build-pgo.ps1 -Mode <mode> [options]

Modes (build directories are build-<mode> unless -BuildDir says otherwise):
  msvc-ltcg           MSVC with /GL + /LTCG (ENABLE_LTO) and /arch:AVX2.
  clang-lto           clang-cl with ThinLTO and -march=x86-64-v3, no profile.
  clang-pgo-generate  clang-cl instrumented build for profile training (slow; not for play).
  merge               Merge the training runs (<ProfileDir>\*.profraw) into <Profile>.
  clang-pgo-use       clang-cl with ThinLTO, -march=x86-64-v3 and -fprofile-use=<Profile>.

Options:
  -Arch v3|generic    Instruction set (default v3: needs AVX2, Haswell / Ryzen or newer).
  -LlvmRoot <dir>     LLVM install with bin\clang-cl.exe (default: LLVM_ROOT, C:\Program Files\LLVM,
                      C:\LLVM, then the Visual Studio "C++ Clang tools" component).
  -ProfileDir <dir>   Raw training profiles (default: .cache\pgo\raw).
  -Profile <file>     Merged profile (default: .cache\pgo\nemyu.profdata).
  -BuildDir <dir>     Build directory, relative to the repository.
  -Targets <list>     gui, cli, tests, all (default: gui,cli).
  -Jobs <n>           Compile jobs (default: build.ps1's, one per 2 GB of RAM).
  -LinkJobs <n>       ThinLTO backend jobs / MSVC /CGTHREADS (default: the linker's).
  -Fresh              Configure the build directory from scratch.
  -DryRun             Check the toolchain and print the commands without building.

Training (after clang-pgo-generate): run the instrumented nemyu-cli.exe or nemyu.exe with
LLVM_PROFILE_FILE=<ProfileDir>\<name>-%p.profraw on the games to optimize for (see docs/Build.md),
then -Mode merge and -Mode clang-pgo-use. Every step uses the same LLVM: a profile written by one
version may not load in another.
"@

function Invoke-Checked([string]$Exe, [string[]]$Arguments, [string]$Failure) {
    Write-Host "> `"$Exe`" $($Arguments -join ' ')" -ForegroundColor DarkGray
    if ($DryRun) { return }
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Failure (exit code $LASTEXITCODE)."
    }
}

function Find-Llvm {
    $candidates = @($LlvmRoot, $env:LLVM_ROOT, "$env:ProgramFiles\LLVM", "C:\LLVM")
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath 2>$null
        if ($vs) { $candidates += Join-Path "$vs".Trim() "VC\Tools\Llvm\x64" }
    }
    $root = $candidates | Where-Object { $_ -and (Test-Path (Join-Path $_ "bin\clang-cl.exe")) } | Select-Object -First 1
    if (-not $root) {
        throw "No LLVM with clang-cl.exe was found. Install LLVM for Windows (https://github.com/llvm/llvm-project/releases, " +
            "the same major version as the forks' Clang 22-23 is a good choice) or the Visual Studio component " +
            "'C++ Clang tools for Windows', or pass -LlvmRoot."
    }
    $tools = @{}
    foreach ($name in "clang-cl", "lld-link", "llvm-lib", "llvm-profdata") {
        $path = Join-Path $root "bin\$name.exe"
        if (-not (Test-Path $path)) { throw "$path is missing; this LLVM install is incomplete." }
        $tools[$name] = $path
    }
    $version = (& $tools["clang-cl"] --version 2>$null | Select-Object -First 1)
    Write-Host "  LLVM:       $root ($version)"
    $tools["root"] = $root
    $tools
}

# compiler-rt's profile runtime: lib\clang\<major>\lib\windows\clang_rt.profile-x86_64.lib in the
# official installer, lib\clang\<major>\lib\x86_64-pc-windows-msvc\clang_rt.profile.lib in builds
# with per-target runtime directories.
function Find-ProfileRuntime([string]$Root) {
    $libs = @(Get-ChildItem -Path (Join-Path $Root "lib\clang") -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in @("clang_rt.profile-x86_64.lib", "clang_rt.profile.lib") -and $_.FullName -notmatch "aarch64|i386" } |
        Sort-Object { [int]($_.FullName -replace '.*\\lib\\clang\\(\d+).*', '$1') } -Descending)
    if (-not $libs) {
        throw "No clang_rt.profile library was found under $Root\lib\clang. The instrumented build needs " +
            "compiler-rt's profile runtime, which the official LLVM installer includes."
    }
    $libs[0].FullName
}

function ConvertTo-CMakePath([string]$Path) {
    [IO.Path]::GetFullPath($Path).Replace("\", "/")
}

$exitCode = 0
try {
    if ($Help -or -not $Mode) {
        Write-Host $Usage
        return
    }
    $root = (& git -C $PSScriptRoot rev-parse --show-toplevel 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $root) {
        throw "Run this from a Nemyu git checkout (and with git on PATH)."
    }
    $root = [IO.Path]::GetFullPath("$root".Trim())
    if (-not $ProfileDir) { $ProfileDir = Join-Path $root ".cache\pgo\raw" }
    if (-not $Profile) { $Profile = Join-Path $root ".cache\pgo\nemyu.profdata" }
    $ProfileDir = [IO.Path]::GetFullPath($ProfileDir)
    $Profile = [IO.Path]::GetFullPath($Profile)

    Write-Host "Nemyu tuned build: $Mode" -ForegroundColor Cyan
    $useClang = $Mode -ne "msvc-ltcg"
    $llvm = $null
    if ($useClang) { $llvm = Find-Llvm }

    if ($Mode -eq "merge") {
        $raw = @(Get-ChildItem -Path $ProfileDir -Filter *.profraw -File -ErrorAction SilentlyContinue)
        if (-not $raw -and -not $DryRun) {
            throw "No .profraw files in $ProfileDir. Run the instrumented build with LLVM_PROFILE_FILE set first."
        }
        Write-Host "  Inputs:     $($raw.Count) raw profiles in $ProfileDir"
        if (-not $DryRun) { New-Item -ItemType Directory -Force (Split-Path $Profile) | Out-Null }
        # A run killed while writing leaves a truncated file: skip it instead of failing the merge.
        Invoke-Checked $llvm["llvm-profdata"] (@("merge", "--failure-mode=all", "-o", $Profile) + @($raw | ForEach-Object { $_.FullName })) "llvm-profdata merge failed"
        if (-not $DryRun) { Write-Host "  Profile:    $Profile ($([Math]::Round((Get-Item $Profile).Length / 1MB, 1)) MB)" }
        return
    }

    if (-not $BuildDir) { $BuildDir = "build-$Mode" }
    $cmakeArgs = @(
        "-DCMAKE_PROJECT_yuzu_INCLUDE=$(ConvertTo-CMakePath (Join-Path $PSScriptRoot 'tuned-build.cmake'))"
        "-DNEMYU_TUNED_ARCH=$Arch"
        "-DNEMYU_LTO_JOBS=$LinkJobs"
    )
    if ($useClang) {
        $cmakeArgs += @(
            "-DCMAKE_C_COMPILER=$(ConvertTo-CMakePath $llvm['clang-cl'])"
            "-DCMAKE_CXX_COMPILER=$(ConvertTo-CMakePath $llvm['clang-cl'])"
            "-DCMAKE_LINKER=$(ConvertTo-CMakePath $llvm['lld-link'])"
            "-DCMAKE_AR=$(ConvertTo-CMakePath $llvm['llvm-lib'])"
        )
    }
    $lto = $true
    switch ($Mode) {
        "msvc-ltcg" { $cmakeArgs += "-DNEMYU_PGO=off" }
        "clang-lto" { $cmakeArgs += "-DNEMYU_PGO=off" }
        "clang-pgo-generate" {
            # Instrumented code runs 2-3x slower; LTO would only make its build longer.
            $lto = $false
            $runtime = Find-ProfileRuntime $llvm["root"]
            Write-Host "  Runtime:    $runtime"
            $cmakeArgs += @("-DNEMYU_PGO=generate", "-DNEMYU_PGO_RUNTIME=$(ConvertTo-CMakePath $runtime)")
        }
        "clang-pgo-use" {
            if (-not (Test-Path $Profile) -and -not $DryRun) {
                throw "$Profile does not exist. Train with -Mode clang-pgo-generate and merge with -Mode merge first."
            }
            if (Test-Path $Profile) {
                Write-Host "  Profile:    $Profile (written $((Get-Item $Profile).LastWriteTime))"
            }
            $cmakeArgs += @("-DNEMYU_PGO=use", "-DNEMYU_PGO_PROFILE=$(ConvertTo-CMakePath $Profile)")
        }
    }

    $buildArgs = @{
        BuildDir  = $BuildDir
        Targets   = $Targets
        Jobs      = $Jobs
        Lto       = $lto
        Fresh     = [bool]$Fresh
        DryRun    = [bool]$DryRun
        CMakeArgs = $cmakeArgs
    }
    & (Join-Path $PSScriptRoot "build.ps1") @buildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "build.ps1 failed."
    }

    if ($Mode -eq "clang-pgo-generate") {
        $bin = Join-Path (Join-Path $root $BuildDir) "bin"
        Write-Host ""
        Write-Host "Instrumented programs are in $bin. Train them, e.g. with the MKW race benchmark:" -ForegroundColor Cyan
        Write-Host "  tools\nx2\mkw_bench\bench.ps1 -Tag pgo-mkw -Exe $bin\nemyu-cli.exe -Game <...> -Config <...> ``"
        Write-Host "      -Seconds 900 -QuitAtFrame 6000 -EnvPairs `"EDEN_NX2_MKW_FPS_MODE=dynamic;LLVM_PROFILE_FILE=$ProfileDir\mkw-%p.profraw`""
        Write-Host "then the other games (docs/Build.md), and: tools\windows\build-pgo.ps1 -Mode merge"
    }
} catch {
    Write-Host ""
    Write-Host "build-pgo.ps1: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
exit $exitCode
