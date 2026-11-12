// Reader for the game's executable (a 32-bit little-endian MIPS ELF).
// Only what the recompiler needs: where each part of the file is loaded in
// the console's memory, and word access by address.
#pragma once

#include <optional>

#include "hp2/common.h"

namespace hp2 {

struct ElfSection {
  std::string name;
  std::uint32_t type = 0, flags = 0, addr = 0, offset = 0, size = 0;
  bool alloc() const { return (flags & 0x2) != 0; }       // occupies memory when loaded
  bool executable() const { return (flags & 0x4) != 0; }  // SHF_EXECINSTR
  bool has_file_data() const { return type != 8; }        // not SHT_NOBITS (.bss)
};

struct ElfSegment {
  std::uint32_t type = 0, offset = 0, vaddr = 0, filesz = 0, memsz = 0, flags = 0;
  bool loadable() const { return type == 1; }
  bool executable() const { return (flags & 0x1) != 0; }
};

class ElfImage {
 public:
  // Throws hp2::Error with the reason when the file is not a usable MIPS ELF.
  static ElfImage parse(Bytes file);

  std::uint32_t entry() const { return entry_; }
  std::uint32_t gp() const { return gp_; }  // from .reginfo; 0 when absent
  const std::vector<ElfSection>& sections() const { return sections_; }
  const std::vector<ElfSegment>& segments() const { return segments_; }
  const ElfSection* section(std::string_view name) const;

  // Initialised memory: is [addr, addr + size) backed by file data of a loadable segment?
  bool contains(std::uint32_t addr, std::uint32_t size = 4) const;
  std::optional<std::uint32_t> try_word(std::uint32_t addr) const;
  std::uint32_t word(std::uint32_t addr) const;  // throws when not contained
  ByteView bytes(std::uint32_t addr, std::uint32_t size) const;

  // Lowest and one-past-highest address of the loaded image (including .bss).
  std::uint32_t image_begin() const { return image_begin_; }
  std::uint32_t image_end() const { return image_end_; }

  const Bytes& file() const { return file_; }

 private:
  Bytes file_;
  std::uint32_t entry_ = 0, gp_ = 0, image_begin_ = 0, image_end_ = 0;
  std::vector<ElfSection> sections_;
  std::vector<ElfSegment> segments_;
};

}  // namespace hp2
