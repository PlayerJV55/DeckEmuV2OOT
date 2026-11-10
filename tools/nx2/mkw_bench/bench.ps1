# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Mario Kart World race benchmark for nemyu-cli. A key script enters Grand Prix with the default
# choices (Mushroom Cup, Mario Bros. Circuit) and holds accelerate; steering assist keeps the kart
# on the track. Prints the boot and in-race frame times (nx2:vi2 600-present windows), the GPU load
# and throttling, and the power source (on battery a laptop caps the GPU and CPU).
#
#   .\bench.ps1 -Tag base -Game "<dump>\exefs\main" -Config "<your config>.ini" [-EnvPairs "A=1;B=2"]
#
# -Soak keeps tapping A so the Grand Prix goes on to its next races (use -Seconds 1200 or more).
# -Idle presses nothing: the game stays on its title screen, where its movie loops.
# -LogPath points at the log of a portable build (its user folder sits next to the exe).
# -QuitAtFrame <n> ends the run at that presented frame the way closing the window does, so the
# emulator shuts down and saves its caches (without it the run is killed after -Seconds).
#
# The config is copied with player 1 moved to the keyboard (the key script presses keys).
param(
    [Parameter(Mandatory)] [string]$Tag,
    [Parameter(Mandatory)] [string]$Game,
    [Parameter(Mandatory)] [string]$Config,
    [string]$Exe = "$PSScriptRoot\..\..\..\build\bin\nemyu-cli.exe",
    [string]$EnvPairs = "",
    [int]$Seconds = 300,
    [string]$Capture = "",
    [string]$Out = "$PSScriptRoot\out",
    [int]$QuitAtFrame = 0,
    [string]$LogPath = "",
    [switch]$Soak,
    [switch]$Idle
)
$python = (Get-Command python -ErrorAction Stop).Source
Add-Type -AssemblyName System.Windows.Forms
function Get-PowerSource {
    switch ([System.Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus.ToString()) {
        "Online" { "AC" }
        "Offline" { "battery" }
        default { "unknown" }
    }
}
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out -ErrorAction Stop).Path
$Config = (Resolve-Path -LiteralPath $Config -ErrorAction Stop).Path
$Game = (Resolve-Path -LiteralPath $Game -ErrorAction Stop).Path
$Exe = (Resolve-Path -LiteralPath $Exe -ErrorAction Stop).Path
$keysConfig = Join-Path $Out "keys.ini"
$script = Join-Path $Out "race_keys.txt"
$mode = if ($Soak) { "soak" } else { "race" }
& $python -I "$PSScriptRoot\make_keys.py" $Config $keysConfig $script $mode $QuitAtFrame | Out-Null

