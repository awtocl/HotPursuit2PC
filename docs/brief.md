# Project Brief — NFS Hot Pursuit 2 (PS2 NTSC) → Native PC Port

**Addressee:** Claude Fable 5.2 (run in Extra mode)
**Repository:** https://github.com/awtocl/HotPursuit2PC (currently empty — placeholder `README.md` only; you initialize this project)
**Code name:** HotPursuit2PC
**Target platform:** Windows 10/11 x64
**Brief version:** v3 (2026-11-11)

**How to run this brief:** Work autonomously, phase by phase (§8). Inside a phase, make your own calls. Stop and produce a Gate Report (§10) only at a milestone gate, a hard blocker, or a decision the user must make. "Should I continue?" is not a hard blocker.

## 1. Mission
Using the user's legally dumped copy of **Need for Speed: Hot Pursuit 2 (PlayStation 2, NTSC-U)** as the *sole* source of game content, build a converter tool and pipeline that turns that dump into a **native Windows build** of the game — or, if a full native re-implementation is not yet achievable, as close to native as engineering honestly allows — playable directly on PC.

The official 2003 PC release is **not** an acceptable substitute. It is a separate port with different handling, tuning, UI flow, camera behavior, and feature set; it does not faithfully reproduce the PS2 experience. The user wants to play *the PS2 game*, in the best possible way, on modern hardware.

- **Milestone 1:** get the game running and stable on PC.
- **Milestone 2:** modern features — DLAA first, then QoL.
- Overall Definition of Done: §12.

## 2. Hard Constraints (self-check against these IDs)
| ID | Constraint |
|---|---|
| C1 | The PS2 NTSC dump is the sole source of game content. No assets, code, or data may be pulled from the PC release, any other game, or the web. |
| C2 | The user's ROM folder (`ROM/Need for Speed - Hot Pursuit 2 (USA)/`) is read-only. Never write into, modify, or "clean up" it. All derived artifacts go into the repo. |
| C3 | No destructive git operations (`reset --hard`, force-push, history rewrites). Small atomic commits with messages stating *what* and *why*. Never push without asking. |
| C4 | Evidence over assertion: every claim about the dump's internals must be backed by a byte-level inspection, a test, or a cited OSS reference, recorded in `docs/dump-analysis.md`. Label anything unconfirmed as UNVERIFIED. |
| C5 | Third-party code/assets require a license check first; record the verdict in `docs/prior-art.md`. Default to reference-only when a license is unclear. |
| C6 | Portable layout: the converter creates (or reuses) a single game folder named `game` **next to the converter's own `.exe`**; every file it produces or consumes (ROM input, extracted assets, output build, logs, settings) is organized inside that folder. The user picks `game` or a custom name at first run. *Portable* = everything lives under `game/` (self-contained, relocatable); *installed* = the same layout, but the folder is the user-chosen install directory (default still `<exe dir>/game`). |
| C7 | 60 fps is the baseline frame timing (NTSC-U). Any mode that changes frame rate (e.g., uncapped) must be opt-in, default-off, and clearly labeled. |
| C8 | Ambiguity rule: if two readings of this brief conflict, prefer the reading that keeps Milestone 1 honest (a running game) over feature richness; record the choice as an ADR. |

## 3. The Converter: UX Contract
The deliverable is a small Windows program (the "converter"), not a game mod. Contract:

1. **Input:** the user selects (or confirms) their ROM. The ROM is the *extracted* contents of the PS2 disc — a flat directory (see §4), not necessarily an `.iso` file. The converter must validate the layout against §4 before proceeding, and fail with a clear, actionable error if it is missing or damaged.
2. **Organization:** the converter creates a dedicated game folder next to its own `.exe` (default name `game`; the user may rename it at first run). **All files go inside that folder**: the selected ROM contents (or a manifest pointing at them), the extracted/converted assets, the output build, logs, and settings. Nothing is scattered next to the `.exe`.
3. **Conversion:** the converter performs the conversion (asset extraction, format translation, packaging, launch stub) into that folder. Progress must be visible; failures must say *which* file/stage failed and *why*.
4. **Output:** the user ends with a native copy of their game inside the `game` folder — an executable (or launcher) that runs the game on PC without the converter. The converter is a build tool; the result must stand on its own.
5. **Modes:** user chooses **portable** or **installed** (C6):
   - *Portable:* everything (ROM input, assets, build) lives inside `game/` next to the `.exe`; the folder is self-contained and relocatable.
   - *Installed:* the converter installs into a user-chosen directory (default `<exe dir>/game`), keeping the same internal layout.
6. **Idempotency:** re-running the converter on an existing `game` folder must detect what is already done, offer to resume, and never corrupt or silently overwrite previous work.
7. **GUI vs CLI:** GUI-first UX (selections, progress, mode choice) but the same operations must be exposed as a CLI for automation and headless use. (Implementation per §6 ADRs.)

