# STATUS — HotPursuit2PC

Single source of truth for project state. A fresh session resumes from this
file alone. Updated in every phase commit. Spec: `docs/brief.md` (brief v3).

**Last update:** 2026-11-12 — end of phase **P3 Converter v0**. **P4 is blocked on owner decisions Q1/Q2** (see Open questions).

## Where we are

| Phase | State | Evidence |
|---|---|---|
| P0 Recon | **done** | commit `2f150c6`; `docs/dump-analysis.md`, `docs/prior-art.md`, ADR 0001, ADR 0002 |
| P1 Scaffold | **done** | commit `fe8ab16`; `pwsh tools/build.ps1` → `selftest` passes (Ninja/dev preset) and the CI command sequence passes locally with the Visual Studio generator. **Not verified:** the GitHub Actions run itself (nothing pushed yet). |
| P2 Unpack & Inventory | **done** | commit `0751cc8`; ADR 0004; `assets-pipeline/` (`hp2assets` library + `hp2assets.exe`); `test_assets` (synthetic) and `test_dump` (real dump) pass; full unpack 402 files / 3,098,727,312 bytes, resume and `--verify` clean; inventory in `docs/dump-analysis.md` §5; C++ inventory equals an independent Python re-hash for all 405 records. |
| P3 Converter v0 | **done** | this commit; ADR 0003; `converter/` (`hp2core`, `hp2pc-cli.exe`, `HotPursuit2PC.exe`); 8 CTest tests pass (dev + release presets); end-to-end run on the real dump from a clean folder: 27 ROM files copied + SHA-256 verified, 402 assets unpacked, 4.5 s; re-run rewrote nothing; ROM folder unchanged (sizes + timestamps); only `game/` next to the exes; GUI checked by screenshot and by a headless test that drives the real window through a conversion. **Not done in v0:** stage `build` (the runnable game). |
| P4 M1 Build | **blocked** | needs owner go-ahead to fetch third-party source (Q1) — see Open questions |
| P5 M1 Evidence + Gate Report | pending | — |

## Decisions (ADRs)

| ADR | Title | Decision in one line |
|---|---|---|
| [0001](adr/0001-architecture.md) | Architecture | Target = static recompilation + native runtime (A′). **M1 ships emulator-backed (Path C1: embedded Play! core, BSD-2, no BIOS), labelled as not native.** Path B rejected for game logic. |
| [0002](adr/0002-language-stack.md) | Language stack | C++20 / MSVC / CMake+Ninja for everything shipped; Python (stdlib) for dev-only recon scripts. |
| [0003](adr/0003-gui-framework.md) | GUI framework | Plain Win32 + Common Controls 6; all logic in `hp2core`; GUI and CLI are thin front-ends, so parity holds by construction. |
| [0004](adr/0004-asset-pipeline.md) | Asset pipeline | Names come from a table recovered from the dump (compiled in); unpack = exact 1:1 extents + manifest with SHA-256; resumable, never overwrites silently; ROM opened read-only. |
| 0005 | Rendering & audio | to write in P4 |

## What P0 established (all byte-verified, see `docs/dump-analysis.md`)

- Brief §4 corrections: the dump has **27** files (not 25); `VSSVER.SCC` **is** present; serial is **SLUS-20362**.
- `ZDIR.BIN` = 405 records × 12 bytes `{u32 hash, u32 sector, u32 size}`; extents tile `ZZDATA.BIN` exactly (no gaps/overlaps).
- Name hash: `h = 0xFFFFFFFF; h = h*33 + byte` over the upper-cased `\`-separated path.
- `.LZC` = `COMP` header + LZSS (u16 flag words, 4-bit length + 12-bit distance); decoder verified byte-exact on all 11 LZC/BUN pairs on the disc.
- **362 of 402** file names recovered from the dump itself (ELF strings/templates + strings inside data). No third-party name list is used. 40 files stay unnamed (31 unreferenced replays, 4 movies, 5 misc).
- SHA-256 manifest of the dump: `converter/data/slus20362_rom_manifest.tsv` (27 files, 3,120,519,117 bytes).

## What P2 added

- `hp2assets` (C++): `name_hash`, `parse_zdir` + `check_zdir_layout`, `lzc_decompress`, `NameTable::builtin()` (362 names),
  `ZFileset::open/read`, `classify_file`, `write_inventory_tsv`, `unpack` (writes `<name>.part` then renames; existing
  right-size files are kept; wrong ones are an error unless `repair`, which moves them aside as `.mismatch-N`).
- Tool: `.cache/build/dev/assets-pipeline/hp2assets.exe` — `info`, `inventory`, `unpack`, `lzc`, `hash`.
- Fixtures: `tests/fixtures/slus20362_zfileset_inventory.tsv` (405 records with SHA-256).
- Name-recovery lesson (dump-analysis §4): a 32-bit hash match is not proof. Tier C/D names now need independent
  confirmation; a chance collision (`CARS\F50\NU8.BIN` over `SOUND\ALPINE.MSB`) was caught and fixed in this phase.
- Performance on the dev PC: SHA-256 inventory of 3.1 GB ≈ 2 s; full unpack ≈ 2–3 s (warm file cache).
- Unpacked copy from the evidence run: `.cache/unpack-full/` (git-ignored, 3.1 GB — safe to remove by the owner).

## Environment facts (development PC)

- Windows 11, MSVC 14.44 (VS Build Tools 2022 17.14) + Windows SDK 10.0.26100, bundled CMake + Ninja under
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\`.
  Not on PATH: use `VC\Auxiliary\Build\vcvars64.bat`.
- Repo: branch `main` tracking `origin/main`
  (`https://github.com/awtocl/HotPursuit2PC`). **Nothing has been pushed.**
