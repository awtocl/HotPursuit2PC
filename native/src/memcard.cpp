// Memory card server (mcserv protocol; layouts measured on the reference core, which documents them in Iop_McServ.h).
// Commands: 01 GetInfo, 02 Open, 03 Close, 04 Seek, 05 Read, 06 Write, 0A Flush, 0C ChDir, 0D GetDir, 0E SetFileInfo,
// 0F Delete, 12 GetEntSpace (and their 0x7x aliases). Paths are card paths ("/BASLUS-20362.../FILE"); a name is kept as is
// on disk except the characters Windows does not allow, which are written as %XX.
#include "memcard.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace mc {
namespace fs = std::filesystem;
namespace {

fs::path g_root;                   // <saves>
std::string g_cwd = "/";           // current card directory (port 0)
bool g_known = false;              // GetInfo has reported the card once
std::map<std::uint32_t, FILE*> g_files;
std::uint32_t g_nextHandle = 1;
struct Entry { std::string name; bool dir; std::uint64_t size; fs::file_time_type time; };
std::vector<Entry> g_listing;      // GetDir continuation
std::size_t g_listPos = 0;

constexpr std::int32_t kNoEntry = -4, kDenied = -5, kNotEmpty = -6;
constexpr std::uint16_t kAttrDir = 0x8427, kAttrFile = 0x8497;

fs::path Card() { return g_root / "mc0"; }
fs::path ToPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string FromPath(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

std::string Escape(const std::string& part) {
  std::string out;
  for (unsigned char ch : part) {
    if (ch < 32 || std::strchr("<>:\"\\|?*%", ch)) {
      char buf[4];
      snprintf(buf, sizeof buf, "%%%02X", ch);
      out += buf;
    } else {
      out += static_cast<char>(ch);
    }
  }
  return out;
}
std::string Unescape(const std::string& part) {
  std::string out;
  for (std::size_t i = 0; i < part.size(); ++i) {
    if (part[i] == '%' && i + 2 < part.size()) {
      out += static_cast<char>(std::strtoul(part.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += part[i];
    }
  }
  return out;
}

// Card path (absolute or relative to the current directory) -> normalised card path components.
std::vector<std::string> Split(const std::string& name) {
  std::string full = name.empty() || name[0] != '/' ? g_cwd + "/" + name : name;
  std::vector<std::string> parts;
  std::string cur;
  for (char ch : full + "/") {
    if (ch == '/' || ch == '\\') {
      if (cur == "..") { if (!parts.empty()) parts.pop_back(); }
      else if (!cur.empty() && cur != ".") parts.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  return parts;
}
fs::path HostPath(const std::vector<std::string>& parts) {
  fs::path p = Card();
  for (const auto& s : parts) p /= ToPath(Escape(s));
  return p;
}

bool Match(const char* pat, const char* s) {  // card wildcards: * and ?
  if (*pat == 0) return *s == 0;
  if (*pat == '*') return Match(pat + 1, s) || (*s && Match(pat, s + 1));
  if (*s && (*pat == '?' || *pat == *s)) return Match(pat + 1, s + 1);
  return false;
}

void WriteTime(std::uint8_t* t, fs::file_time_type ft) {  // sec, min, hour, day, month, year (card times are JST; local is fine)
  const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
  const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
  std::tm tm = {};
  localtime_s(&tm, &tt);
  t[0] = 0;
  t[1] = static_cast<std::uint8_t>(tm.tm_sec);
  t[2] = static_cast<std::uint8_t>(tm.tm_min);
  t[3] = static_cast<std::uint8_t>(tm.tm_hour);
  t[4] = static_cast<std::uint8_t>(tm.tm_mday);
  t[5] = static_cast<std::uint8_t>(tm.tm_mon + 1);
  const std::uint16_t year = static_cast<std::uint16_t>(tm.tm_year + 1900);
  std::memcpy(t + 6, &year, 2);
}

std::string Name(const std::uint32_t* send, std::uint32_t sendSize) {  // CMD.name at byte 20, up to 0x400 bytes
  if (sendSize <= 20) return {};
  const char* p = reinterpret_cast<const char*>(send) + 20;
  return std::string(p, strnlen(p, std::min<std::size_t>(0x400, sendSize - 20)));
}

}  // namespace

void SetRoot(const fs::path& savesFolder) {
  g_root = savesFolder;
  std::error_code ec;
  fs::create_directories(Card(), ec);
}

bool Service(std::uint32_t fn, const std::uint32_t* send, std::uint32_t sendSize, std::uint32_t* recv, std::uint32_t recvSize,
             std::uint8_t* ram, std::uint32_t ramSize) {
  if (g_root.empty() || !send || sendSize < 32 || !recv || recvSize < 4) return false;
  auto guest = [&](std::uint32_t a, std::uint32_t n) -> std::uint8_t* {
    const std::uint32_t p = a & 0x1FFFFFFF;
    return (p + static_cast<std::uint64_t>(n) <= ramSize) ? ram + p : nullptr;
  };
  std::error_code ec;
  switch (fn & 0xFF) {
    case 0x01: case 0x78: {  // GetInfo: args [?, port, slot, want formatted, want free, want type, ?, result buffer]
      if (sendSize < 32) return false;
      const std::uint32_t port = send[1];
      if (port != 0) { recv[0] = static_cast<std::uint32_t>(-10); return true; }  // no card in slot 2
      if (std::uint8_t* out = guest(send[7], 0x94)) {
        std::uint32_t* w = reinterpret_cast<std::uint32_t*>(out);
        if (send[5]) w[0] = 2;           // PS2 card
        if (send[4]) w[1] = 0x1E00;      // free clusters (1 KB): an almost empty 8 MB card
        if (send[3]) w[0x24] = 1;        // formatted
      }
      recv[0] = g_known ? 0u : static_cast<std::uint32_t>(-1);  // first call: "new formatted card"
      g_known = true;
      return true;
    }
    case 0x02: case 0x71: {  // Open: CMD {port, slot, flags, max entries, table, name}
      if (sendSize < 24) return false;
      if (send[0] != 0) { recv[0] = static_cast<std::uint32_t>(kNoEntry); return true; }
      const auto parts = Split(Name(send, sendSize));
      const fs::path path = HostPath(parts);
      const std::uint32_t flags = send[2];
      if (flags == 0x40) {  // create a directory
        if (fs::exists(path, ec)) recv[0] = static_cast<std::uint32_t>(kNoEntry);
        else recv[0] = fs::create_directory(path, ec) ? 0u : static_cast<std::uint32_t>(kNoEntry);
        return true;
      }
      if (!fs::exists(path.parent_path(), ec)) { recv[0] = static_cast<std::uint32_t>(kNoEntry); return true; }
      const bool exists = fs::exists(path, ec);
      if (!exists && !(flags & 0x200)) { recv[0] = static_cast<std::uint32_t>(kNoEntry); return true; }
      FILE* f = nullptr;
      const bool truncate = !exists || (flags & 0x400);
      if (_wfopen_s(&f, path.c_str(), truncate ? L"w+b" : L"r+b") != 0 || !f) { recv[0] = static_cast<std::uint32_t>(kDenied); return true; }
      const std::uint32_t h = g_nextHandle++;
      g_files[h] = f;
      recv[0] = h;
      return true;
    }
    case 0x03: case 0x72: case 0x0A: case 0x7A: {  // Close / Flush: FILECMD {handle, ...}
      const auto it = g_files.find(send ? send[0] : 0);
      if (it == g_files.end()) { recv[0] = static_cast<std::uint32_t>(-1); return true; }
      fflush(it->second);
      if ((fn & 0xFF) == 0x03 || (fn & 0xFF) == 0x72) { fclose(it->second); g_files.erase(it); }
      recv[0] = 0;
      return true;
    }
    case 0x04: {  // Seek: FILECMD {handle, pad, pad, size, offset, origin}
      const auto it = g_files.find(send[0]);
      if (it == g_files.end()) { recv[0] = static_cast<std::uint32_t>(-1); return true; }
      _fseeki64(it->second, static_cast<std::int32_t>(send[4]), send[5] == 1 ? SEEK_CUR : send[5] == 2 ? SEEK_END : SEEK_SET);
      recv[0] = static_cast<std::uint32_t>(_ftelli64(it->second));
      return true;
    }
    case 0x05: case 0x73: {  // Read: FILECMD {handle, pad, pad, size, offset, origin, buffer, param}
      if (std::uint8_t* param = send[7] ? guest(send[7], 8) : nullptr) std::memset(param, 0, 8);  // no unaligned head/tail
      const auto it = g_files.find(send[0]);
      if (it == g_files.end()) { recv[0] = static_cast<std::uint32_t>(kDenied); return true; }
      std::uint8_t* dst = guest(send[6], send[3]);
      if (!dst) { recv[0] = static_cast<std::uint32_t>(kDenied); return true; }
      const std::size_t n = fread(dst, 1, send[3], it->second);
      _fseeki64(it->second, _ftelli64(it->second), SEEK_SET);  // allows a write after the read
      recv[0] = static_cast<std::uint32_t>(n);
      return true;
    }
    case 0x06: case 0x74: {  // Write: FILECMD {..., size, offset, origin = bytes in `data` first, buffer, param, data[16]}
      const auto it = g_files.find(send[0]);
      if (it == g_files.end()) { recv[0] = static_cast<std::uint32_t>(kDenied); return true; }
      std::uint32_t done = 0;
      const std::uint32_t head = std::min<std::uint32_t>(send[5], 16);
      if (head && sendSize >= 48) done += static_cast<std::uint32_t>(fwrite(send + 8, 1, head, it->second));
      if (const std::uint8_t* src = guest(send[6], send[3])) done += static_cast<std::uint32_t>(fwrite(src, 1, send[3], it->second));
      fflush(it->second);
      recv[0] = done;
      return true;
    }
    case 0x0C: {  // ChDir: CMD.name; the previous directory goes to CMD.table (if any)
      const auto parts = Split(Name(send, sendSize));
      if (!fs::is_directory(HostPath(parts), ec)) { recv[0] = static_cast<std::uint32_t>(kNoEntry); return true; }
      if (std::uint8_t* out = send[4] ? guest(send[4], 1024) : nullptr) {
        std::memset(out, 0, 1024);
        std::memcpy(out, g_cwd.c_str(), std::min<std::size_t>(g_cwd.size(), 1023));
      }
      g_cwd = "/";
      for (const auto& s : parts) g_cwd += (g_cwd.size() > 1 ? "/" : "") + s;
      recv[0] = 0;
      return true;
    }
    case 0x0D: case 0x76: {  // GetDir: CMD {port, slot, flags (0 = new search), max entries, table, name pattern}
      if (send[0] != 0) { recv[0] = static_cast<std::uint32_t>(kNoEntry); return true; }
      if (send[2] == 0) {
        g_listing.clear();
        g_listPos = 0;
        auto parts = Split(Name(send, sendSize));
        const std::string pattern = parts.empty() ? "*" : parts.back();
        if (!parts.empty()) parts.pop_back();
        const fs::path dir = HostPath(parts);
        if (!fs::is_directory(dir, ec)) { recv[0] = static_cast<std::uint32_t>(kNoEntry); return true; }
        if (!parts.empty()) {  // a save directory lists "." and ".." like the real card
          const auto t = fs::last_write_time(dir, ec);
          if (Match(pattern.c_str(), ".")) g_listing.push_back({".", true, 0, t});
          if (Match(pattern.c_str(), "..")) g_listing.push_back({"..", true, 0, t});
        }
        for (const auto& e : fs::directory_iterator(dir, ec)) {
          const std::string name = Unescape(FromPath(e.path().filename()));
          if (!Match(pattern.c_str(), name.c_str())) continue;
          const bool isDir = e.is_directory(ec);
          g_listing.push_back({name, isDir, isDir ? 0 : e.file_size(ec), e.last_write_time(ec)});
        }
      }
      const std::int32_t max = static_cast<std::int32_t>(send[3]);
      std::uint32_t count = 0;
      std::uint8_t* table = max > 0 ? guest(send[4], static_cast<std::uint32_t>(max) * 64) : nullptr;
      while (g_listPos < g_listing.size() && (max <= 0 || count < static_cast<std::uint32_t>(max))) {
        const Entry& e = g_listing[g_listPos++];
        if (table) {
          std::uint8_t* t = table + count * 64;
          std::memset(t, 0, 64);
          WriteTime(t, e.time);
          WriteTime(t + 8, e.time);
          const std::uint32_t size = static_cast<std::uint32_t>(e.size);
          const std::uint16_t attr = e.dir ? kAttrDir : kAttrFile;
          std::memcpy(t + 16, &size, 4);
          std::memcpy(t + 20, &attr, 2);
          std::memcpy(t + 32, e.name.c_str(), std::min<std::size_t>(e.name.size(), 31));
        }
        ++count;
      }
      recv[0] = count;
      return true;
    }
    case 0x0E: case 0x7C: recv[0] = 0; return true;  // SetFileInfo: names/attributes/times are kept by the host
    case 0x0F: case 0x79: {  // Delete: moved to <saves>\_deleted\<date time>\<card path> (recoverable)
      const auto parts = Split(Name(send, sendSize));
      const fs::path path = HostPath(parts);
      if (parts.empty() || !fs::exists(path, ec)) { recv[0] = static_cast<std::uint32_t>(kNoEntry); return true; }
      if (fs::is_directory(path, ec) && !fs::is_empty(path, ec)) { recv[0] = static_cast<std::uint32_t>(kNotEmpty); return true; }
      SYSTEMTIME st;
      GetLocalTime(&st);
      wchar_t stamp[32];
      swprintf_s(stamp, L"%04u-%02u-%02u %02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
      fs::path dest = g_root / "_deleted" / stamp;
      for (std::size_t i = 0; i + 1 < parts.size(); ++i) dest /= ToPath(Escape(parts[i]));
      fs::create_directories(dest, ec);
      fs::rename(path, dest / path.filename(), ec);
      recv[0] = ec ? static_cast<std::uint32_t>(kDenied) : 0u;
      return true;
    }
    case 0x12: recv[0] = 0x1E00; return true;  // GetEntSpace: free entries
    default: return false;
  }
}

}  // namespace mc
