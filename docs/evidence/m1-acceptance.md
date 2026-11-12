# Milestone 1 — acceptance evidence (2026-11-12)

Everything below was run on the development PC (Windows 11, an OpenGL 3.2
context) with the owner's dump. Screenshots show
copyrighted game imagery and are therefore **not** in the repository; they
are in the git-ignored folders named below.

Reproduce: `pwsh tools/fetch-play.ps1`, `pwsh tools/build.ps1 -Preset release`,
then the commands in each section.

## 1. Conversion from a clean folder

Folder `.cache/m1-run/` containing only `HotPursuit2PC.exe` and `hp2pc-cli.exe`
(Release build), then `hp2pc-cli convert --rom "<ROM folder>"`:

```
ROM stored: 27 file(s) copied and verified by SHA-256, 0 already present.
Assets unpacked: 402 file(s) written, 0 already present, 0 repaired (365 of 405 names known).
Runnable game installed: '...\.cache\m1-run\game\build\HotPursuit2.exe'.
Note: this is an EMULATOR-BACKED build (...). It is not a native port. See build/README.txt.
  [done   ] Check the ROM                (quick check)
  [done   ] Store the ROM                27 of 27 files present in rom/
  [done   ] Unpack the game assets       402 files in assets/ (manifest.tsv written)
  [done   ] Install the runnable game    build/HotPursuit2.exe (emulator-backed build)
```

- Next to the converter afterwards: `game`, `HotPursuit2PC.exe`, `hp2pc-cli.exe` — nothing else (C6).
- `game/build/`: `HotPursuit2.exe` (3,567,616 bytes, byte-identical to the built launcher), `README.txt`, `THIRD-PARTY-NOTICES.txt`.

## 2. Go/no-go and exploration runs

| Run (folder under `.cache/`) | What it showed |
|---|---|
| `p4b-boot` | boots from the rebuilt ISO: memory-card check screen, intro movie with the EA TRAX overlay |
| `p4b-menu` | title screen → main menu → "Hot Pursuit" menu → pre-race camera → race with full HUD; lap timer 8.85 s at frame 3303 and 18.85 s at frame 3903 (10.00 s per 600 frames) |
| `p4b-vdisc` | boots from the ROM **folder** through the virtual disc (no image file) |
| `p4b-ee-3_2`, `p4b-ee-2_1`, `p4b-ipc-2_1`, `p4b-ipc-3_1` | race still draws 30 frames/s with a faster emulated CPU (ADR 0005 §6) |
| `m1-run/explore2` | Options menu: Audio / Controller / Display / Save/Load / Player Names / Credits |
| `m1-run/explore3` | Save/Load → Save: creates `data/vfs/mc0/BASLUS-20362HP2/` (`BASLUS-20362HP2` 70,880 B, `icon.sys` 964 B, `nfs6.ico` 61,048 B); a second Save asks "Overwrite existing game settings…?" |
| `m1-run/explore4` | next start shows "Loading game settings." — the save is read back |

## 3. Acceptance session (the installed game, default settings, sound on)

`game\build\HotPursuit2.exe --frames 9000 --shots <dir> --shot-every 950 --script "<timed pad input>"`
— no path arguments: ROM from `..\rom`, data in `.\data`, window 1280×896, internal resolution ×2, limiter on.
Scripted input: skip intros → Hot Pursuit → Quick Race → race; accelerate and steer; **pause at frame 5500, resume at 5900**; drive on; stop at frame 9000 (the same clean shutdown path as closing the window).

| Measure | Result |
|---|---|
| Duration / exit | 155 s wall, **exit code 0**, no crash handler output |
| Emulated refresh, all 151 logged seconds | 59–61 Hz in 140; 58 ×2, 57 ×1, 55 ×2, 53 ×1, 51 ×1, 40 ×1, 36 ×1, 33 ×1, 5 ×1 |
| Where the dips are | frames 641, 2020–2789 (loading the race), plus 3383 (57 Hz) and 4213 (53 Hz) during the race |
| Emulated refresh while racing (85 s) | 60 ×55, 59 ×14, 61 ×12, 58 ×2, 57 ×1, 53 ×1 |
| Frames drawn by the game while racing | 30 ×75, 29 ×5, 31 ×3, 28 ×1, 26 ×1 per second |
| Audio produced | mean 43,297 Hz of 44,100 nominal (min 4,500 in a loading second) |
| Pause | screenshot at frame 5700 shows the PAUSE menu (Resume Race / Restart Race / Options / View Replay / Quit Race); earlier run: lap timer advanced 9.99 s over 1000 frames containing a 400-frame pause |
| Files written | only under `game/build/data/` (config.xml, launcher.log, vfs/…) |
| ROM | original ROM folder and `game/rom`: sizes and timestamps of all 27 files unchanged |
| `Documents\Play Data Files` | does not exist |

