# STATUS — HotPursuit2PC

Single source of truth for project state. A fresh session resumes from this
file alone. Updated in every phase commit. Spec: `docs/brief.md` (brief v3).

**Last update:** 2026-11-12 — end of phase **P4b M1 runtime**. The game runs:
`game\build\HotPursuit2.exe` (emulator-backed, labelled as such). Next: P5 (evidence + Milestone 1 Gate Report).

## Where we are

| Phase | State | Evidence |
|---|---|---|
| P0 Recon | **done** | commit `2f150c6`; `docs/dump-analysis.md`, `docs/prior-art.md`, ADR 0001, ADR 0002 |
| P1 Scaffold | **done** | commit `fe8ab16`; build + CI workflow. **Not verified:** the GitHub Actions run itself (nothing pushed yet). |
| P2 Unpack & Inventory | **done** | commit `0751cc8`; ADR 0004; `hp2assets`; full unpack 402 files / 3,098,727,312 bytes; inventory in `docs/dump-analysis.md` §5; C++ = independent Python re-hash for all 405 records. |
| P3 Converter v0 | **done** | commit `758ffc8`; ADR 0003; `hp2core`, `hp2pc-cli.exe`, `HotPursuit2PC.exe`; end-to-end on the real dump; resume; ROM untouched. |
| P4a Disc image packaging | **done** | commit `9f64f48`; ISO 9660 builder/reader, `hp2pc-cli image`; 7-Zip confirms 27 files, all SHA-256 identical. |
| P4b M1 runtime | **done** | this commit; owner approved fetching Play! (Q1). ADR 0005. `engine/` launcher on the Play! core (pinned `1f27232`, unmodified), virtual disc from `game/rom`, converter stage `build` (launcher embedded in the converter). 10 CTest tests pass in dev and release, including Play!'s JIT test suite. The game boots to menus and races from the converter-made folder. |
| P5 M1 Evidence + Gate Report | next | acceptance runs already exist under `.cache/m1-run/`; to be written up |

## Decisions (ADRs)

| ADR | Title | Decision in one line |
|---|---|---|
| [0001](adr/0001-architecture.md) | Architecture | Target = static recompilation + native runtime (A′). **M1 ships emulator-backed (Path C1: embedded Play! core, BSD-2, no BIOS), labelled as not native.** Path B rejected for game logic. |
| [0002](adr/0002-language-stack.md) | Language stack | C++20 / MSVC / CMake+Ninja for everything shipped; Python (stdlib) for dev-only recon scripts. |
| [0003](adr/0003-gui-framework.md) | GUI framework | Plain Win32 + Common Controls 6; all logic in `hp2core`; GUI and CLI are thin front-ends. |
| [0004](adr/0004-asset-pipeline.md) | Asset pipeline | Names from a table recovered from the dump; unpack = exact 1:1 extents + manifest with SHA-256; resumable; ROM read-only. |
| [0005](adr/0005-rendering-audio.md) | Launcher | Play! core libraries linked statically (source unmodified); its OpenGL renderer on our WGL context; WaveOut audio; XInput + keyboard; virtual disc from the ROM folder; all data in `game/build/data`; core's 60 Hz limiter on. **Races are drawn at 30 fps by the game itself, menus at 60** (measured; unverified on a console). |

## What exists now (how to use it)

```powershell
pwsh tools/fetch-play.ps1             # once: pinned Play! source into .cache/third_party/Play- (~80 MB)
pwsh tools/build.ps1 -Preset release  # builds everything, runs the 10 tests
# converter (carries the launcher inside):
#   .cache\build\release\converter\HotPursuit2PC.exe   (GUI)
#   .cache\build\release\converter\hp2pc-cli.exe       (CLI)
```

