// Z fileset directory (NFSHP2/ZDIR.BIN) - layout verified in docs/dump-analysis.md §3.
#pragma once

#include "hp2/common.h"

namespace hp2 {

// h = 0xFFFFFFFF; for each byte of the upper-cased name: h = h * 33 + byte.
// Names use '\' separators exactly as the game builds them.
constexpr std::uint32_t name_hash(std::string_view name) noexcept {
  std::uint32_t h = 0xFFFFFFFFu;
  for (char ch : name) {
    auto c = static_cast<unsigned char>(ch);
    if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
    h = h * 33u + c;
  }
  return h;
}

struct ZdirRecord {
  std::uint32_t hash = 0;    // name_hash() of the file's path
  std::uint32_t sector = 0;  // first 2048-byte sector inside ZZDATA.BIN
  std::uint32_t size = 0;    // size in bytes

  std::uint64_t offset() const { return static_cast<std::uint64_t>(sector) * kSectorSize; }
  std::uint64_t sectors() const { return (static_cast<std::uint64_t>(size) + kSectorSize - 1) / kSectorSize; }
  std::uint64_t end_sector() const { return sector + sectors(); }
  bool operator==(const ZdirRecord&) const = default;
};

inline constexpr std::size_t kZdirRecordSize = 12;

// Parses the raw bytes of ZDIR.BIN. Throws hp2::Error if the size is not a
// positive multiple of 12 bytes.
std::vector<ZdirRecord> parse_zdir(ByteView raw);

struct ZdirLayout {
  std::size_t records = 0;
  std::size_t distinct_hashes = 0;
  std::size_t empty_records = 0;
  std::size_t distinct_extents = 0;
  std::size_t duplicate_hash_records = 0;  // records whose hash appeared earlier
  std::uint64_t gap_sectors = 0;            // sectors of ZZDATA not covered by any record
  std::uint64_t end_sector = 0;             // first sector after the last extent
  std::uint64_t data_sectors = 0;           // size of ZZDATA.BIN in sectors
  bool sorted_by_sector = true;
  std::vector<std::string> errors;          // fatal: unpacking would read wrong data
  std::vector<std::string> warnings;        // unusual but usable

  bool ok() const { return errors.empty(); }
  // True when the records tile the data file exactly (as the retail disc does).
  bool exact_tiling() const { return ok() && gap_sectors == 0 && end_sector == data_sectors; }
};

// Checks the records against the size of ZZDATA.BIN.
ZdirLayout check_zdir_layout(const std::vector<ZdirRecord>& records, std::uint64_t zzdata_size);

}  // namespace hp2
