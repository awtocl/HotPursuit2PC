# STATUS — HotPursuit2PC

Single source of truth for project state. A fresh session resumes from this
file alone. Updated in every phase commit. Spec: `docs/brief.md` (brief v3).

**Last update:** 2026-11-12 — end of phase **P2 Unpack & Inventory**.

## Where we are

| Phase | State | Evidence |
|---|---|---|
| P0 Recon | **done** | commit `2f150c6`; `docs/dump-analysis.md`, `docs/prior-art.md`, ADR 0001, ADR 0002 |
| P1 Scaffold | **done** | commit `fe8ab16`; `pwsh tools/build.ps1` → `selftest` passes (Ninja/dev preset) and the CI command sequence passes locally with the Visual Studio generator. **Not verified:** the GitHub Actions run itself (nothing pushed yet). |
| P2 Unpack & Inventory | **done** | this commit; ADR 0004; `assets-pipeline/` (`hp2assets` library + `hp2assets.exe`); `test_assets` (synthetic) and `test_dump` (real dump) pass; full unpack 402 files / 3,098,727,312 bytes, resume and `--verify` clean; inventory in `docs/dump-analysis.md` §5; C++ inventory equals an independent Python re-hash for all 405 records. |
| P3 Converter v0 | next | — |
| P4 M1 Build | pending | needs owner go-ahead to fetch third-party source (see Open questions) |
| P5 M1 Evidence + Gate Report | pending | — |

## Decisions (ADRs)

| ADR | Title | Decision in one line |
|---|---|---|
| [0001](adr/0001-architecture.md) | Architecture | Target = static recompilation + native runtime (A′). **M1 ships emulator-backed (Path C1: embedded Play! core, BSD-2, no BIOS), labelled as not native.** Path B rejected for game logic. |
| [0002](adr/0002-language-stack.md) | Language stack | C++20 / MSVC / CMake+Ninja for everything shipped; Python (stdlib) for dev-only recon scripts. |
| 0003 | GUI framework | to write in P3 (leaning: plain Win32, same core as the CLI) |
| [0004](adr/0004-asset-pipeline.md) | Asset pipeline | Names come from a table recovered from the dump (compiled in); unpack = exact 1:1 extents + manifest with SHA-256; resumable, never overwrites silently; ROM opened read-only. |
| 0005 | Rendering & audio | to write in P4 |

## What P0 established (all byte-verified, see `docs/dump-analysis.md`)

- Brief §4 corrections: the dump has **27** files (not 25); `VSSVER.SCC` **is** present; serial is **SLUS-20362**.
- `ZDIR.BIN` = 405 records × 12 bytes `{u32 hash, u32 sector, u32 size}`; extents tile `ZZDATA.BIN` exactly (no gaps/overlaps).
- Name hash: `h = 0xFFFFFFFF; h = h*33 + byte` over the upper-cased `\`-separated path.
- `.LZC` = `COMP` header + LZSS (u16 flag words, 4-bit length + 12-bit distance); decoder verified byte-exact on all 11 LZC/BUN pairs on the disc.
- **362 of 402** file names recovered from the dump itself (ELF strings/templates + strings inside data). No third-party name list is used. 40 files stay unnamed (31 unreferenced replays, 4 movies, 5 misc).
- SHA-256 manifest of the dump: `tests/fixtures/slus20362_rom_manifest.tsv`.

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

## Next steps (P3)

1. ADR 0003 (GUI framework).
2. `converter/core`: ROM validation against `tests/fixtures/slus20362_rom_manifest.tsv` (compiled in) with clear, actionable errors;
   `game/` folder layout (C6), portable/installed modes, settings + state file, stage runner (`validate → rom → unpack`), resume, logs.
3. `converter/cli` (`hp2pc-cli.exe`) and `converter/gui` (`HotPursuit2PC.exe`, stub per ADR 0003) on the same core.
4. Tests: synthetic ROM folders (always) + the owner's dump (skipped when absent).
5. Commit `P3 Converter v0: …`, update this file.
