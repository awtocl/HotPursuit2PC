#!/usr/bin/env python3
"""Render the asset inventory section of docs/dump-analysis.md (DEV-ONLY).

Input : tests/fixtures/slus20362_zfileset_inventory.tsv (written by
        `hp2assets inventory --sha256`, cross-checked by crosscheck_inventory.py)
Output: replaces the text between the inventory markers in docs/dump-analysis.md

Usage: python -B tools/recon/make_inventory_md.py
"""
import collections
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TSV = os.path.join(ROOT, "tests", "fixtures", "slus20362_zfileset_inventory.tsv")
DOC = os.path.join(ROOT, "docs", "dump-analysis.md")
BEGIN, END = "<!-- inventory:begin -->", "<!-- inventory:end -->"

KIND_TEXT = {
    "bundle": "chunked bundle (`.BUN`, some `.BIN`)",
    "bundle-lzc": "LZC-compressed bundle (`COMP`)",
    "car-geometry": "car geometry bundle",
    "engine-sound-table": "engine sound table",
    "movie": "RIFF/AVI movie",
    "music": "jukebox music container",
    "nis-camera": "cut-scene camera track",
    "nis-scene": "cut-scene data",
    "placeholder": "empty placeholder entry",
    "replay": "recorded replay",
    "save-icon": "memory card save icon",
    "sound-bank-body": "sound bank sample data",
    "sound-bank-header": "sound bank header",
    "speech-events": "speech event table",
    "speech-pack": "speech pack",
    "strings": "localised string table",
    "text": "text (developer notes)",
    "texture-pack": "texture pack",
    "track-markers": "track marker table",
    "unknown": "unidentified",
}


def main():
    rows = [line.rstrip("\r\n").split("\t") for line in open(TSV, encoding="utf-8")]
    head, rows = rows[0], rows[1:]
    c = {n: i for i, n in enumerate(head)}
    out = []
    w = out.append
    primary = [r for r in rows if "dup-of" not in r[c["flags"]]]
    total = sum(int(r[c["size"]]) for r in primary)
    w(f"{len(rows)} directory records = {len(primary)} distinct files, "
      f"{total:,} bytes (duplicates counted once). Source: "
      "`tests/fixtures/slus20362_zfileset_inventory.tsv`, which also holds the SHA-256 "
      "of every file. *Type* is a classification by name and magic bytes; internal "
      "formats other than LZC are **UNVERIFIED** (ADR 0004).")
    w("")
    w("### 5.1 By type")
    w("")
    w("| Type | Files | Bytes | Meaning |")
    w("|---|---:|---:|---|")
    by_kind = collections.defaultdict(lambda: [0, 0])
    for r in primary:
        k = by_kind[r[c["kind"]]]
        k[0] += 1
        k[1] += int(r[c["size"]])
    for kind in sorted(by_kind):
        n, size = by_kind[kind]
        w(f"| `{kind}` | {n} | {size:,} | {KIND_TEXT.get(kind, '')} |")
    w("")
    w("### 5.2 By top-level folder")
    w("")
    w("| Folder | Files | Bytes |")
    w("|---|---:|---:|")
    by_dir = collections.defaultdict(lambda: [0, 0])
    for r in primary:
        name = r[c["name"]]
        d = name.split("\\")[0] + "\\" if "\\" in name else "(root)"
        by_dir[d][0] += 1
        by_dir[d][1] += int(r[c["size"]])
    for d in sorted(by_dir):
        w(f"| `{d}` | {by_dir[d][0]} | {by_dir[d][1]:,} |")
    w("")
    w("### 5.3 Every record, in disc order")
    w("")
    w("Tier = name confidence (§4); `-` = no recovered name. Flags: `empty`, "
      "`dup-of=N` (same file stored again; identical bytes), `unnamed`.")
    w("")
    w("| # | Hash | Sector | Size | Tier | Type | Flags | Name |")
    w("|---:|---|---:|---:|:-:|---|---|---|")
    for r in rows:
        flags = r[c["flags"]]
        w(f"| {r[c['index']]} | `{r[c['hash']]}` | {r[c['sector']]} | {int(r[c['size']]):,} | "
          f"{r[c['tier']]} | {r[c['kind']]} | {'' if flags == '-' else flags} | `{r[c['name']]}` |")
    block = "\n".join(out)

    doc = open(DOC, encoding="utf-8").read()
    a, b = doc.index(BEGIN), doc.index(END)
    doc = doc[:a + len(BEGIN)] + "\n" + block + "\n" + doc[b:]
    with open(DOC, "w", encoding="utf-8", newline="\n") as f:
        f.write(doc)
    print(f"inventory section written: {len(rows)} records, {len(primary)} files, {total} bytes")


if __name__ == "__main__":
    main()
