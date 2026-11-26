<#
.SYNOPSIS
  Scripted test run of the native runtime: build (the code package only when recomp/ changed), run to a game time with
  scripted pad presses, faster than real time by default, and collect the pictures and a short log summary.
  Everything goes to <repo>\.cache\native\test\<date time>\ (git-ignored): at_<s>.png (game frame), shot_<s>.png (window,
  post-processing and settings menu included), native.log.
.PARAMETER Until      game seconds to run (59.94 refreshes per second; the game's own time, independent of the PC's speed)
.PARAMETER Press      HP2_PRESS script, e.g. "start@124-124.3,cross@176-176.3" (game seconds)
.PARAMETER DumpAt     game seconds for game-frame pictures, e.g. "130,176"
.PARAMETER ShotAt     game seconds for window pictures
.PARAMETER Turbo      'max' (default: refreshes as fast as the game allows), a factor like '4', or '1' (real time, with sound)
.PARAMETER TurboUntil game second where the run goes back to real time, 'menu' (the main menu's arrival; default with
                      -FastBoot) or 'none'. Menus animate on real-time timers: scripted menu navigation runs in real time.
.PARAMETER FastBoot   skip the intro and the title (Start pulses until the menus' music starts, ~40 s of game instead of
                      ~150 s); then -Until, -Press, -DumpAt and -ShotAt count from the main menu's arrival
.PARAMETER Grep       extra log patterns for the summary (regex)
.PARAMETER Env        extra environment variables for the run, e.g. @{ HP2_GS_THREADS = '0' }
.EXAMPLE
  pwsh tools/native-test.ps1 -Until 60 -FastBoot -DumpAt "40,55"
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][double]$Until,
  [string]$Press = '',
  [string]$DumpAt = '',
  [string]$ShotAt = '',
  [string]$Turbo = 'max',
  [string]$TurboUntil = '',
  [switch]$FastBoot,
  [switch]$NoBuild,
  [double]$RealSeconds = 0,  # real-time safety limit (default: twice the game time plus a minute)
  [string]$Grep = '',
  [hashtable]$Env = @{}
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$out = Join-Path $repo '.cache\native'
$rom = Join-Path $repo 'ROM\Need for Speed - Hot Pursuit 2 (USA)'
New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not $NoBuild) {
  $build = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Preset release -NoTest 2>&1
  if ($LASTEXITCODE -ne 0) { $build | Select-String -Pattern 'error|FAILED' | Select-Object -First 15; throw 'build failed' }
  $dll = Join-Path $out 'hp2code.dll'
  $newest = Get-ChildItem (Join-Path $repo 'recomp') -Recurse -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not (Test-Path $dll) -or $newest.LastWriteTime -gt (Get-Item $dll).LastWriteTime) {
    & (Join-Path $repo '.cache\build\release\recomp\hp2recomp.exe') package (Join-Path $rom 'SLUS_203.62') $dll | Select-Object -Last 1
  }
  # tests run their own copy, so a game the user has open is not disturbed (a running exe cannot be replaced)
  Copy-Item (Join-Path $repo '.cache\build\release\native\HotPursuit2Native.exe') (Join-Path $out 'HotPursuit2Native-test.exe') -Force
}

$run = Join-Path $out ('test\' + (Get-Date -Format 'yyyy-MM-dd HHmmss'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
$vars = @{ HP2_PRESS = $Press; HP2_DUMP_AT = $DumpAt; HP2_SHOT_AT = $ShotAt; HP2_FAST_BOOT = $(if ($FastBoot) { '1' } else { '' }) }
if ($FastBoot -and -not $TurboUntil) { $TurboUntil = 'menu' }
if ($Turbo -ne '1') { $vars.HP2_TURBO = $Turbo; if ($TurboUntil -and $TurboUntil -ne 'none') { $vars.HP2_TURBO_UNTIL = $TurboUntil } }
foreach ($k in $Env.Keys) { $vars[$k] = [string]$Env[$k] }
$saved = @{}
foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $(if ($vars[$k]) { $vars[$k] } else { $null })) }
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
  Push-Location $repo
  # real-time limit as a safety net: the game-time limit (--until) ends the run
  & (Join-Path $out 'HotPursuit2Native-test.exe') --rom $rom --until $Until --seconds $(if ($RealSeconds -gt 0) { $RealSeconds } else { [math]::Max(60, $Until * 2 + 60) }) 2>&1 | Out-Null
  $code = $LASTEXITCODE
} finally {
  Pop-Location
  foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}
$watch.Stop()

Copy-Item (Join-Path $out 'HotPursuit2Native-test.log') (Join-Path $run 'native.log')
Add-Type -AssemblyName System.Drawing
foreach ($f in Get-ChildItem $out -File | Where-Object { $_.Name -match '^(at|shot)_.*\.bmp$' }) {
  $dest = Join-Path $run $f.Name
  Move-Item $f.FullName $dest -Force
  $b = [Drawing.Bitmap]::new($dest)
  $w = [math]::Min(960, $b.Width); $h = [int]($b.Height * $w / $b.Width)
  $s = [Drawing.Bitmap]::new($b, $w, $h)
  $s.Save([IO.Path]::ChangeExtension($dest, '.png'), [Drawing.Imaging.ImageFormat]::Png)
  $s.Dispose(); $b.Dispose()
}

$log = Join-Path $run 'native.log'
"run: $run  (exit $code, $([math]::Round($watch.Elapsed.TotalSeconds)) s real for $Until s of game)"
$perf = Select-String -Path $log -Pattern '^perf: vblank \d+, ([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
if ($perf) { "speed: refreshes/s min $([math]::Round(($perf | Measure-Object -Minimum).Minimum,1)) avg $([math]::Round(($perf | Measure-Object -Average).Average,1)) max $([math]::Round(($perf | Measure-Object -Maximum).Maximum,1))" }
$pattern = '^(Stopped|Ran |Turbo|Audio|menu|hook |fast boot)' + $(if ($Grep) { "|$Grep" } else { '' })
Select-String -Path $log -Pattern $pattern | Select-Object -First 40 | ForEach-Object { $_.Line.Substring(0, [math]::Min(220, $_.Line.Length)) }
Get-ChildItem $run -Filter *.png | ForEach-Object { "picture: $($_.FullName)" }
