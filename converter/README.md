# converter

The deliverable of the brief's §3: a small Windows program that validates the
user's ROM folder, organises everything inside one `game/` folder next to its
own `.exe`, converts, and (from Milestone 1 on) leaves a build that runs
without the converter. GUI framework decision: [ADR 0003](../docs/adr/0003-gui-framework.md).

| Piece | Output | Status |
|---|---|---|
| `core/` — `hp2core` library: `check_rom`, `inspect_game_folder`, `convert` | — | v0 done |
| `cli/` — headless front-end | `hp2pc-cli.exe` | v0 done |
| `gui/` — Win32 front-end | `HotPursuit2PC.exe` | v0 done |
| Stage `build` (the runnable game) | `game/build/` | **not implemented** — Milestone 1 (phase P4) |

Both executables are static (≈ 0.7 MB each, Release) and depend only on DLLs
that ship with Windows.

## What v0 does

1. **Validate** the ROM folder against the compiled-in manifest of SLUS-20362
   (`data/slus20362_rom_manifest.tsv`: 27 files, sizes, SHA-256): `SYSTEM.CNF`
   says which disc it is; every file must be present with the right size;
   every file up to 64 MiB is hashed (the 3 GB `ZZDATA.BIN` too with the full
   check); the Z fileset must be structurally sound. If the `NFSHP2` folder or
   the folder *above* the disc was picked, the root is corrected and a note
   says so. The ROM folder is only ever read.
2. **Store the ROM**: copied into `game/rom/` and verified by SHA-256 while
   copying (portable and default), or referenced in place (installed mode only).
3. **Unpack the assets** into `game/assets/` (see `assets-pipeline/`).

## The game folder (C6)

```
<converter folder>/
  HotPursuit2PC.exe        GUI
  hp2pc-cli.exe            CLI
  game/                    the only thing the converter creates (name chosen at first run)
    hp2pc.cfg              settings + stage state; marks the folder as ours
    rom/                   ROM copy, or rom-reference.tsv
    assets/                unpacked Z fileset + manifest.tsv
    build/                 the runnable game (Milestone 1)
    logs/                  convert-YYYYMMDD-HHMMSS.log, one per run
```

- **Portable**: `game/` (or the name typed at first run) next to the `.exe`,
  always with its own ROM copy. The folder can be moved; a re-run uses the
  internal copy if the original ROM is gone.
- **Installed**: same layout in a directory the user picks; the ROM may be
  referenced instead of copied.

## Safety rules implemented (and tested)

- Never writes into the ROM folder; refuses a game folder inside it (C2).
- Refuses a non-empty folder that is not a game folder; nothing is changed.
- Files appear under their final name only when complete (`.part` + rename).
- Re-running detects finished work and rewrites nothing; cancelling keeps
  what is complete (brief §3.6).
- A file that exists but does not match is never overwritten silently: it is
  an error, or with *repair* it is moved aside as `<name>.mismatch-N` first.
- Errors name the stage and the file and say what to do (brief §3.3).

## CLI

```
hp2pc-cli validate <rom_dir> [--full]
hp2pc-cli convert  --rom <rom_dir> [--name <folder> | --install-dir <dir>]
                   [--rom-storage copy|reference] [--full-check] [--repair] [--no-assets]
hp2pc-cli status   [--name <folder> | --game <dir>]
hp2pc-cli image    --rom <rom_dir> --out <file.iso>
hp2pc-cli version
```

`image` rebuilds an ISO 9660 disc image from the validated ROM folder
(`core/include/hp2/iso.h`) — the form any PS2 runtime boots. It is a separate
command for now; `convert` will call it once the Milestone 1 runtime is chosen.
Verified with an independent reader (7-Zip): 27 files, all SHA-256 identical
to the ROM. Not yet verified: the game booting from it.

Exit codes: 0 ok, 1 failed, 2 bad usage, 3 ROM folder not usable, 4 cancelled (Ctrl+C).

## Tests

- `test_core` — synthetic disc + profile: every validation message, folder
  rules, layout, resume, cancel, repair, relocation.
- `test_core_dump` — real dump (skipped when absent): quick and full check,
  root correction, refusal to write into the ROM folder.
- `test_iso` — disc images from synthetic files: byte-exact read-back, determinism,
  descriptor fields, multi-sector directories, rejected names/sizes, damaged images.
- `gui_selftest` — window and controls exist, mode switching enables the right inputs.
- `gui_convert_dump` — drives the real window through a real conversion of the
  dump (reference mode, no asset unpacking) and checks log, progress and
  status afterwards (skipped when the dump is absent).
