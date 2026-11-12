<#
.SYNOPSIS
  Fetch the pinned Play! emulator source the launcher (engine/) is built on.

.DESCRIPTION
  Play! (https://github.com/jpd002/Play-, BSD 2-Clause) is the embedded PS2
  core of Milestone 1 (ADR 0001, path C1; license verdict in docs/prior-art.md).
  Its source is NOT part of this repository. This script downloads exactly
  the pinned commit and the submodules the headless core needs into
  <repo>\.cache\third_party\Play- (git-ignored). Nothing is installed and
  nothing outside that folder is written.

  Submodules fetched: Framework, CodeGen, libchdr, Dependencies
  (+ its nested zstd, xxHash, zlib). Not fetched: Nuanceur (Vulkan only),
  AltKit and SDWebImage (iOS only), ghc_filesystem (not needed with MSVC).

.EXAMPLE
  pwsh tools/fetch-play.ps1
#>
[CmdletBinding()]
param(
  [string]$Destination = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Destination) { $Destination = Join-Path $repo '.cache\third_party\Play-' }

# Pinned revision (2026-10-24). Update deliberately, together with docs/prior-art.md.
$PlayUrl = 'https://github.com/jpd002/Play-.git'
$PlayCommit = '1f27232772e484bfb79364b751b84f41f4c87481'

$env:GIT_TERMINAL_PROMPT = '0'
function Invoke-Git {
  param([string]$Dir, [string[]]$GitArgs)
  & git -C $Dir -c core.longpaths=true @GitArgs
  if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed in $Dir ($LASTEXITCODE)" }
}

if (-not (Test-Path (Join-Path $Destination '.git'))) {
  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  Invoke-Git $Destination @('init', '-q')
  Invoke-Git $Destination @('remote', 'add', 'origin', $PlayUrl)
}
$head = (& git -C $Destination rev-parse -q --verify HEAD 2>$null)
if ($head -ne $PlayCommit) {
  Write-Host "Fetching Play! $PlayCommit ..."
  Invoke-Git $Destination @('fetch', '-q', '--depth', '1', 'origin', $PlayCommit)
  Invoke-Git $Destination @('checkout', '-q', '--detach', 'FETCH_HEAD')
}
Write-Host 'Fetching submodules (Framework, CodeGen, Dependencies, libchdr) ...'
Invoke-Git $Destination @('submodule', 'update', '--init', '--depth', '1',
  'deps/Framework', 'deps/CodeGen', 'deps/Dependencies', 'deps/libchdr')
Write-Host 'Fetching nested dependencies (zstd, xxHash, zlib) ...'
Invoke-Git (Join-Path $Destination 'deps\Dependencies') @('submodule', 'update', '--init', '--depth', '1',
  'zstd', 'xxHash', 'zlib')

$now = (& git -C $Destination rev-parse HEAD)
if ($now -ne $PlayCommit) { throw "Play! is at $now, expected $PlayCommit" }
Write-Host "Play! ready at $Destination ($PlayCommit)"
