// Disc image tests on synthetic files: what is written can be read back
// byte for byte, the layout is deterministic, bad input is refused.
#include "minitest.h"
#include "testkit.h"

#include <set>

#include "hp2/iso.h"

using namespace hp2;
using testkit::pattern;
using testkit::TempDir;

namespace {

struct Fixture {
  TempDir tmp{"iso"};
  std::vector<IsoSource> sources;

  void add(const std::string& iso_path, std::size_t size, std::uint32_t seed) {
    const fs::path host = tmp.path() / "src" / iso_path;
    testkit::write_bytes(host, pattern(size, seed));
    IsoSource s;
    s.iso_path = iso_path;
    s.host_path = host;
    s.size = size;
    s.time = {2002, 8, 4, 9, 21, 13};
    sources.push_back(s);
  }

  Fixture() {
    add("SYSTEM.CNF", 57, 1);
    add("SLUS_203.62", 5000, 2);
    add("NFSHP2/ZDIR.BIN", 24, 3);
    add("NFSHP2/ZZDATA.BIN", 10000, 4);
    add("NFSHP2/IRX243/PADMAN.IRX", 2048, 5);  // exactly one sector
    add("NFSHP2/IRX253/EMPTY.IRX", 0, 6);       // empty file
  }
};

const IsoEntry* find(const IsoInfo& info, std::string_view path) {
  for (const IsoEntry& e : info.entries)
    if (e.path == path) return &e;
  return nullptr;
}

}  // namespace

TEST(iso_roundtrip_every_file_reads_back_identically) {
  Fixture f;
  const IsoLayout layout = IsoLayout::build(f.sources, "SLUS_20362");
  const fs::path out = f.tmp.path() / "out dir" / "disc.iso";
  std::uint64_t last = 0;
  write_iso(layout, out, [&](const Progress& p) {
    CHECK(p.done >= last);
    CHECK(p.done <= p.total);
    last = p.done;
  });
  CHECK_EQ(last, layout.total_bytes());
  CHECK_EQ(fs::file_size(out), static_cast<std::uintmax_t>(layout.total_bytes()));
  CHECK_EQ(layout.total_bytes() % 2048, static_cast<std::uint64_t>(0));
  CHECK(!fs::exists(out.string() + ".part"));

  const ReadOnlyFile iso(out);
  const IsoInfo info = read_iso_directory(iso);
  CHECK_EQ(info.system_id, std::string("PLAYSTATION"));
  CHECK_EQ(info.volume_id, std::string("SLUS_20362"));
  CHECK_EQ(info.volume_sectors, layout.total_sectors());

  // three directories + six files, nothing else
  CHECK_EQ(info.entries.size(), static_cast<std::size_t>(9));
  for (const char* d : {"NFSHP2", "NFSHP2/IRX243", "NFSHP2/IRX253"}) {
    REQUIRE(find(info, d) != nullptr);
    CHECK(find(info, d)->directory);
  }
  const std::uint32_t first_data = static_cast<std::uint32_t>(layout.metadata().size() / 2048);
  std::set<std::uint32_t> used;
  for (const IsoSource& s : f.sources) {
    const IsoEntry* e = find(info, s.iso_path);
    REQUIRE(e != nullptr);
    CHECK(!e->directory);
    CHECK_EQ(e->size, s.size);
    if (s.size == 0) continue;
    CHECK(e->sector >= first_data);
    CHECK(iso.read_at(static_cast<std::uint64_t>(e->sector) * 2048, static_cast<std::size_t>(s.size)) ==
          testkit::read_bytes(s.host_path));
    for (std::uint32_t k = 0; k < (s.size + 2047) / 2048; ++k) CHECK(used.insert(e->sector + k).second);  // no overlap
  }
  // the image ends right after the last file: no stray sectors
  CHECK_EQ(first_data + static_cast<std::uint32_t>(used.size()), layout.total_sectors());
}

TEST(iso_layout_is_deterministic) {
  Fixture f;
  const IsoLayout a = IsoLayout::build(f.sources, "SLUS_20362");
  std::vector<IsoSource> shuffled(f.sources.rbegin(), f.sources.rend());  // input order must not matter
  const IsoLayout b = IsoLayout::build(shuffled, "SLUS_20362");
  CHECK(a.metadata() == b.metadata());
  CHECK_EQ(a.total_sectors(), b.total_sectors());
  write_iso(a, f.tmp.path() / "a.iso");
  write_iso(b, f.tmp.path() / "b.iso");
  CHECK(testkit::read_bytes(f.tmp.path() / "a.iso") == testkit::read_bytes(f.tmp.path() / "b.iso"));
}

