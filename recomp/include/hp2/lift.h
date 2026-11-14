// Translator (ADR 0007): game functions -> LLVM -> x64.
//
// Each translated game function becomes
//
//     void fn(uint8_t* state, uint8_t* ram, uint8_t* spr, const Runtime* rt);
//
// It reads and writes the processor state in the layout the caller describes
// (the reference core's own structure, so that both run on the same data),
// main memory and the scratchpad directly, and everything else through the
// runtime's slow-path callbacks. It returns with the state's program counter
// set to where execution continues:
//  - after a normal return, the return address;
//  - wherever the translated code stops early and leaves the rest to the
//    reference core: an instruction the translator does not cover, a call to
//    code that was not translated, a system call, a loop that used up the
//    time slice (`cycle_quota`), a hardware access that raised an interrupt.
// Stopping early is always correct: the reference core continues from that
// exact instruction with the exact state.
//
// A translated function can also be entered in the middle, at the places
// lookup_entry() names: that is how execution comes back to translated code
// after the reference core ran an instruction the translator does not cover.
//
// Calls between translated functions are direct calls; after each call the
// caller checks that execution came back where a normal return would, and
// otherwise stops as well (this is how an early stop deep in a call chain
// reaches the reference core).
#pragma once

#include <map>
#include <set>
#include <memory>
#include <optional>

#include "hp2/discover.h"

namespace hp2 {

// Byte offsets of the pieces of processor state inside the state structure.
struct StateLayout {
  std::uint32_t pc = 0;
  std::uint32_t gpr = 0;  // 32 registers, gpr_stride bytes apart, 128 bits each (low 64 bits first)
  std::uint32_t gpr_stride = 16;
  std::uint32_t hi = 0, lo = 0;    // 64 bits each
  std::uint32_t hi1 = 0, lo1 = 0;  // second multiply unit, 64 bits each
  std::uint32_t fpr = 0;           // 32 single-precision registers as 32-bit patterns
  std::uint32_t fcsr = 0;          // FPU control/status; bit 23 is the condition flag
  std::uint32_t fpacc = 0;         // FPU accumulator (madd.s, adda.s, ...)
  std::uint32_t vf = 0;            // VU0 vector registers, 32 x 128 bits
  std::uint32_t cycle_quota = 0;   // int32: decremented by loops; at 0 or below the code stops
  std::uint32_t has_exception = 0; // uint32: non-zero after a hardware access -> stop at the next instruction
  // VU0 macro mode (the reference core's layout of these is assumed: see lift.cpp)
  std::uint32_t vacc = 0;          // accumulator, 128 bits
  std::uint32_t vi = 0;            // 16 integer registers, 32 bits each
  std::uint32_t vq = 0, vi_i = 0, vr = 0;  // Q, I, R registers (32 bits)
  std::uint32_t vcf = 0, vmf = 0, vdf = 0, vt = 0;  // clip flags, MAC flags, division flag, temporary
  std::uint32_t pipe_mac = 0, pipe_sticky = 0, pipe_clip = 0;  // flag pipelines: index, 8 values, 8 times
  std::uint32_t pipe_time = 0;     // uint32
  std::uint32_t pipe_q = 0;        // Q pipeline: counter, held value
};

// Slow path for addresses that are neither main memory nor the scratchpad
// (hardware registers, VU memory, ...). `state` is the state pointer the
// function was called with.
struct Runtime {
  std::uint64_t (*read)(std::uint8_t* state, std::uint32_t address, std::uint32_t size);  // size 1, 2, 4 or 8
  void (*write)(std::uint8_t* state, std::uint32_t address, std::uint64_t value, std::uint32_t size);
};

using LiftedFunction = void (*)(std::uint8_t* state, std::uint8_t* ram, std::uint8_t* spr, const Runtime* rt);

inline constexpr std::uint32_t kScratchpadSize = 0x4000;

struct TranslateStats {
  std::size_t instructions = 0;   // reached instructions in the translated functions
  std::size_t stops = 0;          // places where the code hands over to the reference core
  std::size_t calls = 0;          // direct calls
};

class Translator {
 public:
  // `threads`: modules compiled in parallel (0 = one per hardware thread).
  Translator(const ElfImage& elf, const Discovery& discovery, const StateLayout& layout, std::uint32_t ram_size,
             unsigned threads = 0);
  ~Translator();
  Translator(const Translator&) = delete;
  Translator& operator=(const Translator&) = delete;

  // Translates the function that starts at `start`. Returns the reason when
  // it cannot (only when its very first instruction would have to stop:
  // the function would make no progress).
  std::optional<std::string> add_function(std::uint32_t start);

  // Optimises (-O2, host CPU) and compiles everything added to x64 code in
  // memory. Call once, after the last add_function.
  void compile();

  // Instead of compile(): object code (COFF) for linking into a DLL, built
  // for the LLVM CPU `cpu` (for example "x86-64-v3"). Every translated
  // function is the external symbol symbol(start); "hp2_dispatch" too.
  std::vector<std::vector<char>> emit_objects(const std::string& cpu);
  static std::string symbol(std::uint32_t start);
  const std::set<std::uint32_t>& functions() const;                         // translated function starts
  const std::map<std::uint32_t, std::uint32_t>& entry_functions() const;    // entry place -> its function's start

  // After compile(): the code for a function that was added, or nullptr.
  LiftedFunction lookup(std::uint32_t start) const;
  // After compile(): the code to call with the state's program counter at
  // `address`, when the translated code can be entered there (a function
  // start, or a place inside a translated function where a straight run of
  // code begins and translated code makes progress), or nullptr.
  LiftedFunction lookup_entry(std::uint32_t address) const;
  std::size_t entries() const;  // entry places other than function starts

  std::string ir() const;  // intermediate form, for inspection
  std::size_t size() const;
  const TranslateStats& stats() const;
  // Why the translated code stops early, by instruction (mnemonic -> places).
  const std::map<std::string, std::size_t>& stop_reasons() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hp2
