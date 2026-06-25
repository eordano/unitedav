# SPDX-License-Identifier: Apache-2.0
# Run the Windows standalone Player inside a Windows VM (pushed in by
# tests/unity/push-windows.sh). Launches the Player headlessly; the example app's
# IntegrationBattery self-tests open/play/pause/seek/loop/volume/mute/events/
# texture + HW backend, writes a JSON result, and sets the exit code. HW is forced
# with UAV_HWDECODE=auto + an H.264 fixture (D3D11VA, CUDA/NVDEC fallback).
#
# Usage (inside the guest, PowerShell):
#   pwsh tests\unity\run-windows.ps1 `
#     -PlayerExe C:\uav\Player\UnitedAVEndToEnd.exe `
#     -Fixture   C:\uav\media\mp4__h264__aac.mp4
[CmdletBinding()]
param(
  [string]$PlayerExe = "",
  [string]$Fixture   = "",
  [string]$OutDir    = "C:\uav\out",
  [string]$HwDecode  = "auto"
)

$ErrorActionPreference = "Stop"

function UavLog($m)  { Write-Host "[unity] $m" }
function UavSkip($m) { Write-Host "UNITY-SKIP: $m";    exit 0 }
function UavFail($m) { Write-Host "UNITY-FAIL: $m";    exit 1 }
function UavSection($m) { Write-Host ""; Write-Host "========== $m ==========" }

UavSection "Run Windows Player (Windows VM)"

# Resolve the Player .exe.
if ([string]::IsNullOrEmpty($PlayerExe)) {
  $candidates = @(
    "C:\uav\Player\$($env:UAV_PLAYER_NAME).exe",
    "C:\uav\Player\UnitedAVEndToEnd.exe"
  )
  foreach ($c in $candidates) { if (Test-Path $c) { $PlayerExe = $c; break } }
  if ([string]::IsNullOrEmpty($PlayerExe)) {
    $found = Get-ChildItem -Path "C:\uav\Player" -Filter *.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $PlayerExe = $found.FullName }
  }
}
if ([string]::IsNullOrEmpty($PlayerExe) -or -not (Test-Path $PlayerExe)) {
  UavSkip "Windows Player .exe not found (looked under C:\uav\Player). Cross-build it and push it in with tests/unity/push-windows.sh."
}
UavLog "player exe      : $PlayerExe"

# Confirm the native plugin shipped beside the Player (Unity puts plugins under
# <Player>_Data\Plugins\x86_64\). Absence => DllNotFound at runtime.
$dataDir = [System.IO.Path]::ChangeExtension($PlayerExe, $null).TrimEnd('.') + "_Data"
$dllHit  = $null
if (Test-Path $dataDir) {
  $dllHit = Get-ChildItem -Path $dataDir -Filter "UnitedAV.dll" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
}
if ($dllHit) {
  UavLog "native plugin   : $($dllHit.FullName)"
} else {
  UavLog "native plugin   : UnitedAV.dll NOT found under $dataDir — the Player will DllNotFound. Ensure the .dll + .meta were gathered before the cross-build."
}

# Resolve the H.264 fixture (forces the HW/D3D11VA path).
if ([string]::IsNullOrEmpty($Fixture)) {
  $fixCandidates = @(
    "C:\uav\media\mp4__h264__aac.mp4",
    "C:\uav\media\mov__h264__aac.mov",
    "C:\uav\media\mpegts__h264__mp3.ts"
  )
  foreach ($c in $fixCandidates) { if (Test-Path $c) { $Fixture = $c; break } }
}
if ([string]::IsNullOrEmpty($Fixture) -or -not (Test-Path $Fixture)) {
  UavSkip "No H.264 fixture found (looked under C:\uav\media). push-windows.sh should copy mp4__h264__aac.mp4 in."
}
UavLog "h264 fixture    : $Fixture"
UavLog "UAV_HWDECODE    : $HwDecode"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$playerLog  = Join-Path $OutDir "player.log"
$resultJson = Join-Path $OutDir "battery.json"

