<#
.SYNOPSIS
  Makes the ready-to-run download: a zip with the launcher, the native game and the recompiler, plus the license,
  the third-party notices, the changelog and a short read-me. No game files go in it.
.DESCRIPTION
  1. builds the release preset (unless -NoBuild; the recompiler needs LLVM, see tools/install-native.ps1);
  2. copies HotPursuit2Launcher.exe, HotPursuit2Native.exe and hp2recomp.exe into .cache\dist\HotPursuit2PC-v<version>-win64\;
  3. adds README.txt, LICENSE.txt, CHANGELOG.md and THIRD-PARTY-NOTICES.txt (LLVM and LLD, which hp2recomp.exe contains);
  4. refuses to continue if anything that looks like game data is in the folder;
  5. zips it next to the folder and prints the zip's SHA-256.
  The player unzips it anywhere and starts HotPursuit2Launcher.exe, which asks for their own disc and makes
  hp2code.dll from it on their PC.
.EXAMPLE
  pwsh tools/package-release.ps1 -Version 0.2.0
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string]$Version,
  [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if (-not $NoBuild) {
  $build = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Preset release -NoTest 2>&1
  if ($LASTEXITCODE -ne 0) { $build | Select-String -Pattern 'error|FAILED' | Select-Object -First 15; throw 'The release build failed.' }
}
$bin = Join-Path $repo '.cache\build\release'
$files = @(
  (Join-Path $bin 'launcher\HotPursuit2Launcher.exe'),
  (Join-Path $bin 'native\HotPursuit2Native.exe'),
  (Join-Path $bin 'recomp\hp2recomp.exe')
)
foreach ($f in $files) { if (-not (Test-Path -LiteralPath $f)) { throw "Not built: $f" } }

$name = "HotPursuit2PC-v$Version-win64"
$dist = Join-Path $repo '.cache\dist'
$out = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"
if ((Test-Path -LiteralPath $out) -or (Test-Path -LiteralPath $zip)) {
  # an earlier package of the same version is moved aside, never deleted
  $aside = Join-Path $dist ("_previous\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
  New-Item -ItemType Directory -Force -Path $aside | Out-Null
  foreach ($old in $out, $zip) { if (Test-Path -LiteralPath $old) { Move-Item -LiteralPath $old -Destination $aside } }
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($f in $files) { Copy-Item -LiteralPath $f -Destination $out }
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $out 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $repo 'CHANGELOG.md') -Destination $out

# third-party notices: hp2recomp.exe links LLVM and LLD (Apache License 2.0 with LLVM Exceptions)
$llvmSrc = Get-ChildItem (Join-Path $repo '.cache\third_party') -Directory -Filter 'llvm-*' |
           Where-Object { Test-Path (Join-Path $_.FullName 'llvm\LICENSE.TXT') } | Select-Object -First 1
if (-not $llvmSrc) { throw 'The LLVM source (for its license text) was not found under .cache\third_party.' }
$notices = @"
HotPursuit2PC $Version - third-party notices

hp2recomp.exe contains LLVM and LLD ($($llvmSrc.Name), https://llvm.org), used as the recompiler's code generator and
linker. They are distributed under the Apache License v2.0 with LLVM Exceptions; the full texts follow.
HotPursuit2Launcher.exe and HotPursuit2Native.exe contain no third-party code beyond the Microsoft C/C++ runtime and
the Windows SDK.

Need for Speed and Hot Pursuit are trademarks of Electronic Arts Inc. This project is not affiliated with or endorsed
by Electronic Arts or Sony Interactive Entertainment, and it contains none of their files.

================================================================================
LLVM (llvm/LICENSE.TXT)
================================================================================

"@ + (Get-Content -Raw -LiteralPath (Join-Path $llvmSrc.FullName 'llvm\LICENSE.TXT')) + @"

================================================================================
LLD (lld/LICENSE.TXT)
================================================================================

"@ + (Get-Content -Raw -LiteralPath (Join-Path $llvmSrc.FullName 'lld\LICENSE.TXT'))
Set-Content -LiteralPath (Join-Path $out 'THIRD-PARTY-NOTICES.txt') -Value $notices -Encoding utf8

$readme = @"
HotPursuit2PC $Version - Need for Speed: Hot Pursuit 2 (PS2) running natively on Windows
https://github.com/awtomi/HotPursuit2PC

No game files are included. You need your own disc of the North American PlayStation 2 release (SLUS-20362), in a DVD
drive or as an .iso image you made of it (double-click the .iso to mount it).

1. Unzip this folder anywhere you like (for example C:\Games\HotPursuit2PC). Keep the three programs together.
2. Start HotPursuit2Launcher.exe. Windows may warn that the program is unrecognised (it isn't signed):
   click "More info", then "Run anyway".
3. The launcher finds your disc, copies and checks it (about 3 GB), and prepares the game from it (about a minute).
4. Press Play. In the game: F1 settings, F12 screenshot, Alt+Enter full screen.

Needs Windows 10 or 11 (64-bit) and a Direct3D 11 graphics card. Settings, saves and screenshots are kept in this
folder. Guides in six languages, known problems and help: see the web page above (docs/GETTING-STARTED.md).

What's in here:
  HotPursuit2Launcher.exe   the launcher (start this one)
  HotPursuit2Native.exe     the game's runtime
  hp2recomp.exe             translates the game's program from your disc into hp2code.dll, on your PC
  CHANGELOG.md              what changed in each version
  LICENSE.txt               MIT license of this project
  THIRD-PARTY-NOTICES.txt   licenses of LLVM and LLD (inside hp2recomp.exe)
"@
Set-Content -LiteralPath (Join-Path $out 'README.txt') -Value $readme -Encoding utf8

# nothing from the game may ship: disc images, the game's program, dumps, the translated code, saves, settings
$bad = Get-ChildItem -LiteralPath $out -Recurse -File | Where-Object {
  $_.Name -match '(?i)\.(iso|bin|img|mdf|nrg|cso|pdb|ini|ps2|dll)$|^SLUS_|^SLES_|^SCUS_|hp2code|mc0'
}
if ($bad) { $bad | ForEach-Object { Write-Host "Not allowed in the package: $($_.FullName)" -ForegroundColor Red }; throw 'Game data or local files in the package.' }

Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
Get-ChildItem -LiteralPath $out | Format-Table Name, Length -AutoSize | Out-String | Write-Host
Write-Host ("{0}  ({1:N1} MB)" -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB))
Write-Host "SHA-256 $hash"
