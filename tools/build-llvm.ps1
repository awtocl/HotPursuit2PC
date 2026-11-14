<#
.SYNOPSIS
  Build the pinned LLVM (fetched by tools/fetch-llvm.ps1) once, as static
  libraries for the recompiler (ADR 0007).

.DESCRIPTION
  Libraries only: x86 back end, Release, static C runtime (like the rest of
  this project), no tools, tests, examples or optional dependencies. Also the
  LLD linker's libraries (COFF), which the converter uses to link the
  translated game code into a DLL (ADR 0009); tools are configured but not built.
  Everything stays inside <repo>\.cache\third_party\:
    llvm-<version>-build\     build tree (can be removed after a successful build)
    llvm-<version>-install\   headers, libraries and CMake package files used by our build
  Takes roughly 15-40 minutes and several GB the first time; nothing is installed on the system.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$version = '23.1.5'
$repo = Split-Path -Parent $PSScriptRoot
$src = Join-Path $repo ".cache\third_party\llvm-$version\llvm"
$build = Join-Path $repo ".cache\third_party\llvm-$version-build"
$install = Join-Path $repo ".cache\third_party\llvm-$version-install"
if (-not (Test-Path (Join-Path $src 'CMakeLists.txt'))) { throw "LLVM source not found: run tools/fetch-llvm.ps1 first" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio 2022 Build Tools (C++ workload)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$tmp = Join-Path $repo '.cache\tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
foreach ($line in (& cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set")) {
  $i = $line.IndexOf('=')
  if ($i -gt 0) { Set-Item -Path "Env:$($line.Substring(0, $i))" -Value $line.Substring($i + 1) }
}
$env:TEMP = $tmp
$env:TMP = $tmp

$sw = [Diagnostics.Stopwatch]::StartNew()
& cmake -S $src -B $build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  "-DCMAKE_INSTALL_PREFIX=$install" `
  -DLLVM_TARGETS_TO_BUILD=X86 `
  -DLLVM_ENABLE_PROJECTS=lld -DLLVM_INCLUDE_TOOLS=ON -DLLVM_BUILD_TOOLS=OFF `
  -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TESTS=OFF `
  -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF `
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF `
  -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_DIA_SDK=OFF -DLLVM_ENABLE_PLUGINS=OFF `
  -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_WARNINGS=OFF
if ($LASTEXITCODE -ne 0) { throw "LLVM configure failed ($LASTEXITCODE)" }
& cmake --build $build --target install
if ($LASTEXITCODE -ne 0) { throw "LLVM build failed ($LASTEXITCODE)" }
Write-Host ("LLVM {0} built and installed into {1} in {2:N1} minutes" -f $version, $install, $sw.Elapsed.TotalMinutes)
