# ADR 0017 — Controller layouts, remapping and languages

- **Status:** Accepted (2026-11-27)
- **Depends on:** ADR 0013 (settings overlay), ADR 0016 (launcher)
- **Owner requirements in play:** a modern controller layout (triggers for throttle and brake); remap the keyboard and
  the controller; presets that are easy to go back to ("restore defaults"); the launcher and the repository in Spanish
  and other popular languages

## Context

Until now the keys and the Xbox buttons were fixed in `PadPoll` (main.cpp): the PS2 controller button for button.

What the game itself does (measured, not assumed):

- Its **Options → Controller → Configuration** screen (default "A", seen in a scripted run) lists: Steering = left
  stick / D-pad, Accelerate = right stick up / ✕, Brake/Reverse = right stick down / □, Handbrake R1, Horn/Siren ○,
  Change view △, Look back L1, Speed boost R3, Pause Start, Cycle HUD Select, Zone Freeze L2 / Zone Zoom R2, and with
  a manual gearbox Shift up R2 / Shift down L2. The program also names `JOY_EVENT_THROTTLE_ANALOG` and friends.
- In a scripted Quick Race, holding ✕ drives the car; holding the right stick up (axis byte 0) does not; ✕ at a
  pressure of 30/255 accelerates about as hard as at 255. So in practice the game reads throttle and brake as
  buttons; the triggers can only be buttons for it.
- Language: the disc has `LANGUAGES\FE_{ENGLISH,FRENCH,GERMAN,ITALIAN,SPANISH,SWEDISH}.BIN` and speech packs. The
  North American program does not take its language from the console settings (kernel call 0x4B,
  `GetOsdConfigParam`): with French or Spanish there the menus stay English. Its loader (0x0021E0D0, table at
  0x0031C168) picks the file by an index it keeps itself; what sets that index is not found yet. Answering 0x4A/0x4B
  at all had a trap: the library at 0x0028B2D0 writes a "version" and reads it back to tell an old console, and a
  read-only answer made it install a kernel patch at 0x80074000 that is not translated (the run stopped).

## Decision

- **Controls** (`native/src/controls.*`, shared by the game and the launcher): 15 actions — the game's own, each one
  PS2 button — with two keys and two controller inputs each. Presets: keyboard *Default* (the old keys, plus E for
  Speed boost, which had none); controller *Classic* (the old mapping) and *Modern*: RT accelerate (A too, as it
  selects in the menus), LT brake/reverse, B handbrake, X speed boost, Y change view, RB/LB shift up/down, left/right
  stick click horn and look back. A *Custom* layout of each lives in `hp2native.ini` (`[keyboard_custom]`,
  `[controller_custom]`); the layouts in use are `[controls] keyboard_layout` and `[graphics] controller_layout` (the
  latter is also an item of the F1 menu). The sticks pass through as the DualShock 2's. A trigger that presses a button
  also sets that button's pressure byte by its travel (harmless; the game does not seem to use it).
- **Launcher → Controls page**: the two layout choices, a table of all actions with their keys and inputs, "press a
  key / button" for any box (Esc cancels, Delete clears a key), duplicates shown in red, **Restore Default Keyboard**
  and **Restore Default Controller**. The first change made while a preset is in use starts the custom layout from that
  preset; restoring switches back to the preset and keeps the custom layout for later.
- **Languages** (`native/src/i18n.*`): English, Spanish, Portuguese (Brazil), French, German, Italian for the launcher
  and the F1 menu, one table of English strings and their translations. Every string drawn by `hp2_style` goes through
  it, so a page needs no per-string code; text built from pieces uses templates (`F`). First start: Windows' display
  language; then **Settings → Language** (launcher) or the F1 menu. CJK and Cyrillic are left out for now: the fonts
  used (Bahnschrift, Arial Black) do not cover them and GDI+ paths do no font fallback.
- **The repository**: README and the Getting started guide in the same languages, with a language line at the top of
  each; the English text is the reference (and prevails for the legal section).
- **Kernel**: 0x4A/0x4B keep a settings word like a console's kernel (English, version 1), so the game takes the
  newer-console path as before.

## Consequences

- Labels follow the game's Configuration A; a player who picks B, C or D in the game's own menu sees other meanings.
- Translations are machine-quality-checked by the author of the change, not by native speakers: corrections welcome.
- The game's own text stays English; switching it is a separate task (find the index the FE loader uses).

## Verified (2026-11-27)

- Scripted race: ✕ drives, the right stick does not, pressure 30 ≈ 255. The game runs to its menus with the new
  0x4A/0x4B (and stopped at 0x80074000 with the read-only first try).
- Launcher driven by window messages: Controls → Accelerate key → P saved `keyboard_layout=1`, `accelerate=80`.
  The F1 menu in Spanish (picture), the launcher in Spanish and German (pictures; German's long words fit).
- Not verified: an Xbox controller on the Modern layout in a race (the mapping is to ✕/□, which were verified),
  controller capture in the launcher, Portuguese/French/Italian pictures.
