#include "hp2/convert.h"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <cwctype>
#include <fstream>

#include "hp2/fileset.h"
#include "hp2/io.h"

namespace hp2 {

namespace {

// Resources embedded by converter/CMakeLists.txt when the engine is built.
ByteView embedded_resource(const wchar_t* name) {
  const HRSRC res = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
  if (!res) return {};
  const HGLOBAL handle = LoadResource(nullptr, res);
  const void* data = handle ? LockResource(handle) : nullptr;
  if (!data) return {};
  return ByteView(static_cast<const std::uint8_t*>(data), SizeofResource(nullptr, res));
}

constexpr std::uint64_t kQuickHashLimit = 64ull << 20;
constexpr std::uint64_t kSpaceMargin = 64ull << 20;
constexpr std::size_t kChunk = 4u << 20;

struct Cancelled {};

std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return std::string(s);
}

std::tm local_now() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  return tm;
}

std::string timestamp() {  // 2026-11-12 14:03:59
  const std::tm tm = local_now();
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string file_stamp() {  // 20261112-140359
  const std::tm tm = local_now();
  return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string human_size(std::uint64_t bytes) {
  if (bytes >= (1ull << 30)) return std::format("{:.2f} GB", bytes / 1073741824.0);
  if (bytes >= (1ull << 20)) return std::format("{:.1f} MB", bytes / 1048576.0);
  return std::format("{} bytes", bytes);
}

fs::path normalised(const fs::path& p) {
  std::error_code ec;
  fs::path abs = fs::weakly_canonical(fs::absolute(p, ec), ec);
  if (ec || abs.empty()) abs = fs::absolute(p, ec).lexically_normal();
  return abs;
}

std::wstring lower_key(const fs::path& p) {
  std::wstring s = p.lexically_normal().generic_wstring();
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  while (s.size() > 1 && s.back() == L'/') s.pop_back();
  return s;
}

// True if child is the same folder as parent or lies below it.
bool is_same_or_inside(const fs::path& child, const fs::path& parent) {
  const std::wstring c = lower_key(child), p = lower_key(parent);
  if (c == p) return true;
  return c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == L'/';
}

bool dir_is_empty(const fs::path& dir) {
  std::error_code ec;
  return fs::directory_iterator(dir, ec) == fs::directory_iterator();
}

std::string stage_title(std::string_view id) {
  if (id == "validate") return "check the ROM";
  if (id == "prepare") return "prepare the game folder";
  if (id == "rom") return "store the ROM";
  if (id == "unpack") return "unpack the game assets";
  if (id == "build") return "build the runnable game";
  return std::string(id);
}

class Logger {
 public:
  explicit Logger(const ConvertEvents& ev) : ev_(ev) {}

  void open(const fs::path& path) {
    file_.open(path, std::ios::binary | std::ios::app);
    if (!file_) fail("cannot create the log file '{}'", to_utf8(path));
    path_ = path;
    for (const std::string& l : pending_) file_ << l << "\r\n";
    pending_.clear();
    file_.flush();
  }

  void line(std::string_view text) {
    const std::string stamped = std::format("[{}] {}", timestamp(), text);
    if (file_.is_open()) {
      file_ << stamped << "\r\n";
      file_.flush();
    } else {
      pending_.push_back(stamped);
    }
    if (ev_.log) ev_.log(text);
  }

  void block(std::string_view text) {  // multi-line text, one log line each
    std::size_t pos = 0;
    while (pos < text.size()) {
      std::size_t eol = text.find('\n', pos);
      if (eol == std::string_view::npos) eol = text.size();
      if (eol > pos) line(text.substr(pos, eol - pos));
      pos = eol + 1;
    }
  }

  const fs::path& path() const { return path_; }

 private:
  const ConvertEvents& ev_;
  std::ofstream file_;
  fs::path path_;
  std::vector<std::string> pending_;
};

// Moves a file that must not be overwritten out of the way; returns its new name.
fs::path move_aside(const fs::path& target) {
  std::error_code ec;
  fs::path aside;
  for (int n = 1; n < 10000; ++n) {
    aside = target;
    aside += std::format(L".mismatch-{}", n);
    if (!fs::exists(aside, ec)) break;
  }
  fs::rename(target, aside, ec);
  if (ec) fail("cannot move '{}' aside: {}", to_utf8(target), ec.message());
  return aside;
}

std::string sha256_of_path(const fs::path& path, const std::function<void(std::size_t)>& on_chunk) {
  const ReadOnlyFile f(path);
  Sha256 h;
  Bytes buf(kChunk);
  std::uint64_t done = 0;
  while (done < f.size()) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(f.size() - done, kChunk));
    f.read_at(done, std::span<std::uint8_t>(buf.data(), n));
    h.update(ByteView(buf.data(), n));
    done += n;
    if (on_chunk) on_chunk(n);
  }
  return Sha256::hex(h.finish());
}

