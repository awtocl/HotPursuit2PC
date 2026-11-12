#!/usr/bin/env python3
"""Where the time of each emulated refresh goes in the reference build.

Reads the profile.csv written by the launcher's --trace switch (measurement
build: `pwsh tools/build.ps1 -Preset profile`, then `tools/run-launcher.ps1
-Preset profile -Trace`). One row per emulated refresh:

  frame, t_ms        refresh number, and when its work ended
  vm_cpu_ms          CPU time the VM thread used for it
  gs_cpu_ms          CPU time the graphics thread used since the previous row
  wait_ms            time the launcher's pacer then waited
  EE, VU, VIF1, GIF, IOP, SPU, GSSYNC, OTHER ...   the core's own profiler zones (ms)

"Work" below is the sum of all zones except OTHER (OTHER contains the pacer's
wait). GSSYNC is the VM thread waiting for the graphics thread.

  python -B tools/profile.py .cache/r1-trace2/trace/profile.csv --from-frame 3120 --to-frame 4500 --label race
"""
import argparse
import csv
import statistics as st


def pct(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * q))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--from-frame", type=int, default=0)
    ap.add_argument("--to-frame", type=int, default=1 << 30)
    ap.add_argument("--busy-ms", type=float, default=8.0, help="a refresh with at least this much work counts as busy")
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    with open(args.csv, newline="") as f:
        rows = [r for r in csv.DictReader(f) if args.from_frame <= int(r["frame"]) < args.to_frame]
    if len(rows) < 3:
        raise SystemExit("not enough rows in that range")
    fixed = ("frame", "t_ms", "vm_cpu_ms", "gs_cpu_ms", "wait_ms")
    zones = [c for c in rows[0] if c not in fixed and c != "OTHER"]
    if not zones:
        raise SystemExit("no profiler zones in this file: it was not recorded with the 'profile' build")

    def val(r, c):
        return float(r[c])

    def work(r):
        return sum(val(r, z) for z in zones)

    w = [work(r) for r in rows]
    t = [val(r, "t_ms") for r in rows]
    ends = [b - a for a, b in zip(t, t[1:])]
    # Time from one refresh start to the next = work + wait of the same row, roughly; what matters for
    # pacing is whether the VM advances evenly. Two refreshes "at once" then a long stall is the burst pattern.
    starts = [t[i] + val(rows[i], "wait_ms") for i in range(len(rows))]
    periods = [b - a for a, b in zip(starts, starts[1:])]
    title = f"{args.label or args.csv}: frames {rows[0]['frame']}..{rows[-1]['frame']} ({len(rows)} refreshes)"
    print(title)
    print(f"  work per refresh: mean {st.mean(w):.2f}  median {st.median(w):.2f}  p95 {pct(w, .95):.2f}  "
          f"p99 {pct(w, .99):.2f}  max {max(w):.1f} ms;  over 16.7 ms: {sum(1 for x in w if x > 16.67)}")
    print(f"  refresh period (after pacing): median {st.median(periods):.2f}  p95 {pct(periods, .95):.2f}  "
          f"max {max(periods):.1f} ms;  shorter than 8 ms: {sum(1 for x in periods if x < 8)}  "
          f"longer than 25 ms: {sum(1 for x in periods if x > 25)}")
    for name, group in (("busy", [r for r in rows if work(r) >= args.busy_ms]),
                        ("light", [r for r in rows if work(r) < args.busy_ms])):
        if not group:
            continue
        total = st.mean(work(r) for r in group)
        print(f"  {name} refreshes ({'>=' if name == 'busy' else '<'} {args.busy_ms:g} ms): {len(group)}, "
              f"mean work {total:.2f} ms; VM thread CPU {st.mean(val(r, 'vm_cpu_ms') for r in group):.2f} ms, "
              f"graphics thread CPU {st.mean(val(r, 'gs_cpu_ms') for r in group):.2f} ms, "
              f"pacer wait {st.mean(val(r, 'wait_ms') for r in group):.2f} ms")
        parts = sorted(((st.mean(val(r, z) for r in group), z) for z in zones), reverse=True)
        print("    " + "  ".join(f"{z} {m:.2f} ({100 * m / total:.0f}%)" for m, z in parts if m >= 0.005))
    _ = ends


if __name__ == "__main__":
    main()
