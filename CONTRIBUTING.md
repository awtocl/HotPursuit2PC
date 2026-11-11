# Contributing

Read `docs/STATUS.md` first — it says what is done, what is next and which
questions are open. The project brief is `docs/brief.md`.

## Hard rules (from the brief, §2)

| ID | Rule |
|---|---|
| C1 | The user's PS2 NTSC-U dump is the only source of game content. Nothing from the PC release, other games or the web. |
| C2 | The ROM folder is read-only. Derived artifacts go into the repo (code, hashes, sizes) or into the user's `game/` folder (everything else). |
| C3 | No destructive git (`reset --hard`, force-push, history rewrites). Small atomic commits that say *what* and *why*. |
| C4 | Evidence over assertion. Claims about the dump are backed by a byte-level check, a test or a cited source, recorded in `docs/dump-analysis.md`. Unproven = **UNVERIFIED**. |
| C5 | Third-party code/assets need a license verdict in `docs/prior-art.md` *before* use. Unclear license = reference-only. |
| C6 | Portable layout: everything the converter produces lives in one `game/` folder next to its `.exe`. |
| C7 | 60 fps is the baseline. Anything that changes frame timing is opt-in and off by default. |
| C8 | When two readings conflict, prefer the one that keeps Milestone 1 honest; record it as an ADR. |

**Never commit game data.** No files from the disc, no extracted assets, no
strings tables, no screenshots of copyrighted art. Names, hashes and sizes of
files are fine. CI rejects tracked files that look like game data.

## Workflow

- Work in phases (brief §8). Every phase ends with one commit
  `P<#> <phase>: <what> — <why>` and an updated `docs/STATUS.md`.
- Decisions with alternatives are ADRs: `docs/adr/NNNN-<title>.md` with context,
  ≥ 2 options, decision, consequences, confidence, and what would reopen it.
- A hash match is not proof by itself (2³² is small): see the tier rules in
  `docs/dump-analysis.md` §4 before adding a file name.

## Build and test

Requirements: Windows 10/11 x64, Visual Studio 2022 (or Build Tools) with the
C++ workload. CMake and Ninja ship with it. No other dependencies.

```powershell
pwsh tools/build.ps1                  # configure + build + test (preset "dev")
pwsh tools/build.ps1 -Preset release
```

Outputs land in `.cache/build/<preset>/` (git-ignored).

Tests come in two kinds:

- **Always run** (also in CI): synthetic fixtures built by the tests themselves, plus hashes/sizes in `tests/fixtures/`.
- **Dump tests**: run against a real dump when one is found at
  `ROM/Need for Speed - Hot Pursuit 2 (USA)/` or in the `HP2_ROM_DIR`
  environment variable; otherwise they report *skipped*. They open the dump read-only.

## Code style

C++20, MSVC `/W4 /permissive-`, warnings are errors in CI. All file parsing
goes through bounds-checked readers; never trust an offset or size read from
a file. Errors carry the file and stage that failed and why (brief §3.3).