std::size_t count_files(const fs::path& dir) {
  std::error_code ec;
  std::size_t n = 0;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec))
    if (it->is_regular_file(ec)) ++n;
  return n;
}

}  // namespace

// --------------------------------------------------------------------- runtime

RuntimePayload load_embedded_runtime() {
  RuntimePayload payload;
  const ByteView launcher = embedded_resource(L"HP2_LAUNCHER");
  const ByteView notices = embedded_resource(L"HP2_NOTICES");
  payload.launcher.assign(launcher.begin(), launcher.end());
  payload.notices.assign(notices.begin(), notices.end());
  return payload;
}

std::string runtime_readme_text() {
  return "Need for Speed: Hot Pursuit 2 (PlayStation 2, NTSC-U) - PC build made by HotPursuit2PC\r\n"
         "========================================================================================\r\n"
         "\r\n"
         "START THE GAME:  HotPursuit2.exe  (in this folder)\r\n"
         "\r\n"
         "WHAT THIS BUILD IS\r\n"
         "  This is an EMULATOR-BACKED build. The original PlayStation 2 game code from your own\r\n"
         "  disc runs inside the open-source Play! emulator core, which is built into\r\n"
         "  HotPursuit2.exe. It is the PS2 game (handling, AI, cameras, menus), but it is NOT a\r\n"
         "  native Windows port. The native port is the project's later goal.\r\n"
         "  The game files are read from ..\\rom and are never modified. No BIOS file is needed.\r\n"
         "\r\n"
         "CONTROLS\r\n"
         "  Xbox-style controller          PS2 button      Keyboard\r\n"
         "  A / B / X / Y                  Cross / Circle / Square / Triangle    X / C / Z / S\r\n"
         "  D-pad, left stick              D-pad, left stick                     arrow keys\r\n"
         "  LB / RB, LT / RT               L1 / R1, L2 / R2                      Q / W, 1 / 2\r\n"
         "  Start / Back                   Start / Select                        Enter / Backspace\r\n"
         "  Alt+Enter or F11 switches between window and full screen.\r\n"
         "\r\n"
         "OPTIONS (add them after HotPursuit2.exe, e.g. in a shortcut)\r\n"
         "  --fullscreen        start in full screen\r\n"
         "  --res <1..8>        internal resolution multiplier (default 2 = 1280x896)\r\n"
         "  --size <W>x<H>      window size (default 1280x896)\r\n"
         "  --mute              no sound\r\n"
         "\r\n"
         "FRAME RATE\r\n"
         "  The picture is refreshed at the console's 60 Hz and the game runs at its original\r\n"
         "  speed. The game itself draws menus at 60 frames per second and races at 30; that is\r\n"
         "  how the game paces itself, not a limit of this build or of your PC.\r\n"
         "\r\n"
         "WHERE THINGS ARE\r\n"
         "  data\\               created on first start: settings, launcher.log\r\n"
         "  data\\vfs\\mc0\\       the PlayStation 2 memory card (your saved games)\r\n"
         "  ..\\rom\\             your game files (or a pointer to them)\r\n"
         "  THIRD-PARTY-NOTICES.txt   licenses of the software inside HotPursuit2.exe\r\n"
         "\r\n"
         "IF SOMETHING GOES WRONG\r\n"
         "  Look at data\\launcher.log. If the game files are reported as damaged, run the\r\n"
         "  HotPursuit2PC converter again on this game folder; it repairs what is missing.\r\n";
}

// ------------------------------------------------------------------ small enums

std::string_view to_string(Mode m) { return m == Mode::Portable ? "portable" : "installed"; }
std::string_view to_string(RomStorage s) { return s == RomStorage::Copy ? "copy" : "reference"; }

std::optional<Mode> parse_mode(std::string_view s) {
  if (s == "portable") return Mode::Portable;
  if (s == "installed") return Mode::Installed;
  return std::nullopt;
}

