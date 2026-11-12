#include "hp2/zdir.h"

#include <algorithm>
#include <map>
#include <set>

namespace hp2 {

std::vector<ZdirRecord> parse_zdir(ByteView raw) {
  if (raw.empty()) fail("ZDIR.BIN is empty");
  if (raw.size() % kZdirRecordSize != 0)
    fail("ZDIR.BIN is {} bytes, which is not a multiple of the {}-byte record size "
         "(file truncated or not a Hot Pursuit 2 ZDIR)",
         raw.size(), kZdirRecordSize);
  const std::size_t count = raw.size() / kZdirRecordSize;
  if (count > 1'000'000) fail("ZDIR.BIN claims {} records - not a plausible directory", count);
  std::vector<ZdirRecord> records;
  records.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t off = i * kZdirRecordSize;
    records.push_back({read_u32le(raw, off), read_u32le(raw, off + 4), read_u32le(raw, off + 8)});
  }
  return records;
}

ZdirLayout check_zdir_layout(const std::vector<ZdirRecord>& records, std::uint64_t zzdata_size) {
  ZdirLayout out;
  out.records = records.size();
  out.data_sectors = zzdata_size / kSectorSize;
  if (zzdata_size % kSectorSize != 0)
    out.warnings.push_back(std::format(
        "ZZDATA.BIN size {} is not a multiple of {} bytes", zzdata_size, kSectorSize));

  std::set<std::uint32_t> hashes;
  std::map<std::uint32_t, ZdirRecord> first_by_hash;
  std::set<std::pair<std::uint32_t, std::uint32_t>> extents;  // (sector, size), non-empty only

  for (std::size_t i = 0; i < records.size(); ++i) {
    const ZdirRecord& r = records[i];
    if (i > 0 && r.sector < records[i - 1].sector) out.sorted_by_sector = false;
    if (!hashes.insert(r.hash).second) {
      ++out.duplicate_hash_records;
      const ZdirRecord& first = first_by_hash.at(r.hash);
      if (first.size != r.size)
        out.errors.push_back(std::format(
            "record {} repeats hash {:08X} with a different size ({} vs {})", i, r.hash, r.size,
            first.size));
    } else {
      first_by_hash.emplace(r.hash, r);
    }
    if (r.size == 0) {
      ++out.empty_records;
      continue;
    }
    if (r.offset() > zzdata_size || zzdata_size - r.offset() < r.size) {
      out.errors.push_back(std::format(
          "record {} (hash {:08X}) points outside ZZDATA.BIN: sector {} + {} bytes, file has {} bytes",
          i, r.hash, r.sector, r.size, zzdata_size));
      continue;
    }
    extents.emplace(r.sector, r.size);
  }
  out.distinct_hashes = hashes.size();
  out.distinct_extents = extents.size();
  if (!out.sorted_by_sector) out.warnings.push_back("records are not sorted by sector");

  // Walk the distinct extents in sector order: overlaps are fatal, gaps are counted.
  std::uint64_t pos = 0;
  for (const auto& [sector, size] : extents) {
    const std::uint64_t n = (static_cast<std::uint64_t>(size) + kSectorSize - 1) / kSectorSize;
    if (sector < pos) {
      out.errors.push_back(std::format(
          "extent at sector {} ({} bytes) overlaps the previous extent, which ends at sector {}",
          sector, size, pos));
    } else {
      out.gap_sectors += sector - pos;
    }
    pos = std::max<std::uint64_t>(pos, sector + n);
  }
  out.end_sector = pos;
  if (out.data_sectors > pos) out.gap_sectors += out.data_sectors - pos;
  if (out.gap_sectors)
    out.warnings.push_back(
        std::format("{} sectors of ZZDATA.BIN are not referenced by any record", out.gap_sectors));
  return out;
}

}  // namespace hp2
