#!/usr/bin/env python3
"""Recon of the HP2 (PS2, NTSC-U) Z fileset.  DEV-ONLY evidence generator.

Reads the user's dump strictly read-only (C2).  Everything it learns comes
from the dump itself (C1): ZDIR.BIN records, strings inside SLUS_203.62 and
strings inside the files of ZZDATA.BIN (LZC files are decompressed first).

Name recovery
-------------
ZDIR stores only a 32-bit hash per file.  A candidate name is accepted only
if its hash is present in ZDIR, but a hash match alone is *not* proof when
many candidates are tried (2^32 is small).  Candidates are therefore ranked
by provenance and the expected number of chance matches is reported:

  A  the complete path is a literal string in the ELF or in a data file,
     or an ELF printf template whose only argument is a number
  B  an ELF printf template with one %s, filled with a clean token
  C  <known directory> + clean token + <known extension>

For tiers B and C a content check must also pass (extension vs. magic bytes
of files already named in tier A, .MSH/.MSB sibling pairs, .BIN/.LZC pairs).

Usage:  python -B tools/recon/zrecon.py <rom_dir> [--out <dir>] [--deep]
Outputs (to --out, default .cache/recon):
  zdir_records.tsv   one line per ZDIR record (+ recovered name, tier)
  names.tsv          hash, name, tier, provenance
  unresolved.tsv     records without a name
  report.txt         human-readable summary (also printed)
"""
import argparse
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SECTOR = 2048
M32 = 0xFFFFFFFF
INV33 = pow(33, -1, 1 << 32)
MAXTOK = 40
CLEAN_TOKEN = re.compile(r"[A-Za-z0-9_]{2,24}")
CLEAN_PATH = re.compile(r"[A-Za-z0-9_ ]+(\\[A-Za-z0-9_ ]+)*\.[A-Za-z0-9]{2,4}")
CLEAN_SUBPATH = re.compile(r"[A-Za-z0-9_]+(\\[A-Za-z0-9_]+)+(\.[A-Za-z0-9]{2,4})?")
# "name.ext" or "dir\dir\name.ext" embedded anywhere in binary data
PATH_ANY = re.compile(rb"[A-Za-z0-9_][A-Za-z0-9_ \\]{1,80}\.[A-Za-z][A-Za-z0-9]{1,3}")


def nfs_hash(name: str, seed: int = 0xFFFFFFFF) -> int:
    """h = seed; h = h*33 + byte, over the upper-cased name."""
    h = seed
    for b in name.upper().encode("ascii", "replace"):
        h = (h * 33 + b) & M32
    return h


def c_strings(data: bytes, minlen: int = 3, maxlen: int = 64):
    """Printable runs that are NUL-terminated (real C strings, less junk)."""
    for m in re.finditer(rb"([\x20-\x7e]{%d,%d})\x00" % (minlen, maxlen), data):
        yield m.group(1).decode("ascii")


