// Shared basics for the HotPursuit2PC libraries.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hp2 {

namespace fs = std::filesystem;

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

inline constexpr std::uint64_t kSectorSize = 2048;

// Every failure in the pipeline is an hp2::Error whose message names the
// file/record involved and the reason (brief §3.3).
class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

template <class... Args>
[[noreturn]] void fail(std::format_string<Args...> fmt, Args&&... args) {
  throw Error(std::format(fmt, std::forward<Args>(args)...));
}

// Paths in messages, manifests and logs are UTF-8.
std::string to_utf8(const fs::path& p);
fs::path from_utf8(std::string_view s);

// Bounds-checked little-endian read. Never index file data directly.
inline std::uint32_t read_u32le(ByteView b, std::size_t off) {
  if (off > b.size() || b.size() - off < 4)
    fail("read of 4 bytes at offset {} is outside a {}-byte buffer", off, b.size());
  return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
         (static_cast<std::uint32_t>(b[off + 2]) << 16) |
         (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

}  // namespace hp2
