// hp2recomp - developer tool for stage R1 (static recompilation, ADR 0006).
//
//   hp2recomp info <elf>                       sections, code size, instruction census
//   hp2recomp functions <elf> [--out <tsv>]    function discovery summary (and the full list)
//   hp2recomp disasm <elf> <address> [count]   disassembly
//   hp2recomp check-trace <elf> <trace dir>    compare discovery with what the reference core
//                                              executed (the launcher's --trace output)
//
// <elf> is the game's executable (SLUS_203.62 in the ROM folder). It is
// opened read-only; nothing is ever written next to it.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#include "hp2/discover.h"
#include "hp2/io.h"
#ifdef HP2_HAVE_LLVM
#include "hp2/lift.h"
#endif

using namespace hp2;
using r5900::Flow;
using r5900::Insn;
using r5900::Op;

namespace {

constexpr std::uint64_t kMaxElfSize = 64ull << 20;

ElfImage load_elf(const fs::path& path) {
  return ElfImage::parse(read_whole_file(path, kMaxElfSize));
}

std::uint32_t parse_address(std::string_view text) {
  std::size_t used = 0;
  const unsigned long value = std::stoul(std::string(text), &used, 16);
  if (used != text.size()) fail("'{}' is not a hexadecimal address", text);
  return static_cast<std::uint32_t>(value);
}

double percent(std::size_t part, std::size_t whole) {
  return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

// ---- info ----------------------------------------------------------------------------------

int cmd_info(const fs::path& elf_path) {
  const ElfImage elf = load_elf(elf_path);
  std::cout << std::format("entry {:08X}   gp {:08X}   image {:08X}..{:08X}\n", elf.entry(), elf.gp(),
                           elf.image_begin(), elf.image_end());
  for (const ElfSegment& s : elf.segments())
    if (s.loadable())
      std::cout << std::format("  segment  {:08X}..{:08X}  file {:7} bytes  memory {:7} bytes  {}\n", s.vaddr,
                               s.vaddr + s.memsz, s.filesz, s.memsz, s.executable() ? "code" : "data");
  for (const ElfSection& s : elf.sections())
    if (s.alloc() && s.size)
      std::cout << std::format("  section  {:<20} {:08X}..{:08X}  {:7} bytes{}{}\n", s.name, s.addr,
                               s.addr + s.size, s.size, s.executable() ? "  exec" : "",
                               s.has_file_data() ? "" : "  (no file data)");

  const Discovery d = discover(elf);
  std::array<std::size_t, r5900::kOpCount> census{};
  for (const Insn& in : d.insns) ++census[static_cast<std::size_t>(in.op)];
  std::size_t distinct = 0;
  for (std::size_t i = 1; i < census.size(); ++i) distinct += census[i] != 0;
  std::cout << std::format("\ncode section {:08X}..{:08X}: {} words, {} do not decode, {} distinct instructions used\n",
                           d.text_begin, d.text_end, d.insns.size(), census[0], distinct);
  std::vector<std::pair<std::size_t, std::size_t>> order;
  for (std::size_t i = 1; i < census.size(); ++i)
    if (census[i]) order.emplace_back(census[i], i);
  std::sort(order.rbegin(), order.rend());
  std::cout << "instruction census (static):\n";
  std::size_t column = 0;
  for (const auto& [n, i] : order) {
    std::cout << std::format("  {:<9}{:>7}", r5900::mnemonic(static_cast<Op>(i)), n);
    if (++column % 6 == 0) std::cout << '\n';
  }
  if (column % 6) std::cout << '\n';
  return 0;
}

// ---- functions -----------------------------------------------------------------------------

void print_discovery_summary(const Discovery& d) {
  std::map<FuncOrigin, std::size_t> by_origin;
  std::size_t empty = 0;
  for (const Function& f : d.functions) {
    ++by_origin[f.origin];
    empty += f.instructions == 0;
  }
  const std::size_t words = d.insns.size();
  const std::size_t reached = d.count(kReached), padding = d.count(kPadding), invalid = d.invalid_words();
  std::size_t unreached_code = 0, reached_invalid = 0, from_orphans = 0;
  {
    // Words reached only because an orphan root was adopted: follow the flags in address order.
    bool in_orphan = false;
    for (std::size_t i = 0; i < words; ++i) {
      const std::uint8_t f = d.flags[i];
      const bool valid = d.insns[i].valid();
      if (!(f & (kReached | kPadding)) && valid) ++unreached_code;
      if ((f & kReached) && !valid) ++reached_invalid;
      if (f & kOrphan) in_orphan = true;
      else if (f & (kFuncStart | kPadding)) in_orphan = false;
      if (in_orphan && (f & kReached)) ++from_orphans;
    }
  }
  std::size_t table_targets = 0;
  for (const JumpTable& t : d.jump_tables) table_targets += t.targets.size();

  std::cout << std::format("code section {:08X}..{:08X}: {} words\n", d.text_begin, d.text_end, words);
  std::cout << std::format("functions: {} (direct jumps classified in {} rounds)\n", d.functions.size(), d.passes);
  for (const auto& [origin, n] : by_origin) std::cout << std::format("  {:<13} {:>6}\n", to_string(origin), n);
  std::cout << std::format("  (of these, {} contain no reached instruction)\n", empty);
  std::cout << std::format("reached by control flow: {} words ({:.2f}%)\n", reached, percent(reached, words));
  std::cout << std::format("filler (nop) between functions: {} words\n", padding);
  std::cout << std::format("words that do not decode: {} ({} of them reached by control flow)\n", invalid,
                           reached_invalid);
  std::cout << std::format("valid instructions never reached: {}\n", unreached_code);
  std::cout << std::format("orphan roots (code nothing leads to, kept as entry points): {}  (about {} words follow them)\n",
                           d.count(kOrphan), from_orphans);
  std::cout << std::format("leaders (places where a straight run can begin): {}\n", d.count(kLeader));
  std::cout << std::format("address-taken instructions: {}\n", d.count(kAddrTaken));
  std::cout << std::format("switches: {} recognised ({} cases), {} indirect jumps not recognised\n",
                           d.jump_tables.size(), table_targets, d.unresolved_jumps.size());
  std::cout << std::format("branches from one function into the middle of another: {}\n", d.cross_branches.size());
}

int cmd_functions(const fs::path& elf_path, const fs::path& out) {
  const ElfImage elf = load_elf(elf_path);
  const Discovery d = discover(elf);
  print_discovery_summary(d);
  if (!d.unresolved_jumps.empty()) {
    std::cout << "indirect jumps not recognised as a switch (first 20):\n";
    for (std::size_t i = 0; i < d.unresolved_jumps.size() && i < 20; ++i)
      std::cout << std::format("  {:08X}  {}\n", d.unresolved_jumps[i],
                               r5900::disassemble(d.unresolved_jumps[i], d.insns[d.index_of(d.unresolved_jumps[i])].raw));
  }
  if (!d.cross_branches.empty()) {
    std::cout << "cross-function branches (first 20):\n";
    for (std::size_t i = 0; i < d.cross_branches.size() && i < 20; ++i)
      std::cout << std::format("  {:08X} -> {:08X}\n", d.cross_branches[i].first, d.cross_branches[i].second);
  }
  {
    std::size_t shown = 0, taken = 0, prologue = 0, total = 0;
    for (std::size_t i = 0; i < d.insns.size(); ++i) {
      if (!(d.flags[i] & kOrphan)) continue;
      ++total;
      taken += (d.flags[i] & kAddrTaken) != 0;
      // "addiu sp, sp, -N": the usual first instruction of a function
      const Insn& in = d.insns[i];
      prologue += in.op == Op::ADDIU && in.rs() == 29 && in.rt() == 29 && in.simm() < 0;
    }
    std::cout << std::format("orphan roots: {} ({} address-taken, {} begin with a stack frame set-up); first 12:\n", total,
                             taken, prologue);
    for (std::size_t i = 0; i < d.insns.size() && shown < 12; ++i)
      if (d.flags[i] & kOrphan) {
        const std::uint32_t pc = d.text_begin + static_cast<std::uint32_t>(i * 4);
        std::cout << std::format("  {:08X}  {}\n", pc, r5900::disassemble(pc, d.insns[i].raw));
        ++shown;
      }
  }
  if (!out.empty()) {
    std::string text = "start\tend\torigin\tinstructions\tindirect_jumps\n";
    for (const Function& f : d.functions)
      text += std::format("{:08X}\t{:08X}\t{}\t{}\t{}\n", f.start, f.end, to_string(f.origin), f.instructions,
                          f.indirect_jumps);
    write_file_replacing(out, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    std::cout << std::format("wrote {} functions to {}\n", d.functions.size(), to_utf8(out));
  }
  return 0;
}

// ---- disasm --------------------------------------------------------------------------------

int cmd_disasm(const fs::path& elf_path, std::uint32_t address, std::uint32_t count) {
  const ElfImage elf = load_elf(elf_path);
  const Discovery d = discover(elf);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t pc = address + i * 4;
    const auto word = elf.try_word(pc);
    if (!word) {
      std::cout << std::format("{:08X}  (not part of the program's file data)\n", pc);
      break;
    }
    const std::uint8_t f = d.flags_at(pc);
    std::string marks;
    if (f & kFuncStart) marks += 'F';
    if (f & kLeader) marks += 'L';
    if (f & kTableTarget) marks += 'T';
    if (f & kAddrTaken) marks += 'A';
    if (f & kDelaySlot) marks += 'd';
    if (f & kOrphan) marks += 'O';
    if (!(f & kReached)) marks += f & kPadding ? 'p' : '-';
    std::cout << std::format("{:08X}  {:08X}  {:<5} {}\n", pc, *word, marks, r5900::disassemble(pc, *word));
  }
  return 0;
}

// ---- check-trace ---------------------------------------------------------------------------

struct Block {
  std::uint32_t begin = 0, end = 0;  // end = address of the last instruction
};

std::vector<Block> read_blocks(const fs::path& path) {
  std::ifstream in(path);
  if (!in) fail("cannot open '{}'", to_utf8(path));
  std::set<std::pair<std::uint32_t, std::uint32_t>> unique;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string b, e;
    if (!(fields >> b >> e)) fail("malformed line in '{}': '{}'", to_utf8(path), line);
    unique.emplace(parse_address(b), parse_address(e));
  }
  std::vector<Block> blocks;
  for (const auto& [b, e] : unique) blocks.push_back({b, e});
  return blocks;
}

int cmd_check_trace(const fs::path& elf_path, const fs::path& trace_dir) {
  const ElfImage elf = load_elf(elf_path);
  const Discovery d = discover(elf);
  print_discovery_summary(d);
  const std::vector<Block> blocks = read_blocks(trace_dir / "blocks.tsv");

  std::cout << std::format("\n== what the reference core executed ({}) ==\n", to_utf8(trace_dir / "blocks.tsv"));
  std::vector<Block> inside, outside;
  for (const Block& b : blocks) (d.in_text(b.begin) ? inside : outside).push_back(b);
  std::cout << std::format("distinct blocks: {}   inside the code section: {}   outside: {}\n", blocks.size(),
                           inside.size(), outside.size());

  // Blocks outside the game's code: the core's own kernel stand-in, or code the game placed elsewhere.
  std::map<std::string, std::size_t> outside_regions;
  for (const Block& b : outside) {
    std::string region;
    if (b.begin < 0x00100000) region = "below 0x00100000 (kernel area)";
    else if (b.begin >= 0x1FC00000) region = "0x1FC00000.. (the core's BIOS stand-in)";
    else if (b.begin >= elf.image_begin() && b.begin < elf.image_end()) region = "inside the program image, outside .text";
    else region = "elsewhere in main memory";
    ++outside_regions[region];
  }
  for (const auto& [region, n] : outside_regions) std::cout << std::format("  outside: {:>6} blocks  {}\n", n, region);
  for (const Block& b : outside)
    if (b.begin >= 0x00100000 && b.begin < 0x1FC00000)
      std::cout << std::format("    {:08X}..{:08X}\n", b.begin, b.end);

  // Executed instructions, and the checks.
  std::vector<std::uint8_t> executed(d.insns.size(), 0);
  std::size_t not_leader = 0, not_reached = 0, undecodable = 0, no_function = 0, at_orphan = 0;
  std::vector<std::uint32_t> missed_leaders, missed_reached, orphans_executed, unreferenced_executed;
  std::set<std::uint32_t> executed_functions;
  std::map<FuncOrigin, std::size_t> entered_by_origin;
  for (const Block& b : inside) {
    const std::uint8_t f = d.flags_at(b.begin);
    if (f & kOrphan) {
      ++at_orphan;
      orphans_executed.push_back(b.begin);
    }
    if (f & kFuncStart)
      if (const Function* fn = d.function_at(b.begin)) {
        ++entered_by_origin[fn->origin];
        if (fn->origin == FuncOrigin::Unreferenced) unreferenced_executed.push_back(b.begin);
      }
    if (!(f & kLeader)) {
      ++not_leader;
      missed_leaders.push_back(b.begin);
    }
    if (!(f & kReached)) {
      ++not_reached;
      missed_reached.push_back(b.begin);
    }
    if (const Function* fn = d.function_at(b.begin)) executed_functions.insert(fn->start);
    else ++no_function;
    for (std::uint32_t pc = b.begin; pc <= b.end && d.in_text(pc); pc += 4) {
      const std::size_t i = d.index_of(pc);
      if (!executed[i] && !d.insns[i].valid()) ++undecodable;
      executed[i] = 1;
    }
  }
  const std::size_t executed_words = static_cast<std::size_t>(std::count(executed.begin(), executed.end(), 1));
  std::size_t executed_unreached = 0;
  for (std::size_t i = 0; i < executed.size(); ++i)
    if (executed[i] && !(d.flags[i] & kReached)) ++executed_unreached;

  std::cout << std::format("executed instructions: {} of {} words in the code section ({:.1f}%)\n", executed_words,
                           d.insns.size(), percent(executed_words, d.insns.size()));
  std::cout << std::format("executed functions: {} of {}\n", executed_functions.size(), d.functions.size());
  std::cout << std::format("CHECK 1  every executed instruction decodes:                {} failures\n", undecodable);
  std::cout << std::format("CHECK 2  every executed block starts at a predicted leader:  {} of {} do not\n",
                           not_leader, inside.size());
  std::cout << std::format("CHECK 3  every executed block start was reached statically:  {} of {} were not\n",
                           not_reached, inside.size());
  std::cout << std::format("CHECK 4  every executed instruction was reached statically:  {} of {} were not\n",
                           executed_unreached, executed_words);
  std::cout << std::format("         executed blocks outside any discovered function:    {}\n", no_function);
  std::cout << std::format("CHECK 6  no executed block starts at an orphan root:          {} do\n", at_orphan);
  for (std::size_t i = 0; i < orphans_executed.size() && i < 30; ++i)
    std::cout << std::format("  executed orphan: {:08X}  {}\n", orphans_executed[i],
                             r5900::disassemble(orphans_executed[i], d.insns[d.index_of(orphans_executed[i])].raw));
  std::cout << "functions entered at their first instruction, by how the function was found:\n";
  for (const auto& [origin, n] : entered_by_origin) std::cout << std::format("  {:<13} {:>6}\n", to_string(origin), n);
  for (std::size_t i = 0; i < unreferenced_executed.size() && i < 30; ++i)
    std::cout << std::format("  executed, but no reference found: {:08X}  {}\n", unreferenced_executed[i],
                             r5900::disassemble(unreferenced_executed[i],
                                                d.insns[d.index_of(unreferenced_executed[i])].raw));
  for (std::size_t i = 0; i < missed_leaders.size() && i < 30; ++i) {
    const std::uint32_t pc = missed_leaders[i];
    std::cout << std::format("  not a leader: {:08X}  {}   (previous: {})\n", pc,
                             r5900::disassemble(pc, d.insns[d.index_of(pc)].raw),
                             pc >= d.text_begin + 8 ? r5900::disassemble(pc - 8, d.insns[d.index_of(pc - 8)].raw) : "");
  }
  for (std::size_t i = 0; i < missed_reached.size() && i < 30; ++i)
    std::cout << std::format("  not reached: {:08X}  {}\n", missed_reached[i],
                             r5900::disassemble(missed_reached[i], d.insns[d.index_of(missed_reached[i])].raw));

  // How the executed blocks end: this is what the translator has to support.
  std::map<std::string, std::size_t> endings;
  std::size_t jr_resolved = 0, jr_unresolved = 0;
  std::set<std::uint32_t> table_jumps;
  for (const JumpTable& t : d.jump_tables) table_jumps.insert(t.jump);
  for (const Block& b : inside) {
    // The core ends a block with the delay slot of a branch: the branch is the instruction before the last.
    const std::uint32_t last = b.end, before = b.end - 4;
    const Insn* in = d.in_text(before) && before >= b.begin ? &d.insns[d.index_of(before)] : nullptr;
    if (!in || !in->has_delay_slot()) in = d.in_text(last) ? &d.insns[d.index_of(last)] : nullptr;
    std::string kind = "no branch (block size limit or other)";
    if (in) switch (in->flow) {
        case Flow::Branch: kind = in->likely ? "conditional branch (likely)" : "conditional branch"; break;
        case Flow::BranchLink: kind = "branch and link"; break;
        case Flow::Jump: kind = "j"; break;
        case Flow::Call: kind = "jal (direct call)"; break;
        case Flow::CallReg: kind = "jalr (call through a pointer)"; break;
        case Flow::JumpReg:
          if (in->rs() == 31) kind = "jr ra (return)";
          else {
            kind = "jr (switch or jump through a pointer)";
            (table_jumps.count(d.in_text(before) && d.insns[d.index_of(before)].flow == Flow::JumpReg ? before : last)
                 ? jr_resolved : jr_unresolved)++;
          }
          break;
        case Flow::Syscall: kind = "syscall"; break;
        case Flow::Break: kind = "break"; break;
        case Flow::Eret: kind = "eret"; break;
        case Flow::None: break;
      }
    ++endings[kind];
  }
  std::cout << "how the executed blocks end:\n";
  for (const auto& [kind, n] : endings) std::cout << std::format("  {:>7}  {}\n", n, kind);
  std::cout << std::format("  executed indirect jumps: {} recognised as a switch, {} not\n", jr_resolved, jr_unresolved);

  // Instruction census of the executed code.
  std::array<std::size_t, r5900::kOpCount> census{};
  for (std::size_t i = 0; i < executed.size(); ++i)
    if (executed[i]) ++census[static_cast<std::size_t>(d.insns[i].op)];
  std::vector<std::pair<std::size_t, std::size_t>> order;
  for (std::size_t i = 1; i < census.size(); ++i)
    if (census[i]) order.emplace_back(census[i], i);
  std::sort(order.rbegin(), order.rend());
  std::cout << std::format("distinct instructions executed: {}\n", order.size());
  std::size_t column = 0;
  for (const auto& [n, i] : order) {
    std::cout << std::format("  {:<9}{:>7}", r5900::mnemonic(static_cast<Op>(i)), n);
    if (++column % 6 == 0) std::cout << '\n';
  }
  if (column % 6) std::cout << '\n';

  // Main memory at the end of the session: was the code modified, and which code addresses does it hold?
  const fs::path ram_path = trace_dir / "ee-ram.bin";
  std::error_code ec;
  if (fs::is_regular_file(ram_path, ec)) {
    const ReadOnlyFile ram(ram_path);
    std::cout << std::format("\n== main memory at the end of the session ({} bytes) ==\n", ram.size());
    if (ram.size() >= d.text_end) {
      const Bytes text = ram.read_at(d.text_begin, d.text_end - d.text_begin);
      std::size_t changed = 0;
      std::uint32_t first_changed = 0;
      for (std::size_t i = 0; i < d.insns.size(); ++i)
        if (read_u32le(text, i * 4) != d.insns[i].raw) {
          if (!changed) first_changed = d.text_begin + static_cast<std::uint32_t>(i * 4);
          ++changed;
        }
      std::cout << std::format("CHECK 5  code section in memory equals the file:  {} words differ", changed);
      if (changed) std::cout << std::format(" (first at {:08X})", first_changed);
      std::cout << '\n';

      // Words in memory outside the code section that hold a code address.
      const std::uint32_t scan_end = static_cast<std::uint32_t>(std::min<std::uint64_t>(ram.size(), 0x02000000));
      std::set<std::uint32_t> pointers;
      const std::size_t chunk = 1 << 20;
      for (std::uint32_t base = 0; base < scan_end; base += static_cast<std::uint32_t>(chunk)) {
        const Bytes part = ram.read_at(base, std::min<std::size_t>(chunk, scan_end - base));
        for (std::size_t off = 0; off + 4 <= part.size(); off += 4) {
          const std::uint32_t addr = base + static_cast<std::uint32_t>(off);
          if (addr >= d.text_begin && addr < d.text_end) continue;
          const std::uint32_t value = read_u32le(part, off);
          if (d.in_text(value)) pointers.insert(value);
        }
      }
      std::size_t starts = 0, tables = 0, returns = 0, other_leaders = 0, unknown = 0;
      std::vector<std::uint32_t> unknown_list;
      for (std::uint32_t p : pointers) {
        const std::uint8_t f = d.flags_at(p);
        const bool after_call = p >= d.text_begin + 8 &&
                                (d.insns[d.index_of(p - 8)].flow == Flow::Call ||
                                 d.insns[d.index_of(p - 8)].flow == Flow::CallReg);
        if (f & kFuncStart) ++starts;
        else if (f & kTableTarget) ++tables;
        else if (after_call) ++returns;
        else if (f & kLeader) ++other_leaders;
        else {
          ++unknown;
          unknown_list.push_back(p);
        }
      }
      std::cout << std::format("distinct code addresses found in memory outside the code section: {}\n", pointers.size());
      std::cout << std::format("  {:>6} function starts\n  {:>6} switch cases\n  {:>6} return addresses (after a call)\n"
                               "  {:>6} other leaders\n  {:>6} not a predicted entry point (may be coincidental data)\n",
                               starts, tables, returns, other_leaders, unknown);
      std::size_t unknown_executed = 0;
      std::set<std::uint32_t> block_starts;
      for (const Block& b : inside) block_starts.insert(b.begin);
      for (std::uint32_t p : unknown_list) unknown_executed += block_starts.count(p);
      std::cout << std::format("  of the last group, {} are addresses where an executed block begins\n", unknown_executed);
    }
  }
  return (undecodable || not_leader || not_reached || at_orphan) ? 1 : 0;
}

// ---- hot -----------------------------------------------------------------------------------

// Which functions execute the most instructions (needs a trace recorded with --trace-counts).
int cmd_hot(const fs::path& elf_path, const fs::path& trace_dir, std::size_t top) {
  const ElfImage elf = load_elf(elf_path);
  const Discovery d = discover(elf);
  std::ifstream in(trace_dir / "blocks.tsv");
  if (!in) fail("cannot open '{}'", to_utf8(trace_dir / "blocks.tsv"));

  struct Heat {
    std::uint64_t instructions = 0, entries = 0;
    std::size_t blocks = 0;
  };
  std::map<std::uint32_t, Heat> heat;  // by function start
  std::array<std::uint64_t, r5900::kOpCount> dynamic{};
  std::uint64_t total = 0, outside = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string b, e;
    unsigned frame = 0;
    std::uint64_t count = 0;
    if (!(fields >> b >> e >> frame >> count))
      fail("'{}' has no execution counts: record the trace with --trace-counts", to_utf8(trace_dir / "blocks.tsv"));
    const std::uint32_t begin = parse_address(b), end = parse_address(e);
    const std::uint64_t executed = count * ((end - begin) / 4 + 1);
    total += executed;
    const Function* f = d.function_at(begin);
    if (!f) {
      outside += executed;
      continue;
    }
    Heat& h = heat[f->start];
    h.instructions += executed;
    ++h.blocks;
    if (begin == f->start) h.entries += count;
    for (std::uint32_t pc = begin; pc <= end && d.in_text(pc); pc += 4)
      dynamic[static_cast<std::size_t>(d.insns[d.index_of(pc)].op)] += count;
  }
  std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
  for (const auto& [start, h] : heat) order.emplace_back(h.instructions, start);
  std::sort(order.rbegin(), order.rend());

