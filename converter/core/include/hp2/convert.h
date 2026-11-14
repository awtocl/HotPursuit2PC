// The converter itself (brief §3): one game folder, portable or installed,
// resumable stages, visible progress, errors that name the stage and file.
// The CLI and the GUI are thin front-ends over this header (ADR 0003).
#pragma once

#include <map>
#include <optional>

#include "hp2/rom.h"

namespace hp2 {

inline constexpr std::string_view kConverterVersion = "0.1.0";
inline constexpr std::string_view kDefaultGameFolderName = "game";
inline constexpr std::string_view kStateFileName = "hp2pc.cfg";

// Layout inside the game folder (C6: everything lives here).
//   hp2pc.cfg   settings + stage state (marks the folder as ours)
//   rom/        copy of the ROM, or rom-reference.tsv pointing at it
//   assets/     unpacked Z fileset + manifest.tsv
//   build/      the runnable game: HotPursuit2.exe, README.txt, notices,
//               and (created by the game) data/ with settings and memory cards
//   logs/       one log per conversion run
struct GameLayout {
  fs::path root;
  fs::path state_file() const { return root / kStateFileName; }
  fs::path rom() const { return root / "rom"; }
  fs::path assets() const { return root / "assets"; }
  fs::path build() const { return root / "build"; }
  fs::path logs() const { return root / "logs"; }
};

enum class Mode { Portable, Installed };
enum class RomStorage {
  Copy,       // the ROM is copied into <game>/rom (self-contained, relocatable)
  Reference,  // <game>/rom holds only a manifest pointing at the original folder
};

std::string_view to_string(Mode m);
std::string_view to_string(RomStorage s);
std::optional<Mode> parse_mode(std::string_view s);
std::optional<RomStorage> parse_rom_storage(std::string_view s);

// Small "key=value" text file, written atomically. Used for hp2pc.cfg.
class KeyValueFile {
 public:
  static KeyValueFile load(const fs::path& path);  // missing file -> empty
  void save(const fs::path& path) const;

  std::string get(std::string_view key, std::string_view fallback = {}) const;
  bool has(std::string_view key) const;
  void set(std::string_view key, std::string_view value);
  const std::map<std::string, std::string, std::less<>>& values() const { return values_; }

 private:
  std::map<std::string, std::string, std::less<>> values_;
};

// --------------------------------------------------------------------- inspect

enum class StageState { NotStarted, Partial, Done, NotAvailable };

struct StageStatus {
  std::string id;      // "validate", "rom", "unpack", "build", "code"
  std::string title;   // human readable
  StageState state = StageState::NotStarted;
  std::string detail;  // e.g. "27 of 27 files present"
};

struct GameFolderStatus {
  fs::path root;
  bool exists = false;
  bool is_game_folder = false;   // has a readable hp2pc.cfg
  bool foreign_content = false;  // exists, not empty, and not ours -> must not be used
  Mode mode = Mode::Portable;
  RomStorage rom_storage = RomStorage::Copy;
  std::string rom_source;        // where the ROM was taken from
  std::string last_error;
  std::vector<StageStatus> stages;

  bool anything_done() const;
  std::string describe() const;  // multi-line, for the CLI "status" and the GUI
};

// Cheap, read-only look at a game folder: what is already done there?
GameFolderStatus inspect_game_folder(const fs::path& game_dir,
                                     const RomProfile& profile = RomProfile::slus20362());

// First sub-folder of base_dir that is a game folder (re-run without arguments).
std::optional<fs::path> find_game_folder(const fs::path& base_dir);

// A folder name the user may type at first run: one path component, no
// reserved characters. Returns an explanation if it is not acceptable.
std::optional<std::string> why_bad_folder_name(std::string_view name);

// --------------------------------------------------------------------- runtime

// What the "build" stage installs into <game>/build so that the game runs
// without the converter (brief §3.4). Milestone 1: an EMULATOR-BACKED
// launcher (ADR 0001/0005) - the README it writes says so.
struct RuntimePayload {
  Bytes launcher;        // HotPursuit2.exe
  std::string notices;   // THIRD-PARTY-NOTICES.txt
  bool available() const { return !launcher.empty(); }
};

inline constexpr std::string_view kLauncherFileName = "HotPursuit2.exe";

// The payload embedded in the running converter executable, or an empty
// payload if this converter was built without the engine.
RuntimePayload load_embedded_runtime();

// The text of <game>/build/README.txt.
std::string runtime_readme_text();

// --------------------------------------------------------------------- code

// The "code" stage (ADR 0009): the game program and its vector-unit microcode
// translated into native code once, stored as <game>/build/hp2code.dll. It
// needs the translator (LLVM), which only converters built with it carry: they
// register a builder at startup; without one the stage is unavailable and the
// game runs on the reference core.
struct CodeBuilder {
  // One line naming what would be built from this executable (format, hash,
  // layout, CPU level); a package whose recorded identity matches is kept.
  std::function<std::string(ByteView elf)> identity;
  // Builds the package; `log` gets progress lines. Throws on failure.
  std::function<void(ByteView elf, const fs::path& dll, const std::function<void(std::string_view)>& log)> build;
};
void register_code_builder(CodeBuilder builder);
const CodeBuilder* registered_code_builder();

inline constexpr std::string_view kCodeFileName = "hp2code.dll";
inline constexpr std::string_view kGameExecutable = "SLUS_203.62";

// --------------------------------------------------------------------- convert

struct ConvertRequest {
  fs::path rom_dir;                  // user's ROM folder (read-only)
  fs::path game_dir;                 // target game folder
  Mode mode = Mode::Portable;
  RomStorage rom_storage = RomStorage::Copy;
  bool full_check = false;           // hash the large ROM files before starting
  bool repair = false;               // move mismatching files aside and redo them
  bool unpack_assets = true;
  std::optional<RuntimePayload> runtime;  // default: load_embedded_runtime()
};

struct ConvertEvents {
  std::function<void(std::string_view)> log;  // one line, no trailing newline
  ProgressFn progress;
  std::function<bool()> cancelled;            // polled; return true to stop cleanly
};

struct ConvertResult {
  bool ok = false;
  bool cancelled = false;
  std::string failed_stage;   // id of the stage that failed, empty if ok
  std::string error;          // what failed and why
  fs::path log_file;
  GameFolderStatus status;    // state of the game folder after the run
};

// Runs validate -> rom -> unpack -> build -> code. Safe to call again on the same
// folder: work that is already complete is detected and kept. Never throws;
// failures are reported in the result and written to the log.
ConvertResult convert(const ConvertRequest& request, const ConvertEvents& events = {},
                      const RomProfile& profile = RomProfile::slus20362());

}  // namespace hp2
