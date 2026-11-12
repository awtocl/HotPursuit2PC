#include "hp2/rom.h"

#include <algorithm>

#include "hp2/fileset.h"
#include "hp2/io.h"

namespace hp2 {

namespace {

constexpr std::uint64_t kQuickHashLimit = 64ull << 20;
constexpr std::size_t kChunk = 4u << 20;

struct RawEntry {
  const char* path;
  std::uint64_t size;
  const char* sha256;
};

// Generated at configure time from converter/data/slus20362_rom_manifest.tsv.
constexpr RawEntry kSlus20362[] = {
#include "hp2_rom_manifest.inc"
};

std::string upper(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return out;
}

std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return std::string(s);
}

fs::path join(const fs::path& root, std::string_view rel) { return root / from_utf8(rel); }

bool is_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

// SYSTEM.CNF is "KEY = value" lines. Returns the value for key or "".
std::string cnf_value(std::string_view text, std::string_view key) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    const std::string_view line = text.substr(pos, eol - pos);
    const std::size_t eq = line.find('=');
    if (eq != std::string_view::npos && upper(trim(line.substr(0, eq))) == key)
      return trim(line.substr(eq + 1));
    pos = eol + 1;
  }
  return {};
}

// "cdrom0:\SLUS_203.62;1" -> "SLUS_203.62"
std::string boot_file_of(std::string_view boot2) {
  std::size_t start = boot2.find_last_of("\\/:");
  start = start == std::string_view::npos ? 0 : start + 1;
  std::size_t end = boot2.find(';', start);
  if (end == std::string_view::npos) end = boot2.size();
  return upper(trim(boot2.substr(start, end - start)));
}

std::string sha256_of_file(const ReadOnlyFile& f, const ProgressFn& progress, Progress& p,
                           std::string_view item) {
  Sha256 h;
  Bytes buf(static_cast<std::size_t>(std::min<std::uint64_t>(std::max<std::uint64_t>(f.size(), 1), kChunk)));
  std::uint64_t done = 0;
  while (done < f.size()) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(f.size() - done, kChunk));
    f.read_at(done, std::span<std::uint8_t>(buf.data(), n));
    h.update(ByteView(buf.data(), n));
    done += n;
    p.done += n;
    if (progress) {
      p.item = item;
      progress(p);
    }
  }
  return Sha256::hex(h.finish());
}

// Finds the disc root when the user picked a neighbouring folder.
fs::path resolve_root(const fs::path& dir, RomReport& report) {
  if (is_file(dir / "SYSTEM.CNF")) return dir;
  std::error_code ec;
  // The NFSHP2 folder itself was selected.
  if (is_file(dir / "ZDIR.BIN") && is_file(dir.parent_path() / "SYSTEM.CNF")) {
    report.notes.push_back(std::format(
        "'{}' is the NFSHP2 folder of the disc; using its parent '{}' as the ROM folder.",
        to_utf8(dir), to_utf8(dir.parent_path())));
    return dir.parent_path();
  }
  // A folder that contains exactly one disc root.
  std::vector<fs::path> candidates;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    std::error_code ec2;
    if (de.is_directory(ec2) && is_file(de.path() / "SYSTEM.CNF")) candidates.push_back(de.path());
  }
  if (candidates.size() == 1) {
    report.notes.push_back(std::format("'{}' contains the disc in its subfolder; using '{}' as the ROM folder.",
                                       to_utf8(dir), to_utf8(candidates.front())));
    return candidates.front();
  }
  return dir;
}

}  // namespace

// ------------------------------------------------------------------ RomProfile

std::uint64_t RomProfile::total_size() const {
  std::uint64_t n = 0;
  for (const RomManifestEntry& e : files) n += e.size;
  return n;
}

const RomManifestEntry* RomProfile::find(std::string_view path) const {
  for (const RomManifestEntry& e : files)
    if (e.path == path) return &e;
  return nullptr;
}

const RomProfile& RomProfile::slus20362() {
  static const RomProfile profile = [] {
    RomProfile p;
    p.serial = "SLUS-20362";
    p.title = "Need for Speed: Hot Pursuit 2 (PlayStation 2, NTSC-U, v1.00)";
    p.boot_file = "SLUS_203.62";
    p.version = "1.00";
    p.video_mode = "NTSC";
    for (const RawEntry& r : kSlus20362) {
      RomManifestEntry e;
      e.path = r.path;
      e.size = r.size;
      e.sha256 = r.sha256;
      // Visual SourceSafe leftover on the disc; nothing loads it.
      e.optional = e.path.ends_with("VSSVER.SCC");
      p.files.push_back(std::move(e));
    }
    return p;
  }();
  return profile;
}