  std::cout << std::format("instructions executed in the session: {} ({:.1f}% outside the game's code section)\n", total,
                           100.0 * static_cast<double>(outside) / static_cast<double>(total ? total : 1));
  std::cout << std::format("functions executed: {}\n", order.size());
  std::uint64_t running = 0;
  std::size_t for50 = 0, for90 = 0, for99 = 0;
  for (std::size_t i = 0; i < order.size(); ++i) {
    running += order[i].first;
    const double share = static_cast<double>(running) / static_cast<double>(total - outside);
    if (!for50 && share >= 0.50) for50 = i + 1;
    if (!for90 && share >= 0.90) for90 = i + 1;
    if (!for99 && share >= 0.99) for99 = i + 1;
  }
  std::cout << std::format("functions that account for 50% / 90% / 99% of the game code executed: {} / {} / {}\n", for50,
                           for90, for99);
  std::cout << "   start      size  share   cumul.  entered      what it uses\n";
  running = 0;
  for (std::size_t i = 0; i < order.size() && i < top; ++i) {
    const Function* f = d.function_at(order[i].second);
    const Heat& h = heat[order[i].second];
    running += h.instructions;
    bool fpu = false, vu = false, mmi = false, calls = false, pointer_calls = false, sys = false;
    for (std::uint32_t pc = f->start; pc < f->end; pc += 4) {
      const Insn& insn = d.insns[d.index_of(pc)];
      const std::uint32_t opcode = insn.raw >> 26;
      fpu |= opcode == 0x11 || insn.op == Op::LWC1 || insn.op == Op::SWC1;
      vu |= opcode == 0x12 || insn.op == Op::LQC2 || insn.op == Op::SQC2;
      mmi |= opcode == 0x1C || insn.op == Op::LQ || insn.op == Op::SQ;
      calls |= insn.flow == Flow::Call;
      pointer_calls |= insn.flow == Flow::CallReg;
      sys |= insn.flow == Flow::Syscall;
    }
    std::string uses;
    if (fpu) uses += "fpu ";
    if (vu) uses += "vu0 ";
    if (mmi) uses += "128-bit ";
    if (calls) uses += "calls ";
    if (pointer_calls) uses += "pointer-calls ";
    if (f->indirect_jumps) uses += "switch ";
    if (sys) uses += "syscall ";
    std::cout << std::format("  {:08X} {:>6} {:>5.1f}%  {:>5.1f}%  {:>10}  {}\n", f->start, f->instructions,
                             100.0 * static_cast<double>(h.instructions) / static_cast<double>(total - outside),
                             100.0 * static_cast<double>(running) / static_cast<double>(total - outside), h.entries, uses);
  }
  std::vector<std::pair<std::uint64_t, std::size_t>> ops;
  for (std::size_t i = 1; i < dynamic.size(); ++i)
    if (dynamic[i]) ops.emplace_back(dynamic[i], i);
  std::sort(ops.rbegin(), ops.rend());
  std::cout << "most executed instructions (share of all executed):\n ";
  for (std::size_t i = 0; i < ops.size() && i < 24; ++i)
    std::cout << std::format(" {} {:.1f}%", r5900::mnemonic(static_cast<Op>(ops[i].second)),
                             100.0 * static_cast<double>(ops[i].first) / static_cast<double>(total - outside));
  std::cout << '\n';
  return 0;
}

