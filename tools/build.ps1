<#
.SYNOPSIS
  Configure, build and test HotPursuit2PC with the MSVC toolchain.

.DESCRIPTION
  Finds Visual Studio / Build Tools with vswhere, loads the x64 compiler
  environment into this process, then runs CMake presets. All build output,
  compiler temp files and caches stay inside <repo>\.cache (git-ignored);
  nothing is written elsewhere and nothing is installed.

.EXAMPLE
  pwsh tools/build.ps1                 # configure + build + test, preset "dev"
  pwsh tools/build.ps1 -Preset release -NoTest
#>
[CmdletBinding()]
param(
  [ValidateSet('dev', 'release')] [string]$Preset = 'dev',
  [switch]$NoTest,
  [switch]$Fresh,
  [string]$TestFilter = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# --- locate the toolchain ---------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio 2022 Build Tools (C++ workload)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the x64 C++ tools was found." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }

# --- keep temp files and caches inside the repo ----------------------------
$tmp = Join-Path $repo '.cache\tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TEMP = $tmp
$env:TMP = $tmp

# --- import the MSVC environment into this PowerShell process ---------------
$envDump = & cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed" }
foreach ($line in $envDump) {
  $i = $line.IndexOf('=')
  if ($i -gt 0) { Set-Item -Path "Env:$($line.Substring(0, $i))" -Value $line.Substring($i + 1) }
}
$env:TEMP = $tmp
$env:TMP = $tmp
foreach ($tool in 'cl', 'cmake', 'ninja') {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found after loading the MSVC environment" }
}

# --- configure / build / test ------------------------------------------------
Push-Location $repo
try {
  $cfg = @('--preset', $Preset)
  if ($Fresh) { $cfg += '--fresh' }
  & cmake @cfg
  if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
  & cmake --build --preset $Preset
  if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
  if (-not $NoTest) {
    $t = @('--preset', $Preset)
    if ($TestFilter) { $t += @('-R', $TestFilter) }
    & ctest @t
    if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
  }
}
finally { Pop-Location }
