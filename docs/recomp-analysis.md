# Recompiler analysis — evidence for stage R1 (2026-11-12)

What was established about the game's executable and about the reference build before choosing how to translate
the game ([ADR 0006](adr/0006-recompilation-main-line.md), [ADR 0007](adr/0007-code-generator.md)). Every number
below was produced by the tools named next to it, on the owner's dump (C4). Anything not measured is labelled
**UNVERIFIED**.

Reproduce: §8.

## 1. The executable

`SLUS_203.62`, 2,321,608 bytes, 32-bit little-endian MIPS ELF (`hp2recomp info`).

| Fact | Value |
|---|---|
| Entry point | `0x00100008` |
| `gp` (from `.reginfo`) | `0x003358F0` |
| Loaded image | `0x00100000`–`0x0033D8FC` (two segments: code 1,747,360 bytes, data 554,011 bytes in the file / 600,316 in memory) |
| `.text` | `0x00100000`–`0x002A5904`, 1,726,724 bytes = **431,681 instruction words** |
| `.vutext` | `0x002A5910`–`0x002AA9A0`, 20,624 bytes: vector-unit microcode (§6), not main-processor code |
| `.rodata`, `.data`, `.lit4`, `.sdata` | 115,390 + 419,144 + 6,456 + 12,699 bytes |
| `.gcc_except_table` | 92 bytes (C++ exception tables: almost nothing uses exceptions) |
| `.sbss`, `.bss` | 240 + 45,948 bytes, zero-initialised |
| ABI | section name `.mdebug.eabi64`: 64-bit registers, 32-bit pointers |
| Symbols | none |

## 2. Instruction decoder

`recomp/include/hp2/r5900.h` decodes the PS2 main processor's instruction set (MIPS III, parts of MIPS IV, the
128-bit multimedia instructions, the single-precision FPU, the VU0 macro instructions).

- **All 431,681 words of `.text` decode**; none is left over. The section uses **182** distinct instructions.
- The traced session (§4) executed 180,320 of them, **163** distinct instructions, and every one decodes
  (`check-trace` CHECK 1).
- Multimedia (128-bit) and VU0 macro instructions are present but rare in what ran: e.g. `pextlw` 163, `pcpyld` 74,
  `vaddbc` 408, `vmul` 206 executed instruction words out of 180,320. VU0 is used in macro mode only (no
  `vcallms` executed).

Limits: the decoder's tables were written from the published MIPS/R5900 encodings and cross-checked only in the
sense above (the game's own code decodes completely, and its control flow is predicted correctly, §4). The
*meaning* of each instruction is not implemented here yet; that is the translator's job and will be checked
against the reference core instruction by instruction.

## 3. Function discovery

`recomp/include/hp2/discover.h` (`hp2recomp functions`). Method:

1. function starts that are certain: the entry point and every target of a direct call (`jal`);
2. every switch is recognised from the compiler's pattern (index check `sltiu`, table address built by
   `lui`/`addiu`, `addu`, `lw`, `jr`), the table is read from `.rodata`;
3. an instruction whose address is stored in data (or built in code), that is not a switch case and before which
   straight-line execution stops, starts a function called through a pointer (virtual methods, callbacks);
4. a direct jump (`j`) is a tail call when a certain function start lies between it and its target, otherwise it
   stays inside its function;
5. a function is contiguous, so no boundary is placed where a branch, jump or switch passes over; wherever
   straight-line execution stops and nothing passes over, a new function begins;
6. the control flow of every function is followed to mark what is reached and where straight runs begin
   ("leaders"); code nothing leads to is kept as an entry point of its own ("orphan").

Result (the whole analysis takes about 0.4 s):

| | |
|---|---|
| Functions | **10,146**: 1 entry, 5,507 call targets, 553 jump (tail-call) targets, 1,297 reached through pointers, 2,788 without any reference found |
| Words reached by control flow | 426,800 (98.87 %); the remaining **4,881 are `nop` filler** between functions; no valid instruction is left unreached |
| Leaders | 90,612 |
| Address-taken instructions | 2,714 |
| Switches | **193 recognised, 3,077 cases**; every case lies inside the function that contains the jump |
| Indirect jumps not recognised | **3** — `0x0029EFF0`, `0x0029F9C8`, `0x0029FBB0`: they restore a saved stack pointer and jump to a saved address. This is the C++ exception unwinder of the compiler's runtime library, at the very end of `.text` |
| Branches between functions | 6, all inside that same unwinder code (`0x0029EF08`–`0x002A2224`) |
| Orphan roots | 349 (about 6,750 words): code that sits under a long jump and that nothing references. None is address-taken; none was executed (§4) |

