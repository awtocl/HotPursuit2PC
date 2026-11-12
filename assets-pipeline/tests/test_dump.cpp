// Asset-pipeline tests against the owner's real dump (brief §9). The dump is
// opened read-only. Without a dump every test reports "skipped" (exit 77).
#include "minitest.h"
#include "testkit.h"

#include <set>
#include <sstream>

#include "hp2/fileset.h"
#include "hp2/lzc.h"

using namespace hp2;

namespace {

fs::path need_dump() {
  const auto d = testkit::find_dump(HP2_SOURCE_DIR);
  if (!d) SKIP("no dump found (put it in ROM/ or set HP2_ROM_DIR)");
  return *d;
}

const ZFileset& the_set() {
  static const ZFileset set = ZFileset::open(need_dump());
  return set;
}

const Entry* find_name(const ZFileset& set, std::string_view name) {
  const std::uint32_t h = name_hash(name);
  for (const Entry& e : set.entries())
    if (e.record.hash == h) return &e;
  return nullptr;
}

std::string normalise_newlines(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  return s;
}

}  // namespace

TEST(dump_zdir_layout_is_the_documented_one) {
  const ZFileset& set = the_set();
  const ZdirLayout& l = set.layout();
  CHECK_EQ(l.records, static_cast<std::size_t>(405));
  CHECK_EQ(l.distinct_hashes, static_cast<std::size_t>(402));
  CHECK_EQ(l.empty_records, static_cast<std::size_t>(33));
  CHECK_EQ(l.distinct_extents, static_cast<std::size_t>(372));
  CHECK_EQ(l.duplicate_hash_records, static_cast<std::size_t>(3));
  CHECK(l.sorted_by_sector);
  CHECK(l.exact_tiling());
  CHECK(l.warnings.empty());
  CHECK_EQ(set.data().size(), static_cast<std::uint64_t>(3116437504ull));
  CHECK_EQ(l.data_sectors, static_cast<std::uint64_t>(1521698));
}

TEST(dump_every_builtin_name_is_on_the_disc) {
  const ZFileset& set = the_set();
  std::set<std::uint32_t> on_disc;
  for (const Entry& e : set.entries()) on_disc.insert(e.record.hash);
  for (const NameEntry& n : NameTable::builtin().entries()) CHECK(on_disc.count(n.hash) == 1);
  // 362 distinct names; three of them are stored twice -> 365 named records.
  CHECK_EQ(set.named_count(), static_cast<std::size_t>(365));
  CHECK_EQ(set.entries().size() - set.named_count(), static_cast<std::size_t>(40));
  CHECK_EQ(set.entries().front().name, std::string("GLOBAL\\GLOBALA.BUN"));
  CHECK_EQ(set.entries().back().name, std::string("_UNNAMED\\313E6330.TXT"));
}

TEST(dump_content_matches_the_name_extension) {
  // Independent of how names were recovered: the bytes must fit the name.
  const ZFileset& set = the_set();
  std::size_t lzc = 0, avi = 0, msh = 0;
  for (const Entry& e : set.entries()) {
    if (!e.named || e.record.size == 0) continue;
    const ByteView head(e.head.data(), e.head_size);
    const bool is_comp = is_lzc(head);
    const bool is_riff = e.head_size >= 4 && std::string_view(reinterpret_cast<const char*>(e.head.data()), 4) == "RIFF";
    const std::string_view name = e.name;
    if (name.ends_with(".LZC")) {
      CHECK(is_comp);
      ++lzc;
    } else {
      CHECK(!is_comp);
    }
    if (name.ends_with(".AVI")) {
      CHECK(is_riff);
      ++avi;
    } else {
      CHECK(!is_riff);
    }
    if (name.ends_with(".MSH")) {
      CHECK_EQ(read_u32le(head, 0), e.record.size);  // header starts with its own size
      CHECK(find_name(set, std::string(name.substr(0, name.size() - 1)) + "B") != nullptr);
      ++msh;
    }
  }
  CHECK_EQ(lzc, static_cast<std::size_t>(81));
  CHECK_EQ(avi, static_cast<std::size_t>(17));
  CHECK_EQ(msh, static_cast<std::size_t>(26));
}

TEST(dump_lzc_pairs_decompress_to_their_raw_twins) {
  const ZFileset& set = the_set();
  std::vector<std::string> stems = {"GLOBAL\\GLOBALB", "FRONTEND\\FRONTB", "GLOBAL\\INGAMEB"};
  for (int n : {24, 25, 26, 44, 45, 46, 65, 66}) stems.push_back(std::format("TRACKS\\TRACKB{}", n));
  for (const std::string& stem : stems) {
    const Entry* packed = find_name(set, stem + ".LZC");
    const Entry* raw = find_name(set, stem + ".BUN");
    REQUIRE(packed != nullptr);
    REQUIRE(raw != nullptr);
    const Bytes unpacked = lzc_decompress(set.read(*packed));
    CHECK_EQ(unpacked.size(), static_cast<std::size_t>(raw->record.size));
    CHECK(unpacked == set.read(*raw));
  }
}

TEST(dump_every_comp_stream_decompresses_to_its_declared_size) {
  const ZFileset& set = the_set();
  std::size_t count = 0;
  for (const Entry& e : set.entries()) {
    if (e.kind.id != "bundle-lzc") continue;
    const Bytes packed = set.read(e);
    const LzcHeader h = lzc_header(packed);
    CHECK_EQ(h.packed_size, e.record.size);
    CHECK_EQ(lzc_decompress(packed).size(), static_cast<std::size_t>(h.unpacked_size));
    ++count;
  }
  CHECK_EQ(count, static_cast<std::size_t>(81));
}

TEST(dump_inventory_matches_the_committed_fixture) {
  // Hashes all 3.1 GB: proves the C++ reader sees the same bytes as when the
  // fixture was generated (and as the Python reference, see dump-analysis §5).
  const ZFileset& set = the_set();
  std::ostringstream os;
  InventoryOptions opt;
  opt.with_sha256 = true;
  write_inventory_tsv(set, os, opt);
  const fs::path fixture = fs::path(HP2_SOURCE_DIR) / "tests" / "fixtures" / "slus20362_zfileset_inventory.tsv";
  REQUIRE(fs::exists(fixture));
  const Bytes raw = testkit::read_bytes(fixture);
  CHECK(normalise_newlines(os.str()) == normalise_newlines(std::string(raw.begin(), raw.end())));
}

TEST(dump_unpack_of_small_files_is_exact_and_resumable) {
  const ZFileset& set = the_set();
  testkit::TempDir tmp("dump-unpack");
  UnpackOptions opt;
  opt.filter = [](const Entry& e) { return e.record.size <= (1u << 20); };
  const UnpackResult r1 = unpack(set, tmp.path(), opt);
  CHECK(r1.written > 100);
  std::uint32_t compared = 0;
  for (const Entry& e : set.entries()) {
    if (!opt.filter(e) || e.duplicate_of) continue;
    fs::path p = tmp.path();
    std::string part;
    std::istringstream parts(e.name);
    while (std::getline(parts, part, '\\')) p /= part;
    REQUIRE(fs::exists(p));
    CHECK(testkit::read_bytes(p) == set.read(e));
    ++compared;
  }
  CHECK_EQ(compared, r1.written);
  UnpackOptions again = opt;
  again.verify_existing = true;
  const UnpackResult r2 = unpack(set, tmp.path(), again);
  CHECK_EQ(r2.written, 0u);
  CHECK_EQ(r2.kept, r1.written);
}

MINITEST_MAIN()
