# STATUS — HotPursuit2PC

Single source of truth for project state. A fresh session resumes from this
file alone. Updated in every phase commit. Spec: `docs/brief.md` (brief v3).

**Last update:** 2026-11-14 — phase **R1b-3b (VU1 microcode translated)**: one image of the game's microcode
translated with the reference core's own pipeline model; 0 differences in 5,947,088 compared time slices (14.5 million
graphics packets); the whole race now runs on translated game code, VU0 and VU1 with no hand-over: same race, mean
VM-thread work per refresh 6.22 → 4.48 ms (VU1 2.31 → 1.09 ms), refreshes over 16.7 ms 17 → 4. Next: the converter
stage that compiles once and stores the code, then VU1 on its own thread.
Previous: phase **R1b-3a (VU1 analysis)**: VU1 microcode decoded and measured (54 % of upper and
35 % of lower slots are padding; no MAC/status flag is ever read); ADR 0008 decides how to translate it. Next: the
VU1 translator and its verification.
Previous: phase **R1b-2 (VU0 macro mode translated)**: 0 differences in 1,530 compared calls
including the VU0 state; in the same race, refreshes over 16.7 ms 138 → 17, worst 46 → 28 ms, game program
3.6 ms per busy refresh (reference core 6.1). Next: VU1 microprograms.
Previous: phase **R1b-1 (game running on translated code)**: 3,692 functions (99.8 % of executed
game instructions) translated; the scripted session runs on them; 0 differences in 1,156 compared calls; menu
game-program time 14.3 → 5.6 ms per busy refresh, reference-core compilation 29,935 → 6,649 blocks. Next: VU0
macro mode, then VU1 microprograms, then the converter stage that compiles once and stores the code.
Previous: phase **R1b-spike (translator spike)**. The owner allowed LLVM (Q10, 2026-11-12); the
spike passed: translated game functions match the reference core exactly (0 differences in 1,864 real calls) and
run 4.1x faster; ADR 0007 accepted. Next: the whole-program translator (R1b proper).
**The owner played the Milestone 1 build and did NOT accept it** (stutter, smoke/brake-light glow drawn as translucent
squares, sound problems; wants "much closer to a recompilation than to an emulator trimmed for the game", clearly
better than PCSX2). Direction changed by [ADR 0006](adr/0006-recompilation-main-line.md): **the product is now a
statically recompiled build; the emulator-backed launcher is kept as the reference.** Milestone 2 features are on
hold until the recompiled build exists.

Owner requirements added on 2026-11-12 (they bind every later stage):

