// The translated game code as the converter stores it (ADR 0009): a DLL,
// build/hp2code.dll, exporting one table, `hp2_code_table`. The converter
// writes it (recomp/src/package.cpp); the launcher loads it
// (engine/src/translated.cpp). Plain C layout, shared by both; included by
// C++17 and C++20 code.
//
// The translated code reads and writes the reference core's processor state
// in place, so it is only usable with a launcher whose state layout is the
// one it was translated for: the table carries a fingerprint of that layout
// (Hp2CodeLayout), of the executable it came from (SHA-256) and the CPU level
// it was compiled for.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hp2 {

inline constexpr std::uint32_t kCodeTableMagic = 0x43325048;  // "HP2C"
// Raise whenever the table layout or the translators' output changes meaning.
inline constexpr std::uint32_t kCodeTableVersion = 2;  // 2: Hp2CodeLayout gained `sa`
inline constexpr const char* kCodeTableSymbol = "hp2_code_table";
inline constexpr const char* kCodeDllName = "hp2code.dll";

// Where the translated code expects things: state offsets in the reference
// core's MIPSSTATE and the constants of its VU1 setup (all 32-bit values,
// no padding: the fingerprint is computed over the bytes).
struct Hp2CodeLayout {
  std::uint32_t pc, gpr, gpr_stride, hi, lo, hi1, lo1, fpr, fcsr, fpacc, vf, cycle_quota, has_exception;
  std::uint32_t vacc, vi, vq, vi_i, vr, vcf, vmf, vdf, vt, pipe_mac, pipe_sticky, pipe_clip, pipe_time, pipe_q;
  std::uint32_t delayed_jump, fmac_write, saved_next_index, saved_next_value, xgkick_address;
  std::uint32_t sa;  // shift amount register (mtsa/mtsab/mtsah/mfsa/qfsrv); 0 = not provided: those instructions stop the translated code
  std::uint32_t micro_size, data_mask, addr_top, addr_xgkick, addr_itop;
  std::uint32_t exception_dbit, exception_tbit, exception_ebit, quota_done, invalid_pc;
  std::uint32_t ram_size;
};
static_assert(sizeof(Hp2CodeLayout) == 44 * 4, "Hp2CodeLayout must have no padding");

inline std::uint64_t code_layout_fingerprint(const Hp2CodeLayout& layout) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&layout);
  std::uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (std::size_t i = 0; i < sizeof layout; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
  return h ^ kCodeTableVersion;
}

struct Hp2CodeEntry {
  std::uint32_t address;
  std::uint32_t reserved;
  void* code;
};

struct Hp2VuImageTable {
  std::uint32_t address, word_count;
  const std::uint32_t* words;  // the microcode the image was translated from (checked before use)
  void* code;                  // the image function: block starts only
  std::uint32_t block_count, entry_count;
  const std::uint32_t* block_starts;  // sorted
  const Hp2CodeEntry* entries;        // sorted: every other pair of the image that can be entered
};

struct Hp2CodeTable {
  std::uint32_t magic, version;
  std::uint8_t elf_sha256[32];
  std::uint64_t layout_fingerprint;
  char cpu[16];                       // LLVM CPU level, e.g. "x86-64-v3"
  std::uint32_t function_count, entry_count;
  const Hp2CodeEntry* functions;      // sorted by address: function starts
  const Hp2CodeEntry* entries;        // sorted: places inside a function where it can be entered -> that function
  std::uint32_t vu_image_count, reserved;
  const Hp2VuImageTable* vu_images;
  void* dispatch;                     // hp2_dispatch: calls through a pointer
};

}  // namespace hp2
