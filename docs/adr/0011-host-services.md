# ADR 0011 — R2: host services replace the game's system libraries at their entry functions

- **Status:** Proposed (2026-11-16) from the R2a measurements; nothing of R2 is implemented yet.
- **Constraints in play:** C1, C4, C7; owner requirement 1 (native, full use of the PC).
- **Evidence:** `docs/recomp-analysis.md` §17; `hp2recomp refs`; launcher `--trace-calls`, `--trace-disc`;
  `tools/recon/irxinfo.py`; `.cache/r2a-calls`, `.cache/r2a-fio`, `.cache/r2a-disc`.

## Context

ADR 0006 defines R2 as "host services where the game calls its libraries: disc reads from files, pad with analog
triggers and rumble, memory card, timers". R2a measured what those libraries are and what they cost.

- The executable links Sony's libraries statically (markers `PsIIlibgraph`, `libdma`, `libpad`, `libkernl`,
  `libcdvd`, `libmc`, `libscf`, versions 2500/2530). No symbols, but their functions can be found by the RPC server
  numbers they bind to and the messages they print (`hp2recomp refs`).
- The game talks to 12 IOP servers: file I/O `80000001`, module loading `80000006`, pad `80000100/101`, memory card
  `80000400`, disc `80000592/593/595/597/59A`, **the game's own streaming server `00012345`** (`STREAM.IRX`,
  "multi_streamer": music and sound streaming, sent a command about every refresh) and a Logitech wheel driver
  `046D046D` (`LGDEV.IRX`).
- **CPU cost is negligible:** all code in the library address range is 1.95 % of executed game instructions
  (the largest: a pad function 0.65 %, `sceCdSync` polling 0.26 % - 1.43 million calls in the session).
- **Loading is what the libraries cost.** The session reads 39.6 MB from the disc, 36.4 MB of it from `ZZDATA.BIN`,
  always in single 2048-byte sector reads. While loading a race the game issues about 20 read requests per second
  (one every 3 refreshes, each waited for by polling) and gets 0.3-1.2 MB/s: the 14.5 MB of the race load take
  11.7 s. The host disc is not the limit; the request/poll round trip through the emulated IOP and drive is.

## Decision (proposed)

1. **Intercept at the library's entry functions, in translated code.** The translator gets a table of host
   services keyed by function address (valid for the one supported executable, identified by its SHA-256; the
   addresses are found with `hp2recomp refs`, not typed by hand). A call to such a function calls the host
   implementation instead; its translated body is not emitted. The reference core keeps running the same game with
   the original functions, so every service can be compared against it (same data returned, same files).
2. **Order:** (a) disc reads (`sceCdRead` at 0028D938, the reader at 0028D620, `sceCdSearchFile`, `sceCdSync`,
   status calls) served directly from the ROM folder, completing at once - the measurable gain is loading time;
   (b) pad through XInput with analog triggers and rumble; (c) memory card as files; (d) timers. The streaming
   server belongs to R4 (audio).
3. **Acceptance of (a):** the scripted session with no read going through the emulated drive after boot, identical
   data delivered (hash of every buffer against the reference), and the race load time measured before/after.

## Amendment (2026-11-16, same day): owner requirement 4 - no emulator layer whatsoever

The owner requires the working recompilation to run without any emulator layer. This makes the direction of this
ADR mandatory rather than an optimisation, and widens it: the reference core stays only as a development oracle.
Order of replacement, each step verified against the oracle before the next:

| Step | Replaces in the reference core | Notes |
|---|---|---|
| R2b | disc (libcdvd, file I/O) | this ADR, point 2a |
| R2c | pad, memory card, timers | XInput, files |
| R2d | EE kernel services the game uses (threads, semaphores, interrupt and vblank handlers, syscalls) | list them from the executable's syscall sites first |
| R2e | every remaining hand-over to the reference JIT (23 functions, 355 places: branch in a delay slot etc.) | translated code must be complete; no interpreter fallback |
| R3 | DMA, VIF1, GIF and the GS renderer (own VIF1 with VU1 on its own thread per ADR 0010; own renderer) | the largest part |
| R4 | the sound streamer and SPU2 (native mixer) | |
| R5 | own launcher without the Play! libraries linked; frame scheduler | acceptance: the release launcher links no reference-core code |

The translated code's use of the reference core's state structure is only a memory layout; it moves to an own
structure in R5 (the code package is rebuilt by the converter, ADR 0009).

## Not verified

- Which of the disc's IRX modules run as IOP code in the reference core and which it replaces by its own versions
  (it reads all of them from the disc).
- That 0028D620 is `sceCdReadChain` or EA's own wrapper: 533 calls, always with the same request block; its
  argument layout still has to be decoded before (a).
- The expected loading gain: not measured until (a) exists.

## What would reopen this ADR

- The game depending on the timing of disc reads (streamed track data during a race).
- Library entry points that cannot be told apart reliably without symbols.
