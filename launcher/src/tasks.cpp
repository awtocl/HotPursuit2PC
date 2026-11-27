// The launcher's work behind the screens (ADR 0016). See tasks.h.
#include "tasks.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

namespace launcher {
namespace {

// The files of the supported disc (NTSC-U SLUS-20362), from converter/data/slus20362_rom_manifest.tsv (generated at build).
struct ManifestEntry {
  const wchar_t* path;  // relative, with '/'
  std::uint64_t size;
  const char* sha256;
};
const ManifestEntry kManifest[] = {
#include "disc_manifest.inc"
};

std::thread g_worker;

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
std::wstring Ini(const Paths& p) { return (p.dir / L"launcher.ini").wstring(); }
std::wstring ReadIni(const Paths& p, const wchar_t* section, const wchar_t* key) {
  std::wstring buffer(4096, L'\0');
  const DWORD n = GetPrivateProfileStringW(section, key, L"", buffer.data(), static_cast<DWORD>(buffer.size()), Ini(p).c_str());
  buffer.resize(n);
  return buffer;
}
std::int64_t ReadIniInt(const Paths& p, const wchar_t* section, const wchar_t* key) {
  const std::wstring v = ReadIni(p, section, key);
  return v.empty() ? 0 : _wtoi64(v.c_str());
}
void WriteIni(const Paths& p, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
  WritePrivateProfileStringW(section, key, value.c_str(), Ini(p).c_str());
}

// SHA-256 with the Windows crypto API (bcrypt.dll).
class Hasher {
 public:
  Hasher() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
  }
  ~Hasher() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;
  void Update(const void* data, std::size_t n) {
    if (hash_) BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(n), 0);
  }
  std::string Hex() {
    unsigned char d[32] = {};
    if (hash_) BCryptFinishHash(hash_, d, 32, 0);
    std::string s;
    char b[3];
    for (unsigned char c : d) {
      snprintf(b, sizeof b, "%02x", c);
      s += b;
    }
    return s;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// Hashes a file; `onBytes` is called after every block (false: cancelled).
template <class F>
bool HashFile(const fs::path& file, std::string& hex, F onBytes) {
  HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  Hasher hasher;
  std::vector<unsigned char> buffer(1 << 20);
  DWORD got = 0;
  bool ok = true;
  while (ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got) {
    hasher.Update(buffer.data(), got);
    if (!onBytes(got)) {
      ok = false;
      break;
    }
  }
  CloseHandle(h);
  hex = hasher.Hex();
  return ok;
}

const ManifestEntry& Executable() {
  for (const ManifestEntry& e : kManifest)
    if (std::wstring(e.path) == L"SLUS_203.62") return e;
  return kManifest[0];
}

void Finish(Progress& pr, std::wstring error, void* window, unsigned done, int result = 0) {
  {
    std::lock_guard l(pr.m);
    pr.error = std::move(error);
    pr.result = result;
  }
  pr.running = false;
  PostMessageW(static_cast<HWND>(window), done, static_cast<WPARAM>(pr.job.load()), 0);
}

bool Begin(Progress& pr, Job job) {
  if (pr.running) return false;
  if (g_worker.joinable()) g_worker.join();
  pr.job = job;
  pr.running = true;
  pr.cancel = false;
  pr.fraction = 0;
  pr.bytes = 0;
  pr.total = 0;
  pr.started = Clock();
  {
    std::lock_guard l(pr.m);
    pr.status.clear();
    pr.error.clear();
    pr.result = 0;
  }
  return true;
}

std::int64_t ToTimeT(fs::file_time_type t) {
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
  return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

std::wstring Stamp() {
  const std::time_t now = std::time(nullptr);
  std::tm t = {};
  localtime_s(&t, &now);
  wchar_t s[32];
  wcsftime(s, 32, L"%Y-%m-%d %H-%M-%S", &t);
  return s;
}

bool CopyTree(const fs::path& from, const fs::path& to, std::wstring& error) {
  std::error_code ec;
  fs::create_directories(to, ec);
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) error = Widen(ec.message());
  return !ec;
}

}  // namespace

double Clock() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Paths FindPaths() {
  Paths p;
  std::wstring buffer(32768, L'\0');
  buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
  p.dir = fs::path(buffer).parent_path();
  if (const wchar_t* d = _wgetenv(L"HP2_LAUNCHER_DIR")) p.dir = d;  // tests: a folder of their own
  // A game folder holds everything; a build tree has the pieces in their own folders (development).
  auto first = [](std::initializer_list<fs::path> candidates) {
    for (const fs::path& c : candidates)
      if (fs::exists(c)) return c;
    return fs::path();
  };
  const fs::path exeDir = fs::path(buffer).parent_path();
  p.game = first({p.dir / L"HotPursuit2Native.exe", exeDir / L"HotPursuit2Native.exe", exeDir.parent_path() / L"native" / L"HotPursuit2Native.exe"});
  if (p.game.empty()) p.game = p.dir / L"HotPursuit2Native.exe";
  p.recomp = first({p.dir / L"hp2recomp.exe", exeDir / L"hp2recomp.exe", exeDir.parent_path() / L"recomp" / L"hp2recomp.exe",
                    p.dir.parent_path() / L".cache" / L"build" / L"release" / L"recomp" / L"hp2recomp.exe"});
  p.package = p.game.parent_path() / L"hp2code.dll";
  p.copiedDisc = p.dir / L"disc";
  return p;
}

Config LoadConfig(const Paths& p) {
  Config c;
  c.disc = ReadIni(p, L"disc", L"folder");
  c.verified = ReadIniInt(p, L"disc", L"verified");
  c.afterGame = static_cast<int>(std::clamp<std::int64_t>(ReadIniInt(p, L"launcher", L"after_game"), 0, 1));
  c.playedSeconds = ReadIniInt(p, L"stats", L"played_seconds");
  c.lastPlayed = ReadIniInt(p, L"stats", L"last_played");
  c.sessions = static_cast<int>(ReadIniInt(p, L"stats", L"sessions"));
  return c;
}

void SaveConfig(const Paths& p, const Config& c) {
  WriteIni(p, L"disc", L"folder", c.disc);
  WriteIni(p, L"disc", L"verified", std::to_wstring(c.verified));
  WriteIni(p, L"launcher", L"after_game", std::to_wstring(c.afterGame));
  WriteIni(p, L"stats", L"played_seconds", std::to_wstring(c.playedSeconds));
  WriteIni(p, L"stats", L"last_played", std::to_wstring(c.lastPlayed));
  WriteIni(p, L"stats", L"sessions", std::to_wstring(c.sessions));
}

DiscCheck QuickCheck(const fs::path& folder) {
  DiscCheck r;
  std::error_code ec;
  if (folder.empty() || !fs::is_directory(folder, ec)) {
    r.detail = L"The folder was not found.";
    return r;
  }
  const fs::path exe = folder / L"SLUS_203.62";
  if (!fs::exists(folder / L"SYSTEM.CNF", ec)) {
    r.state = DiscState::NotDisc;
    r.detail = L"This folder does not hold the contents of a PlayStation 2 disc (SYSTEM.CNF is missing).";
    return r;
  }
  if (!fs::exists(exe, ec)) {
    std::ifstream cnf(folder / L"SYSTEM.CNF");
    std::string line, code;
    while (std::getline(cnf, line))
      if (const auto at = line.find("cdrom0:\\"); at != std::string::npos) code = line.substr(at + 8, 11);
    r.state = code.empty() ? DiscState::NotDisc : DiscState::OtherRegion;
    r.detail = code.empty() ? L"SLUS_203.62 is missing: this is not the North American Hot Pursuit 2 disc."
                            : L"This disc is " + Widen(code) + L". Only the North American release (SLUS-20362) is supported.";
    return r;
  }
  if (!fs::exists(folder / L"NFSHP2", ec)) {
    r.state = DiscState::NotDisc;
    r.detail = L"The NFSHP2 folder is missing: the copy of the disc is incomplete.";
    return r;
  }
  const ManifestEntry& e = Executable();
  std::string hex;
  if (fs::file_size(exe, ec) != e.size || !HashFile(exe, hex, [](DWORD) { return true; }) || hex != e.sha256) {
    r.state = DiscState::WrongVersion;
    r.detail = L"SLUS_203.62 is not the supported version (another edition or a damaged copy).";
    return r;
  }
  r.state = DiscState::Ok;
  return r;
}

std::vector<DiscDrive> FindDiscDrives() {
  std::vector<DiscDrive> out;
  const DWORD mask = GetLogicalDrives();
  for (int i = 2; i < 26; ++i) {  // C: onwards (A: and B: would wake floppy drives)
    if (!(mask & (1u << i))) continue;
    const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
    const UINT type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_CDROM && type != DRIVE_REMOVABLE) continue;  // fixed disks: the user chooses a folder
    const UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);  // an empty drive must not show a system dialog
    wchar_t label[MAX_PATH + 1] = {};
    const bool ready = GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) != 0;
    std::error_code ec;
    const bool game = ready && fs::exists(fs::path(root) / L"SYSTEM.CNF", ec) && fs::exists(fs::path(root) / L"NFSHP2", ec);
    SetErrorMode(oldMode);
    if (game) out.push_back({root, label, type == DRIVE_CDROM});
  }
  return out;
}

