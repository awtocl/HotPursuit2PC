// Function discovery in the game's executable (stage R1, ADR 0006).
//
// The executable has no symbols. A static recompiler must know, before the
// game runs, every address where execution can begin: function starts,
// branch targets, the cases of every switch, and code whose address is
// stored in data. This finds them from the file alone; `hp2recomp
// check-trace` then compares the result with what the reference core
// actually executed (docs/recomp-analysis.md).
//
// Method, in short:
//   1. decode every word of the code section;
//   2. function starts that are certain: the entry point and every target of
//      a direct call ("jal");
//   3. recognise every switch (a "jr" through a table of code addresses);
//   4. a function is a contiguous run of code, so it cannot be cut anywhere a
//      branch, a jump or a switch passes over. Wherever straight-line
//      execution ends (after "jr", "j", "b") and nothing passes over, a new
//      function begins;
//   5. follow the control flow of each function to mark what is reached and
//      where straight runs begin ("leaders").
#pragma once

#include "hp2/elf.h"
#include "hp2/r5900.h"

namespace hp2 {

// Why an address is believed to start a function.
enum class FuncOrigin : std::uint8_t {
  Entry,         // the program's entry point
  Call,          // target of a direct call ("jal")
  Jump,          // target of a direct jump ("j") that leaves another function: a tail call
  Pointer,       // follows the end of a function, and its address is stored in data or built in code
  Unreferenced,  // follows the end of a function; no reference to it was found
  Hint,          // given by the caller (DiscoverOptions::extra_entries)
};
std::string_view to_string(FuncOrigin origin);

struct Function {
  std::uint32_t start = 0;
  std::uint32_t end = 0;  // one past the last word of its range (trailing filler included)
  FuncOrigin origin = FuncOrigin::Call;
  std::uint32_t instructions = 0;  // reached by following the control flow
  std::uint32_t indirect_jumps = 0;
};

// A switch: "jr" through a table of code addresses.
struct JumpTable {
  std::uint32_t jump = 0;   // address of the "jr"
  std::uint32_t table = 0;  // address of the first entry
  std::vector<std::uint32_t> targets;
};

// Per-instruction facts, one byte per word of the code section.
enum InsnFlag : std::uint8_t {
  kReached = 0x01,      // reached by following control flow from a function start (or an orphan root)
  kLeader = 0x02,       // execution can begin a straight run here (target, fall-through after a branch, ...)
  kFuncStart = 0x04,
  kDelaySlot = 0x08,    // executes as the delay slot of the branch before it
  kAddrTaken = 0x10,    // its address appears in data, or is built by code (lui + addiu/ori)
  kTableTarget = 0x20,  // a case of a recognised switch
  kPadding = 0x40,      // "nop" filler between functions
  kOrphan = 0x80,       // code that nothing was found to lead to; kept as an entry point of its own
};

struct Discovery {
  std::uint32_t text_begin = 0, text_end = 0;
  std::vector<r5900::Insn> insns;   // every word of the code section, decoded
  std::vector<std::uint8_t> flags;  // InsnFlag bits, same indexing
  std::vector<Function> functions;  // sorted by start; together they cover the section
  std::vector<JumpTable> jump_tables;            // sorted by the address of the jump
  std::vector<std::uint32_t> unresolved_jumps;   // "jr" (not through ra) without a recognised table
  // Transfers from one function into another that are not calls or tail calls (source, target).
  std::vector<std::pair<std::uint32_t, std::uint32_t>> cross_branches;
  std::uint32_t passes = 0;  // rounds needed to classify the direct jumps

  bool in_text(std::uint32_t addr) const { return addr >= text_begin && addr < text_end && (addr & 3) == 0; }
  std::size_t index_of(std::uint32_t addr) const { return (addr - text_begin) / 4; }
  std::uint8_t flags_at(std::uint32_t addr) const { return in_text(addr) ? flags[index_of(addr)] : 0; }
  const Function* function_at(std::uint32_t addr) const;  // the function whose range contains addr

  std::size_t count(std::uint8_t flag) const;
  std::size_t invalid_words() const;  // words that do not decode as an instruction
};

struct DiscoverOptions {
  std::vector<std::uint32_t> extra_entries;  // additional known function starts
};

// Throws hp2::Error when the executable has no usable code section.
Discovery discover(const ElfImage& elf, const DiscoverOptions& options = {});

}  // namespace hp2