1. The game must run **natively and make full use of the PC's hardware**.
2. The modern graphics settings must be implemented, close to native, **inside the game's own options menus**.
3. **Publishing:** push the recompiled build to the GitHub repository only when it works well *and* requirement 2
   is in. Nothing is pushed before that. When both seem met, show the owner the evidence first ("works well" is
   the owner's judgement, as the Milestone 1 trial showed), then push.

R1a result in one line: function-level static recompilation of this game is practical (every block the reference
core executed starts where the analysis predicted), and the cost to remove is one third game program, one third
vector-unit microcode — so R1 translates both ([docs/recomp-analysis.md](recomp-analysis.md),
[ADR 0007](adr/0007-code-generator.md), proposed).

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
| P6 M1 trial follow-up (stage R0 of ADR 0006) | **done** | commit `e5d927d`; launcher frame pacer (absolute 60 Hz grid, presentation half a period later), `--frametimes` recorder + `tools/frametimes.py`; race presentations 94 % at exactly 33.3 ms (were spread over 27–38 ms); remaining late frames traced to the core's JIT cost; smoke/glow defect reproduced, cause not identified; ADR 0006; README rewritten for GitHub with legal and AI-assistance disclosures (owner's request). |
| **R1a recompiler analysis** | **done** | this commit; `recomp/` (ELF reader, R5900 decoder, function discovery, `hp2recomp.exe`); launcher `--trace` + `profile` build preset + `tools/profile.py`, `tools/recon/vucheck.py`; `docs/recomp-analysis.md`: 431,681 words all decode, 10,146 functions, 193 switches, **0 of 29,860 executed blocks start anywhere unpredicted**, code not modified at run time, all executed VU1 microcode is in `.vutext`; time shares of a busy race refresh measured; **pacer defect in 60 fps content found and fixed** (evidence §4b); ADR 0007 proposed; 12 CTest tests pass in dev and release. |
| **R1b-spike translator spike** | **done** | this commit; LLVM 23.1.5 fetched (pinned SHA-256) and built in `.cache`; `recomp/src/lift.cpp` (leaf functions → LLVM → x64); `--trace-counts` fixed (epilogue), `hp2recomp hot` / `lift`; launcher `--lift-check` (profile build); 97 of the 400 hottest functions translated in 0.57 s; 81 compared on 1,864 calls: **0 differences**; **4.1x** faster; 16 trapped on the scratchpad; ADR 0007 accepted; 14 CTest tests pass in dev and release. The VU1 microprogram part of the spike was not done (next). |
| **R1b-1 calls, hand-over, re-entry, run mode** | **done** | this commit; whole functions with calls, hand-over to the reference core at any uncovered instruction, re-entry at 38,624 places, scratchpad and hardware slow path, time slice and waiting loops; launcher `--run-translated`; `docs/recomp-analysis.md` §10 |
| **R1b-2 VU0 macro mode** | **done** | this commit; VU0 vector, integer, Q and control-register instructions with the reference core's clamping, special cases and flag pipelines; flag writes buffered per straight run; `docs/recomp-analysis.md` §11 |
| **R1b-3a VU1 analysis** | **done** | this commit; `recomp/include/hp2/vu.h` (decoder, disassembler, upload packets), `hp2recomp vu1`, VU1 block counts in `--trace-counts`; `docs/recomp-analysis.md` §12; [ADR 0008](adr/0008-vu1-microcode.md) |
| **R1b-3b VU1 microcode translated** | **done** | this commit; `recomp/src/vulift.cpp`, shared vector code in `recomp/src/lift_internal.h`, operand tables in `hp2/vu.h`, `hp2recomp vu1-lift`, launcher `--vu1 check\|run`, `test_vulift` (7 tests); `docs/recomp-analysis.md` §13. **Not verified:** other tracks, cars and game modes than the scripted session (other microcode entries are translated on first use; a refused one runs on the reference core). |
| **R1b translator (rest)** | **next** | converter stage that compiles once and stores the code (today ~3 min at launcher start); VU1 on its own thread (ADR 0008 point 5); branch in a delay slot; then R2 (own runtime) |

## Decisions (ADRs)

