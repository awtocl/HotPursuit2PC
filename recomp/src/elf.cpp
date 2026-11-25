#include "hp2/elf.h"

#include <algorithm>

namespace hp2 {

namespace {

std::uint16_t read_u16le(ByteView b, std::size_t off) {
  if (off > b.size() || b.size() - off < 2)
    fail("read of 2 bytes at offset {} is outside a {}-byte buffer", off, b.size());
  return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}

// A range [offset, offset + size) inside the file?
bool in_file(const Bytes& file, std::uint64_t offset, std::uint64_t size) {
  return offset <= file.size() && size <= file.size() - offset;
}

}  // namespace

ElfImage ElfImage::parse(Bytes file) {
  ElfImage e;
  e.file_ = std::move(file);
  const ByteView b(e.file_);
  if (b.size() < 52) fail("ELF: the file is only {} bytes, smaller than an ELF header", b.size());
  if (!(b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')) fail("ELF: the magic number is missing");
  if (b[4] != 1) fail("ELF: not a 32-bit file (class {})", b[4]);
  if (b[5] != 1) fail("ELF: not little-endian (data encoding {})", b[5]);
  const std::uint16_t machine = read_u16le(b, 18);
  if (machine != 8) fail("ELF: machine type {} is not MIPS (8)", machine);
  e.entry_ = read_u32le(b, 24);
  const std::uint32_t phoff = read_u32le(b, 28), shoff = read_u32le(b, 32);
  const std::uint16_t phentsize = read_u16le(b, 42), phnum = read_u16le(b, 44);
  const std::uint16_t shentsize = read_u16le(b, 46), shnum = read_u16le(b, 48);
  const std::uint16_t shstrndx = read_u16le(b, 50);

  if (phnum != 0) {
    if (phentsize < 32) fail("ELF: program header entries are {} bytes, expected at least 32", phentsize);
    if (!in_file(e.file_, phoff, std::uint64_t{phentsize} * phnum))
      fail("ELF: the program header table lies outside the file");
  }
  for (unsigned i = 0; i < phnum; ++i) {
    const std::size_t p = phoff + std::size_t{i} * phentsize;
    ElfSegment s;
    s.type = read_u32le(b, p);
    s.offset = read_u32le(b, p + 4);
    s.vaddr = read_u32le(b, p + 8);
    s.filesz = read_u32le(b, p + 16);
    s.memsz = read_u32le(b, p + 20);
    s.flags = read_u32le(b, p + 24);
    if (s.loadable()) {
      if (!in_file(e.file_, s.offset, s.filesz)) fail("ELF: segment {} lies outside the file", i);
      if (s.memsz < s.filesz) fail("ELF: segment {} is larger in the file than in memory", i);
      if (std::uint64_t{s.vaddr} + s.memsz > 0x100000000ull) fail("ELF: segment {} wraps the address space", i);
    }
    e.segments_.push_back(s);
  }

  if (shnum != 0) {
    if (shentsize < 40) fail("ELF: section header entries are {} bytes, expected at least 40", shentsize);
    if (!in_file(e.file_, shoff, std::uint64_t{shentsize} * shnum))
      fail("ELF: the section header table lies outside the file");
    if (shstrndx >= shnum) fail("ELF: the section name table index {} is out of range", shstrndx);
    const std::size_t strhdr = shoff + std::size_t{shstrndx} * shentsize;
    const std::uint32_t stroff = read_u32le(b, strhdr + 16), strsize = read_u32le(b, strhdr + 20);
    if (!in_file(e.file_, stroff, strsize)) fail("ELF: the section name table lies outside the file");
    for (unsigned i = 0; i < shnum; ++i) {
      const std::size_t p = shoff + std::size_t{i} * shentsize;
      ElfSection s;
      const std::uint32_t name = read_u32le(b, p);
      s.type = read_u32le(b, p + 4);
      s.flags = read_u32le(b, p + 8);
      s.addr = read_u32le(b, p + 12);
      s.offset = read_u32le(b, p + 16);
      s.size = read_u32le(b, p + 20);
      if (name >= strsize && strsize != 0) fail("ELF: section {} has its name outside the name table", i);
      for (std::uint32_t k = name; k < strsize && b[stroff + k] != 0; ++k)
        s.name.push_back(static_cast<char>(b[stroff + k]));
      if (s.has_file_data() && s.type != 0 && !in_file(e.file_, s.offset, s.size))
        fail("ELF: section {} ('{}') lies outside the file", i, s.name);
      e.sections_.push_back(std::move(s));
    }
  }

  bool any = false;
  for (const ElfSegment& s : e.segments_) {
    if (!s.loadable() || s.memsz == 0) continue;
    e.image_begin_ = any ? std::min(e.image_begin_, s.vaddr) : s.vaddr;
    e.image_end_ = any ? std::max(e.image_end_, s.vaddr + s.memsz) : s.vaddr + s.memsz;
    any = true;
  }
  if (!any) fail("ELF: there is no loadable segment");

  // .reginfo (MIPS): six 32-bit words, the last one is the gp value.
  if (const ElfSection* r = e.section(".reginfo"); r && r->size >= 24) e.gp_ = read_u32le(b, r->offset + 20);
  return e;
}

ElfImage ElfImage::code_blob(std::uint32_t load_address, ByteView code) {
  // A minimal ELF32 little-endian MIPS file: header, one executable PT_LOAD segment, the code.
  Bytes f(84 + code.size(), 0);
  auto put16 = [&](std::size_t o, std::uint16_t v) { f[o] = v & 0xFF; f[o + 1] = v >> 8; };
  auto put32 = [&](std::size_t o, std::uint32_t v) { for (int i = 0; i < 4; ++i) f[o + i] = (v >> (8 * i)) & 0xFF; };
  f[0] = 0x7F; f[1] = 'E'; f[2] = 'L'; f[3] = 'F'; f[4] = 1; f[5] = 1; f[6] = 1;
  put16(16, 2);                 // executable
  put16(18, 8);                 // MIPS
  put32(20, 1);
  put32(24, load_address);      // entry: the start of the blob
  put32(28, 52);                // program headers right after the header
  put16(40, 52);
  put16(42, 32);
  put16(44, 1);
  put32(52, 1);                 // PT_LOAD
  put32(56, 84);                // offset of the code
  put32(60, load_address);
  put32(64, load_address);
  put32(68, static_cast<std::uint32_t>(code.size()));
  put32(72, static_cast<std::uint32_t>(code.size()));
  put32(76, 5);                 // R + X
  put32(80, 4);
  std::copy(code.begin(), code.end(), f.begin() + 84);
  return parse(std::move(f));
}

const ElfSection* ElfImage::section(std::string_view name) const {
  for (const ElfSection& s : sections_)
    if (s.name == name) return &s;
  return nullptr;
}

bool ElfImage::contains(std::uint32_t addr, std::uint32_t size) const {
  for (const ElfSegment& s : segments_)
    if (s.loadable() && addr >= s.vaddr && std::uint64_t{addr} + size <= std::uint64_t{s.vaddr} + s.filesz)
      return true;
  return false;
}

ByteView ElfImage::bytes(std::uint32_t addr, std::uint32_t size) const {
  for (const ElfSegment& s : segments_)
    if (s.loadable() && addr >= s.vaddr && std::uint64_t{addr} + size <= std::uint64_t{s.vaddr} + s.filesz)
      return ByteView(file_).subspan(s.offset + (addr - s.vaddr), size);
  fail("ELF: address range {:08X}+{} is not initialised data of the program", addr, size);
}

std::optional<std::uint32_t> ElfImage::try_word(std::uint32_t addr) const {
  if (!contains(addr, 4)) return std::nullopt;
  return read_u32le(bytes(addr, 4), 0);
}

std::uint32_t ElfImage::word(std::uint32_t addr) const {
  return read_u32le(bytes(addr, 4), 0);
}

}  // namespace hp2
