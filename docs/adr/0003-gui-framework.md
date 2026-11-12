# ADR 0003 — GUI framework for the converter

- **Status:** Accepted (2026-11-12, phase P3)
- **Constraints in play:** C6 (single portable `.exe`, nothing scattered next to it), brief §3.7 (GUI-first, CLI parity), clean-machine acceptance (§7)

## Context

The converter's UI is small: pick the ROM folder, pick portable/installed and
the folder name or install directory, start, watch progress, read a clear
error. It must run on a clean Windows 10/11 x64 machine without installing
anything, and every operation must also exist as a CLI. Per ADR 0002 the
converter is C++ with no package manager.

## Options considered

| Option | Dependencies shipped | Size | Native look / accessibility / DPI | Verdict |
|---|---|---|---|---|
| **Plain Win32 + Common Controls 6** | none (in Windows) | ≈ 0 extra | native controls, screen-reader friendly, per-monitor DPI via manifest | **chosen** |
| Dear ImGui + Direct3D 11 | vendored source (MIT) | ≈ 1 MB | custom-drawn, no accessibility, needs a GPU context for a file picker and a progress bar | rejected — wrong tool for a form |
| Qt 6 Widgets | 20–40 MB of DLLs, LGPL obligations | large | very good | rejected — size and licensing for a two-control wizard |
| WebView2 (HTML UI) | Evergreen runtime (present on Win 11, usually on Win 10) | small | good | rejected — runtime not guaranteed on a clean Windows 10; second language |
| WinUI 3 / WPF / WinForms | .NET or Windows App SDK runtime | 70–150 MB self-contained | very good | rejected — no .NET SDK here; runtime dependency |

## Decision

1. **Win32 with Common Controls 6**, one top-level window built in code (no
   resource-editor dialogs), application manifest for visual styles,
   per-monitor-v2 DPI awareness and long paths. Folder selection uses the
   shell's `IFileOpenDialog`.
2. **CLI parity by construction:** all behaviour lives in the `hp2core`
   library (`check_rom`, `inspect_game_folder`, `convert`). Two thin
   front-ends link it:
   - `HotPursuit2PC.exe` — GUI (Windows subsystem);
   - `hp2pc-cli.exe` — console, for automation and headless use.
   The GUI calls `convert()` on a worker thread and receives the same log
   lines and progress events the CLI prints. Neither front-end contains
   conversion logic; a feature is not done until both expose it.
3. The GUI has a `--selftest` switch (creates the window, checks its controls,
   exits) so CI can exercise it without a desktop session being driven by hand.

## Consequences

- Zero added dependencies or runtime; the GUI `.exe` stays around 1 MB.
- Layout is manual; acceptable for one form, would not scale to a rich UI.
- If the launcher (engine) later needs in-game settings screens, that is a
  different problem (rendered UI) and gets its own decision in ADR 0005.

## Confidence

High for the converter's needs as specified in brief §3.

## What would reopen this ADR

- The converter UI growing beyond a single form (asset browser, mod manager).
- A requirement for localisation with complex layout, or theming.
