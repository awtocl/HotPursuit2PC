#!/usr/bin/env python3
"""HP2 'COMP' (.LZC) decompressor - recon/reference implementation.

Format worked out from the user's dump (hand-traced on GLOBAL\\GLOBALB.LZC vs
GLOBAL\\GLOBALB.BUN, then verified on every LZC/BUN pair on the disc):

Header (16 bytes, little-endian):
  0  char[4]  "COMP"
  4  u32      0x00001001
  8  u32      unpacked size
  12 u32      packed size (== whole file size, header included)
Body: LZSS.  Repeat: read a u16 LE flag word, consume its bits LSB-first:
  bit 0 -> copy one literal byte
  bit 1 -> 2-byte token (b0, b1):
             length   = (b0 & 0x0F) + 3          (3..18)
             distance = ((b0 & 0xF0) << 4) | b1  (1..4095, back from output)
Stops when `unpacked size` bytes have been produced.

Usage:  python -B tools/recon/lzc.py <rom_dir>     (self-test on the dump)
"""
import struct

MAGIC = b"COMP"


def is_comp(data: bytes) -> bool:
    return len(data) >= 16 and data[:4] == MAGIC


def header(data: bytes):
    """-> (magic, version, unpacked_size, packed_size)"""
    return struct.unpack_from("<4sIII", data, 0)


def decompress(data: bytes) -> bytes:
    magic, _ver, unpacked, packed = header(data)
    if magic != MAGIC:
        raise ValueError("not a COMP stream")
    out = bytearray(unpacked)
    n = min(len(data), packed)
    ip, op = 16, 0
    while op < unpacked:
        if ip + 2 > n:
            raise ValueError(f"truncated input at {ip} (out {op}/{unpacked})")
        flags = data[ip] | (data[ip + 1] << 8)
        ip += 2
        for _ in range(16):
            if op >= unpacked:
                break
            if flags & 1:
                b0, b1 = data[ip], data[ip + 1]
                ip += 2
                length = (b0 & 0x0F) + 3
                dist = ((b0 & 0xF0) << 4) | b1
                if dist == 0 or dist > op:
                    raise ValueError(f"bad distance {dist} at out {op}, in {ip - 2}")
                if length > unpacked - op:
                    length = unpacked - op
                for _i in range(length):
                    out[op] = out[op - dist]
                    op += 1
            else:
                out[op] = data[ip]
                op += 1
                ip += 1
            flags >>= 1
    return bytes(out)


if __name__ == "__main__":
    import hashlib
    import os
    import sys

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from zrecon import nfs_hash, read_zdir  # noqa: E402

    rom = sys.argv[1]
    recs = read_zdir(os.path.join(rom, "NFSHP2", "ZDIR.BIN"))
    by_hash = {}
    for _idx, h, sec, size in recs:
        by_hash.setdefault(h, (sec, size))
    stems = ["GLOBAL\\GLOBALB", "FRONTEND\\FRONTB", "GLOBAL\\INGAMEB"]
    stems += [f"TRACKS\\TRACKB{n}" for n in (24, 25, 26, 44, 45, 46, 65, 66)]
    ok_all = True
    with open(os.path.join(rom, "NFSHP2", "ZZDATA.BIN"), "rb") as f:
        for stem in stems:
            ls, lz = by_hash[nfs_hash(stem + ".LZC")]
            bs, bz = by_hash[nfs_hash(stem + ".BUN")]
            f.seek(ls * 2048)
            comp = f.read(lz)
            f.seek(bs * 2048)
            raw = f.read(bz)
            try:
                dec = decompress(comp)
                ok = dec == raw
                print(f"{stem}: packed={lz} unpacked={len(dec)} match={ok} "
                      f"sha256={hashlib.sha256(dec).hexdigest()[:16]}")
            except Exception as e:  # noqa: BLE001 - recon output
                ok = False
                print(f"{stem}: FAILED {e}")
            ok_all &= ok
    print("ALL PAIRS MATCH" if ok_all else "MISMATCH")
    sys.exit(0 if ok_all else 1)
