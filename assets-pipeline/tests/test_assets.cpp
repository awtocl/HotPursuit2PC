// Asset-pipeline tests that need no game data: hash vectors, synthetic ZDIR /
// LZC / fileset fixtures built on the fly, and the built-in name table.
#include "minitest.h"
#include "testkit.h"

#include <set>
#include <sstream>

#include "hp2/fileset.h"
#include "hp2/lzc.h"

using namespace hp2;
using testkit::pattern;
using testkit::SynthRecord;
using testkit::TempDir;

namespace {

// Independent LZSS encoder for the COMP format (test-only): greedy longest
// match, 4095-byte window, lengths 3..18.
Bytes comp_encode(const Bytes& src, bool use_matches) {
  Bytes out = {'C', 'O', 'M', 'P', 0x01, 0x10, 0x00, 0x00};
  auto put32 = [&](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  };
  put32(static_cast<std::uint32_t>(src.size()));
  put32(0);  // packed size, patched below
  std::size_t ip = 0;
  while (ip < src.size()) {
    const std::size_t flag_pos = out.size();
    out.push_back(0);
    out.push_back(0);
    std::uint32_t flags = 0;
    for (int bit = 0; bit < 16 && ip < src.size(); ++bit) {
      std::size_t best_len = 0, best_dist = 0;
      if (use_matches) {
        const std::size_t max_dist = std::min<std::size_t>(ip, 4095);
        for (std::size_t dist = 1; dist <= max_dist; ++dist) {
          std::size_t len = 0;
          while (len < 18 && ip + len < src.size() && src[ip + len] == src[ip + len - dist]) ++len;
          if (len > best_len) {
            best_len = len;
            best_dist = dist;
          }
        }
      }
      if (best_len >= 3) {
        flags |= 1u << bit;
        out.push_back(static_cast<std::uint8_t>(((best_dist >> 8) << 4) | (best_len - 3)));
        out.push_back(static_cast<std::uint8_t>(best_dist & 0xFF));
        ip += best_len;
      } else {
        out.push_back(src[ip++]);
      }
    }
    out[flag_pos] = static_cast<std::uint8_t>(flags & 0xFF);
    out[flag_pos + 1] = static_cast<std::uint8_t>(flags >> 8);
  }
  const auto packed = static_cast<std::uint32_t>(out.size());
  for (int i = 0; i < 4; ++i) out[12 + i] = static_cast<std::uint8_t>(packed >> (8 * i));
  return out;
}

Bytes bytes_of(std::string_view s) { return Bytes(s.begin(), s.end()); }

}  // namespace

// ------------------------------------------------------------------ name hash

TEST(name_hash_matches_values_stored_on_the_disc) {
  // Values read from ZDIR.BIN of SLUS-20362 (docs/dump-analysis.md §3.2).
  CHECK_EQ(name_hash("GLOBAL\\GLOBALA.BUN"), 0x4C815791u);  // record 0
  CHECK_EQ(name_hash("GLOBAL\\GLOBALB.LZC"), 0x4C939B36u);  // record 1
  CHECK_EQ(name_hash("NFS6.ICO"), 0x5986D8A5u);
  CHECK_EQ(name_hash("MOVIES\\WORLDRACING_ MOVIE.AVI"), 0xA3661577u);
}

TEST(name_hash_is_case_insensitive_and_seeded) {
  CHECK_EQ(name_hash("global\\globala.bun"), name_hash("GLOBAL\\GLOBALA.BUN"));
  CHECK_EQ(name_hash(""), 0xFFFFFFFFu);
  CHECK_EQ(name_hash("A"), 0xFFFFFFFFu * 33u + 'A');
  // Structural facts used during recovery: sibling deltas.
  CHECK_EQ(name_hash("SOUND\\COMMON.MSH") - name_hash("SOUND\\COMMON.MSB"), 6u);
  CHECK_EQ(name_hash("TRACKS\\TRACKA12.BUN") - name_hash("TRACKS\\TRACKA11.BUN"), 33u * 33u * 33u * 33u);
  CHECK(name_hash("A/B") != name_hash("A\\B"));  // separators are not normalised
}

// ----------------------------------------------------------------------- ZDIR

TEST(zdir_parses_twelve_byte_records) {
  const Bytes raw = {0x91, 0x57, 0x81, 0x4C, 0, 0, 0, 0, 0, 0, 0, 0,
                     0x36, 0x9B, 0x93, 0x4C, 0, 0, 0, 0, 0xC6, 0xC0, 0x04, 0x00};
  const auto recs = parse_zdir(raw);
  REQUIRE(recs.size() == 2);
  CHECK_EQ(recs[0].hash, 0x4C815791u);
  CHECK_EQ(recs[0].size, 0u);
  CHECK_EQ(recs[1].hash, 0x4C939B36u);
  CHECK_EQ(recs[1].sector, 0u);
  CHECK_EQ(recs[1].size, 311494u);
  CHECK_EQ(recs[1].sectors(), static_cast<std::uint64_t>(153));
}

