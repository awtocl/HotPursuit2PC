// Scaffold self-test: proves the harness works and that the build/test
// environment can create, write, read and remove files under paths with
// spaces and non-ASCII characters. CI must fail loudly if it cannot (brief §9).
#include "minitest.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST(harness_check_eq_works) {
  CHECK_EQ(2 + 2, 4);
  CHECK(std::string("hp2") == "hp2");
  CHECK_THROWS(throw 1);
}

TEST(source_tree_is_visible) {
  // HP2_SOURCE_DIR is injected by hp2_add_test(); a wrong value is a path error.
  const fs::path root = HP2_SOURCE_DIR;
  REQUIRE(fs::exists(root / "CMakeLists.txt"));
  REQUIRE(fs::exists(root / "docs" / "STATUS.md"));
}

TEST(temp_dir_roundtrip_with_spaces_and_unicode) {
  std::error_code ec;
  const fs::path base = fs::temp_directory_path(ec);
  REQUIRE(!ec);
  const fs::path dir = base / L"hp2pc selftest ñ é";
  fs::remove_all(dir, ec);
  REQUIRE(fs::create_directories(dir));
  const fs::path file = dir / L"Need for Speed - Hot Pursuit 2 (USA).bin";
  {
    std::ofstream out(file, std::ios::binary);
    REQUIRE(out.good());
    out << "ok";
  }
  CHECK_EQ(fs::file_size(file), static_cast<std::uintmax_t>(2));
  {
    std::ifstream in(file, std::ios::binary);
    std::string s;
    in >> s;
    CHECK_EQ(s, std::string("ok"));
  }
  fs::remove_all(dir, ec);
  CHECK(!ec);
  CHECK(!fs::exists(dir));
}

MINITEST_MAIN()
