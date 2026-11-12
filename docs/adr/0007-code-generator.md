# ADR 0007 — Code generator for the recompiler, translation model, and the revised scope of R1

- **Status:** **Proposed (2026-11-12).** The recommended option needs something only the owner can grant: permission
  to download and build LLVM. It becomes *Accepted* when the spike in Decision 5 confirms the expected gain, and is
  rewritten if it does not.
- **Constraints in play:** C1 (no game code distributed), C5 (license check), C6 (one portable converter), C8
- **Evidence:** [docs/recomp-analysis.md](../recomp-analysis.md)

## Context

ADR 0006 made the statically recompiled build the product and left one question for a spike: which code generator
turns the game's PS2 code into x64 code *inside the converter, on the user's machine*. Since then:

- The owner added two requirements (2026-11-12): **the game must run natively and make full use of the PC's
  hardware**, and the modern graphics settings must live **inside the game's own options menus**.
- The analysis stage showed that function-level translation of this binary is practical: 10,146 functions found
  without symbols, and every one of the 29,860 blocks the reference core executed in the traced session begins
  where the analysis predicted (recomp-analysis §3–4).
- Measured cost of a busy race refresh in the reference build: 13.3–15.0 ms on one thread, of which the game
  program (EE) is 5.6–6.1 ms and the **vector-unit microcode (VU1) 4.1–5.8 ms**; VIF + GIF 2.2–2.8 ms. Compiling
  code while playing costs ≈ 0.2 ms per block, 29,935 blocks in 82 s, in bursts of up to 550 ms (§5).
- All VU1 microcode that ran is fixed data in the executable's `.vutext` (§6).

What the generator has to do: translate about 431,000 MIPS instructions in 10,146 functions and about 2,600
vector-unit instruction pairs; produce code good enough that the two heavy shares shrink several times; run as part
of a single self-contained converter; be deterministic; carry a license compatible with this MIT repository.

## Options considered

| Option | Code quality | Cost | Verdict |
|---|---|---|---|
| A. The reference core's own generator (Play! CodeGen, BSD 2-Clause) used ahead of time | Same code as today's JIT (it optimises inside one block only). Removes the compilation hitches, leaves the steady cost: in the traced race 41 of 69 late refreshes were steady cost, not compilation. | Smallest: it is already linked. | **Rejected as the product generator.** It cannot meet the owner's bar; it stays what the reference build uses. |
| B. Our own x64 emitter (one fixed x64 sequence per PS2 instruction) | About the level of a simple JIT. Getting further means writing an optimising compiler ourselves: register allocation across a function, constant propagation, removal of unused vector-unit flag updates. Gain over the reference **UNVERIFIED**; expected modest. | No dependency, tiny converter, conversion in seconds. Large amount of our own compiler code to write and to get right. | **Fallback** if LLVM is refused or fails the spike. |
| C. **LLVM embedded in the converter** (x86 back end only). Each function is turned into LLVM's intermediate form, optimised, and emitted as x64 machine code. | That of an optimising compiler, applied to whole functions. 128-bit multimedia, FPU and vector-unit operations map directly onto SSE/AVX. | Download and one-time build of LLVM (estimates, **UNVERIFIED** until done: ≈ 150 MB of source, several GB of build output inside `.cache/`, 20–40 minutes); the converter grows by tens of MB; conversion takes minutes instead of seconds (once per game folder, resumable, uses all cores); CI needs a build cache. License: Apache 2.0 with LLVM exceptions (from prior knowledge — to be confirmed from the fetched `LICENSE.TXT`, C5). | **Recommended.** |
| D. Emit C++ and compile it | Compiler quality | Needs a compiler on the user's machine, or shipping compiled game code | **Rejected** by ADR 0006 (C1, C6). This is what the existing open PS2 recompiler does (`docs/prior-art.md`). |
| E. Other embeddable generators (Cranelift, QBE, MIR, asmjit) | Below LLVM; from prior knowledge only, **UNVERIFIED** | Cranelift needs a Rust toolchain (not installed; installing software needs the owner); QBE emits assembly text and has no Windows calling convention; asmjit is an assembler with register allocation but no optimiser | Not evaluated further; none changes the picture between B and C. |

