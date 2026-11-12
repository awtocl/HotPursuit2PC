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
`converter/data/slus20362_rom_manifest.tsv` (compiled into the converter as the
reference for ROM validation).

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
| C | known directory + clean token + known extension; **accepted only with independent confirmation** (a sibling that is a second hash hit, or a rare matching magic) | 53 | ≈ 6.6 before confirmation; survivors are 26 `.MSH`/`.MSB` sibling pairs (two independent hash hits each, `.MSH` first word = its own size) and `SOUND\JUKEBOX.JBC` (same 8-byte header as `JUKEBOXM.JBC`, which only those two files have) |
| D | guessed numbered pattern, accepted only when ≥ 2 files fit it: `MARKERS\TRACK{11,12,21,22,32}.TXT` (five independent hits of one pattern) | 5 | negligible |

**362 of 402** distinct hashes are named; the table is
`assets-pipeline/data/hp2_slus20362_names.txt` and two runs of the tool produce
an identical file. Unnamed (kept as `_UNNAMED\<HASH>.<EXT>`):

| Count | What | Why unnamed |
|---:|---|---|
| 31 | replay files (same header as the five named `REPLAY\JUL23\T*.BIN`) | the ELF references only five replays by name; **UNVERIFIED** whether the game can load the others |
| 4 | RIFF/AVI movies (`5D50EC33`, `29BA024B`, `6693FD6C`, `39514A5E`) | names not present as strings (embedded stream names: `event_tree_flag_256.avi`, `desat.avi`) |
| 2 | `E2F0CC8A` (6.0 MB), `68645B6E` — same header, between the engine sound banks and speech | **UNVERIFIED** type (sound data?) |
| 1 | `CEA9F930` (207,280 B) between `GERMANDAT.PCK` and `ITALIANBAN.PCK` | **UNVERIFIED** |
| 2 | `48671A56` (130 B of hex text), `313E6330` ("Hello.") | developer leftovers |

Two mistakes made and corrected on the way — recorded because they show why a
hash match alone proves nothing:

1. A first, loose search (1.9 M junk-laden tokens) produced plausible-looking
   collisions such as `TRACKS\4%%%.BIN`. → tiers, clean NUL-terminated tokens.
2. With tiers in place, tier C still accepted `.BIN` candidates on a weak check;
   a chance match `CARS\F50\NU8.BIN` took the hash of `SOUND\ALPINE.MSB`. It
   was caught by the C++ inventory (26 bank headers but 25 bodies).
   → tier C/D now require independent confirmation, and candidates are tried
   in sorted order so the result is deterministic.

The dump test `dump_content_matches_the_name_extension` re-checks, in C++ and
independently of the recovery tool, that every `.LZC` is a `COMP` stream, every
`.AVI` is RIFF, and every `.MSH` starts with its own size and has its `.MSB`.

## 5. Full asset inventory

Evidence chain for this section:

| Step | Command | Result (2026-11-11) |
|---|---|---|
| C++ inventory with SHA-256 | `hp2assets inventory <rom> --sha256 --out tests/fixtures/slus20362_zfileset_inventory.tsv` | 405 records |
| Independent re-hash in Python | `python -B tools/recon/crosscheck_inventory.py <rom> <tsv>` | "405 records compared, 0 differences" |
| Full unpack | `hp2assets unpack <rom> <out>` | written 402, duplicate records 3, 2,955.2 MB |
| Re-run (resume) | same command | written 0, already present 402 |
| Re-run with `--verify` | re-hashes every extracted file against the disc | written 0, already present 402, repaired 0 |
| Extracted tree vs. manifest | file count and byte total counted independently in PowerShell | 402 files, 3,098,727,312 bytes — equal to the sum of distinct record sizes |
| Test-suite | `pwsh tools/build.ps1` | `test_assets`, `test_dump`, `selftest` pass |

<!-- inventory:begin -->
405 directory records = 402 distinct files, 3,098,727,312 bytes (duplicates counted once). Source: `tests/fixtures/slus20362_zfileset_inventory.tsv`, which also holds the SHA-256 of every file. *Type* is a classification by name and magic bytes; internal formats other than LZC are **UNVERIFIED** (ADR 0004).

### 5.1 By type

| Type | Files | Bytes | Meaning |
|---|---:|---:|---|
| `bundle` | 12 | 100,713,444 | chunked bundle (`.BUN`, some `.BIN`) |
| `bundle-lzc` | 81 | 188,439,924 | LZC-compressed bundle (`COMP`) |
| `car-geometry` | 47 | 16,247,424 | car geometry bundle |
| `engine-sound-table` | 2 | 13,008 | engine sound table |
| `movie` | 21 | 681,847,296 | RIFF/AVI movie |
| `music` | 2 | 385,552,384 | jukebox music container |
| `nis-camera` | 8 | 183,952 | cut-scene camera track |
| `nis-scene` | 8 | 796,864 | cut-scene data |
| `placeholder` | 33 | 0 | empty placeholder entry |
| `replay` | 36 | 4,657,764 | recorded replay |
| `save-icon` | 1 | 61,048 | memory card save icon |
| `sound-bank-body` | 26 | 5,993,808 | sound bank sample data |
| `sound-bank-header` | 26 | 22,208 | sound bank header |
| `speech-events` | 1 | 48,092 | speech event table |
| `speech-pack` | 10 | 1,515,154,413 | speech pack |
| `strings` | 8 | 857,660 | localised string table |
| `text` | 7 | 20,647 | text (developer notes) |
| `texture-pack` | 65 | 191,822,568 | texture pack |
| `track-markers` | 5 | 4,560 | track marker table |
| `unknown` | 3 | 6,290,248 | unidentified |

### 5.2 By top-level folder