std::optional<RomStorage> parse_rom_storage(std::string_view s) {
  if (s == "copy") return RomStorage::Copy;
  if (s == "reference") return RomStorage::Reference;
  return std::nullopt;
}

// ---------------------------------------------------------------- KeyValueFile

KeyValueFile KeyValueFile::load(const fs::path& path) {
  KeyValueFile kv;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return kv;
  const Bytes raw = read_whole_file(path, 1u << 20);
  const std::string_view text(reinterpret_cast<const char*>(raw.data()), raw.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    const std::string line = trim(text.substr(pos, eol - pos));
    pos = eol + 1;
    if (line.empty() || line[0] == '#') continue;
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv.values_[trim(std::string_view(line).substr(0, eq))] = trim(std::string_view(line).substr(eq + 1));
  }
  return kv;
}

void KeyValueFile::save(const fs::path& path) const {
  std::string text = "# HotPursuit2PC game folder - settings and conversion state.\r\n"
                     "# Written by the converter; safe to read, edit only if you know why.\r\n";
  for (const auto& [k, v] : values_) text += k + "=" + v + "\r\n";
  write_file_replacing(path, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string KeyValueFile::get(std::string_view key, std::string_view fallback) const {
  const auto it = values_.find(key);
  return it == values_.end() ? std::string(fallback) : it->second;
}

bool KeyValueFile::has(std::string_view key) const { return values_.find(key) != values_.end(); }

void KeyValueFile::set(std::string_view key, std::string_view value) {
  std::string v(value);
  for (char& c : v)
    if (c == '\r' || c == '\n') c = ' ';
  values_[std::string(key)] = std::move(v);
}

// --------------------------------------------------------------------- inspect

bool GameFolderStatus::anything_done() const {
  return std::any_of(stages.begin(), stages.end(), [](const StageStatus& s) {
    return s.state == StageState::Done || s.state == StageState::Partial;
  });
}

std::string GameFolderStatus::describe() const {
  std::string s = std::format("Game folder: {}\n", to_utf8(root));
  if (!exists) return s + "  does not exist yet\n";
  if (foreign_content) return s + "  exists but is NOT a HotPursuit2PC game folder (contains other files)\n";
  if (!is_game_folder) return s + "  empty folder, nothing converted yet\n";
  s += std::format("  mode: {}   ROM storage: {}\n", to_string(mode), to_string(rom_storage));
  if (!rom_source.empty()) s += std::format("  ROM taken from: {}\n", rom_source);
  for (const StageStatus& st : stages) {
    const char* mark = st.state == StageState::Done      ? "done"
                       : st.state == StageState::Partial ? "partial"
                       : st.state == StageState::NotAvailable ? "n/a" : "to do";
    s += std::format("  [{:7}] {:28} {}\n", mark, st.title, st.detail);
  }
  if (!last_error.empty()) s += std::format("  last error: {}\n", last_error);
  return s;
}

GameFolderStatus inspect_game_folder(const fs::path& game_dir, const RomProfile& profile) {
  GameFolderStatus st;
  st.root = game_dir;
  const GameLayout layout{game_dir};
  std::error_code ec;
  st.exists = fs::exists(game_dir, ec);
  KeyValueFile cfg;
  if (st.exists) {
    if (fs::is_directory(game_dir, ec) && fs::is_regular_file(layout.state_file(), ec)) {
      try {
        cfg = KeyValueFile::load(layout.state_file());
        st.is_game_folder = cfg.get("format") == "1";
      } catch (const Error&) {
        st.is_game_folder = false;
      }
    }
    if (!st.is_game_folder)
      st.foreign_content = !fs::is_directory(game_dir, ec) || !dir_is_empty(game_dir);
  }
  if (st.is_game_folder) {
    st.mode = parse_mode(cfg.get("mode")).value_or(Mode::Portable);
    st.rom_storage = parse_rom_storage(cfg.get("rom_storage")).value_or(RomStorage::Copy);
    st.rom_source = cfg.get("rom_source");
    st.last_error = cfg.get("last_error");
  }

  StageStatus validate{"validate", "Check the ROM"};
  StageStatus rom{"rom", "Store the ROM"};
  StageStatus unpack{"unpack", "Unpack the game assets"};
  StageStatus build{"build", "Install the runnable game"};
  const fs::path launcher = layout.build() / from_utf8(kLauncherFileName);
  if (st.is_game_folder && fs::is_regular_file(launcher, ec)) {
    build.state = StageState::Done;
    build.detail = "build/HotPursuit2.exe (emulator-backed build)";
  } else if (embedded_resource(L"HP2_LAUNCHER").empty()) {
    build.state = StageState::NotAvailable;
    build.detail = "this converter was built without the game runtime";
  }

  if (st.is_game_folder) {
    if (cfg.has("stage.validate")) {
      validate.state = StageState::Done;
      validate.detail = cfg.get("stage.validate");
    }
    if (st.rom_storage == RomStorage::Copy) {
      std::size_t present = 0, required = 0, required_present = 0;
      for (const RomManifestEntry& e : profile.files) {
        const fs::path f = layout.rom() / from_utf8(e.path);
        const bool ok = fs::is_regular_file(f, ec) && fs::file_size(f, ec) == e.size;
        if (ok) ++present;
        if (!e.optional) {
          ++required;
          if (ok) ++required_present;
        }
      }
      rom.detail = std::format("{} of {} files present in rom/", present, profile.files.size());
      rom.state = required_present == required ? StageState::Done
                  : present ? StageState::Partial : StageState::NotStarted;
    } else if (fs::is_regular_file(layout.rom() / "rom-reference.tsv", ec)) {
      rom.state = StageState::Done;
      rom.detail = "referenced in place (rom/rom-reference.tsv)";
    }
    if (fs::is_regular_file(layout.assets() / "manifest.tsv", ec)) {
      unpack.state = StageState::Done;
      unpack.detail = std::format("{} files in assets/ (manifest.tsv written)",
                                  count_files(layout.assets()) - 1);
    } else if (fs::is_directory(layout.assets(), ec) && !dir_is_empty(layout.assets())) {
      unpack.state = StageState::Partial;
      unpack.detail = std::format("{} files so far, no manifest yet", count_files(layout.assets()));
    }
  }
  st.stages = {validate, rom, unpack, build};
  return st;
}

std::optional<fs::path> find_game_folder(const fs::path& base_dir) {
  std::error_code ec;
  std::vector<fs::path> found;
  for (const fs::directory_entry& de : fs::directory_iterator(base_dir, ec)) {
    std::error_code ec2;
    if (de.is_directory(ec2) && fs::is_regular_file(de.path() / kStateFileName, ec2))
      found.push_back(de.path());
  }
  if (found.empty()) return std::nullopt;
  std::sort(found.begin(), found.end());
  for (const fs::path& p : found)
    if (to_utf8(p.filename()) == kDefaultGameFolderName) return p;
  return found.front();
}

std::optional<std::string> why_bad_folder_name(std::string_view name) {
  if (name.empty()) return "The folder name is empty.";
  if (name.size() > 64) return "The folder name is longer than 64 characters.";
  if (name == "." || name == "..") return "'.' and '..' are not folder names.";
  for (char ch : name) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x20 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos)
      return "The folder name contains a character Windows does not allow (< > : \" / \\ | ? *).";
  }
  if (name.back() == ' ' || name.back() == '.' || name.front() == ' ')
    return "The folder name must not start with a space or end with a space or dot.";
  std::string stem(name.substr(0, name.find('.')));
  for (char& c : stem)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
                                    "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2",
                                    "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  for (const char* r : kReserved)
    if (stem == r) return std::format("'{}' is a name reserved by Windows.", name);
  return std::nullopt;
}

