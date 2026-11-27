# Changelog

Everything that changed, newest version first. While the project is young (0.x), a new minor version means "a big step";
there are no promises about compatibility between them yet.

Downloads are on the [Releases page](https://github.com/awtocl/HotPursuit2PC/releases). No game files are ever included:
you need your own disc of the North American PS2 release (SLUS-20362).

## [0.2.0] — 2026-11-27 — The native port

The first version you can download and play. The game runs as native Windows code (no emulator), translated on your
PC from your own disc.

### Added

- **Native game** (`HotPursuit2Native.exe`): the game's own code and its vector-unit microprograms are translated to
  x64 with LLVM. `hp2recomp.exe` makes `hp2code.dll` from your `SLUS_203.62`, on your PC. The console's hardware is
  replaced with PC services: kernel and threads, DMA, VIF1 and GIF, disc and files, a memory card kept as files, and
  controllers.
- **Direct3D 11 renderer**: internal resolution from 1x to 8x (averaged down to your screen, so higher is smoother),
  sharp bilinear, bilinear or nearest scaling, sharpening, scanlines and **anti-aliasing (FXAA)**. Borderless full
  screen, and movies fill the screen.
- **16:9 and 60 fps** by default, thanks to the community's patches for this game. Both can be switched off.
- **Sound**: EA Trax music, engines and effects, the police radio and the movies' sound, from a mixer modelled on the
  PS2's sound chip.
- **Launcher**: finds your disc (DVD drive or mounted `.iso`), copies and checks it, prepares the game, and then starts
  it with one click. Settings with a recommendation for your screen, memory card backups and a screenshot gallery
  (F12). Works with mouse, keyboard or controller, in the game's own front-end style.
- **Controls**: a Classic (PS2) and a Modern (triggers) controller layout; every key and button can be remapped, and
  the defaults come back with one click.
- **Six languages** for the launcher and the in-game menu: English, Spanish, Portuguese (Brazil), French, German and
  Italian.
- **In-game settings menu** (F1, or Select+Start), including *Quit game*.

### Fixed

These came up while testing the first native builds:

- **Races hold a steady 60 fps at high internal resolutions.** Every frame the game reads a few depth pixels around
  the sun to dim its glare, and the renderer used to wait for the graphics card to finish the whole frame before
  answering. At 6x a race dropped to about 40 frames per second. Zone Freeze and Zone Zoom skip that test, which is why
  they stayed smooth. The answer now comes from a frame or two earlier, without waiting.
- About 40% less work per frame on the game's thread in races.
- **Music and the police radio pause with the pause menu** and carry on where they stopped. Before, the song kept
  playing under the menu, ran out early and the game skipped to the next one.

### Known problems

- Saving and loading a career have not been tested from start to finish.
- No reverb or volume envelopes in the sound yet; no depth fog, mipmaps, dithering or lines in the graphics yet.
- No rumble or steering wheel support. The game reads throttle and brake as on/off buttons.
- The game's own text is in English (that's how this disc ships); the launcher and the menu follow your language.
- Switching between 16:9 and 4:3 takes effect after a restart.
- Tested on one PC so far. The programs aren't signed, so Windows SmartScreen may ask before the
  first start (*More info* → *Run anyway*).

## [0.1.0] — 2026-11-12 — Milestone 1: the converter

Not published as a download; it's the starting point the native port grew from.

- **Converter** (command line and Windows app): checks your dump against the known files of SLUS-20362, unpacks and
  lists the game's data archives, and builds a game folder (portable or installed), with resumable stages.
- **Disc image tools**: an ISO 9660 builder and reader.
- **Reference launcher** on the Play! emulator core with a virtual disc: the game played from the converter-made
  folder. It's kept as a development reference (to compare against); the native game doesn't use any of it.

[0.2.0]: https://github.com/awtocl/HotPursuit2PC/releases/tag/v0.2.0
[0.1.0]: https://github.com/awtocl/HotPursuit2PC/tree/v0.1.0
