# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Sets up the Windows toolchain, then configures, builds and packages Nemyu with MSVC and Ninja.
# Run tools\windows\build.ps1 -Help for the options. Tool versions match the release CI
# (tools/windows/ci-release.ps1); everything it installs goes into .cache, except the Vulkan SDK.

param(
    [switch]$Setup,
    [switch]$Configure,
    [switch]$Build,
    [switch]$Package,
    [switch]$Clean,
    [switch]$Fresh,
    [string]$BuildType = "Release",
    [string]$BuildDir = "build",
    [string[]]$Targets = @("gui", "cli"),
    [int]$Jobs = 0,
    [switch]$Lto,
    [switch]$Sccache,
    [switch]$Tests,
    [string[]]$CMakeArgs = @(),
    [switch]$AllowDirty,
    [switch]$DryRun,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
# Windows PowerShell's progress bar slows Invoke-WebRequest and Expand-Archive down several times.
$ProgressPreference = "SilentlyContinue"

$QtVersion = "6.10.2"
$QtArch = "win64_msvc2022_64"
$VulkanSdkVersion = "1.4.341.1"
$PythonPins = [ordered]@{ "cmake" = "3.31.6"; "ninja" = "1.13.2"; "aqtinstall" = "3.3.0" }
$SccacheVersion = "0.19.0"
$SccacheSha256 = "28BE8C61F6B2C89B9EB2869F6AF7F78E6F992E78A8ABAE0CE855FCC129CBEAC1"
$TargetNames = [ordered]@{ "gui" = "yuzu"; "cli" = "yuzu-cmd"; "tests" = "tests"; "all" = "" }

$Usage = @"
Usage: tools\windows\build.ps1 [steps] [options]

With no step, sets up what is missing, configures when needed and builds.

Steps (run in this order when combined):
  -Setup              Find or install the toolchain and stop.
  -Clean              Delete the build outputs (ninja clean), keeping the configuration.
  -Configure          Run the CMake configure step even when nothing changed.
  -Build              Build, configuring first when needed.
  -Package            Zip the build into <BuildDir>\release\nemyu-<commit>-windows-x86_64.zip
                      and a .sha256. Refuses a build that is stale or not configured at HEAD.

Options:
  -Fresh              Configure from scratch (cmake --fresh). Alone, the build follows. Needed
                      for a build directory configured by another CMake (e.g. WinGet's).
  -BuildType <type>   Release (default), RelWithDebInfo or Debug.
  -BuildDir <dir>     Build directory, relative to the repository (default: build).
  -Targets <list>     Comma-separated: gui, cli, tests, all (default: gui,cli).
  -Jobs <n>           Parallel jobs (default: one per 2 GB of RAM, at most one per thread).
  -Lto                Link-time optimization (ENABLE_LTO).
  -Sccache            Compile through sccache (on PATH, or downloaded into .cache\build-tools).
  -Tests              Configure the unit tests (YUZU_TESTS), build the tests target and run ctest.
  -CMakeArgs <args>   Extra configure arguments, e.g. -CMakeArgs '-DENABLE_WERROR=OFF','-DYUZU_ROOM=OFF'
  -AllowDirty         Let -Package zip a build of uncommitted changes (named "-dirty").
  -DryRun             Check the toolchain and print the commands without installing or building.
  -Help               Show this text.

Setup uses Visual Studio 2022 with "Desktop development with C++", git (Git for Windows, for
GNU patch), Python 3 for a venv in .cache\build-tools with CMake $($PythonPins.cmake), Ninja $($PythonPins.ninja)
and aqtinstall $($PythonPins.aqtinstall), Qt $QtVersion msvc2022_64 + qtcharts in .cache\qt-msvc, and the
Vulkan SDK (VULKAN_SDK, or C:\VulkanSDK\$VulkanSdkVersion; installing it needs an elevated shell).

Examples:
  tools\windows\build.ps1
  tools\windows\build.ps1 -BuildType RelWithDebInfo -BuildDir build-dbg -Sccache
  tools\windows\build.ps1 -Build -Package
  tools\windows\build.ps1 -Fresh -Lto -Jobs 6
"@

function Write-Section([string]$Title) {
    Write-Host ""
    Write-Host "== $Title" -ForegroundColor Cyan
}

function Write-Status([string]$State, [string]$What) {
    $color = switch ($State) { "found" { "Green" } "installed" { "Green" } "missing" { "Yellow" } default { "Gray" } }
    Write-Host ("  {0,-10}" -f $State) -ForegroundColor $color -NoNewline
    Write-Host " $What"
}

# Runs a program, echoing it first, and stops the script if it fails. With -DryRun it is only echoed.
function Invoke-Native([string]$Exe, [string[]]$Arguments, [string]$Failure) {
    Write-Host "> `"$Exe`" $($Arguments -join ' ')" -ForegroundColor DarkGray
    if ($DryRun) { return }
    # CMake prints its warnings on stderr, which Windows PowerShell would turn into errors when
    # this script's output is redirected.
    $ErrorActionPreference = "Continue"
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Failure (exit code $LASTEXITCODE)."
    }
}

# Runs a program for its output, ignoring stderr. $LASTEXITCODE is left for the caller.
function Get-NativeOutput([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = "Continue"
    & $Exe @Arguments 2>$null
}

function Add-ToPath([string]$Dir) {
    $parts = $env:PATH -split ";"
    if ($parts -notcontains $Dir) {
        $env:PATH = "$Dir;$env:PATH"
    }
}

function Save-Download([string]$Uri, [string]$OutFile, [string]$Sha256) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $OutFile -TimeoutSec 180
            break
        } catch {
            if ($attempt -eq 3) { throw }
            Write-Warning "Download attempt $attempt of $Uri failed; retrying."
            Start-Sleep -Seconds 5
        }
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $OutFile).Hash -ne $Sha256) {
        Remove-Item -LiteralPath $OutFile -Force
        throw "$Uri does not have the expected SHA-256."
    }
}

function Get-DefaultJobs {
    # MSVC compiles of this tree take up to about 2 GB each; more jobs than that make 16 GB machines swap.
    $ramGiB = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB
    [Math]::Max(1, [Math]::Min([Environment]::ProcessorCount, [int][Math]::Round($ramGiB / 2)))
}

function Initialize-Git {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        throw "git was not found on PATH. Install Git for Windows: https://git-scm.com/download/win"
    }
    Write-Status found "$(Get-NativeOutput $git.Source @('--version')) ($($git.Source))"

    # CPM applies its patches with GNU patch, which it looks for in usr\bin next to git, as CPMUtil does.
    $gitDir = Split-Path $git.Source -Parent
    $candidates = @(
        (Join-Path (Split-Path $gitDir -Parent) "usr\bin\patch.exe")
        (Join-Path (Split-Path (Split-Path $gitDir -Parent) -Parent) "usr\bin\patch.exe")
    )
    $patch = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $patch) {
        $patch = (Get-Command patch -ErrorAction SilentlyContinue).Source
    }
    if (-not $patch) {
        throw "GNU patch was not found next to git or on PATH. Install the full Git for Windows (MinGit has no patch.exe)."
    }
    Write-Status found "GNU patch ($patch)"
    $script:PatchExe = $patch
}

function Initialize-Msvc {
    if ($env:PROCESSOR_ARCHITECTURE -ne "AMD64") {
        throw "Only x64 Windows is supported (Qt $QtVersion msvc2022_64)."
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe was not found. Install Visual Studio 2022 with 'Desktop development with C++'."
    }
    # The same query as load-msvc-env.ps1, so the instance found is the one it loads.
    $query = @("-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64")
    $vs = Get-NativeOutput $vswhere ($query + @("-property", "installationPath"))
    if (-not $vs) {
        throw "No Visual Studio with the C++ x64 tools was found. Add 'Desktop development with C++' in the Visual Studio Installer."
    }
    $name = Get-NativeOutput $vswhere ($query + @("-property", "displayName"))
    $version = Get-NativeOutput $vswhere ($query + @("-property", "installationVersion"))
    Write-Status found "$name $version ($vs)"
    if (-not "$version".StartsWith("17.")) {
        Write-Warning "Qt $QtVersion msvc2022_64 and the release are built with Visual Studio 2022 (17.x)."
    }

    if ($env:VSCMD_ARG_TGT_ARCH -eq "x64" -and (Get-Command cl -ErrorAction SilentlyContinue)) {
        Write-Status found "MSVC environment already loaded ($env:VCToolsVersion)"
        return
    }
    # vcvarsall calls vswhere itself and complains when it is not on PATH.
    Add-ToPath (Split-Path $vswhere -Parent)
    . (Join-Path $PSScriptRoot "load-msvc-env.ps1")
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        throw "load-msvc-env.ps1 did not put cl.exe on PATH."
    }
    Write-Status found "MSVC $env:VCToolsVersion (cl.exe loaded from vcvarsall)"
}

function Find-BasePython {
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) {
        $exe = Get-NativeOutput $launcher.Source @("-3", "-c", "import sys; print(sys.executable)")
        if ($LASTEXITCODE -eq 0 -and $exe) { return "$exe".Trim() }
    }
    # Without Python, the python.exe in WindowsApps is only a Store shortcut, which fails here.
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        $exe = Get-NativeOutput $python.Source @("-c", "import sys; print(sys.executable)")
        if ($LASTEXITCODE -eq 0 -and $exe) { return "$exe".Trim() }
    }
    throw "Python 3 was not found. Install it from https://www.python.org/downloads/windows/ (with the py launcher)."
}

function Initialize-BuildTools {
    $venv = Join-Path $root ".cache\build-tools"
    $scripts = Join-Path $venv "Scripts"
    $python = Join-Path $scripts "python.exe"
    $script:ToolsDir = $venv
    $script:VenvPython = $python
    $script:CMakeExe = Join-Path $scripts "cmake.exe"
    $script:NinjaExe = Join-Path $scripts "ninja.exe"

    $venvWorks = $false
    if (Test-Path $python) {
        Get-NativeOutput $python @("-c", "import sys") | Out-Null
        $venvWorks = $LASTEXITCODE -eq 0
    }
    if (-not $venvWorks) {
        # A venv whose base Python was removed or upgraded no longer runs, so start it over.
        $base = Find-BasePython
        Write-Status missing "Python venv in $venv (creating it with $base)"
        Invoke-Native $base @("-m", "venv", "--clear", $venv) "Could not create the Python venv"
    } else {
        Write-Status found "Python venv ($venv)"
    }

    $installed = @{}
    if (-not $DryRun -or $venvWorks) {
        foreach ($line in Get-NativeOutput $python @("-m", "pip", "list", "--format=freeze", "--disable-pip-version-check")) {
            $name, $version = "$line".Split("=", [StringSplitOptions]::RemoveEmptyEntries)
            if ($name) { $installed[$name.ToLowerInvariant()] = $version }
        }
    }
    $wanted = @($PythonPins.Keys | ForEach-Object { "$_==$($PythonPins[$_])" })
    $stale = @($PythonPins.Keys | Where-Object { $installed[$_] -ne $PythonPins[$_] })
    if ($stale) {
        Write-Status missing "$(($stale | ForEach-Object { "$_ $($PythonPins[$_])" }) -join ', ')"
        Invoke-Native $python (@("-m", "pip", "install", "--disable-pip-version-check", "--retries", "3", "--timeout", "120") + $wanted) "Could not install the build tools"
        if (-not $DryRun) { Write-Status installed ($wanted -join ", ") }
    } else {
        Write-Status found ($wanted -join ", ")
    }
    Add-ToPath $scripts
}

function Initialize-Qt {
    $qtRoot = Join-Path $root ".cache\qt-msvc"
    $qt = Join-Path $qtRoot "$QtVersion\msvc2022_64"
    $script:QtDir = $qt
    # The marker is written after a complete install; an older install without it is checked by its files.
    $files = @("bin\windeployqt.exe", "bin\Qt6Core.dll", "bin\Qt6Widgets.dll", "bin\Qt6Charts.dll",
        "plugins\platforms\qwindows.dll", "lib\cmake\Qt6\Qt6Config.cmake", "lib\cmake\Qt6Charts\Qt6ChartsConfig.cmake")
    $complete = (Test-Path "$qt\.nemyu-install-complete") -or
        -not ($files | Where-Object { -not (Test-Path (Join-Path $qt $_)) })
    if ($complete) {
        Write-Status found "Qt $QtVersion msvc2022_64 + qtcharts ($qt)"
    } else {
        Write-Status missing "Qt $QtVersion msvc2022_64 + qtcharts (installing with aqtinstall into $qtRoot)"
        # aqt writes its log into the current directory, which a dry run may not have created yet.
        Push-Location $(if (Test-Path $ToolsDir) { $ToolsDir } else { $root })
        try {
            Invoke-Native $VenvPython @("-m", "aqt", "install-qt", "windows", "desktop", $QtVersion, $QtArch, "--outputdir", $qtRoot, "--modules", "qtcharts") "Could not install Qt"
        } finally {
            Pop-Location
        }
        if (-not $DryRun) {
            New-Item -ItemType File -Path "$qt\.nemyu-install-complete" -Force | Out-Null
            Write-Status installed "Qt $QtVersion ($qt)"
        }
    }
    Add-ToPath "$qt\bin"
}

function Initialize-VulkanSdk {
    # Any installed SDK will do (only glslangValidator is used); the pinned one is installed when none is.
    $installed = @(Get-ChildItem "C:\VulkanSDK" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+(\.\d+)+$' } | Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { $_.FullName })
    $candidates = @($env:VULKAN_SDK, "C:\VulkanSDK\$VulkanSdkVersion") + $installed | Where-Object { $_ }
    $sdk = $candidates | Where-Object { Test-Path (Join-Path $_ "Bin\glslangValidator.exe") } | Select-Object -First 1
    if ($sdk -and (Split-Path $sdk -Leaf) -ne $VulkanSdkVersion) {
        Write-Warning "Using the Vulkan SDK in $sdk; the release CI uses $VulkanSdkVersion."
    }
    if (-not $sdk) {
        Write-Status missing "Vulkan SDK $VulkanSdkVersion"
        $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
        if (-not $admin -and -not $DryRun) {
            throw "The Vulkan SDK installs system-wide: run this once from an elevated PowerShell, or install $VulkanSdkVersion from https://vulkan.lunarg.com/sdk/home."
        }
        # The installer script downloads into the current directory and needs Windows PowerShell.
        $download = Join-Path $ToolsDir "vulkan-sdk"
        if (-not $DryRun) { New-Item -ItemType Directory -Path $download -Force | Out-Null }
        Push-Location $(if ($DryRun) { $root } else { $download })
        try {
            Invoke-Native "powershell.exe" @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "install-vulkan-sdk.ps1")) "Could not install the Vulkan SDK"
        } finally {
            Pop-Location
        }
        $sdk = "C:\VulkanSDK\$VulkanSdkVersion"
        if ($DryRun) { return }
        if (-not (Test-Path "$sdk\Bin\glslangValidator.exe")) {
            throw "The Vulkan SDK installer finished, but $sdk\Bin\glslangValidator.exe is missing."
        }
        Write-Status installed "Vulkan SDK ($sdk)"
    } else {
        Write-Status found "Vulkan SDK ($sdk)"
    }
    $env:VULKAN_SDK = $sdk
    Add-ToPath "$sdk\Bin"
    $glslang = (Get-Command glslangValidator -ErrorAction Stop).Source
    Write-Status found "glslangValidator ($glslang)"
}

function Initialize-Sccache {
    $onPath = Get-Command sccache -ErrorAction SilentlyContinue
    if ($onPath) {
        $script:SccacheExe = $onPath.Source
        Write-Status found "sccache ($($onPath.Source))"
        return
    }
    $dir = Join-Path $ToolsDir "sccache"
    $exe = Get-ChildItem -LiteralPath $dir -Recurse -File -Filter sccache.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($exe) {
        $script:SccacheExe = $exe.FullName
        Write-Status found "sccache ($($exe.FullName))"
        return
    }
    $uri = "https://github.com/mozilla/sccache/releases/download/v$SccacheVersion/sccache-v$SccacheVersion-x86_64-pc-windows-msvc.zip"
    Write-Status missing "sccache $SccacheVersion (downloading $uri)"
    $script:SccacheExe = Join-Path $dir "sccache-v$SccacheVersion-x86_64-pc-windows-msvc\sccache.exe"
    if ($DryRun) { return }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $archive = Join-Path $dir "sccache.zip"
    Save-Download $uri $archive $SccacheSha256
    Expand-Archive -LiteralPath $archive -DestinationPath $dir -Force
    Remove-Item -LiteralPath $archive
    $exe = Get-ChildItem -LiteralPath $dir -Recurse -File -Filter sccache.exe | Select-Object -First 1
    if (-not $exe) { throw "The sccache archive did not contain sccache.exe." }
    $script:SccacheExe = $exe.FullName
    Write-Status installed "sccache ($($exe.FullName))"
}

function Get-ConfigureArgs {
    $onOff = @{ $true = "ON"; $false = "OFF" }
    $arguments = @(
        "-S", $root, "-B", $buildPath, "-G", "Ninja"
        "-DCMAKE_BUILD_TYPE=$BuildType"
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$NinjaExe"
        "-DQt6_DIR=$QtDir\lib\cmake\Qt6"
        "-DENABLE_QT=ON", "-DYUZU_CMD=ON", "-DYUZU_USE_BUNDLED_QT=OFF"
        "-DYUZU_TESTS=$($onOff[[bool]$wantTests])"
        "-DENABLE_LTO=$($onOff[[bool]$Lto])"
        "-DPATCH_EXE:FILEPATH=$PatchExe"
        "-DUSE_CCACHE=$($onOff[[bool]$Sccache])"
    )
    if ($Sccache) {
        $arguments += "-DCCACHE_PATH=$SccacheExe"
        # sccache cannot cache compiles that write a shared PDB (/Zi), so embed debug info instead.
        if ($BuildType -ne "Release") {
            $arguments += "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded"
        }
    }
    if (-not $Sccache -or $BuildType -eq "Release") {
        $arguments += "-UCMAKE_MSVC_DEBUG_INFORMATION_FORMAT"
    }
    $arguments + $CMakeArgs
}

# The commit the build directory was configured at, or "" when it was not.
function Get-BuiltRevision {
    $scmRev = Join-Path $buildPath "src\common\scm_rev.cpp"
    if (-not (Test-Path $scmRev)) { return "" }
    [regex]::Match((Get-Content $scmRev -Raw), '#define GIT_REV "([0-9a-f]+)"').Groups[1].Value
}

function Invoke-Configure([bool]$Force) {
    $arguments = Get-ConfigureArgs
    $stampFile = Join-Path $buildPath ".nemyu-build-args"
    $stamp = $arguments -join "`n"
    $configured = Test-Path (Join-Path $buildPath "CMakeCache.txt")
    if (-not $Force -and $configured -and (Test-Path $stampFile)) {
        $head = (Get-NativeOutput "git" @("-C", $root, "rev-parse", "HEAD"))
        # The configure step stamps the commit into the build, so a new commit also needs one.
        if ((Get-Content $stampFile -Raw) -eq $stamp -and (Get-BuiltRevision) -eq "$head".Trim()) {
            Write-Host "  Already configured with these options at HEAD; skipping (use -Configure to force)."
            return
        }
    }
    if ($Fresh) {
        $arguments = @("--fresh") + $arguments
    } elseif ($configured) {
        # A cache written by another CMake (another version, or a WSL one) is not reconfigured in place:
        # it can fail halfway, and the directory may be in use by a build started some other way.
        $cache = Get-Content (Join-Path $buildPath "CMakeCache.txt") -Raw
        $cachedCMake = [regex]::Match($cache, '(?m)^CMAKE_COMMAND:INTERNAL=(.+?)\r?$').Groups[1].Value
        # pip's cmake.exe is a launcher, so the cache names the real binary somewhere inside the venv.
        if ($cachedCMake -and -not $cachedCMake.Replace("/", "\").StartsWith("$ToolsDir\", [StringComparison]::OrdinalIgnoreCase)) {
            throw "$buildPath was configured with another CMake ($cachedCMake). Pass -Fresh to reconfigure it from scratch with CMake $($PythonPins.cmake), or pick another -BuildDir."
        }
    }
    Invoke-Native $CMakeExe $arguments "CMake configure failed"
    if (-not $DryRun) {
        [IO.File]::WriteAllText($stampFile, $stamp, (New-Object Text.UTF8Encoding $false))
    }
}

$savedEnvironment = [Environment]::GetEnvironmentVariables("Process")
$exitCode = 0
try {
    if ($args.Count) {
        throw "Unknown argument(s): $args. Run with -Help for the options."
    }
    if ($Help) {
        Write-Host $Usage
        return
    }

    # Validate and normalize the options before touching anything.
    $buildTypes = @("Release", "RelWithDebInfo", "Debug")
    $match = $buildTypes | Where-Object { $_ -eq $BuildType }
    if (-not $match) {
        throw "-BuildType must be one of $($buildTypes -join ', '), not '$BuildType'."
    }
    $BuildType = $match
    $Targets = @($Targets | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ })
    foreach ($target in $Targets) {
        if (-not $TargetNames.Contains($target)) {
            throw "Unknown target '$target'. Use $($TargetNames.Keys -join ', ')."
        }
    }
    if ($Tests -and $Targets -notcontains "tests" -and $Targets -notcontains "all") {
        $Targets += "tests"
    }
    $wantTests = $Tests -or $Targets -contains "tests"
    if ($Jobs -lt 0) {
        throw "-Jobs must be a positive number."
    }
    if ($Jobs -eq 0) {
        $Jobs = Get-DefaultJobs
    }

    $root = (Get-NativeOutput "git" @("-C", $PSScriptRoot, "rev-parse", "--show-toplevel"))
    if ($LASTEXITCODE -ne 0 -or -not $root) {
        throw "Run this from a Nemyu git checkout (and with git on PATH)."
    }
    $root = [IO.Path]::GetFullPath("$root".Trim())
    $buildPath = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
    $buildPath = [IO.Path]::GetFullPath($buildPath).TrimEnd("\")
    if ("$root\".StartsWith("$buildPath\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "-BuildDir '$BuildDir' must not be the repository or one of its parents."
    }

    # Which steps run. With none, set up, configure when needed and build.
    $anyStep = $Setup -or $Configure -or $Build -or $Package -or $Clean
    $doBuild = $Build -or -not $anyStep
    $doConfigure = $Configure -or $Fresh -or $doBuild
    $forceConfigure = $Configure -or $Fresh
    $needTools = $Setup -or $Clean -or $doConfigure

    Write-Host "Nemyu Windows build" -ForegroundColor Cyan
    Write-Host "  Source:     $root"
    Write-Host "  Build dir:  $buildPath ($BuildType)"
    Write-Host "  Targets:    $($Targets -join ', ')"
    Write-Host "  Jobs:       $Jobs ($([Environment]::ProcessorCount) threads, $([Math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)) GB RAM)"
    $options = @()
    if ($Lto) { $options += "LTO" }
    if ($Sccache) { $options += "sccache" }
    if ($wantTests) { $options += "tests" }
    if ($DryRun) { $options += "dry run" }
    if ($options) { Write-Host "  Options:    $($options -join ', ')" }

    if ($needTools) {
        Write-Section "Setup"
        Initialize-Git
        Initialize-Msvc
        Initialize-BuildTools
        Initialize-Qt
        Initialize-VulkanSdk
        if ($Sccache) { Initialize-Sccache }
    }

    if ($Clean) {
        Write-Section "Clean"
        if (Test-Path (Join-Path $buildPath "build.ninja")) {
            Invoke-Native $CMakeExe @("--build", $buildPath, "--target", "clean") "Cleaning failed"
        } else {
            Write-Host "  $buildPath is not configured; nothing to clean."
        }
    }

    if ($doConfigure) {
        Write-Section "Configure"
        Invoke-Configure $forceConfigure
    }

    if ($doBuild) {
        Write-Section "Build"
        $buildArgs = @("--build", $buildPath, "--parallel", "$Jobs")
        $names = @($Targets | ForEach-Object { $TargetNames[$_] })
        if ($names -notcontains "") {
            $buildArgs += @("--target") + $names
        }
        $started = Get-Date
        Invoke-Native $CMakeExe $buildArgs "The build failed"
        if (-not $DryRun) {
            $elapsed = (Get-Date) - $started
            Write-Host ("  Built in {0:hh\:mm\:ss}. Programs are in {1}" -f $elapsed, (Join-Path $buildPath "bin"))
        }
        if ($Sccache -and -not $DryRun) {
            Get-NativeOutput $SccacheExe @("--show-stats") | Select-Object -First 6 | ForEach-Object { "  $_" }
        }
        if ($Tests) {
            Invoke-Native (Join-Path (Split-Path $CMakeExe -Parent) "ctest.exe") @("--test-dir", $buildPath, "--output-on-failure", "--parallel", "$Jobs") "Some tests failed"
        }
    }

    if ($Package) {
        Write-Section "Package"
        $packageArgs = @{ BuildDir = $buildPath; AllowDirty = [bool]$AllowDirty }
        Write-Host "> package.ps1 -BuildDir `"$buildPath`"$(if ($AllowDirty) { ' -AllowDirty' })" -ForegroundColor DarkGray
        if (-not $DryRun) {
            & (Join-Path $PSScriptRoot "package.ps1") @packageArgs
        }
    }
} catch {
    Write-Host ""
    Write-Host "build.ps1: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    # Leave the calling shell's environment (PATH, the MSVC variables) as it was.
    $current = [Environment]::GetEnvironmentVariables("Process")
    foreach ($name in @($current.Keys)) {
        if (-not $savedEnvironment.Contains($name)) {
            [Environment]::SetEnvironmentVariable($name, $null, "Process")
        }
    }
    foreach ($name in @($savedEnvironment.Keys)) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], "Process")
    }
}
exit $exitCode