Log: `.cache/m1-run/acceptance/launcher.log`; screenshots `frame_000950 … frame_008550.png` in the same folder.

## 4. Automated tests

`pwsh tools/build.ps1` (dev) and `-Preset release`: 10 CTest tests pass —
`test_assets`, `test_dump`, `CodeGenTestSuite` (Play!'s JIT test suite),
`test_core`, `test_core_dump`, `test_iso`, `gui_selftest`, `gui_convert_dump`,
`cli_usage`, `selftest`.

## 4a. Addendum — the owner's trial and what per-frame measurement showed (2026-11-12)

**The owner played the build and did not accept it**: unstable frame time with a lot of stutter (videos, menus
and races), stutter when moving between menus, the glow of brake lights and tyre smoke drawn as translucent
squares, sound problems, all while CPU and GPU load stayed under 20 %.

The owner's session left a clean log (2 min 28 s, exit code 0, 59–61 Hz in most seconds). **The acceptance
measurements in §3 were per-second averages and could not see the problem.** The launcher now records the instant
of every emulated refresh and every presented image (`--frametimes`), analysed by `tools/frametimes.py`.
Same scripted session each time (1280×896, ×2, muted), windows: menus 25–43 s, race 52–75 s.

Presentation intervals (the time between two images reaching the screen):

| Run (`.cache/…`) | Menus | Race |
|---|---|---|
| `diag-base` — as shipped at the gate (core's limiter, present when the work ends) | spread over 13–19 ms and 32–36 ms; **66 % more than 25 % off the median**; p99 138 ms | spread over 27–38 ms (sd 8.3 ms); p99 52 ms; max 182 ms |
| `diag-pacer` — own pacer, images on the 60 Hz grid | 17 ms ×299, 33 ms ×222, 50 ms ×35 | 33.3 ms ×627; **17 ms ×28 and 50 ms ×17** (images one refresh late) |
| `diag-pacer2` — same, presentation half a period after the grid | 17 ms ×297, 33 ms ×220, 50 ms ×32 | 33.3 ms ×643 (94 %); 17 ms ×20; 50 ms ×7; six longer (67–200 ms) |

Emulated refresh intervals in the race: as shipped, median 14.95 ms with 81 % of intervals more than 25 % off
(the limiter alternated ≈ 13 / 20 ms); with the pacer, median 16.67 ms and p95 16.68–16.92 ms.

What this says:

1. The constant jitter was pacing, and is fixed by the launcher's pacer.
2. The remaining late images are CPU cost: the game does the work of a 30 fps frame inside one refresh, and
   emulating that refresh takes about 13 ms on one thread of the core's JIT, sometimes more than 16.7 ms. The
   "2.5–5× headroom" stated earlier averaged busy and idle refreshes and was misleading; the busy refresh has
   about 1.25×. This matches the owner's observation of low overall CPU/GPU load.
3. Longer gaps (100–200 ms) outside loading are first-use compilation of code and shaders.
4. Smoke and brake-light glow are reproduced as flat translucent polygons
   (`.cache/diag-pacer2/shots/frame_003000.png`, `frame_004050.png`). Cause not identified. The core's OpenGL
   renderer has no mip-level support and its compatibility tracker lists a mip-mapping issue for this game, but
   that this is the cause is **UNVERIFIED**.
5. The sound problem is not characterised yet (the measurement runs were muted).

Consequence: ADR 0006 — the recompiled build becomes the product; this build becomes the reference.

## 5. What this evidence does not cover

- No human has played it: feel of the handling, audio quality by ear, and
  visual correctness beyond the screenshots are unjudged.
- One session of 2.5 minutes, one track, Quick Race only. No full race, no
  other modes (You're The Cop, Championship), no split-screen.
- Only this PC and GPU. Not a clean Windows machine.
- The lower edge of the rear-view mirror shows a striped band in screenshots;
  unknown whether the console shows the same.
- Analog pressure buttons, rumble, second controller: not implemented or not tested.