## 4. Verified Source Facts (byte-inspected 2026-11-11 — ground truth)
The user's NTSC-U dump is a flat extraction at `ROM/Need for Speed - Hot Pursuit 2 (USA)/` (25 files; not an `.iso`, not an unpacked asset tree). Verified by direct inspection:

- **Disc:** `SLUS-2036`, NTSC-U. `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_203.62;1`, `VMODE = NTSC`, `VER = 1.00`.
- **`SLUS_203.62`** — 2,321,608 bytes. The main game **ELF** (PS2 MIPS32 little-endian ELF; magic `7F 45 4C 46 01 01 01`). This is the game's boot executable.
- **Game data is a PS2 "Z" fileset** under `NFSHP2/`:
  - `ZZDATA.BIN` — 3,116,437,504 bytes (~3.0 GB). A single blob holding essentially **all** game content.
  - `ZDIR.BIN` — 4,860 bytes. The directory table for `ZZDATA.BIN`. **Entries store hashed filenames — no plaintext names** (hexdump confirms packed records). Do not assume you can read a file list from it directly; you need a proper ZDIR parser plus a hash→name list.
- **`NFSHP2/IRX243/` and `NFSHP2/IRX253/`** — two IOP module sets (`IOPRP243.IMG` / `IOPRP253.IMG` plus PADMAN, STREAM, USBD, SIO2MAN, LGDEV, LIBSD, MCMAN, MCSERV, MTAPMAN, SDRDRV). Two IOP load phases/configs. Relevant to an emulator path; not needed for a native-asset path.
- **Implication:** the first asset-pipeline stage is **unpacking the Z fileset** (`ZDIR.BIN` + `ZZDATA.BIN`) into individual, correctly-named assets. That is a concrete, testable stage — give it its own ADR and tests, and produce a full asset inventory (name, type, size) in `docs/dump-analysis.md`.