bool IsOpticalDrive(const fs::path& folder) {
  const std::wstring root = folder.root_path().wstring();
  return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_CDROM;
}

std::uint64_t DiscBytes() {
  std::uint64_t n = 0;
  for (const ManifestEntry& e : kManifest) n += e.size;
  return n;
}

bool StartCopy(Progress& pr, const fs::path& from, const fs::path& to, void* window, unsigned done) {
  if (!Begin(pr, Job::Copy)) return false;
  g_worker = std::thread([&pr, from, to, window, done] {
    // Everything on the disc (the manifest's files and anything else at its root, e.g. the manual's PDF on some discs).
    std::vector<std::pair<fs::path, std::uint64_t>> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      const std::wstring name = it->path().filename().wstring();
      if (it->is_directory(ec) && (name == L"System Volume Information" || name == L"$RECYCLE.BIN")) {
        it.disable_recursion_pending();
        continue;
      }
      if (it->is_regular_file(ec)) files.emplace_back(it->path(), it->file_size(ec));
    }
    if (files.empty()) return Finish(pr, L"Nothing could be read from " + from.wstring() + L".", window, done);
    std::uint64_t total = 0;
    for (const auto& f : files) total += f.second;
    pr.total = total;
    std::uint64_t base = 0;
    for (const auto& [file, size] : files) {
      const fs::path target = to / fs::relative(file, from, ec);
      fs::create_directories(target.parent_path(), ec);
      pr.SetStatus(L"Copying " + fs::relative(file, from, ec).wstring());
      struct Ctx { Progress* pr; std::uint64_t base, total; } ctx{&pr, base, total};
      auto routine = [](LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                        LPVOID data) -> DWORD {
        auto* c = static_cast<Ctx*>(data);
        c->pr->bytes = c->base + static_cast<std::uint64_t>(done.QuadPart);
        c->pr->fraction = c->total ? static_cast<double>(c->pr->bytes) / static_cast<double>(c->total) : 0;
        return c->pr->cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
      };
      SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);  // a copy left by an earlier, interrupted run
      if (!CopyFileExW(file.c_str(), target.c_str(), routine, &ctx, nullptr, 0)) {
        const DWORD e = GetLastError();
        if (pr.cancel || e == ERROR_REQUEST_ABORTED) return Finish(pr, L"Copy cancelled.", window, done);
        return Finish(pr, L"Copying " + file.filename().wstring() + L" failed (Windows error " + std::to_wstring(e) +
                              L"). Is the disc clean, and is there enough free space?", window, done);
      }
      SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);  // files from a DVD are read-only
      base += size;
    }
    pr.fraction = 1;
    Finish(pr, L"", window, done);
  });
  return true;
}