#ifdef HP2_HAVE_LLVM
// ---- lift ----------------------------------------------------------------------------------

// Tries to translate the `top` hottest functions (trace recorded with
// --trace-counts), says why the others cannot be, and writes the list of
// translated ones (input of the launcher's --lift-check).
int cmd_lift(const fs::path& elf_path, const fs::path& trace_dir, std::size_t top, const fs::path& out) {
  const ElfImage elf = load_elf(elf_path);
  const Discovery d = discover(elf);
  std::ifstream in(trace_dir / "blocks.tsv");
  if (!in) fail("cannot open '{}'", to_utf8(trace_dir / "blocks.tsv"));
  std::map<std::uint32_t, std::uint64_t> heat;
  std::uint64_t total = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string b, e;
    unsigned frame = 0;
    std::uint64_t count = 0;
    if (!(fields >> b >> e >> frame >> count)) fail("the trace has no execution counts (use --trace-counts)");
    const std::uint32_t begin = parse_address(b), end = parse_address(e);
    const std::uint64_t executed = count * ((end - begin) / 4 + 1);
    if (const Function* f = d.function_at(begin)) {
      heat[f->start] += executed;
      total += executed;
    }
  }
  std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
  for (const auto& [start, n] : heat) order.emplace_back(n, start);
  std::sort(order.rbegin(), order.rend());

  StateLayout layout;  // any layout will do for this report
  layout.pc = 0;
  layout.gpr = 16;
  layout.hi = 16 + 32 * 16;
  layout.lo = layout.hi + 8;
  layout.fpr = layout.lo + 8;
  layout.fcsr = layout.fpr + 128;
  layout.fpacc = layout.fcsr + 4;
  layout.hi1 = layout.fpacc + 4;
  layout.lo1 = layout.hi1 + 8;
  layout.vf = layout.lo1 + 8;
  layout.cycle_quota = layout.vf + 32 * 16;
  layout.has_exception = layout.cycle_quota + 4;
  Translator t(elf, d, layout, 0x04000000);
  std::map<std::string, std::pair<std::size_t, std::uint64_t>> reasons;  // kind -> functions, instructions
  std::uint64_t lifted_share = 0;
  std::string list = "# hottest game functions the translator spike can translate (hp2recomp lift)\n";
  std::cout << "   start   share  result\n";
  for (std::size_t i = 0; i < order.size() && i < top; ++i) {
    const auto [n, start] = order[i];
    const double share = 100.0 * static_cast<double>(n) / static_cast<double>(total);
    const auto why = t.add_function(start);
    if (!why) {
      lifted_share += n;
      list += std::format("{:08X}  # {:.3f}% of executed game instructions\n", start, share);
    } else {
      std::string kind = *why;
      if (const auto colon = kind.find(": "); kind.starts_with("instruction at") && colon != std::string::npos)
        kind = "instruction " + kind.substr(colon + 2, kind.find(' ', colon + 2) - colon - 2);
      else if (const auto at = kind.find(" at "); at != std::string::npos)
        kind = kind.substr(0, at);
      auto& r = reasons[kind];
      ++r.first;
      r.second += n;
    }
    if (i < 40) std::cout << std::format("  {:08X} {:>5.2f}%  {}\n", start, share, why ? *why : "translated");
  }
  const auto t0 = std::chrono::steady_clock::now();
  t.compile();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << std::format("\ntranslated {} of the {} hottest functions; they execute {:.1f}% of the game's instructions "
                           "in the session\n", t.size(), std::min(top, order.size()),
                           100.0 * static_cast<double>(lifted_share) / static_cast<double>(total));
  std::cout << std::format("optimising and compiling them to x64 took {:.2f} s\n", seconds);
  std::cout << std::format("translated instructions: {}; direct calls: {}; places that hand over to the reference core: {}\n",
                           t.stats().instructions, t.stats().calls, t.stats().stops);
  std::vector<std::pair<std::size_t, std::string>> stops;
  for (const auto& [why, n] : t.stop_reasons()) stops.emplace_back(n, why);
  std::sort(stops.rbegin(), stops.rend());
  std::cout << "hand-over places by reason (first 20):\n";
  for (std::size_t i = 0; i < stops.size() && i < 20; ++i) std::cout << std::format("  {:>6}  {}\n", stops[i].first, stops[i].second);
  std::vector<std::pair<std::uint64_t, std::string>> by_weight;
  for (const auto& [kind, r] : reasons) by_weight.emplace_back(r.second, kind);
  std::sort(by_weight.rbegin(), by_weight.rend());
  std::cout << "not translated, by reason (functions, share of executed instructions):\n";
  for (const auto& [n, kind] : by_weight)
    std::cout << std::format("  {:>4}  {:>5.1f}%  {}\n", reasons[kind].first,
                             100.0 * static_cast<double>(n) / static_cast<double>(total), kind);
  if (!out.empty()) {
    write_file_replacing(out, ByteView(reinterpret_cast<const std::uint8_t*>(list.data()), list.size()));
    std::cout << "wrote " << to_utf8(out) << '\n';
  }
  return 0;
}
#endif

