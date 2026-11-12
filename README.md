# HotPursuit2PC

A converter that takes **your own dump** of *Need for Speed: Hot Pursuit 2*
(PlayStation 2, NTSC-U, `SLUS-20362`) and produces a build you can play on
Windows 10/11 x64 — the PS2 game, not the 2003 PC port.

> **Status: Milestone 1 reached as an emulator-backed build, awaiting the owner's hands-on check.**
> The converter produces a game folder whose `build\HotPursuit2.exe` boots the
> game to menus and races. It is *not* the native port yet.
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

Not released yet — build it from source. What works today:

1. Put `HotPursuit2PC.exe` (GUI) and/or `hp2pc-cli.exe` in an empty folder and start it.
2. Select your ROM folder (the extracted disc: `SYSTEM.CNF`, `SLUS_203.62`, `NFSHP2/…`).
   It is validated file by file and never modified.
3. Choose *portable* (everything in `game/` next to the `.exe`) or *installed* (a folder you pick).
4. Convert: the ROM is stored and verified, the 402 game files are unpacked
   with their names into `game/assets/`, and the game is installed into
   `game/build/`. Running it again resumes.
5. Play: `game\build\HotPursuit2.exe`. Xbox-style controller or keyboard;
   Alt+Enter for full screen; saves go to a virtual PS2 memory card in
   `game\build\data\`. `game\build\README.txt` has the details.

What you get is the PS2 game running on an embedded open-source PS2 core
(emulator-backed). No BIOS file is needed and nothing is installed on the PC.

```
hp2pc-cli validate "D:\ROMs\Need for Speed - Hot Pursuit 2 (USA)"
hp2pc-cli convert --rom "D:\ROMs\Need for Speed - Hot Pursuit 2 (USA)"
hp2pc-cli status
```

Details: [`converter/README.md`](converter/README.md).

## Building from source

Windows 10/11 x64 with Visual Studio 2022 or Build Tools (C++ workload), and git.

```powershell
pwsh tools/fetch-play.ps1            # pinned Play! core (BSD-2), ~80 MB into .cache/ - needed for the launcher
pwsh tools/build.ps1 -Preset release # converter in .cache/build/release/converter/
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

The launcher contains the Play! emulator core and the libraries it bundles
(all permissively licensed; see [`docs/prior-art.md`](docs/prior-art.md) and
the `THIRD-PARTY-NOTICES.txt` written next to the launcher).

MIT-licensed code (see [`LICENSE`](LICENSE)). Not affiliated with or endorsed
by Electronic Arts or Sony. *Need for Speed* and *Hot Pursuit 2* are
trademarks of Electronic Arts Inc.
