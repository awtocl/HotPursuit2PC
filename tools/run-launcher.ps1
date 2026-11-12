<#
.SYNOPSIS
  Developer helper: run the launcher on a disc image for a fixed number of
  frames, collect its log and screenshots, and convert the screenshots to PNG.

.DESCRIPTION
  Used for the Milestone 1 go/no-go and acceptance runs. Everything is
  written under <repo>\.cache\<RunName>\ (git-ignored). A game window opens
  while it runs and closes by itself.

.EXAMPLE
  pwsh tools/run-launcher.ps1 -RunName boot -Frames 1800 -ShotEvery 300 -Mute
#>
[CmdletBinding()]
param(
  [string]$RunName = 'run',
  [ValidateSet('dev', 'release', 'profile')] [string]$Preset = 'release',
  [string]$Disc = '',
  [string]$Rom = '',
  [int]$Frames = 1800,
  [int]$ShotEvery = 300,
  [string]$Script = '',
  [string]$Size = '640x448',
  [int]$Res = 1,
  [string[]]$Extra = @(),
  [switch]$Mute,
  [switch]$Unlimited,
  [switch]$Trace,
  [switch]$KeepData,
  [int]$TimeoutSeconds = 0
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo ".cache\build\$Preset\engine\HotPursuit2.exe"
if (-not (Test-Path $exe)) { throw "Launcher not built: $exe (run tools/build.ps1 -Preset $Preset)" }
if ($Rom) {
  if (-not (Test-Path -LiteralPath $Rom)) { throw "ROM folder not found: $Rom" }
} else {
  if (-not $Disc) { $Disc = Join-Path $repo '.cache\p4a\hp2.iso' }
  if (-not (Test-Path $Disc)) { throw "Disc image not found: $Disc (hp2pc-cli image --rom ... --out ...)" }
}

$run = Join-Path $repo ".cache\$RunName"
$data = Join-Path $run 'data'
$shots = Join-Path $run 'shots'
New-Item -ItemType Directory -Force -Path $data, $shots | Out-Null
# Start each run with an empty screenshot folder: move old pictures aside rather than delete them.
$old = Get-ChildItem $shots -File -ErrorAction SilentlyContinue
if ($old) {
  $aside = Join-Path $run ("shots-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
  New-Item -ItemType Directory -Force -Path $aside | Out-Null
  $old | Move-Item -Destination $aside
}

$source = if ($Rom) { @('--rom', "`"$((Resolve-Path -LiteralPath $Rom).Path)`"") } else { @('--disc', "`"$Disc`"") }
$a = $source + @('--data', "`"$data`"", '--shots', "`"$shots`"", '--shot-every', $ShotEvery,
       '--frames', $Frames, '--size', $Size, '--res', $Res)
if ($Script) { $a += @('--script', $Script) }
$a += @('--frametimes', "`"$(Join-Path $run 'frametimes.csv')`"")
if ($Trace) { $a += @('--trace', "`"$(Join-Path $run 'trace')`"") }
if ($Extra) { $a += $Extra }
if ($Mute) { $a += '--mute' }
if ($Unlimited) { $a += '--unlimited' }
if (-not $TimeoutSeconds) { $TimeoutSeconds = [int]($Frames / 60 * 3 + 60) }

$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $exe -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { Write-Host 'TIMEOUT - stopping the launcher'; $p.Kill(); $p.WaitForExit() }
Write-Host ("exit code: {0}   wall time: {1:N1} s" -f $p.ExitCode, $sw.Elapsed.TotalSeconds)

Add-Type -AssemblyName System.Drawing
foreach ($bmp in Get-ChildItem $shots -Filter *.bmp) {
  $img = [System.Drawing.Image]::FromFile($bmp.FullName)
  $img.Save([IO.Path]::ChangeExtension($bmp.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
  $img.Dispose()
}
Write-Host ("screenshots: " + ((Get-ChildItem $shots -Filter *.png).Name -join ' '))
Write-Host "log: $data\launcher.log"