| ADR | Title | Decision in one line |
|---|---|---|
| [0001](adr/0001-architecture.md) | Architecture | Target = static recompilation + native runtime (A′). **M1 ships emulator-backed (Path C1: embedded Play! core, BSD-2, no BIOS), labelled as not native.** Path B rejected for game logic. |
| [0002](adr/0002-language-stack.md) | Language stack | C++20 / MSVC / CMake+Ninja for everything shipped; Python (stdlib) for dev-only recon scripts. |
| [0003](adr/0003-gui-framework.md) | GUI framework | Plain Win32 + Common Controls 6; all logic in `hp2core`; GUI and CLI are thin front-ends. |
| [0004](adr/0004-asset-pipeline.md) | Asset pipeline | Names from a table recovered from the dump; unpack = exact 1:1 extents + manifest with SHA-256; resumable; ROM read-only. |
| [0006](adr/0006-recompilation-main-line.md) | Direction after the M1 trial | **Static recompilation + purpose-built runtime is the product (stages R0–R4); the Play!-based launcher is the reference oracle and fallback.** No game code is ever distributed: translation happens in the converter on the user's machine. |
| [0008](adr/0008-vu1-microcode.md) | VU1 microcode | Translate each microprogram with LLVM keeping only what this game's microcode can observe (clip flags, Q, R and the timing they depend on); no MAC/status flags (never read; the translator refuses microcode that reads them); verify at each end bit; later run it on its own thread. |
| [0007](adr/0007-code-generator.md) | Code generator, translation model, scope of R1 | **Accepted 2026-11-13 (spike: 0 differences, 4.1x):** LLVM embedded in the converter (x86 back end only); one host function per game function, re-enterable at resume points, running on the reference machine model with fallback; R1 covers the game program **and** VU1 microcode; acceptance restated (EE + VU ≤ 3 ms per busy refresh, nothing compiled while playing; "3×" moves to R3). Fallback if LLVM is refused: own x64 emitter. |
| [0005](adr/0005-rendering-audio.md) | Launcher (now: reference build) | Play! core libraries linked statically (source unmodified); its OpenGL renderer on our WGL context; WaveOut audio; XInput + keyboard; virtual disc from the ROM folder; all data in `game/build/data`; core's 60 Hz limiter on. **Races are drawn at 30 fps by the game itself, menus at 60** (measured; unverified on a console). |

## What exists now (how to use it)

```powershell
pwsh tools/fetch-play.ps1             # once: pinned Play! source into .cache/third_party/Play- (~80 MB)
pwsh tools/build.ps1 -Preset release  # builds everything, runs the 12 tests
# converter (carries the launcher inside):
#   .cache\build\release\converter\HotPursuit2PC.exe   (GUI)
#   .cache\build\release\converter\hp2pc-cli.exe       (CLI)
# recompiler analysis tool:  .cache\build\release\recomp\hp2recomp.exe  (info | functions | disasm | check-trace)
pwsh tools/build.ps1 -Preset profile -NoTest   # measurement build of the launcher (core's profiler zones), .cache\build\profile
```