// --------------------------------------------------------------------- convert

ConvertResult convert(const ConvertRequest& request, const ConvertEvents& events,
                      const RomProfile& profile) {
  ConvertResult result;
  Logger log(events);
  std::string stage = "prepare";
  bool folder_is_ours = false;
  GameLayout layout;
  KeyValueFile cfg;

  const auto check_cancel = [&] {
    if (events.cancelled && events.cancelled()) throw Cancelled{};
  };
  const auto report = [&](std::string_view stage_id, std::string_view item, std::uint64_t done,
                          std::uint64_t total) {
    check_cancel();
    if (events.progress) events.progress(Progress{stage_id, item, done, total});
  };

  try {
    if (request.game_dir.empty()) fail("no game folder was given");
    const fs::path game_dir = normalised(request.game_dir);
    layout.root = game_dir;
    if (request.mode == Mode::Portable && request.rom_storage == RomStorage::Reference)
      fail("portable mode keeps a copy of the ROM inside the game folder so that the folder can be "
           "moved; choose installed mode if the ROM should stay where it is");

    // ---- 1. validate the ROM (reads only) ------------------------------------------
    stage = "validate";
    log.line(std::format("HotPursuit2PC converter {} - {} mode, ROM storage: {}", kConverterVersion,
                         to_string(request.mode), to_string(request.rom_storage)));
    // A portable game folder is self-contained: if the original ROM folder is
    // gone (or none was given) and a complete copy sits in <game>/rom, use it.
    fs::path rom_dir = request.rom_dir;
    bool using_internal_copy = false;
    {
      std::error_code ec0;
      if (rom_dir.empty() || !fs::exists(rom_dir, ec0)) {
        const GameFolderStatus st = inspect_game_folder(game_dir, profile);
        if (st.is_game_folder && st.rom_storage == RomStorage::Copy && st.stages.size() > 1 &&
            st.stages[1].state == StageState::Done) {
          log.line(rom_dir.empty()
                       ? std::string("No ROM folder given; using the copy inside the game folder.")
                       : std::format("The ROM folder '{}' is not available; using the copy inside "
                                     "the game folder.",
                                     to_utf8(rom_dir)));
          rom_dir = layout.rom();
          using_internal_copy = true;
        }
      }
    }
    log.line(std::format("Checking the ROM folder ({} check) ...", request.full_check ? "full" : "quick"));
    const RomReport rom = check_rom(
        rom_dir, request.full_check ? RomCheckLevel::Full : RomCheckLevel::Quick, profile,
        [&](const Progress& p) { report("validate", p.item, p.done, p.total); });
    log.block(rom.summary());
    if (!rom.ok()) {
      std::string all;
      for (const std::string& e : rom.errors) all += "\n  - " + e;
      fail("the ROM folder cannot be used:{}", all);
    }
    const fs::path rom_root = normalised(rom.root);

    // ---- 2. the game folder -------------------------------------------------------
    stage = "prepare";
    if (is_same_or_inside(game_dir, rom_root))
      fail("the game folder '{}' would be inside the ROM folder '{}'. The ROM folder is never "
           "written to - choose a different location",
           to_utf8(game_dir), to_utf8(rom_root));
    const GameFolderStatus before = inspect_game_folder(game_dir, profile);
    if (before.foreign_content)
      fail("'{}' already exists and is not a HotPursuit2PC game folder (it contains other files). "
           "Choose an empty folder or a different name - nothing was changed",
           to_utf8(game_dir));
    if (before.is_game_folder) {
      if (before.rom_storage != request.rom_storage)
        fail("this game folder was created with ROM storage '{}' but '{}' was requested. "
             "Use the same setting to resume, or choose a new folder",
             to_string(before.rom_storage), to_string(request.rom_storage));
      log.line("Existing conversion found in this folder - resuming. Completed work is kept.");
      log.block(before.describe());
    }
    std::error_code ec;
    for (const fs::path& d : {layout.root, layout.rom(), layout.assets(), layout.logs()}) {
      fs::create_directories(d, ec);
      if (ec) fail("cannot create folder '{}': {}", to_utf8(d), ec.message());
    }
    folder_is_ours = true;
    log.open(layout.logs() / std::format("convert-{}.log", file_stamp()));
    result.log_file = log.path();

    cfg = KeyValueFile::load(layout.state_file());
    cfg.set("format", "1");
    cfg.set("converter_version", kConverterVersion);
    cfg.set("disc", profile.serial);
    cfg.set("mode", to_string(request.mode));
    cfg.set("rom_storage", to_string(request.rom_storage));
    if (!using_internal_copy) cfg.set("rom_source", to_utf8(rom_root));
    if (!cfg.has("created")) cfg.set("created", timestamp());
    cfg.set("updated", timestamp());
    cfg.set("last_error", "");
    cfg.set("stage.validate", std::format("done {} ({})", timestamp(),
                                          rom.fully_hashed ? "all files hashed" : "quick check"));
    cfg.save(layout.state_file());

    // ---- 3. free space -----------------------------------------------------------
    std::uint64_t needed = 0;
    if (request.rom_storage == RomStorage::Copy)
      for (const RomManifestEntry& e : profile.files) {
        const fs::path dst = layout.rom() / from_utf8(e.path);
        if (!(fs::is_regular_file(dst, ec) && fs::file_size(dst, ec) == e.size)) needed += e.size;
      }
    if (request.unpack_assets && profile.has_z_fileset &&
        !fs::is_regular_file(layout.assets() / "manifest.tsv", ec))
      if (const RomManifestEntry* zz = profile.find("NFSHP2/ZZDATA.BIN")) needed += zz->size;
    const fs::space_info space = fs::space(layout.root, ec);
    if (!ec && space.available < needed + kSpaceMargin)
      fail("not enough free disk space for '{}': about {} are needed, {} are free",
           to_utf8(layout.root), human_size(needed + kSpaceMargin), human_size(space.available));

    // ---- 4. store the ROM ------------------------------------------------------------
    stage = "rom";
    if (request.rom_storage == RomStorage::Copy) {
      log.line(std::format("Copying the ROM into '{}' ...", to_utf8(layout.rom())));
      const std::uint64_t total = profile.total_size();
      std::uint64_t done = 0;
      std::uint32_t copied = 0, kept = 0;
      Bytes buf(kChunk);
      for (const RomManifestEntry& e : profile.files) {
        const fs::path src = rom_root / from_utf8(e.path);
        const fs::path dst = layout.rom() / from_utf8(e.path);
        if (e.optional && !fs::is_regular_file(src, ec)) {
          done += e.size;
          continue;
        }
        report("rom", e.path, done, total);
        if (is_same_or_inside(dst, rom_root)) {  // the ROM folder *is* <game>/rom (re-run on its own copy)
          ++kept;
          done += e.size;
          continue;
        }
        bool need_copy = true;
        if (fs::exists(dst, ec)) {
          bool good = fs::is_regular_file(dst, ec) && fs::file_size(dst, ec) == e.size;
          if (good && (request.full_check || e.size <= kQuickHashLimit))
            good = sha256_of_path(dst, [&](std::size_t) { check_cancel(); }) == e.sha256;
          if (good) {
            need_copy = false;
            ++kept;
            done += e.size;
          } else if (request.repair) {
            const fs::path aside = move_aside(dst);
            log.line(std::format("'{}' in the game folder did not match the disc; kept it as '{}' "
                                 "and copying it again.",
                                 e.path, to_utf8(aside.filename())));
          } else {
            fail("'{}' already exists in the game folder but does not match the disc. Nothing was "
                 "overwritten. Run again with the repair option to move the old file aside and "
                 "copy it again",
                 to_utf8(dst));
          }
        }
        if (need_copy) {
          const ReadOnlyFile in(src);
          if (in.size() != e.size)
            fail("'{}' changed size during the conversion ({} bytes now, {} expected)",
                 to_utf8(src), in.size(), e.size);
          OutputFile out(dst);
          Sha256 h;
          std::uint64_t pos = 0;
          while (pos < e.size) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(e.size - pos, kChunk));
            in.read_at(pos, std::span<std::uint8_t>(buf.data(), n));
            h.update(ByteView(buf.data(), n));
            out.write(ByteView(buf.data(), n));
            pos += n;
            done += n;
            report("rom", e.path, done, total);
          }
          if (Sha256::hex(h.finish()) != e.sha256)
            fail("'{}' does not have the contents of the {} disc (SHA-256 mismatch while copying) - "
                 "the file is damaged or modified. Nothing was kept from this file",
                 to_utf8(src), profile.serial);
          out.commit();
          ++copied;
        }
      }
      report("rom", {}, total, total);
      log.line(std::format("ROM stored: {} file(s) copied and verified by SHA-256, {} already present.",
                           copied, kept));
      cfg.set("stage.rom", std::format("done {} ({} copied, {} kept)", timestamp(), copied, kept));
    } else {
      std::string text = "# HotPursuit2PC ROM reference: the ROM stays where it is.\n";
      text += "source\t" + to_utf8(rom_root) + "\n";
      for (const RomManifestEntry& e : profile.files)
        text += std::format("file\t{}\t{}\t{}\n", e.path, e.size, e.sha256);
      write_file_replacing(layout.rom() / "rom-reference.tsv",
                           ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
      log.line(std::format("ROM referenced in place: '{}'.", to_utf8(rom_root)));
      cfg.set("stage.rom", std::format("done {} (reference)", timestamp()));
    }
    cfg.set("updated", timestamp());
    cfg.save(layout.state_file());

    // ---- 5. unpack the assets --------------------------------------------------------
    stage = "unpack";
    if (request.unpack_assets && profile.has_z_fileset) {
      const fs::path source = request.rom_storage == RomStorage::Copy ? layout.rom() : rom_root;
      log.line(std::format("Unpacking the game assets into '{}' ...", to_utf8(layout.assets())));
      const ZFileset set = ZFileset::open(is_same_or_inside(layout.rom(), rom_root) ? rom_root : source);
      UnpackOptions opt;
      opt.repair = request.repair;
      const UnpackResult ur = unpack(set, layout.assets(), opt, [&](const UnpackProgress& p) {
        report("unpack", p.current, p.bytes_done, p.bytes_total);
      });
      for (const std::string& n : ur.notes) log.line(n);
      log.line(std::format("Assets unpacked: {} file(s) written, {} already present, {} repaired "
                           "({} of {} names known).",
                           ur.written, ur.kept, ur.repaired, set.named_count(), set.entries().size()));
      cfg.set("stage.unpack", std::format("done {} ({} written, {} kept)", timestamp(), ur.written, ur.kept));
    } else {
      log.line("Asset unpacking was skipped.");
    }
    cfg.set("updated", timestamp());
    cfg.save(layout.state_file());

    // ---- 6. build: install the launcher -------------------------------------------------
    stage = "build";
    const RuntimePayload runtime = request.runtime ? *request.runtime : load_embedded_runtime();
    if (!runtime.available()) {
      log.line("This converter was built without the game runtime, so the runnable game was not "
               "installed (stage 'build' skipped).");
      cfg.set("stage.build", "unavailable (converter built without the runtime)");
    } else {
      fs::create_directories(layout.build(), ec);
      if (ec) fail("cannot create folder '{}': {}", to_utf8(layout.build()), ec.message());
      const std::string readme = runtime_readme_text();
      const auto as_bytes = [](const std::string& s) {
        return ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
      };
      // Our own generated files: an older version is kept aside, never silently lost.
      const auto install = [&](std::string_view name, ByteView content) {
        const fs::path target = layout.build() / from_utf8(name);
        if (fs::exists(target, ec)) {
          bool same = fs::is_regular_file(target, ec) && fs::file_size(target, ec) == content.size();
          if (same) same = sha256_of_path(target, {}) == Sha256::hex_of(content);
          if (same) return false;
          const fs::path aside = move_aside(target);
          log.line(std::format("'build/{}' differed from this converter's version; the old file was "
                               "kept as '{}'.",
                               name, to_utf8(aside.filename())));
        }
        OutputFile out(target);
        out.write(content);
        out.commit();
        return true;
      };
      const bool wrote_launcher = install(kLauncherFileName, runtime.launcher);
      install("THIRD-PARTY-NOTICES.txt", as_bytes(runtime.notices));
      install("README.txt", as_bytes(readme));
      log.line(std::format("Runnable game {}: '{}'.", wrote_launcher ? "installed" : "already in place",
                           to_utf8(layout.build() / from_utf8(kLauncherFileName))));
      log.line("Note: this is an EMULATOR-BACKED build (the PS2 game code runs inside the Play! core "
               "built into the launcher). It is not a native port. See build/README.txt.");
      cfg.set("stage.build", std::format("done {} (emulator-backed launcher)", timestamp()));
    }
    cfg.set("updated", timestamp());
    cfg.save(layout.state_file());
    log.line("Done.");
    result.ok = true;
  } catch (const Cancelled&) {
    result.cancelled = true;
    result.failed_stage = stage;
    result.error = "Cancelled. Everything finished so far is kept; run the converter again to resume.";
  } catch (const std::exception& e) {
    result.failed_stage = stage;
    result.error = std::format("Stage '{}' failed: {}", stage_title(stage), e.what());
  }

  if (!result.ok) {
    try {
      log.line(result.cancelled ? result.error : "ERROR: " + result.error);
      if (folder_is_ours) {
        cfg.set("last_error", result.cancelled ? "" : result.error);
        cfg.set("updated", timestamp());
        cfg.save(layout.state_file());
      }
    } catch (const std::exception&) {
      // Reporting must not hide the original failure.
    }
  }
  if (!layout.root.empty()) result.status = inspect_game_folder(layout.root, profile);
  return result;
}

}  // namespace hp2
