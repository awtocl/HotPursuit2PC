# ADR 0007 — Code generator for the recompiler, translation model, and the revised scope of R1

- **Status:** **Accepted (2026-11-13).** Proposed 2026-11-12; the owner allowed the LLVM download the same day; the
  spike of Decision 5 passed its criterion (translated code 4.1x faster than the reference core on the hottest
  functions it covers, 0 differences in 1,864 compared calls). Results: "Spike result" below and
  `docs/recomp-analysis.md` §9.
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

## Decision

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

## Spike result (2026-11-13)

LLVM 23.1.5 (pinned by SHA-256 in `tools/fetch-llvm.ps1`; license read from the fetched `llvm/LICENSE.TXT`:
Apache 2.0 with LLVM Exceptions) built here once with MSVC and the static C runtime, x86 target only
(`tools/build-llvm.ps1`: about 1.9 GB of build tree, 527 MB installed, all inside `.cache/third_party/`).

The spike translator (`recomp/src/lift.cpp`) covers leaf functions (no calls) using integer, memory, FPU and some
128-bit instructions, with the reference core's semantics for clamping, division by zero and conversions. It was
run inside the reference build (`--lift-check`): at every entry of a translated function the translated code runs on
a copy of the processor state and of main memory, the reference core runs the same call, and both results are
compared when it returns.

| Measure | Result |
|---|---|
| Of the 400 hottest functions, translated | 97 (they execute 13.1 % of the game's instructions in the session); optimising and compiling them to x64 took 0.57 s |
| Compared with the reference | 81 functions, 1,864 calls: **0 differences** in registers, FPU state or memory |
| Left through the trap | 16 functions (1.7 % of instructions): they use memory outside main RAM (the scratchpad), which the spike does not model |
| Time, median call time x calls in the counting session | reference core 842 ms, translated code 206 ms: **4.1x** |
| Why the rest was not translated (share of executed instructions) | calls 51.5 %, VU0 macro instructions 11.6 %, a few 128-bit multiply instructions and second-pipeline mult/div 12 %, the rest small |

Limits of these numbers: the reference time includes one extra call per executed block (the spike's own hook);
the translated code was timed by running each call 64 times on copies of its inputs; the time-stamp counter on
this PC advances in 10 ns steps. FPU-heavy functions gained least (about 1.1-1.5x): the reference core's clamping
is reproduced literally and costs as much in both. Criterion of Decision 5 (at least 2x): **met**.

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
