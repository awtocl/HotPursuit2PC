# Dump analysis — Need for Speed: Hot Pursuit 2 (PS2, NTSC-U, SLUS-20362)

Every statement here was produced by inspecting the owner's dump at
`ROM/Need for Speed - Hot Pursuit 2 (USA)/` (read-only, C2) on 2026-11-11.
Reproduce with the commands in each section. Anything not proven is marked
**UNVERIFIED**.

Evidence generators (dev-only, standard-library Python):

```
python -B tools/recon/zrecon.py "ROM/Need for Speed - Hot Pursuit 2 (USA)" --deep
python -B tools/recon/lzc.py    "ROM/Need for Speed - Hot Pursuit 2 (USA)"
```

## 1. Disc layout — and corrections to brief §4

27 files (the brief said 25). SHA-256 of each file is in
`tests/fixtures/slus20362_rom_manifest.tsv`.

| Path | Size (bytes) | Notes |
|---|---:|---|
| `SYSTEM.CNF` | 57 | `BOOT2 = cdrom0:\SLUS_203.62;1` / `VER = 1.00` / `VMODE = NTSC` (CRLF line ends) |
| `SLUS_203.62` | 2,321,608 | game ELF |
| `NFSHP2/ZDIR.BIN` | 4,860 | directory of the Z fileset |
| `NFSHP2/ZZDATA.BIN` | 3,116,437,504 | Z fileset data (1,521,698 sectors of 2,048 bytes, no remainder) |
| `NFSHP2/IRX243/` | 12 files | `IOPRP243.IMG`, `LGDEV`, `LIBSD`, `MCMAN`, `MCSERV`, `MTAPMAN`, `PADMAN`, `SDRDRV`, `SIO2MAN`, `STREAM`, `USBD` `.IRX`, **`VSSVER.SCC`** (208 B) |
| `NFSHP2/IRX253/` | 11 files | `IOPRP253.IMG` + the same ten `.IRX` names (different builds; only `LGDEV.IRX` is byte-identical in both sets) |

Corrections to the brief / earlier note:

| Brief §4 said | Verified |
|---|---|
| 25 files | **27 files** |
| `VSSVER.SCC` is not present | **present**: `NFSHP2/IRX243/VSSVER.SCC` (a Visual SourceSafe leftover) |
| Disc `SLUS-2036` | serial is **SLUS-20362** (boot file `SLUS_203.62`) |
| ELF magic `7F 45 4C 46 01 01 01` | confirmed |
| ZDIR needs "a hash→name list" | names are recoverable from the dump itself (§4) — no external list needed |

## 2. `SLUS_203.62` (ELF)

| Field | Value |
|---|---|
| `e_ident` | `7F 45 4C 46 01 01 01 00…` — ELF32, little-endian |
| `e_type` / `e_machine` | 2 (EXEC) / 8 (MIPS) |
| `e_entry` | `0x00100008` |
| `e_flags` | `0x20924001` |
| Program headers | 2 × `PT_LOAD`: `0x00100000` (file 0x1AA9A0, R+X) and `0x002AB000` (file 0x8741B, mem 0x928FC, R+W) |
| Sections | 25; `.text` 0x1A5904 bytes @ `0x00100000`, `.vutext` 0x5090 @ `0x002A5910` (VU microcode), `.rodata`, `.data`, `.sdata`, `.sbss`, `.bss`, `.lit4`, `.gcc_except_table`, 8 `.DVP.overlay*` sections, empty `.mdebug.eabi64` |
| Symbols | **none** (no `.symtab`) |

