# ADR 0006 — After the Milestone 1 trial: recompilation becomes the main line

- **Status:** Accepted (2026-11-12). Supersedes the staging in ADR 0001 §Decision 2 ("Milestone 1 ships on Path C1") as the
  *product* direction; ADR 0001's target architecture (A′) is unchanged and is now what is being built.
- **Amended 2026-11-12** by [ADR 0007](0007-code-generator.md): scope and acceptance of R1 (see "Amendments" at the end).
- **Constraints in play:** C1, C5, C7, C8

## Context

The owner played the Milestone 1 build (`docs/gate-reports/2026-11-12-M1.md`) and did **not** accept it:

> unstable frame time with a lot of stuttering; the glow around brake lights shows as translucent squares; stutters
> when moving between menus; meanwhile CPU and GPU use were under 20 %. […] I want this to be much closer to a
> recompilation than to an emulator trimmed for the game. The idea is a substantial improvement over what emulation
> through PCSX2 can do.

Later the owner added that tyre smoke shows the same defect, and that sound had problems (not yet described).

ADR 0001 named this as a reopening condition ("the owner decides emulator-backed M1 is not acceptable even as a
labelled stopgap"). What was then measured (`docs/evidence/m1-acceptance.md`, addendum):

| Finding | Evidence |
|---|---|
| The report was right and the acceptance evidence had hidden it. Per-second averages said "60 Hz"; per-frame intervals did not. | menus: 66 % of presentation intervals off by more than 25 %; race: intervals spread over 27–38 ms plus spikes to 180 ms |
| Constant jitter came from pacing (the core's limiter has no absolute clock; images were presented whenever a frame's work ended). | fixed in the launcher: refresh intervals p95 16.68 ms; 94 % of race presentations exactly 33.3 ms apart |
| The remaining stutter is **single-thread CPU cost of the core's JIT**. The game does a 30 fps frame's work inside one refresh; emulating that refresh costs ≈ 13 ms and sometimes exceeds the 16.7 ms available. | 50 ms + 17 ms presentation pairs; whole-system CPU/GPU load low, as the owner saw |
| The earlier claim "2.5–5× headroom" was wrong for the frames that matter. It averaged busy and idle refreshes. | busy-refresh headroom ≈ 1.25× |
| Code and shaders are compiled on first use, which adds long hitches. | present-interval maxima of 100–200 ms outside loading |
| Smoke and brake-light glow are drawn as flat translucent polygons. | screenshots `.cache/diag-pacer2/shots/frame_003000.png`, `frame_004050.png`; cause in the third-party renderer **not identified** (it has no mip-level support; unproven whether that is the cause) |

## Options considered

| Option | What it would give | Verdict |
|---|---|---|
| A. Keep improving the Play!-based build (patch its JIT, add a shader cache, fix the renderer) | fewer hitches, some defects fixed | rejected as the main line — it stays "an emulator trimmed for the game"; the ceiling is "as good as a mature emulator", not better |
| B. Embed or drive PCSX2 instead | PCSX2's accuracy and speed | rejected — needs a console BIOS (content outside the dump, C1), GPL-3, and by definition cannot be *better* than PCSX2 |
| C. **Static recompilation of the game program + a purpose-built runtime, replacing emulator parts stage by stage; the Play!-based build is kept as the reference oracle** | no run-time compilation; CPU cost of the game code cut by a compiler-quality translation; function-level hooks, which is what makes resolution-independent rendering, analog input, 60 fps options and real motion vectors possible | **chosen** |

Why C can exceed an emulator: an emulator must reproduce the machine for any program, at run time. A recompiled
port translates one known program ahead of time and may replace whole layers of it — its calls into the disc,
pad, sound and graphics libraries — with native equivalents. That is where the gains come from, not from the
translation alone.

## Decision

1. **The product is the recompiled build.** The emulator-backed launcher is no longer the deliverable; it is kept
   working as (a) the reference the recompiled code is compared against, state for state, and (b) the fallback the
   owner can still run.
2. Stages, each ending with something the owner can run and with numbers comparable to the stage before:

   | Stage | Content | Acceptance (measured with `tools/frametimes.py` and the reference core) |
   |---|---|---|
   | R0 | Frame pacing on an absolute 60 Hz grid; per-frame measurement tooling | **done**: see the addendum of `docs/evidence/m1-acceptance.md` |
   | R1 | **Static recompiler** in the converter: function discovery in `SLUS_203.62`, MIPS R5900 → x64 ahead of time on the user's machine; runs on the reference core's machine model first | identical CPU/memory state to the reference at checkpoints over a scripted session; **no code compiled while playing**; busy-refresh CPU cost at least 3× lower than the 13 ms measured now; no late presentations in the scripted race |
   | R2 | Host services where the game calls its libraries: disc reads from files, pad with analog triggers and rumble, memory card, timers | the same scripted session without the emulated disc/pad/memory-card devices |
   | R3 | Rendering: correct first (smoke, brake-light glow), then resolution independence, anti-aliasing with real motion vectors, widescreen, optional 60 fps races | defect test cases match reference captures; opt-in features off by default (C7) |
   | R4 | Audio mixed natively | by ear (owner) and sample-accurate against the reference |

3. **No game code is ever distributed.** Translation happens in the converter on the user's machine from the
   user's dump (C1). This rules out "emit C++ and ship the compiled result", and rules out requiring the user to
   install a compiler; the code generator has to be part of the converter. Which generator — an embedded LLVM, the
   BSD-licensed code generator that already ships with the reference core, or our own x64 emitter — is the first
   question of R1 and gets its own ADR (0007) after a spike on a handful of functions.
4. The 30 fps race pacing stays the default (ADR 0005 §6); with function-level hooks a 60 fps mode becomes a real
   option in R3 instead of a hack.

## Consequences

- Milestone 1 as defined in the brief ("running and stable") is met only in the weak sense recorded in the gate
  report; the owner's bar is higher and is now the bar. `docs/STATUS.md` says so.
- This is a long effort over many sessions. R1 alone is a compiler project (≈ 430,000 instructions of game code,
  no symbols). R3 is the largest part. No dates are promised; each stage is gated on measurements.
- "Better than PCSX2" cannot be measured directly here: PCSX2 needs a BIOS, which this project does not use.
  The yardsticks are therefore the console's own behaviour (60 Hz, the game's pacing), frame-time statistics,
  and reference captures the owner can provide or judge.