TEST(zdir_rejects_bad_sizes) {
  CHECK_THROWS(parse_zdir(Bytes{}));
  CHECK_THROWS(parse_zdir(Bytes(13, 0)));
  CHECK_THROWS(parse_zdir(Bytes(11, 0)));
}

TEST(zdir_layout_accepts_exact_tiling) {
  const std::vector<ZdirRecord> recs = {{1, 0, 0}, {2, 0, 2049}, {3, 2, 100}, {4, 3, 2048}};
  const ZdirLayout l = check_zdir_layout(recs, 4 * 2048);
  CHECK(l.ok());
  CHECK(l.exact_tiling());
  CHECK_EQ(l.empty_records, static_cast<std::size_t>(1));
  CHECK_EQ(l.distinct_extents, static_cast<std::size_t>(3));
  CHECK_EQ(l.gap_sectors, static_cast<std::uint64_t>(0));
}

TEST(zdir_layout_reports_out_of_range_overlap_and_gaps) {
  // out of range
  ZdirLayout l = check_zdir_layout({{1, 0, 4096}, {2, 2, 2049}}, 3 * 2048);
  CHECK(!l.ok());
  // overlap: second extent starts inside the first
  l = check_zdir_layout({{1, 0, 4096}, {2, 1, 100}}, 4 * 2048);
  CHECK(!l.ok());
  // gap: usable but not exact
  l = check_zdir_layout({{1, 0, 100}, {2, 3, 100}}, 4 * 2048);
  CHECK(l.ok());
  CHECK(!l.exact_tiling());
  CHECK_EQ(l.gap_sectors, static_cast<std::uint64_t>(2));
  // same hash twice with different sizes is an error
  l = check_zdir_layout({{7, 0, 100}, {7, 1, 200}}, 2 * 2048);
  CHECK(!l.ok());
  // same hash twice with the same size (as on the retail disc) is fine
  l = check_zdir_layout({{7, 0, 100}, {7, 1, 100}}, 2 * 2048);
  CHECK(l.ok());
  CHECK_EQ(l.duplicate_hash_records, static_cast<std::size_t>(1));
}

// ------------------------------------------------------------------------ LZC

TEST(lzc_decodes_hand_built_stream) {
  // "ABC" as literals, then one token: length 6, distance 3  ->  "ABCABCABC"
  const Bytes stream = {'C', 'O', 'M', 'P', 0x01, 0x10, 0, 0, 9, 0, 0, 0, 23, 0, 0, 0,
                        0x08, 0x00,        // flags: bits 0-2 literal, bit 3 match
                        'A', 'B', 'C',
                        0x03, 0x03};       // length 3+3, distance 3
  REQUIRE(stream.size() == 23);
  CHECK(is_lzc(stream));
  const LzcHeader h = lzc_header(stream);
  CHECK_EQ(h.unpacked_size, 9u);
  CHECK_EQ(h.packed_size, 23u);
  CHECK(lzc_decompress(stream) == bytes_of("ABCABCABC"));
}

TEST(lzc_roundtrips_against_independent_encoder) {
  // Compressible data: repeated motifs plus noise, long runs, and pure noise.
  Bytes mixed;
  for (int i = 0; i < 40; ++i) {
    const Bytes motif = pattern(37 + i % 5, 7u + static_cast<std::uint32_t>(i % 3));
    mixed.insert(mixed.end(), motif.begin(), motif.end());
    mixed.insert(mixed.end(), static_cast<std::size_t>(i), static_cast<std::uint8_t>(i));
  }
  for (const Bytes& src : {mixed, Bytes(5000, 0x11), pattern(3000, 99), Bytes{}, Bytes{42}}) {
    for (bool matches : {true, false}) {
      const Bytes packed = comp_encode(src, matches);
      CHECK(lzc_decompress(packed) == src);
    }
  }
  // The encoder must actually have used matches on compressible input.
  CHECK(comp_encode(mixed, true).size() < mixed.size());
}

