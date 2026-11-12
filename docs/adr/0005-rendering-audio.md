# ADR 0005 — Launcher: hosting the core, rendering, audio, input, frame pacing

- **Status:** Accepted (2026-11-12, phase P4b)
- **Depends on:** ADR 0001 (Milestone 1 is emulator-backed, path C1)
- **Constraints in play:** C1, C2, C5, C6, C7, C8

## Context

The go/no-go test of ADR 0001 passed: the Play! core (BSD 2-Clause, pinned at
`1f27232`, 2026-10-24) boots SLUS-20362 from the owner's files and runs it from
the memory-card check to a race (evidence: `docs/evidence/m1-acceptance.md`).
This ADR fixes how `game/build/HotPursuit2.exe` is put together.

## Decisions and the options behind them

### 1. How the core is hosted

| Option | Verdict |
|---|---|
| **Link Play!'s core libraries (`PlayCore`, `gsh_opengl`) statically into our own executable** | **chosen** |
| Build Play!'s libretro core and write a libretro front-end | rejected — that wrapper stores settings and memory cards in `Documents\Play Data Files` (breaks C6), leaves frame pacing to the front-end, and adds an API layer we do not need |
| Ship/drive Play!'s own Qt application | rejected — needs Qt, shows an emulator UI, same data-location problem |

Play! leaves `CAppConfig::GetBasePath()` for the application to define; ours
returns `<game>/build/data`. Nothing is written outside the game folder
(checked in the acceptance run). Play!'s source is **not modified**: the
launcher uses only public members.

### 2. Rendering

| Option | Verdict |
|---|---|
| **Play!'s OpenGL renderer (`CGSH_OpenGL`) on a WGL 3.2 core context we create on our window** | **chosen** — it is what the tracker's "playable" rating was earned on; resolution multiplier comes for free |
| Play!'s Vulkan renderer | not now — needs the Nuanceur submodule and a Vulkan surface; candidate for Milestone 2 if it renders this game better |
| paraLLEl-GS or our own renderer | belongs to the native track (stage N2 of ADR 0001) |

Defaults: internal resolution ×2 (1280×896), window 1280×896, the PS2 picture
kept at its 4:3 shape ("fit"), Alt+Enter/F11 for borderless full screen.
Screenshots for evidence are read from the back buffer by the launcher
(Play!'s `GetScreenshot` is a stub in the OpenGL renderer).

### 3. Audio

Play!'s `CSH_WaveOut` (winmm) — the handler its own Windows build uses.
Alternatives present in the tree: `CSH_XAudio2`; an own WASAPI handler. Not
chosen because nothing measured asks for them yet. The launcher counts the
samples the game produces and logs the rate each second.

### 4. Input

Play!'s `CPH_Generic` pad handler fed once per loop from XInput controller 0
and the keyboard (mapping in `build/README.txt`). Digital buttons only: the
DualShock 2's pressure-sensitive buttons are **not** emulated (**UNVERIFIED**
whether the game uses button pressure for throttle/brake). No rumble yet.

### 5. The disc

| Option | Verdict |
|---|---|
| **Virtual disc**: `engine/vdisc` validates `<game>/rom` (same quick check as the converter), plans the ISO 9660 layout of `hp2/iso.h` and serves sectors straight from the 27 files | **chosen** — no third copy of 3 GB, works when the ROM is referenced instead of copied, and a damaged ROM is reported at start-up |
| Write `disc.iso` into `game/build` | rejected — 3.1 GB of duplication; `hp2pc-cli image` still exists for other uses |

Play! mounts discs from a file path only, so the launcher does the same
three steps through public members (`m_cdrom0`, `CIopBios::GetCdvdman/Cdvdfsv`)
and re-mounts in `BeforeExecutableReloaded`, because the core resets its
drive when a game reloads its executable. This also settles an earlier
unknown: the game finds its files **by name**, so the rebuilt layout (different
sector positions from the original disc) is fine.

### 6. Frame pacing and what "60 fps" means here (C7, C8)

- The core's frame limiter stays **on**: the emulated console produces 60
  vertical blanks per second (NTSC), and the game runs at its original speed
  (lap timer: exactly 10.00 s per 600 frames). `--unlimited` exists only as a
  developer switch.
- Measured: the game draws **menus at 60 frames per second and races at 30**
  (a new frame every second vertical blank).
- That 30 is the game's own pacing, not missing performance. Evidence:

  | Experiment (same scripted race) | Frames drawn per second in the race |
  |---|---|
  | stock core | 30 (29–31) |
  | emulated EE clock ×1.5 (`SetEeFrequencyScale(3,2)`) | 30 |
  | emulated EE clock ×2 | 30 |
  | 2 EE instructions per clock tick (local experiment, reverted) | 30 |
  | 3 EE instructions per clock tick (local experiment, reverted) | 30 |
  | limiter off | emulation runs at 150–330 fps, i.e. 2.5–5× headroom |

  A CPU-starved game would have sped up in at least one of these.
  A 2002 GameSpot review is reported (search summary; the page itself could
  not be fetched) to say the PS2 version "falls short of the 60fps" of a
  contemporary. **UNVERIFIED on a real console.**
- Reading of C7 adopted (C8: keep Milestone 1 honest): the **60 Hz console
  timing is the baseline and is preserved**; the launcher does not alter how
  often the game chooses to draw. Making races draw at 60 would change the
  game's behaviour → a Milestone 2 feature, opt-in and off by default.

## Consequences

- `HotPursuit2.exe` is one static executable (≈ 3.5 MB) depending only on
  Windows system DLLs (OpenGL32, WinMM, XInput1_4, DbgHelp, User32, GDI32,
  Shell32, Kernel32). It is embedded in the converter and written by the
  `build` stage with `README.txt` and `THIRD-PARTY-NOTICES.txt`.
- The build is labelled *emulator-backed* in the window title, the README,
  the converter log and the GUI message.
- A crash handler writes the exception and a stack trace to `data/launcher.log`.
- CI must fetch Play! (`tools/fetch-play.ps1`, pinned revision) to build the
  launcher; without it the converter still builds, with the `build` stage
  unavailable. Play!'s own code-generator test suite runs as part of our CTest.

## Confidence

High that this is the right shape for Milestone 1. Medium on fidelity details
that need the owner's eyes and ears (see the Gate Report's "not verified").

## What would reopen this ADR

- The owner remembers races at 60 fps on the console, or finds handling/timing off.
- Audio problems by ear (crackle, drift) → try `CSH_XAudio2` or an own handler.
- Rendering defects that Play!'s Vulkan renderer does not have.
- The game turning out to need button pressure.
