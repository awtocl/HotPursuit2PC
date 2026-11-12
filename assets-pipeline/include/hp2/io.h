// File access with the guarantees the converter needs:
//  - sources are opened read-only (C2: the ROM is never written);
//  - outputs appear under their final name only when complete.
#pragma once

#include <array>

#include "hp2/common.h"

namespace hp2 {

// Random-access reader. Opens with read access only and share-read, so it
// cannot modify the file and fails if someone holds it for exclusive write.
class ReadOnlyFile {
 public:
  explicit ReadOnlyFile(const fs::path& path);
  ~ReadOnlyFile();
  ReadOnlyFile(ReadOnlyFile&& other) noexcept;
  ReadOnlyFile& operator=(ReadOnlyFile&&) = delete;
  ReadOnlyFile(const ReadOnlyFile&) = delete;
  ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;

  std::uint64_t size() const { return size_; }
  const fs::path& path() const { return path_; }

  // Reads exactly out.size() bytes at offset or throws (short read = error).
  void read_at(std::uint64_t offset, std::span<std::uint8_t> out) const;
  Bytes read_at(std::uint64_t offset, std::size_t count) const;

 private:
  fs::path path_;
  void* handle_ = nullptr;
  std::uint64_t size_ = 0;
};

// Reads a whole (small) file; throws if it is larger than max_size.
Bytes read_whole_file(const fs::path& path, std::uint64_t max_size);

// Writes to "<final>.part" and renames to <final> in commit(). Without
// commit() the partial file is removed. Refuses to replace an existing file.
class OutputFile {
 public:
  explicit OutputFile(const fs::path& final_path);
  ~OutputFile();
  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;

  void write(ByteView data);
  void commit();

 private:
  fs::path final_path_;
  fs::path part_path_;
  void* handle_ = nullptr;
  bool committed_ = false;
};

// Convenience: atomically write a small file (replacing an older version of
// the same generated file is allowed here - used for manifests/state files).
void write_file_replacing(const fs::path& path, ByteView data);

class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void update(ByteView data);
  std::array<std::uint8_t, 32> finish();

  static std::string hex(const std::array<std::uint8_t, 32>& digest);
  static std::string hex_of(ByteView data);

 private:
  void* algorithm_ = nullptr;
  void* hash_ = nullptr;
};

}  // namespace hp2
