#!/usr/bin/env python3
"""Where does the vector-unit (VU1) microcode that the game runs come from?

The launcher's --trace switch records every VU1 block the reference core
compiled, with its contents (vu1-blocks.tsv). This looks each block up in the
game's executable. If all of them are found there, the microprograms are
fixed data of the executable and can be translated ahead of time, like the
main program (stage R1, docs/recomp-analysis.md).

  python -B tools/recon/vucheck.py "ROM/Need for Speed - Hot Pursuit 2 (USA)/SLUS_203.62" .cache/r1-trace3/trace

Reads only. Prints a summary; exit code 1 when some executed microcode is not in the executable.
"""
import collections
import struct
import sys


def sections(elf):
    shoff, = struct.unpack_from("<I", elf, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 46)
    raw = [struct.unpack_from("<IIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    stroff = raw[shstrndx][4]
    out = []
    for name, kind, _flags, addr, off, size in raw:
        end = elf.index(b"\0", stroff + name)
        out.append((elf[stroff + name:end].decode("latin1"), kind, addr, off, size))
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    elf = open(sys.argv[1], "rb").read()
    secs = [s for s in sections(elf) if s[1] != 8 and s[4]]  # sections with file data

    def section_of(offset):
        for name, _kind, addr, off, size in secs:
            if off <= offset < off + size:
                return name, addr + (offset - off) if addr else None
        return "(outside any section)", None

    blocks = []
    created_total = 0
    with open(sys.argv[2].rstrip("/\\") + "/vu1-blocks.tsv") as f:
        for line in f:
            if line.startswith("#"):
                continue
            begin, end, frame, created, words = line.split("\t")
            data = b"".join(struct.pack("<I", int(words[i:i + 8], 16)) for i in range(0, len(words.strip()), 8))
            blocks.append((int(begin, 16), int(end, 16), int(frame), int(created), data))
            created_total += int(created)
    if not blocks:
        raise SystemExit("no VU1 blocks in the trace")

    by_section = collections.Counter()
    missing = []
    split = []  # blocks found as two consecutive pieces with a few bytes between them in the file
    load_delta = collections.Counter()  # (section, file position - VU address): one value per uploaded piece
    for begin, end, frame, created, data in blocks:
        pos = elf.find(data)
        if pos >= 0:
            name, _ = section_of(pos)
            by_section[name] += 1
            load_delta[(name, pos - begin)] += 1
            continue
        # The executable stores microcode as upload packets of at most 256 instruction pairs, each
        # with a header. A block that runs across two packets is therefore not contiguous in the file.
        found = None
        for k in range(8, len(data), 8):
            p1 = elf.find(data[:k])
            if p1 < 0:
                break
            p2 = elf.find(data[k:], p1 + k)
            if 0 <= p2 - (p1 + k) <= 16:
                found = (k, p1, p2, elf[p1 + k:p2])
        if found:
            k, p1, p2, between = found
            split.append((begin, end, begin + k, between))
            by_section[section_of(p1)[0] + " (across two upload packets)"] += 1
        else:
            missing.append((begin, end, frame, len(data)))

    pairs = sum(len(b[4]) for b in blocks) // 8
    addresses = collections.Counter(b[0] for b in blocks)
    print(f"distinct VU1 blocks executed: {len(blocks)} ({pairs} instruction pairs), "
          f"created {created_total} times in the session")
    print(f"block start addresses: {len(addresses)}; with more than one content over time: "
          f"{sum(1 for n in addresses.values() if n > 1)}")
    for name, n in by_section.most_common():
        print(f"  found in the executable, section {name}: {n} blocks")
    for begin, end, at, between in split:
        print(f"    VU address {begin:04X}..{end:04X}: packet boundary at {at:04X}, "
              f"{len(between)} bytes between the pieces in the file: {between.hex()}")
    print(f"  NOT found in the executable: {len(missing)} blocks")
    for begin, end, frame, size in missing[:20]:
        print(f"    VU address {begin:04X}..{end:04X} ({size} bytes), first seen at frame {frame}")
    programs = sorted(load_delta.items(), key=lambda kv: kv[0][1])
    print(f"distinct placements (one per uploaded microprogram): {len(programs)}")
    for (name, delta), n in programs:
        print(f"  {name} file offset {delta:#x} -> VU address 0: {n} blocks")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