### 4a. OSS References (verify before relying on them — C5)
- **Ekey/NFS.BIN.Tool** (https://github.com/Ekey/NFS.BIN.Tool, C#) — extracts/repacks ZZDATA archives for NFS console games. For **HP2 (PS2)** use the old-format path: `NFS.Unpacker -true <ZDIR.BIN> <out_dir>`; `NFS.Packer <in_dir> <out_dir>` repacks. It resolves hashed filenames via an internal name-hash list; its README example path (`E:\Games\SLUS-20362\NFSHP2\ZDIR.BIN`) matches this dump's layout exactly. **No license file at repo root → treat as reference-only** unless the user says otherwise.
- Community threads on HP2's hashed ZDIR/ZZDATA: ResHax #18787 (hashed filenames), ZenHAX t=1614 (ZZDATA chunk structure; an aluigi script), r/REGames 1b22u6q. Filenames are hashed — recover them via a known hash→name mapping, not by reading `ZDIR.BIN` as text.
- Record what you confirmed vs. what remained UNVERIFIED in `docs/prior-art.md`.

## 5. "Native" — Definition and Honest Scoping
**Native = the game runs as a native Windows application**: its own process, native rendering, native input, native audio — not a PS2 emulator process pretending to be a game, and not a Windows build of the 2003 PC version.

Honest scoping (decide this early — ADR 0001 — and be transparent about it):
- **Path A (native re-implementation):** rebuild the game engine in native code; port assets 1:1. Highest fidelity ceiling, largest effort.
- **Path B (asset-native hybrid):** extract the assets natively and run them on a native engine you build or license (e.g., a clean-room renderer driven by the game's data). The game *logic* is where fidelity risk lives — budget for it explicitly.
- **Path C (emulator-backed):** run the PS2 ELF (`SLUS_203.62`) under an open PS2 emulator core with the dumped content. *This is not "native"* — a legitimate stopgap and a useful reference, but it must be labeled as such and is not the end goal.

Milestone 1 may legitimately land on Path C with a working game if A/B are not yet stable — but the repo, docs, and ADRs must make the path choice explicit, and the milestone gate report must state exactly which path shipped and what that means for fidelity.

## 6. Technical Decisions — Each Must Be an ADR
Do not pick a stack by vibes. For each decision below, write a numbered ADR (`docs/adr/NNNN-<title>.md`) with: context, options considered (≥2), decision, consequences, confidence, and what would reopen it.

1. **0001 Architecture** — Path A/B/C decision, scored against: fidelity, effort, license risk, and Milestone 1 timing.
2. **0002 Language stack** — the "best language for the job" is yours to pick, per component if needed (converter/GUI, asset pipeline, engine, launcher). Justify per component: tooling, performance, licensing, solo-maintainability.
3. **0003 GUI framework** — for the converter's UI (must support file pickers, progress, mode choice; CLI parity per §3).
4. **0004 Asset pipeline** — how `ZZDATA.BIN` is unpacked, validated, and stored; where Ekey's format knowledge is reused (reference-only, C5).
5. **0005 Rendering & audio** — only after 0001; pick the path that matches the architecture decision.

## 7. Milestones
### Milestone 1 — Running & Stable
Definition: a user, on a clean Windows machine, can point the converter at their ROM and end up with a playable, stable build of the PS2 NTSC game inside the `game` folder.
Acceptance (all must hold):
- Converter validates the ROM layout (§4) and fails cleanly on bad input.
- Conversion completes end-to-end with visible progress; no silent corruption.
- The output build launches and is playable from menu to gameplay; a representative session (boot, a race, pause/save, quit) is stable with no crash loop.
- 60 fps baseline preserved (C7).
- `docs/dump-analysis.md` contains the full asset inventory and the Z fileset unpack evidence (C4).
- Gate: produce a Gate Report (§10) and stop for the user to try it.

### Milestone 2 — Modern Features
- **DLAA first** (dynamic resolution/anti-aliasing or equivalent upscaling) — opt-in, default-off, ADR'd.
- Then QoL in order of user impact: resolution/aspect handling, save-state robustness, config file, controller improvements, frame-rate modes (uncapped, opt-in per C7).
- Each feature is ADR'd, tested, and documented in the repo README.

## 8. Execution Protocol (read before starting)
Work in bounded phases. Each phase must be small enough to finish in one working session, and **every phase ends with**: (a) an atomic commit, and (b) an updated `docs/STATUS.md` — the project's memory. A fresh session must be able to resume from `docs/STATUS.md` alone: what is done (with evidence), what is next, open questions, current ADR list.

- **P0 Recon** — read this brief; verify §4 facts against the actual dump (C2 read-only); survey OSS (§4a); write `docs/STATUS.md`, ADRs 0001–0002, `docs/prior-art.md`.
- **P1 Scaffold** — repo structure, `.gitignore`, `LICENSE`, `CONTRIBUTING.md`, CI (build + test), README skeleton, `docs/` layout.
- **P2 Unpack & Inventory** — implement the Z fileset unpack (ADR 0004); produce the full asset inventory in `docs/dump-analysis.md`; tests against the user's actual dump.
- **P3 Converter v0** — converter per §3: CLI validation, `game` folder creation, output-mode selection; GUI stub per ADR 0003.
- **P4 M1 Build** — implement Milestone 1 per §7 (per the chosen architecture path).
- **P5 M1 Evidence** — acceptance runs on a clean machine; record evidence; write the Gate Report; stop.
- **M2** — after the user approves M1: DLAA, then QoL, each ADR'd and gate-reported.

Rules:
- Never skip or merge phases to save time; if a phase overruns, split it and note it in `docs/STATUS.md`.
- Commit message format per phase: `P<#> <phase>: <what> — <why>`.
- When blocked: record the blocker in `docs/STATUS.md`, produce a Gate Report, stop.

## 9. Repository & Engineering Practices
- Structure: `converter/`, `assets-pipeline/`, `engine/` (if Path A/B), `docs/` (ADRs, STATUS, dump-analysis, prior-art). `game/` is user data — never committed.
- Tests: asset-pipeline tests run against fixtures derived from the user's dump (hashes/sizes, not full blobs). Engine/converter tests per component.
- CI: build + tests on Windows; fail loudly on path or permission errors.
- `docs/STATUS.md` is the single source of truth for project state — update it in every phase commit.

## 10. Gate Report Format (every milestone gate and hard blocker)
1. **What changed / why** — phases completed this gate, key decisions.
2. **Verified** — with evidence paths (tests, logs, byte-level checks).
3. **Not verified** — explicit list, including anything UNVERIFIED.
4. **File locations** — where the user should look.
5. **What the user needs to do** — concrete next action (e.g., "run the converter, try a race").
6. **Next phase plan** — what happens after approval.

## 11. First Deliverables (in order)
1. `docs/STATUS.md` (initialized) + ADRs 0001, 0002 + `docs/prior-art.md` (P0).
2. Repo scaffold + CI (P1).
3. Z fileset unpacker + asset inventory in `docs/dump-analysis.md` (P2) — **the first code deliverable**.
4. Converter v0 per §3 (P3).
5. Milestone 1 (P4/P5) + Gate Report.

## 12. Definition of Done (overall)
- The repo builds from a clean clone with CI green.
- A user following the README can convert their ROM and play the game (M1) — evidence recorded.
- M2 features shipped are ADR'd, tested, default-safe, and documented.
- `docs/STATUS.md`, ADRs, dump-analysis, and prior-art are current and consistent with the code.
- No constraint (§2) violated; no UNVERIFIED claim left unlabeled.
