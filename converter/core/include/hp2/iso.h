// Disc image packaging: rebuild an ISO 9660 image from the extracted disc
// files. Any emulator-backed runtime (ADR 0001, Milestone 1) boots a disc
// image, and the user's ROM is a folder. Format: ECMA-119 level 1, one
// volume, 2048-byte sectors - what PS2 discs use for their file system.
//
// UNVERIFIED until the game runs from such an image: that the game addresses
// files by name only (the original sector positions are unknown - the dump is
// a flat extraction).
#pragma once

#include "hp2/io.h"
#include "hp2/rom.h"

namespace hp2 {

struct IsoTime {
  int year = 2002, month = 1, day = 1, hour = 0, minute = 0, second = 0;  // UTC
};

struct IsoSource {
  std::string iso_path;   // path inside the image, '/' separators, e.g. "NFSHP2/ZDIR.BIN"
  fs::path host_path;     // file to read the bytes from
  std::uint64_t size = 0;
  IsoTime time;
};

class IsoLayout {
 public:
  struct FileExtent {
    std::string iso_path;
    fs::path host_path;
    std::uint32_t sector = 0;
    std::uint64_t size = 0;
  };

  // Plans the image. Names must be ISO 9660 level 1 (8.3, A-Z 0-9 _), at most
  // 8 directory levels, files below 4 GiB. Throws hp2::Error otherwise.
  // The result is deterministic: same inputs -> same bytes.
  static IsoLayout build(std::vector<IsoSource> files, std::string_view volume_id,
                         std::string_view system_id = "PLAYSTATION");

  std::uint32_t total_sectors() const { return total_sectors_; }
  std::uint64_t total_bytes() const { return static_cast<std::uint64_t>(total_sectors_) * kSectorSize; }
  // Everything before the first file: system area, descriptors, path tables, directories.
  const Bytes& metadata() const { return metadata_; }
  const std::vector<FileExtent>& files() const { return files_; }  // ascending sector order

 private:
  Bytes metadata_;
  std::vector<FileExtent> files_;
  std::uint32_t total_sectors_ = 0;
};

// The layout for a validated ROM folder: exactly the files of the profile's
// manifest (optional ones only if present), nothing else from the folder.
IsoLayout iso_layout_for_rom(const fs::path& rom_root, const RomProfile& profile = RomProfile::slus20362());

// Writes the image (as <out>.part, renamed when complete). Refuses to
// overwrite an existing file. Source files are opened read-only.
void write_iso(const IsoLayout& layout, const fs::path& out_path, const ProgressFn& progress = {});

// ---- reading back (verification and tests) -----------------------------------

struct IsoEntry {
  std::string path;        // '/' separators, version suffix ";1" removed
  std::uint32_t sector = 0;
  std::uint64_t size = 0;
  bool directory = false;
};

struct IsoInfo {
  std::string system_id, volume_id;
  std::uint32_t volume_sectors = 0;
  std::vector<IsoEntry> entries;  // depth-first, directories before their contents
};

// Parses the primary volume descriptor and walks the directory tree.
// Throws hp2::Error on anything inconsistent; never reads out of bounds.
IsoInfo read_iso_directory(const ReadOnlyFile& iso);

}  // namespace hp2