## Decision (proposed)

1. **Generator: LLVM, embedded** (option C). Pinned release, fetched by a script like `tools/fetch-play.ps1` into
   `.cache/third_party/`, built once with the static C runtime and only the x86 target. Its license text is added
   to `THIRD-PARTY-NOTICES.txt`. No LLVM source enters this repository.

2. **Translation model: one host function per game function, with the reference machine model underneath.**
   - A direct call (`jal`) becomes a direct host call; a return becomes a host return; a switch becomes a host
     switch over the recognised cases; a call through a pointer looks the target up among the function starts and
     address-taken instructions.
   - Correctness never depends on the host stack. A host function can be entered at its start *and* at the few
     places where execution can resume (after a call, at a loop head, after a system call). After every call the
     code checks that the game's program counter is where a normal return would leave it; if not (the time slice
     ran out, an interrupt is pending, the kernel switched threads, the exception unwinder ran), every host frame
     returns and the dispatcher re-enters at the game's program counter. The same mechanism lets the recompiled
     code run in the time slices the reference core hands out, and lets its state be compared with the reference
     at every such point.
   - The translated code is installed through the seam proven in the analysis stage (replacing the core's
     executor). Anything the translator does not cover falls back to the core's own executor instead of failing.

3. **R1 covers the game program and the VU1 microprograms.** The second half was not in ADR 0006; the measurements
   make it necessary. What is uploaded to the vector unit at run time is compared with what was translated; a
   difference falls back to the reference path and is logged.

4. **Acceptance of R1, restated** (replaces the R1 row of ADR 0006's table):
   - identical processor and memory state to the reference at checkpoints over the scripted session (unchanged);
   - **nothing compiled while playing**: 0 blocks compiled by the core for the EE and for VU1 in that session
     (today: 29,935 and 125);
   - EE + VU share of a busy race refresh cut from ≈ 10–11 ms to **≤ 3 ms**, i.e. the whole refresh at about
     half of today's 13.3–15.0 ms (a target, not a measurement);
   - no refresh over 16.7 ms in the scripted race outside loading;
   - the "3× lower" figure of ADR 0006 is **moved to R3**, because the remaining third of the cost (VIF, GIF, the
     graphics thread) is the rendering path and is replaced there, not translated.

5. **The spike that settles this ADR** (first thing after the owner's permission): fetch and build LLVM; translate
   about 20 of the hottest functions and one microprogram; run them through the tracing executor in lock-step with
   the core's JIT; time the same functions in isolation, JIT against LLVM. If the gain on those is below 2×, this
   ADR is rewritten around option B.

## Consequences

- The converter stops being a few-MB program and the conversion stops being a matter of seconds. Both are stated
  to the user by the converter itself (progress, resumable stage `recompile`).
- Building the project from source needs the LLVM build once; `tools/build.ps1` must keep working without it (the
  recompiler is then simply not built, as the launcher is without the Play! source).
- The reference core remains part of the build through R1 and R2: as the machine model the recompiled code runs
  on, as the oracle it is compared with, and as the fallback.
- Owner requirement "inside the game's own menus": the settings UI is not a launcher dialog. It needs hooks in the
  game's front-end code, which function-level translation makes possible; it is R3 work and gets its own ADR.

## Confidence

High that A cannot meet the bar and that function-level translation is practical (both measured). Medium that C
delivers the ≤ 3 ms target; that is what the spike measures. The download and build figures are estimates.

## What would reopen this ADR

- The owner declining the LLVM download → option B.
- The spike showing less than 2× on hot functions, or an LLVM build that cannot be made to work with the static
  C runtime and MSVC here.
- The lock-step comparison showing that function-level translation breaks on real behaviour the trace did not
  exercise (then: block-level translation with the same generator).