std::string serial_from_boot_file(std::string_view boot_file) {
  // SLUS_203.62 -> SLUS-20362
  if (boot_file.size() == 11 && boot_file[4] == '_' && boot_file[8] == '.') {
    std::string s(boot_file.substr(0, 4));
    s += '-';
    s += boot_file.substr(5, 3);
    s += boot_file.substr(9, 2);
    return s;
  }
  return std::string(boot_file);
}

// ------------------------------------------------------------------- RomReport

std::string RomReport::summary() const {
  std::string s = std::format("ROM folder: {}\n", to_utf8(root.empty() ? requested : root));
  for (const std::string& n : notes) s += "  note: " + n + "\n";
  std::size_t ok_n = 0;
  for (const RomFileResult& f : files)
    if (f.state == RomFileState::Ok || f.state == RomFileState::OkSizeOnly) ++ok_n;
  if (!files.empty()) s += std::format("  files: {} of {} as expected\n", ok_n, files.size());
  for (const std::string& w : warnings) s += "  warning: " + w + "\n";
  for (const std::string& e : errors) s += "  ERROR: " + e + "\n";
  if (ok())
    s += fully_hashed ? "  result: valid (every file verified by SHA-256)\n"
                      : "  result: valid (large files checked by size only; use the full check to hash them)\n";
  else
    s += std::format("  result: NOT usable ({} problem(s))\n", errors.size());
  return s;
}

// ------------------------------------------------------------------- check_rom

