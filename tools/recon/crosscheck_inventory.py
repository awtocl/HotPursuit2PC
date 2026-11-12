#!/usr/bin/env python3
"""Independent cross-check of the C++ inventory (DEV-ONLY).

Recomputes, straight from ZDIR.BIN/ZZDATA.BIN, the (index, hash, sector, size,
sha256) of every record and compares them with the TSV written by
`hp2assets inventory --sha256`. Also checks every name in the TSV against its
hash. Exit code 0 = identical.

Usage: python -B tools/recon/crosscheck_inventory.py <rom_dir> <inventory.tsv>
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zrecon import nfs_hash, read_zdir  # noqa: E402


def main():
    rom, tsv = sys.argv[1], sys.argv[2]
    recs = read_zdir(os.path.join(rom, "NFSHP2", "ZDIR.BIN"))
    rows = [line.rstrip("\r\n").split("\t") for line in open(tsv, encoding="utf-8")]
    header, rows = rows[0], rows[1:]
    col = {name: i for i, name in enumerate(header)}
    bad = 0
    if len(rows) != len(recs):
        print(f"row count differs: tsv {len(rows)} vs zdir {len(recs)}")
        bad += 1
    with open(os.path.join(rom, "NFSHP2", "ZZDATA.BIN"), "rb") as f:
        for (idx, h, sec, size), row in zip(recs, rows):
            f.seek(sec * 2048)
            digest = hashlib.sha256(f.read(size)).hexdigest()
            want = (str(idx), f"{h:08X}", str(sec), str(size), digest)
            got = tuple(row[col[c]] for c in ("index", "hash", "sector", "size", "sha256"))
            if want != got:
                print(f"record {idx}: python {want} != c++ {got}")
                bad += 1
            name = row[col["name"]]
            if "unnamed" not in row[col["flags"]] and nfs_hash(name) != h:
                print(f"record {idx}: name {name!r} does not hash to {h:08X}")
                bad += 1
    print(f"{len(rows)} records compared, {bad} differences")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
