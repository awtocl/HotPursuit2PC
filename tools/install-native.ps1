<#
.SYNOPSIS
  Builds the native game (no emulator) and its launcher, and puts them in a game folder ready to play from YOUR OWN
  disc of Need for Speed: Hot Pursuit 2 (PS2, NTSC-U, SLUS-20362).
.DESCRIPTION
  No game files are in this repository or downloaded by this script. The script:
    1. checks the disc folder given with -Rom (skipped without it: the launcher asks for your disc);
    2. the first time only, downloads and builds LLVM (the recompiler's code generator; 15-40 minutes);
    3. builds the project (release) unless -NoBuild;
    4. with -Rom: recompiles the game's program from YOUR SLUS_203.62 into hp2code.dll (on this PC; it stays here);
    5. copies the launcher (HotPursuit2Launcher.exe), the game (HotPursuit2Native.exe) and the recompiler (hp2recomp.exe)
       into -Destination, then starts the launcher (unless -NoLaunch).
  The launcher's first-run setup finds your disc (DVD drive or mounted .iso), copies and checks it, and prepares the
  game. Your disc folder is only read. Run the script again after updating the project (git pull).
  Requirements and a step-by-step guide: docs/GETTING-STARTED.md.
.EXAMPLE
  pwsh tools/install-native.ps1                 # build, then the launcher asks for your disc
.EXAMPLE
  pwsh tools/install-native.ps1 -Rom "D:\My dumps\Need for Speed - Hot Pursuit 2 (USA)" -NoLaunch
#>
[CmdletBinding()]
param(
  [string]$Rom = '',
  [string]$Destination = '',  # default: <repo>\game-native
  [switch]$NoBuild,
  [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Destination) { $Destination = Join-Path $repo 'game-native' }
$watch = [Diagnostics.Stopwatch]::StartNew()

function Step([string]$text) { Write-Host ''; Write-Host "==> $text" -ForegroundColor Cyan }

# ---- 0. the C++ build tools (needed by every build step) ----
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $NoBuild -and -not $vs) {
  throw "The C++ build tools were not found. Install 'Build Tools for Visual Studio 2022' with the 'Desktop development with C++' workload (see docs/GETTING-STARTED.md, step 1), then run this script again."
}

# ---- 1. your disc (optional here: the launcher asks for it) ----
Step '1/5  Your disc'
if ($Rom) {
  $Rom = (Resolve-Path -LiteralPath $Rom).Path
  foreach ($need in 'SYSTEM.CNF', 'SLUS_203.62', 'NFSHP2') {
    if (-not (Test-Path -LiteralPath (Join-Path $Rom $need))) {
      throw "'$Rom' does not look like the contents of the disc: $need is missing. Choose the folder that holds SYSTEM.CNF, SLUS_203.62 and NFSHP2."
    }
  }
  $manifest = Get-Content (Join-Path $repo 'converter\data\slus20362_rom_manifest.tsv') | Where-Object { $_ -like 'SLUS_203.62*' }
  $expected = ($manifest -split "`t")[2]
  $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Rom 'SLUS_203.62')).Hash.ToLowerInvariant()
  if ($actual -ne $expected) {
    throw "SLUS_203.62 is not the supported version (SHA-256 $actual). Only the North American release SLUS-20362 v1.00 is supported."
  }
  Write-Host "OK: $Rom (SLUS_203.62 is the supported version)"
} else {
  Write-Host 'Later: the launcher asks for your disc when it starts.'
}

if (-not $NoBuild) {
  # ---- 2. LLVM, the recompiler's code generator (once) ----
  Step '2/5  LLVM (the recompiler''s code generator)'
  $llvm = Get-ChildItem (Join-Path $repo '.cache\third_party') -Directory -Filter 'llvm-*-install' -ErrorAction SilentlyContinue |
          Where-Object { Test-Path (Join-Path $_.FullName 'lib\cmake\llvm\LLVMConfig.cmake') } | Select-Object -First 1
  if ($llvm) {
    Write-Host "OK: already built ($($llvm.Name))"
  } else {
    Write-Host 'First time only: downloading the LLVM source (about 171 MB) and building it. This takes 15-40 minutes and'
    Write-Host 'several GB of disk space; nothing is installed on the system. Later runs skip this step.'
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'fetch-llvm.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Downloading LLVM failed (see the messages above).' }
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build-llvm.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Building LLVM failed (see the messages above).' }
  }

  # ---- 3. the project ----
  Step '3/5  Building HotPursuit2PC (a few minutes)'
  $build = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Preset release -NoTest 2>&1
  if ($LASTEXITCODE -ne 0) { $build | Select-String -Pattern 'error|FAILED' | Select-Object -First 15; throw 'The build failed (errors above).' }
  Write-Host 'OK'
}
$bin = Join-Path $repo '.cache\build\release'
$recomp = Join-Path $bin 'recomp\hp2recomp.exe'
$exe = Join-Path $bin 'native\HotPursuit2Native.exe'
$launcher = Join-Path $bin 'launcher\HotPursuit2Launcher.exe'
if (-not (Test-Path $recomp)) { throw "The recompiler was not built ($recomp): LLVM is missing. Run this script again without -NoBuild." }
foreach ($built in $exe, $launcher) { if (-not (Test-Path $built)) { throw "Not built: $built. Run this script again without -NoBuild." } }
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

# ---- 4. the game's program, recompiled from your copy (or later, in the launcher) ----
Step '4/5  Preparing the game from your SLUS_203.62'
if ($Rom) {
  & $recomp package (Join-Path $Rom 'SLUS_203.62') (Join-Path $Destination 'hp2code.dll') | Select-Object -Last 1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'Recompiling failed.' }
  Write-Host 'OK'
} else {
  Write-Host 'Later: the launcher does it after checking your disc (about a minute).'
}

# ---- 5. the game folder ----
Step '5/5  Game folder'
foreach ($file in $launcher, $exe, $recomp) { Copy-Item $file $Destination -Force }
$Destination = (Resolve-Path -LiteralPath $Destination).Path
if ($Rom) {  # tell the launcher where the disc is (UTF-16: folder names with accents work)
  $ini = Join-Path $Destination 'launcher.ini'
  $text = if (Test-Path -LiteralPath $ini) { Get-Content -LiteralPath $ini -Raw } else { '' }
  if ($text -match '(?m)^folder=') { $text = $text -replace '(?m)^folder=[^\r\n]*', ('folder=' + $Rom.Replace('$', '$$')) }
  else { $text = "[disc]`r`nfolder=$Rom`r`n" + $text }
  Set-Content -LiteralPath $ini -Value $text -Encoding Unicode -NoNewline
}
Write-Host "OK: $Destination"
Write-Host ''
$minutes = [math]::Max(1, [math]::Round($watch.Elapsed.TotalMinutes))
Write-Host ("Done in {0} minute{1}. To play, double-click:" -f $minutes, $(if ($minutes -eq 1) { '' } else { 's' })) -ForegroundColor Green
Write-Host "  $Destination\HotPursuit2Launcher.exe" -ForegroundColor Green
if (-not $NoLaunch) {
  Write-Host 'Starting the launcher...'
  Start-Process -FilePath (Join-Path $Destination 'HotPursuit2Launcher.exe') -WorkingDirectory $Destination
}
