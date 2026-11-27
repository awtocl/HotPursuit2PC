<#
.SYNOPSIS
  Builds the native game (HotPursuit2Native.exe, no emulator) from YOUR OWN dump of Need for Speed: Hot Pursuit 2
  (PS2, NTSC-U, SLUS-20362) and puts it in a folder ready to play.
.DESCRIPTION
  No game files are in this repository or downloaded by this script. You point it at the extracted contents of your own
  disc (the folder holding SYSTEM.CNF, SLUS_203.62 and NFSHP2; a mounted .iso drive works too). The script:
    1. checks the folder and that SLUS_203.62 is the supported version (SHA-256 from converter/data);
    2. the first time only, downloads and builds LLVM (the recompiler's code generator; 15-40 minutes);
    3. builds the project (release) unless -NoBuild;
    4. recompiles the game's program from YOUR SLUS_203.62 into hp2code.dll (on this PC; it stays on this PC);
    5. copies HotPursuit2Native.exe and hp2code.dll into -Destination and writes Play.cmd that starts the game from your
       disc folder (read-only: nothing is written to it).
  Without -Rom a folder picker asks for the disc folder. Run it again after updating the project (git pull).
  Requirements and a step-by-step guide: docs/GETTING-STARTED.md.
.EXAMPLE
  pwsh tools/install-native.ps1 -Rom "D:\My dumps\Need for Speed - Hot Pursuit 2 (USA)"
.EXAMPLE
  pwsh tools/install-native.ps1        # asks for the folder
#>
[CmdletBinding()]
param(
  [string]$Rom = '',
  [string]$Destination = '',  # default: <repo>\game-native
  [switch]$NoBuild
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

# ---- 1. your disc ----
Step '1/5  Your disc'
if (-not $Rom) {
  Write-Host 'Choose the folder with the contents of your disc in the window that opens (it may open behind this one).'
  Add-Type -AssemblyName System.Windows.Forms
  $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
  $dialog.Description = 'Folder with the contents of YOUR Need for Speed: Hot Pursuit 2 disc (SYSTEM.CNF, SLUS_203.62, NFSHP2)'
  if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw 'No folder chosen.' }
  $Rom = $dialog.SelectedPath
}
$Rom = (Resolve-Path -LiteralPath $Rom).Path
foreach ($need in 'SYSTEM.CNF', 'SLUS_203.62', 'NFSHP2') {
  if (-not (Test-Path -LiteralPath (Join-Path $Rom $need))) {
    throw "'$Rom' does not look like the contents of the disc: $need is missing. Copy everything on your disc into a folder (or mount your .iso) and choose the folder that holds SYSTEM.CNF, SLUS_203.62 and NFSHP2."
  }
}
$manifest = Get-Content (Join-Path $repo 'converter\data\slus20362_rom_manifest.tsv') | Where-Object { $_ -like 'SLUS_203.62*' }
$expected = ($manifest -split "`t")[2]
$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Rom 'SLUS_203.62')).Hash.ToLowerInvariant()
if ($actual -ne $expected) {
  throw "SLUS_203.62 is not the supported version (SHA-256 $actual). Only the North American release SLUS-20362 v1.00 is supported."
}
Write-Host "OK: $Rom (SLUS_203.62 is the supported version)"

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
$recomp = Join-Path $repo '.cache\build\release\recomp\hp2recomp.exe'
$exe = Join-Path $repo '.cache\build\release\native\HotPursuit2Native.exe'
if (-not (Test-Path $recomp)) { throw "The recompiler was not built ($recomp): LLVM is missing. Run this script again without -NoBuild." }
if (-not (Test-Path $exe)) { throw "The native game was not built ($exe)." }

# ---- 4. the game's program, recompiled from your copy ----
Step '4/5  Recompiling the game from your SLUS_203.62 (a few minutes)'
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
& $recomp package (Join-Path $Rom 'SLUS_203.62') (Join-Path $Destination 'hp2code.dll') | Select-Object -Last 1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Recompiling failed.' }
Write-Host 'OK'

# ---- 5. the game folder ----
Step '5/5  Game folder'
Copy-Item $exe $Destination -Force
$play = Join-Path $Destination 'Play.cmd'
Set-Content -LiteralPath $play -Encoding oem -Value @(  # (the console's code page: paths with accents work)
  '@echo off',
  "start `"`" `"%~dp0HotPursuit2Native.exe`" --rom `"$Rom`""
)
$Destination = (Resolve-Path -LiteralPath $Destination).Path
Write-Host "OK: $Destination"
Write-Host ''
$minutes = [math]::Max(1, [math]::Round($watch.Elapsed.TotalMinutes))
Write-Host ("Done in {0} minute{1}. To play, double-click:" -f $minutes, $(if ($minutes -eq 1) { '' } else { 's' })) -ForegroundColor Green
Write-Host "  $Destination\Play.cmd" -ForegroundColor Green
Write-Host 'In the game, F1 opens the settings (and Quit game). Keep your disc folder where it is: the game reads it.'
