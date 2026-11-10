# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Runs nemyu-cli hidden for a fixed time, then copies its log next to the other outputs.
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$Game,
    [Parameter(Mandatory)] [string]$Config,
    [Parameter(Mandatory)] [string]$Tag,
    [Parameter(Mandatory)] [string]$Out,
    [int]$Seconds = 300,
    [string]$EnvPairs = "",
    [string]$Capture = "",
    [string]$LogPath = (Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'nemyu\log\nemyu_log.txt')
)
# The variables are set in this session for the emulator to inherit, and restored after the run:
# several runs from one script would otherwise inherit the previous run's variables.
$saved = @{}
function Set-RunVariable([string]$Name, [string]$Value) {
    if (-not $saved.ContainsKey($Name)) { $saved[$Name] = [Environment]::GetEnvironmentVariable($Name) }
    [Environment]::SetEnvironmentVariable($Name, $Value)
}
foreach ($pair in ($EnvPairs -split ";")) {
    if ($pair -match "^(\w+)=(.*)$") { Set-RunVariable $Matches[1] $Matches[2] }
}
Set-RunVariable "EDEN_NX2_HIDE_WINDOW" "1"
if ($Capture -ne "") {
    Remove-Item -Recurse -Force $Capture -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $Capture | Out-Null
    Set-RunVariable "EDEN_NX2_FRAME_CAPTURE" $Capture
}
# Builds before the Nemyu log rename write eden_log.txt: clear both so a stale log is never read.
$logDir = Split-Path $LogPath
Remove-Item $LogPath, (Join-Path $logDir "eden_log.txt") -ErrorAction SilentlyContinue
try {
    $p = Start-Process -FilePath $Exe -ArgumentList @("-c", "`"$Config`"", "-g", "`"$Game`"") `
        -WorkingDirectory (Split-Path $Exe) -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $Out "$Tag.stdout.txt") -RedirectStandardError (Join-Path $Out "$Tag.stderr.txt")
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}
$start = Get-Date
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds 1
}
$exited = $p.HasExited
if (-not $exited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2
$log = @($LogPath, (Join-Path $logDir "eden_log.txt")) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $log) {
    throw "the run wrote no log in $logDir"
}
Copy-Item $log (Join-Path $Out "$Tag.log") -Force
"tag=$Tag exited_by_itself=$exited elapsed=$([int]((Get-Date) - $start).TotalSeconds)s"
