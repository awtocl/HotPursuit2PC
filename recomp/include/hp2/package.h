// Building the translated game code once, as a DLL (ADR 0009): every function
// the discovery finds and the VU1 microcode, translated, compiled in parallel
// for a fixed CPU level and linked with LLD. See hp2/code_package.h for what
// the DLL contains.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hp2/code_package.h"
#include "hp2/io.h"

namespace hp2 {

// Code the program copies elsewhere at run time (found by reading the program: the game
// installs a kernel-mode helper layer of its own at 0x80076000): `size` bytes at `source`
// in the executable's image are translated as if they were located at `load`.
struct CodeOverlay {
  std::uint32_t source = 0, size = 0, load = 0;
};

struct CodePackageOptions {
  std::vector<CodeOverlay> overlays;
  std::string cpu = "x86-64-v3";  // LLVM CPU level of the generated code (AVX2 and earlier extensions)
  unsigned threads = 0;           // 0: one per hardware thread
  bool native = false;            // for the native runtime: waiting loops yield (StateLayout::yield_in_idle_loops)
  std::vector<std::uint32_t> exclude;  // functions left untranslated: the runtime provides them (host services)
  std::function<void(std::string_view)> log;
};

struct CodePackageResult {
  std::size_t functions_found = 0, functions_translated = 0, ee_entries = 0;
  std::size_t vu_images = 0, vu_block_starts = 0, vu_entries = 0;
  double translate_seconds = 0, compile_seconds = 0, link_seconds = 0;
  std::uint64_t dll_bytes = 0;
  std::map<std::string, std::size_t> refusals;  // why functions were not translated
  std::map<std::string, std::size_t> stops;     // why translated code hands over, by instruction
  std::string summary;                          // a few lines for logs and the sidecar file
};

// Translates the executable `elf_bytes` and writes `dll` (replacing it), plus
// `dll` with extension .txt (what it is, for the converter's status). Throws
// on failure; nothing is left half-written.
CodePackageResult build_code_package(ByteView elf_bytes, const Hp2CodeLayout& layout, const fs::path& dll,
                                     const CodePackageOptions& options = {});

// The identity line stored in the sidecar file and checked by the converter
// before keeping an existing package: format version, executable hash, layout
// fingerprint, CPU level.
std::string code_package_identity(const std::array<std::uint8_t, 32>& elf_sha256, const Hp2CodeLayout& layout,
                                  std::string_view cpu);

}  // namespace hp2