bool StartVerify(Progress& pr, const fs::path& folder, void* window, unsigned done) {
  if (!Begin(pr, Job::Verify)) return false;
  g_worker = std::thread([&pr, folder, window, done] {
    pr.total = DiscBytes();
    std::uint64_t base = 0;
    for (const ManifestEntry& e : kManifest) {
      fs::path file = folder;
      for (const wchar_t* s = e.path; *s;) {  // '/'-separated
        const wchar_t* t = s;
        while (*t && *t != L'/') ++t;
        file /= std::wstring(s, t);
        s = *t ? t + 1 : t;
      }
      pr.SetStatus(std::wstring(L"Checking ") + e.path);
      std::error_code ec;
      if (!fs::exists(file, ec)) return Finish(pr, std::wstring(e.path) + L" is missing from the disc folder.", window, done);
      if (fs::file_size(file, ec) != e.size)
        return Finish(pr, std::wstring(e.path) + L" has the wrong size: the copy is incomplete or damaged.", window, done);
      std::string hex;
      const bool read = HashFile(file, hex, [&](DWORD n) {
        pr.bytes += n;
        pr.fraction = static_cast<double>(pr.bytes) / static_cast<double>(pr.total);
        return !pr.cancel.load();
      });
      if (pr.cancel) return Finish(pr, L"Check cancelled.", window, done);
      if (!read) return Finish(pr, std::wstring(e.path) + L" could not be read.", window, done);
      if (hex != e.sha256)
        return Finish(pr, std::wstring(e.path) + L" differs from the original disc: copy the disc again.", window, done);
      base += e.size;
      pr.bytes = base;
    }
    pr.fraction = 1;
    Finish(pr, L"", window, done);
  });
  return true;
}