def read_zdir(path):
    raw = open(path, "rb").read()
    assert len(raw) % 12 == 0, "ZDIR size not a multiple of 12"
    return [(i // 12,) + struct.unpack_from("<III", raw, i)
            for i in range(0, len(raw), 12)]


def elf_section_names(elf: bytes):
    shoff, = struct.unpack_from("<I", elf, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 46)
    o = shoff + shstrndx * shentsize
    stroff, strsz = struct.unpack_from("<II", elf, o + 16)
    return {s.decode("ascii", "replace")
            for s in elf[stroff:stroff + strsz].split(b"\x00") if s}


SPEC = re.compile(r"%(-?)(0?)(\d*)([ds])")


def template_pairs(tmpl, max_num=100):
    """printf template -> (prefix, suffix) around its single %s; a single %d
    is expanded 0..max_num-1.  No %s -> (full_string, None)."""
    specs = list(SPEC.finditer(tmpl))
    n_s = sum(1 for m in specs if m.group(4) == "s")
    n_d = len(specs) - n_s
    if n_s > 1 or n_d > 1 or not specs:
        return
    variants = [tmpl]
    if n_d:
        m = next(m for m in specs if m.group(4) == "d")
        _, zero, width, _ = m.groups()
        w = int(width) if width else 0
        variants = [tmpl[:m.start()] +
                    (str(n).rjust(w, "0" if zero else " ") if w else str(n)) +
                    tmpl[m.end():] for n in range(max_num)]
    for v in variants:
        if n_s:
            i = v.index("%s")
            yield v[:i], v[i + 2:]
        else:
            yield v, None


class Solver:
    def __init__(self, recs, heads):
        self.recs = recs
        self.heads = heads                      # idx -> first 16 bytes
        self.wanted = {r[1] for r in recs}
        self.by_hash = collections.defaultdict(list)
        for r in recs:
            self.by_hash[r[1]].append(r)
        self.names = {}                         # hash -> (NAME, tier, how)
        self.rejected = []                      # (hash, name, tier, how, why)
        self.pairs = {}                         # (PREFIX, SUFFIX) -> tier
        self.pair_group = {}                    # (PREFIX, SUFFIX) -> sibling group or None
        self.pending = collections.defaultdict(dict)   # (group, token) -> {hash: cand}
        self.table = {}
        self.tried = collections.Counter()      # tier -> candidates tested
        self.magic_count = collections.Counter(
            heads[r[0]][:4] for r in {(x[2], x[3]): x for x in recs if x[3]}.values())

    # ---- content validation -------------------------------------------
    def magic(self, h):
        for idx, _h, _sec, size in self.by_hash[h]:
            if size:
                return self.heads[idx]
        return b""

    def size(self, h):
        return max(r[3] for r in self.by_hash[h])

    def ext_magics(self):
        m = collections.defaultdict(set)
        for h, (name, tier, _how) in self.names.items():
            if tier == "A" and "." in name and self.size(h):
                m[name.rsplit(".", 1)[1]].add(self.magic(h)[:4])
        return m

    def validate(self, h, name):
        """-> (reason or None, strong).  `strong` means the content/sibling
        evidence is independent enough that a chance hash match is implausible."""
        head, size = self.magic(h), self.size(h)
        stem, _, ext = name.rpartition(".")

        def sib(other_ext):
            return nfs_hash(f"{stem}.{other_ext}")

        if size == 0:
            return None, False
        if ext == "LZC" and head[:4] != b"COMP":
            return "LZC without COMP magic", False
        if ext != "LZC" and head[:4] == b"COMP":
            return "COMP data under non-LZC name", False
        if ext == "AVI" and head[:4] != b"RIFF":
            return "AVI without RIFF magic", False
        if ext != "AVI" and head[:4] == b"RIFF":
            return "RIFF data under non-AVI name", False
        if ext == "MSH":
            if struct.unpack_from("<I", head, 0)[0] != size:
                return "MSH whose first u32 is not its own size", False
            if sib("MSB") not in self.wanted:
                return "MSH without its MSB sibling", False
            return None, True
        if ext == "MSB":
            s = sib("MSH")
            if s not in self.wanted:
                return "MSB without its MSH sibling", False
            if struct.unpack_from("<I", self.magic(s), 0)[0] != self.size(s):
                return "MSB whose MSH sibling is not a bank header", False
            return None, True
        if ext == "LZC":       # a second, independent hash hit on the raw twin
            return None, sib("BIN") in self.wanted or sib("BUN") in self.wanted
        if ext in ("BIN", "BUN"):
            s = sib("LZC")
            return None, s in self.wanted and self.magic(s)[:4] == b"COMP"
        known = self.ext_magics().get(ext)
        if known:
            if head[:4] not in known:
                return f"magic {head[:4].hex()} unlike other .{ext} files", False
            return None, self.magic_count[head[:4]] <= 4   # rare magic = strong
        return None, False

    # ---- candidates ------------------------------------------------------
    def offer(self, s, tier, how, group=None):
        self.tried[tier] += 1
        h = nfs_hash(s)
        if h not in self.wanted:
            return False
        name = s.upper()
        cur = self.names.get(h)
        if cur and (cur[0] == name or cur[1] <= tier):
            return False
        if tier != "A":
            why, strong = self.validate(h, name)
            if why:
                self.rejected.append((h, name, tier, how, why))
                return False
            if tier in "CD" and not strong:
                # Not accepted on its own.  Numbered guesses may still be
                # confirmed later by siblings of the same pattern.
                if group is not None:
                    self.pending[group][h] = (name, tier, how)
                else:
                    self.rejected.append((h, name, tier, how, "no independent confirmation"))
                return False
        self.names[h] = (name, tier, how)
        return True

    def confirm_groups(self, minimum=2):
        """Accept pending numbered candidates when >= `minimum` different
        files fit one pattern (same directory, token and extension)."""
        n = 0
        for group, members in sorted(self.pending.items()):
            members = {h: v for h, v in members.items() if h not in self.names}
            if len(members) >= minimum:
                for h, (name, tier, how) in members.items():
                    self.names[h] = (name, tier, how + f" [{len(members)} siblings]")
                    n += 1
            else:
                for h, (name, tier, how) in members.items():
                    self.rejected.append((h, name, tier, how, "lone numbered guess"))
        self.pending.clear()
        return n

    def add_pair(self, prefix, suffix, tier, group=None):
        p = (prefix.upper(), suffix.upper())
        if p not in self.pairs or tier < self.pairs[p]:
            self.pairs[p] = tier
            self.pair_group[p] = group

    def rebuild(self):
        self.table = collections.defaultdict(list)
        todo = [h for h in self.wanted if self.names.get(h, ("", "Z"))[1] > "A"]
        pow33 = [pow(33, k, 1 << 32) for k in range(MAXTOK + 1)]
        for (prefix, suffix), tier in self.pairs.items():
            hp, es = nfs_hash(prefix), nfs_hash(suffix, 0)
            inv_q = pow(INV33, len(suffix), 1 << 32)
            for h in todo:
                x = ((h - es) * inv_q) & M32          # hash(prefix + token)
                if x == hp:                            # empty token
                    self.offer(prefix + suffix, tier, f"{prefix}*{suffix} (empty)")
                for ln in range(1, MAXTOK + 1):
                    t = (x - hp * pow33[ln]) & M32
                    self.table[(ln << 32) | t].append((prefix, suffix, tier))

    def try_token(self, tok, src):
        ln = len(tok)
        if not 1 <= ln <= MAXTOK:
            return 0
        self.tried["tokens"] += 1
        hits = self.table.get((ln << 32) | nfs_hash(tok, 0))
        n = 0
        if hits:
            for prefix, suffix, tier in hits:
                g = self.pair_group.get((prefix, suffix))
                n += self.offer(prefix + tok + suffix, tier,
                                f"{src}:{prefix}*{suffix}",
                                group=(g, tok.upper()) if g else None)
        return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--out", default=os.path.join(".cache", "recon"))
    ap.add_argument("--deep", action="store_true",
                    help="also mine strings from the files inside ZZDATA")
    ap.add_argument("--export-names", metavar="FILE",
                    help="write the recovered name table (data file for the C++ library)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    zdir = os.path.join(a.rom, "NFSHP2", "ZDIR.BIN")
    zz = os.path.join(a.rom, "NFSHP2", "ZZDATA.BIN")
    elf = os.path.join(a.rom, "SLUS_203.62")
    recs = read_zdir(zdir)
    zsize = os.path.getsize(zz)
    rep = []
    P = rep.append

    # ---------------- structure ----------------
    P(f"records: {len(recs)}  distinct hashes: {len({r[1] for r in recs})}")
    P(f"ZZDATA size: {zsize} bytes = {zsize // SECTOR} sectors (rem {zsize % SECTOR})")
    P(f"sorted by sector: {all(recs[i][2] <= recs[i+1][2] for i in range(len(recs)-1))}")
    P(f"zero-size records: {sum(1 for r in recs if r[3] == 0)}")
    ext = sorted({(r[2], r[3]) for r in recs if r[3]})
    P(f"distinct non-empty extents: {len(ext)}")
    overl, gaps, pos = 0, [], 0
    for sec, size in ext:
        nsec = -(-size // SECTOR)
        if sec < pos:
            overl += 1
        elif sec > pos:
            gaps.append((pos, sec - pos))
        pos = max(pos, sec + nsec)
    P(f"overlapping extents: {overl}; gaps: {len(gaps)}; end of last extent: "
      f"sector {pos} (file has {zsize // SECTOR})")
    dup = collections.Counter(r[1] for r in recs)
    for h, n in dup.items():
        if n > 1:
            P(f"duplicate hash {h:08X} x{n}: " +
              ", ".join(f"#{r[0]}@{r[2]}+{r[3]}" for r in recs if r[1] == h))
    P(f"sum of record sizes: {sum(r[3] for r in recs)}")

    # ---------------- read heads / mine strings ----------------
    import lzc
    heads, magics = {}, collections.Counter()
    mined = set()
    anywhere = set()      # path-like byte runs found anywhere (not NUL-bound)
    lzc_ok = lzc_bad = 0
    with open(zz, "rb") as f:
        for idx, h, sec, size in recs:
            head = b""
            if size:
                f.seek(sec * SECTOR)
                head = f.read(min(16, size))
            heads[idx] = head
            magics[head[:4].hex() if size else "(empty)"] += 1
            if not (a.deep and size) or head[:4] == b"RIFF" or size > (64 << 20):
                continue
            f.seek(sec * SECTOR)
            data = f.read(size)
            if lzc.is_comp(data):
                try:
                    data = lzc.decompress(data)
                    lzc_ok += 1
                except ValueError:
                    lzc_bad += 1
                    continue
            mined.update(c_strings(data + b"\x00"))
            anywhere.update(m.group().decode() for m in PATH_ANY.finditer(data))
        if a.deep:                               # stream names inside the movies
            for idx, h, sec, size in recs:
                if heads[idx][:4] == b"RIFF":
                    f.seek(sec * SECTOR)
                    anywhere.update(m.group().decode()
                                    for m in PATH_ANY.finditer(f.read(4096)))

    # ---------------- name recovery ----------------
    S = Solver(recs, heads)
    elf_data = open(elf, "rb").read()
    sect = elf_section_names(elf_data)
    elf_strs = sorted(set(c_strings(elf_data, 2, 96)))

    # Tier A: literals and numeric templates
    for s in elf_strs:
        S.offer(s, "A", "elf-literal")
    for s in elf_strs:
        if SPEC.search(s) and re.search(r"\\|\.[A-Za-z0-9]{2,4}$", s):
            for prefix, suffix in template_pairs(s) or ():
                if suffix is None:
                    S.offer(prefix, "A", f"elf-template {s}")
                else:
                    S.add_pair(prefix, suffix, "B")
    for s in mined:
        if CLEAN_PATH.fullmatch(s):
            S.offer(s, "A", "data-literal")
    # literal sub-paths: data embeds build paths such as
    # "\Psx2\CD\GLOBAL\CHARACTERSA.BIN"; every tail after a backslash counts.
    bare_stems = set()
    for s in anywhere | {m.group().decode() for m in PATH_ANY.finditer(elf_data)}:
        parts = s.split("\\")
        if len(parts) == 1:
            bare_stems.add(s.rsplit(".", 1)[0].strip())
            continue
        for k in range(len(parts) - 1):
            S.offer("\\".join(parts[k:]), "A", "literal-subpath")
        bare_stems.add(parts[-1].rsplit(".", 1)[0].strip())
    bare_stems = {t for t in bare_stems if re.fullmatch(r"[A-Za-z0-9_ ]{2,32}", t)}
    nA = len(S.names)
    P(f"ELF C-strings: {len(elf_strs)}; mined data C-strings: {len(mined)} "
      f"(LZC ok={lzc_ok} failed={lzc_bad})")
    P(f"tier A (literal paths, numeric templates): {nA} names, "
      f"{S.tried['A']} candidates tried -> expected chance matches "
      f"{S.tried['A'] * len(S.wanted) / 2**32:.4f}")

    # tokens: clean identifiers and clean sub-paths only
    def tokens(strings):
        out = set()
        for s in strings:
            if CLEAN_TOKEN.fullmatch(s) or CLEAN_SUBPATH.fullmatch(s):
                out.add(s)
            if CLEAN_PATH.fullmatch(s) or CLEAN_SUBPATH.fullmatch(s):
                for part in re.split(r"[\\.]", s):
                    if CLEAN_TOKEN.fullmatch(part):
                        out.add(part)
                out.add(s.rsplit(".", 1)[0])
        return out

    elf_tok, data_tok = tokens(elf_strs), tokens(mined)
    data_tok -= elf_tok

    elf_dirs = {s for s in elf_strs if re.fullmatch(r"[A-Za-z_]{3,12}\\", s)}
    elf_exts = {s for s in elf_strs
                if re.fullmatch(r"\.[A-Za-z][A-Za-z0-9]{1,3}", s) and s not in sect}
    P(f"ELF dir fragments: {sorted(elf_dirs)}; ext fragments: {sorted(elf_exts)}")

    for rnd in range(1, 6):
        dirs = {d.upper() for d in elf_dirs}
        exts = {e.upper() for e in elf_exts}
        for n, _tier, _how in S.names.values():
            parts = n.split("\\")
            for k in range(1, len(parts)):
                dirs.add("\\".join(parts[:k]) + "\\")
            if "." in n:
                exts.add("." + n.rsplit(".", 1)[1])
        for d in dirs:
            for e in exts:
                S.add_pair(d, e, "C")
        before = len(S.names), sum(1 for v in S.names.values() if v[1] == "B")
        S.rebuild()
        for t in sorted(elf_tok):
            S.try_token(t, "elf-token")
        for t in sorted(data_tok):
            S.try_token(t, "data-token")
        nB = sum(1 for v in S.names.values() if v[1] == "B")
        nC = sum(1 for v in S.names.values() if v[1] == "C")
        P(f"round {rnd}: pairs={len(S.pairs)} dirs={len(dirs)} exts={len(exts)} "
          f"tokens={len(elf_tok)}+{len(data_tok)} -> names A={nA} B={nB} C={nC}")
        if (len(S.names), nB) == before:
            break
    # Tier D ("probable"): stems of bare file names seen in data ("desat.avi",
    # AVI stream names) under a known directory + known extension.  Guessed
    # numbered ".TXT" siblings cover the developer notes next to MARKERS\.
    for d in sorted(dirs):
        for e in sorted(exts) + [".TXT"]:
            S.add_pair(d, e, "D")
            for n in (11, 12, 21, 22, 32):
                S.add_pair(d, f"{n}{e}", "D", group=("numbered", d, e))
    S.rebuild()
    nd0 = len(S.names)
    for t in sorted(bare_stems | elf_tok | data_tok):
        S.try_token(t, "stem")
    S.confirm_groups()
    P(f"tier D (bare stems {len(bare_stems)}; numbered guesses need >=2 siblings): "
      f"+{len(S.names) - nd0}")
    nb_pairs = sum(1 for t in S.pairs.values() if t == "B")
    nc_pairs = len(S.pairs) - nb_pairs
    ntok = len(elf_tok) + len(data_tok)
    todo = len(S.wanted) - nA
    P(f"expected chance matches before content checks: "
      f"tier B ~{ntok * nb_pairs * todo / 2**32:.2f}, "
      f"tier C ~{ntok * nc_pairs * todo / 2**32:.2f}")
    P(f"names recovered: {len(S.names)} of {len(S.wanted)} distinct hashes; "
      f"rejected by content checks: {len(S.rejected)}")
    P("first-4-byte magics: " + ", ".join(f"{k}x{v}" for k, v in magics.most_common(48)))

    with open(os.path.join(a.out, "zdir_records.tsv"), "w") as f:
        f.write("idx\thash\tsector\tsize\thead16\ttier\tname\n")
        for idx, h, sec, size in recs:
            n = S.names.get(h, ("", "", ""))
            f.write(f"{idx}\t{h:08X}\t{sec}\t{size}\t{heads[idx].hex()}\t{n[1]}\t{n[0]}\n")
    with open(os.path.join(a.out, "names.tsv"), "w") as f:
        for h, (n, tier, how) in sorted(S.names.items(), key=lambda kv: kv[1][0]):
            f.write(f"{h:08X}\t{n}\t{tier}\t{how}\n")
    with open(os.path.join(a.out, "rejected.tsv"), "w") as f:
        for h, n, tier, how, why in S.rejected:
            f.write(f"{h:08X}\t{n}\t{tier}\t{how}\t{why}\n")
    with open(os.path.join(a.out, "unresolved.tsv"), "w") as f:
        for idx, h, sec, size in recs:
            if h not in S.names:
                f.write(f"{idx}\t{h:08X}\t{sec}\t{size}\t{heads[idx].hex()}\n")
    if a.export_names:
        with open(a.export_names, "w", newline="\n") as f:
            f.write(
                "# File names of the Z fileset of Need for Speed: Hot Pursuit 2 (PS2, NTSC-U,\n"
                "# SLUS-20362 v1.00), recovered from the owner's dump by tools/recon/zrecon.py.\n"
                "# No third-party list was used (docs/prior-art.md). Format: HASH<TAB>TIER<TAB>NAME\n"
                "# HASH = name hash stored in ZDIR.BIN; TIER = confidence, docs/dump-analysis.md section 4:\n"
                "#   A literal path in the dump / numeric template   B ELF %s template + token\n"
                "#   C directory + token + extension, content-checked   D pattern confirmed by siblings\n"
                "# Regenerate: python -B tools/recon/zrecon.py <rom_dir> --deep --export-names <this file>\n")
            for h, (n, tier, _how) in sorted(S.names.items(), key=lambda kv: kv[1][0]):
                f.write(f"{h:08X}\t{tier}\t{n}\n")
    text = "\n".join(rep)
    with open(os.path.join(a.out, "report.txt"), "w") as f:
        f.write(text + "\n")
    print(text)


if __name__ == "__main__":
    sys.exit(main())