TEST(iso_descriptor_fields_follow_ecma_119) {
  Fixture f;
  const IsoLayout layout = IsoLayout::build(f.sources, "SLUS_20362");
  const Bytes& m = layout.metadata();
  const std::size_t pvd = 16 * 2048, term = 17 * 2048;
  for (std::size_t i = 0; i < pvd; ++i) REQUIRE(m[i] == 0);  // system area is empty
  CHECK_EQ(m[pvd], static_cast<std::uint8_t>(1));
  CHECK(std::string(m.begin() + pvd + 1, m.begin() + pvd + 6) == "CD001");
  CHECK_EQ(m[term], static_cast<std::uint8_t>(255));
  CHECK(std::string(m.begin() + term + 1, m.begin() + term + 6) == "CD001");
  // logical block size 2048 in both byte orders
  CHECK_EQ(m[pvd + 128], static_cast<std::uint8_t>(0x00));
  CHECK_EQ(m[pvd + 129], static_cast<std::uint8_t>(0x08));
  CHECK_EQ(m[pvd + 130], static_cast<std::uint8_t>(0x08));
  CHECK_EQ(m[pvd + 131], static_cast<std::uint8_t>(0x00));
  // root directory record: 34 bytes, directory flag, identifier 0x00
  CHECK_EQ(m[pvd + 156], static_cast<std::uint8_t>(34));
  CHECK_EQ(m[pvd + 156 + 25], static_cast<std::uint8_t>(0x02));
  CHECK_EQ(m[pvd + 156 + 32], static_cast<std::uint8_t>(1));
  // recording date of the root record: 2002-08-04 09:21:13 UTC
  CHECK_EQ(m[pvd + 156 + 18], static_cast<std::uint8_t>(102));
  CHECK_EQ(m[pvd + 156 + 19], static_cast<std::uint8_t>(8));
  CHECK_EQ(m[pvd + 156 + 20], static_cast<std::uint8_t>(4));
  // L path table at sector 18: root entry (len 1, extent = root dir sector, parent 1)
  const std::size_t lt = 18 * 2048;
  CHECK_EQ(m[lt], static_cast<std::uint8_t>(1));
  CHECK_EQ(read_u32le(m, lt + 2), read_u32le(m, pvd + 156 + 2));
  CHECK_EQ(m[lt + 6], static_cast<std::uint8_t>(1));
  // second path table entry is NFSHP2, child of the root
  CHECK_EQ(m[lt + 10], static_cast<std::uint8_t>(6));
  CHECK(std::string(m.begin() + lt + 18, m.begin() + lt + 24) == "NFSHP2");
}

TEST(iso_directory_larger_than_one_sector) {
  TempDir tmp("iso-big");
  std::vector<IsoSource> sources;
  for (int i = 0; i < 150; ++i) {  // 150 records of 46 bytes > 2 sectors
    IsoSource s;
    s.iso_path = std::format("DATA/FILE{:04}.BIN", i);
    s.host_path = tmp.path() / "src" / std::format("{}.bin", i);
    testkit::write_bytes(s.host_path, pattern(static_cast<std::size_t>(100 + i * 7), static_cast<std::uint32_t>(i)));
    s.size = static_cast<std::uint64_t>(100 + i * 7);
    sources.push_back(s);
  }
  write_iso(IsoLayout::build(sources, "TEST"), tmp.path() / "big.iso");
  const ReadOnlyFile iso(tmp.path() / "big.iso");
  const IsoInfo info = read_iso_directory(iso);
  CHECK_EQ(info.entries.size(), static_cast<std::size_t>(151));
  REQUIRE(find(info, "DATA") != nullptr);
  CHECK(find(info, "DATA")->size >= 3 * 2048);
  for (const IsoSource& s : sources) {
    const IsoEntry* e = find(info, s.iso_path);
    REQUIRE(e != nullptr);
    CHECK(iso.read_at(static_cast<std::uint64_t>(e->sector) * 2048, static_cast<std::size_t>(s.size)) ==
          testkit::read_bytes(s.host_path));
  }
}

TEST(iso_build_rejects_names_and_sizes_it_cannot_represent) {
  const auto one = [](const std::string& path, std::uint64_t size = 1) {
    IsoSource s;
    s.iso_path = path;
    s.size = size;
    return std::vector<IsoSource>{s};
  };
  CHECK_THROWS(IsoLayout::build({}, "X"));
  CHECK_THROWS(IsoLayout::build(one("lower.bin"), "X"));
  CHECK_THROWS(IsoLayout::build(one("TOOLONGNAME.BIN"), "X"));
  CHECK_THROWS(IsoLayout::build(one("NAME.LONG"), "X"));
  CHECK_THROWS(IsoLayout::build(one("SP ACE.BIN"), "X"));
  CHECK_THROWS(IsoLayout::build(one("LONGDIRNAME/A.BIN"), "X"));
  CHECK_THROWS(IsoLayout::build(one("A//B.BIN"), "X"));
  CHECK_THROWS(IsoLayout::build(one("A/B/C/D/E/F/G/H/I.BIN"), "X"));  // 9 levels
  CHECK_THROWS(IsoLayout::build(one("BIG.BIN", 1ull << 32), "X"));
  CHECK_THROWS(IsoLayout::build(one("OK.BIN"), "bad volume id!"));
  std::vector<IsoSource> twice = one("A.BIN");
  twice.push_back(twice.front());
  CHECK_THROWS(IsoLayout::build(twice, "X"));
  std::vector<IsoSource> clash = one("A");
  clash.push_back(one("A/B.BIN").front());
  CHECK_THROWS(IsoLayout::build(clash, "X"));
  // and the plain valid cases still work
  CHECK_EQ(IsoLayout::build(one("A/B/C/D/E/F/G/H.BIN"), "X").files().size(), static_cast<std::size_t>(1));
  CHECK_EQ(IsoLayout::build(one("NOEXT"), "X").files().size(), static_cast<std::size_t>(1));
}