bool StartPrepare(Progress& pr, const Paths& p, const fs::path& disc, void* window, unsigned done) {
  if (!Begin(pr, Job::Prepare)) return false;
  g_worker = std::thread([&pr, p, disc, window, done] {
    if (p.recomp.empty())
      return Finish(pr, L"hp2recomp.exe was not found next to the launcher. Install the game again (docs/GETTING-STARTED.md).",
                    window, done);
    pr.SetStatus(L"Reading the game's program");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    CreatePipe(&readEnd, &writeEnd, &sa, 0);
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + p.recomp.wstring() + L"\" package \"" + (disc / L"SLUS_203.62").wstring() + L"\" \"" +
                       p.package.wstring() + L"\"";
    const BOOL started = CreateProcessW(p.recomp.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, p.dir.c_str(), &si, &pi);
    CloseHandle(writeEnd);
    if (!started) {
      CloseHandle(readEnd);
      return Finish(pr, L"hp2recomp.exe could not be started (Windows error " + std::to_wstring(GetLastError()) + L").", window, done);
    }
    // Phases, from its messages: translating (about 10% of the time), compiling (about 90%), linking.
    std::FILE* log = _wfopen((p.dir / L"prepare.log").c_str(), L"wb");
    std::string pending, last;
    std::atomic<double> phaseStart{0.0}, phaseEnd{0.08}, phaseBegan{Clock()}, expected{4.0};  // also read by the ticker
    char buffer[4096];
    DWORD got = 0;
    auto line = [&](const std::string& l) {
      if (log) fprintf(log, "%s\n", l.c_str());
      if (!l.empty()) last = l;
      if (l.rfind("Translated ", 0) == 0) {
        phaseStart = 0.08, phaseEnd = 0.97, phaseBegan = Clock();
        expected = 26.0 * 16.0 / std::max(1u, std::thread::hardware_concurrency());
        pr.SetStatus(L"Compiling the game's code for your processor");
      } else if (l.rfind("Compiled ", 0) == 0) {
        phaseStart = 0.97, phaseEnd = 1.0, phaseBegan = Clock(), expected = 1.0;
        pr.SetStatus(L"Linking");
      } else if (l.rfind("game patch", 0) == 0 || l.rfind("Relocated", 0) == 0) {
        pr.SetStatus(L"Translating the game's program");
      }
    };
    // The pipe is read on this thread; the bar moves on between messages (an estimate that never reaches the next phase).
    std::thread ticker([&] {
      while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
        const double t = (Clock() - phaseBegan) / expected;
        pr.fraction = phaseStart + (phaseEnd - phaseStart) * (1.0 - 1.0 / (1.0 + t * 1.2));
        if (pr.cancel) TerminateProcess(pi.hProcess, 1);
      }
    });
    while (ReadFile(readEnd, buffer, sizeof buffer, &got, nullptr) && got) {
      pending.append(buffer, got);
      for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
        std::string l = pending.substr(0, nl);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        line(l);
        pending.erase(0, nl + 1);
      }
    }
    if (!pending.empty()) line(pending);
    ticker.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readEnd);
    if (log) fclose(log);
    if (pr.cancel) return Finish(pr, L"Cancelled.", window, done);
    if (code != 0) return Finish(pr, L"Preparing the game failed: " + Widen(last) + L" (details in prepare.log)", window, done);
    pr.fraction = 1;
    Finish(pr, L"", window, done);
  });
  return true;
}

