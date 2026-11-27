<#
.SYNOPSIS
  Builds the native game (HotPursuit2Native.exe, no emulator) from YOUR OWN dump of Need for Speed: Hot Pursuit 2
  (PS2, NTSC-U, SLUS-20362) and puts it in a folder ready to play.
.DESCRIPTION
  No game files are in this repository or downloaded by this script. You point it at the extracted contents of your own
  disc (the folder holding SYSTEM.CNF, SLUS_203.62 and NFSHP2; a mounted .iso drive works too). The script:
    1. checks the folder and that SLUS_203.62 is the supported version (SHA-256 from converter/data);
    2. builds the project (release) unless -NoBuild;
    3. recompiles the game's program from YOUR SLUS_203.62 into hp2code.dll (on this PC; it stays on this PC);
    4. copies HotPursuit2Native.exe and hp2code.dll into -Destination and writes Play.cmd that starts the game from your
       dump folder (read-only: nothing is written to it).
  Without -Rom a folder picker asks for the dump folder.
  Requirements: see README "Building from source" (Visual Studio 2022 C++, and LLVM via tools/fetch-llvm.ps1 +
  tools/build-llvm.ps1 for the recompiler).
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

if (-not $Rom) {
  Add-Type -AssemblyName System.Windows.Forms
  $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
  $dialog.Description = 'Folder with the contents of YOUR Need for Speed: Hot Pursuit 2 disc (SYSTEM.CNF, SLUS_203.62, NFSHP2)'
  if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw 'No folder chosen.' }
  $Rom = $dialog.SelectedPath
}
$Rom = (Resolve-Path -LiteralPath $Rom).Path
foreach ($need in 'SYSTEM.CNF', 'SLUS_203.62', 'NFSHP2') {
  if (-not (Test-Path -LiteralPath (Join-Path $Rom $need))) {
    throw "'$Rom' does not look like the contents of the disc: $need is missing. Extract your disc (or mount your .iso) and choose the folder that holds SYSTEM.CNF, SLUS_203.62 and NFSHP2."
  }
}
$manifest = Get-Content (Join-Path $repo 'converter\data\slus20362_rom_manifest.tsv') | Where-Object { $_ -like 'SLUS_203.62*' }
$expected = ($manifest -split "`t")[2]
$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Rom 'SLUS_203.62')).Hash.ToLowerInvariant()
if ($actual -ne $expected) {
  throw "SLUS_203.62 is not the supported version (SHA-256 $actual). Only the North American release SLUS-20362 v1.00 is supported."
}
Write-Host "Dump: $Rom (SLUS_203.62 verified)"

if (-not $NoBuild) {
  Write-Host 'Building (release) ...'
  $build = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Preset release -NoTest 2>&1
  if ($LASTEXITCODE -ne 0) { $build | Select-String -Pattern 'error|FAILED' | Select-Object -First 15; throw 'build failed' }
}
$recomp = Join-Path $repo '.cache\build\release\recomp\hp2recomp.exe'
$exe = Join-Path $repo '.cache\build\release\native\HotPursuit2Native.exe'
if (-not (Test-Path $recomp)) { throw "The recompiler was not built ($recomp): run tools/fetch-llvm.ps1 and tools/build-llvm.ps1 first, then this script again." }
if (-not (Test-Path $exe)) { throw "The native runtime was not built ($exe)." }

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
Write-Host 'Recompiling the game program from your SLUS_203.62 (a few minutes the first time) ...'
& $recomp package (Join-Path $Rom 'SLUS_203.62') (Join-Path $Destination 'hp2code.dll') | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw 'recompilation failed' }
Copy-Item $exe $Destination -Force
$play = Join-Path $Destination 'Play.cmd'
Set-Content -LiteralPath $play -Encoding oem -Value @(  # (the console's code page: paths with accents work)
  '@echo off',
  "start `"`" `"%~dp0HotPursuit2Native.exe`" --rom `"$Rom`""
)
Write-Host ''
Write-Host "Ready: $Destination"
Write-Host "Play:  $play   (F1 in the game: settings and Quit game)"