TEST(lzc_rejects_corrupt_streams_without_overrunning) {
  const Bytes good = comp_encode(bytes_of("ABCABCABCABCABCABCxyzxyzxyz"), true);
  // bad magic
  Bytes bad = good;
  bad[0] = 'X';
  CHECK(!is_lzc(bad));
  CHECK_THROWS(lzc_decompress(bad));
  // unsupported version
  bad = good;
  bad[4] = 0x02;
  CHECK_THROWS(lzc_decompress(bad));
  // truncated: packed size larger than the data available
  bad = good;
  bad.pop_back();
  CHECK_THROWS(lzc_decompress(bad));
  // header shorter than 16 bytes
  CHECK_THROWS(lzc_decompress(Bytes(good.begin(), good.begin() + 10)));
  // match that points before the start of the output
  const Bytes before_start = {'C', 'O', 'M', 'P', 0x01, 0x10, 0, 0, 8, 0, 0, 0, 20, 0, 0, 0,
                              0x01, 0x00, 0x05, 0x04};  // first token: distance 4 at output 0
  CHECK_THROWS(lzc_decompress(before_start));
  // distance 0
  const Bytes zero_dist = {'C', 'O', 'M', 'P', 0x01, 0x10, 0, 0, 8, 0, 0, 0, 21, 0, 0, 0,
                           0x02, 0x00, 'A', 0x05, 0x00};
  CHECK_THROWS(lzc_decompress(zero_dist));
  // input ends before the declared output size is reached
  const Bytes short_in = {'C', 'O', 'M', 'P', 0x01, 0x10, 0, 0, 50, 0, 0, 0, 19, 0, 0, 0,
                          0x00, 0x00, 'A'};
  CHECK_THROWS(lzc_decompress(short_in));
  // absurd unpacked size
  bad = good;
  bad[11] = 0x7F;
  CHECK_THROWS(lzc_decompress(bad));
}

// ----------------------------------------------------------------- name table

TEST(builtin_names_hash_to_their_stored_values) {
  const NameTable& t = NameTable::builtin();
  CHECK_EQ(t.size(), static_cast<std::size_t>(362));
  std::set<std::uint32_t> seen;
  std::size_t tiers[4] = {};
  for (const NameEntry& e : t.entries()) {
    CHECK_EQ(name_hash(e.name), e.hash);
    CHECK(seen.insert(e.hash).second);
    CHECK(is_safe_relative_name(e.name));
    REQUIRE(e.tier >= 'A' && e.tier <= 'D');
    ++tiers[e.tier - 'A'];
  }
  CHECK_EQ(tiers[0], static_cast<std::size_t>(273));
  CHECK_EQ(tiers[1], static_cast<std::size_t>(31));
  CHECK_EQ(tiers[2], static_cast<std::size_t>(53));
  CHECK_EQ(tiers[3], static_cast<std::size_t>(5));
  REQUIRE(t.find(0x4C815791u) != nullptr);
  CHECK(t.find(0x4C815791u)->name == "GLOBAL\\GLOBALA.BUN");
  CHECK(t.find(0x12345678u) == nullptr);
}

TEST(safe_name_check_blocks_path_tricks) {
  CHECK(is_safe_relative_name("CARS\\F50\\GEOMETRY.BIN"));
  CHECK(is_safe_relative_name("_UNNAMED\\5D50EC33.AVI"));
  CHECK(!is_safe_relative_name(""));
  CHECK(!is_safe_relative_name("..\\X.BIN"));
  CHECK(!is_safe_relative_name("A\\..\\X.BIN"));
  CHECK(!is_safe_relative_name("\\X.BIN"));
  CHECK(!is_safe_relative_name("A\\\\X.BIN"));
  CHECK(!is_safe_relative_name("C:\\X.BIN"));
  CHECK(!is_safe_relative_name("A/B.BIN"));
  CHECK(!is_safe_relative_name("A\\B."));
  CHECK(!is_safe_relative_name("a\\b.bin"));  // table names are upper-case
}

// ----------------------------------------------------- synthetic fileset + unpack

namespace {

struct Synth {
  TempDir tmp{"assets"};
  fs::path rom = tmp.path() / "rom dir with spaces";
  fs::path out = tmp.path() / "out";
  Bytes a = pattern(5000, 1), b = comp_encode(pattern(300, 2), true), c = pattern(2048, 3);
  Bytes text = bytes_of("Hello.\r\n");
  std::vector<SynthRecord> records() const {
    return {
        {name_hash("GLOBAL\\GLOBALA.BUN"), {}},              // empty placeholder
        {name_hash("GLOBAL\\HUDTEX.BIN"), a},
        {name_hash("GLOBAL\\GLOBALB.LZC"), b},
        {0xDEADBEEFu, c},                                     // no known name
        {name_hash("GLOBAL\\HUDTEX.BIN"), a},                // same name stored twice
        {0x0BADF00Du, text},                                  // unnamed text
    };
  }
  Synth() { testkit::write_synthetic_fileset(rom, records()); }
};

const Entry& by_index(const ZFileset& set, std::uint32_t i) { return set.entries().at(i); }

}  // namespace

