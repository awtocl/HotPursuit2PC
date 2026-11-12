# HotPursuit2PC

A converter that takes **your own dump** of *Need for Speed: Hot Pursuit 2*
(PlayStation 2, NTSC-U, `SLUS-20362`) and produces a build you can play on
Windows 10/11 x64 — the PS2 game, not the 2003 PC port.

> **Status: early development — nothing playable yet.**
> Current state, evidence and next steps: [`docs/STATUS.md`](docs/STATUS.md).

## What it is (and is not)

- A build tool. You point it at your extracted disc; it validates the files,
  organises everything inside one `game/` folder next to its `.exe`, and
  leaves a launcher there that runs without the converter.
- It contains **no game code, assets or data**. You must own the game and
  supply the dump. Only SLUS-20362 v1.00 is supported.
- How "native" the result is, is staged and stated plainly
  ([ADR 0001](docs/adr/0001-architecture.md)): Milestone 1 is
  **emulator-backed** (an embedded open-source PS2 core); the end goal is the
  game's own code recompiled to x64 on a native runtime.

## Using it

Not released yet — build it from source. What works today (converter v0):

1. Put `HotPursuit2PC.exe` (GUI) and/or `hp2pc-cli.exe` in an empty folder and start it.
2. Select your ROM folder (the extracted disc: `SYSTEM.CNF`, `SLUS_203.62`, `NFSHP2/…`).
   It is validated file by file and never modified.
3. Choose *portable* (everything in `game/` next to the `.exe`) or *installed* (a folder you pick).
4. Convert: the ROM is stored and verified, and the 402 game files are unpacked
   with their names into `game/assets/`. Running it again resumes.

What does **not** work yet: step 5, *run the game*. The launcher arrives with Milestone 1.

```
hp2pc-cli validate "D:\ROMs\Need for Speed - Hot Pursuit 2 (USA)"
hp2pc-cli convert --rom "D:\ROMs\Need for Speed - Hot Pursuit 2 (USA)"
hp2pc-cli status
```

Details: [`converter/README.md`](converter/README.md).

## Building from source

Windows 10/11 x64 with Visual Studio 2022 or Build Tools (C++ workload). No other dependencies.

```powershell
pwsh tools/build.ps1
```

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for rules, tests and code style.

## Repository layout

| Path | Contents |
|---|---|
| `assets-pipeline/` | Readers for the disc's containers (Z fileset, LZC) |
| `converter/` | Converter core, CLI and GUI |
| `engine/` | Launcher/runtime that runs the game |
| `tests/` | Test harness and fixtures (hashes and sizes only) |
| `tools/` | Build helper, dev-only recon scripts |
| `docs/` | `STATUS.md`, ADRs, `dump-analysis.md`, `prior-art.md`, the brief |

## Features (Milestone 2 — none shipped yet)

Each will be opt-in, default-off where it changes the original behaviour,
with its own ADR: anti-aliasing/upscaling first, then resolution and aspect
handling, save robustness, config file, controller options, frame-rate modes.

## Legal

MIT-licensed code (see [`LICENSE`](LICENSE)). Not affiliated with or endorsed
by Electronic Arts or Sony. *Need for Speed* and *Hot Pursuit 2* are
trademarks of Electronic Arts Inc.
