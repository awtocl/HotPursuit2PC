"""Analyse a VIF1 command stream recorded with the launcher's --trace-vif (vif1-stream.bin).

Question it answers (ADR 0010): if VU1 ran on its own thread, how much of
VIF1's work could overlap with a running microprogram? After a program starts
(MSCAL/MSCALF/MSCNT), VIF1 can keep going until a command that must wait for
the program's end: the next program start, FLUSH/FLUSHE/FLUSHA, MPG (changes
microcode), DIRECT/DIRECTHL (GIF path 2, ordered against the program's xgkicks
in the reference core). The UNPACK data VIF1 handles before that is the
overlappable share; UNPACKs relative to TOPS (FLG bit) go to the double buffer
the running program does not use.

Usage: python -B tools/recon/vifstream.py <trace dir>/vif1-stream.bin
"""
import collections
import struct
import sys

WAIT = {0x10: "FLUSHE", 0x11: "FLUSH", 0x13: "FLUSHA", 0x14: "MSCAL", 0x15: "MSCALF", 0x17: "MSCNT",
        0x4A: "MPG", 0x50: "DIRECT", 0x51: "DIRECTHL"}
START = {0x14, 0x15, 0x17}


def load(path):
    """Concatenated stream; the DMA tag half of tag-carrying transfers is replaced by two NOP codes."""
    data = bytearray()
    frames = []  # (stream offset, frame)
    with open(path, "rb") as f:
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            frame, size, tag, _address = struct.unpack("<4I", h)
            chunk = bytearray(f.read(size))
            if tag:
                chunk[0:8] = b"\0" * 8
            frames.append((len(data), frame))
            data += chunk
    return bytes(data), frames


def main(path):
    data, frames = load(path)
    n = len(data)
    pos = 0
    cl, wl = 1, 1
    events = []  # (offset, kind, unpack bytes, flg)
    unknown = collections.Counter()
    while pos + 4 <= n:
        code = struct.unpack_from("<I", data, pos)[0]
        at = pos
        pos += 4
        cmd = (code >> 24) & 0x7F
        num = (code >> 16) & 0xFF
        imm = code & 0xFFFF
        if cmd == 0x01:
            cl, wl = imm & 0xFF, (imm >> 8) & 0xFF
            events.append((at, "STCYCL", 0, 0))
        elif cmd in (0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07):
            events.append((at, {0: "NOP", 2: "OFFSET", 3: "BASE", 4: "ITOP", 5: "STMOD", 6: "MSKPATH3", 7: "MARK"}[cmd], 0, 0))
        elif cmd in WAIT and cmd not in (0x4A, 0x50, 0x51):
            events.append((at, WAIT[cmd], 0, 0))
        elif cmd == 0x20:
            pos += 4
            events.append((at, "STMASK", 0, 0))
        elif cmd in (0x30, 0x31):
            pos += 16
            events.append((at, "STROW" if cmd == 0x30 else "STCOL", 0, 0))
        elif cmd == 0x4A:
            pairs = num or 256
            pos = (pos + 7) & ~7
            pos += pairs * 8
            events.append((at, "MPG", 0, 0))
        elif cmd in (0x50, 0x51):
            qw = imm or 0x10000
            pos = (pos + 15) & ~15
            pos += qw * 16
            events.append((at, WAIT[cmd], 0, 0))
        elif cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            count = num or 256
            if wl > cl and cl > 0:
                count = (count // wl) * cl + min(count % wl, cl)
            elem_bits = 32 >> vl
            if vn == 3 and vl == 3:  # V4-5: 16 bits per vector
                bits = 16
            else:
                bits = elem_bits * (vn + 1)
            size = (count * bits + 31) // 32 * 4
            pos += size
            events.append((at, "UNPACK", size, (imm >> 15) & 1))
        else:
            unknown[cmd] += 1
            events.append((at, "?%02X" % cmd, 0, 0))

    # Segment: from each program start to the next command that must wait for the program's end.
    starts = 0
    overlappable = 0
    overlappable_flg = 0
    total_unpack = sum(e[2] for e in events if e[1] == "UNPACK")
    stopper = collections.Counter()
    cmds_between = []
    i = 0
    while i < len(events):
        kind = events[i][1]
        if kind in ("MSCAL", "MSCALF", "MSCNT"):
            starts += 1
            j = i + 1
            seg = 0
            seg_flg = 0
            k = 0
            # The reference core starts an MSCAL program one command later (after the next UNPACK).
            while j < len(events) and events[j][1] not in WAIT.values():
                if events[j][1] == "UNPACK":
                    seg += events[j][2]
                    seg_flg += events[j][2] if events[j][3] else 0
                j += 1
                k += 1
            overlappable += seg
            overlappable_flg += seg_flg
            cmds_between.append(k)
            stopper[events[j][1] if j < len(events) else "end"] += 1
            i = j
            continue
        i += 1

    nframes = len({f for _, f in frames}) or 1
    kinds = collections.Counter(e[1] for e in events)
    print(f"{path}: {n / 1e6:.1f} MB of VIF1 stream over {nframes} frames")
    print(f"commands per frame: " + ", ".join(f"{k} {v / nframes:.0f}" for k, v in kinds.most_common()))
    print(f"microprogram starts per frame: {starts / nframes:.0f}")
    print(f"UNPACK data per frame: {total_unpack / nframes / 1024:.0f} KB; between a start and the next waiting command: "
          f"{overlappable / nframes / 1024:.0f} KB ({100 * overlappable / max(total_unpack, 1):.1f} %), of which relative "
          f"to TOPS (double buffer): {100 * overlappable_flg / max(overlappable, 1):.1f} %")
    cb = sorted(cmds_between)
    if cb:
        print(f"commands between a start and the next waiting command: median {cb[len(cb) // 2]}, mean {sum(cb) / len(cb):.1f}")
    print("what ends that window: " + ", ".join(f"{k} {v}" for k, v in stopper.most_common()))
    if unknown:
        print("unparsed command codes: " + ", ".join(f"{k:02X}x{v}" for k, v in unknown.most_common(8)))


if __name__ == "__main__":
    main(sys.argv[1])
