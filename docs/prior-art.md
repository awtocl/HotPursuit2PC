# Prior art and third-party license verdicts (C5)

Checked 2026-11-11 (phase P0). Verdicts:

- **reuse-ok** — license permits use in this repo; still needs an ADR/STATUS note when actually pulled in.
- **reference-only** — may be read for format/behaviour knowledge; no code or data copied.
- **not used** — looked at, nothing taken.

Nothing third-party is in the tree yet.

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
| [jpd002/Play-](https://github.com/jpd002/Play-) | **BSD 2-Clause** (`License.txt`, © 2006-2026 Jean-Philip Desjardins) | **reuse-ok** (candidate M1 core, ADR 0001 path C1) | HLE: "Using an external BIOS file is not necessary or possible". Boots disc images and ELFs. CMake + Visual Studio build; Qt needed only for its own UI; a `build_retro` (libretro core) target exists. | Compatibility with **SLUS-20362** specifically. Exact build dependencies of the headless/libretro target. Supported image formats beyond what the README lists. |
| [Play-Compatibility #400](https://github.com/jpd002/Play-Compatibility/issues/400) | — | evidence | **SLES-50731** (PAL HP2): labels `state-playable` + `issue-mipmapping`, last tested 2024-04-26 on commit `07c1898`: "Fully playable with good performance", texture flicker between mip levels on some tracks. History: loadable (2018) → ingame (2022) → playable (2024). | NTSC-U behaviour; whether the mip issue is fixed in current builds. |
| [PCSX2](https://github.com/PCSX2/pcsx2) | GPL-3.0+ (from prior knowledge; not re-fetched this session → treat as UNVERIFIED until checked at use) | **reference-only** for now (fallback path C2) | Search snippets of the PCSX2 wiki: game is flagged CPU- and GPU-intensive, a widescreen patch exists, old "Busted screen" and sun-ray bugs are listed as fixed. | The live wiki page returned HTTP 403, so the current compatibility rating is unverified. PCSX2 requires a BIOS dumped from the user's own console — content outside the game dump (C1), a user decision if ever needed. |
| [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) | GPL-3.0 | **reference-only** | "PlayStation 2 Static Recompiler (Experimental)". README limitations: "Performance is very bad for VU and GS", "Hardware emulation is partial and many paths are stubbed". Emits C++ (needs a C++20 compiler at conversion time). No tested-games list. | Any commercial game booting. Not a basis for M1; revisit when planning stage N1/N2 (ADR 0001). |
| [Arntzen-Software/parallel-gs](https://github.com/Arntzen-Software/parallel-gs) | LGPL-3.0+ ("dual-licensing may be possible") | **reference-only** for now (M2 candidate) | Vulkan compute-shader GS emulation, 2x–16x SSAA, standalone tools + experimental PCSX2 integration; "early days". | No documented embedding API; external contributions not accepted. LGPL needs dynamic linking or relinkable objects. |

## Decisions that follow

- The repo's own code is MIT (see `LICENSE`): it can be combined with BSD/LGPL/GPL components later without relicensing our files (a GPL component would make the *combined binary* GPL — to be decided in the ADR that introduces it).
- No name list, table or code from any third party is shipped. `assets-pipeline/data/hp2_slus20362_names.txt` contains only names recovered from the owner's dump by `tools/recon/zrecon.py`; every entry is checked against `ZDIR.BIN` hashes by the test-suite.