- ROM (read-only, git-ignored): `ROM/Need for Speed - Hot Pursuit 2 (USA)/`.

## Working rules that bind every session

- C1–C8 from the brief. ROM is read-only. No destructive git, **no push without asking**.
- Owner's working rules: announce every command; never delete project files (archive them instead); no installing software; ask before downloads.
  Standing permission (2026-11-11): detect/use installed dev tools, build inside the project folder with caches in `.cache\`, fetch dependencies from nuget.org/crates.io when named first, local-only git.
  Fetching third-party **source from GitHub** (e.g. Play!) is *not* covered — ask first.
- Local build state, caches and scratch go to `.cache/` (git-ignored).

## Open questions

| # | Question | Needed by | Owner decision? |
|---|---|---|---|
| Q1 | Go-ahead to fetch and build Play! (BSD-2) source for the M1 go/no-go test on SLUS-20362 | P4 | **yes** |
| Q2 | If Play! fails the M1 bar: does the owner have a BIOS dump from their own PS2 (needed for the PCSX2 fallback)? | P4 | yes |
| Q3 | Repo license: MIT chosen as default for our own code — acceptable? | before first push | yes (can change freely until pushed) |
| Q4 | Names of 40 unnamed files (31 replays, 4 movies, 5 misc) — are any loaded by the game? | P4/P5 (watch file accesses at runtime) | no |
| Q5 | Which IRX set (243 vs 253) the ELF loads, and when | P4 | no |

## How to build and test

```powershell
pwsh tools/build.ps1            # loads MSVC env, cmake preset "dev", builds, runs CTest
```

Build output and compiler temp files stay in `.cache/` (git-ignored). CI
(`.github/workflows/ci.yml`) runs: tracked-file guard → configure (`-A x64 -DHP2_WERROR=ON`)
→ build Release → `ctest --no-tests=error`. Tests that need the dump exit 77 = skipped when it is absent.

## What P3 added

- `hp2core`: `check_rom` (quick/full; corrects a neighbouring folder; actionable errors), `inspect_game_folder`,
  `find_game_folder`, `convert` (stages `validate → rom → unpack`, then `build` = not available yet), `KeyValueFile` (`hp2pc.cfg`).
- Game folder layout (C6): `hp2pc.cfg`, `rom/`, `assets/`, `logs/` (+ `build/` from P4). Portable = next to the exe with its own ROM copy,
  relocatable (re-run falls back to `game/rom` if the original ROM is gone). Installed = chosen directory, ROM copy or reference.
- Rules enforced and tested: ROM folder never written; no game folder inside the ROM folder; foreign non-empty folders refused;
  `.part` + rename; mismatching files never overwritten silently (`repair` moves them aside); cancel keeps finished work.
- Release executables ≈ 0.7 MB each, dependencies: only Windows system DLLs (checked with `dumpbin /dependents`).
- Evidence run left a complete converted folder at `.cache/p3-run/game/` (6.2 GB, git-ignored; the owner may remove it).
- Dev switches of the GUI: `--selftest`, `--selftest-convert <rom> <game>`, `--screenshot <bmp>`.

## Next steps (P4 — Milestone 1 build) — needs Q1 answered first

1. Go/no-go test: does the Play! core run SLUS-20362 to gameplay at a stable 60 fps? (compatibility is UNVERIFIED for NTSC-U).
   Needs: fetch Play! source + submodules from GitHub (BSD-2), build its core/libretro target with MSVC, boot the disc.
   The disc must be presented as an image: write an ISO9660 builder in `converter/core` (stage `package`: `game/rom/` → `game/build/disc.iso`).
2. ADR 0005 (rendering & audio) for the launcher: window, graphics API, audio device, XInput → pad.
3. `engine/`: launcher hosting the core; converter stage `build` copies launcher + runtime into `game/build/`, writes per-game config.
4. If Play! fails the bar: fallback C2 (PCSX2 as external process) — needs Q2 (owner's own BIOS dump).
5. P5: acceptance runs, evidence, Gate Report.
