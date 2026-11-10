# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

$ErrorActionPreference = 'Stop'
# Windows PowerShell's progress bar slows Invoke-WebRequest and Expand-Archive down several times.
$ProgressPreference = 'SilentlyContinue'

function Save-Download([string]$Uri, [string]$OutFile, [string]$Sha256 = '') {
    if ($Sha256 -and (Test-Path -LiteralPath $OutFile) -and
        (Get-FileHash -Algorithm SHA256 -LiteralPath $OutFile).Hash -eq $Sha256) { return }
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
    if ($Sha256 -and (Get-FileHash -Algorithm SHA256 -LiteralPath $OutFile).Hash -ne $Sha256) {
        throw "$Uri does not have the expected SHA-256."
    }
}

function Publish-Package([string]$File, [string]$Url) {
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Method Put -Uri $Url -InFile $File `
                -Headers @{ 'JOB-TOKEN' = $env:CI_JOB_TOKEN } -TimeoutSec 900
            if ($response.StatusCode -ne 201) { throw "Package upload returned HTTP $($response.StatusCode)." }
            return
        } catch {
            if ($attempt -eq 3) { throw }
            Write-Warning "Package upload attempt $attempt failed; retrying."
            Start-Sleep -Seconds 5
        }
    }
}

$sccache = $null
$buildTimedOut = $false
try {
    $root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    Set-Location -LiteralPath $root
    if (-not $env:CI_COMMIT_TAG -or -not $env:CI_JOB_TOKEN) {
        throw 'Run this script in a GitLab tag pipeline with CI_JOB_TOKEN available.'
    }
    foreach ($variable in 'CI_API_V4_URL', 'CI_PROJECT_ID', 'CI_COMMIT_SHA', 'CI_PIPELINE_ID', 'CI_JOB_ID') {
        if (-not [Environment]::GetEnvironmentVariable($variable)) { throw "$variable is required." }
    }
    $started = if ($env:CI_JOB_STARTED_AT) { [DateTimeOffset]::Parse($env:CI_JOB_STARTED_AT) } else { [DateTimeOffset]::Now }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

    # CPM applies its patches with GNU patch, which it looks for next to the git CMake finds. This
    # runner only has MinGit, which has none, so take it from a portable Git for Windows. Its usr\bin
    # stays off PATH (its link.exe would shadow MSVC's): CMake gets the path.
    $gitVersion = '2.51.2'
    $gitDir = Join-Path $root '.cache\ci-git'
    New-Item -ItemType Directory -Path $gitDir -Force | Out-Null
    $gitArchive = Join-Path $gitDir 'PortableGit.7z.exe'
    Save-Download "https://github.com/git-for-windows/git/releases/download/v$gitVersion.windows.1/PortableGit-$gitVersion-64-bit.7z.exe" $gitArchive 'F5764D546FF9A2511B50EC4E20424C5F4669DE1695ABC3FA4128E7F7D4A7B2CD'
    # A windowed self-extractor, which & would not wait for.
    $extract = Start-Process -FilePath $gitArchive -ArgumentList '-y', "-o$gitDir" -NoNewWindow -PassThru -Wait
    if ($extract.ExitCode -ne 0) { throw 'Could not extract Git for Windows.' }
    $patch = Join-Path $gitDir 'usr\bin\patch.exe'
    & $patch --version
    if ($LASTEXITCODE -ne 0) { throw 'patch.exe from Git for Windows could not run.' }

    $tools = Join-Path $root '.cache\ci-tools'
    python -m venv $tools
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the build tools environment.' }
    $scripts = Join-Path $tools 'Scripts'
    $python = Join-Path $scripts 'python.exe'
    & $python -m pip install --retries 3 --timeout 120 cmake==3.31.6 ninja==1.13.2 aqtinstall==3.3.0
    if ($LASTEXITCODE -ne 0) { throw 'Could not install the build tools.' }
    $cmake = Join-Path $scripts 'cmake.exe'
    $ninja = Join-Path $scripts 'ninja.exe'
    & $cmake --version
    if ($LASTEXITCODE -ne 0) { throw 'The installed CMake could not run.' }
    & $ninja --version
    if ($LASTEXITCODE -ne 0) { throw 'The installed Ninja could not run.' }

    # Compiler cache, kept between pipelines in .sccache by the job's GitLab cache. A full build
    # takes the 2-CPU runner over 2 hours; with the cache, only changed sources compile.
    $sccacheVersion = '0.19.0'
    $sccacheDir = Join-Path $root '.cache\ci-sccache'
    New-Item -ItemType Directory -Path $sccacheDir -Force | Out-Null
    $archive = Join-Path $sccacheDir 'sccache.zip'
    Save-Download "https://github.com/mozilla/sccache/releases/download/v$sccacheVersion/sccache-v$sccacheVersion-x86_64-pc-windows-msvc.zip" $archive '28BE8C61F6B2C89B9EB2869F6AF7F78E6F992E78A8ABAE0CE855FCC129CBEAC1'
    Expand-Archive -LiteralPath $archive -DestinationPath $sccacheDir -Force
    $sccache = Get-ChildItem -LiteralPath $sccacheDir -Recurse -File -Filter sccache.exe | Select-Object -First 1 -ExpandProperty FullName
    if (-not $sccache) { throw 'The sccache archive did not contain sccache.exe.' }
    $env:SCCACHE_DIR = Join-Path $root '.sccache'
    $env:SCCACHE_CACHE_SIZE = '2G'
    $env:SCCACHE_IDLE_TIMEOUT = '0'
    # Started here rather than by the first compile, so stopping the build below leaves it running.
    & $sccache --start-server
    if ($LASTEXITCODE -ne 0) { throw 'sccache could not start.' }

    $qtRoot = Join-Path $root '.cache\qt-msvc'
    $qt = Join-Path $qtRoot '6.10.2\msvc2022_64'
    if (-not (Test-Path "$qt\.nemyu-install-complete") -or -not (Test-Path "$qt\bin\windeployqt.exe") -or
        -not (Test-Path "$qt\lib\cmake\Qt6Charts\Qt6ChartsConfig.cmake")) {
        & $python -m aqt install-qt windows desktop 6.10.2 win64_msvc2022_64 --outputdir $qtRoot --modules qtcharts
        if ($LASTEXITCODE -ne 0) { throw 'Could not install Qt.' }
        New-Item -ItemType File -Path "$qt\.nemyu-install-complete" -Force | Out-Null
    }

    # The SDK installer needs Windows PowerShell's Get-WmiObject.
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'install-vulkan-sdk.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Could not install the Vulkan SDK.' }
    $env:VULKAN_SDK = 'C:\VulkanSDK\1.4.341.1'
    if (-not (Test-Path "$env:VULKAN_SDK\Bin\glslangValidator.exe")) {
        throw 'Vulkan SDK tools were not found.'
    }

    . (Join-Path $PSScriptRoot 'load-msvc-env.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Could not load the MSVC environment.' }
    Get-Command cl -ErrorAction Stop | Out-Null
    $env:PATH = "$scripts;$qt\bin;$env:VULKAN_SDK\Bin;$env:PATH"

    & $cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYUZU_TESTS=OFF -DENABLE_QT=ON -DYUZU_CMD=ON -DYUZU_USE_BUNDLED_QT=OFF "-DQt6_DIR=$qt\lib\cmake\Qt6" "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja" "-DPATCH_EXE:FILEPATH=$patch" "-DCMAKE_C_COMPILER_LAUNCHER=$sccache" "-DCMAKE_CXX_COMPILER_LAUNCHER=$sccache"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

    # GitLab ends the job at its timeout and then saves no cache. Stop the build 20 minutes before
    # that: the job fails, the objects compiled so far are cached, and a retry carries on from them.
    $timeout = if ($env:CI_JOB_TIMEOUT) { [int]$env:CI_JOB_TIMEOUT } else { 3 * 3600 }
    $deadline = $started.AddSeconds($timeout - 20 * 60)
    $build = Start-Process -FilePath $cmake -ArgumentList '--build', 'build', '--target', 'yuzu', 'yuzu-cmd', '--parallel', '2' -NoNewWindow -PassThru
    $null = $build.Handle # Windows PowerShell only fills in ExitCode once the handle was read
    $wait = [int][Math]::Max(0, ($deadline - [DateTimeOffset]::Now).TotalMilliseconds)
    if (-not $build.WaitForExit($wait)) {
        $buildTimedOut = $true
        & taskkill.exe /PID $build.Id /T /F | Out-Null
        throw 'The build ran out of time. Retry the job: it carries on from the objects cached so far.'
    }
    if ($build.ExitCode -ne 0) { throw 'CMake build failed.' }

    & (Join-Path $PSScriptRoot 'publish-gitlab-release.ps1') -Tag $env:CI_COMMIT_TAG -BuildDir build -TagAlreadyExists -PackageOnly
    $out = Join-Path $root 'out\windows-x86_64'
    New-Item -ItemType Directory -Path $out -Force | Out-Null
    $filename = "nemyu-$env:CI_COMMIT_TAG-windows-x86_64.zip"
    $zip = Join-Path $root "build\release\$filename"
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    $checksum = Join-Path $out "$filename.sha256"
    $utf8 = New-Object Text.UTF8Encoding $false
    [IO.File]::WriteAllText($checksum, "$hash  $filename`n", $utf8)
    $packageUrl = "$env:CI_API_V4_URL/projects/$env:CI_PROJECT_ID/packages/generic/nemyu-windows-x86_64/$env:CI_COMMIT_SHA-$env:CI_JOB_ID"
    Publish-Package $zip "$packageUrl/$filename"
    Publish-Package $checksum "$packageUrl/$filename.sha256"
    $manifest = @{
        tag = $env:CI_COMMIT_TAG; commit = $env:CI_COMMIT_SHA; pipeline_id = $env:CI_PIPELINE_ID
        assets = @(
            @{ name = $filename; url = "$packageUrl/$filename"; link_type = 'package' }
            @{ name = "$filename.sha256"; url = "$packageUrl/$filename.sha256"; link_type = 'other' }
        )
    } | ConvertTo-Json -Depth 4
    [IO.File]::WriteAllText((Join-Path $out 'release.json'), $manifest, $utf8)
    "Windows package uploaded: $packageUrl/$filename"
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    if ($buildTimedOut) { exit 75 }
    exit 1
} finally {
    # The cache is archived after this script, so sccache must have written everything by then.
    if ($sccache) {
        & $sccache --show-stats
        & $sccache --stop-server | Out-Null
    }
}
exit 0
