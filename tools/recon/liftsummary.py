#!/usr/bin/env python3
"""Summary of a --lift-check run (translator spike, ADR 0007).

  python -B tools/recon/liftsummary.py <lift-check.tsv> <blocks.tsv with counts> <hp2recomp functions TSV>

Weights each function's median call time (reference core vs translated code)
by how often the counting run entered it, and prints the totals.
"""
import csv
import sys


def main():
    check, blocks, functions = sys.argv[1:4]
    rows = list(csv.DictReader(open(check), delimiter="\t"))
    starts = {}
    for r in csv.DictReader(open(functions), delimiter="\t"):
        starts[int(r["start"], 16)] = int(r["end"], 16)
    entries, instructions = {}, {}
    total_instructions = 0
    ordered = sorted(starts)
    import bisect
    for line in open(blocks):
        if line.startswith("#"):
            continue
        b, e, _frame, count = line.split()
        b, e, count = int(b, 16), int(e, 16), int(count)
        n = count * ((e - b) // 4 + 1)
        total_instructions += n
        i = bisect.bisect_right(ordered, b) - 1
        if i < 0 or b >= starts[ordered[i]]:
            continue
        f = ordered[i]
        instructions[f] = instructions.get(f, 0) + n
        if b == f:
            entries[f] = entries.get(f, 0) + count
    verified = [r for r in rows if int(r["verified"]) > 0]
    trapped = [r for r in rows if int(r["verified"]) == 0]
    mism = sum(int(r["state_mismatch"]) + int(r["memory_mismatch"]) for r in rows)
    calls = sum(int(r["verified"]) for r in rows)
    ref_total = lifted_total = 0.0
    share = 0
    for r in verified:
        f = int(r["function"], 16)
        n = entries.get(f, 0)
        ref_total += n * float(r["reference_median_ns"])
        lifted_total += n * float(r["translated_median_ns"])
        share += instructions.get(f, 0)
    trap_share = sum(instructions.get(int(r["function"], 16), 0) for r in trapped)
    print(f"functions translated: {len(rows)}; compared with the reference: {len(verified)} "
          f"({calls} calls); always left through the trap (memory outside main RAM): {len(trapped)}")
    print(f"mismatches (processor state or memory): {mism}")
    print(f"compared functions execute {100.0 * share / total_instructions:.1f}% of the game's instructions; "
          f"trapped ones {100.0 * trap_share / total_instructions:.1f}%")
    print(f"time in the compared functions over the counting session, median call time x calls:")
    print(f"  reference core: {ref_total / 1e6:.0f} ms   translated: {lifted_total / 1e6:.0f} ms   "
          f"ratio {ref_total / lifted_total if lifted_total else 0:.1f}x")
    print("largest contributors (reference ms, translated ms, calls):")
    top = sorted(verified, key=lambda r: -entries.get(int(r["function"], 16), 0) * float(r["reference_median_ns"]))
    for r in top[:12]:
        f = int(r["function"], 16)
        n = entries.get(f, 0)
        print(f"  {r['function']}  {n * float(r['reference_median_ns']) / 1e6:8.1f} {n * float(r['translated_median_ns']) / 1e6:8.1f}  {n:>9}")


if __name__ == "__main__":
    main()
