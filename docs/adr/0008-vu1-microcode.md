# ADR 0008 — Translating the VU1 microprograms

- **Status:** Accepted (2026-11-13), as the plan for the rest of R1; the translator itself is not written yet.
- **Constraints in play:** C1, C4, C7
- **Evidence:** `docs/recomp-analysis.md` §12, `hp2recomp vu1`, `.cache/r1-vucounts`

## Context

After the game program and VU0 were translated (ADR 0007, `docs/recomp-analysis.md` §10-11), a busy race refresh
costs about 12 ms on the VM thread, of which **VU1 microcode is 4.8 ms**, VIF1 + GIF 2.9 ms and the game program
3.6 ms. VU1 is the vector unit that transforms and lights geometry and sends it to the graphics chip (`xgkick`).

Measured (counting run, same session as before; `hp2recomp vu1`):

| Fact | Value |
|---|---|
| Microcode in the executable | 8 upload packets (VIF `MPG`) in `.vutext`, 1,563 instruction pairs, all decode; 9 end bits |
| Executed in the 82 s session | 3.88 billion instruction pairs in 125 distinct blocks (≈ 47 million per second); 9 blocks make up half |
| Cost in the reference core | ≈ 3 ns per pair (≈ 4.8 ms per busy refresh) |
| Upper slot (FMAC) | **54 % `nop`**; then `vmaddbc` 8.2 %, `vmaddabc` 7.5 %, `vmulabc` 4.7 %, `vmula` 2.9 %, `vclip` 2.4 % ... |
| Lower slot | **35 % `nop`** (encoded as an empty `move`); then `sqi` 8.7 %, `lqi` 7.9 %, `lq` 4.9 %, integer arithmetic, branches 8.2 % |
| Flag reads in all the microcode | clip flags only (`fcget` 9, `fcand` 4, `fcset` 3); **no MAC or status flag read (`fm*`, `fs*`) anywhere** |
| Other units | Q by `div` (29) and `waitq` (3); R by `rnext`/`rinit`/`rget`; **no EFU / P register**; `xgkick` 19; `xtop`/`xitop`; subroutines `bal` 15 / `jalr` 17 |

The reference core reproduces the VU's pipeline: static FMAC stall delays per block, register writes that land in
the next block, MAC/sticky/clip flag pipelines with real latencies, Q and P pipelines, a special rule for integer
registers read by a branch right after being written, the order of `xgkick`, and an end bit in a branch delay slot.
Most of its per-pair cost is this bookkeeping and the padding `nop`s, not the arithmetic.

## Options considered

| Option | What it would give | Verdict |
|---|---|---|
| A. Translate the VU1 microprograms with LLVM, like the game program, keeping only what this game's microcode can observe | No work for padding `nop`s; FMAC operations as SIMD instructions; no MAC/sticky/status flag bookkeeping (never read); only the clip flags, Q and R with their exact timing | **chosen** |
| B. Use the reference core's own VU front end and replace only its back end by LLVM | Exact by construction | rejected as the main line: keeps all the bookkeeping the game never observes; per-block code, little room for LLVM |
| C. Run the reference core's VU1 on a second thread (as some emulators do) | Takes VU1 off the critical thread without translating it | **later, combined with A** (translated VU1 on its own core); needs changes to how the core schedules VIF1/VU1 - its own ADR |

## Decision

1. **Translate each microprogram** (from its entry address to the end bit, with its subroutines) to a host function,
   at conversion time, from the microcode in the executable (all of it is there: `docs/recomp-analysis.md` §6). What
   is uploaded at run time is compared with what was translated; anything else runs on the reference core.
2. **Semantics:** the FMAC arithmetic, clamping and special cases of the reference core (as for VU0); the
   **timing that the game can observe** reproduced exactly: the static stall delays and pipeline time per pair, the
   clip-flag latency of 4, Q's latency (`div` 7, `sqrt` 7, `rsqrt` 13) and `waitq`, the integer-branch rule, the end
   bit's delay slot, and `xgkick` at the same point relative to the other instructions (it hands a GS packet to the
   reference core's GIF).
3. **Not maintained:** MAC, sticky and status flags and the P pipeline, because no microprogram of this game reads
   them (checked over all 1,563 pairs, not only the executed ones). The translator refuses a microprogram that uses
   `fm*`, `fs*`, `fsset` or an EFU instruction, so that this can never silently change.
4. **Verification** as for the game program: at every start of a translated microprogram, run the translated code on
   copies of the VU1 state and data memory, capture the packets it would send to the GIF, let the reference core run
   the same microprogram, and compare at the end bit: VF and VI registers, accumulator, Q, I, R, clip flags, data
   memory and the GIF packets. MAC/sticky/status and their pipelines are excluded from the comparison (point 3).
5. Afterwards, option C: run the translated VU1 on its own thread, so that VU1 work leaves the critical thread
   entirely; that changes how the reference core schedules VIF1 and gets its own ADR.

## Consequences

- Expected (not measured): VU1 well below half of its current 4.8 ms per busy refresh. The VIF1 and GIF shares
  (2.9 ms) remain until R2/R3.
- A large piece of work: about 70 lower instructions (most simple), the pipeline-time model, subroutines and the
  verification harness.

## What would reopen this ADR

- A microprogram of another mode or track that reads MAC/status flags or uses the EFU (point 3 makes the
  translator refuse it; then those flags are added).
- Verification showing behaviour that depends on timing beyond what point 2 reproduces.
