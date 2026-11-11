# ADR 0001 — Architecture: how the PS2 game becomes a PC game

- **Status:** Accepted (2026-11-11, phase P0)
- **Deciders:** project owner (brief v3) + implementing agent
- **Constraints in play:** C1 (dump is the sole content source), C5 (license check first), C7 (60 fps), C8 (keep Milestone 1 honest)

## Context

The goal is the *PS2* game — its handling, AI, cameras, UI flow — running as a
Windows application. The 2003 PC release is explicitly not a substitute.
What the dump gives us (all verified, see `docs/dump-analysis.md`):

- `SLUS_203.62`, a 2.3 MB MIPS R5900 ELF with **no symbols** (25 sections, `.text`
  1.65 MB, `.vutext` 20 KB of VU microcode, 8 DVP overlay sections).
- A fully decoded asset container (405 files, names recovered from the dump).
- Two IOP module sets, including EA's own `STREAM.IRX` (custom streaming driver)
  and Sony's `SDRDRV`/`LIBSD` sound stack.

Where fidelity actually lives: the feel of the game is **code** (physics,
AI, camera, frame pacing), not assets. Any path that re-authors that code
re-creates exactly the problem the owner has with the PC port.

## Options considered

Scores: 5 = best. "Logic fidelity" = is it the PS2 game's behaviour.
"M1 timing" = how soon a playable, stable build can honestly be claimed.

| # | Option | Logic fidelity | Presentation fidelity | Effort | License risk | M1 timing | Native? |
|---|---|---|---|---|---|---|---|
| A  | Hand re-implementation (decompile, rewrite engine by hand) | 3 (only as good as a complete, matching decompilation) | 4 | 1 (person-years; no symbols) | 1 (rewritten code is a derivative of EA's; could not live in a public repo) | 1 | yes |
| A′ | **Static recompilation**: translate the user's ELF to x64 at conversion time + a native runtime that implements the PS2 interfaces (GS→GPU, SPU2→audio, pad→XInput, kernel/IOP services) | **5** (it *is* the game's code) | 3→5 (depends on runtime accuracy; grows over time) | 2 (large but incremental and testable per subsystem) | **5** (our code only; game code is translated on the user's machine, never distributed) | 2 | yes |
| B  | Asset-native hybrid: new engine + re-created game logic driven by extracted data | 1–2 (handling/AI re-created by ear) | 4 | 1–2 | 5 | 1 | yes |
| C1 | Emulator-backed, **embedded open core with HLE BIOS** (Play!, BSD-2-Clause) inside our own launcher | 5 | 3–4 (tracker: PAL release "playable", texture mip flicker) | **4** | 4 (BSD-2; no BIOS needed → nothing outside the dump) | **4** | no (labelled) |
| C2 | Emulator-backed, external PCSX2 (GPL-3.0+) driven by a launcher | 5 | 5 | 4 | 2–3 (needs the user's own PS2 BIOS dump — content outside the game dump; GPL process boundary) | 3 (blocked if no BIOS) | no |

Evidence behind the scores is in `docs/prior-art.md` (PS2Recomp is GPL-3.0 and
experimental with GS/VU stubbed; Play! issue #400 marks SLES-50731 `state-playable`
on 2024-04-26; PCSX2 runs the game but cannot run without a BIOS).

## Decision

1. **Target architecture: A′ — static recompilation + native runtime.**
   It is the only native option whose game logic is the PS2 game's logic, and
   the only one with no licensing exposure (the converter translates the
   user's own ELF; the repository never contains game code or assets).
   In the brief's vocabulary this is Path A's goal ("native code, assets 1:1")
   reached by machine translation rather than by hand-rewriting.
2. **Milestone 1 ships on Path C1 (emulator-backed), explicitly labelled.**
   An embedded Play! core (BSD-2-Clause, HLE BIOS — no BIOS file, so C1 holds)
   hosted by our own launcher: one window, native input/audio devices, fixed
   per-game configuration, no emulator UI. This is **not native** and every
   document, the launcher's About text and the M1 Gate Report must say so.
   The first P4 task is a go/no-go test of the NTSC-U disc on that core
   (compatibility is UNVERIFIED for SLUS-20362). If it fails the M1 bar,
   fall back to C2 and record what the user must supply (their own BIOS dump).
3. **Path B is rejected for game logic**, but its asset half is kept: the
   Z fileset unpacker and format decoders (ADR 0004) are needed anyway for
   ROM validation, the inventory, and later for native-runtime file I/O and
   texture/HD work in Milestone 2.
4. **Path A (hand rewrite) is rejected** — effort, and its output could not be
   published.

Native-ness is therefore a staged property, not a switch:

| Stage | Game code runs as | PS2 hardware/OS provided by | Label |
|---|---|---|---|
| M1 | JIT-translated inside the embedded core | embedded core (HLE BIOS) | Path C — emulator-backed |
| N1 | **AOT-translated at conversion time** | same core's HLE services | hybrid |
| N2 | AOT-translated | our runtime, subsystem by subsystem (GS first) | Path A′ — native |

## Consequences

- M1 can be honest and near; "native" is not claimed until stage N2.
- The converter's stage list and `game/` layout must not assume a path: stages
  are `validate → unpack → package → install-runtime`; only the last two differ
  between C1 and A′.
- Third-party code enters the tree (Play! and its submodules) — each needs a
  `docs/prior-art.md` entry before use (C5), and source must be fetched with the
  owner's go-ahead (workspace rule).
- 60 fps (C7): the PS2 game's own frame pacing is preserved in every stage
  because the game code is unchanged; any unlock is an opt-in patch (M2).

## Confidence

- Target architecture A′ is the right end goal: **high**.
- A′ reaching a fully playable N2 build: **uncertain** — it is a large runtime
  (GS, VU1, SPU2, IOP services). It is scheduled after M1 and gated per subsystem.
- C1 meeting the M1 acceptance bar on NTSC-U: **medium** until the P4 go/no-go test.

## What would reopen this ADR

- The P4 test shows Play! cannot run SLUS-20362 acceptably and the owner has no BIOS dump for C2.
- A maintained, permissively licensed PS2 recompilation runtime appears that runs commercial games (would shortcut N1/N2).
- The owner decides emulator-backed M1 is not acceptable even as a labelled stopgap.