void usage() {
  std::cout << "hp2recomp - stage R1 developer tool (static recompilation)\n"
               "  hp2recomp info <elf>\n"
               "  hp2recomp functions <elf> [--out <tsv>]\n"
               "  hp2recomp disasm <elf> <hex address> [count]\n"
               "  hp2recomp check-trace <elf> <trace dir>\n"
               "  hp2recomp hot <elf> <trace dir> [how many]     (trace recorded with --trace-counts)\n"
               "  hp2recomp lift <elf> <trace dir> <how many> [--out <list>]   (builds with LLVM)\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    std::vector<std::wstring> a(argv + 1, argv + argc);
    if (a.empty()) {
      usage();
      return 2;
    }
    auto narrow = [](const std::wstring& w) { return to_utf8(fs::path(w)); };
    if (a[0] == L"info" && a.size() == 2) return cmd_info(a[1]);
    if (a[0] == L"functions" && (a.size() == 2 || (a.size() == 4 && a[2] == L"--out")))
      return cmd_functions(a[1], a.size() == 4 ? fs::path(a[3]) : fs::path());
    if (a[0] == L"disasm" && (a.size() == 3 || a.size() == 4))
      return cmd_disasm(a[1], parse_address(narrow(a[2])),
                        a.size() == 4 ? static_cast<std::uint32_t>(std::stoul(a[3])) : 32);
    if (a[0] == L"check-trace" && a.size() == 3) return cmd_check_trace(a[1], a[2]);
    if (a[0] == L"hot" && (a.size() == 3 || a.size() == 4))
      return cmd_hot(a[1], a[2], a.size() == 4 ? std::stoul(a[3]) : 30);
#ifdef HP2_HAVE_LLVM
    if (a[0] == L"lift" && (a.size() == 4 || (a.size() == 6 && a[4] == L"--out")))
      return cmd_lift(a[1], a[2], std::stoul(a[3]), a.size() == 6 ? fs::path(a[5]) : fs::path());
#endif
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 3;
  }
}
