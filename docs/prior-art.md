# Prior art and third-party license verdicts (C5)

Checked 2026-11-11 (phase P0). Verdicts:

- **reuse-ok** — license permits use in this repo; still needs an ADR/STATUS note when actually pulled in.
- **reference-only** — may be read for format/behaviour knowledge; no code or data copied.
- **not used** — looked at, nothing taken.

**In use since phase P4b:** the Play! emulator core and the libraries it
bundles, linked statically into `HotPursuit2.exe` (see "Third-party software
in the launcher" below). Their source is *not* in this repository;
`tools/fetch-play.ps1` downloads the pinned revision. No third-party code or
data is in the tree itself.

## Asset formats

| Source | License | Verdict | What was confirmed | What remains UNVERIFIED |
|---|---|---|---|---|
| [Ekey/NFS.BIN.Tool](https://github.com/Ekey/NFS.BIN.Tool) (C#) | **None** — no LICENSE file or statement in the repo root | **reference-only** | README only: HP2 (PS2) is listed as tested with the "old format" flag (`NFS.Unpacker -true <ZDIR.BIN> <out>`); the packer is "not tested in games"; folders `NFS.Unpacker/{FileSystem,Projects,Properties}`. | Its source was **not read**, so whether its record layout, hash and name list agree with ours is unverified — and not needed: our layout, hash function and names were derived from the dump itself (`docs/dump-analysis.md`). Its name list was **not** used. |
| ResHax topic 18787, ZenHAX t=1614 (aluigi script), r/REGames 1b22u6q (listed in the brief) | forum posts | **not used** | — | Not consulted in P0; everything they would have provided was derived from the dump instead. |
| "JDLZ" LZ codec of later Black Box titles (widely described in community tools) | n/a (algorithm knowledge) | **not used** | Tried from memory as a hypothesis for HP2's `COMP` streams; **it does not decode them** (fails at output byte 8). HP2's codec is a different, simpler LZSS and was worked out from an LZC/BUN pair on the disc, then verified on all 11 pairs. | — |
| tcrf.net page on the PS2 version | — | **not used** | The fetched page returned text addressed to AI agents (instructions to write files / move money). Ignored; no information taken. | Its claims about developer leftovers (we found the notes independently: `MARKERS\TRACK%d.TXT`). |

## Running the game

| Source | License | Verdict | What was confirmed | What remains UNVERIFIED |
|---|---|---|---|---|
| [jpd002/Play-](https://github.com/jpd002/Play-) | **BSD 2-Clause** (`License.txt`, © 2006-2026 Jean-Philip Desjardins) | **reuse-ok — in use** (M1 core, ADR 0001 path C1, ADR 0005). Pinned: `1f27232772e484bfb79364b751b84f41f4c87481` (2026-10-24). Fetched with the owner's approval on 2026-11-12. Source unmodified. | HLE: "Using an external BIOS file is not necessary or possible". Its core libraries build with MSVC without Qt and link statically. **Runs SLUS-20362** from the owner's files: boot, menus, race, pause, memory-card save and load (`docs/evidence/m1-acceptance.md`). | Everything listed under "does not cover" in the evidence file (other modes, long sessions, other PCs). |
| [Play-Compatibility #400](https://github.com/jpd002/Play-Compatibility/issues/400) | — | evidence | **SLES-50731** (PAL HP2): labels `state-playable` + `issue-mipmapping`, last tested 2024-04-26 on commit `07c1898`: "Fully playable with good performance", texture flicker between mip levels on some tracks. History: loadable (2018) → ingame (2022) → playable (2024). | NTSC-U behaviour; whether the mip issue is fixed in current builds. |
| [PCSX2](https://github.com/PCSX2/pcsx2) | GPL-3.0+ (from prior knowledge; not re-fetched this session → treat as UNVERIFIED until checked at use) | **reference-only** for now (fallback path C2) | Search snippets of the PCSX2 wiki: game is flagged CPU- and GPU-intensive, a widescreen patch exists, old "Busted screen" and sun-ray bugs are listed as fixed. | The live wiki page returned HTTP 403, so the current compatibility rating is unverified. PCSX2 requires a BIOS dumped from the user's own console — content outside the game dump (C1), a user decision if ever needed. |
| [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) | GPL-3.0 | **reference-only** | "PlayStation 2 Static Recompiler (Experimental)". README limitations: "Performance is very bad for VU and GS", "Hardware emulation is partial and many paths are stubbed". Emits C++ (needs a C++20 compiler at conversion time). No tested-games list. | Any commercial game booting. Not a basis for M1; revisit when planning stage N1/N2 (ADR 0001). |
| [Arntzen-Software/parallel-gs](https://github.com/Arntzen-Software/parallel-gs) | LGPL-3.0+ ("dual-licensing may be possible") | **reference-only** for now (M2 candidate) | Vulkan compute-shader GS emulation, 2x–16x SSAA, standalone tools + experimental PCSX2 integration; "early days". | No documented embedding API; external contributions not accepted. LGPL needs dynamic linking or relinkable objects. |

## Translating the game (stage R1, ADR 0007)

| Source | License | Verdict | What was confirmed | What remains UNVERIFIED |
|---|---|---|---|---|
| [LLVM](https://llvm.org/) 23.1.5 | **Apache License 2.0 with LLVM Exceptions** (read from the fetched `llvm/LICENSE.TXT`, 2026-11-13; third-party parts listed in that file) | **reuse-ok — in use by the recompiler** (ADR 0007). Fetched with the owner's permission (2026-11-12): `llvm-project-23.1.5.src.tar.xz`, 179,220,264 bytes, SHA-256 pinned in `tools/fetch-llvm.ps1`; built by `tools/build-llvm.ps1`. Not in the repository. | Builds here with MSVC and the static C runtime (x86 target only); compiles translated game functions that match the reference core (`docs/recomp-analysis.md` §9). Linked so far only into `hp2recomp.exe`, its tests and the measurement build of the launcher. | When the converter ships the translator, its license text and the notices of the parts it bundles must go into `THIRD-PARTY-NOTICES.txt` (to do in R1b). |
| Play! CodeGen (already linked, see below) | BSD 2-Clause | **in use by the reference build only** | Its block-level code is what the measurements in `docs/recomp-analysis.md` §5 time. | — |
| Cranelift, QBE, MIR, asmjit | — | **not used, not fetched** | Named in ADR 0007 from prior knowledge only. | Everything about them. |
| The MIPS R5900 instruction encodings | public processor documentation; also visible in the reference core's sources (BSD 2-Clause) | **reference-only** | `recomp/src/r5900.cpp` was written from the published opcode tables; no code was copied. All 431,681 words of the game's code decode with it and its control-flow predictions match what the reference core executed. | The meaning of each instruction is not implemented yet. |

## Third-party software in the launcher (`HotPursuit2.exe`)

Exactly the static libraries that the link step uses (checked in the build
tree). Each license text is copied from the fetched source into
`THIRD-PARTY-NOTICES.txt`, which the converter writes next to the launcher;
the build fails if one of the license files is missing.

| Component | Where it comes from | License | License file used |
|---|---|---|---|
| Play! core (`PlayCore`, `app_shared`, `gsh_opengl`) | jpd002/Play- @ `1f27232` | BSD 2-Clause | `License.txt` |
| Play! Framework (`Framework`, `Framework_Http`, `Framework_OpenGl`) | jpd002/Play--Framework @ `587f278` | BSD 2-Clause | `deps/Framework/License.txt` |
| Play! CodeGen (JIT) | jpd002/Play--CodeGen @ `a5009f7` | BSD 2-Clause | `deps/CodeGen/License.txt` |
| libchdr | jpd002/libchdr @ `284e38b` (fork of rtissera/libchdr) | BSD 3-Clause | `deps/libchdr/LICENSE.txt` |
| LZMA SDK 24.05 (inside libchdr) | bundled in libchdr | public domain | `deps/libchdr/deps/lzma-24.05/LICENSE` |
| Zstandard | facebook/zstd @ `f8745da` | BSD (dual BSD/GPLv2; used under BSD) | `deps/Dependencies/zstd/LICENSE` |
| zlib | madler/zlib @ `5a82f71` | zlib license | `deps/Dependencies/zlib/LICENSE` |
| xxHash | Cyan4973/xxHash @ `e626a72` | BSD 2-Clause | `deps/Dependencies/xxHash/LICENSE` |
| bzip2 1.0.6 | bundled in Play-Dependencies @ `8a5f6b1` | bzip2 license (BSD-style) | `deps/Dependencies/bzip2-1.0.6/LICENSE` |
| GLEW 2.0.0 | bundled in Play-Dependencies | Modified BSD / MIT | `deps/Dependencies/glew-2.0.0/LICENSE.txt` |

All are permissive and compatible with shipping a binary under our MIT
license plus notices. Not fetched and not linked: Nuanceur (Vulkan only),
AltKit, SDWebImage (iOS), ghc_filesystem, nlohmann_json (used only by Play!'s UI code).

Other sources consulted in P4b: a web search summary of GameSpot's 2002 PS2
review (frame rate remark; the page itself returned HTTP 403, so the wording
is second-hand) and silentsblog.com's mod page for this game (no frame-rate
information; nothing taken).

## Decisions that follow

- The repo's own code is MIT (see `LICENSE`): it can be combined with BSD/LGPL/GPL components later without relicensing our files (a GPL component would make the *combined binary* GPL — to be decided in the ADR that introduces it).
- No name list, table or code from any third party is shipped. `assets-pipeline/data/hp2_slus20362_names.txt` contains only names recovered from the owner's dump by `tools/recon/zrecon.py`; every entry is checked against `ZDIR.BIN` hashes by the test-suite.
