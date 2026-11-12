# ADR 0004 — Asset pipeline: unpacking, validating and storing the Z fileset

- **Status:** Accepted (2026-11-11, phase P2)
- **Constraints in play:** C1, C2, C4, C5, C6; brief §3.6 (idempotency)

## Context

All game content is in `NFSHP2/ZZDATA.BIN` (3.1 GB), indexed by `NFSHP2/ZDIR.BIN`,
which stores a 32-bit name hash, a sector and a byte size per file — no names.
The formats were decoded from the dump in P0 (`docs/dump-analysis.md` §3–4).
Decisions needed: where file names come from, what "unpacked" means on disk,
how a re-run behaves, and how the result is validated.

## Options considered

### Where names come from

| Option | Verdict |
|---|---|
| **A. Ship a name table recovered from the owner's dump**, checked against ZDIR hashes | **chosen** |
| B. Reuse the name list inside Ekey/NFS.BIN.Tool | rejected — no license (C5), and not needed |
| C. Re-run the recovery (string mining) inside the converter on every conversion | rejected — 50 s of heuristics in the user's path, non-deterministic by design; the result for a given disc is constant |
| D. No names, hash-only file tree | rejected — unusable for humans and for later format work |

### What is written to disk

| Option | Verdict |
|---|---|
| **A. 1:1 byte copies of each file under its recovered path**, compressed files stay compressed; a manifest records hash, sector, size, SHA-256, kind | **chosen** |
| B. Decompress/convert during unpack | rejected for this stage — mixes container extraction with format translation; makes verification against the disc indirect. Decoders are separate, later stages. |
| C. No unpack — read `ZZDATA.BIN` in place through an index | kept as a *library capability* (`ZFileset::read`), because the emulator-backed M1 build needs the original blob anyway; the unpacked tree is for inspection, tooling and the native track |

## Decision

1. **Names:** `assets-pipeline/data/hp2_slus20362_names.txt` holds the names recovered by
   `tools/recon/zrecon.py` with their confidence tier. It is compiled into the
   library. A test asserts every entry hashes to the stored value, and the dump
   test asserts every entry is present in the real `ZDIR.BIN`. Files without a
   recovered name are extracted as `_UNNAMED\<HASH8>.<EXT>` (extension from
   content sniffing) — nothing is dropped.
2. **Unpack = exact extents.** For each record, bytes `[sector*2048, +size)` are
   copied unchanged. Zero-size records become empty files. A hash that appears
   in several records is written once; the other extents are compared and
   flagged in the manifest (`dup-of`).
3. **Validation before extraction:** record size, ordering, extents inside
   `ZZDATA.BIN`, overlaps and gaps are checked; out-of-range or overlapping
   extents are fatal with the record index in the message.
4. **Resume and safety (brief §3.6):** every file is written as `<name>.part`
   and renamed when complete, so a file with its final name is complete by
   construction. On a re-run, an existing file of the right size is skipped
   (or re-hashed with `verify`). An existing file of the *wrong* size is never
   overwritten silently: it is an error unless `repair` is requested, in which
   case it is moved aside as `<name>.mismatch-N` and re-extracted.
5. **Manifest:** `manifest.tsv` (index, hash, sector, size, sha256, kind, flags,
   path) is written atomically at the end and is the input for later stages
   and for the inventory in `docs/`.
6. **The ROM is opened with read access only** (`GENERIC_READ`, share-read).
7. **Kinds** (texture pack, bundle, sound bank, …) are a classification by
   name and magic bytes. Internal formats other than LZC are **UNVERIFIED**
   until a decoder with tests exists.

## Consequences

- Deterministic, fast (one sequential pass over 3 GB), and verifiable: the
  SHA-256 of every extracted file is in the manifest and in the committed
  inventory, so any two dumps of the same disc can be compared.
- The name table is specific to SLUS-20362 v1.00. Another region/revision
  needs its own recovery run (the tool exists) — the unpacker still works
  without names.
- Reference knowledge from Ekey's tool is limited to its README (the "old
  format" flag exists); no code or data was taken (C5).
- A second, independent implementation (Python, `tools/recon/`) exists for
  cross-checking the C++ one.

## Confidence

High: container layout, hash and LZC are verified byte-for-byte on the dump.
Medium: tier C/D names (58 of 362) rest on sibling/content confirmation rather
than literal strings — documented per name in the data file.

## What would reopen this ADR

- A second disc revision whose ZDIR has a different record layout.
- Evidence that a recovered name is wrong (e.g. the game requests a different
  name at runtime — observable once the game runs under the M1 launcher).
- A need to repack (`ZZDATA.BIN` writer) for the native track.
