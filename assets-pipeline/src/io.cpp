#include "hp2/io.h"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>

namespace hp2 {

namespace {

std::string last_error_text(DWORD code = GetLastError()) {
  wchar_t* buf = nullptr;
  const DWORD n = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::wstring w = n ? std::wstring(buf, n) : L"unknown error";
  if (buf) LocalFree(buf);
  while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.'))
    w.pop_back();
  return std::format("{} (Windows error {})", to_utf8(fs::path(w)), code);
}

// Absolute path with the \\?\ prefix so long paths work without a manifest.
std::wstring win_path(const fs::path& p) {
  std::error_code ec;
  fs::path abs = fs::absolute(p, ec);
  if (ec) abs = p;
  std::wstring s = abs.lexically_normal().wstring();
  if (s.rfind(L"\\\\?\\", 0) == 0) return s;
  if (s.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + s.substr(2);
  return L"\\\\?\\" + s;
}

}  // namespace

std::string to_utf8(const fs::path& p) {
  const std::u8string u8 = p.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

fs::path from_utf8(std::string_view s) {
  return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// ---------------------------------------------------------------- ReadOnlyFile

ReadOnlyFile::ReadOnlyFile(const fs::path& path) : path_(path) {
  HANDLE h = CreateFileW(win_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    fail("cannot open '{}' for reading: {}", to_utf8(path), last_error_text());
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz)) {
    const std::string why = last_error_text();
    CloseHandle(h);
    fail("cannot get the size of '{}': {}", to_utf8(path), why);
  }
  handle_ = h;
  size_ = static_cast<std::uint64_t>(sz.QuadPart);
}

ReadOnlyFile::~ReadOnlyFile() {
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

ReadOnlyFile::ReadOnlyFile(ReadOnlyFile&& other) noexcept
    : path_(std::move(other.path_)), handle_(other.handle_), size_(other.size_) {
  other.handle_ = nullptr;
  other.size_ = 0;
}

void ReadOnlyFile::read_at(std::uint64_t offset, std::span<std::uint8_t> out) const {
  if (offset > size_ || size_ - offset < out.size())
    fail("read of {} bytes at offset {} is outside '{}' ({} bytes)", out.size(), offset,
         to_utf8(path_), size_);
  std::size_t done = 0;
  while (done < out.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 26));
    OVERLAPPED ov{};
    const std::uint64_t pos = offset + done;
    ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), out.data() + done, chunk, &got, &ov))
      fail("read error in '{}' at offset {}: {}", to_utf8(path_), pos, last_error_text());
    if (got == 0) fail("unexpected end of '{}' at offset {}", to_utf8(path_), pos);
    done += got;
  }
}

Bytes ReadOnlyFile::read_at(std::uint64_t offset, std::size_t count) const {
  Bytes b(count);
  read_at(offset, std::span<std::uint8_t>(b));
  return b;
}

Bytes read_whole_file(const fs::path& path, std::uint64_t max_size) {
  ReadOnlyFile f(path);
  if (f.size() > max_size)
    fail("'{}' is {} bytes, larger than the {} bytes expected for this kind of file",
         to_utf8(path), f.size(), max_size);
  return f.read_at(0, static_cast<std::size_t>(f.size()));
}

// ------------------------------------------------------------------ OutputFile

OutputFile::OutputFile(const fs::path& final_path) : final_path_(final_path) {
  part_path_ = final_path;
  part_path_ += L".part";
  std::error_code ec;
  if (fs::exists(final_path, ec))
    fail("refusing to overwrite existing file '{}'", to_utf8(final_path));
  if (final_path.has_parent_path()) {
    fs::create_directories(final_path.parent_path(), ec);
    if (ec)
      fail("cannot create folder '{}': {}", to_utf8(final_path.parent_path()), ec.message());
  }
  HANDLE h = CreateFileW(win_path(part_path_).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    fail("cannot create '{}': {}", to_utf8(part_path_), last_error_text());
  handle_ = h;
}

OutputFile::~OutputFile() {
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
  if (!committed_) DeleteFileW(win_path(part_path_).c_str());  // our own partial file
}

void OutputFile::write(ByteView data) {
  std::size_t done = 0;
  while (done < data.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 26));
    DWORD put = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), data.data() + done, chunk, &put, nullptr) ||
        put == 0)
      fail("write error in '{}': {}", to_utf8(part_path_), last_error_text());
    done += put;
  }
}

void OutputFile::commit() {
  if (!CloseHandle(static_cast<HANDLE>(handle_))) {
    handle_ = nullptr;
    fail("cannot finish writing '{}': {}", to_utf8(part_path_), last_error_text());
  }
  handle_ = nullptr;
  if (!MoveFileExW(win_path(part_path_).c_str(), win_path(final_path_).c_str(), 0))
    fail("cannot move '{}' into place as '{}': {}", to_utf8(part_path_), to_utf8(final_path_),
         last_error_text());
  committed_ = true;
}

void write_file_replacing(const fs::path& path, ByteView data) {
  fs::path tmp = path;
  tmp += L".tmp";
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) fail("cannot create folder '{}': {}", to_utf8(path.parent_path()), ec.message());
  }
  HANDLE h = CreateFileW(win_path(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail("cannot create '{}': {}", to_utf8(tmp), last_error_text());
  std::size_t done = 0;
  while (done < data.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 26));
    DWORD put = 0;
    if (!WriteFile(h, data.data() + done, chunk, &put, nullptr) || put == 0) {
      const std::string why = last_error_text();
      CloseHandle(h);
      fail("write error in '{}': {}", to_utf8(tmp), why);
    }
    done += put;
  }
  FlushFileBuffers(h);
  CloseHandle(h);
  if (!MoveFileExW(win_path(tmp).c_str(), win_path(path).c_str(), MOVEFILE_REPLACE_EXISTING))
    fail("cannot move '{}' into place as '{}': {}", to_utf8(tmp), to_utf8(path), last_error_text());
}

// ---------------------------------------------------------------------- Sha256

Sha256::Sha256() {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    fail("cannot initialise SHA-256 (BCryptOpenAlgorithmProvider failed)");
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(alg, 0);
    fail("cannot initialise SHA-256 (BCryptCreateHash failed)");
  }
  algorithm_ = alg;
  hash_ = hash;
}

Sha256::~Sha256() {
  if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
  if (algorithm_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
}

void Sha256::update(ByteView data) {
  std::size_t done = 0;
  while (done < data.size()) {
    const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(data.size() - done, 1u << 30));
    if (BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_),
                       const_cast<PUCHAR>(data.data() + done), chunk, 0) != 0)
      fail("SHA-256 update failed");
    done += chunk;
  }
}

std::array<std::uint8_t, 32> Sha256::finish() {
  std::array<std::uint8_t, 32> out{};
  if (BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), out.data(),
                       static_cast<ULONG>(out.size()), 0) != 0)
    fail("SHA-256 finish failed");
  return out;
}

std::string Sha256::hex(const std::array<std::uint8_t, 32>& digest) {
  static const char* kDigits = "0123456789abcdef";
  std::string s;
  s.reserve(64);
  for (std::uint8_t b : digest) {
    s.push_back(kDigits[b >> 4]);
    s.push_back(kDigits[b & 15]);
  }
  return s;
}

std::string Sha256::hex_of(ByteView data) {
  Sha256 h;
  h.update(data);
  return hex(h.finish());
}

}  // namespace hp2