The ELF contains its own file-name strings (literals and `printf` templates),
source paths (`..\indep\src\*.cpp`, `bWare/inc/bWare.hpp`) and the module load
list (`NFSHP2\IRX243\`, `NFSHP2\IRX253\`, `IOPRP243.IMG`, `IOPRP253.IMG`, the ten IRX names).
Engine lineage per those strings: EA Black Box ("bWare", "BBGLOGO"). Internal
project name "NFS6" (`NFS6.ICO`). **UNVERIFIED:** which IRX set is loaded when.

## 3. Z fileset (`ZDIR.BIN` + `ZZDATA.BIN`)

### 3.1 `ZDIR.BIN` record layout — verified

`4860 = 405 × 12`. No header, no footer. Each record, little-endian:

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | hash of the file name (§3.2) |
| 4 | u32 | start **sector** in `ZZDATA.BIN` (× 2048 = byte offset) |
| 8 | u32 | size in **bytes** |

Evidence:

- Record 1 = `36 9B 93 4C | 00 00 00 00 | C6 C0 04 00` → sector 0, 311,494 bytes;
  `ceil(311494 / 2048) = 153`; record 2 starts at sector `0x99` = 153. Holds for every record:
- Sorted by sector: yes. Distinct non-empty extents: 372. Overlaps: 0. Gaps: 0.
  The last extent ends at sector 1,521,698 = exactly the size of `ZZDATA.BIN`.
  Sum of record sizes 3,116,078,044 bytes (the rest is sector padding).
- 33 records have size 0 (placeholders: `GLOBAL\GLOBALA.BUN`, `FRONTEND\FRONTA.BUN`,
  `GLOBAL\INGAMEA.BUN`, 30 × `TRACKS\TRACKA%d.BUN`).
- 402 distinct hashes: three files are stored twice at different sectors with
  identical size and content hash (`GLOBAL\HUDTEX.BIN`, `CARS\TEXTURES.BIN`,
  `SOUND\SPEECH\EVENTS.EVT`) — placed near the data that is loaded with them.
- Disc order is load order for the boot/front-end files, then alphabetical by
  full path inside each group (`CARS\`, `SOUND\`, `MOVIES\`, …).

### 3.2 Name hash — verified

```
h = 0xFFFFFFFF
for each byte b of UPPERCASE(path with '\' separators):  h = (h * 33 + b) mod 2^32
```

Evidence: 45 literal path strings in the ELF hash to ZDIR entries
(e.g. `GLOBAL\GLOBALA.BUN` → `4C815791`, the first record); numbered siblings
differ by exactly 33⁴ (`TRACKS\TRACKA11.BUN`/`…12.BUN`), `.BUN`→`.LZC` by 0x2B24,
`.MSB`→`.MSH` by 6. Other variants tested (seed 0, seed 5381, xor form,
case-preserving) match nothing.

### 3.3 `COMP` / `.LZC` compression — verified

| Offset | Type | Meaning |
|---|---|---|
| 0 | char[4] | `COMP` |
| 4 | u32 | `0x00001001` |
| 8 | u32 | unpacked size |
| 12 | u32 | packed size (= file size, header included) |
| 16 | … | LZSS stream |

Stream: read a u16 LE flag word and consume its 16 bits LSB-first.
Bit 0 → one literal byte. Bit 1 → two bytes `b0 b1`:
`length = (b0 & 0x0F) + 3`, `distance = ((b0 & 0xF0) << 4) | b1`, copy from
`distance` bytes back in the output. Stop at the unpacked size.

Evidence: 11 files exist on the disc both packed and raw
(`GLOBAL\GLOBALB`, `FRONTEND\FRONTB`, `GLOBAL\INGAMEB`, `TRACKS\TRACKB{24,25,26,44,45,46,65,66}` as `.LZC` and `.BUN`).
`tools/recon/lzc.py` decompresses every `.LZC` of those pairs to a byte-identical
copy of its `.BUN` ("ALL PAIRS MATCH"). All 81 `COMP` files on the disc decompress
without error.

## 4. File names — recovered from the dump (C1)

A name is accepted only if its hash is in `ZDIR.BIN`; because 2³² is small,
names are tiered by how small the candidate set was
(`tools/recon/zrecon.py --deep`, 50 s):

| Tier | Source of the candidate | Names | Expected chance matches |
|---|---|---:|---|
| A | complete path is a literal string in the ELF or in a data file (incl. tails of embedded build paths such as `\Psx2\CD\GLOBAL\CHARACTERSA.BIN`), or an ELF template with a number (`TRACKS\TEX%dTRACK.BIN`) | 273 | 0.0009 |
| B | ELF template with one `%s` (`NIS\%s.bin`, `REPLAY\%s`, `Sound\Speech\%sBAN.pck`) filled with a clean token from the ELF/data | 31 | ≈ 0.01 |
| C | known directory + clean token + known extension; **kept only when a content check passes** | 53 | ≈ 6.6 before checks; survivors are 26 `.MSH`/`.MSB` sibling pairs (two independent hash hits each, `.MSH` first word = its own size) and `SOUND\JUKEBOX.JBC` (same header as `JUKEBOXM.JBC`) |
| D | guessed pattern, confirmed by siblings: `MARKERS\TRACK{11,12,21,22,32}.TXT` (five independent hits of one pattern) | 5 | negligible |

**362 of 402** distinct hashes are named. Unnamed (kept as `_UNNAMED\<HASH>.<ext>`):

| Count | What | Why unnamed |
|---:|---|---|
| 31 | replay files (same header as the five named `REPLAY\JUL23\T*.BIN`) | the ELF references only five replays by name; **UNVERIFIED** whether the game can load the others |
| 4 | RIFF/AVI movies (`5D50EC33`, `29BA024B`, `6693FD6C`, `39514A5E`) | names not present as strings (embedded stream names: `event_tree_flag_256.avi`, `desat.avi`) |
| 2 | `E2F0CC8A` (6.0 MB), `68645B6E` — same header, between the engine sound banks and speech | **UNVERIFIED** type (sound data?) |
| 1 | `CEA9F930` (207,280 B) between `GERMANDAT.PCK` and `ITALIANBAN.PCK` | **UNVERIFIED** |
| 2 | `48671A56` (130 B of hex text), `313E6330` ("Hello.") | developer leftovers |

Caveat recorded for honesty: an earlier, looser version of the search (1.9 M
junk-laden tokens) produced plausible-looking collisions such as
`TRACKS\4%%%.BIN`; that is why tiers and content checks exist.

## 5. Full asset inventory

Generated in phase P2 by `hp2pc inventory` → section appended below and
`docs/asset-inventory.tsv`.
