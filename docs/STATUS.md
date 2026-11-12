# STATUS — HotPursuit2PC

Single source of truth for project state. A fresh session resumes from this
file alone. Updated in every phase commit. Spec: `docs/brief.md` (brief v3).

**Last update:** 2026-11-12 — phase **P6 M1 trial follow-up**.
**The owner played the Milestone 1 build and did NOT accept it** (stutter, smoke/brake-light glow drawn as translucent
squares, sound problems; wants "much closer to a recompilation than to an emulator trimmed for the game", clearly
better than PCSX2). Direction changed by [ADR 0006](adr/0006-recompilation-main-line.md): **the product is now a
statically recompiled build; the emulator-backed launcher is kept as the reference.** Milestone 2 features are on
hold until the recompiled build exists. Next: stage **R1** (static recompiler), starting with a spike and ADR 0007.

## Where we are

| Phase | State | Evidence |
|---|---|---|
| P0 Recon | **done** | commit `2f150c6`; `docs/dump-analysis.md`, `docs/prior-art.md`, ADR 0001, ADR 0002 |
| P1 Scaffold | **done** | commit `fe8ab16`; build + CI workflow. **Not verified:** the GitHub Actions run itself (nothing pushed yet). |
| P2 Unpack & Inventory | **done** | commit `0751cc8`; ADR 0004; `hp2assets`; full unpack 402 files / 3,098,727,312 bytes; inventory in `docs/dump-analysis.md` §5; C++ = independent Python re-hash for all 405 records. |
| P3 Converter v0 | **done** | commit `758ffc8`; ADR 0003; `hp2core`, `hp2pc-cli.exe`, `HotPursuit2PC.exe`; end-to-end on the real dump; resume; ROM untouched. |
| P4a Disc image packaging | **done** | commit `9f64f48`; ISO 9660 builder/reader, `hp2pc-cli image`; 7-Zip confirms 27 files, all SHA-256 identical. |
| P4b M1 runtime | **done** | commit `aa10b4f`; owner approved fetching Play! (Q1). ADR 0005. `engine/` launcher on the Play! core (pinned `1f27232`, unmodified), virtual disc from `game/rom`, converter stage `build` (launcher embedded in the converter). 10 CTest tests pass in dev and release, including Play!'s JIT test suite. The game boots to menus and races from the converter-made folder. |
| P5 M1 Evidence + Gate Report | **done** | this commit; `docs/evidence/m1-acceptance.md`; `docs/gate-reports/2026-11-12-M1.md`. Acceptance session: 155 s, exit code 0, console refresh 59–61 Hz outside loading, pause menu, memory-card save and load. `tools/fetch-play.ps1` verified from scratch. **Not verified:** played by a person; clean machine; other GPUs; long sessions; CI run. |
| **Milestone 1 gate** | **not accepted by the owner (2026-11-12)** | owner's report + per-frame measurements: `docs/evidence/m1-acceptance.md` §4a |
| P6 M1 trial follow-up (stage R0 of ADR 0006) | **done** | this commit; launcher frame pacer (absolute 60 Hz grid, presentation half a period later), `--frametimes` recorder + `tools/frametimes.py`; race presentations 94 % at exactly 33.3 ms (were spread over 27–38 ms); remaining late frames traced to the core's JIT cost; smoke/glow defect reproduced, cause not identified; ADR 0006; README rewritten for GitHub with legal and AI-assistance disclosures (owner's request). |
| **R1 static recompiler** | **next** | spike on a few functions → ADR 0007 (code generator) → full translation checked against the reference core |

## Decisions (ADRs)

