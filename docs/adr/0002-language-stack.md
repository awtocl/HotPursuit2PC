# ADR 0002 — Language stack

- **Status:** Accepted (2026-11-11, phase P0)
- **Constraints in play:** C5, C6 (single portable `.exe` next to `game/`), clean-machine acceptance (§7)

## Context

Components: asset pipeline (binary parsing, hashing, 3 GB of I/O), converter
(CLI + GUI), launcher/runtime (hosts an emulator core now, a recompiled game
later). The result must run on a clean Windows 10/11 x64 machine with nothing
to install, and CI must build it on `windows-latest`.

Toolchain facts on the development machine (detected read-only, 2026-11-11):

| Tool | Present | Notes |
|---|---|---|
| MSVC 14.44 (VS Build Tools 2022 17.14), Windows SDK 10.0.26100 | yes | x64 `cl.exe`, MSBuild, bundled CMake + Ninja |
| .NET SDK | **no** | runtimes 6–11 only; no targeting packs |
| Rust (cargo/rustc) | **no** | |

The workspace rules forbid the agent from installing software, so a stack
that is not already installed needs the owner to install it first.

## Options considered

| Option | Tooling here | Runtime deps on a clean PC | Fit with PS2 OSS (Play!, PCSX2, parallel-gs are C++) | Parser safety | Solo maintainability |
|---|---|---|---|---|---|
| **C++20 / MSVC / CMake** | ready | none with static CRT; exe ≈ 1 MB | direct linking | manual (mitigated: bounds-checked reader, tests on real data) | one toolchain for everything |
| Rust | not installed | none; exe ≈ 5–10 MB | via C ABI / `cc`+`cmake` crates | best | second toolchain next to the C++ needed for cores |
| C# / .NET 8+ | SDK not installed | 70–150 MB self-contained, or a runtime install | P/Invoke | good | second toolchain; GUI is easiest here |
| C# / .NET Framework 4.8 | only via bare `csc` | none (in-box) | P/Invoke | good | legacy; no package/test tooling without an SDK |

## Decision

| Component | Language | Why |
|---|---|---|
| Asset pipeline (`assets-pipeline/`) | **C++20** | Same code is reused by the converter and, later, by the native runtime's file layer; no FFI. |
| Converter core + CLI + GUI (`converter/`) | **C++20**, Win32 for the GUI (ADR 0003) | Single static `.exe`, zero dependencies on a clean machine (C6). |
| Launcher / runtime (`engine/`) | **C++20** | Must link C++ cores and graphics/audio APIs directly. |
| Dev-time recon and evidence scripts (`tools/recon/`) | **Python 3** (standard library only) | Fast to write, never shipped, not needed to build. They double as an independent second implementation the C++ code is checked against. |

Build: CMake ≥ 3.25 with presets, Ninja, MSVC, `/W4 /permissive-`, static CRT
(`MultiThreaded`), x64 only. Tests: a small in-repo test harness driven by CTest
(no package manager is available, and none is needed).

## Consequences

- One compiler toolchain for everything that ships; CI is a plain CMake build.
- Memory-safety is our job: all file parsing goes through a bounds-checked
  reader, parsers never trust sizes/offsets from files, and tests run against
  the real dump plus corrupted variants.
- No package manager: third-party code is vendored or fetched as pinned
  source (each with a `docs/prior-art.md` verdict).

## Confidence

High for "C++ for anything that links emulator/graphics code". Medium for the
converter/asset pipeline — on a machine with a free choice of toolchain, Rust
would have been the pick for the parsers.

## What would reopen this ADR

- A parser memory-safety bug class that tests/fuzzing do not contain.
- The owner installs Rust and prefers it for `assets-pipeline/` (the Python
  reference implementation makes a port cheap to validate).
- GUI requirements outgrowing plain Win32 (see ADR 0003).
