"""List the IOP modules (IRX) on the game disc: module name and version from the
.iopmod section, size, and the libraries they import (by name).

Usage: python -B tools/recon/irxinfo.py <ROM folder>/NFSHP2/IRX243
Reads only. Tells which modules the reference core can replace by its own
high-level versions (matched by module name) and which run as IOP code.
"""
import pathlib
import re
import struct
import sys

IOPMOD = 0x70000080


def info(path):
    d = path.read_bytes()
    if d[:4] != b"\x7fELF":
        return None
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    name = version = None
    for i in range(shnum):
        _n, typ, _f, _a, off, size = struct.unpack_from("<6I", d, shoff + i * shentsize)
        if typ == IOPMOD:
            # IOPMOD: module info ptr, entry, gp, text size, data size, bss size, version (16), name...
            version = struct.unpack_from("<H", d, off + 24)[0]
            raw = d[off + 26:off + size]
            name = raw.split(b"\0")[0].decode("latin1")
    # Import tables: magic 0x41E00000, then the library name (8 bytes) at +12
    imports = sorted({d[m.start() + 12:m.start() + 20].rstrip(b"\0").decode("latin1")
                      for m in re.finditer(re.escape(struct.pack("<I", 0x41E00000)), d)})
    return name, version, len(d), imports


def main(folder):
    for p in sorted(pathlib.Path(folder).glob("*.IRX")):
        r = info(p)
        if not r:
            print(f"{p.name}: not an ELF module")
            continue
        name, version, size, imports = r
        v = f"{version >> 8}.{version & 0xFF}" if version is not None else "?"
        print(f"{p.name:12} module '{name}' v{v}, {size} bytes; imports: {', '.join(imports)}")


if __name__ == "__main__":
    main(sys.argv[1])