| ADR | Title | Decision in one line |
|---|---|---|
| [0001](adr/0001-architecture.md) | Architecture | Target = static recompilation + native runtime (A′). **M1 ships emulator-backed (Path C1: embedded Play! core, BSD-2, no BIOS), labelled as not native.** Path B rejected for game logic. |
| [0002](adr/0002-language-stack.md) | Language stack | C++20 / MSVC / CMake+Ninja for everything shipped; Python (stdlib) for dev-only recon scripts. |
| [0003](adr/0003-gui-framework.md) | GUI framework | Plain Win32 + Common Controls 6; all logic in `hp2core`; GUI and CLI are thin front-ends. |
| [0004](adr/0004-asset-pipeline.md) | Asset pipeline | Names from a table recovered from the dump; unpack = exact 1:1 extents + manifest with SHA-256; resumable; ROM read-only. |
| [0006](adr/0006-recompilation-main-line.md) | Direction after the M1 trial | **Static recompilation + purpose-built runtime is the product (stages R0–R4); the Play!-based launcher is the reference oracle and fallback.** No game code is ever distributed: translation happens in the converter on the user's machine. |
| 0007 | Code generator for the recompiler | to write after the R1 spike (embedded LLVM vs the reference core's BSD code generator vs own x64 emitter) |
| [0005](adr/0005-rendering-audio.md) | Launcher (now: reference build) | Play! core libraries linked statically (source unmodified); its OpenGL renderer on our WGL context; WaveOut audio; XInput + keyboard; virtual disc from the ROM folder; all data in `game/build/data`; core's 60 Hz limiter on. **Races are drawn at 30 fps by the game itself, menus at 60** (measured; unverified on a console). |

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

Runtime (P4b/P5, `docs/evidence/m1-acceptance.md`):

- Play! `1f27232` runs SLUS-20362 from the owner's files: memory-card check, intro movie, title, menus, Quick Race, pause menu, Save/Load (creates `BASLUS-20362HP2` on the virtual memory card; next start loads it).
- The game finds files **by name**: a rebuilt image / the virtual disc with different sector positions works.
- **Corrected (P6):** "2.5–5× headroom" was an average over busy and idle refreshes. The game does a 30 fps frame's work inside one refresh; emulating that busy refresh costs ≈ 13 ms on one thread of the core's JIT (≈ 1.25× headroom) and sometimes misses 16.7 ms. This is the stutter the owner saw with CPU/GPU under 20 %.
- **Method lesson (P6):** per-second averages hide stutter. Always record per-frame intervals (`--frametimes`, `tools/frametimes.py`) and report p95/p99/max and the count of off-grid intervals.
- Launcher pacing (P6): the core's limiter is off; `FramePacer` holds frame ends to an absolute 1/60 s grid on the VM thread and presents half a period later on the GS thread. `--core-limiter` restores the old behaviour for comparison.
- Rendering defect (open): tyre smoke and brake-light glow are drawn as flat translucent polygons by the core's OpenGL renderer. Cause not identified; the renderer has no mip-level support (candidate, unverified).
- Races: 30 frames drawn per second, unchanged by a 1.5×/2× EE clock or 2×/3× instructions per tick → the game's own pacing.
- An unnecessary `CDROM0_SyncPath()` call from the launcher right after `Initialize()` crashed the core; removed (do not reintroduce).
- Play!'s OpenGL `GetScreenshot()` is a stub → the launcher reads the back buffer itself.
- Play!'s `ResetVM()` re-opens the disc from its path preference → the launcher re-mounts the virtual disc in `BeforeExecutableReloaded`.

## Environment facts (development PC)

- Windows 11, MSVC 14.44 (VS Build Tools 2022 17.14) + Windows SDK 10.0.26100, bundled CMake 3.31 + Ninja (not on PATH; `tools/build.ps1` loads `vcvars64.bat`).
- Repo: branch `main` tracking `origin/main` (`https://github.com/awtocl/HotPursuit2PC`). **Nothing has been pushed.**
- ROM (read-only, git-ignored): `ROM/Need for Speed - Hot Pursuit 2 (USA)/`.
- Third-party source (git-ignored): `.cache/third_party/Play-` (pinned, must stay unmodified). `.cache/third_party/Play-fetchtest` is a second, identical copy made to test the fetch script (removable).
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
| Q6 | Does the owner remember races at 30 or 60 fps on the console? Does handling/speed feel right? | R3 | still open — the owner has not said |
| Q7 | Audio: the owner reported sound problems but not which (crackle, cuts, sync, pitch?) | R0/R4 | **ask the owner; then measure un-muted** |
| Q9 | Does the pacer (P6) remove the constant judder for the owner? Which monitor refresh rate / VRR? | R0 | owner to re-try `.cache\m1-run\game\build\HotPursuit2.exe` |
| Q8 | Does the game use DualShock 2 button pressure (throttle/brake)? Not emulated by the pad handler. | M1 sign-off / M2 | observation by the owner |

## Next steps

1. **R1 spike** (ADR 0006): a `recomp/` component — MIPS R5900 decoder and function discovery over `SLUS_203.62`
   (1.65 MB `.text`, no symbols), validated against the code addresses the reference core actually executes in the
   scripted session (the core's executor knows every block it compiled). Then translate a handful of functions with
   each candidate code generator and compare → ADR 0007.
   Starting points inside the reference core: its MIPS instruction semantics (`Source/MA_*.cpp`, `ee/`), its BSD
   code generator (`deps/CodeGen`, IR "Jitter" with an x64 back end and object-file writers), and the
   `USE_AOT_CACHE` / `BUILD_AOT_CACHE` build options.
2. R1 proper: whole-program translation at conversion time, executed in the launcher instead of the JIT; lock-step
   comparison with the reference; acceptance numbers in ADR 0006.
3. R2–R4 as in ADR 0006. Milestone 2 features (anti-aliasing first — a true DLAA becomes possible only once we own
   the projection and can produce motion vectors, i.e. in R3) follow on the recompiled build.
4. Before any of it if the owner prefers: characterise the sound problem (Q7) and the smoke/glow defect.