TEST(fileset_names_classifies_and_flags_duplicates) {
  Synth s;
  const ZFileset set = ZFileset::open(s.rom);
  REQUIRE(set.entries().size() == 6);
  CHECK(set.layout().exact_tiling());
  CHECK_EQ(set.named_count(), static_cast<std::size_t>(4));
  CHECK_EQ(by_index(set, 0).name, std::string("GLOBAL\\GLOBALA.BUN"));
  CHECK(by_index(set, 0).kind.id == "placeholder");
  CHECK(by_index(set, 2).kind.id == "bundle-lzc");
  CHECK_EQ(by_index(set, 3).name, std::string("_UNNAMED\\DEADBEEF.BIN"));
  CHECK(!by_index(set, 3).named);
  CHECK(by_index(set, 4).duplicate_of.has_value());
  CHECK_EQ(*by_index(set, 4).duplicate_of, 1u);
  CHECK_EQ(by_index(set, 5).name, std::string("_UNNAMED\\0BADF00D.TXT"));
  CHECK(set.read(by_index(set, 1)) == s.a);
  CHECK(lzc_decompress(set.read(by_index(set, 2))) == pattern(300, 2));
}

TEST(fileset_open_fails_clearly_on_missing_or_damaged_input) {
  TempDir tmp("assets-bad");
  CHECK_THROWS(ZFileset::open(tmp.path() / "nope"));
  // ZDIR present but ZZDATA missing
  testkit::write_bytes(tmp.path() / "a" / "NFSHP2" / "ZDIR.BIN", Bytes(12, 0));
  CHECK_THROWS(ZFileset::open(tmp.path() / "a"));
  // record pointing outside ZZDATA
  testkit::write_synthetic_fileset(tmp.path() / "b", {{1, pattern(100, 1)}});
  Bytes zdir = testkit::read_bytes(tmp.path() / "b" / "NFSHP2" / "ZDIR.BIN");
  zdir[8] = 0xFF;
  zdir[9] = 0xFF;  // size far larger than the data file
  testkit::write_bytes(tmp.path() / "b" / "NFSHP2" / "ZDIR.BIN", zdir);
  bool threw = false;
  try {
    ZFileset::open(tmp.path() / "b");
  } catch (const Error& e) {
    threw = true;
    CHECK(std::string(e.what()).find("record 0") != std::string::npos);
  }
  CHECK(threw);
  // truncated ZDIR
  testkit::write_bytes(tmp.path() / "b" / "NFSHP2" / "ZDIR.BIN", Bytes(7, 0));
  CHECK_THROWS(ZFileset::open(tmp.path() / "b"));
}

TEST(unpack_extracts_everything_and_is_resumable) {
  Synth s;
  const ZFileset set = ZFileset::open(s.rom);
  std::uint32_t callbacks = 0;
  const UnpackResult r1 = unpack(set, s.out, {}, [&](const UnpackProgress& p) {
    ++callbacks;
    CHECK(p.bytes_done <= p.bytes_total);
  });
  CHECK_EQ(r1.written, 5u);
  CHECK_EQ(r1.duplicates, 1u);
  CHECK_EQ(r1.kept, 0u);
  CHECK(callbacks > 0);
  CHECK(testkit::read_bytes(s.out / "GLOBAL" / "HUDTEX.BIN") == s.a);
  CHECK(testkit::read_bytes(s.out / "GLOBAL" / "GLOBALB.LZC") == s.b);
  CHECK(testkit::read_bytes(s.out / "_UNNAMED" / "DEADBEEF.BIN") == s.c);
  CHECK(testkit::read_bytes(s.out / "_UNNAMED" / "0BADF00D.TXT") == s.text);
  CHECK_EQ(fs::file_size(s.out / "GLOBAL" / "GLOBALA.BUN"), static_cast<std::uintmax_t>(0));
  CHECK(fs::exists(r1.manifest));
  // no temporary files are left behind
  for (const auto& de : fs::recursive_directory_iterator(s.out))
    CHECK(de.path().extension() != ".part");

  // manifest: header + one line per record, sha256 of the extracted bytes
  const Bytes mraw = testkit::read_bytes(r1.manifest);
  const std::string manifest(mraw.begin(), mraw.end());
  CHECK_EQ(std::count(manifest.begin(), manifest.end(), '\n'), static_cast<std::ptrdiff_t>(7));
  CHECK(manifest.find(Sha256::hex_of(s.a)) != std::string::npos);
  CHECK(manifest.find("dup-of=1") != std::string::npos);
  CHECK(manifest.find("GLOBAL/HUDTEX.BIN") != std::string::npos);

  // second run: nothing rewritten
  const auto stamp = fs::last_write_time(s.out / "GLOBAL" / "HUDTEX.BIN");
  const UnpackResult r2 = unpack(set, s.out);
  CHECK_EQ(r2.written, 0u);
  CHECK_EQ(r2.kept, 5u);
  CHECK(fs::last_write_time(s.out / "GLOBAL" / "HUDTEX.BIN") == stamp);
}

