<#
.SYNOPSIS
  Developer helper for the native runtime (ADR 0012): build (release), rebuild
  the code package for its state layout, run it for a few seconds and show
  the end of its log. Everything stays in <repo>\.cache\native (git-ignored).
.EXAMPLE
  pwsh tools/native-run.ps1 -Seconds 20
#>
[CmdletBinding()]
param([double]$Seconds = 20, [int]$Tail = 40, [switch]$NoPackage)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$out = Join-Path $repo '.cache\native'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$build = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Preset release -NoTest 2>&1
if ($LASTEXITCODE -ne 0) { $build | Select-String -Pattern 'error|FAILED' | Select-Object -First 15; throw 'build failed' }
$rom = Join-Path $repo 'ROM\Need for Speed - Hot Pursuit 2 (USA)'
if (-not $NoPackage) {
  & (Join-Path $repo '.cache\build\release\recomp\hp2recomp.exe') package (Join-Path $rom 'SLUS_203.62') (Join-Path $out 'hp2code.dll') | Select-Object -Last 1
}
Copy-Item (Join-Path $repo '.cache\build\release\native\HotPursuit2Native.exe') $out -Force
& (Join-Path $out 'HotPursuit2Native.exe') --rom $rom --seconds $Seconds 2>&1 | Select-Object -Last $Tail
