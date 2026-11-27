# ADR 0015 — Widescreen 16:9 by default and 60 fps, through game patches applied by the runtime

- **Status:** Accepted (2026-11-27)
- **Depends on:** ADR 0009 (code package), ADR 0012 (native runtime), ADR 0013 (settings overlay)
- **Owner requirements in play:** a PC game first (16:9 by default, the image not stretched), no emulator layer (4)

## Context

The game draws a 4:3 picture. Stretching it to 16:9 distorts everything; showing it at 4:3 leaves bars on modern screens.
PCSX2's public patch database (`pcsx2_patches`, `SLUS-20362_1D2818AF.pnach`) has a 16:9 patch for this disc: the camera's
horizontal extent (one code word, two data words) and the race HUD's scale and positions (three data words, two `lui`
constants). In a statically recompiled program code words are fixed at translation time, and 4:3 must stay available
from the settings menu.

The HUD part of the patch also squeezes the game's 2D front end (menus, loading screens) into the middle three quarters of
the 640-pixel frame (0x00330188 = 853.33 = 640 × 4/3, 0x002CFD64 = 64), so at 16:9 the menus keep their 4:3 shape. The game
lays the HUD and the front end out from these values early: applying them only from a race's loading on left the race HUD
misplaced (measured: lap panel and tachometer pushed off the right edge).

## Decision

1. **Three kinds of patch point** (`recomp/include/hp2/game_patches.h`, shared by the recompiler and the runtime):
   - *Baked code words:* `mul.s f28, f2, f14` at 0x0010E994 becomes `mul.s f28, f2, f1`. The camera kept
     `min(width × 0.5, width × [gp-24736])`; the patched form is `width × [gp-24736]`, identical while that data word
     keeps its original 0.5. The recompiler patches the ELF image before translation, only where the expected word is.
   - *Run-time `lui` constants:* the two HUD `lui` instructions are translated to read their 16-bit constant from the
     instruction word in guest memory; the runtime writes the wanted constant into the word's low half.
   - *Data words:* written by the runtime.
   The code package format version goes to 3, so older packages are rebuilt.
2. **Chosen once at start, applied every refresh.** The setting is read at the first refresh; with 16:9 the patches are
   written every refresh from then on (as PCSX2 applies patches each vsync: the game may write those words itself) and the
   window shows the picture at 16:9. Switching between 16:9 and the 4:3 modes takes effect after a restart; the settings
   menu marks a pending change "(restart)". 4:3, 4:3 integer scale and Stretch apply at once among themselves.
3. **60 frames per second** ("60 FPS" by asasega in the same file): the frame loop's `sltiu v0, v0, 2` (wait for two
   vertical blanks) at 0x00113E2C reads its constant from memory like the HUD `lui` constants; the runtime writes 2 or 1
   every refresh from the *Frame rate* setting (60 by default), which applies at once. Measured: the race clock advances
   10.0 s in 10 s of game time at 60 fps (the game times its simulation itself), 600 pictures per 10 s.
4. **Settings:** *Aspect ratio* has 16:9 (default), 4:3, 4:3 integer scale and Stretch, stored under a new key
   (`aspect_ratio`) so the old `aspect` values (0 = 4:3) do not carry over. `HP2_ASPECT=n` and `HP2_FPS=30|60` override
   them for tests.

## Consequences

- Races fill a 16:9 screen with a wider view (Hor+), the HUD keeps its shapes; menus keep their 4:3 shape inside the
  16:9 picture (drawn so by the game).
- A change between 16:9 and 4:3 needs a restart; the frame rate switches at once.
- At 60 fps the game does twice the work per second (CPU, VU1 and GS); the native runtime keeps 60 refreshes per second.
- Future game patches use the same three mechanisms; a code word that is not neutral with the original data needs a
  run-time form like the `lui` constants.
