# HANDOVER — current state of the native runtime (updated 2026-11-25)

> Development resumed 2026-11-25 (owner: "continue the native port, document so other models can resume when I say *timeout*"). The section "Progress 2026-11-25" below is the newest; the rest is the 2026-11-16 pause note and is partly superseded.

## Stage 2, 2026-11-25 evening (newest: read this first, then the section below it)

Owner request: "continue everything and get a playable build with the QoL I asked for". **Not reached; the honest
distance is below.** What this stage achieved (all in the native runtime, no emulator code):

- The game runs its start-up, **opens `ZDIR.BIN` from the disc, searches `ZZDATA.BIN`, reads 39 times**, syncs VIF1
  (MARK `ABCD`, 105 packets), and runs 15+ s stable (900+ refreshes) in its own state loop at `0x00245830`.
- Host services now in `native/src/main.cpp`: disc (`g_disc` = project's `hp2vdisc` + a mini ISO 9660 reader
  `DiscFind`), the game's file API as C-level hooks (`kHookOpen/Close/Lseek/Read` = 0x286980/0x286C08/0x286D88/0x286FC0),
  libcdvd RPCs (`80000593` fn 3 disk type = 0x14, fn 4 error = 0; `80000597` fn 0 search file, result written to the first
  36 bytes of the send buffer, path at +36; `80000595` fn 1 read, fn 15 read chain = (sector, count, address) x n ended
  by -1), EA streamer (`00012345` fn 0, `StreamerService`: protocol decoded from the reference core, see below), pad
  fn 1 reply copied from the reference, SIF RPC *end functions* are now called, VU1/VU0 memory windows are real
  arrays (`VuMemory`, MPG microprograms are stored in `g_vu1Micro`), VIF1 parser fixed (no alignment before MPG/DIRECT).
- Translator: `pexcw`, `pmfhi`; development switch `HP2_TRACE_ENTRIES=1` when building the package makes every function
  entry call the runtime; the run then writes `.cache/native/functions.txt` (functions in order of first entry with
  counts). Compare with the reference's `blocks.tsv` to find where the native run diverges (script idea in this stage:
  map block -> function with `hp2recomp functions <elf> --out`, list functions the reference ran by refresh N that the
  native run never entered). `entries.txt` lists runtime re-entry places.
- Reference tooling: `engine/src/ref_trace.cpp` now dumps the streamer command/reply buffers to `rpc.bin`
  (`--trace-calls 26A1F0,26A1B8`, record = tag 'S'|'R', frame u32, 2048 bytes; written to the launcher's working
  directory). `--trace-calls 285958` lists every `sceSifCallRpc` (a0 client, a1 function). The launcher works only with
  `--disc .cache\p4a\hp2.iso` (the `--rom` path of `tools/run-launcher.ps1` produced no output in this stage).
- Tests: `pwsh tools/build.ps1 -Preset dev` = 15/15 after all of this. Reference-mode session checks NOT re-run.

**EA streamer protocol (decoded, in `StreamerService`):** request = u16 array `[count][id, nargs, args...]`; reply =
u32 words: w1 = batch counter, w2 = -1, w4 = result of the last command 0x0E `(a0<<16)|a1`, after the first command 0x34
words 66/67/68/70 = 2, 3, `(a0<<16)|a1`, that + 0x20000. Streams (w0 > 0, 6-word entries) are not modelled (no sound).

