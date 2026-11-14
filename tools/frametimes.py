#!/usr/bin/env python3
"""Frame-time statistics from the launcher's --frametimes CSV (DEV-ONLY).

The CSV has one line per event: kind ('V' = emulated vertical blank,
'P' = image presented; 'R' = GS thread has an image ready) and the
time in milliseconds.

Usage: python -B tools/frametimes.py <frametimes.csv> [--from <ms>] [--to <ms>] [--label <text>]

Reports, for vblanks and for presents separately: count, mean, median, p95,
p99, max interval, standard deviation, and how many intervals deviate from
the median by more than 25 % ("uneven"). A stutter-free 60 Hz stream has a
median of 16.7 ms and essentially no uneven intervals.
"""
import argparse
import statistics
import sys


def stats(times, label):
    if len(times) < 3:
        print(f"  {label:9} too few events ({len(times)})")
        return
    iv = [b - a for a, b in zip(times, times[1:])]
    iv_sorted = sorted(iv)
    med = statistics.median(iv)
    pct = lambda p: iv_sorted[min(len(iv_sorted) - 1, int(len(iv_sorted) * p))]
    uneven = sum(1 for x in iv if abs(x - med) > 0.25 * med)
    long_ = sum(1 for x in iv if x > 1.5 * med)
    print(f"  {label:9} n={len(iv):5}  mean={statistics.fmean(iv):6.2f}  median={med:6.2f}  "
          f"p95={pct(0.95):6.2f}  p99={pct(0.99):6.2f}  max={iv_sorted[-1]:7.2f}  "
          f"sd={statistics.pstdev(iv):5.2f} ms  uneven={uneven} ({100.0 * uneven / len(iv):.1f}%)  "
          f">1.5x median={long_}")


def histogram(times, label, width=1.0, top=12):
    iv = [b - a for a, b in zip(times, times[1:])]
    buckets = {}
    for x in iv:
        k = round(x / width) * width
        buckets[k] = buckets.get(k, 0) + 1
    common = sorted(buckets.items(), key=lambda kv: -kv[1])[:top]
    print(f"  {label} most common intervals (ms:count): " +
          "  ".join(f"{k:.0f}:{n}" for k, n in sorted(common)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--from", dest="t0", type=float, default=0.0)
    ap.add_argument("--to", dest="t1", type=float, default=float("inf"))
    ap.add_argument("--label", default="")
    a = ap.parse_args()
    v, p = [], []
    with open(a.csv) as f:
        next(f)
        for line in f:
            kind, ms = line.strip().split(",")
            t = float(ms)
            if a.t0 <= t <= a.t1:
                if kind == "V":
                    v.append(t)
                elif kind == "P":  # R: GS-thread mark, not a presentation
                    p.append(t)
    span = f"{a.t0 / 1000:.0f}-{(a.t1 if a.t1 != float('inf') else (v[-1] if v else 0)) / 1000:.0f} s"
    print(f"{a.label or a.csv} [{span}]")
    stats(v, "vblank")
    stats(p, "present")
    histogram(p, "present")


if __name__ == "__main__":
    sys.exit(main())