$csv = Join-Path $Out "$Tag.gpu.csv"
Remove-Item $csv -ErrorAction SilentlyContinue
$runStart = Get-Date
$powerBefore = Get-PowerSource
$job = Start-Job -ArgumentList $csv, $Seconds -ScriptBlock {
    param($path, $seconds)
    $query = "utilization.gpu,clocks.gr,power.draw,memory.used,clocks_event_reasons.active,pstate,power.limit," +
        "enforced.power.limit"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $seconds + 5) {
        $line = & nvidia-smi "--query-gpu=$query" --format=csv,noheader,nounits
        "{0:F0},{1}" -f $sw.Elapsed.TotalSeconds, $line | Out-File -Append -Encoding ascii $path
        Start-Sleep -Milliseconds 950
    }
}
# A portable build (a user folder next to the exe) logs there instead of in %APPDATA%.
$logArgs = @{}
if ($LogPath -ne "") { $logArgs.LogPath = $LogPath }
$pairs = if ($Idle) { "" } else { "EDEN_NX2_KEY_SCRIPT=@$script" }
if ($EnvPairs -ne "") { $pairs = (@($pairs, $EnvPairs) | Where-Object { $_ }) -join ";" }
& "$PSScriptRoot\run.ps1" -Exe $Exe -Game $Game -Config $keysConfig -Seconds $Seconds -Tag $Tag `
    -EnvPairs $pairs -Capture $Capture -Out $Out @logArgs
Stop-Job $job; Remove-Job $job -Force
$powerAfter = Get-PowerSource
# Windows logs every power source change as Kernel-Power event 105.
$changes = @(Get-WinEvent -ErrorAction SilentlyContinue -FilterHashtable @{
        LogName = "System"; ProviderName = "Microsoft-Windows-Kernel-Power"; Id = 105; StartTime = $runStart
    } | Sort-Object TimeCreated | ForEach-Object {
        $ac = (([xml]$_.ToXml()).Event.EventData.Data | Where-Object Name -eq "AcOnline").'#text'
        $source = if ($ac -eq "true") { "AC" } else { "battery" }
        "{0:HH:mm:ss} {1}" -f $_.TimeCreated, $source
    })

& $python -I "$PSScriptRoot\racestats.py" (Join-Path $Out "$Tag.log")
# A guest abort (SetTerminateResult, then svcBreak) leaves the game on a still screen at 60 fps.
$aborts = @(Select-String -Path (Join-Path $Out "$Tag.log") -Pattern "SetTerminateResult")
"Guest aborts: $($aborts.Count)" + $(if ($aborts.Count) { " (first at $($aborts[0].Line.Substring(0, 14).Trim()))" } else { "" })
$rows = Get-Content $csv | ForEach-Object {
    $p = $_ -split ','
    $reasons = if ($p[5] -match '0x([0-9a-f]+)') { [Convert]::ToInt64($Matches[1], 16) } else { 0 }
    [pscustomobject]@{ t = [double]$p[0]; util = [double]$p[1]; clk = [double]$p[2]; w = [double]$p[3]; mem = [double]$p[4]
        reasons = $reasons; pstate = "$($p[6])".Trim(); limit = "$($p[7])".Trim() -as [double]
        enforced = "$($p[8])".Trim() -as [double] }
}
$race = @($rows | Where-Object { $_.t -ge 175 })
"GPU in race (nvidia-smi): util {0:N0}%  clock {1:N0} MHz  power {2:N1} W  VRAM {3:N0} MiB" -f `
    ($race | Measure-Object util -Average).Average, ($race | Measure-Object clk -Average).Average, `
    ($race | Measure-Object w -Average).Average, ($race | Measure-Object mem -Maximum).Maximum
if ($race.Count -gt 0) {
    # clocks_event_reasons bits (NVML): 0x4 software power cap, 0x20 and 0x40 thermal slowdown, 0x8 and
    # 0x80 hardware slowdown and power brake. 0x400 (reliability) also shows at full clock, so it is left out.
    $causes = [ordered]@{ "power cap" = 0x4; "thermal" = 0x60; "hardware slowdown" = 0x88 }
    $throttled = foreach ($name in $causes.Keys) {
        $mask = $causes[$name]
        "{0} {1:N0}%" -f $name, (100 * @($race | Where-Object { $_.reasons -band $mask }).Count / $race.Count)
    }
    $pstates = $race | Group-Object pstate | Sort-Object Name | ForEach-Object {
        "{0} {1:N0}%" -f $_.Name, (100 * $_.Count / $race.Count)
    }
    $enforced = $race | Where-Object { $null -ne $_.enforced } | Measure-Object enforced -Minimum -Maximum
    $limit = $race | Where-Object { $null -ne $_.limit } | Measure-Object limit -Maximum
    $watts = "no power limit reported"
    if ($enforced.Count) { $watts = "enforced power limit {0:N0}-{1:N0} W" -f $enforced.Minimum, $enforced.Maximum }
    if ($enforced.Count -and $limit.Count) { $watts += " of {0:N0} W" -f $limit.Maximum }
    "GPU throttling in race: {0} of the samples; {1}; {2}" -f ($throttled -join ", "), ($pstates -join ", "), $watts
}

$power = @("Power source: $powerBefore before the run, $powerAfter after, " +
    $(if ($changes.Count) { "changes at $($changes -join ', ')" } else { "no change logged" }))
if ($powerBefore -eq "battery" -or $powerAfter -eq "battery" -or @($changes -match "battery$").Count) {
    $power += "WARNING: the laptop was on battery during this run, which caps the GPU and CPU: these frame " +
        "times are not comparable, plug it in and run again"
}
$power | Set-Content -Encoding ascii (Join-Path $Out "$Tag.power.txt")
$power
