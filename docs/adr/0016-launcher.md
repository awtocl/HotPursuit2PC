# ADR 0016 — A launcher for players: first-run setup, Play, settings, saves and screenshots

- **Status:** Accepted (2026-11-27)
- **Depends on:** ADR 0009 (code package), ADR 0012 (native runtime), ADR 0013 (settings overlay), ADR 0015 (game patches)
- **Owner requirements in play:** a polished PC experience "similar to Azimuth"; the user provides their own disc; no
  emulator layer; the port's own UI in Hot Pursuit 2's visual language

## Context

Until now a player ran `tools/install-native.ps1`, chose the disc folder in a folder picker, waited in a console and
double-clicked `Play.cmd`. Settings existed only inside the game (F1). There was no place to see whether the disc and the
translated code were in order, nothing to do after an update but run the script again, and no way to manage saves.

Azimuth (the Forza wrapper by Almost There Games) is the reference the owner named: its launcher finds the games, shows
each with **Play** and **Settings**, has graphics presets per game, and offers save backups and a photo gallery (read from
public coverage of its 1.0 release; its own pages were not read). Its look and code were not used.

Two facts make a launcher able to do the whole setup on the player's PC:

- `hp2recomp package` compiles *and links* `hp2code.dll` itself (LLVM and LLD inside the executable): preparing the game
  needs no compiler on the player's PC, only `hp2recomp.exe`. On the development PC: 2.4 s translation, 26–28 s
  compilation (16 threads), 0.1 s link.
- The converter's manifest (`converter/data/slus20362_rom_manifest.tsv`) has the size and SHA-256 of all 27 files of the
  supported disc, so the launcher can check a copy file by file.

## Decision

A new program, `HotPursuit2Launcher.exe` (`launcher/`), sits next to the game in the game folder together with
`HotPursuit2Native.exe` and `hp2recomp.exe`.

- **Drawing:** Win32 + GDI+ (a Windows component; no new dependency), a 1280×720 logical layout scaled to the monitor's
  DPI, its own title bar on a frameless window (DWM shadow and rounded corners). **The game's own front-end style**,
  matched by eye against its main menu and shared with the in-game settings menu (`native/src/hp2_style.*`): a navy
  blueprint grid on near-black, brushed-steel strips (title and ticker), a heavy italic silver page title with a black
  outline (Arial Black, leaned), a red rule with a thin white one under it, menu rows on dark translucent bands with
  condensed bold outlined lettering (Bahnschrift SemiBold Condensed), the focus in orange with an orange underline, small
  list cells like the game's music list. Every image and letter is drawn by the program; no image or font of the game
  is used and none is in the repository.
- **Input:** mouse, keyboard and the first Xbox controller (XInput, loaded at run time): D-pad/stick move, A select,
  B back, LB/RB pages, Start plays.
- **First-run setup (when there is no usable disc):** 1 Disc — the drives that hold the game (a DVD or a mounted `.iso`,
  polled every 2 s) are offered for copying into `<game folder>\disc` (so the disc need not stay in), or the player
  chooses a folder with the disc's files (used in place, read-only); a wrong region or edition is named (from
  `SYSTEM.CNF`). 2 Check — every file against the manifest. 3 Prepare — `hp2recomp package` with a progress bar driven by
  its messages. 4 Ready — on a new installation the picture quality is set for the monitor (the guide's table: up to
  1200 lines 4x, up to 1600 lines 6x, above 8x).
- **Pages:** Play (status of disc and package, PLAY, play time, the in-game keys, the reason when the last session ended
  badly), Settings (the in-game menu's settings from the same `hp2native.ini`, with an explanation per row and the
  recommendation; "Internal resolution" is shown as *Picture quality*), Memory card (what is on the card, back up, restore
  — restoring keeps the current card as a backup first), Screenshots (F12 in the game; the newest also becomes the
  launcher's backdrop), Disc & setup (check all files, prepare again, change disc, desktop shortcut), About (what the
  project is, the AI-assisted note, credits, trademarks, links).
- **Staying in step with updates:** `HotPursuit2Native.exe --check-package` (0.2 s, writes no log) tells whether
  `hp2code.dll` belongs to this runtime and this disc; when it does not, PLAY becomes *PREPARE & PLAY*.
- **Playing:** the launcher hides while the game runs and comes back when it ends (or closes, if the player chose so). The
  game's own log decides whether it ended normally (`Stopped: the window was closed`); otherwise the reason is shown with
  *Open log* and *Report a problem*.
- **The game:** F12 saves the window's picture as PNG (WIC, on its own thread) in `<game folder>\screenshots`; the
  settings table of the overlay is shared with the launcher (`ui::SettingCount`, `ui::Setting`).
- `tools/install-native.ps1` builds and copies the three programs; `-Rom` is optional (without it the launcher asks for
  the disc), and it starts the launcher at the end (`-NoLaunch` not).

## Consequences

- A game folder is self-contained: it could be distributed as a prebuilt download (no game material in it) and a player
  would need no Visual Studio, LLVM build or command line. Publishing such a release is a separate decision of the owner.
- Saves are files under `<game folder>\saves`; whether the game saves and loads a career end to end is still unverified.
- `--selftest` (ctest `launcher_selftest`) draws every screen off-screen; `--screenshot <dir>` writes them as PNG;
  `--setup-from <dir>` runs the setup from a folder as if it were a drive.

## Verified (2026-11-27, the development PC)

- Setup from the ROM folder in an empty game folder: copy 8 s, check of all 27 files 6 s, prepare 22 s; Ready chose
  the picture quality for that screen. Play started the game; the launcher hid and came back after the game window was closed, with the session
  counted. `--check-package`: 0 with the matching package, 5 without the disc. F12 wrote PNG files during the intro.
- Not verified: a real DVD drive or a mounted `.iso` (none here), the restore of a real save, an Xbox controller in the
  launcher, other DPI scales than 100 %.
