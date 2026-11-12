// Helpers shared by the test executables: scratch folders, dump discovery,
// and a builder for synthetic Z filesets (so tests never need game data).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace testkit {

namespace fs = std::filesystem;

// A scratch folder under the temp directory, removed when the object dies.
class TempDir {
 public:
  explicit TempDir(const std::string& label) {
    static int counter = 0;
    std::error_code ec;
    path_ = fs::temp_directory_path() / "hp2pc-tests" /
            (label + "-" + std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
             std::to_string(++counter));
    fs::remove_all(path_, ec);
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

// The owner's dump, if present: HP2_ROM_DIR, else <repo>/ROM/<redump name>.
inline std::optional<fs::path> find_dump(const fs::path& source_dir) {
  std::vector<fs::path> candidates;
  if (const wchar_t* env = _wgetenv(L"HP2_ROM_DIR")) candidates.emplace_back(env);
  candidates.push_back(source_dir / "ROM" / "Need for Speed - Hot Pursuit 2 (USA)");
  std::error_code ec;
  for (const fs::path& c : candidates)
    if (fs::is_regular_file(c / "NFSHP2" / "ZDIR.BIN", ec) &&
        fs::is_regular_file(c / "NFSHP2" / "ZZDATA.BIN", ec))
      return c;
  return std::nullopt;
}

inline void write_bytes(const fs::path& p, const std::vector<std::uint8_t>& data) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) throw std::runtime_error("testkit: cannot write " + p.string());
}

inline std::vector<std::uint8_t> read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("testkit: cannot read " + p.string());
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

// Deterministic pseudo-random bytes (xorshift) - no game data in tests.
inline std::vector<std::uint8_t> pattern(std::size_t n, std::uint32_t seed) {
  std::vector<std::uint8_t> v(n);
  std::uint32_t x = seed * 2654435761u + 1u;
  for (auto& b : v) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b = static_cast<std::uint8_t>(x >> 11);
  }
  return v;
}

struct SynthRecord {
  std::uint32_t hash;
  std::vector<std::uint8_t> data;
};

// Writes <root>/NFSHP2/ZDIR.BIN and ZZDATA.BIN from the records (2048-byte sectors).
inline void write_synthetic_fileset(const fs::path& root, const std::vector<SynthRecord>& recs) {
  std::vector<std::uint8_t> zdir, zz;
  auto put32 = [&](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) zdir.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  };
  for (const SynthRecord& r : recs) {
    put32(r.hash);
    put32(static_cast<std::uint32_t>(zz.size() / 2048));
    put32(static_cast<std::uint32_t>(r.data.size()));
    zz.insert(zz.end(), r.data.begin(), r.data.end());
    zz.resize((zz.size() + 2047) / 2048 * 2048, 0);
  }
  write_bytes(root / "NFSHP2" / "ZDIR.BIN", zdir);
  write_bytes(root / "NFSHP2" / "ZZDATA.BIN", zz);
}

}  // namespace testkit
