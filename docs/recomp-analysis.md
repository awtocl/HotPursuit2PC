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
```
(`-Extra` with two values must be passed from inside PowerShell, as shown; through `pwsh -File` the list arrives as one argument.)

The trace folder contains data derived from the game (a copy of its memory, its code addresses, its microcode)
and is never committed.
