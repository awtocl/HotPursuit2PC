// The launcher's work behind the screens (ADR 0016): where things are, the user's disc (find, copy, check), preparing
// the game (hp2recomp package), starting it, the memory card's backups and the screenshots. No drawing here.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace launcher {
namespace fs = std::filesystem;

// ---- places ----
struct Paths {
  fs::path dir;        // the game folder: the launcher's own folder (hp2native.ini, launcher.ini, saves, screenshots, log)
  fs::path game;       // HotPursuit2Native.exe
  fs::path recomp;     // hp2recomp.exe (empty: not found)
  fs::path package;    // hp2code.dll
  fs::path copiedDisc; // <dir>\disc: where a disc copied by the launcher goes
};
Paths FindPaths();

// ---- launcher.ini (next to the launcher) ----
struct Config {
  std::wstring disc;            // folder with the contents of the user's disc
  std::int64_t verified = 0;    // time the whole disc was checked against the manifest (0 never)
  int afterGame = 0;            // 0 show the launcher again, 1 close it
  std::int64_t playedSeconds = 0;
  std::int64_t lastPlayed = 0;  // time() of the last session's end
  int sessions = 0;
};
Config LoadConfig(const Paths& p);
void SaveConfig(const Paths& p, const Config& c);

// ---- the disc ----
enum class DiscState { Ok, Missing, NotDisc, OtherRegion, WrongVersion };
struct DiscCheck {
  DiscState state = DiscState::Missing;
  std::wstring detail;  // what is wrong, for the screen
};
// Quick check: the three items, and SLUS_203.62's size and SHA-256 (about 3 MB read).
DiscCheck QuickCheck(const fs::path& folder);
// Drives (DVD drives, mounted .iso images, USB sticks) whose root holds the game.
struct DiscDrive {
  std::wstring root;   // "E:\"
  std::wstring label;  // volume label
  bool optical = false;
};
std::vector<DiscDrive> FindDiscDrives();
bool IsOpticalDrive(const fs::path& folder);
std::uint64_t DiscBytes();  // total size of the manifest's files

// ---- background work, one at a time ----
enum class Job { None, Copy, Verify, Prepare, CheckPackage, Backup, Restore };
struct Progress {
  std::atomic<Job> job{Job::None};
  std::atomic<bool> running{false}, cancel{false};
  std::atomic<double> fraction{0};   // 0..1
  std::atomic<std::uint64_t> bytes{0}, total{0};
  std::atomic<double> started{0};    // seconds (Clock())
  mutable std::mutex m;
  std::wstring status;               // what is happening now
  std::wstring error;                // empty: success (valid once running is false)
  int result = 0;                    // job-specific (CheckPackage: 0 ok, 5 missing, 6 out of date)

  std::wstring Status() const { std::lock_guard l(m); return status; }
  std::wstring Error() const { std::lock_guard l(m); return error; }
  void SetStatus(std::wstring s) { std::lock_guard l(m); status = std::move(s); }
};
double Clock();
// Each starts a thread and returns at once (false when another job runs). `done` is posted to the window afterwards.
bool StartCopy(Progress& pr, const fs::path& from, const fs::path& to, void* window, unsigned done);
bool StartVerify(Progress& pr, const fs::path& folder, void* window, unsigned done);
bool StartPrepare(Progress& pr, const Paths& p, const fs::path& disc, void* window, unsigned done);
bool StartCheckPackage(Progress& pr, const Paths& p, const fs::path& disc, void* window, unsigned done);
void JoinWorker();  // at exit

// ---- the game ----
// Starts the game; the process handle is returned (nullptr: error in `error`).
void* StartGame(const Paths& p, const fs::path& disc, std::wstring& error);
// After the game ended: "" when it closed normally, otherwise the reason from its log.
std::wstring GameEndProblem(const Paths& p);

// ---- the memory card ----
struct SaveEntry {
  std::wstring name;
  std::uint64_t bytes = 0;
  std::int64_t time = 0;  // last change (time_t)
};
std::vector<SaveEntry> ListSaves(const Paths& p);    // what is on the card (saves\mc0)
std::vector<SaveEntry> ListBackups(const Paths& p);  // saves\backups\<name>, newest first
bool BackupSaves(const Paths& p, std::wstring& message);
// Restores a backup. The card's current contents are first kept as a backup of their own (nothing is deleted).
bool RestoreSaves(const Paths& p, const std::wstring& backup, std::wstring& message);

// ---- screenshots (F12 in the game) ----
std::vector<fs::path> ListScreenshots(const Paths& p);  // newest first

// ---- helpers ----
std::wstring FormatBytes(std::uint64_t bytes);
std::wstring FormatAgo(std::int64_t time);       // "2 days ago"
std::wstring FormatDuration(std::int64_t seconds);  // "3 h 12 min"
void OpenFolder(const fs::path& folder);
void OpenUrl(const wchar_t* url);

}  // namespace launcher