# Export the env the IntegrationBattery reads. It writes its BatteryReport to
# UAV_E2E_REPORT (see unity/Samples~/EndToEnd/IntegrationBattery.ResolveReportPath).
$env:UAV_HWDECODE       = $HwDecode
$env:UAV_TEST_MEDIA_DIR = Split-Path -Parent $Fixture
$env:UAV_H264_FIXTURE   = $Fixture
$env:UAV_E2E_REPORT     = $resultJson

UavLog "running battery (one run) -> $resultJson"
# -batchmode so the Player self-tests + quits; D3D11 still gives a real GfxDevice.
# The battery calls Application.Quit(exitCode): 0 pass / 1 fail / 2 skip-only.
$proc = Start-Process -FilePath $PlayerExe `
  -ArgumentList @("-batchmode", "-logFile", $playerLog) `
  -NoNewWindow -PassThru -Wait
$rc = $proc.ExitCode

# ---- report (mirrors _report-run.sh; same BatteryReport contract) ----
UavSection "Run report: windows"
UavLog "player exit code: $rc"
UavLog "player log      : $playerLog"

# Battery report.
$overall = $null; $exitcode = $null; $pass = $null; $fail = $null; $skip = $null; $backend = $null
if (Test-Path $resultJson) {
  UavLog "battery report  : $resultJson"
  $raw = Get-Content -Raw -Path $resultJson
  $raw -split "`n" | ForEach-Object { Write-Host "  [battery] $_" }
  try {
    $obj      = $raw | ConvertFrom-Json
    $overall  = $obj.overallPass
    $exitcode = $obj.exitCode
    $pass     = $obj.passCount
    $fail     = $obj.failCount
    $skip     = $obj.skipCount
    $backend  = $obj.hwBackend
  } catch {
    UavLog "battery report  : present but not valid JSON ($($_.Exception.Message))"
  }
} else {
  UavLog "battery report  : not written (set UAV_E2E_REPORT; the Player may have crashed before Finalize)"
}

# HW backend: prefer the JSON value, fall back to grepping the Player log.
if ([string]::IsNullOrEmpty($backend) -and (Test-Path $playerLog)) {
  $m = Select-String -Path $playerLog -Pattern '\[uav\] hardware decode enabled: (vaapi|videotoolbox|cuda|d3d11va)' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($m) { $backend = ($m.Matches[0].Groups[1].Value) }
}
if (-not [string]::IsNullOrEmpty($backend)) {
  UavLog "HW backend      : $backend  (`"[uav] hardware decode enabled: $backend`")"
} else {
  UavLog "HW backend      : NOT confirmed (no hwBackend in JSON, no '[uav] hardware decode enabled' line in the log)"
}

# Effective exit: the battery's own exitCode is authoritative; else the process rc.
$effExit = if ($null -ne $exitcode) { [int]$exitcode } else { [int]$rc }

# Verdict (mirrors _report-run.sh): 0 green / 2 partial / else failed.
$verdict = "failed"
switch ($effExit) {
  0 {
      if ($overall -eq $true)                                    { $verdict = "green" }
      elseif (($pass -ge 1) -and ($fail -eq 0) -and ($skip -eq 0)) { $verdict = "green" }
      else                                                       { $verdict = "partial" }
    }
  2 { $verdict = "partial" }
  default { $verdict = "failed" }
}

UavLog "tallies         : pass=$pass fail=$fail skip=$skip overallPass=$overall battery_exit=$exitcode"
UavLog "VERDICT         : $verdict"
switch ($verdict) {
  "green"   { Write-Host "UNITY-RUN: windows green (pass=$pass)";              exit 0 }
  "partial" { Write-Host "UNITY-RUN: windows partial (pass=$pass skip=$skip)"; exit 0 }
  default   { Write-Host "UNITY-FAIL: windows run failed (fail=$fail exit=$effExit)"; exit 1 }
}