What the 2,788 unreferenced functions are is **UNVERIFIED** (most plausibly library code that was linked in but is
never called). They are translated like the rest, so it does not matter for correctness.

## 4. Validation against the reference core

The launcher's `--trace` switch (`engine/src/ref_trace.cpp`) replaces the reference core's main-processor
executor by a subclass that records every block of code the core compiles — i.e. every straight run of game code
that actually executed — and saves main memory at the end. The same seam will later run the recompiled code.

Session: the scripted run used since P6 (boot, intro videos, title, menus, Quick Race, about 25 s of racing; 4,600
emulated refreshes, 82 s). `hp2recomp check-trace` on `.cache/r1-trace3/trace`:

| Check | Result |
|---|---|
| Blocks executed | 29,935 distinct (the run is deterministic: identical count in two runs) |
| ... inside `.text` | 29,860 |
| ... outside | 75: 42 in the core's BIOS stand-in (`0x1FC00000`…), 33 below `0x00100000` (kernel area the core fills itself). **No game code ran outside `.text`.** |
| Executed instructions / functions | 180,320 of 431,681 (41.8 %) / 3,698 of 10,146 |
| 1. every executed instruction decodes | **0 failures** |
| 2. every executed block starts at a predicted leader | **0 of 29,860 do not** |
| 3. / 4. every executed block start / instruction was reached statically | **0 / 0 were not** |
| 5. `.text` in memory at the end equals the file | **0 words differ** (no self-modifying code in the session) |
| 6. no executed block starts at an orphan root | **0 do** |
| Executed indirect jumps (`jr`, not returns) | 62, all recognised switches |
| Functions entered at their first instruction | 3,028 call targets, 187 jump targets, 481 pointer targets, **0 "unreferenced"** |
| Blocks compiled more than once | 0 (the core never had to throw code away and recompile it) |

How the 29,860 blocks end: 15,288 conditional branches (2,639 of them "likely"), 9,772 direct calls, 3,719 returns,
664 direct jumps, 307 calls through pointers, 62 switches, 48 system calls.

Main memory at the end holds 5,505 distinct values that look like code addresses (outside `.text`): 1,391 function
starts, 1,246 switch cases, 215 return addresses, 339 other leaders, and 2,314 values that are not predicted entry
points. None of the last group is an address where an executed block begins; they are consistent with ordinary
data that happens to fall in the address range of `.text` (**UNVERIFIED** one by one).

**What this does not prove.** One session of 82 s covers 42 % of the code. Other modes, tracks, the replay, the
credits, error paths and the 58 % that did not run are covered only by the static analysis. Check 2 shows that
execution never *began a run* somewhere unexpected; it does not show that every return went back to its call site
(the block list has no order). The recompiled code will be compared with the reference state by state, which
tests exactly that.

## 5. Where the time goes in the reference build

