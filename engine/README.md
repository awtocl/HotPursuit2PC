# engine

What runs the game. Per ADR 0001 this is staged:

| Stage | What runs the game code | Label |
|---|---|---|
| M1 | embedded open-source PS2 core with HLE BIOS, hosted by our launcher | **emulator-backed (Path C)** — not native |
| N1 | game code translated ahead-of-time at conversion | hybrid |
| N2 | translated code on our own runtime (GS, audio, input, kernel services) | native (Path A′) |

Nothing is implemented yet (phase P4). ADR 0005 (rendering & audio) will be
written when this directory gets its first code.
