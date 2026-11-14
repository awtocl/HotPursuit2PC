<#
.SYNOPSIS
  Download the pinned LLVM release (source) that the recompiler uses as its
  code generator (ADR 0007), and unpack the parts that are needed.

.DESCRIPTION
  One file is downloaded from the LLVM project's GitHub release page:
  llvm-project-<version>.src.tar.xz (about 171 MB). Its SHA-256 must match
  the value pinned below. Only llvm/, cmake/, third-party/, libc/ (shared
  headers), lld/ (the linker, used as a library by the converter to link
  the translated game code into a DLL, ADR 0009) and libunwind/ (headers lld
  needs) are unpacked,
  into <repo>\.cache\third_party\llvm-<version>\ (git-ignored). Nothing is
  installed and nothing outside the repository is written.

  LLVM is licensed under the Apache License 2.0 with LLVM Exceptions
  (llvm/LICENSE.TXT in the unpacked tree; verdict in docs/prior-art.md).

.EXAMPLE
  pwsh tools/fetch-llvm.ps1
  pwsh tools/build-llvm.ps1
#>
[CmdletBinding()]
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$version = '23.1.5'                                             # tag llvmorg-23.1.5 = commit 392dfff97d29b59d77e5cac6d4b6ef2585fe7117
$sha256 = 'fb50b1eac93ddeb3aa8dab4cfe324dff74f4a9c2bdee6160c9e36188f8d06a6e'  # 179,220,264 bytes, as downloaded 2026-11-12
$name = "llvm-project-$version.src"
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$name.tar.xz"

$repo = Split-Path -Parent $PSScriptRoot
$third = Join-Path $repo '.cache\third_party'
$downloads = Join-Path $third 'downloads'
$archive = Join-Path $downloads "$name.tar.xz"
$dest = Join-Path $third "llvm-$version"
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

$parts = @('llvm', 'cmake', 'third-party', 'libc', 'lld', 'libunwind')
$missing = @($parts | Where-Object { -not (Test-Path (Join-Path $dest $_)) })
if ((Test-Path (Join-Path $dest 'llvm\CMakeLists.txt')) -and $missing.Count -eq 0 -and -not $Force) {
  Write-Host "LLVM $version is already unpacked in $dest"
  exit 0
}
if (-not (Test-Path $archive)) {
  Write-Host "Downloading $url"
  $part = "$archive.part"
  & curl.exe --fail --location --silent --show-error --user-agent 'build-fetch' --output $part $url
  if ($LASTEXITCODE -ne 0) { throw "download failed (curl exit code $LASTEXITCODE)" }
  Move-Item -LiteralPath $part -Destination $archive
}
$actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "SHA-256: $actual"
if ($actual -ne $sha256) { throw "The downloaded archive does not have the pinned SHA-256 ($sha256). Not unpacking it." }

# Unpack into a fresh folder, then move it into place, so that an interrupted run leaves nothing half-done.
$staging = "$dest.unpacking"   # a folder left by an interrupted run is simply unpacked over
New-Item -ItemType Directory -Force -Path $staging | Out-Null
$wanted = if (Test-Path $dest) { $missing } else { $parts }
Write-Host ("Unpacking {0} ..." -f (($wanted | ForEach-Object { "$_/" }) -join ', '))
# llvm/utils/mlgo-utils holds three symbolic links (Python helpers, not part of the build) that tar cannot create on Windows.
& tar.exe -xf $archive -C $staging --strip-components=1 --exclude "$name/llvm/utils/mlgo-utils/*.py" ($wanted | ForEach-Object { "$name/$_" })
if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }
if (-not (Test-Path $dest)) {
  if (-not (Test-Path (Join-Path $staging 'llvm\LICENSE.TXT'))) { throw "llvm/LICENSE.TXT is missing from the archive" }
  Move-Item -LiteralPath $staging -Destination $dest
} else {
  # An earlier version of this script unpacked fewer parts: add the missing ones.
  foreach ($part in $wanted) { Move-Item -LiteralPath (Join-Path $staging $part) -Destination (Join-Path $dest $part) }
  $aside = Join-Path $repo '.cache\_to_delete'
  New-Item -ItemType Directory -Force -Path $aside | Out-Null
  Move-Item -LiteralPath $staging -Destination (Join-Path $aside ("llvm-unpacking-" + (Get-Date -Format 'yyyyMMdd-HHmmss')))
}
Write-Host "LLVM $version source is in $dest"
