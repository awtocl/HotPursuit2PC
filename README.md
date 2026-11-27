# HotPursuit2PC

A converter that takes **your own dump** of *Need for Speed: Hot Pursuit 2*
for the PlayStation 2 (NTSC-U, `SLUS-20362`) and builds a folder you can play
from on Windows 10/11 x64 — the PS2 game itself, not the separate 2003 PC port.

## Why this exists

> **The best version of this game is the PlayStation 2 one — so that is the
> version this project brings to the best platform.**

*Hot Pursuit 2* shipped on several systems, and the PS2 release is the one
worth keeping: its handling, its cars, its tracks and its feel are the
reference. This project is not interested in a lookalike; it takes that exact
game and makes it a first-class citizen on a modern PC: native code, your
monitor's resolution, your controller, your hardware — with nothing of the
original console left in the way.

## Status

> **Development build — work in progress (2026-11-27).** Two things exist today:
>
> 1. **The native game** (`native/`, `HotPursuit2Native.exe`), the real goal:
>    the game's program recompiled to x64 and run with **no emulator code at
>    all**. **It plays**: intro movies with sound, menus, races, music, sound
>    effects, police radio, pause, keyboard and Xbox-style controllers, a GPU
>    renderer up to 8x internal resolution, 16:9 and 60 fps by default. It is a
>    development build: see [Known problems](#known-problems).
> 2. **A converter and a launcher** that play the game on top of an embedded
>    emulator core. This was the first milestone and is now the *reference* the
>    native game is compared against; it is not the goal.
>
> **You need your own copy of the game.** Nothing of the game is in this
> repository; every tool here works only from a dump you made of your own disc.
>
> The honest way to follow progress: [`docs/STATUS.md`](docs/STATUS.md) and
> [`docs/HANDOVER.md`](docs/HANDOVER.md).

---

## Contents

- [What this is — and is not](#what-this-is--and-is-not)
- [Legal position and provenance](#legal-position-and-provenance)
- [AI assistance disclosure](#ai-assistance-disclosure)
- [What works today](#what-works-today)
- [Known problems](#known-problems)
- [Using it](#using-it)
- [Building from source](#building-from-source)
- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [Documentation](#documentation)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License and third-party software](#license-and-third-party-software)

---

## What this is — and is not

**It is** a native build of the PS2 game made on your PC from your own disc
(`tools/install-native.ps1`, see [Using it](#using-it)), plus a build tool
and a reference launcher:

- `HotPursuit2PC.exe` / `hp2pc-cli.exe` — the **converter**. You point it at
  the extracted contents of your disc. It verifies every file against known
  sizes and SHA-256 hashes, organises everything inside a single `game\`
  folder next to itself, unpacks the game's asset archive, and installs the
  launcher.
- `HotPursuit2.exe` — the **launcher** the converter installs into
  `game\build\`. It runs the game from your files.

**It is not:**

- a download of the game. No game code, art, audio, video or data is in this
  repository or in its releases, and none is fetched from anywhere. Without
  your own disc dump the tools do nothing;
- an emulator. The native game (`HotPursuit2Native.exe`) contains no emulator
  code: the game's program is recompiled to x64 on your PC. Only the
  *reference* launcher (`HotPursuit2.exe`) runs the original program inside
  the open-source [Play!](https://github.com/jpd002/Play-) emulator core,
  which is linked into it; that is stated in its window title, in the README
  the converter writes, and here;
- the 2003 Windows release of the game. That is a different port with
  different handling and features, and nothing is taken from it.

Only the North American PlayStation 2 release, `SLUS-20362` v1.00, is supported.

## Legal position and provenance

Please read this section before using or redistributing anything.

**No Electronic Arts property is in this repository.** Every source file
here was written for this project and is released under the MIT license
(see [`LICENSE`](LICENSE)). The repository contains no code, assets, text,
audio, video or other material owned by Electronic Arts Inc., Sony
Interactive Entertainment, or any other rights holder of the game, and no
part of the game's executable in any form (original, disassembled,
decompiled or translated).

So that this can be checked, here is everything in the repository that was
*derived from* the disc — all of it facts about the disc, none of it content:

| File | What it holds |
|---|---|
| `converter/data/slus20362_rom_manifest.tsv` | names, sizes and SHA-256 hashes of the 27 files on the disc — used to tell a good dump from a damaged or different one |
| `assets-pipeline/data/hp2_slus20362_names.txt` | the names of 362 files inside the game's archive, recovered so that unpacked files get readable names |
| `tests/fixtures/slus20362_zfileset_inventory.tsv` | per-file offsets, sizes and SHA-256 hashes inside that archive, used by the tests |
| `docs/dump-analysis.md` | a description of the archive's container format and the full file inventory |
| `docs/recomp-analysis.md`, `recomp/tests/test_recomp_dump.cpp` | measurements of the game program: section addresses and sizes, counts of instructions, functions and switch tables, a handful of code addresses, timing of the reference build. No instructions or bytes of the program itself. |

The test programs and status documents repeat some of these numbers.

**You must bring your own copy.** To use these tools you need a dump of a
copy of the game that *you* legally own, made by you from your own disc. This
project does not provide, link to, or help locate game files, disc images or
console firmware, and will not. No PlayStation 2 BIOS is needed or used. The
laws on making and using backup copies of software you own differ between
countries; you are responsible for complying with the ones that apply to you.

**How it was tested.** All development and testing of this project was done
with a single dump: the one the repository owner made from the physical
retail disc of the game in their possession. No other copy of the game, no
downloaded image and no material from the Windows release was used at any
point. The converter opens the dump read-only and never modifies it; the
test-suite checks this.

**Translation happens on your machine.** Anything the tools derive from the
game — unpacked assets today, recompiled code in the future — is produced on
the user's own computer from the user's own dump and stays in the user's
`game\` folder. Do not redistribute that folder.

**No affiliation.** This is an independent, non-commercial preservation and
interoperability project. It is not affiliated with, sponsored by or endorsed
by Electronic Arts, Sony, or the authors of the open-source software it
builds on. *Need for Speed* and *Hot Pursuit 2* are trademarks of Electronic
Arts Inc.; *PlayStation* is a trademark of Sony Interactive Entertainment Inc.
They are used here only to say what the tools are compatible with.

If you are a rights holder and believe something here should not be, please
open an issue on this repository.

## AI assistance disclosure

This project was built with substantial assistance from artificial
intelligence, and you should know exactly how:

- **The source code, tests, build scripts and documentation in this
  repository were written by an AI coding agent** — Anthropic's Claude,
  running in Claude Code — working at the direction of the repository owner.
  Commits carry a `Co-Authored-By: Claude` trailer.
- **The repository owner** defined the goals and hard constraints (kept in
  [`docs/brief.md`](docs/brief.md)), supplied the dump of their own disc,
  approved each step that reached outside the project (such as fetching
  third-party source code), tries the builds, and decides the direction. The
  decision to move from the emulator-backed build to recompilation, for
  example, is theirs, made after playing it.
- **Reverse-engineering findings were derived from the owner's dump** by
  inspection and experiment, and are recorded with the evidence for each
  claim in [`docs/dump-analysis.md`](docs/dump-analysis.md). The AI was not
  given, and did not use, the game's source code or any third party's
  reverse-engineering data; where existing tools were consulted, that is
  listed with their license status in [`docs/prior-art.md`](docs/prior-art.md).

What this means for you:

- AI-written code can be wrong in ways that look confident. The project's
  defence is process rather than trust: every claim about the disc is backed
  by a byte-level check or a test; anything not proven is marked
  **UNVERIFIED** in the docs; decisions are written down as ADRs with the
  alternatives that were considered; and each phase ends with a report that
  lists what was *not* verified.
- That process has caught real mistakes, and they are left visible on
  purpose — for example a hash collision that briefly gave a file the wrong
  name, and a performance claim based on per-second averages that hid the
  stutter the owner later saw (both are written up in the docs).
- No human has audited the whole code base line by line. Review it before
  you rely on it, and treat the numbers in the docs as measurements on one
  PC, not guarantees.

## What works today

| Area | State |
|---|---|
| ROM validation | Done. 27 files checked by size and SHA-256; wrong disc, missing/truncated/modified files and wrong folder each get a specific message. The dump is never written to. |
| Game folder | Done. Portable (next to the converter, relocatable) or installed (a folder you choose); re-running resumes and never overwrites silently; one log per run. |
| Asset unpacking | Done. The game's 3 GB archive is fully understood (directory format, name hash, LZ compression) and unpacked into 402 files, 362 of them with their original names. |
| Disc image | Done. `hp2pc-cli image` rebuilds an ISO 9660 image from the validated folder (verified with an independent reader). |
| Native game | **Plays.** Recompiled game program (no emulator), intro movies with their sound, menus, races, pause; music, sound effects and police radio through an SPU2-style mixer; keyboard and XInput controllers; GPU renderer (Direct3D 11) with 1x–8x internal resolution (supersampled above the screen's), 16:9 by default through the game's own widescreen patch, 60 fps by default; settings and Quit game on F1. |
| Reference launcher | **Works, with problems.** Boots, plays movies, menus, races, pause, and saves to / loads from a virtual PS2 memory card. Emulator-backed. |
| GUI and CLI | Done. Same operations in both. |
| Recompiler, analysis half | Done. Every one of the 431,681 instruction words of the game program decodes; 10,146 functions and 193 switch tables are found without symbols; in a traced session every one of the 29,860 code blocks the embedded core executed starts where the analysis predicted. |
| Recompiler, translator | **Done for what the game runs** (the native game runs entirely on it). Earlier measurement, against the reference: 3,692 game functions (99.8 % of what the game executes in a test session) are translated through LLVM to x64; the test session runs on them inside a measurement build, with the embedded core still handling what is not translated yet (the PS2's second vector unit, a few instructions). Identical results to the core on every compared call. |
| Automated tests | 15 CTest tests, including runs against a real dump when one is present (skipped otherwise) and the embedded core's own JIT test suite. |

## Known problems

Native game (development build, tested on one PC):

- **Saving**: the memory card is not implemented yet (the game runs as with no card).
- **Sound**: no reverb and no real ADSR envelopes yet, so sounds end more abruptly than on a console.
- **Graphics**: no depth fog, mipmaps, dithering or lines yet; the race's distance fog is skipped.
- **Input**: digital buttons only (no DualShock 2 pressure), no rumble, no steering wheel.
- Switching between 16:9 and 4:3 takes effect after a restart (the game lays out its HUD once).
- Not tested on AMD/Intel graphics, on a clean Windows installation, or for long sessions.

Reference launcher: stutter on heavy frames (embedded core), flat tyre smoke and brake-light glow,
4:3 only, races at 30 fps; details in [`docs/STATUS.md`](docs/STATUS.md).

## Using it

There are no releases yet; build from source (next section). Every route
starts from **your own dump**: the extracted contents of your own disc, a
folder that holds `SYSTEM.CNF`, `SLUS_203.62` and the `NFSHP2` folder. If
you made an `.iso` of your disc, double-click it in Windows to mount it and
use the drive it gets, or extract it with a tool such as 7-Zip.

### The native game

```powershell
pwsh tools/install-native.ps1                       # asks for your dump folder
pwsh tools/install-native.ps1 -Rom "D:\My dumps\Need for Speed - Hot Pursuit 2 (USA)"
```

The script checks the folder and that `SLUS_203.62` is the supported version,
builds the project, recompiles the game's program **from your copy, on your
PC**, and writes `game-native\` with `HotPursuit2Native.exe`, the recompiled
code (`hp2code.dll`) and `Play.cmd`. The game reads your dump folder and never
writes to it. In the game: **F1** (or Select+Start) opens the settings
(renderer, internal resolution, frame rate, display mode, V-Sync, aspect
ratio, filters) and *Quit game*; Alt+Enter toggles full screen. Settings are
kept in `hp2native.ini` next to the executable. An Xbox-style controller
works as a DualShock 2. Keyboard:

| Key | PS2 button | Key | PS2 button |
|---|---|---|---|
| Arrows | D-pad / steering | Enter, Space | Start |
| Z | ✕ | Backspace, Right Shift | Select |
| X | ○ | Q / W | L1 / R1 |
| A | □ | 1 / 2 | L2 / R2 |
| S | △ | F1 | settings |

### The reference launcher (emulator-backed)

1. Dump your own disc and extract its contents to a folder. It must contain
   `SYSTEM.CNF`, `SLUS_203.62` and the `NFSHP2` folder.
2. Put `HotPursuit2PC.exe` (or `hp2pc-cli.exe`) in an empty folder and run it.
3. Select the folder from step 1 and choose *portable* or *installed*.
4. Convert. You get:

   ```
   game\
     hp2pc.cfg        settings and conversion state
     rom\             your disc files (copied and verified), or a pointer to them
     assets\          the unpacked game archive (402 files) + manifest.tsv
     build\           HotPursuit2.exe, README.txt, THIRD-PARTY-NOTICES.txt
     logs\            one log per conversion run
   ```

5. Play: `game\build\HotPursuit2.exe`. Xbox-style controller or keyboard;
   Alt+Enter toggles full screen; saves go to a virtual memory card in
   `game\build\data\`. `game\build\README.txt` lists controls and options.

Command line:

```
hp2pc-cli validate "D:\dumps\Need for Speed - Hot Pursuit 2 (USA)"
hp2pc-cli convert --rom "D:\dumps\Need for Speed - Hot Pursuit 2 (USA)"
hp2pc-cli status
```

## Building from source

Requirements: Windows 10/11 x64, Visual Studio 2022 (or the Build Tools) with
the C++ workload, and git. CMake and Ninja come with Visual Studio. No other
dependencies and no package manager.

```powershell
pwsh tools/fetch-play.ps1             # downloads the pinned Play! source (BSD-2) into .cache/ (~80 MB)
pwsh tools/build.ps1 -Preset release  # builds everything and runs the tests
```

Outputs: `.cache\build\release\converter\HotPursuit2PC.exe` and
`hp2pc-cli.exe` (each carries the launcher inside it). Without the first
command everything except the launcher still builds, and the converter's
last stage reports itself as unavailable.

Optional, for the recompiler's translator (LLVM, Apache 2.0 with LLVM Exceptions; about 171 MB to download,
about 2.5 GB on disk after building, once):

```powershell
pwsh tools/fetch-llvm.ps1
pwsh tools/build-llvm.ps1
```

Without it everything else builds; the translator and its tests are left out.

Tests that need a real dump look for it in `ROM\Need for Speed - Hot Pursuit 2 (USA)\`
or in the `HP2_ROM_DIR` environment variable, and are skipped when it is absent.

## How it works

```
 your disc dump (read-only)
        │  validate: 27 files, sizes + SHA-256, archive structure
        ▼
 game\rom\          copy verified while copying (or a reference)
        │  unpack: ZDIR.BIN + ZZDATA.BIN  →  402 named files
        ▼
 game\assets\
        │  install: launcher + notices + README
        ▼
 game\build\HotPursuit2.exe
        ├─ virtual disc: serves game\rom to the core as DVD sectors (no image file)
        ├─ Play! core: runs the PS2 program (EE/IOP/VU/GS/SPU emulation)
        └─ our window, OpenGL context, audio output, XInput, frame pacer, crash log
```

Things worked out from the dump along the way (full evidence in
[`docs/dump-analysis.md`](docs/dump-analysis.md)):

- `ZDIR.BIN` is 405 twelve-byte records `{name hash, sector, size}`; the
  extents tile `ZZDATA.BIN` exactly.
- The name hash is `h = 0xFFFFFFFF; h = h*33 + byte` over the upper-cased path.
- `.LZC` files are an LZSS stream behind a `COMP` header; the decoder matches
  all eleven files that exist on the disc both packed and raw.
- File names are not stored on the disc. They were recovered from strings in
  the game's own files, each accepted only with independent confirmation —
  a 32-bit hash match alone proves nothing.

## Repository layout

| Path | Contents |
|---|---|
| `assets-pipeline/` | C++ library and tool for the disc's containers (directory, hash, LZC, unpack, inventory) |
| `converter/` | Converter core, CLI and GUI; disc-image builder |
| `engine/` | The launcher and the virtual disc |
| `recomp/` | Static recompiler, analysis half: executable reader, PS2 instruction decoder, function discovery, and the tool that checks them against the embedded core |
| `tests/` | Test harness and fixtures (names, sizes and hashes only) |
| `tools/` | Build helper, Play! fetch script, launcher run/measurement scripts, dev-only analysis scripts |
| `docs/` | Status, decisions, evidence, reports, the original project brief |

## Documentation

| Document | What it is |
|---|---|
| [`docs/STATUS.md`](docs/STATUS.md) | Where the project stands, what is next, open questions |
| [`docs/adr/`](docs/adr/) | Architecture decision records, each with the options that were rejected |
| [`docs/dump-analysis.md`](docs/dump-analysis.md) | Byte-level findings about the disc and the full file inventory |
| [`docs/recomp-analysis.md`](docs/recomp-analysis.md) | What was measured about the game program and the reference build before writing the translator |
| [`docs/prior-art.md`](docs/prior-art.md) | Every third-party source consulted or used, with its license and what was taken |
| [`docs/evidence/`](docs/evidence/) | Measurements and acceptance runs |
| [`docs/gate-reports/`](docs/gate-reports/) | End-of-phase reports: verified, not verified, what the owner must decide |
| [`docs/brief.md`](docs/brief.md) | The owner's original project brief and constraints |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Rules, workflow, tests, code style |

## Roadmap

Decided in [ADR 0006](docs/adr/0006-recompilation-main-line.md). Each stage
ends with a build the owner can play and numbers comparable with the stage
before it; the emulator-backed build stays as the reference the new code is
checked against.

1. **Static recompilation of the game program and of its vector-unit
   microprograms.** The converter translates the game's PS2 code to x64 ahead
   of time, on the user's machine. No code is compiled while playing. Checked
   by running it in lock-step against the reference core.
   *Analysis done* (function discovery validated against a traced session);
   *translator prototyped* with LLVM ([ADR 0007](docs/adr/0007-code-generator.md)):
   identical results to the reference on real calls, 4.1x faster.
2. **Host services instead of emulated hardware** where the game calls its
   libraries: disc reads straight from files, controller with analog
   triggers and rumble, memory card, timers.
3. **Rendering.** First correct (the smoke and glow defects are the first
   test cases), then the things an emulator cannot do well: resolution
   independence, anti-aliasing with real motion vectors, widescreen, an
   optional 60 fps race mode.
4. **Audio** mixed natively.

The modern graphics settings are to live inside the game's own options menus,
not in a separate launcher window (owner's requirement). Other
quality-of-life features (save states, remapping) follow, each opt-in where
it changes original behaviour.

Where it stands (2026-11-27): stages 1–4 are in the native game — the whole
program and its vector-unit microprograms recompiled, the I/O side served by
host services, a GPU renderer with 16:9 and 60 fps, a native mixer. Open:
memory card saves, reverb and envelopes, depth fog and mipmaps, analog
pressure and rumble, and a friendlier installer than a PowerShell script.

This is a long road and there is no promise of dates.

## Contributing

Read [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`docs/STATUS.md`](docs/STATUS.md)
first. Two rules are absolute: never commit game data (CI rejects it), and
never state something about the disc or the game without the evidence for it.

## License and third-party software

The code and documentation in this repository are released under the
[MIT License](LICENSE).

The launcher links, statically, the Play! emulator core (BSD 2-Clause) and
the libraries it bundles: Play! Framework and CodeGen (BSD 2-Clause), libchdr
(BSD 3-Clause), LZMA SDK (public domain), Zstandard (BSD), zlib (zlib
license), xxHash (BSD 2-Clause), bzip2 (bzip2 license) and GLEW (Modified
BSD / MIT). Their source is not in this repository; `tools/fetch-play.ps1`
downloads pinned revisions at build time, and their license texts are written
next to the launcher as `THIRD-PARTY-NOTICES.txt`. Details and revisions:
[`docs/prior-art.md`](docs/prior-art.md).
