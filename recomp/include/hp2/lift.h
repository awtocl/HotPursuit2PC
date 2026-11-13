// Translator spike (ADR 0007, Decision 5): game functions -> LLVM -> x64.
//
// Scope of the spike: "leaf" functions (no calls, no system calls, no
// switch), integer and FPU instructions. Each game function becomes
//
//     void fn(uint8_t* state, uint8_t* ram);
//
// which reads and writes the processor state in the layout the caller
// describes (the reference core's own state structure, so that both can run
// on the same data), accesses main memory directly, and returns with the
// state's program counter set to where the function returned to. If the
// function touches an address outside main memory the program counter is set
// to kTrapPc instead: the spike does not model hardware registers.
#pragma once

#include <memory>
#include <optional>

#include "hp2/discover.h"

namespace hp2 {

// Byte offsets of the pieces of processor state inside the state structure.
struct StateLayout {
  std::uint32_t pc = 0;
  std::uint32_t gpr = 0;       // 32 registers, gpr_stride bytes apart, low 64 bits first
  std::uint32_t gpr_stride = 16;
  std::uint32_t hi = 0, lo = 0;  // 64 bits each
  std::uint32_t fpr = 0;       // 32 single-precision registers as 32-bit patterns
  std::uint32_t fcsr = 0;      // FPU control/status; bit 23 is the condition flag
  std::uint32_t fpacc = 0;     // FPU accumulator (madd.s, adda.s, ...)
};

inline constexpr std::uint32_t kTrapPc = 1;
using LiftedFunction = void (*)(std::uint8_t* state, std::uint8_t* ram);

class Translator {
 public:
  Translator(const ElfImage& elf, const Discovery& discovery, const StateLayout& layout, std::uint32_t ram_size);
  ~Translator();
  Translator(const Translator&) = delete;
  Translator& operator=(const Translator&) = delete;

  // Translates the function that starts at `start` into LLVM's intermediate
  // form. Returns the reason when it cannot (an instruction or a kind of
  // control flow the spike does not cover); nothing is added then.
  std::optional<std::string> add_function(std::uint32_t start);

  // Optimises (the standard -O2 pipeline) and compiles everything added so
  // far to x64 code in memory. Call once, after the last add_function.
  void compile();

  // After compile(): the code for a function that was added, or nullptr.
  LiftedFunction lookup(std::uint32_t start) const;

  // The intermediate form of everything added, as text (before compile(): as
  // generated; after: as optimised). For inspection.
  std::string ir() const;
  // Number of functions added successfully.
  std::size_t size() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hp2
