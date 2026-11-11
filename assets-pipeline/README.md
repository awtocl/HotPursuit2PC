# assets-pipeline

Library that understands the containers on the PS2 disc. Format evidence:
`docs/dump-analysis.md`. Design: ADR 0004 (written in phase P2).

| Piece | Phase | Status |
|---|---|---|
| Name hash, `ZDIR.BIN` parser, layout validation | P2 | pending |
| `COMP`/`.LZC` decompressor | P2 | pending |
| Name table recovered from the dump (`data/`) | P2 | pending |
| Unpacker + inventory | P2 | pending |
| Format decoders (textures, bundles, audio) | after M1 | not started |

Rule: this directory never contains game data — only code, and names/hashes/sizes
derived from the owner's dump.