RomReport check_rom(const fs::path& rom_dir, RomCheckLevel level, const RomProfile& profile,
                    const ProgressFn& progress) {
  RomReport report;
  report.requested = rom_dir;
  std::error_code ec;

  if (rom_dir.empty()) {
    report.errors.push_back("No ROM folder was given. Select the folder that contains SYSTEM.CNF, " +
                            profile.boot_file + " and the NFSHP2 folder.");
    return report;
  }
  if (fs::is_regular_file(rom_dir, ec)) {
    const std::string ext = upper(to_utf8(rom_dir.extension()));
    if (ext == ".ISO" || ext == ".BIN" || ext == ".IMG" || ext == ".CHD" || ext == ".CSO")
      report.errors.push_back(std::format(
          "'{}' is a disc image file. This converter needs the extracted contents of the disc: "
          "open the image with a tool such as 7-Zip, extract everything into a folder, and select that folder.",
          to_utf8(rom_dir)));
    else
      report.errors.push_back(std::format(
          "'{}' is a file, not a folder. Select the folder that contains SYSTEM.CNF, {} and NFSHP2.",
          to_utf8(rom_dir), profile.boot_file));
    return report;
  }
  if (!fs::is_directory(rom_dir, ec)) {
    report.errors.push_back(std::format("The ROM folder '{}' does not exist.", to_utf8(rom_dir)));
    return report;
  }

  report.root = resolve_root(rom_dir, report);
  const fs::path& root = report.root;

  // ---- SYSTEM.CNF: which disc is this? ---------------------------------------
  const fs::path cnf_path = root / "SYSTEM.CNF";
  if (!is_file(cnf_path)) {
    report.errors.push_back(std::format(
        "'{}' does not look like an extracted PlayStation 2 disc: SYSTEM.CNF is missing. "
        "Select the folder that directly contains SYSTEM.CNF, {} and the NFSHP2 folder.",
        to_utf8(root), profile.boot_file));
    return report;
  }
  try {
    const Bytes raw = read_whole_file(cnf_path, 64 * 1024);
    const std::string_view text(reinterpret_cast<const char*>(raw.data()), raw.size());
    const std::string boot2 = cnf_value(text, "BOOT2");
    const std::string boot = boot_file_of(boot2);
    if (boot2.empty()) {
      report.errors.push_back(
          "SYSTEM.CNF has no BOOT2 line - this is not a PlayStation 2 game disc (or the file is damaged).");
      return report;
    }
    if (boot != upper(profile.boot_file)) {
      report.errors.push_back(std::format(
          "This folder holds disc {} (boot file {}). Only {} - {} - is supported.",
          serial_from_boot_file(boot), boot, profile.serial, profile.title));
      return report;
    }
    const std::string ver = cnf_value(text, "VER");
    const std::string vmode = upper(cnf_value(text, "VMODE"));
    if (!profile.version.empty() && ver != profile.version)
      report.warnings.push_back(std::format("SYSTEM.CNF says version '{}' (expected {}).", ver, profile.version));
    if (!profile.video_mode.empty() && vmode != profile.video_mode)
      report.warnings.push_back(std::format("SYSTEM.CNF says video mode '{}' (expected {}).", vmode, profile.video_mode));
  } catch (const Error& e) {
    report.errors.push_back(std::format("SYSTEM.CNF cannot be read: {}", e.what()));
    return report;
  }

  // ---- every file of the manifest: presence and size ----------------------
  Progress p;
  p.stage = "validate";
  for (const RomManifestEntry& e : profile.files) {
    RomFileResult r;
    r.path = e.path;
    r.expected_size = e.size;
    r.optional = e.optional;
    const fs::path file = join(root, e.path);
    if (!is_file(file)) {
      r.state = RomFileState::Missing;
    } else {
      r.actual_size = fs::file_size(file, ec);
      r.state = (!ec && r.actual_size == e.size) ? RomFileState::OkSizeOnly : RomFileState::WrongSize;
      if (r.state == RomFileState::OkSizeOnly &&
          (level == RomCheckLevel::Full || e.size <= kQuickHashLimit))
        p.total += e.size;
    }
    report.files.push_back(std::move(r));
  }

  // ---- hashes -------------------------------------------------------------------
  bool all_hashed = true;
  for (std::size_t i = 0; i < profile.files.size(); ++i) {
    const RomManifestEntry& e = profile.files[i];
    RomFileResult& r = report.files[i];
    if (r.state != RomFileState::OkSizeOnly) {
      if (!e.optional) all_hashed = false;
      continue;
    }
    if (level != RomCheckLevel::Full && e.size > kQuickHashLimit) {
      all_hashed = false;
      continue;
    }
    try {
      const ReadOnlyFile f(join(root, e.path));
      r.state = sha256_of_file(f, progress, p, e.path) == e.sha256 ? RomFileState::Ok
                                                                    : RomFileState::WrongHash;
    } catch (const Error& err) {
      r.state = RomFileState::Unreadable;
      report.errors.push_back(std::format("{} cannot be read: {}", e.path, err.what()));
    }
  }
  report.fully_hashed = all_hashed;

  // ---- turn file states into messages -------------------------------------
  std::size_t missing = 0;
  for (const RomFileResult& r : report.files) {
    std::string msg;
    switch (r.state) {
      case RomFileState::Missing:
        msg = std::format("{} is missing.", r.path);
        if (!r.optional) ++missing;
        break;
      case RomFileState::WrongSize:
        msg = std::format("{} is {} bytes but the {} disc has {} bytes there - the file is "
                          "truncated, damaged or from another release.",
                          r.path, r.actual_size, profile.serial, r.expected_size);
        break;
      case RomFileState::WrongHash:
        msg = std::format("{} has the right size but different contents (SHA-256 mismatch) - "
                          "the file is damaged or modified.",
                          r.path);
        break;
      default:
        continue;
    }
    (r.optional ? report.warnings : report.errors).push_back(std::move(msg));
  }
  if (missing)
    report.errors.push_back(std::format(
        "{} required file(s) are missing. Dump the disc again, or re-extract the image so that "
        "all files (including the NFSHP2\\IRX243 and NFSHP2\\IRX253 folders) are present.",
        missing));

  // ---- Z fileset structure ----------------------------------------------------
  if (profile.has_z_fileset) {
    const auto usable = [&](std::string_view path) {
      for (const RomFileResult& r : report.files)
        if (r.path == path) return r.state == RomFileState::Ok || r.state == RomFileState::OkSizeOnly;
      return false;
    };
    if (usable("NFSHP2/ZDIR.BIN") && usable("NFSHP2/ZZDATA.BIN")) {
      try {
        const ZFileset set = ZFileset::open(root);
        if (!set.layout().exact_tiling())
          report.warnings.push_back("ZDIR.BIN does not cover ZZDATA.BIN exactly (unexpected for this disc).");
      } catch (const Error& e) {
        report.errors.push_back(std::format("The game data archive is inconsistent: {}", e.what()));
      }
    }
  }
  return report;
}

}  // namespace hp2