**Where the native run is stuck now:** in the loop at `0x00245830` (a state machine on an object whose state word at
+376 is 16/15/2; virtual call through the object's table). The reference calls `sceSifCallRpc` on client `0x33A700`
(server `80000006`, module loader) **10 times with function 0 at refresh 3** and on `0x33A440` (server `80000001`)
functions 0/4/2/1 (open, seek, read, close of the IRX files); the native run only makes the function-255 init call and
never loads modules. Functions the reference runs by refresh 37 and the native run never enters: libpad
`0x281E38, 0x2822A8, 0x282708, 0x2823D8, 0x282840, 0x282440`. Next: find why the game does not request the modules
(compare `0x00289720` init and the loader functions, reply words of function 255), serve `80000006` fn 0 (return a
positive module id; for `80000001` fn 0/4/2/1 serve the IRX from the disc), then pad (`80000100/101`), memory card
(`80000400`: fn 254, then 1/2/3), wheel (`046D046D` fn 11 every frame).

**Honest distance to "playable with the owner's QoL"** (not a promise, my estimate of the missing blocks):
1. Finish the I/O hosts above so the game leaves its loading state (hours to days).
2. Run the VU1 microprograms (translator exists, ADR 0008/0010; the native runtime must start them on MSCAL/MSCNT,
   run XGKICK output into GIF).
3. **A GS renderer of our own** (GIF packet decoder, texture/CLUT/local-memory handling, a D3D11/OpenGL back end). This is
   the largest block and is also what makes owner requirement 2 (modern graphics settings inside the game's menus)
   possible. 4. Sound: EA's IOP streamer + SPU2 (own implementation of the streamer protocol and a mixer).
5. Input (XInput/keyboard through the pad structure), memory card as host files, wheel (optional).
The playable build today is still the reference-core launcher (`HotPursuit2.exe`, Play! core), which does not meet
requirement 4.

## Progress 2026-11-25 (earlier the same day)

Run: `pwsh tools/native-run.ps1 -Seconds 15` (builds, rebuilds the 30 s code package, runs; `-NoPackage` when only `native/` changed). Log: `.cache/native/native.log`.

**The game now runs natively through its whole start-up** (no emulator code anywhere): kernel set-up, heap, the game's own kernel-mode layer, SIF/RPC set-up, the I/O servers it binds, pad/wheel/streamer set-up, first DMA, timed waits. It stops waiting for an interrupt from VIF1 (see Next).

Fixed this session (all in `native/src/main.cpp` or `recomp/`):
- `EndOfHeap` (service 0x3E) returns the limit `g_heapEnd` (was the start).
- **Kernel syscall table at 0x80002000**: `SetSyscall` (0x74) writes there; any syscall whose table entry is set runs that *game* function (the game installs its own syscalls 8,84-91,131,252-255,300 and searches kernel memory for them).
- **Relocated code** (ADR-worthy, `recomp/include/hp2/native_state.h` `kNativeOverlays`): the game copies two blocks of its own code into kernel memory: 1856 B from 0x00328E58 to 0x80076000 (interrupt/exception helper layer, `mfc0/eret`) and 816 B from 0x00328020 to 0x80075000 (TLB manager, `tlbwi`...). They are translated as extra images (`ElfImage::code_blob`, `CodeOverlay`, second `Translator` per blob with its own `dispatch_symbol`). A third blob of 40 bytes (0x00329598 -> 0x80082000, thread-exit stub) is NOT translated (not needed: threads end at the runtime's return address).
- Translator additions: MMI `pmaxw pminw psubb pcpyh pexew prot3w pmflo`; `cfc2/ctc2` on VU control regs 27-31 (native only: VPU_STAT reads 0).
- `sceSifInitRpc` (0x00285118/0x0028511C) is a host service (sets the game's init word 0x00327F7C); `sceSifBindRpc/CallRpc/CheckStatRpc` hooks as before.
- COP0 Count (`mfc0 c0r9`) follows the host clock at 294.912 MHz (the game's timed waits).

**Later the same day:** a minimal DMA engine exists (`RunDma`/`ParseVif1` in `native/src/main.cpp`): chain/normal transfers complete at once, VIF1 codes are parsed (interrupt bit -> INTC cause 5 delivered by the scheduler, MARK -> `VIF1_MARK` register, MSCAL counted but VU1 programs not run, UNPACK/MPG/DIRECT skipped by size). With it the game leaves the VIF1 wait and reaches its main loop (thread 1 at ~0x001AE6E8) making ~850 RPC calls in 15 s - the polls of the I/O servers still answered with zeros. Tests: `pwsh tools/build.ps1` = 15/15 pass (dev preset, /WX) after these translator changes; reference-mode session checks NOT re-run.

### Next (in order)
0. **Find what the main loop at ~0x001AE6E8 waits for** (it polls RPC functions; `.cache/native/native.log` lists the first call per server/function; add logging of repeated calls if needed) and serve it: most likely module-load (`80000006`) and file/disc servers (ADR 0011), then pad (`80000100/101`).
1. (done, see above) **VIF1/GIF/DMA minimum (R3 start).** Thread 1 spins at 0x00111E00 (function 0x00111DD8) waiting for words at gp-25248/-25244 that the INTC handler 0x00111FD8 (cause 5 = VIF1) clears. The game marks its VIF1 packets with the interrupt bit and waits. Implement in `HwWrite` (DMA CHCR start): walk the DMA tag chain (source chain mode), for channel 1 parse VIF codes (I bit -> raise INTC cause 5 and run the handler; MSCAL/MSCNT -> VU1 microcode via the translated image, see ADR 0008/0010; UNPACK -> VU1 memory), for channel 2 GIF packets (discard for now, later the renderer).
2. I/O host services (ADR 0011): servers `80000592/593/597/595/59A` (disc), `80000006` (module load: report success), `80000001` (files), `80000100/101` (pad), `80000400` (memory card), `00012345` (EA streamer), `046D046D` (wheel). All still return zeros.
3. Remaining translator gaps printed by `hp2recomp package` (`break` 266 = asserts, `bc0t/bc0f`, `tlb*`, `eret` in the kernel layer, `mfsa/mtsa/mtsab`, `pexcw`, `pmfhi`, `vcallmsr`, `vlqi/vsqi`, 193 'code outside the analysed function', 168 `syscall` sites handled by the runtime).
4. Still owed to the owner: **GitHub dev build `v0.2.0-dev.1`** (nothing pushed; `gh` is logged in as the repo owner); ADR 0012 (native runtime) + `docs/recomp-analysis.md` section 18; re-run `pwsh tools/build.ps1` (tests) and the reference-mode checks (translator changed: beql/break pattern, MMI ops).

---

## (older) 2026-11-16 pause note

The owner paused development in the middle of the first native-runtime step. Read `docs/STATUS.md` first (project
state, owner requirements, ADRs), then this file (what was in progress and the exact next actions).

## Owner instructions in force (newest last)

1. Requirements 1-4 in `docs/STATUS.md` (native, full use of the PC; graphics settings inside the game's menus;
   **no emulator layer whatsoever** in the working recompilation).
2. 2026-11-16: **"Make first a base executable that runs NATIVELY and with no emulation anywhere, behaving like a PC
   game. First step is to get it running and we go from there."**
3. 2026-11-16: **"Post the progress in the GitHub repo as a dev build version with the appropriate numeration."**
   This supersedes the earlier "publish only when it works well" rule for *dev builds*. **Not done yet** (see below).
4. 2026-11-16: the GitHub README must state that the game was ported because **the PS2 version is the best version
   of the game, and this way the best version runs on the best platform** (to be written in good style).
   **Not done yet.**
5. 2026-11-16: pause; leave everything ready for another agent.

Working rules: explain every command, never delete project files (archive them instead), repo docs in English, no
game data in git.

## What is in progress: the native runtime (to become ADR 0012 — not written yet)

`native/` → `HotPursuit2Native.exe`: links **none** of the reference core (only `hp2recomp`: ELF reader, helpers).

- Loads `SLUS_203.62` from the ROM folder into its own 32 MB, loads `hp2code.dll` built for **its own state layout**
  (`recomp/include/hp2/native_state.h`: `NativeState`, `native_code_layout()`, the hook list `kNativeHooks`).
- Build the package with `hp2recomp package <elf> <out.dll>` (new command; sets `CodePackageOptions::native` and
  excludes the hooked functions). Translator additions for native mode (`StateLayout::yield_in_idle_loops`):
  waiting loops are translated and return to the runtime after each pass; `ei/di/mfc0/mtc0` call runtime slot 2
  (`NativeRuntime::system`); the compiler's division check (`beql; break` in the delay slot) is translated in both
  modes (this removed all 247 "branch in a delay slot" hand-overs).
- Runtime (`native/src/main.cpp`): EE threads as Windows fibers, scheduler by priority, kernel services (threads,
  semaphores, interrupt/DMA handlers, vblank at 59.94 Hz from the host clock, heap), hardware registers (timers,
  INTC, DMAC with transfers completing at once, GS CSR; others logged once), SIF RPC served at the library's entry
  functions (`sceSifBindRpc` 00285788, `sceSifCallRpc` 00285958, `sceSifCheckStatRpc` 00285B48) - **all servers
  still return zeros** (logged on first use), a watchdog that reports where game code is if it does not return.
- One command builds, repackages and runs it: `pwsh tools/native-run.ps1 -Seconds 6` (`-NoPackage` skips the 26 s
  package rebuild when only `native/` changed). Log: `.cache/native/native.log`. `--verbose` lists syscalls.

### How far it gets (last run)

The game's start-up code runs natively: crt0, kernel setup, 67 entries into native code, 9 kernel calls. It then
stops at `break 1` in 00250AB0, called from the assert `"Trouble reserving heap in bInitPlatformMemory()"`
(`src\bMemory.cpp`, caller 0025763C): the game's `sbrk` (0028BA48) fails for its 30 MB heap request (0x01CB2720).

**Known cause, fix not applied (one line):** in `Syscall()` of `native/src/main.cpp`, service `0x3E` (EndOfHeap)
currently returns `g_heapStart`; the game's `sbrk` compares `EndOfHeap() < current + size` and fails, so it must
return the **limit**: `g_heapEnd`. (The main stack is already placed at the top of memory, 01FFC000, which is what
makes the 30 MB request fit.) Apply, run `tools/native-run.ps1 -Seconds 6 -NoPackage`, continue from what it reports.

### Expected next obstacles (in order of likelihood)

- Kernel service 0x63 (called once at 00100168 with a0=0x15, a1=1) is unknown; 0 is returned.
- The I/O servers (ADR 0011): module loading `80000006`, file I/O `80000001`, disc `8000059x`, pad `80000100/101`,
  memory card `80000400`, EA's streamer `00012345`, wheel `046D046D`. Each needs a host service; start with what
  the log shows first. The reference build can show the real replies (`--trace-calls`).
- Remaining hand-over instructions the translator does not cover (the package command prints them): `break` 265
  (asserts and division traps), MMI ops (`pmaxw` 42, `prot3w` 9, `psubb` 8, `pminw` 4 ...), `bc0t/bc0f` 5,
  `cfc2/ctc2` on other registers, `mfsa/mtsa/mtsab`, `vlqi/vsqi`, and 194 "code outside the analysed function".
  With no emulator there is no fallback: each must be translated (or shown never to execute).
- No graphics, sound or input yet: DMA data is discarded. Those are R3/R4 of the plan in ADR 0011's amendment.

## Not done from the owner's latest requests

- **README rationale** (item 4 above): add near the top of `README.md`; also refresh its status block (it still
  says 2026-11-12 and "embedded emulator core").
- **ADR 0012** (native runtime) and `docs/recomp-analysis.md` §18: write once the runtime gets past start-up.
- **GitHub dev build** (item 3): nothing has been pushed (`main` is ahead of `origin/main`; `gh` is logged in as
  the repo owner). Proposed numbering: tag `v0.2.0-dev.1` (converter is `0.1.0`; the native runtime prints
  `0.2.0-dev.1`), GitHub pre-release with notes saying exactly how far the native executable gets. Decide with the
  owner whether to attach binaries (they contain no game data; the converter carries LLVM/LLD, notices included).

## Verified / not verified at the pause

- Verified: release build compiles with the native runtime; the native package builds (10,125 functions, 26 s);
  the run above. All 15 tests passed before the native work started (commit `587afaa`).
- **Not verified:** the test suite after the translator changes of this step (run `pwsh tools/build.ps1` first);
  that the reference-mode package (`.cache/r1c-game`) still behaves identically with the `beql; break` change
  (re-run the `--vu1 check` / `--lift-check` sessions of `docs/recomp-analysis.md` §8 before trusting it).

## Stage 3, 2026-11-25 night

The state loop at 0x00245830 was NOT the module loader: it ran the file manager's start (0x0012AA40 -> 0x0028E8A0), which returned -100
because the manager's client (0x0033C0C0, server `80000400`) was dropped by its init (0x0028DD40): the reply of function 254 must have
word 1 >= 522 and word 2 >= 526 (version words). Now answered in `Hook()` with {0, 522, 526}. Also fixed in the lifter: `can_enter` accepts
the `beql divisor,zero; break 7` block, so a quota yield there can be re-entered (it stopped the run at 0x00236DA8).
Result (40 s): the game runs without stopping, ~60 refreshes/s, 984 disc reads (ReadChain from ZZDATA.BIN), 33816 VU1 starts (not run),
memory card calls (80000400 fn 1/2/3/5, answered with zeros = no card). Thread 1 idles at 0x00111E00 (frame sync). Tests 15/15 (dev).
The 80000006 loader is still only called with fn 255 (the game does not need IRX loading in this path).
Next: see what the game does now (add logging of the first calls of each new server function and of GIF/VIF1 packets), then VU1 execution
from `g_vu1Micro` on MSCAL/MSCNT, XGKICK -> GIF, own GS renderer, input (pad 80000100/101), sound.

## Stage 4, 2026-11-26: the native runtime draws

New in `native/src/main.cpp`: VIF1 UNPACK (all formats, CL/WL, STMASK/STROW/STCOL, STMOD, BASE/OFFSET/ITOP, TOPS double buffering), VU1
microprograms run on MSCAL/MSCNT with the translated images from `hp2code.dll` (`RunVu1`, `VuCodeAt`; XGKICK port write = quadword
index, so address = value*16), and a streaming GIF decoder (`GifFeed`, paths 1 XGKICK / 2 DIRECT / 3 DMA2).
New `native/src/gs.cpp` + `gs.h` (own code): GS local memory 4 MB with the page/block/column layouts (CT32/24/16/16S, T8, T4, T8H,
T4HL/HH, Z32/24/16/16S), host-to-local and local-to-local transfers, registers (PRIM, RGBAQ, ST/Q, UV, XYZ(F)2/3, TEX0/1, CLAMP,
XYOFFSET, SCISSOR, ALPHA, TEST, FRAME, ZBUF, TEXA, PABE...), a software rasteriser (triangles, strips, fans, sprites; Gouraud, perspective
ST or UV, nearest or bilinear, CLUT, modulate/decal, alpha blend, alpha test, Z test, frame mask) and a Win32 window that shows the display
buffer (PMODE/DISPFB/DISPLAY) at every vblank. `gs::DumpFrame` writes `.cache/native/frame.bmp` at the end of a run (dev only).
Verified by eye (frame dump): the EA Games logo and, 45 s later, the Ferrari intro movie render correctly; 15 VU1 runs per frame with no
missing microprogram; 1034 frames in 45 s (software GS ~23 fps wall: needs speed work); tests 15/15 (dev).
Not done / known gaps: lines and points (counted, not drawn), fog, dithering, AA, the CLUT CSM2 path is approximate, TFX highlight modes
approximate, alpha-fail modes other than KEEP, local-to-host transfers, Z24 buffer shares the frame width, no mip-maps. No sound, no input
(pad 80000100/101 returns the reference's constant reply), memory card = none.
Display is 512x444 here (DISPLAY DW/MAGH of the game); window scale setting `gs::SetWindowScale` exists but nothing calls it yet.
Next: (1) input: pad server 80000100/101 from keyboard/XInput so the menus can be driven; (2) speed: tile/threads or move the rasteriser to the
GPU (D3D11 back end fed by the same register stream) - this is also where the owner's QoL goes (internal resolution multiplier, texture
filtering, aspect, widescreen) and where requirement 2's in-game options menu hooks in; (3) sound (EA streamer + SPU2 mixer);
(4) ADR 0012 and `docs/recomp-analysis.md` section 18; (5) GitHub dev build `v0.2.0-dev.1` (only if the owner asks).
