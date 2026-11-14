# ADR 0009 — The translated code is built once by the converter, as a DLL

- **Status:** Accepted (2026-11-14), implemented in the same change (`docs/recomp-analysis.md` §14)
- **Constraints in play:** C1 (the dump is the only source), C6 (portable game folder), C7, owner requirement 1
  (native, full use of the PC's hardware)
- **Builds on:** ADR 0006 (no game code is distributed: translation happens on the user's machine), ADR 0007 (LLVM in
  the converter; "nothing compiled while playing"), ADR 0008 (VU1)

## Context

Until now the translated game code existed only inside the measurement launcher, compiled at start-up (about three
minutes for the game program and eight seconds for VU1) and only with `--trace`. The product needs the code to be
made once and then simply used.

## Options considered

| Option | Verdict |
|---|---|
| A. The converter translates, compiles and **links a native DLL** (`build/hp2code.dll`) with LLD used as a library; the launcher loads it with `LoadLibrary` | **chosen** |
| B. The converter stores object files; the launcher links them in memory at start-up with LLVM's JIT linker | rejected: puts LLVM (tens of MB) into the launcher and still links at every start |
| C. Keep compiling in the launcher, with a disk cache | rejected: LLVM in the launcher, first start takes minutes |

LLD comes from the same pinned LLVM source archive already approved (same release, same Apache-2.0-with-LLVM-exceptions
licence); no new download. `tools/fetch-llvm.ps1` now also unpacks `lld/` and `libunwind/` (headers LLD needs);
`tools/build-llvm.ps1` builds LLD's libraries with the rest.

## Decision

1. A new converter stage, **"code"**, after "build": every function the discovery finds (10,146; 10,123 translate) and
   the VU1 microcode image, with an entry function for every microcode pair that is not a block start (so nothing is
   ever translated while playing). Modules are compiled in parallel, one per hardware thread, then linked with LLD
   into `build/hp2code.dll` (no C runtime: the DLL carries `_fltused` and the usual `__chkstk` stack probe itself).
2. **CPU level:** `x86-64-v3` (AVX2 generation, 2013 and later), not "this machine's CPU": the game folder stays usable
   when moved to another modern PC (C6). The launcher checks the instructions with `cpuid`; on an older processor it
   runs everything on the reference core.
3. **One table** exported by the DLL (`hp2/code_package.h`): format version, SHA-256 of the executable it was made
   from, a fingerprint of the reference core's state layout it was made for (`engine/src/reference_layout.cpp`,
   shared by converter and launcher), the CPU level, sorted function and entry tables, the microcode images with the
   words they were translated from. The launcher uses the package only if every one of these matches; the converter
   rebuilds it when its recorded identity (`build/hp2code.txt`) differs, keeping the old files aside.
4. The **normal launcher** loads it automatically when it sits next to it (`--reference` turns it off for
   comparisons); anything it does not cover - 23 untranslated functions, microcode that differs from the image -
   keeps running on the reference core, exactly as in the verified measurement runs.

## Consequences

- Measured: the stage takes 26.5 s on the owner's PC (translation 2.1 s, compilation 24.3 s on 16 threads, link 0.1 s);
  the DLL is 27 MB; the launcher starts in 0.4 s instead of about three minutes.
- The converter executable grows to 48 MB (LLVM and LLD inside); the launcher does not grow.
- The package depends on the reference core's state layout; a launcher change that moves fields invalidates it
  (detected by the fingerprint, fixed by running the converter again).

## What would reopen this ADR

- The own runtime of R2 replacing the reference core's machine model (the table then describes a different layout).
- Antivirus or loader problems with a locally generated DLL on users' machines.
