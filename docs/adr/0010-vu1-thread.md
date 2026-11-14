# ADR 0010 — VU1 on its own thread: not inside the reference core's VIF model; with the own runtime (R2)

- **Status:** Accepted (2026-11-14). Decides ADR 0008 point 5: **deferred to R2**, with the measurements below.
- **Constraints in play:** C4 (evidence), C7, owner requirement 1 (full use of the PC's hardware); the reference core
  stays unmodified (ADR 0005/0006).
- **Evidence:** `docs/recomp-analysis.md` §15; `tools/recon/vifstream.py`; launcher `--trace-vif`; `.cache/r1d-vif`.

## Context

With the game program and VU1 translated (ADR 0007-0009), a busy race refresh costs about 9.7 ms on the VM thread:
game program 4.0, VU1 2.55, VIF1 1.40, GIF 1.14 ms (`r1b-runvu1`). Running VU1 on a second core could take its share off
the critical thread, as some emulators do.

## What was measured

1. **Hand-off cost on this PC** (microbenchmark, two spinning threads, double-buffered input): VIF-like work 350 ns
   plus VU-like work 650 ns per item costs 1,105 ns serially, **710 ns pipelined** (64 %), but **1,258 ns** (114 %)
   when the producer must wait for every item. A second thread pays only if VIF1 really keeps working while VU1 runs.
2. **What the game sends to VIF1** (the stream of 150 race frames, 575 MB): about 3,400 microprogram starts per frame;
   947 KB of UNPACK data per frame, 87 % of it between a start and the next command that must wait for the program,
   97.5 % of that into the double buffer (relative to TOPS). The game itself is written for the overlap.
3. **How the reference core runs it:** `CVif` does not start a program at `MSCAL`; it starts it at the next command
   that resumes a delayed program (`ITOP`, `FLUSH*`, `MSCAL*`, `DIRECT*`, end of the packet) and then stalls (`VEW`)
   until the VU is ready. The UNPACKs the game meant to overlap are executed before the program starts. A stalled VIF1
   is retried only at the next scheduler turn: the core gives the units a turn every 4,800 EE cycles, about **1,024
   turns per refresh** - fewer than the 1,700-3,400 programs per refresh, so a VU1 that finishes "later" would cap
   VIF1 at one program per turn and slow the game down in emulated time.
4. **Graphics output:** every path writes GS registers through `CGSHandler::WriteRegister`, an inline append to one
   shared buffer, which the VM thread also submits on its own (vertical blank, other paths). Packets from a VU1
   thread would race with it; there is no seam to serialise this without modifying the reference core.

## Decision

- **Not now, not inside the reference core.** Moving VU1 to its own thread within Play!'s VIF model gains nothing (the
  overlap window is empty there) and would slow the game; making it gain needs changes to the reference core's VIF1,
  GIF and scheduler, which the project keeps unmodified.
- **With the own runtime (R2), as part of its VIF1/GIF design:** start a program at `MSCAL` as the hardware does, let
  VIF1 unpack into the double buffer while it runs, wait only at `MSCAL`/`FLUSH*`/`DIRECT*`/non-buffered UNPACKs,
  hand packets to the graphics side through an ordered queue. Expected from the measurements: up to about 1.4 ms
  (VIF1 hidden behind VU1) per busy refresh with a spinning hand-off, more if packet building moves too.

## What would reopen this ADR

- A busy refresh that no longer fits 16.7 ms with the remaining work on one thread before R2 exists.
- A change upstream in the reference core that starts programs at `MSCAL` or offers a thread-safe GS submission path.