TEST(unpack_never_overwrites_a_mismatching_file_silently) {
  Synth s;
  const ZFileset set = ZFileset::open(s.rom);
  unpack(set, s.out);
  const fs::path victim = s.out / "GLOBAL" / "HUDTEX.BIN";

  // wrong size -> error by default, file untouched
  testkit::write_bytes(victim, bytes_of("user edited this"));
  CHECK_THROWS(unpack(set, s.out));
  CHECK(testkit::read_bytes(victim) == bytes_of("user edited this"));

  // repair -> old file kept aside, correct file extracted
  UnpackOptions repair;
  repair.repair = true;
  const UnpackResult r = unpack(set, s.out, repair);
  CHECK_EQ(r.repaired, 1u);
  CHECK(testkit::read_bytes(victim) == s.a);
  CHECK(testkit::read_bytes(s.out / "GLOBAL" / "HUDTEX.BIN.mismatch-1") == bytes_of("user edited this"));
  CHECK(!r.notes.empty());

  // same size but different bytes: only --verify notices
  Bytes flipped = s.a;
  flipped[100] ^= 0xFF;
  testkit::write_bytes(victim, flipped);
  CHECK_EQ(unpack(set, s.out).kept, 5u);  // size-only check accepts it
  UnpackOptions verify;
  verify.verify_existing = true;
  CHECK_THROWS(unpack(set, s.out, verify));
  verify.repair = true;
  CHECK_EQ(unpack(set, s.out, verify).repaired, 1u);
  CHECK(testkit::read_bytes(victim) == s.a);
}

TEST(unpack_filter_and_interrupted_part_files) {
  Synth s;
  const ZFileset set = ZFileset::open(s.rom);
  // a stale .part from an interrupted run must not block or leak into the result
  testkit::write_bytes(s.out / "GLOBAL" / "HUDTEX.BIN.part", bytes_of("partial"));
  UnpackOptions opt;
  opt.filter = [](const Entry& e) { return e.named; };
  const UnpackResult r = unpack(set, s.out, opt);
  CHECK_EQ(r.filtered_out, 2u);
  CHECK(!fs::exists(s.out / "_UNNAMED"));
  CHECK(!fs::exists(s.out / "GLOBAL" / "HUDTEX.BIN.part"));
  CHECK(testkit::read_bytes(s.out / "GLOBAL" / "HUDTEX.BIN") == s.a);
}

TEST(unpack_refuses_two_extents_with_one_name_but_different_data) {
  TempDir tmp("assets-dup");
  const std::uint32_t h = name_hash("GLOBAL\\HUDTEX.BIN");
  testkit::write_synthetic_fileset(tmp.path() / "rom", {{h, pattern(64, 1)}, {h, pattern(64, 2)}});
  const ZFileset set = ZFileset::open(tmp.path() / "rom");
  CHECK_THROWS(unpack(set, tmp.path() / "out"));
}

TEST(inventory_lists_every_record) {
  Synth s;
  const ZFileset set = ZFileset::open(s.rom);
  std::ostringstream os;
  InventoryOptions opt;
  opt.with_sha256 = true;
  write_inventory_tsv(set, os, opt);
  const std::string tsv = os.str();
  CHECK_EQ(std::count(tsv.begin(), tsv.end(), '\n'), static_cast<std::ptrdiff_t>(7));
  CHECK(tsv.find("index\thash\tsector\tsize\tsha256\ttier\tkind\tflags\tname\n") == 0);
  CHECK(tsv.find("\tunnamed\t_UNNAMED\\DEADBEEF.BIN\n") != std::string::npos);
  CHECK(tsv.find(Sha256::hex_of(s.b)) != std::string::npos);
  CHECK(tsv.find("\tempty\tGLOBAL\\GLOBALA.BUN\n") != std::string::npos);
}

TEST(sha256_matches_known_vectors) {
  CHECK_EQ(Sha256::hex_of(Bytes{}),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(Sha256::hex_of(bytes_of("abc")),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

MINITEST_MAIN()