| Folder | Files | Bytes |
|---|---:|---:|
| `(root)` | 1 | 61,048 |
| `CARS\` | 95 | 42,273,597 |
| `FRONTEND\` | 4 | 41,508,062 |
| `GLOBAL\` | 11 | 8,966,253 |
| `LANGUAGES\` | 8 | 857,660 |
| `MARKERS\` | 10 | 25,067 |
| `MOVIES\` | 17 | 638,645,248 |
| `NIS\` | 16 | 980,816 |
| `REPLAY\` | 5 | 547,280 |
| `SOUND\` | 67 | 1,906,783,913 |
| `TRACKS\` | 128 | 404,475,448 |
| `_UNNAMED\` | 40 | 53,602,920 |

### 5.3 Every record, in disc order

Tier = name confidence (§4); `-` = no recovered name. Flags: `empty`, `dup-of=N` (same file stored again; identical bytes), `unnamed`.

| # | Hash | Sector | Size | Tier | Type | Flags | Name |
|---:|---|---:|---:|:-:|---|---|---|
| 0 | `4C815791` | 0 | 0 | A | placeholder | empty | `GLOBAL\GLOBALA.BUN` |
| 1 | `4C939B36` | 0 | 311,494 | A | bundle-lzc |  | `GLOBAL\GLOBALB.LZC` |
| 2 | `29EE288F` | 153 | 2,684,984 | A | texture-pack |  | `GLOBAL\DYNTEX.BIN` |
| 3 | `0E1049ED` | 1465 | 107,576 | A | strings |  | `LANGUAGES\FE_ENGLISH.BIN` |
| 4 | `0CE74979` | 1518 | 116,828 | A | strings |  | `LANGUAGES\FE_FRENCH.BIN` |
| 5 | `D4BFF1FD` | 1576 | 115,268 | A | strings |  | `LANGUAGES\FE_GERMAN.BIN` |
| 6 | `39164870` | 1633 | 55,360 | A | strings |  | `LANGUAGES\FE_ID.BIN` |
| 7 | `57479765` | 1661 | 115,844 | A | strings |  | `LANGUAGES\FE_ITALIAN.BIN` |
| 8 | `16B1F17F` | 1718 | 124,456 | A | strings |  | `LANGUAGES\FE_LONGEST.BIN` |
| 9 | `7ED05979` | 1779 | 114,544 | A | strings |  | `LANGUAGES\FE_SPANISH.BIN` |
| 10 | `3279C49A` | 1835 | 107,784 | A | strings |  | `LANGUAGES\FE_SWEDISH.BIN` |
| 11 | `2D945845` | 1888 | 1,540,664 | A | texture-pack |  | `GLOBAL\HUDTEX.BIN` |
| 12 | `0287BF4F` | 2641 | 15,761,976 | A | texture-pack |  | `CARS\TEXTURES.BIN` |
| 13 | `84CBF2FD` | 10338 | 75,744 | C | sound-bank-body |  | `SOUND\COMMON.MSB` |
| 14 | `84CBF303` | 10375 | 768 | C | sound-bank-header |  | `SOUND\COMMON.MSH` |
| 15 | `B9819E5A` | 10376 | 2,304 | C | sound-bank-header |  | `SOUND\FRONTEND.MSH` |
| 16 | `B9819E54` | 10378 | 1,052,128 | C | sound-bank-body |  | `SOUND\FRONTEND.MSB` |
| 17 | `14B36198` | 10892 | 0 | A | placeholder | empty | `FRONTEND\FRONTA.BUN` |
| 18 | `28DB8B0A` | 10892 | 48,092 | A | speech-events |  | `SOUND\SPEECH\EVENTS.EVT` |
| 19 | `14C5A53D` | 10916 | 6,482,490 | A | bundle-lzc |  | `FRONTEND\FRONTB.LZC` |
| 20 | `5986D8A5` | 14082 | 61,048 | A | save-icon |  | `NFS6.ICO` |
| 21 | `14C57A19` | 14112 | 14,385,132 | A | bundle |  | `FRONTEND\FRONTB.BUN` |
| 22 | `198117D6` | 21136 | 20,640,440 | A | texture-pack |  | `FRONTEND\TSELECT.BIN` |
| 23 | `74BF07B7` | 31215 | 157,608 | - | replay | unnamed | `_UNNAMED\74BF07B7.BIN` |
| 24 | `8F054266` | 31292 | 132,028 | - | replay | unnamed | `_UNNAMED\8F054266.BIN` |
| 25 | `F5E49C3A` | 31357 | 152,864 | - | replay | unnamed | `_UNNAMED\F5E49C3A.BIN` |
| 26 | `989795F7` | 31432 | 200,084 | - | replay | unnamed | `_UNNAMED\989795F7.BIN` |
| 27 | `B58FEC03` | 31530 | 83,852 | - | replay | unnamed | `_UNNAMED\B58FEC03.BIN` |
| 28 | `D056F360` | 31571 | 217,308 | - | replay | unnamed | `_UNNAMED\D056F360.BIN` |
| 29 | `E2AEF28D` | 31678 | 235,884 | - | replay | unnamed | `_UNNAMED\E2AEF28D.BIN` |
| 30 | `009F3BB9` | 31794 | 167,352 | - | replay | unnamed | `_UNNAMED\009F3BB9.BIN` |
| 31 | `EB6B5DBE` | 31876 | 149,804 | - | replay | unnamed | `_UNNAMED\EB6B5DBE.BIN` |
| 32 | `01789B2F` | 31950 | 194,932 | - | replay | unnamed | `_UNNAMED\01789B2F.BIN` |
| 33 | `F7887AC3` | 32046 | 145,088 | - | replay | unnamed | `_UNNAMED\F7887AC3.BIN` |
| 34 | `E0896A88` | 32117 | 69,316 | - | replay | unnamed | `_UNNAMED\E0896A88.BIN` |
| 35 | `7DBD9453` | 32151 | 188,252 | - | replay | unnamed | `_UNNAMED\7DBD9453.BIN` |
| 36 | `E7740B18` | 32243 | 119,552 | - | replay | unnamed | `_UNNAMED\E7740B18.BIN` |
| 37 | `7C125FAB` | 32302 | 64,220 | B | replay |  | `REPLAY\JUL23\T11BLUEELISE.BIN` |
| 38 | `1FD78955` | 32334 | 93,340 | B | replay |  | `REPLAY\JUL23\T12YELLOWSPIDER.BIN` |
| 39 | `7FC267DA` | 32380 | 147,656 | - | replay | unnamed | `_UNNAMED\7FC267DA.BIN` |
| 40 | `8DD706CA` | 32453 | 149,464 | B | replay |  | `REPLAY\JUL23\T33GOLDVANQUISH.BIN` |
| 41 | `84CB0F49` | 32526 | 123,612 | B | replay |  | `REPLAY\JUL23\T43GREENM5.BIN` |
| 42 | `87923363` | 32587 | 116,644 | B | replay |  | `REPLAY\JUL23\T62ORANGEF1LM.BIN` |
| 43 | `80403A0F` | 32644 | 135,728 | - | replay | unnamed | `_UNNAMED\80403A0F.BIN` |
| 44 | `82CE9D25` | 32711 | 94,260 | - | replay | unnamed | `_UNNAMED\82CE9D25.BIN` |
| 45 | `0FCA2BDA` | 32758 | 95,608 | - | replay | unnamed | `_UNNAMED\0FCA2BDA.BIN` |
| 46 | `8EEB9E1E` | 32805 | 62,288 | - | replay | unnamed | `_UNNAMED\8EEB9E1E.BIN` |
| 47 | `B6E921DC` | 32836 | 62,132 | - | replay | unnamed | `_UNNAMED\B6E921DC.BIN` |
| 48 | `7A3DDAA4` | 32867 | 63,276 | - | replay | unnamed | `_UNNAMED\7A3DDAA4.BIN` |
| 49 | `9F3967BC` | 32898 | 78,872 | - | replay | unnamed | `_UNNAMED\9F3967BC.BIN` |
| 50 | `479FC593` | 32937 | 66,064 | - | replay | unnamed | `_UNNAMED\479FC593.BIN` |
| 51 | `D8D03B93` | 32970 | 157,960 | - | replay | unnamed | `_UNNAMED\D8D03B93.BIN` |
| 52 | `4C36BE13` | 33048 | 62,384 | - | replay | unnamed | `_UNNAMED\4C36BE13.BIN` |
| 53 | `A421A3AF` | 33079 | 99,164 | - | replay | unnamed | `_UNNAMED\A421A3AF.BIN` |
| 54 | `99A3EFB4` | 33128 | 198,136 | - | replay | unnamed | `_UNNAMED\99A3EFB4.BIN` |
| 55 | `E4B891E5` | 33225 | 146,392 | - | replay | unnamed | `_UNNAMED\E4B891E5.BIN` |
| 56 | `9B4470A7` | 33297 | 160,756 | - | replay | unnamed | `_UNNAMED\9B4470A7.BIN` |
| 57 | `EDD531AC` | 33376 | 105,444 | - | replay | unnamed | `_UNNAMED\EDD531AC.BIN` |
| 58 | `D60A2FC8` | 33428 | 160,440 | - | replay | unnamed | `_UNNAMED\D60A2FC8.BIN` |
| 59 | `5A914411` | 33507 | 0 | A | placeholder | empty | `GLOBAL\INGAMEA.BUN` |
| 60 | `5AA35C92` | 33507 | 1,983,296 | A | bundle |  | `GLOBAL\INGAMEB.BUN` |
| 61 | `5AA387B6` | 34476 | 1,001,750 | A | bundle-lzc |  | `GLOBAL\INGAMEB.LZC` |
| 62 | `0287BF4F` | 34966 | 15,761,976 | A | texture-pack | dup-of=12 | `CARS\TEXTURES.BIN` |
| 63 | `508439C9` | 42663 | 51,472 | A | car-geometry |  | `CARS\4DR_SEDAN02\GEOMETRY.BIN` |
| 64 | `50846679` | 42689 | 32,937 | A | bundle-lzc |  | `CARS\4DR_SEDAN02\GEOMETRY.LZC` |
| 65 | `1F43DE27` | 42706 | 52,432 | A | car-geometry |  | `CARS\4DR_SEDAN\GEOMETRY.BIN` |
| 66 | `1F440AD7` | 42732 | 34,016 | A | bundle-lzc |  | `CARS\4DR_SEDAN\GEOMETRY.LZC` |
| 67 | `89C70F1A` | 42749 | 529,472 | A | car-geometry |  | `CARS\911TURBO\GEOMETRY.BIN` |
| 68 | `89C73BCA` | 43008 | 334,620 | A | bundle-lzc |  | `CARS\911TURBO\GEOMETRY.LZC` |
| 69 | `FDDB6AFB` | 43172 | 47,632 | A | car-geometry |  | `CARS\AMBULANCE\GEOMETRY.BIN` |
| 70 | `FDDB97AB` | 43196 | 28,904 | A | bundle-lzc |  | `CARS\AMBULANCE\GEOMETRY.LZC` |
| 71 | `2D7F8FFD` | 43211 | 281,568 | A | car-geometry |  | `CARS\BARCHETTA\DASHGEOM.BIN` |
| 72 | `2D7FBCAD` | 43349 | 165,937 | A | bundle-lzc |  | `CARS\BARCHETTA\DASHGEOM.LZC` |
| 73 | `BE0EF321` | 43431 | 545,152 | A | car-geometry |  | `CARS\BARCHETTA\GEOMETRY.BIN` |
| 74 | `BE0F1FD1` | 43698 | 343,220 | A | bundle-lzc |  | `CARS\BARCHETTA\GEOMETRY.LZC` |
| 75 | `8826CBDB` | 43866 | 492,592 | A | car-geometry |  | `CARS\BMWM5\GEOMETRY.BIN` |
| 76 | `8826F88B` | 44107 | 314,273 | A | bundle-lzc |  | `CARS\BMWM5\GEOMETRY.LZC` |
| 77 | `E8DC831D` | 44261 | 471,696 | A | car-geometry |  | `CARS\BMWM5COP\GEOMETRY.BIN` |
| 78 | `E8DCAFCD` | 44492 | 286,503 | A | bundle-lzc |  | `CARS\BMWM5COP\GEOMETRY.LZC` |
| 79 | `84880B8B` | 44632 | 467,856 | A | car-geometry |  | `CARS\BMWZ8\GEOMETRY.BIN` |
| 80 | `8488383B` | 44861 | 302,460 | A | bundle-lzc |  | `CARS\BMWZ8\GEOMETRY.LZC` |
| 81 | `D3331F7D` | 45009 | 55,552 | A | car-geometry |  | `CARS\BUS\GEOMETRY.BIN` |
| 82 | `D3334C2D` | 45037 | 32,617 | A | bundle-lzc |  | `CARS\BUS\GEOMETRY.LZC` |
| 83 | `6B8733AE` | 45053 | 505,616 | A | car-geometry |  | `CARS\CARRERAGT\GEOMETRY.BIN` |
| 84 | `6B87605E` | 45300 | 326,795 | A | bundle-lzc |  | `CARS\CARRERAGT\GEOMETRY.LZC` |
| 85 | `22A39416` | 45460 | 528,432 | A | car-geometry |  | `CARS\CHALLENGE\GEOMETRY.BIN` |
| 86 | `22A3C0C6` | 45719 | 332,108 | A | bundle-lzc |  | `CARS\CHALLENGE\GEOMETRY.LZC` |
| 87 | `23A263C1` | 45882 | 524,752 | A | car-geometry |  | `CARS\CL55AMG\GEOMETRY.BIN` |
| 88 | `23A29071` | 46139 | 338,337 | A | bundle-lzc |  | `CARS\CL55AMG\GEOMETRY.LZC` |
| 89 | `FD3DC439` | 46305 | 572,256 | A | car-geometry |  | `CARS\CLK_GTR\GEOMETRY.BIN` |
| 90 | `FD3DF0E9` | 46585 | 367,905 | A | bundle-lzc |  | `CARS\CLK_GTR\GEOMETRY.LZC` |
| 91 | `5E835D1F` | 46765 | 419,696 | A | car-geometry |  | `CARS\CORVETTE\GEOMETRY.BIN` |
| 92 | `5E8389CF` | 46970 | 263,900 | A | bundle-lzc |  | `CARS\CORVETTE\GEOMETRY.LZC` |
| 93 | `C160BDE1` | 47099 | 452,288 | A | car-geometry |  | `CARS\CORVETTECOP\GEOMETRY.BIN` |
| 94 | `C160EA91` | 47320 | 283,865 | A | bundle-lzc |  | `CARS\CORVETTECOP\GEOMETRY.LZC` |
| 95 | `2160498F` | 47459 | 56,672 | A | car-geometry |  | `CARS\COUPE\GEOMETRY.BIN` |
| 96 | `2160763F` | 47487 | 36,392 | A | bundle-lzc |  | `CARS\COUPE\GEOMETRY.LZC` |
| 97 | `6600EADE` | 47505 | 486,736 | A | car-geometry |  | `CARS\CROWNVIC\GEOMETRY.BIN` |
| 98 | `6601178E` | 47743 | 301,015 | A | bundle-lzc |  | `CARS\CROWNVIC\GEOMETRY.LZC` |
| 99 | `CA52719E` | 47890 | 527,872 | A | car-geometry |  | `CARS\DIABLO\GEOMETRY.BIN` |
| 100 | `CA529E4E` | 48148 | 343,767 | A | bundle-lzc |  | `CARS\DIABLO\GEOMETRY.LZC` |
| 101 | `9C576A85` | 48316 | 538,624 | A | car-geometry |  | `CARS\ELISE\GEOMETRY.BIN` |
| 102 | `9C579735` | 48579 | 343,990 | A | bundle-lzc |  | `CARS\ELISE\GEOMETRY.LZC` |
| 103 | `F0C2BABA` | 48747 | 153,024 | A | car-geometry |  | `CARS\F50\DASHGEOM.BIN` |
| 104 | `F0C2E76A` | 48822 | 92,638 | A | bundle-lzc |  | `CARS\F50\DASHGEOM.LZC` |
| 105 | `81521DDE` | 48868 | 565,168 | A | car-geometry |  | `CARS\F50\GEOMETRY.BIN` |
| 106 | `81524A8E` | 49144 | 362,157 | A | bundle-lzc |  | `CARS\F50\GEOMETRY.LZC` |
| 107 | `95EFEE22` | 49321 | 56,656 | A | car-geometry |  | `CARS\FIRETRUCK\GEOMETRY.BIN` |
| 108 | `95F01AD2` | 49349 | 33,893 | A | bundle-lzc |  | `CARS\FIRETRUCK\GEOMETRY.LZC` |
| 109 | `88A1914E` | 49366 | 51,808 | A | car-geometry |  | `CARS\HATCHBACK02\GEOMETRY.BIN` |
| 110 | `88A1BDFE` | 49392 | 32,435 | A | bundle-lzc |  | `CARS\HATCHBACK02\GEOMETRY.LZC` |
| 111 | `DEAB906C` | 49408 | 49,360 | A | car-geometry |  | `CARS\HATCHBACK\GEOMETRY.BIN` |
| 112 | `DEABBD1C` | 49433 | 31,900 | A | bundle-lzc |  | `CARS\HATCHBACK\GEOMETRY.LZC` |
| 113 | `D7D39F4D` | 49449 | 436,704 | A | car-geometry |  | `CARS\HOLDEN\GEOMETRY.BIN` |
| 114 | `D7D3CBFD` | 49663 | 278,841 | A | bundle-lzc |  | `CARS\HOLDEN\GEOMETRY.LZC` |
| 115 | `857BD76D` | 49800 | 609,984 | A | car-geometry |  | `CARS\JAGUAR\GEOMETRY.BIN` |
| 116 | `857C041D` | 50098 | 394,426 | A | bundle-lzc |  | `CARS\JAGUAR\GEOMETRY.LZC` |
| 117 | `482D9DC0` | 50291 | 548,448 | A | car-geometry |  | `CARS\LAMBOCOP\GEOMETRY.BIN` |
| 118 | `482DCA70` | 50559 | 334,166 | A | bundle-lzc |  | `CARS\LAMBOCOP\GEOMETRY.LZC` |
| 119 | `7FBDED95` | 50723 | 475,408 | A | car-geometry |  | `CARS\MCLAREN\GEOMETRY.BIN` |
| 120 | `7FBE1A45` | 50956 | 293,055 | A | bundle-lzc |  | `CARS\MCLAREN\GEOMETRY.LZC` |
| 121 | `D355F1CE` | 51100 | 499,152 | A | car-geometry |  | `CARS\MCLARENLM\GEOMETRY.BIN` |
| 122 | `D3561E7E` | 51344 | 310,599 | A | bundle-lzc |  | `CARS\MCLARENLM\GEOMETRY.LZC` |
| 123 | `BB8E87E5` | 51496 | 58,576 | A | car-geometry |  | `CARS\MINIVAN\GEOMETRY.BIN` |
| 124 | `BB8EB495` | 51525 | 37,827 | A | bundle-lzc |  | `CARS\MINIVAN\GEOMETRY.LZC` |
| 125 | `23042DFB` | 51544 | 515,584 | A | car-geometry |  | `CARS\MURCIELAGO\GEOMETRY.BIN` |
| 126 | `23045AAB` | 51796 | 319,659 | A | bundle-lzc |  | `CARS\MURCIELAGO\GEOMETRY.LZC` |
| 127 | `A7199792` | 51953 | 485,072 | A | car-geometry |  | `CARS\MUSTANG\GEOMETRY.BIN` |
| 128 | `A719C442` | 52190 | 309,619 | A | bundle-lzc |  | `CARS\MUSTANG\GEOMETRY.LZC` |
| 129 | `6A43C774` | 52342 | 511,216 | A | car-geometry |  | `CARS\MUSTANGCOP\GEOMETRY.BIN` |
| 130 | `6A43F424` | 52592 | 318,002 | A | bundle-lzc |  | `CARS\MUSTANGCOP\GEOMETRY.LZC` |
| 131 | `DF823523` | 52748 | 525,136 | A | car-geometry |  | `CARS\OPEL\GEOMETRY.BIN` |
| 132 | `DF8261D3` | 53005 | 333,096 | A | bundle-lzc |  | `CARS\OPEL\GEOMETRY.LZC` |
| 133 | `D27A0A48` | 53168 | 50,944 | A | car-geometry |  | `CARS\PANELVAN\GEOMETRY.BIN` |
| 134 | `D27A36F8` | 53193 | 31,866 | A | bundle-lzc |  | `CARS\PANELVAN\GEOMETRY.LZC` |
| 135 | `398DE32F` | 53209 | 47,696 | A | car-geometry |  | `CARS\PARCELVAN\GEOMETRY.BIN` |
| 136 | `398E0FDF` | 53233 | 27,684 | A | bundle-lzc |  | `CARS\PARCELVAN\GEOMETRY.LZC` |
| 137 | `A0E46AFF` | 53247 | 57,056 | A | car-geometry |  | `CARS\PICKUP\GEOMETRY.BIN` |
| 138 | `A0E497AF` | 53275 | 35,638 | A | bundle-lzc |  | `CARS\PICKUP\GEOMETRY.LZC` |
| 139 | `03300396` | 53293 | 247,456 | A | car-geometry |  | `CARS\SPIDER\DASHGEOM.BIN` |
| 140 | `03303046` | 53414 | 142,470 | A | bundle-lzc |  | `CARS\SPIDER\DASHGEOM.LZC` |
| 141 | `93BF66BA` | 53484 | 537,552 | A | car-geometry |  | `CARS\SPIDER\GEOMETRY.BIN` |
| 142 | `93BF936A` | 53747 | 342,944 | A | bundle-lzc |  | `CARS\SPIDER\GEOMETRY.LZC` |
| 143 | `547B5051` | 53915 | 57,504 | A | car-geometry |  | `CARS\SUV\GEOMETRY.BIN` |
| 144 | `547B7D01` | 53944 | 36,520 | A | bundle-lzc |  | `CARS\SUV\GEOMETRY.LZC` |
| 145 | `5C38938B` | 53962 | 53,648 | A | car-geometry |  | `CARS\TAXI02\GEOMETRY.BIN` |
| 146 | `5C38C03B` | 53989 | 34,319 | A | bundle-lzc |  | `CARS\TAXI02\GEOMETRY.LZC` |
| 147 | `7332DF69` | 54006 | 52,384 | A | car-geometry |  | `CARS\TAXI\GEOMETRY.BIN` |
| 148 | `73330C19` | 54032 | 33,974 | A | bundle-lzc |  | `CARS\TAXI\GEOMETRY.LZC` |
| 149 | `32E5E71F` | 54049 | 515,024 | A | car-geometry |  | `CARS\TS50\GEOMETRY.BIN` |
| 150 | `32E613CF` | 54301 | 334,141 | A | bundle-lzc |  | `CARS\TS50\GEOMETRY.LZC` |
| 151 | `844BE642` | 54465 | 466,208 | A | car-geometry |  | `CARS\VANQUISH\GEOMETRY.BIN` |
| 152 | `844C12F2` | 54693 | 299,169 | A | bundle-lzc |  | `CARS\VANQUISH\GEOMETRY.LZC` |
| 153 | `7C32DE18` | 54840 | 504,016 | A | car-geometry |  | `CARS\VAUXHALL\GEOMETRY.BIN` |
| 154 | `7C330AC8` | 55087 | 318,527 | A | bundle-lzc |  | `CARS\VAUXHALL\GEOMETRY.LZC` |
| 155 | `7C428227` | 55243 | 508,272 | A | car-geometry |  | `CARS\VIPERGTS\GEOMETRY.BIN` |
| 156 | `7C42AED7` | 55492 | 331,071 | A | bundle-lzc |  | `CARS\VIPERGTS\GEOMETRY.LZC` |
| 157 | `0F3999EE` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA11.BUN` |
| 158 | `0F4BB26F` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA12.BUN` |
| 159 | `0F5DCAF0` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA13.BUN` |
| 160 | `0F6FE371` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA14.BUN` |
| 161 | `0F81FBF2` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA15.BUN` |
| 162 | `0F941473` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA16.BUN` |
| 163 | `118EC28F` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA21.BUN` |
| 164 | `11A0DB10` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA22.BUN` |
| 165 | `11B2F391` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA23.BUN` |
| 166 | `11C50C12` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA24.BUN` |
| 167 | `11D72493` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA25.BUN` |
| 168 | `11E93D14` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA26.BUN` |
| 169 | `13E3EB30` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA31.BUN` |
| 170 | `13F603B1` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA32.BUN` |
| 171 | `14081C32` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA33.BUN` |
| 172 | `141A34B3` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA34.BUN` |
| 173 | `142C4D34` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA35.BUN` |
| 174 | `143E65B5` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA36.BUN` |
| 175 | `163913D1` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA41.BUN` |
| 176 | `164B2C52` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA42.BUN` |
| 177 | `165D44D3` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA43.BUN` |
| 178 | `166F5D54` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA44.BUN` |
| 179 | `168175D5` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA45.BUN` |
| 180 | `16938E56` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA46.BUN` |
| 181 | `1AE36513` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA61.BUN` |
| 182 | `1AF57D94` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA62.BUN` |
| 183 | `1B079615` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA63.BUN` |
| 184 | `1B19AE96` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA64.BUN` |
| 185 | `1B2BC717` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA65.BUN` |
| 186 | `1B3DDF98` | 55654 | 0 | A | placeholder | empty | `TRACKS\TRACKA66.BUN` |
| 187 | `5C3401D3` | 55654 | 5,267,410 | A | bundle-lzc |  | `TRACKS\TRACKB11.LZC` |
| 188 | `5C461A54` | 58226 | 5,231,886 | A | bundle-lzc |  | `TRACKS\TRACKB12.LZC` |
| 189 | `5C5832D5` | 60781 | 5,852,261 | A | bundle-lzc |  | `TRACKS\TRACKB13.LZC` |
| 190 | `5C6A4B56` | 63639 | 5,266,384 | A | bundle-lzc |  | `TRACKS\TRACKB14.LZC` |
| 191 | `5C7C63D7` | 66211 | 5,230,862 | A | bundle-lzc |  | `TRACKS\TRACKB15.LZC` |
| 192 | `5C8E7C58` | 68766 | 5,851,284 | A | bundle-lzc |  | `TRACKS\TRACKB16.LZC` |
| 193 | `5E892A74` | 71624 | 5,974,063 | A | bundle-lzc |  | `TRACKS\TRACKB21.LZC` |
| 194 | `5E9B42F5` | 74542 | 5,941,787 | A | bundle-lzc |  | `TRACKS\TRACKB22.LZC` |
| 195 | `5EAD5B76` | 77444 | 6,583,344 | A | bundle-lzc |  | `TRACKS\TRACKB23.LZC` |
| 196 | `5EBF48D3` | 80659 | 9,863,612 | A | bundle |  | `TRACKS\TRACKB24.BUN` |
| 197 | `5EBF73F7` | 85476 | 5,976,934 | A | bundle-lzc |  | `TRACKS\TRACKB24.LZC` |
| 198 | `5ED16154` | 88395 | 10,094,900 | A | bundle |  | `TRACKS\TRACKB25.BUN` |
| 199 | `5ED18C78` | 93325 | 5,943,284 | A | bundle-lzc |  | `TRACKS\TRACKB25.LZC` |
| 200 | `5EE379D5` | 96227 | 11,123,352 | A | bundle |  | `TRACKS\TRACKB26.BUN` |
| 201 | `5EE3A4F9` | 101659 | 6,584,074 | A | bundle-lzc |  | `TRACKS\TRACKB26.LZC` |
| 202 | `60DE5315` | 104874 | 4,348,484 | A | bundle-lzc |  | `TRACKS\TRACKB31.LZC` |
| 203 | `60F06B96` | 106998 | 4,296,619 | A | bundle-lzc |  | `TRACKS\TRACKB32.LZC` |
| 204 | `61028417` | 109096 | 4,839,133 | A | bundle-lzc |  | `TRACKS\TRACKB33.LZC` |
| 205 | `61149C98` | 111459 | 4,354,927 | A | bundle-lzc |  | `TRACKS\TRACKB34.LZC` |
| 206 | `6126B519` | 113586 | 4,303,110 | A | bundle-lzc |  | `TRACKS\TRACKB35.LZC` |
| 207 | `6138CD9A` | 115688 | 4,845,603 | A | bundle-lzc |  | `TRACKS\TRACKB36.LZC` |
| 208 | `63337BB6` | 118055 | 6,242,696 | A | bundle-lzc |  | `TRACKS\TRACKB41.LZC` |
| 209 | `63459437` | 121104 | 5,994,605 | A | bundle-lzc |  | `TRACKS\TRACKB42.LZC` |
| 210 | `6357ACB8` | 124032 | 6,172,145 | A | bundle-lzc |  | `TRACKS\TRACKB43.LZC` |
| 211 | `63699A15` | 127046 | 10,486,856 | A | bundle |  | `TRACKS\TRACKB44.BUN` |
| 212 | `6369C539` | 132167 | 6,243,246 | A | bundle-lzc |  | `TRACKS\TRACKB44.LZC` |
| 213 | `637BB296` | 135216 | 10,124,708 | A | bundle |  | `TRACKS\TRACKB45.BUN` |
| 214 | `637BDDBA` | 140160 | 5,995,143 | A | bundle-lzc |  | `TRACKS\TRACKB45.LZC` |
| 215 | `638DCB17` | 143088 | 10,055,944 | A | bundle |  | `TRACKS\TRACKB46.BUN` |
| 216 | `638DF63B` | 147999 | 6,014,893 | A | bundle-lzc |  | `TRACKS\TRACKB46.LZC` |
| 217 | `67DDCCF8` | 150936 | 6,113,378 | A | bundle-lzc |  | `TRACKS\TRACKB61.LZC` |
| 218 | `67EFE579` | 153922 | 6,237,572 | A | bundle-lzc |  | `TRACKS\TRACKB62.LZC` |
| 219 | `6801FDFA` | 156968 | 6,149,976 | A | bundle-lzc |  | `TRACKS\TRACKB63.LZC` |
| 220 | `6814167B` | 159971 | 6,114,612 | A | bundle-lzc |  | `TRACKS\TRACKB64.LZC` |
| 221 | `682603D8` | 162957 | 10,815,616 | A | bundle |  | `TRACKS\TRACKB65.BUN` |
| 222 | `68262EFC` | 168239 | 6,238,953 | A | bundle-lzc |  | `TRACKS\TRACKB65.LZC` |
| 223 | `68381C59` | 171286 | 10,682,464 | A | bundle |  | `TRACKS\TRACKB66.BUN` |
| 224 | `6838477D` | 176503 | 6,151,408 | A | bundle-lzc |  | `TRACKS\TRACKB66.LZC` |
| 225 | `28DB8B0A` | 179507 | 48,092 | A | speech-events | dup-of=18 | `SOUND\SPEECH\EVENTS.EVT` |
| 226 | `FDFBE4A5` | 179531 | 939,328 | C | sound-bank-body |  | `SOUND\INGAME.MSB` |
| 227 | `FDFBE4AB` | 179990 | 3,584 | C | sound-bank-header |  | `SOUND\INGAME.MSH` |
| 228 | `2D945845` | 179992 | 1,540,664 | A | texture-pack | dup-of=11 | `GLOBAL\HUDTEX.BIN` |
| 229 | `E08E0121` | 180745 | 257,952 | C | sound-bank-body |  | `SOUND\PARKLAND.MSB` |
| 230 | `E08E0127` | 180871 | 640 | C | sound-bank-header |  | `SOUND\PARKLAND.MSH` |
| 231 | `251F3E3B` | 180872 | 234,432 | C | sound-bank-body |  | `SOUND\DESERT.MSB` |
| 232 | `251F3E41` | 180987 | 640 | C | sound-bank-header |  | `SOUND\DESERT.MSH` |
| 233 | `0BEA395F` | 180988 | 190,624 | C | sound-bank-body |  | `SOUND\GREECE.MSB` |
| 234 | `0BEA3965` | 181082 | 448 | C | sound-bank-header |  | `SOUND\GREECE.MSH` |
| 235 | `AD26042D` | 181083 | 221,712 | C | sound-bank-body |  | `SOUND\ALPINE.MSB` |
| 236 | `AD260433` | 181192 | 512 | C | sound-bank-header |  | `SOUND\ALPINE.MSH` |
| 237 | `CE56B858` | 181193 | 251,328 | C | sound-bank-body |  | `SOUND\TROPICS.MSB` |
| 238 | `CE56B85E` | 181316 | 640 | C | sound-bank-header |  | `SOUND\TROPICS.MSH` |
| 239 | `A2533CCA` | 181317 | 19,917 | A | bundle-lzc |  | `GLOBAL\PRERACEANIM.LZC` |
| 240 | `0FFFDCF6` | 181327 | 5,034,396 | A | texture-pack |  | `TRACKS\TEX11LOCATION.BIN` |
| 241 | `73607F72` | 183786 | 324,920 | A | texture-pack |  | `TRACKS\TEX11TRACK.BIN` |
| 242 | `5F5EE677` | 183945 | 5,034,396 | A | texture-pack |  | `TRACKS\TEX12LOCATION.BIN` |
| 243 | `776B1093` | 186404 | 324,920 | A | texture-pack |  | `TRACKS\TEX12TRACK.BIN` |
| 244 | `AEBDEFF8` | 186563 | 5,034,396 | A | texture-pack |  | `TRACKS\TEX13LOCATION.BIN` |
| 245 | `7B75A1B4` | 189022 | 324,920 | A | texture-pack |  | `TRACKS\TEX13TRACK.BIN` |
| 246 | `FE1CF979` | 189181 | 5,034,396 | A | texture-pack |  | `TRACKS\TEX14LOCATION.BIN` |
| 247 | `7F8032D5` | 191640 | 324,920 | A | texture-pack |  | `TRACKS\TEX14TRACK.BIN` |
| 248 | `4D7C02FA` | 191799 | 5,034,396 | A | texture-pack |  | `TRACKS\TEX15LOCATION.BIN` |
| 249 | `838AC3F6` | 194258 | 324,920 | A | texture-pack |  | `TRACKS\TEX15TRACK.BIN` |
| 250 | `9CDB0C7B` | 194417 | 5,034,396 | A | texture-pack |  | `TRACKS\TEX16LOCATION.BIN` |
| 251 | `87955517` | 196876 | 324,920 | A | texture-pack |  | `TRACKS\TEX16TRACK.BIN` |
| 252 | `4B401697` | 197035 | 3,602,072 | A | texture-pack |  | `TRACKS\TEX21LOCATION.BIN` |
| 253 | `F8BD34B3` | 198794 | 588,216 | A | texture-pack |  | `TRACKS\TEX21TRACK.BIN` |
| 254 | `9A9F2018` | 199082 | 3,602,072 | A | texture-pack |  | `TRACKS\TEX22LOCATION.BIN` |
| 255 | `FCC7C5D4` | 200841 | 588,216 | A | texture-pack |  | `TRACKS\TEX22TRACK.BIN` |
| 256 | `E9FE2999` | 201129 | 3,602,072 | A | texture-pack |  | `TRACKS\TEX23LOCATION.BIN` |
| 257 | `00D256F5` | 202888 | 588,216 | A | texture-pack |  | `TRACKS\TEX23TRACK.BIN` |
| 258 | `395D331A` | 203176 | 3,602,072 | A | texture-pack |  | `TRACKS\TEX24LOCATION.BIN` |
| 259 | `04DCE816` | 204935 | 688,056 | A | texture-pack |  | `TRACKS\TEX24TRACK.BIN` |
| 260 | `88BC3C9B` | 205271 | 3,602,072 | A | texture-pack |  | `TRACKS\TEX25LOCATION.BIN` |
| 261 | `08E77937` | 207030 | 688,056 | A | texture-pack |  | `TRACKS\TEX25TRACK.BIN` |
| 262 | `D81B461C` | 207366 | 3,602,072 | A | texture-pack |  | `TRACKS\TEX26LOCATION.BIN` |
| 263 | `0CF20A58` | 209125 | 688,056 | A | texture-pack |  | `TRACKS\TEX26TRACK.BIN` |
| 264 | `86805038` | 209461 | 4,972,416 | A | texture-pack |  | `TRACKS\TEX31LOCATION.BIN` |
| 265 | `7E19E9F4` | 211889 | 655,032 | A | texture-pack |  | `TRACKS\TEX31TRACK.BIN` |
| 266 | `D5DF59B9` | 212209 | 4,972,416 | A | texture-pack |  | `TRACKS\TEX32LOCATION.BIN` |
| 267 | `82247B15` | 214637 | 655,032 | A | texture-pack |  | `TRACKS\TEX32TRACK.BIN` |
| 268 | `253E633A` | 214957 | 4,972,416 | A | texture-pack |  | `TRACKS\TEX33LOCATION.BIN` |
| 269 | `862F0C36` | 217385 | 655,032 | A | texture-pack |  | `TRACKS\TEX33TRACK.BIN` |
| 270 | `749D6CBB` | 217705 | 4,972,416 | A | texture-pack |  | `TRACKS\TEX34LOCATION.BIN` |
| 271 | `8A399D57` | 220133 | 324,920 | A | texture-pack |  | `TRACKS\TEX34TRACK.BIN` |
| 272 | `C3FC763C` | 220292 | 4,972,416 | A | texture-pack |  | `TRACKS\TEX35LOCATION.BIN` |
| 273 | `8E442E78` | 222720 | 324,920 | A | texture-pack |  | `TRACKS\TEX35TRACK.BIN` |
| 274 | `135B7FBD` | 222879 | 4,972,416 | A | texture-pack |  | `TRACKS\TEX36LOCATION.BIN` |
| 275 | `924EBF99` | 225307 | 324,920 | A | texture-pack |  | `TRACKS\TEX36TRACK.BIN` |
| 276 | `C1C089D9` | 225466 | 4,275,808 | A | texture-pack |  | `TRACKS\TEX41LOCATION.BIN` |
| 277 | `03769F35` | 227554 | 992,184 | A | texture-pack |  | `TRACKS\TEX41TRACK.BIN` |
| 278 | `111F935A` | 228039 | 4,275,808 | A | texture-pack |  | `TRACKS\TEX42LOCATION.BIN` |
| 279 | `07813056` | 230127 | 992,184 | A | texture-pack |  | `TRACKS\TEX42TRACK.BIN` |
| 280 | `607E9CDB` | 230612 | 4,275,808 | A | texture-pack |  | `TRACKS\TEX43LOCATION.BIN` |
| 281 | `0B8BC177` | 232700 | 992,184 | A | texture-pack |  | `TRACKS\TEX43TRACK.BIN` |
| 282 | `AFDDA65C` | 233185 | 4,275,808 | A | texture-pack |  | `TRACKS\TEX44LOCATION.BIN` |
| 283 | `0F965298` | 235273 | 992,184 | A | texture-pack |  | `TRACKS\TEX44TRACK.BIN` |
| 284 | `FF3CAFDD` | 235758 | 4,275,808 | A | texture-pack |  | `TRACKS\TEX45LOCATION.BIN` |
| 285 | `13A0E3B9` | 237846 | 992,184 | A | texture-pack |  | `TRACKS\TEX45TRACK.BIN` |
| 286 | `4E9BB95E` | 238331 | 4,275,808 | A | texture-pack |  | `TRACKS\TEX46LOCATION.BIN` |
| 287 | `17AB74DA` | 240419 | 992,184 | A | texture-pack |  | `TRACKS\TEX46TRACK.BIN` |
| 288 | `3840FD1B` | 240904 | 4,470,284 | A | texture-pack |  | `TRACKS\TEX61LOCATION.BIN` |
| 289 | `0E3009B7` | 243087 | 337,208 | A | texture-pack |  | `TRACKS\TEX61TRACK.BIN` |
| 290 | `87A0069C` | 243252 | 4,470,284 | A | texture-pack |  | `TRACKS\TEX62LOCATION.BIN` |
| 291 | `123A9AD8` | 245435 | 337,208 | A | texture-pack |  | `TRACKS\TEX62TRACK.BIN` |
| 292 | `D6FF101D` | 245600 | 4,470,284 | A | texture-pack |  | `TRACKS\TEX63LOCATION.BIN` |
| 293 | `16452BF9` | 247783 | 377,400 | A | texture-pack |  | `TRACKS\TEX63TRACK.BIN` |
| 294 | `265E199E` | 247968 | 4,470,284 | A | texture-pack |  | `TRACKS\TEX64LOCATION.BIN` |
| 295 | `1A4FBD1A` | 250151 | 324,920 | A | texture-pack |  | `TRACKS\TEX64TRACK.BIN` |
| 296 | `75BD231F` | 250310 | 4,470,284 | A | texture-pack |  | `TRACKS\TEX65LOCATION.BIN` |
| 297 | `1E5A4E3B` | 252493 | 324,920 | A | texture-pack |  | `TRACKS\TEX65TRACK.BIN` |
| 298 | `C51C2CA0` | 252652 | 4,470,284 | A | texture-pack |  | `TRACKS\TEX66LOCATION.BIN` |
| 299 | `2264DF5C` | 254835 | 365,112 | A | texture-pack |  | `TRACKS\TEX66TRACK.BIN` |
| 300 | `36C33219` | 255014 | 326,584 | A | texture-pack |  | `GLOBAL\DRIVTEX.BIN` |
| 301 | `98696CD4` | 255174 | 323,632 | A | bundle |  | `GLOBAL\CHARACTERSA.BIN` |
| 302 | `4C937012` | 255333 | 773,932 | A | bundle |  | `GLOBAL\GLOBALB.BUN` |
| 303 | `5FB1FAA7` | 255711 | 85,440 | B | nis-scene |  | `NIS\NIS_ARREST01.BIN` |
| 304 | `5FB1FDDF` | 255753 | 15,824 | B | nis-camera |  | `NIS\NIS_ARREST01.CAM` |
| 305 | `5FC41328` | 255761 | 108,672 | B | nis-scene |  | `NIS\NIS_ARREST02.BIN` |
| 306 | `5FC41660` | 255815 | 26,432 | B | nis-camera |  | `NIS\NIS_ARREST02.CAM` |
| 307 | `5FD62BA9` | 255828 | 105,152 | B | nis-scene |  | `NIS\NIS_ARREST03.BIN` |
| 308 | `5FD62EE1` | 255880 | 25,712 | B | nis-camera |  | `NIS\NIS_ARREST03.CAM` |
| 309 | `E4320A5A` | 255893 | 99,520 | B | nis-scene |  | `NIS\NIS_TICKET01.BIN` |
| 310 | `E4320D92` | 255942 | 24,560 | B | nis-camera |  | `NIS\NIS_TICKET01.CAM` |
| 311 | `E44422DB` | 255954 | 99,520 | B | nis-scene |  | `NIS\NIS_TICKET02.BIN` |
| 312 | `E4442613` | 256003 | 17,744 | B | nis-camera |  | `NIS\NIS_TICKET02.CAM` |
| 313 | `E4563B5C` | 256012 | 99,520 | B | nis-scene |  | `NIS\NIS_TICKET03.BIN` |
| 314 | `E4563E94` | 256061 | 24,560 | B | nis-camera |  | `NIS\NIS_TICKET03.CAM` |
| 315 | `E46853DD` | 256073 | 99,520 | B | nis-scene |  | `NIS\NIS_TICKET04.BIN` |
| 316 | `E4685715` | 256122 | 24,560 | B | nis-camera |  | `NIS\NIS_TICKET04.CAM` |
| 317 | `E48C84DF` | 256134 | 99,520 | B | nis-scene |  | `NIS\NIS_TICKET06.BIN` |
| 318 | `E48C8817` | 256183 | 24,560 | B | nis-camera |  | `NIS\NIS_TICKET06.CAM` |
| 319 | `57F88461` | 256195 | 6,504 | A | engine-sound-table |  | `SOUND\ENGINE.CTB` |
| 320 | `57F8AAAA` | 256199 | 6,504 | A | engine-sound-table |  | `SOUND\ENGINE.LTB` |
| 321 | `AD921759` | 256203 | 192,385,024 | C | music |  | `SOUND\JUKEBOX.JBC` |
| 322 | `61F6C1A6` | 350141 | 193,167,360 | A | music |  | `SOUND\JUKEBOXM.JBC` |
| 323 | `34494CE2` | 444461 | 159,136 | C | sound-bank-body |  | `SOUND\NEWE348.MSB` |
| 324 | `34494CE8` | 444539 | 704 | C | sound-bank-header |  | `SOUND\NEWE348.MSH` |
| 325 | `0D760A08` | 444540 | 163,952 | C | sound-bank-body |  | `SOUND\NEWE993.MSB` |
| 326 | `0D760A0E` | 444621 | 704 | C | sound-bank-header |  | `SOUND\NEWE993.MSH` |
| 327 | `F374CB89` | 444622 | 148,368 | C | sound-bank-body |  | `SOUND\NEWEBMW.MSB` |
| 328 | `F374CB8F` | 444695 | 704 | C | sound-bank-header |  | `SOUND\NEWEBMW.MSH` |
| 329 | `624587DB` | 444696 | 149,680 | C | sound-bank-body |  | `SOUND\NEWEBMW2.MSB` |
| 330 | `624587E1` | 444770 | 704 | C | sound-bank-header |  | `SOUND\NEWEBMW2.MSH` |
| 331 | `FEFA65AC` | 444771 | 161,120 | C | sound-bank-body |  | `SOUND\NEWEBRU.MSB` |
| 332 | `FEFA65B2` | 444850 | 704 | C | sound-bank-header |  | `SOUND\NEWEBRU.MSH` |
| 333 | `0CC30FEF` | 444851 | 159,552 | C | sound-bank-body |  | `SOUND\NEWEBXR.MSB` |
| 334 | `0CC30FF5` | 444929 | 704 | C | sound-bank-header |  | `SOUND\NEWEBXR.MSH` |
| 335 | `23BC2BB4` | 444930 | 157,696 | C | sound-bank-body |  | `SOUND\NEWECAM.MSB` |
| 336 | `23BC2BBA` | 445007 | 704 | C | sound-bank-header |  | `SOUND\NEWECAM.MSH` |
| 337 | `4E259A0C` | 445008 | 155,152 | C | sound-bank-body |  | `SOUND\NEWECSS.MSB` |
| 338 | `4E259A12` | 445084 | 704 | C | sound-bank-header |  | `SOUND\NEWECSS.MSH` |
| 339 | `5068AA2C` | 445085 | 119,488 | C | sound-bank-body |  | `SOUND\NEWECTR.MSB` |
| 340 | `5068AA32` | 445144 | 704 | C | sound-bank-header |  | `SOUND\NEWECTR.MSH` |
| 341 | `8E8ACD1B` | 445145 | 161,312 | C | sound-bank-body |  | `SOUND\NEWEDNF.MSB` |
| 342 | `8E8ACD21` | 445224 | 704 | C | sound-bank-header |  | `SOUND\NEWEDNF.MSH` |
| 343 | `83AEC0A7` | 445225 | 141,552 | C | sound-bank-body |  | `SOUND\NEWEGTI.MSB` |
| 344 | `83AEC0AD` | 445295 | 704 | C | sound-bank-header |  | `SOUND\NEWEGTI.MSH` |
| 345 | `4B41A854` | 445296 | 151,472 | C | sound-bank-body |  | `SOUND\NEWEMR2.MSB` |
| 346 | `4B41A85A` | 445370 | 704 | C | sound-bank-header |  | `SOUND\NEWEMR2.MSH` |
| 347 | `7BF59B85` | 445371 | 149,184 | C | sound-bank-body |  | `SOUND\NEWENEO.MSB` |
| 348 | `7BF59B8B` | 445444 | 704 | C | sound-bank-header |  | `SOUND\NEWENEO.MSH` |
| 349 | `9A6B892E` | 445445 | 151,760 | C | sound-bank-body |  | `SOUND\NEWEPANTERA.MSB` |
| 350 | `9A6B8934` | 445520 | 704 | C | sound-bank-header |  | `SOUND\NEWEPANTERA.MSH` |
| 351 | `34CDE93C` | 445521 | 158,320 | C | sound-bank-body |  | `SOUND\NEWEPRW.MSB` |
| 352 | `34CDE942` | 445599 | 704 | C | sound-bank-header |  | `SOUND\NEWEPRW.MSH` |
| 353 | `A29AD39E` | 445600 | 164,672 | C | sound-bank-body |  | `SOUND\NEWEPRW02.MSB` |
| 354 | `A29AD3A4` | 445681 | 704 | C | sound-bank-header |  | `SOUND\NEWEPRW02.MSH` |
| 355 | `1243CBF9` | 445682 | 155,136 | C | sound-bank-body |  | `SOUND\NEWESNU.MSB` |
| 356 | `1243CBFF` | 445758 | 704 | C | sound-bank-header |  | `SOUND\NEWESNU.MSH` |
| 357 | `38AAE9F9` | 445759 | 163,008 | C | sound-bank-body |  | `SOUND\NEWETESTAROSA.MSB` |
| 358 | `38AAE9FF` | 445839 | 704 | C | sound-bank-header |  | `SOUND\NEWETESTAROSA.MSH` |
| 359 | `E2F0CC8A` | 445840 | 6,025,080 | - | unknown | unnamed | `_UNNAMED\E2F0CC8A.BIN` |
| 360 | `68645B6E` | 448782 | 57,888 | - | unknown | unnamed | `_UNNAMED\68645B6E.BIN` |
| 361 | `88915EDF` | 448811 | 93,988 | B | speech-pack |  | `SOUND\SPEECH\ENGLISHBAN.PCK` |
| 362 | `22F26B67` | 448857 | 303,441,517 | B | speech-pack |  | `SOUND\SPEECH\ENGLISHDAT.PCK` |
| 363 | `2D46F36B` | 597022 | 93,788 | B | speech-pack |  | `SOUND\SPEECH\FRENCHBAN.PCK` |
| 364 | `C7A7FFF3` | 597068 | 282,770,490 | B | speech-pack |  | `SOUND\SPEECH\FRENCHDAT.PCK` |
| 365 | `6286FD6F` | 735140 | 93,904 | B | speech-pack |  | `SOUND\SPEECH\GERMANBAN.PCK` |
| 366 | `FCE809F7` | 735186 | 310,805,589 | B | speech-pack |  | `SOUND\SPEECH\GERMANDAT.PCK` |
| 367 | `CEA9F930` | 886947 | 207,280 | - | unknown | unnamed | `_UNNAMED\CEA9F930.BIN` |
| 368 | `84E35957` | 887049 | 93,672 | B | speech-pack |  | `SOUND\SPEECH\ITALIANBAN.PCK` |
| 369 | `1F4465DF` | 887095 | 313,869,866 | B | speech-pack |  | `SOUND\SPEECH\ITALIANDAT.PCK` |
| 370 | `49D7D2EB` | 1040352 | 93,744 | B | speech-pack |  | `SOUND\SPEECH\SPANISHBAN.PCK` |
| 371 | `E438DF73` | 1040398 | 303,797,855 | B | speech-pack |  | `SOUND\SPEECH\SPANISHDAT.PCK` |
| 372 | `5D50EC33` | 1188737 | 521,728 | - | movie | unnamed | `_UNNAMED\5D50EC33.AVI` |
| 373 | `9BB9A742` | 1188992 | 15,608,320 | A | movie |  | `MOVIES\HOTPURSUIT_MOVIE.AVI` |
| 374 | `44AE5152` | 1196614 | 11,173,888 | A | movie |  | `MOVIES\INTRO\BBGLOGO_EN.AVI` |
| 375 | `29BA024B` | 1202070 | 10,446,336 | - | movie | unnamed | `_UNNAMED\29BA024B.AVI` |
| 376 | `34073F4D` | 1207171 | 3,177,472 | A | movie |  | `MOVIES\INTRO\EALOGO_EN.AVI` |
| 377 | `36A4C9F2` | 1208723 | 3,730,432 | A | movie |  | `MOVIES\INTRO\EALOGO_FR.AVI` |
| 378 | `380EB406` | 1210545 | 3,502,592 | A | movie |  | `MOVIES\INTRO\EALOGO_GE.AVI` |
| 379 | `3DC874D7` | 1212256 | 3,383,296 | A | movie |  | `MOVIES\INTRO\EALOGO_IT.AVI` |
| 380 | `54D3A91D` | 1213908 | 3,494,912 | A | movie |  | `MOVIES\INTRO\EALOGO_SP.AVI` |
| 381 | `555254A4` | 1215615 | 3,849,728 | A | movie |  | `MOVIES\INTRO\EALOGO_SW.AVI` |
| 382 | `56CC36E9` | 1217495 | 349,414,400 | A | movie |  | `MOVIES\INTRO\MAKEOF_EN.AVI` |
| 383 | `9B959AE9` | 1388108 | 58,766,336 | A | movie |  | `MOVIES\INTRO\NFSINTRO_EN.AVI` |
| 384 | `9E33258E` | 1416803 | 59,768,320 | A | movie |  | `MOVIES\INTRO\NFSINTRO_FR.AVI` |
| 385 | `5C94BFBB` | 1445987 | 30,944,768 | A | movie |  | `MOVIES\INTRO\REWARD_EN.AVI` |
| 386 | `5F324A60` | 1461097 | 27,202,560 | A | movie |  | `MOVIES\INTRO\REWARD_FR.AVI` |
| 387 | `81D0C635` | 1474380 | 12,130,816 | A | movie |  | `MOVIES\MAINMENU_MOVIE.AVI` |
| 388 | `25B48247` | 1480304 | 16,573,952 | A | movie |  | `MOVIES\OPTIONS_MOVIE.AVI` |
| 389 | `0FE92050` | 1488397 | 16,893,440 | A | movie |  | `MOVIES\OPTIONS_MOVIE_NEW.AVI` |
| 390 | `6693FD6C` | 1496646 | 21,950,464 | - | movie | unnamed | `_UNNAMED\6693FD6C.AVI` |
| 391 | `39514A5E` | 1507364 | 10,283,520 | - | movie | unnamed | `_UNNAMED\39514A5E.AVI` |
| 392 | `A3661577` | 1512386 | 19,030,016 | A | movie |  | `MOVIES\WORLDRACING_ MOVIE.AVI` |
| 393 | `48671A56` | 1521678 | 130 | - | text | unnamed | `_UNNAMED\48671A56.TXT` |
| 394 | `95673D4E` | 1521679 | 928 | A | track-markers |  | `MARKERS\MARKERS11.BIN` |
| 395 | `957955CF` | 1521680 | 768 | A | track-markers |  | `MARKERS\MARKERS12.BIN` |
| 396 | `97BC65EF` | 1521681 | 1,568 | A | track-markers |  | `MARKERS\MARKERS21.BIN` |
| 397 | `97CE7E70` | 1521682 | 1,280 | A | track-markers |  | `MARKERS\MARKERS22.BIN` |
| 398 | `9A23A711` | 1521683 | 16 | A | track-markers |  | `MARKERS\MARKERS32.BIN` |
| 399 | `406CB595` | 1521684 | 4,775 | D | text |  | `MARKERS\TRACK11.TXT` |
| 400 | `407ECE16` | 1521687 | 3,910 | D | text |  | `MARKERS\TRACK12.TXT` |
| 401 | `42C1DE36` | 1521689 | 7,267 | D | text |  | `MARKERS\TRACK21.TXT` |
| 402 | `42D3F6B7` | 1521693 | 4,501 | D | text |  | `MARKERS\TRACK22.TXT` |
| 403 | `45291F58` | 1521696 | 54 | D | text |  | `MARKERS\TRACK32.TXT` |
| 404 | `313E6330` | 1521697 | 10 | - | text | unnamed | `_UNNAMED\313E6330.TXT` |
<!-- inventory:end -->