bool StartCheckPackage(Progress& pr, const Paths& p, const fs::path& disc, void* window, unsigned done) {
  if (!Begin(pr, Job::CheckPackage)) return false;
  g_worker = std::thread([&pr, p, disc, window, done] {
    std::error_code ec;
    if (!fs::exists(p.package, ec)) return Finish(pr, L"", window, done, 5);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + p.game.wstring() + L"\" --check-package --rom \"" + disc.wstring() + L"\"";
    if (!CreateProcessW(p.game.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, p.game.parent_path().c_str(), &si, &pi))
      return Finish(pr, L"HotPursuit2Native.exe could not be started.", window, done, 5);
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 6;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    Finish(pr, L"", window, done, code == 0 ? 0 : code == 5 ? 5 : 6);
  });
  return true;
}

void JoinWorker() {
  if (g_worker.joinable()) g_worker.join();
}

void* StartGame(const Paths& p, const fs::path& disc, std::wstring& error) {
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + p.game.wstring() + L"\" --rom \"" + disc.wstring() + L"\"";
  if (!CreateProcessW(p.game.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, p.game.parent_path().c_str(), &si, &pi)) {
    error = L"HotPursuit2Native.exe could not be started (Windows error " + std::to_wstring(GetLastError()) + L").";
    return nullptr;
  }
  CloseHandle(pi.hThread);
  AllowSetForegroundWindow(pi.dwProcessId);
  return pi.hProcess;
}

std::wstring GameEndProblem(const Paths& p) {
  std::ifstream log(p.game.parent_path() / L"native.log");
  std::string l, stopped, error;
  while (std::getline(log, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (l.rfind("Stopped: ", 0) == 0) stopped = l.substr(9);
    if (l.rfind("error: ", 0) == 0) error = l.substr(7);
  }
  if (!error.empty()) return Widen(error);
  if (stopped.empty()) return L"The game ended without a message (it may have crashed).";
  if (stopped == "the window was closed") return L"";
  return Widen(stopped);
}

std::vector<SaveEntry> ListSaves(const Paths& p) {
  std::vector<SaveEntry> out;
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(p.game.parent_path() / L"saves" / L"mc0", ec)) {
    SaveEntry s{d.path().filename().wstring()};
    s.time = ToTimeT(d.last_write_time(ec));
    if (d.is_directory(ec)) {
      for (const auto& f : fs::recursive_directory_iterator(d.path(), ec))
        if (f.is_regular_file(ec)) {
          s.bytes += f.file_size(ec);
          s.time = std::max(s.time, ToTimeT(f.last_write_time(ec)));
        }
    } else {
      s.bytes = d.file_size(ec);
    }
    out.push_back(s);
  }
  return out;
}