TEST(iso_write_is_safe) {
  Fixture f;
  const IsoLayout layout = IsoLayout::build(f.sources, "SLUS_20362");
  const fs::path out = f.tmp.path() / "disc.iso";
  write_iso(layout, out);
  CHECK_THROWS(write_iso(layout, out));  // never overwrites
  // a source that changed after planning is detected, and no partial image stays behind
  testkit::write_bytes(f.sources[1].host_path, pattern(4999, 2));
  const fs::path out2 = f.tmp.path() / "second.iso";
  CHECK_THROWS(write_iso(layout, out2));
  CHECK(!fs::exists(out2));
  CHECK(!fs::exists(out2.string() + ".part"));
}

TEST(iso_reader_rejects_damaged_images) {
  Fixture f;
  const fs::path out = f.tmp.path() / "disc.iso";
  write_iso(IsoLayout::build(f.sources, "SLUS_20362"), out);
  const Bytes good = testkit::read_bytes(out);

  testkit::write_bytes(f.tmp.path() / "notiso.bin", pattern(40 * 2048, 9));
  CHECK_THROWS(read_iso_directory(ReadOnlyFile(f.tmp.path() / "notiso.bin")));
  testkit::write_bytes(f.tmp.path() / "tiny.iso", pattern(100, 9));
  CHECK_THROWS(read_iso_directory(ReadOnlyFile(f.tmp.path() / "tiny.iso")));

  // truncated: the volume claims more sectors than the file has
  testkit::write_bytes(f.tmp.path() / "cut.iso", Bytes(good.begin(), good.begin() + good.size() / 2));
  CHECK_THROWS(read_iso_directory(ReadOnlyFile(f.tmp.path() / "cut.iso")));

  // the two byte orders of the root directory position disagree
  Bytes bad = good;
  bad[16 * 2048 + 156 + 2] ^= 0x01;
  testkit::write_bytes(f.tmp.path() / "endian.iso", bad);
  CHECK_THROWS(read_iso_directory(ReadOnlyFile(f.tmp.path() / "endian.iso")));

  // a directory record with an impossible length
  bad = good;
  const std::uint32_t root_sector = read_u32le(good, 16 * 2048 + 156 + 2);
  bad[static_cast<std::size_t>(root_sector) * 2048 + 68] = 5;  // third record: length below the minimum
  testkit::write_bytes(f.tmp.path() / "record.iso", bad);
  CHECK_THROWS(read_iso_directory(ReadOnlyFile(f.tmp.path() / "record.iso")));
}

TEST(iso_layout_for_rom_uses_exactly_the_manifest) {
  TempDir tmp("iso-rom");
  const fs::path rom = tmp.path() / "rom";
  RomProfile profile;
  profile.serial = "SLUS-20362";
  for (const auto& [rel, size, optional] :
       std::vector<std::tuple<std::string, std::size_t, bool>>{
           {"SYSTEM.CNF", 57, false}, {"SLUS_203.62", 3000, false}, {"NFSHP2/ZDIR.BIN", 12, false},
           {"NFSHP2/IRX243/VSSVER.SCC", 208, true}}) {
    if (!optional) testkit::write_bytes(rom / rel, pattern(size, 1));
    RomManifestEntry e;
    e.path = rel;
    e.size = size;
    e.optional = optional;
    profile.files.push_back(e);
  }
  testkit::write_bytes(rom / "EXTRA.TXT", pattern(10, 1));  // not in the manifest: must not be packed
  const IsoLayout layout = iso_layout_for_rom(rom, profile);
  CHECK_EQ(layout.files().size(), static_cast<std::size_t>(3));  // optional file absent -> skipped
  write_iso(layout, tmp.path() / "rom.iso");
  const IsoInfo info = read_iso_directory(ReadOnlyFile(tmp.path() / "rom.iso"));
  CHECK_EQ(info.volume_id, std::string("SLUS_20362"));
  CHECK(find(info, "SYSTEM.CNF") != nullptr);
  CHECK(find(info, "EXTRA.TXT") == nullptr);
  CHECK(find(info, "NFSHP2/IRX243/VSSVER.SCC") == nullptr);
  // a required file that is missing is an error, not a silently smaller image
  std::error_code ec;
  fs::rename(rom / "SLUS_203.62", tmp.path() / "gone", ec);
  CHECK_THROWS(iso_layout_for_rom(rom, profile));
}

MINITEST_MAIN()
