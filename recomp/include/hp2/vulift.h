// VU1 microcode translator (ADR 0008): microprograms -> LLVM -> x64.
//
// The unit of translation is an image: microcode words (lower/upper pairs) as
// one upload loads them at a fixed address of microcode memory. Its code is
//
//     void image(uint8_t* state, uint8_t* vu_mem, const VuRuntime* rt);
//
// and behaves exactly like the reference core's VU executor running blocks
// from the state's program counter: it runs block after block until the
// state's exception word becomes non-zero (end bit, D/T bits, or the cycle
// quota used up: QUOTADONE), and returns. It also returns, with the exception
// word still zero, when execution reaches a place it has no block for (outside
// the image, a block that would need words beyond it, or code it refuses);
// the reference core then continues from that exact state. Entered at a
// program counter that is not one of its block starts, it returns at once.
//
// The reference core's pipeline model is reproduced as its block compiler
// builds it (docs/adr/0008-vu1-microcode.md): the same block partition, the
// static FMAC stall delays and pipeline time per pair, the dynamic stalls on
// writes left by the previous block, Q and clip-flag latencies, the integer
// branch rules, the moment of each xgkick. MAC, sticky and status flags are
// not maintained (the game's microcode never reads them; the translator
// refuses a block that would).
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hp2/lift.h"

namespace hp2 {

// State fields used only by microcode, beyond StateLayout (same structure).
struct VuLayout {
  std::uint32_t delayed_jump = 0;    // uint32, the reference core's nDelayedJumpAddr
  std::uint32_t fmac_write = 0;      // 3 x 128 bits: FMAC writes still in flight when a block ended
  std::uint32_t saved_next_index = 0, saved_next_value = 0;  // integer register written by a block's last pair
  std::uint32_t xgkick_address = 0;  // uint32
};

// Constants of the reference core's VU1 setup.
struct VuConfig {
  std::uint32_t micro_size = 0x4000;  // microcode memory, bytes
  std::uint32_t data_mask = 0x3FFF;   // data memory address mask
  std::uint32_t addr_top = 0x8400, addr_xgkick = 0x8410, addr_itop = 0x8420;  // I/O ports of the VU1 memory map
  std::uint32_t exception_dbit = 0, exception_tbit = 0, exception_ebit = 0, quota_done = 0x80;
  std::uint32_t invalid_pc = 1;
};

// The VU1 memory map's I/O ports (xtop/xitop read, xgkick writes).
struct VuRuntime {
  std::uint32_t (*read)(std::uint8_t* state, std::uint32_t address);
  void (*write)(std::uint8_t* state, std::uint32_t address, std::uint32_t value);
};

using VuImageCode = void (*)(std::uint8_t* state, std::uint8_t* vu_mem, const VuRuntime* rt);

struct VuTranslateStats {
  std::size_t blocks = 0;  // translated block starts
  std::size_t pairs = 0;   // instruction pairs emitted (a pair shared by overlapping blocks counts each time)
};

class VuTranslator {
 public:
  VuTranslator(const StateLayout& layout, const VuLayout& vu_layout, const VuConfig& config);
  ~VuTranslator();
  VuTranslator(const VuTranslator&) = delete;
  VuTranslator& operator=(const VuTranslator&) = delete;

  // Translates an image (`words`: lower, upper, lower, upper ... loaded at
  // byte address `address`). Blocks start at the image start, at branch
  // targets inside it, and after every block (transitively), plus at
  // `extra_entries`. Returns the image's index.
  std::size_t add_image(std::uint32_t address, std::vector<std::uint32_t> words,
                        const std::vector<std::uint32_t>& extra_entries = {});

  // Optimises and compiles every added image. Call once.
  void compile();

  std::size_t images() const;
  std::uint32_t image_address(std::size_t image) const;
  const std::vector<std::uint32_t>& image_words(std::size_t image) const;
  VuImageCode code(std::size_t image) const;  // after compile()
  bool is_block_start(std::size_t image, std::uint32_t address) const;

  // After compile(): code for entering an image at `address` when it is not
  // one of its block starts (the game starts microprograms at addresses it
  // computes at run time). It runs the block that starts there, then
  // continues in the image's code. Translated on first use (milliseconds) and
  // kept; nullptr when that block cannot be translated.
  VuImageCode entry(std::size_t image, std::uint32_t address);
  std::size_t entry_functions() const;  // entries translated so far
  double entry_seconds() const;         // time spent translating them

  const VuTranslateStats& stats() const;
  // Block starts that were not translated, by reason.
  const std::map<std::string, std::size_t>& refusals() const;
  const std::map<std::uint32_t, std::string>& refused_starts() const;  // block start -> reason
  std::string ir() const;  // before compile(): the generated code; after: the optimised code

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hp2
