// Converter-core tests against the owner's real dump (read-only). Skipped
// (exit 77) when no dump is present.
#include "minitest.h"
#include "testkit.h"

#include <map>

#include "hp2/convert.h"
#include "hp2/iso.h"

using namespace hp2;

namespace {

fs::path need_dump() {
  const auto d = testkit::find_dump(HP2_SOURCE_DIR);
  if (!d) SKIP("no dump found (put it in ROM/ or set HP2_ROM_DIR)");
  return *d;
}

std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>> snapshot(const fs::path& dir) {
  std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>> s;
  for (const auto& de : fs::recursive_directory_iterator(dir))
    if (de.is_regular_file())
      s[fs::relative(de.path(), dir).generic_string()] = {de.file_size(), de.last_write_time()};
  return s;
}

}  // namespace

TEST(dump_passes_the_quick_check) {
  const fs::path dump = need_dump();
  const auto before = snapshot(dump);
  const RomReport r = check_rom(dump);
  for (const std::string& e : r.errors) std::printf("    error: %s\n", e.c_str());
  CHECK(r.ok());
  CHECK(r.warnings.empty());
  CHECK(!r.fully_hashed);  // ZZDATA.BIN is checked by size only in the quick check
  CHECK_EQ(r.files.size(), static_cast<std::size_t>(27));
  std::size_t hashed = 0;
  for (const RomFileResult& f : r.files) {
    CHECK(f.state == RomFileState::Ok || f.state == RomFileState::OkSizeOnly);
    if (f.state == RomFileState::Ok) ++hashed;
  }
  CHECK_EQ(hashed, static_cast<std::size_t>(26));
  CHECK(snapshot(dump) == before);  // C2: read-only
}

TEST(dump_passes_the_full_check) {
  const fs::path dump = need_dump();
  std::uint64_t last_done = 0, total = 0;
  const RomReport r = check_rom(dump, RomCheckLevel::Full, RomProfile::slus20362(), [&](const Progress& p) {
    CHECK(p.done >= last_done);
    last_done = p.done;
    total = p.total;
  });
  CHECK(r.ok());
  CHECK(r.fully_hashed);
  CHECK_EQ(total, RomProfile::slus20362().total_size());
  CHECK_EQ(last_done, total);
}

TEST(dump_is_found_from_neighbouring_folders) {
  const fs::path dump = need_dump();
  const RomReport from_sub = check_rom(dump / "NFSHP2");
  CHECK(from_sub.ok());
  CHECK(fs::equivalent(from_sub.root, dump));
  const RomReport from_parent = check_rom(dump.parent_path());
  CHECK(from_parent.ok());
  CHECK(fs::equivalent(from_parent.root, dump));
}

TEST(dump_disc_image_layout_holds_all_27_files) {
  // Plans the image only (writing 3 GB is left to the manual evidence run).
  const fs::path dump = need_dump();
  const IsoLayout layout = iso_layout_for_rom(dump);
  CHECK_EQ(layout.files().size(), static_cast<std::size_t>(27));
  std::uint64_t sectors = layout.metadata().size() / 2048;
  std::uint32_t expected_next = static_cast<std::uint32_t>(sectors);
  for (const IsoLayout::FileExtent& f : layout.files()) {
    CHECK_EQ(f.sector, expected_next);  // files are packed back to back
    CHECK_EQ(f.size, RomProfile::slus20362().find(f.iso_path)->size);
    expected_next += static_cast<std::uint32_t>((f.size + 2047) / 2048);
  }
  CHECK_EQ(layout.total_sectors(), expected_next);
  CHECK(layout.total_bytes() > RomProfile::slus20362().total_size());
  CHECK(layout.total_bytes() < RomProfile::slus20362().total_size() + (1u << 20));  // < 1 MiB overhead
}

TEST(dump_convert_refuses_to_write_into_the_rom_folder) {
  const fs::path dump = need_dump();
  const auto before = snapshot(dump);
  ConvertRequest req;
  req.rom_dir = dump;
  req.game_dir = dump / "game";
  const ConvertResult r = convert(req);
  CHECK(!r.ok);
  CHECK(r.error.find("never written to") != std::string::npos);
  CHECK(!fs::exists(dump / "game"));
  CHECK(snapshot(dump) == before);
}

MINITEST_MAIN()
