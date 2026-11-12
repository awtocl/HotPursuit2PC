# assets-pipeline

C++ library (`hp2assets`) that understands the containers on the PS2 disc,
plus a small developer tool. Format evidence: `docs/dump-analysis.md`.
Design: [ADR 0004](../docs/adr/0004-asset-pipeline.md).

| Piece | Where | Status |
|---|---|---|
| Name hash, `ZDIR.BIN` parser, layout validation | `include/hp2/zdir.h` | done, tested on the real dump |
| `COMP`/`.LZC` decompressor | `include/hp2/lzc.h` | done, byte-exact on all 11 LZC/BUN pairs of the disc |
| Name table recovered from the dump | `data/hp2_slus20362_names.txt` → compiled in | 362 of 402 names |
| Fileset view, classification, inventory, unpacker (resumable, never overwrites silently) | `include/hp2/fileset.h` | done |
| Read-only file access, atomic outputs, SHA-256 | `include/hp2/io.h` | done |
| Format decoders (textures, bundles, audio, movies) | — | not started (after M1) |

## Tool

```
hp2assets info      <rom_dir>                          directory + layout summary
hp2assets inventory <rom_dir> [--sha256] [--out FILE]  one line per file (TSV)
hp2assets unpack    <rom_dir> <out_dir> [--verify] [--repair]
hp2assets lzc       <in.lzc> <out.bin>                 decompress one COMP stream
hp2assets hash      <name> [...]                       name hash as stored in ZDIR.BIN
```

`<rom_dir>` is the extracted disc root; it is opened read-only.

## Tests

- `test_assets` — no game data needed: hash vectors, synthetic ZDIR/LZC/fileset
  fixtures built on the fly (an independent test-only LZSS encoder round-trips
  the decoder), unpack resume/repair behaviour, the built-in name table.
- `test_dump` — against the owner's dump when present (`ROM/…` or `HP2_ROM_DIR`),
  otherwise skipped: layout facts, every name present, extension-vs-content,
  LZC pairs, all 81 `COMP` streams, full SHA-256 inventory vs. the committed
  fixture, exact + resumable unpack of the small files.

Rule: this directory never contains game data — only code, and names, hashes
and sizes derived from the owner's dump.
