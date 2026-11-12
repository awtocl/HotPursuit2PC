// The Z fileset as a list of named, classified entries, plus extraction.
#pragma once

#include <array>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>

#include "hp2/io.h"
#include "hp2/names.h"
#include "hp2/zdir.h"

namespace hp2 {

// Classification by name and magic bytes. Internal formats other than LZC
// are UNVERIFIED until a decoder with tests exists (ADR 0004 §7).
struct FileKind {
  std::string_view id;           // stable token, e.g. "texture-pack"
  std::string_view description;  // human readable
};

// name may be empty (unnamed file). head = first bytes of the file (<= 16).
FileKind classify_file(std::string_view name, ByteView head, std::uint64_t size);

// Extension (without dot, upper-case) for a file without a recovered name.
std::string_view sniff_extension(ByteView head);

struct Entry {
  std::uint32_t index = 0;            // record number in ZDIR.BIN
  ZdirRecord record;
  std::string name;                   // recovered name or "_UNNAMED\<HASH>.<EXT>"
  bool named = false;
  char tier = ' ';                    // name confidence tier, ' ' when unnamed
  std::optional<std::uint32_t> duplicate_of;  // earlier record with the same hash
  FileKind kind;
  std::array<std::uint8_t, 16> head{};
  std::uint8_t head_size = 0;
};

class ZFileset {
 public:
  // rom_dir is the extracted disc root (contains NFSHP2/ZDIR.BIN and
  // NFSHP2/ZZDATA.BIN). Throws hp2::Error if the directory is unusable:
  // missing files, malformed ZDIR, or extents outside/overlapping in ZZDATA.
  static ZFileset open(const fs::path& rom_dir, const NameTable& names = NameTable::builtin());

  const std::vector<Entry>& entries() const { return entries_; }
  const ZdirLayout& layout() const { return layout_; }
  const ReadOnlyFile& data() const { return *data_; }
  std::size_t named_count() const;

  // Whole content of one entry.
  Bytes read(const Entry& e) const;

 private:
  ZFileset() = default;
  std::vector<Entry> entries_;
  ZdirLayout layout_;
  std::shared_ptr<ReadOnlyFile> data_;
};

// ------------------------------------------------------------------ inventory

struct InventoryOptions {
  bool with_sha256 = false;  // reads every file (about 3 GB)
};
// Tab-separated: index, hash, sector, size, [sha256,] tier, kind, flags, name
void write_inventory_tsv(const ZFileset& set, std::ostream& out, const InventoryOptions& opt = {});

// -------------------------------------------------------------------- unpack

struct UnpackOptions {
  bool verify_existing = false;  // re-hash files that already exist instead of trusting their size
  bool repair = false;           // move aside and re-extract existing files that do not match
  std::function<bool(const Entry&)> filter;  // optional: return false to leave an entry out
};

struct UnpackProgress {
  std::uint64_t bytes_done = 0;
  std::uint64_t bytes_total = 0;
  std::uint32_t files_done = 0;
  std::uint32_t files_total = 0;
  std::string_view current;  // name being processed
};

struct UnpackResult {
  std::uint32_t written = 0;        // files extracted in this run
  std::uint32_t kept = 0;           // files already present and accepted
  std::uint32_t repaired = 0;       // mismatching files moved aside and re-extracted
  std::uint32_t duplicates = 0;     // records that repeat an earlier hash (content compared)
  std::uint32_t filtered_out = 0;
  std::uint64_t bytes_written = 0;
  fs::path manifest;                // <out_dir>/manifest.tsv
  std::vector<std::string> notes;   // human-readable remarks (repairs, anomalies)
};

// Extracts every entry to out_dir/<name with '\' as folders>. Safe to re-run:
// see ADR 0004 §4. Throws hp2::Error naming the file and the reason.
UnpackResult unpack(const ZFileset& set, const fs::path& out_dir, const UnpackOptions& opt = {},
                    const std::function<void(const UnpackProgress&)>& progress = {});

}  // namespace hp2
