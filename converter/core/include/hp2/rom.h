// ROM folder validation (brief §3.1): is this the extracted SLUS-20362 disc,
// complete and undamaged? Errors are sentences a user can act on.
#pragma once

#include <functional>

#include "hp2/common.h"

namespace hp2 {

struct Progress {
  std::string_view stage;  // short id, e.g. "validate", "rom", "unpack"
  std::string_view item;   // file being processed (may be empty)
  std::uint64_t done = 0;
  std::uint64_t total = 0;
};
using ProgressFn = std::function<void(const Progress&)>;

struct RomManifestEntry {
  std::string path;         // relative, '/' separators, as on the disc
  std::uint64_t size = 0;
  std::string sha256;       // lower-case hex
  bool optional = false;    // disc leftovers the game does not need
};

// What a supported disc looks like.
struct RomProfile {
  std::string serial;     // "SLUS-20362"
  std::string title;      // human readable
  std::string boot_file;  // "SLUS_203.62"
  std::string version;    // SYSTEM.CNF VER, "1.00"
  std::string video_mode; // SYSTEM.CNF VMODE, "NTSC"
  std::vector<RomManifestEntry> files;
  bool has_z_fileset = true;  // NFSHP2/ZDIR.BIN + ZZDATA.BIN are structurally checked

  std::uint64_t total_size() const;
  const RomManifestEntry* find(std::string_view path) const;

  // Need for Speed: Hot Pursuit 2, PS2, NTSC-U, v1.00 (the only supported disc).
  static const RomProfile& slus20362();
};

enum class RomCheckLevel {
  Quick,  // presence, sizes, SYSTEM.CNF, SHA-256 of every file up to 64 MiB, Z fileset structure
  Full,   // Quick + SHA-256 of the large files (reads the whole disc, ~3 GB)
};

enum class RomFileState { Ok, OkSizeOnly, Missing, WrongSize, WrongHash, Unreadable };

struct RomFileResult {
  std::string path;
  RomFileState state = RomFileState::Missing;
  std::uint64_t expected_size = 0;
  std::uint64_t actual_size = 0;
  bool optional = false;
};

struct RomReport {
  fs::path requested;   // what the caller passed
  fs::path root;        // the folder actually checked (may be corrected, see notes)
  std::vector<RomFileResult> files;
  std::vector<std::string> errors;    // each one explains what is wrong and what to do
  std::vector<std::string> warnings;
  std::vector<std::string> notes;
  bool fully_hashed = false;          // every required file was SHA-256 verified

  bool ok() const { return errors.empty(); }
  std::string summary() const;        // multi-line, for logs and the CLI
};

// Never writes anything. If the folder passed is the NFSHP2 subfolder or the
// parent of the disc root, the root is corrected and a note says so.
RomReport check_rom(const fs::path& rom_dir, RomCheckLevel level = RomCheckLevel::Quick,
                    const RomProfile& profile = RomProfile::slus20362(),
                    const ProgressFn& progress = {});

// "SLES_507.31" -> "SLES-50731"; returns the input unchanged if it has another shape.
std::string serial_from_boot_file(std::string_view boot_file);

}  // namespace hp2