std::vector<SaveEntry> ListBackups(const Paths& p) {
  std::vector<SaveEntry> out;
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(p.game.parent_path() / L"saves" / L"backups", ec)) {
    if (!d.is_directory(ec)) continue;
    SaveEntry s{d.path().filename().wstring()};
    s.time = ToTimeT(d.last_write_time(ec));
    for (const auto& f : fs::recursive_directory_iterator(d.path(), ec))
      if (f.is_regular_file(ec)) s.bytes += f.file_size(ec);
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const SaveEntry& a, const SaveEntry& b) { return a.name > b.name; });
  return out;
}

bool BackupSaves(const Paths& p, std::wstring& message) {
  const fs::path saves = p.game.parent_path() / L"saves";
  std::error_code ec;
  if (ListSaves(p).empty()) {
    message = L"The memory card is empty: nothing to back up.";
    return false;
  }
  const std::wstring name = Stamp();
  if (!CopyTree(saves / L"mc0", saves / L"backups" / name / L"mc0", message)) return false;
  message = L"Backup saved: " + name;
  return true;
}

bool RestoreSaves(const Paths& p, const std::wstring& backup, std::wstring& message) {
  const fs::path saves = p.game.parent_path() / L"saves";
  std::error_code ec;
  const fs::path source = saves / L"backups" / backup / L"mc0";
  if (!fs::is_directory(source, ec)) {
    message = L"That backup is empty.";
    return false;
  }
  if (!ListSaves(p).empty()) {  // keep what is on the card now
    const fs::path keep = saves / L"backups" / (Stamp() + L" (before restoring)");
    fs::create_directories(keep, ec);
    fs::rename(saves / L"mc0", keep / L"mc0", ec);
    if (ec) {
      message = L"The memory card could not be moved aside: " + Widen(ec.message());
      return false;
    }
  }
  if (!CopyTree(source, saves / L"mc0", message)) return false;
  message = L"Restored the backup of " + backup;
  return true;
}

std::vector<fs::path> ListScreenshots(const Paths& p) {
  std::vector<std::pair<fs::file_time_type, fs::path>> found;
  std::error_code ec;
  for (const auto& f : fs::directory_iterator(p.game.parent_path() / L"screenshots", ec)) {
    std::wstring ext = f.path().extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (f.is_regular_file(ec) && (ext == L".png" || ext == L".bmp" || ext == L".jpg")) found.emplace_back(f.last_write_time(ec), f.path());
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<fs::path> out;
  for (auto& f : found) out.push_back(f.second);
  return out;
}

std::wstring FormatBytes(std::uint64_t bytes) {
  wchar_t s[32];
  if (bytes >= (1ull << 30)) swprintf(s, 32, L"%.1f GB", static_cast<double>(bytes) / (1ull << 30));
  else if (bytes >= (1ull << 20)) swprintf(s, 32, L"%.1f MB", static_cast<double>(bytes) / (1ull << 20));
  else if (bytes >= 1024) swprintf(s, 32, L"%llu KB", static_cast<unsigned long long>(bytes / 1024));
  else swprintf(s, 32, L"%llu bytes", static_cast<unsigned long long>(bytes));
  return s;
}

std::wstring FormatAgo(std::int64_t time) {
  const std::int64_t d = std::time(nullptr) - time;
  auto plural = [](std::int64_t n, const wchar_t* unit) {
    return std::to_wstring(n) + L" " + unit + (n == 1 ? L"" : L"s") + L" ago";
  };
  if (d < 90) return L"just now";
  if (d < 3600) return plural(d / 60, L"minute");
  if (d < 86400) return plural(d / 3600, L"hour");
  if (d < 86400 * 60) return plural(d / 86400, L"day");
  return plural(d / (86400 * 30), L"month");
}

std::wstring FormatDuration(std::int64_t seconds) {
  if (seconds < 60) return L"less than a minute";
  const std::int64_t h = seconds / 3600, m = (seconds % 3600) / 60;
  return h ? std::to_wstring(h) + L" h " + std::to_wstring(m) + L" min" : std::to_wstring(m) + L" min";
}

void OpenFolder(const fs::path& folder) {
  std::error_code ec;
  fs::create_directories(folder, ec);
  ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenUrl(const wchar_t* url) { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); }

}  // namespace launcher