Copy the converter into an empty folder, run it on the ROM folder; it creates
`game\` with `rom\`, `assets\`, `build\`, `logs\`, `hp2pc.cfg`. Play with
`game\build\HotPursuit2.exe` (no arguments needed). A converted, played-in
example is at `.cache\m1-run\game\`.

Developer tools: `tools/run-launcher.ps1` (fixed-length run with timed pad
input and screenshots), `hp2assets.exe` (inventory/unpack/lzc/hash),
`tools/recon/*.py` (evidence generators).

## Facts established so far

Dump and formats (all byte-verified, `docs/dump-analysis.md`):

- The dump has **27** files; serial **SLUS-20362**; `VSSVER.SCC` is present (brief §4 corrections).
- `ZDIR.BIN` = 405 × 12-byte records `{hash, sector, size}`; extents tile `ZZDATA.BIN` exactly.
- Name hash `h = 0xFFFFFFFF; h = h*33 + byte` (upper-cased `\` path). `.LZC` = `COMP` + LZSS, verified on 11 pairs.
- 362 of 402 names recovered from the dump itself; tier C/D names need independent confirmation (a chance collision was caught in P2).
- ROM manifest: `converter/data/slus20362_rom_manifest.tsv` (27 files, 3,120,519,117 bytes).

Runtime (P4b, `docs/evidence/` is written in P5):

- Play! `1f27232` runs SLUS-20362 from the owner's files: memory-card check, intro movie, title, menus, Quick Race, pause menu, Save/Load (creates `BASLUS-20362HP2` on the virtual memory card; next start loads it).
- The game finds files **by name**: a rebuilt image / the virtual disc with different sector positions works.
- Emulated refresh holds 59–61 Hz outside loading; emulation headroom 2.5–5× with the limiter off.
- Races: 30 frames drawn per second, unchanged by a 1.5×/2× EE clock or 2×/3× instructions per tick → the game's own pacing.
- An unnecessary `CDROM0_SyncPath()` call from the launcher right after `Initialize()` crashed the core; removed (do not reintroduce).
- Play!'s OpenGL `GetScreenshot()` is a stub → the launcher reads the back buffer itself.
- Play!'s `ResetVM()` re-opens the disc from its path preference → the launcher re-mounts the virtual disc in `BeforeExecutableReloaded`.

## Environment facts (development PC)

- Windows 11, MSVC 14.44 (VS Build Tools 2022 17.14) + Windows SDK 10.0.26100, bundled CMake 3.31 + Ninja (not on PATH; `tools/build.ps1` loads `vcvars64.bat`).
- Repo: branch `main` tracking `origin/main` (`https://github.com/awtocl/HotPursuit2PC`). **Nothing has been pushed.**
- ROM (read-only, git-ignored): `ROM/Need for Speed - Hot Pursuit 2 (USA)/`.
- Third-party source (git-ignored): `.cache/third_party/Play-` (pinned, must stay unmodified).
- Scratch from evidence runs (git-ignored, ~25 GB, the owner may remove any of it):
  `.cache/unpack-full/`, `.cache/p3-run/`, `.cache/p4a/`, `.cache/p4b*/`, `.cache/m1-run/` (the last one holds the Milestone 1 example folder and acceptance evidence).

## Working rules that bind every session

- C1–C8 from the brief. ROM is read-only. No destructive git, **no push without asking**.
- Owner's working rules: announce every command; never delete project files (archive them instead); no installing software; ask before downloads.
  Standing permissions: (2026-11-11) detect/use installed dev tools, build inside the project folder with caches in `.cache\`, local-only git; (2026-11-12) fetch the Play! source and its submodules from GitHub into `.cache/third_party/`.
  Anything else from the network needs a new question.
- Never commit game data or screenshots of the game (CI guard + CONTRIBUTING).
- Running the launcher opens a window and (unless `--mute`) plays sound on the owner's PC: say so before running it.

## Open questions

| # | Question | Needed by | Owner decision? |
|---|---|---|---|
| Q1 | ~~Fetch and build Play!?~~ — **answered yes (2026-11-12)** | — | — |
| Q2 | BIOS dump for a PCSX2 fallback — **no longer needed** (Play! passed) | — | — |
| Q3 | Repo license: MIT chosen as default for our own code — acceptable? | before first push | yes |
| Q4 | Names of 40 unnamed files — are any loaded by the game? (could be observed by logging the core's file opens) | later | no |
| Q5 | Which IRX set (243 vs 253) the game loads, and when | later | no |
| Q6 | Does the owner remember races at 30 or 60 fps on the console? Does handling/speed feel right? | M1 sign-off | **yes — only the owner can judge** |
| Q7 | Audio by ear: clean, in sync, right pitch? | M1 sign-off | **yes** |
| Q8 | Does the game use DualShock 2 button pressure (throttle/brake)? Not emulated by the pad handler. | M1 sign-off / M2 | observation by the owner |

## Next steps

1. **P5:** write up the acceptance evidence (`docs/evidence/m1-acceptance.md`) and the Milestone 1 Gate Report; commit; stop for the owner to try the game.
2. After sign-off — **Milestone 2**, each item with its own ADR, opt-in where it changes the original:
   anti-aliasing/upscaling first (brief: "DLAA first"; realistic options with this core: higher internal resolution,
   SSAA, post-process AA; a true DLAA needs motion vectors the PS2 pipeline does not provide), then resolution/aspect
   (widescreen), save robustness (save states), a config file, controller improvements (rumble, pressure, remapping), frame-rate modes.
3. Native track (ADR 0001 stages N1/N2): Play! has `USE_AOT_CACHE`/`BUILD_AOT_CACHE` options — the first thing to evaluate for ahead-of-time translation.
