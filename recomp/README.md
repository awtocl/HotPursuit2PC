# recomp

Static recompilation of the game program (stage R1 of
[ADR 0006](../docs/adr/0006-recompilation-main-line.md)): the converter will
translate the game's PS2 machine code into x64 code on the user's machine, ahead of
time, so that nothing is compiled while playing.

What exists so far is the analysis half. Evidence and numbers:
[docs/recomp-analysis.md](../docs/recomp-analysis.md).

| Part | What it does |
|---|---|
| `include/hp2/elf.h` | reads the game's executable (32-bit MIPS ELF): where each part is loaded, word access by address |
| `include/hp2/r5900.h` | decodes every instruction of the PS2's main processor (MIPS III + multimedia + FPU + VU0 macro), with control-flow classification and a disassembler |
| `include/hp2/discover.h` | function discovery without symbols: function starts, switch tables, every place where a straight run of code can begin |
| `tool/main.cpp` | `hp2recomp.exe`: `info`, `functions`, `disasm`, and `check-trace` (compares the analysis with what the reference core really executed) |
| `tests/` | synthetic tests (hand-assembled program, no game data) and tests on the owner's executable (skipped without a dump) |

Not here yet: the translator itself. The code generator is the subject of
[ADR 0007](../docs/adr/0007-code-generator.md).

## Checking the analysis against the reference build

```powershell
pwsh tools/build.ps1 -Preset profile -NoTest      # measurement build of the reference launcher
pwsh tools/run-launcher.ps1 -RunName trace -Preset profile -Rom ".cache\m1-run\game\rom" -Frames 4600 -Mute `
     -Size 1280x896 -Res 2 -Trace `
     -Script "800:start,1100:start,1400:start,1700:cross,2000:cross,2300:cross,2600:cross,2900:cross:1600"
.cache\build\dev\recomp\hp2recomp.exe check-trace "ROM\Need for Speed - Hot Pursuit 2 (USA)\SLUS_203.62" .cache\trace\trace
python -B tools/profile.py .cache\trace\trace\profile.csv --from-frame 3120 --to-frame 4500 --label race
```

`check-trace` exits with 0 only when every instruction the reference core
executed decodes, every block it executed begins where the analysis predicted
a straight run can begin, and none of them begins in code the analysis could
not explain.

The trace folder holds data derived from the game (a copy of its memory,
lists of its code addresses). It stays in `.cache/`, which is git-ignored.