Measurement build (`-Preset profile`: the core's own profiler zones) plus the launcher's per-refresh CPU-time
counters; `tools/profile.py`. Three runs of the same session (`.cache/r1-trace`, `r1-trace2`, `r1-trace3`).
"Work" is everything the VM thread does for one emulated refresh.

**Race** (1,380 refreshes; the game does a frame's work in every second refresh):

| Per busy refresh (≈ 690 of them) | ms | share |
|---|---|---|
| Total work on the VM thread | **13.3 – 15.0** | |
| EE — the game program (what R1 translates) | 5.6 – 6.1 | 37 – 46 % |
| VU — vector unit 1 microcode (geometry) | 4.1 – 5.8 | 31 – 39 % |
| VIF1 + GIF — unpacking data for the vector unit and passing drawing commands on | 2.2 – 2.8 | 17 – 19 % |
| SPU + IOP — sound and the I/O processor | 0.7 | 5 % |
| *Graphics thread (the core's OpenGL renderer), separate thread* | *≈ 9 – 11 per game frame* | |

Refreshes whose work exceeded 16.7 ms: 59 – 122 of 1,380 (p99 ≈ 25 ms; maximum 171 – 194 ms). Of the 69 in
`r1-trace2`, **27 coincide with the core compiling new code**, 41 are the steady cost running over, and one is a
108 ms wait for the graphics thread (first use of a shader, **UNVERIFIED** as the cause).

**Compilation while playing.** The core compiled 29,935 blocks during the session, in 479 of the 4,600 refreshes,
in bursts of up to 2,862 blocks in one refresh. Fitted cost: **≈ 0.2 ms per block** (195 – 233 µs in three
windows), i.e. about 6 s in total; the largest burst accounts for the 550 ms maximum in the menus. In the menu
window 33 of the 55 refreshes over 16.7 ms compiled code; in the videos, 5 of 5.

**Menus** (1,080 refreshes): about 220 busy refreshes, mean 24 ms of work: EE 14.4 ms (60 %), VU 6.0, VIF1 + GIF
2.2, the rest small. The EE share includes the compilation above.

Consequences:

1. Translating the game program ahead of time removes every compilation hitch on the VM thread and attacks the
   largest single share of the steady cost.
2. **It is not enough for the "3× lower" target of ADR 0006.** Even with the EE at zero cost a busy race refresh
   would still take about 8 ms (1.7×). The vector-unit microcode is as heavy as the game program. R1 therefore
   has to translate it too (§6), and the rest — VIF, GIF and the graphics thread — is the rendering path, R3.
3. The graphics thread is not the bottleneck today (its work is on another core and finishes in time), but it
   is where first-use shader compilation shows up.

## 6. Vector-unit microcode

The tracing launcher also records every VU1 block the core compiles, with its contents (`vu1-blocks.tsv`);
`tools/recon/vucheck.py` looks each one up in the executable.

- 125 distinct blocks executed, 1,897 instruction pairs; each compiled once (the microprograms were uploaded
  once and not changed in the session).
- **All 125 are in `.vutext`.** 119 appear there byte for byte; the other 6 run across the boundary between two
  upload packets: `.vutext` stores the microcode as `MPG` upload packets of at most 256 instruction pairs, each
  preceded by an 8-byte header (e.g. `00000000 4A000100` = load the next 256 pairs at pair address `0x100`).
- The pieces sit at 22 different (file position − load address) offsets, i.e. `.vutext` holds several
  microprograms loaded to their own addresses.

So the vector-unit code is fixed data of the executable and can be translated ahead of time as well. Whether other
modes upload microcode from elsewhere (asset files) is **UNVERIFIED**; the recompiled build must check what is
uploaded against what was translated and fall back to the reference path if it ever differs.

## 7. What follows for R1

- **Function-level translation of this binary is practical.** The reopening condition of ADR 0006 ("pervasive
  self-modifying or data-driven code") does not apply: code is in `.text` only, is not modified, and every place
  where execution began was predicted.
- Things the translator must treat specially (all found by the analysis above):
  the C++ exception unwinder (3 indirect jumps; never executed in the session — trap and report if reached);
  `syscall` (48 call sites executed: the kernel services of the reference core's BIOS stand-in);
  calls through pointers (307 sites executed; targets are looked up among the function starts and address-taken
  instructions); interrupts and thread switches, which the reference core delivers only between blocks.
- Scope of R1 becomes: the game program **and** the VU1 microprograms. Acceptance numbers are restated in ADR 0007.

## 9. Translator spike (2026-11-13, ADR 0007 Decision 5)

**Execution counts.** The tracing executor can count how often each block runs (`--trace-counts`; the count is
taken in the block's epilogue, because a block that loops on itself skips its prologue — a first version counted in
the prologue and undercounted tight loops, e.g. 4 instead of 2,888 runs of a start-up loop). `hp2recomp hot`
(`.cache/r1-counts2`): 7.6 billion game instructions in the 82 s session; **12 functions account for half of them,
125 for 90 %, 569 for 99 %**. The hottest (7.9 %) is a 141-instruction function entered 11,852 times; the second
(7.0 %, 7.8 million calls) is a 73-instruction function using VU0 macro instructions. Most executed instructions:
`addiu` 11.2 %, `sll` 11.1 %, `lw` 6.6 %, `addu` 5.1 %, `sw` 4.7 %, `mul.s` 3.3 %.

**Translation.** `hp2recomp lift` translates leaf functions to LLVM IR, optimises them (-O2, host CPU) and compiles
them to x64 in memory: 97 of the 400 hottest functions (13.1 % of executed instructions) in 0.57 s. The others need
calls (51.5 % of executed instructions), VU0 macro mode (11.6 %), `pmulth` / `mult1`-family instructions (12 %),
indirect jumps (3.6 %) or branches out of the function (2.7 %) — all R1b work.

**Comparison with the reference core** (`--lift-check`, `.cache/r1-lift2`, `tools/recon/liftsummary.py`): 81
functions compared on 1,864 real calls (24 per function) with **0 differences** in registers, HI/LO, FPU registers,
FPU accumulator and condition flag, or main memory. 16 functions always touched memory outside main RAM (the
scratchpad at `0x70000000`) and left through the spike's trap: the scratchpad has to be modelled in R1b. Over the
counting session the compared functions would take 842 ms in the reference core and 206 ms translated (4.1x); per
function from about 1.1x (FPU-heavy, clamping dominates) to over 20x (loops). Caveats in ADR 0007.

## 10. R1b, first stage: the game running translated code (2026-11-13)

**Translator.** `recomp/src/lift.cpp` now translates whole functions, not only leaves:

- direct calls between translated functions are host calls; after each call the caller checks that execution came
  back where a normal return would, and otherwise hands over (this is how a hand-over deep in a call chain reaches
  the reference core); calls through pointers and unrecognised indirect jumps go through a generated dispatcher;
- whatever the translator does not cover is left to the reference core at that exact instruction ("hand-over"), so
  any function can be translated except one whose very first instruction would hand over;
- translated code can be **re-entered in the middle**, at every place where a straight run of code begins and it
  makes progress (38,624 such places in the 3,692 functions below), so after the reference core ran a single
  uncovered instruction, execution comes straight back to translated code;
- memory: main memory and the scratchpad directly, everything else (hardware registers, VU memory) through the
  reference core's own memory map; a hardware write that raises an interrupt hands over at the next instruction;
- loops count against the time slice; waiting loops, recognised with the reference core's own rule, are left to it
  (it skips the rest of the slice there instead of spinning);
- newly covered: second multiply unit (`mult1`, `div1`, `madd`...), unaligned loads and stores, `pmulth`, VU0
  register moves (`lqc2`, `sqc2`, `qmfc2`, `qmtc2`), `cache` / `sync` / `pref`.

**Coverage** (`hp2recomp lift`, counting run `.cache/r1-counts2`): 3,692 of the 3,698 functions executed in the
session are translated (99.8 % of executed game instructions; the 6 others start with `mfc0` / `cfc2`). In them,
730 places hand over; in the 400 hottest, 149, mostly VU0 macro instructions (`vsub` 40, `vmul` 21, `vmulbc` 10,
`vmove` 9, ...). Translating and compiling the 3,692 functions at -O2 takes 134-224 s on this PC (single thread).

**Comparison** (`--lift-check`, `.cache/r1b-check`, 399 hottest functions, now including functions that call
others): **0 differences** in 1,156 compared calls of 197 functions (registers, HI/LO and HI1/LO1, FPU, VU0
registers, main memory and scratchpad). Calls that handed over before returning are not compared (1,196).

**Running the game on translated code** (`--run-translated`, the same scripted session, measurement build).
The session completes normally (menus, Quick Race, race; screenshots in `.cache/r1b-runall3/shots`). Quick Race
picks a different car and track than in the reference runs (the game chooses them at random, and the timing is
different); the race is therefore **not the same race**, and race totals are not directly comparable.

| Measure | Reference (`r1-trace3`) | Translated (`r1b-runall3`) |
|---|---|---|
| Calls into translated code | - | 70.5 million |
| Blocks the reference core compiled during the session | 29,935 | **6,649** |
| Menus: game program (EE) per busy refresh | 14.29 ms | **5.62 ms** |
| Menus: refreshes over 16.7 ms / worst | 64 / 554 ms | **17 / 176 ms** |
| Race: game program (EE) per busy refresh | 6.14 ms | **4.27 ms** |
| Race: whole VM thread per busy refresh | 13.27 ms | 13.21 ms (other track: VU 5.13 vs 4.13, VIF+GIF 3.12 vs 2.24) |
| Race: refreshes over 16.7 ms | 59 | 138 (other track) |

Steps on the way (same session): with only the 400 hottest functions and no re-entry the game program's time did
not change (6.12 ms); with all functions but no re-entry 4.65 ms and 20,359 blocks compiled; with re-entry 4.34 ms
and 6,869; with the waiting-loop rule 4.27 ms and 6,649.

**What this says.** Translation works on the real game at full coverage, and it removes most of the reference
core's compilation (the menu hitches drop accordingly). In the race the game program now takes a third of the busy
refresh; the vector unit (VU1 microcode) and the data path to it (VIF, GIF) take the other two thirds and are
untouched so far. The ADR 0007 target (EE + VU at most 3 ms) is far: the next gains are VU0 macro mode (the hottest
function, 7 % of executed instructions, is VU0 geometry) and then VU1.

**Not verified.** Correctness of the running game beyond what the screenshots and the normal end of the session
show (the comparison mode covers calls that return; run mode adds hand-over, re-entry and real hardware access,
which are only exercised, not compared); a same-track race comparison; sound (muted).

## 11. R1b, second stage: VU0 macro mode translated (2026-11-13)

The hottest function of the game (7 % of executed instructions, 7.8 million calls in the session) is geometry on the
EE's vector unit used directly from the main program ("VU0 macro mode"). The translator now covers it:

- vector arithmetic (`vadd`/`vsub`/`vmul`/`vmadd`/`vmsub` with vector, broadcast, I and Q operands, to a register or
  to the accumulator), `vmax`/`vmini`, `vopmula`/`vopmsub`, `vclip`, `vmove`, `vmr32`, `vabs`, `vftoi*`/`vitof*`,
  the integer registers (`viadd`... `vmtir`, `vmfir`), `vdiv`/`vsqrt`/`vrsqrt`/`vwaitq`, `cfc2`/`ctc2` for the
  registers the game uses (integer registers, MAC, clip, R, I, Q);
- with the reference core's exact behaviour: which operands are clamped depends on the operation (`vadd` clamps only
  fs, `vsub` and `vmul` both, `vmadd` fs and a broadcast ft, `vmsub` none, `vaddi` none...), its special cases (a
  broadcast add of vf0's zero is a move; `vsub` of a register from itself is zero), MAC/sticky/clip flags through
  the same 8-slot pipelines with latency 4, and Q held until `vwaitq`;
- translated code tells LLVM that denormals are flushed (as the reference core's thread runs), so that no
  comparison with zero is folded differently;
- flags queued during a straight run are kept in host registers and written at its end (only the last 8 of a
  pipeline can ever be seen): the stored state is identical, with fewer memory writes.

Program-wide, hand-over places dropped from 730 to 355; in the 400 hottest functions from 149 to 76 (none for VU0;
the largest remaining reason is a branch in a delay slot, 38 places).

**Comparison** (`.cache/r1b-vu0check2`, 400 hottest functions): **0 differences** in 1,530 compared calls of 259
functions, now including the VU0 state: vector and integer registers, accumulator, Q, I, MAC/clip/division flags,
the three flag pipelines and the Q pipeline.

**Running the game** (`.cache/r1b-runvu0`). The session picked the same car and track as the previous translated run
(`r1b-runall3`) and reached the same image at the same instant, so these two are directly comparable:

| Race, same race | Before VU0 (`r1b-runall3`) | With VU0 (`r1b-runvu0`) |
|---|---|---|
| Calls into translated code (whole session) | 70.5 million | **20.2 million** (fewer hand-overs and re-entries) |
| Blocks the reference core compiled | 6,649 | **5,847** |
| Busy refresh, whole VM thread | 13.21 ms | **11.94 ms** |
| ... game program (EE) | 4.27 ms | **3.63 ms** |
| ... VU1 + VIF + GIF | 8.25 ms | 7.64 ms |
| Refreshes over 16.7 ms | 138 | **17** |
| p99 / worst refresh | 19.43 / 46.5 ms | **17.02 / 28.4 ms** |

Menus: game program 4.49 ms per busy refresh (reference 14.29 ms), 14 refreshes over 16.7 ms (reference 64), worst
158 ms (reference 554 ms).

Per function, the VU0 geometry functions gain little by themselves (44 vs 60 ns and 39 vs 40 ns per call): the
reference core's JIT also uses SIMD there and the flag pipelines cost the same in both. The gain comes from no
longer handing over to the reference core in the middle of the hottest call chains.

**What is left of the busy race refresh:** VU1 microcode 4.78 ms, VIF + GIF 2.86 ms, game program 3.63 ms. The
next step is the VU1 microprograms (ADR 0007, R1 scope).

## 12. VU1 microcode: what it does and what it costs (2026-11-13)

`hp2recomp vu1` (new): decodes VU microcode (lower and upper words, `recomp/include/hp2/vu.h`), finds the upload
packets in `.vutext`, and with a counting trace (`--trace-counts` now also counts VU1 blocks, in their epilogue)
reports what the reference core executed (`.cache/r1-vucounts`).

- 8 upload packets, 1,563 instruction pairs, all decode; 9 end bits; 19 immediates.
- 3.88 billion pairs executed in the 82 s session, in 125 distinct blocks; 9 blocks make up half. At the measured
  4.8-5.0 ms per busy race refresh that is about 3 ns per pair in the reference core.
- Upper slot: 54 % `nop`; FMAC operations 46 % (`vmaddbc` 8.2 %, `vmaddabc` 7.5 %, `vmulabc` 4.7 %, `vmula` 2.9 %,
  `vclip` 2.4 %, `vadda` 2.3 %, `vmaddq` 2.2 %, `vmulq` 2.1 %, `vftoi0`/`vitof0` 1.6 % each ...).
- Lower slot: 35 % `nop`; `sqi` 8.7 %, `lqi` 7.9 %, `lq` 4.9 %, `iaddi` 4.8 %, `iaddiu` 4.3 %, `ilw` 3.3 %, `mr32` 2.9 %;
  branches 8.2 % of pairs; 14.5 million `xgkick`.
- Over all the microcode (not only what ran): clip-flag reads only (`fcget` 9, `fcand` 4, `fcset` 3), **no MAC or
  status flag read**, no EFU/P instruction; `div` 29, `waitq` 3, `rnext` 22, `rinit` 6, `rget` 2, `bal` 15, `jalr` 17,
  `xtop` 2, `xitop` 1.

Consequence: [ADR 0008](adr/0008-vu1-microcode.md) - translate the microprograms keeping only what this game's
microcode can observe (clip flags, Q, R, the pipeline timing they depend on), not the MAC/status flag bookkeeping
that dominates the reference core's cost.

## 13. R1b, third stage: VU1 microcode translated (2026-11-14)

`recomp/src/vulift.cpp` (`hp2/vulift.h`), as decided in [ADR 0008](adr/0008-vu1-microcode.md):

- **What is translated:** the microcode image the game runs. Its 7 large upload packets are contiguous
  (0x0000-0x30C8) and are exactly what microcode memory held at the end of all 14 recorded sessions; they are
  translated as one image of 136 block starts (2,103 pairs) in about 8 s. The image is used only while microcode
  memory still holds exactly those words (re-checked after any upload or write the core reports).
- **Block model:** the reference core's own partition (end bit, D/T bits, branches with their delay slot) and its
  per-block pipeline model, effect by effect: static FMAC stall delays, the dynamic stalls on writes the previous
  block left in flight, Q (`div` 7, `rsqrt` 13, `waitq`) and clip-flag (4) latencies relative to each pair's
  pipeline time, the integer-branch rules (a branch right after its register was written sees the earlier value;
  the trailing register of the previous block), the "pre-upper" copy when the lower op reads the upper op's
  destination, `xgkick` delivered after the next pair's lower op, LOI, the cycle quota. The operand tables that
  drive the stalls are a port of the reference core's, quirks included; the launcher compares them with the
  reference core's on every word of the image: **3,122 words, 0 differences**.
- **Not maintained:** MAC, sticky and status flags and the P register (never read by the game's microcode); the
  translator refuses any block that would read them, or that relies on rules it does not reproduce (branch or end
  bit in a delay slot, integer fields above vi15, writes to vi0) - the game's microcode has none.
- **Entries:** the game starts microprograms at addresses it computes at run time, two of which are not block
  starts of the image. Such an entry gets its one block translated on first use (2 per session, 43 ms in total),
  which then continues in the image's code.
- The FMAC operations reuse the VU0 code generator (`recomp/src/lift_internal.h`), whose semantics were already
  verified (§11).

**Comparison** (`.cache/r1b-vu1check2`, launcher `--vu1 check`): every time slice the VU1 executor ran was first run
translated on copies of the VU1 state and data memory, then by the reference core, then compared - vector and
integer registers, accumulator, Q, I, R, clip and division flags, Q and clip pipelines, the pending FMAC writes,
pipeline time, the integer-branch state, program counter, cycle quota, all 16 KB of data memory, and every
packet handed to the graphics interface (address plus a hash of all data memory at the `xgkick`):
**5,947,088 time slices, 14,546,494 xgkicks, 0 differences; no slice left uncompared.**

**Running the game** (`.cache/r1b-runvu1`: game program, VU0 and VU1 all translated; `--vu1 run`). The session
reached the same race as `r1b-runvu0` (same car, track and positions; the screenshots at frame 3450 differ by one
hundredth on the lap timer), so they are directly comparable. 11.0 million translated microcode runs, **no
hand-over to the reference core**. Means over all 1,380 race refreshes:

| Race, same race | VU0 translated (`r1b-runvu0`) | + VU1 translated (`r1b-runvu1`) |
|---|---|---|
| Work per refresh, VM thread (mean) | 6.22 ms | **4.48 ms** |
| ... VU1 microcode | 2.31 ms | **1.09 ms** |
| ... game program (EE) | 1.86 ms | 1.72 ms |
| ... VIF1 + GIF | 1.38 ms | 1.09 ms |
| Busy-refresh p99 / worst | 17.02 / 28.4 ms | **12.06 / 24.9 ms** |
| Refreshes over 16.7 ms | 17 | **4** |

The VU zone of the core's profiler still contains the VU executor loop and the image validity check around the
translated code; per busy refresh (8 ms or more of work) it was 4.78 ms with the reference core (§11) and is 2.55 ms
translated - but fewer refreshes are busy now (381 instead of 625), so the means over all refreshes above are the
fair comparison. Not done yet: VU1 on its own thread (ADR 0008 point 5), the converter stage that compiles once
and keeps the code (today everything is compiled when the launcher starts: about 3 minutes for the game program,
8 s for VU1). The smoke/glow defect (R3) is unchanged, as expected: it is not in the translated code.

## 14. R1c: the translated code built once by the converter (2026-11-14)

[ADR 0009](adr/0009-code-package.md). The converter's new stage "code" translates every discovered function and the
VU1 microcode, compiles the modules in parallel for `x86-64-v3` and links `build/hp2code.dll` with LLD; the normal
(release) launcher loads it, checks it and runs on it. Measured on the owner's PC with the test folder
`.cache/r1c-game` (installed mode, ROM referenced in place, no asset unpacking):

| | |
|---|---|
| Stage "code" | 26.5 s: translation 2.1 s, compilation 24.3 s (16 threads, 34 object files), link 0.1 s |
| Game functions | 10,146 found, **10,123 translated** (23 not: the reference core runs those), 75,545 re-entry places |
| VU1 | 1 image, 136 block starts, 1,424 further entries (every other pair) |
| DLL | 27 MB, no C runtime; launcher start to boot **0.4 s** (was about 3 minutes of compilation) |

**The scripted race on the normal launcher** (`.cache/r1c-run`, release build, no measurement hooks): 19.8 million
calls into the translated game program, 10.7 million into translated VU1 microcode, **0 VU1 blocks on the reference
core**; the screenshot at frame 3450 shows the same race at the same instant as the measurement runs (lap timer
6.32). Same launcher with `--reference` (`.cache/r1c-ref`) for comparison, refresh timing from `--frametimes`:

| Refresh (vblank) interval, 4,600 refreshes | Reference core (`--reference`) | Translated (`hp2code.dll`) |
|---|---|---|
| Late (over 1.5x the 16.67 ms period) | 337 | **26** |
| Uneven | 14.5 % | **1.3 %** |
| p99 / worst | 34.2 / 536 ms | **16.96 / 84.6 ms** |

**Open observation (not caused by this stage, cause not identified):** in races, presented images 50 ms apart (the
game producing an image every third refresh instead of every second) are more frequent whenever the game program
runs translated: 150 here, 116-142 in the earlier translated measurement runs, 51 in reference runs - but also 159
in the VU1 check run, where everything ran on the reference core with heavy checking overhead. The refreshes
themselves are on time here, so host speed alone does not explain it. Suspects: how the translated code accounts for
the cycles it spends (only backward branches consume the time slice), or the hand-off between the VM thread and the
graphics thread. To investigate next; it matters for "works well".

## 15. VU1 on its own thread? (2026-11-14)

Measured before building it ([ADR 0010](adr/0010-vu1-thread.md)):

- **Hand-off cost** (two spinning threads on this PC, double-buffered): serial 1,105 ns per item (350 ns VIF-like +
  650 ns VU-like work), pipelined 710 ns (64 %), strict wait-for-each 1,258 ns (114 %). Other sizes: 64-73 % and
  103-111 %.
- **The game's VIF1 stream** (`--trace-vif 3300-3600`, `tools/recon/vifstream.py`, 150 race frames, 575 MB): per
  frame about 3,400 `MSCAL`, 10,900 `UNPACK` (947 KB), 1,700 `ITOP`, 1,270 `DIRECT`, 550 `FLUSHA`; 87 % of the unpacked
  data comes between a program start and the next command that must wait for it, 97.5 % of that into the double
  buffer; the window is ended by the next `MSCAL` (436,458 times), `FLUSHA` (70,938) or `DIRECT` (3,853).
- **The reference core** starts a program only at the next resuming command (not at `MSCAL`), then stalls VIF1 until
  the VU is ready; a stalled VIF1 is retried once per scheduler turn (every 4,800 EE cycles, about 1,024 per
  refresh). The GS register buffer is shared and not thread-safe.

Conclusion: inside the reference core a VU1 thread has no overlap to exploit and would cap VIF1 at one program per
scheduler turn (fewer turns than programs per refresh); it belongs to the own runtime's VIF1/GIF (R2), where programs
start at `MSCAL` and the game's double buffering can be used. Not built.

## 8. Reproduce

```powershell
pwsh tools/fetch-play.ps1                               # once
pwsh tools/build.ps1                                    # dev build: hp2recomp.exe and its tests
pwsh tools/build.ps1 -Preset profile -NoTest            # measurement build of the reference launcher
pwsh tools/run-launcher.ps1 -RunName r1-trace3 -Preset profile -Rom ".cache\m1-run\game\rom" -Frames 4600 `
     -ShotEvery 1150 -Mute -Size 1280x896 -Res 2 -Trace `
     -Script "800:start,1100:start,1400:start,1700:cross,2000:cross,2300:cross,2600:cross,2900:cross:1600"
$elf = "ROM\Need for Speed - Hot Pursuit 2 (USA)\SLUS_203.62"
.cache\build\dev\recomp\hp2recomp.exe info $elf
.cache\build\dev\recomp\hp2recomp.exe check-trace $elf .cache\r1-trace3\trace        # exit code 0 = all checks pass
python -B tools/profile.py .cache\r1-trace3\trace\profile.csv --from-frame 3120 --to-frame 4500 --label race
python -B tools/recon/vucheck.py $elf .cache/r1-trace3/trace
# translator spike (needs LLVM: tools/fetch-llvm.ps1, tools/build-llvm.ps1)
pwsh tools/run-launcher.ps1 -RunName r1-counts2 -Preset profile -Rom ".cache\m1-run\game\rom" -Frames 4600 -Mute -Trace -Extra '--trace-counts' -Script "<as above>"
.cache\build\dev\recomp\hp2recomp.exe hot $elf .cache\r1-counts2\trace 25
.cache\build\dev\recomp\hp2recomp.exe lift $elf .cache\r1-counts2\trace 400 --out .cache\r1-lift-list.txt
& .\tools\run-launcher.ps1 -RunName r1-lift2 -Preset profile -Rom ".cache\m1-run\game\rom" -Frames 4600 -Mute -Trace -Extra @('--lift-check', '.cache\r1-lift-list.txt') -Script "<as above>"
.cache\build\dev\recomp\hp2recomp.exe functions $elf --out .cache\r1-lift\functions.tsv
python -B tools/recon/liftsummary.py .cache/r1-lift2/trace/lift-check.tsv .cache/r1-counts2/trace/blocks.tsv .cache/r1-lift/functions.tsv
# VU1 microcode (section 13)
.cache\build\dev\recomp\hp2recomp.exe vu1-lift $elf
& .\tools\run-launcher.ps1 -RunName r1b-vu1check2 -Preset profile -Rom ".cache\m1-run\game\rom" -Frames 4600 -ShotEvery 1150 -Mute -Size 1280x896 -Res 2 -Trace -Extra @('--vu1', 'check') -Script "<as above>"
& .\tools\run-launcher.ps1 -RunName r1b-runvu1 -Preset profile -Rom ".cache\m1-run\game\rom" -Frames 4600 -ShotEvery 1150 -Mute -Size 1280x896 -Res 2 -Trace -Extra @('--run-translated', '.cache\r1b-all.txt', '--vu1', 'run') -Script "<as above>"
python -B tools/profile.py .cache\r1b-runvu1\trace\profile.csv --from-frame 3120 --to-frame 4500 --label race
# converter stage "code" and the normal launcher (section 14)
pwsh tools/fetch-llvm.ps1; pwsh tools/build-llvm.ps1      # once: adds lld/ and libunwind/ from the same archive
pwsh tools/build.ps1 -Preset release
.cache\build\release\converter\hp2pc-cli.exe convert --rom "ROM\Need for Speed - Hot Pursuit 2 (USA)" --install-dir .cache\r1c-game --rom-storage reference --no-assets
.cache\r1c-game\build\HotPursuit2.exe --frames 4600 --mute --size 1280x896 --res 2 --frametimes .cache\r1c-run\frametimes.csv --script "<as above>"   # add --reference for the baseline
python -B tools/frametimes.py .cache\r1c-run\frametimes.csv
# VIF1 stream (section 15)
& .\tools\run-launcher.ps1 -RunName r1d-vif -Preset profile -Rom ".cache\m1-run\game\rom" -Frames 3700 -Mute -Size 1280x896 -Res 2 -Trace -Extra @('--trace-vif', '3300-3600') -Script "<as above>"
python -B tools/recon/vifstream.py .cache\r1d-vif\trace\vif1-stream.bin
```
(`-Extra` with two values must be passed from inside PowerShell, as shown; through `pwsh -File` the list arrives as one argument.)

The trace folder contains data derived from the game (a copy of its memory, its code addresses, its microcode)
and is never committed.