Copy the converter into an empty folder, run it on the ROM folder; it creates
`game\` with `rom\`, `assets\`, `build\`, `logs\`, `hp2pc.cfg`. Play with
`game\build\HotPursuit2.exe` (no arguments needed). A converted, played-in
example is at `.cache\m1-run\game\`.

Developer tools: `tools/run-launcher.ps1` (fixed-length run with timed pad
input and screenshots; `-Trace` records what the reference core executed),
`tools/frametimes.py` and `tools/profile.py` (per-frame pacing and time
shares), `hp2assets.exe` (inventory/unpack/lzc/hash), `hp2recomp.exe`,
`tools/recon/*.py` (evidence generators). How to reproduce the R1a
evidence: `docs/recomp-analysis.md` §8.

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
- **Pacer defect fixed in R1a:** with a catch-up limit of four refreshes, 60 fps content (videos, parts of the menus) advanced two refreshes at once every 33 ms (game clock, sound and input in bursts; images still even). The grid now restarts when more than **one** refresh behind; do not raise that limit (the core lets the VM run at most two frames ahead of presentation). Evidence §4b. Whether this was part of the owner's sound problem is unverified.

Recompiler analysis (R1a, `docs/recomp-analysis.md`):

- `SLUS_203.62`: entry `0x100008`, `.text` `0x100000`–`0x2A5904` = 431,681 instruction words, all of which decode (182 distinct instructions; 163 executed in the traced session). ABI eabi64 (64-bit registers, 32-bit pointers). No symbols.
- Discovery finds 10,146 functions and 193 switches (3,077 cases). Not recognised: 3 indirect jumps, all in the C++ exception unwinder at the end of `.text` (never executed in the session).
- Traced session (82 s): 29,860 game-code blocks executed, **all start at predicted leaders**; no game code outside `.text`; `.text` unmodified in memory; the core never recompiled a block. 42 % of the code ran.
- A busy race refresh costs 13.3–15.0 ms on the VM thread: EE 5.6–6.1, **VU1 microcode 4.1–5.8**, VIF1 + GIF 2.2–2.8, SPU + IOP 0.7. The graphics thread uses another ≈ 9–11 ms per game frame on its own core.
- The core compiles ≈ 0.2 ms per block: 29,935 blocks in the session, bursts up to 2,862 blocks (≈ 550 ms). 27 of 69 late race refreshes coincided with compilation.
- All executed VU1 microcode (125 blocks, 1,897 instruction pairs) is in `.vutext`, stored as `MPG` upload packets (8-byte header every 256 pairs).
- The core's executors can be replaced from the launcher without touching its source (`m_EE.m_executor`, `m_VU1.m_executor`; unregister/re-register the EE page-fault handler around the swap). This is the seam for the recompiled code.
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
  (2026-11-12) download and build LLVM into `.cache/third_party/` (Q10).
  Anything else from the network needs a new question.
- Publishing rule (owner, 2026-11-12): see requirement 3 at the top. Until then: local commits only.
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
| Q7 | Audio: the owner reported sound problems but not which (crackle, cuts, sync, pitch?). A pacer defect that made the game clock advance in bursts in videos/menus was fixed in R1a — is it still there? | R0/R4 | **ask the owner; then measure un-muted** |
| Q9 | Does the pacer (P6 + the R1a fix) remove the constant judder for the owner? Which monitor refresh rate / VRR? | R0 | owner to re-try `.cache\m1-run\game\build\HotPursuit2.exe` |
| Q10 | ~~May LLVM be downloaded and built?~~ — **answered yes (2026-11-12)**; 171 MB download, 1.9 GB build tree, 527 MB installed | — | — |
| Q11 | Do other modes upload vector-unit microcode that is not in `.vutext`? Do other modes execute code the analysis treats as unreferenced/orphan? | R1b | no — answered by tracing more sessions |
| Q8 | Does the game use DualShock 2 button pressure (throttle/brake)? Not emulated by the pad handler. | M1 sign-off / M2 | observation by the owner |

## Next steps

1. **Done 2026-11-13:** Q10 answered, LLVM fetched and built, spike passed (`docs/recomp-analysis.md` §9). What was
   planned for it:
   - Yes → `tools/fetch-llvm.ps1` (pinned release, license read and recorded in `docs/prior-art.md` first), build
     into `.cache/third_party/`, then the spike of ADR 0007 Decision 5: translate ~20 hot functions and one
     microprogram, run them through the tracing executor in lock-step with the core's JIT, time them in isolation.
     Hot functions are not known yet: the trace lists what ran, not how often — add execution counters to the
     tracing executor first (a call from each block's prologue, measurement builds only).
   - No → design the own-emitter fallback (ADR 0007 option B) and rewrite the ADR.
2. **Next (R1b proper), in this order:** calls between translated functions with the resume-point model of ADR 0007
   (the largest blocker: 51.5 % of executed instructions sit in functions that call others); the scratchpad;
   VU0 macro mode; the remaining instructions; then one VU1 microprogram through the same comparison. Also useful: an interpreter for the R5900 on top of `hp2/r5900.h`
   (the semantic ground truth the translator is tested against, checked itself in lock-step with the core);
   tracing of calls through pointers and of returns (the block list has no order, so "every return goes back to
   its call site" is not yet proven); more traced sessions (other modes, Q11).
3. R1b proper: whole-program translation as a converter stage (`recompile`), executed in the launcher through the
   executor seam; acceptance in ADR 0007 Decision 4.
4. R2–R4 as in ADR 0006. The graphics settings go into the game's own options menus (owner requirement 2): find
   the front-end's menu definitions in R3 and hook them; anti-aliasing first — a true DLAA becomes possible only
   once we own the projection and can produce motion vectors.
5. Whenever the owner describes it: characterise the sound problem (Q7); the smoke/glow defect stays an R3 test case.