- Work on the third-party renderer's defects is limited to understanding them (they become R3 test cases).

## Confidence

High that C is the only option matching the owner's goal. Medium on the cost estimates. The largest unknown is
R3: how much of the game's renderer can be intercepted at a high level rather than emulated at GS level.

## What would reopen this ADR

- The R1 spike showing that function-level translation of this binary is impractical (e.g. pervasive
  self-modifying or data-driven code), which would push towards block-level ahead-of-time translation instead.
  **Checked 2026-11-12: it is practical** (`docs/recomp-analysis.md` §3–4, §7).
- The owner preferring a quicker, smaller improvement (option A) for the time being.

## Amendments

**2026-11-12, after the R1 analysis stage** (`docs/recomp-analysis.md`, ADR 0007):

- Owner requirements added the same day: the game must run **natively and make full use of the PC's hardware**;
  the modern graphics settings must be **inside the game's own options menus**; the recompiled build is to be
  **published to the GitHub repository only once it works well and those settings are in** (nothing is pushed
  before that).
- The measured cost of a busy race refresh is one third game program, one third vector-unit microcode, one fifth
  passing geometry and drawing commands on. **R1 therefore translates the VU1 microprograms as well as the game
  program**, and its acceptance is restated in ADR 0007 (EE + VU share ≤ 3 ms, nothing compiled while playing);
  the "at least 3× lower" figure in the table above moves to R3.
- R0 was not complete as stated: the pacer had a defect in 60 fps content, fixed during R1
  (`docs/evidence/m1-acceptance.md` §4b).
