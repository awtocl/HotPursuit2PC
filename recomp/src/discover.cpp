#include "hp2/discover.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace hp2 {

using r5900::Flow;
using r5900::Insn;
using r5900::Op;

std::string_view to_string(FuncOrigin origin) {
  switch (origin) {
    case FuncOrigin::Entry: return "entry";
    case FuncOrigin::Call: return "call";
    case FuncOrigin::Jump: return "jump";
    case FuncOrigin::Pointer: return "pointer";
    case FuncOrigin::Unreferenced: return "unreferenced";
    case FuncOrigin::Hint: return "hint";
  }
  return "?";
}

const Function* Discovery::function_at(std::uint32_t addr) const {
  auto it = std::upper_bound(functions.begin(), functions.end(), addr,
                             [](std::uint32_t a, const Function& f) { return a < f.start; });
  if (it == functions.begin()) return nullptr;
  --it;
  return addr < it->end ? &*it : nullptr;
}

std::size_t Discovery::count(std::uint8_t flag) const {
  return static_cast<std::size_t>(
      std::count_if(flags.begin(), flags.end(), [flag](std::uint8_t f) { return (f & flag) != 0; }));
}

std::size_t Discovery::invalid_words() const {
  return static_cast<std::size_t>(
      std::count_if(insns.begin(), insns.end(), [](const Insn& i) { return !i.valid(); }));
}

namespace {

constexpr int kBackwardWindow = 24;  // instructions searched backwards for a definition
constexpr std::uint32_t kMaxTableEntries = 4096;

class Discoverer {
 public:
  Discoverer(const ElfImage& elf, Discovery& d) : elf_(elf), d_(d) {}

  void run(const DiscoverOptions& options) {
    locate_text();
    decode_all();
    find_address_taken();
    seed(options);
    resolve_switches();
    seed_pointers();
    classify_jumps();
    place_boundaries();

    d_.flags = permanent_;
    visited_.assign(d_.flags.size(), 0);
    for (const Function& f : d_.functions) flag(f.start) |= kFuncStart | kLeader;
    for (Function& f : d_.functions) traverse(f, f.start);
    adopt_orphans();

    for (Function& f : d_.functions)
      for (std::uint32_t a = f.start; a < f.end; a += 4)
        if (flag(a) & kReached) ++f.instructions;
    for (auto& [jump, table] : tables_) d_.jump_tables.push_back(std::move(table));
    std::sort(d_.unresolved_jumps.begin(), d_.unresolved_jumps.end());
    std::sort(d_.cross_branches.begin(), d_.cross_branches.end());
    d_.cross_branches.erase(std::unique(d_.cross_branches.begin(), d_.cross_branches.end()),
                            d_.cross_branches.end());
  }

 private:
  const ElfImage& elf_;
  Discovery& d_;
  std::vector<std::uint8_t> permanent_;  // kAddrTaken | kTableTarget | kPadding
  std::vector<std::uint8_t> visited_;    // processed as part of a straight run
  std::set<std::uint32_t> address_taken_;
  std::map<std::uint32_t, FuncOrigin> starts_;
  std::map<std::uint32_t, JumpTable> tables_;                    // by the address of the jump
  std::vector<std::pair<std::uint32_t, std::uint32_t>> edges_;   // transfers that stay inside a function

  const Insn& insn(std::uint32_t addr) const { return d_.insns[d_.index_of(addr)]; }
  std::uint8_t& flag(std::uint32_t addr) { return d_.flags[d_.index_of(addr)]; }
  std::uint32_t address_of(std::size_t index) const {
    return d_.text_begin + static_cast<std::uint32_t>(index * 4);
  }

  void locate_text() {
    if (const ElfSection* s = elf_.section(".text"); s && s->executable() && s->has_file_data() && s->size >= 8) {
      d_.text_begin = s->addr;
      d_.text_end = s->addr + (s->size & ~3u);
    } else {
      for (const ElfSegment& seg : elf_.segments())
        if (seg.loadable() && seg.executable() && seg.filesz >= 8) {
          d_.text_begin = seg.vaddr;
          d_.text_end = seg.vaddr + (seg.filesz & ~3u);
          break;
        }
    }
    if (d_.text_end <= d_.text_begin) fail("no code section found in the executable");
    if (d_.text_begin & 3) fail("the code section starts at {:08X}, which is not word-aligned", d_.text_begin);
    if (!elf_.contains(d_.text_begin, d_.text_end - d_.text_begin))
      fail("the code section {:08X}..{:08X} is not backed by file data", d_.text_begin, d_.text_end);
  }

  void decode_all() {
    const ByteView bytes = elf_.bytes(d_.text_begin, d_.text_end - d_.text_begin);
    const std::size_t n = bytes.size() / 4;
    d_.insns.resize(n);
    permanent_.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) d_.insns[i] = r5900::decode(read_u32le(bytes, i * 4));
  }

  void note_address(std::uint32_t value) {
    if (!d_.in_text(value)) return;
    address_taken_.insert(value);
    permanent_[d_.index_of(value)] |= kAddrTaken;
  }

  // Code addresses that the program stores or computes: candidates for
  // indirect calls and jumps.
  void find_address_taken() {
    // (1) words in initialised data that point into the code section
    auto scan = [&](std::uint32_t addr, std::uint32_t size) {
      const std::uint32_t first = (addr + 3) & ~3u;
      if (size < 4 || first - addr > size - 4) return;
      const ByteView bytes = elf_.bytes(first, (size - (first - addr)) & ~3u);
      for (std::size_t off = 0; off + 4 <= bytes.size(); off += 4) note_address(read_u32le(bytes, off));
    };
    bool any_section = false;
    for (const ElfSection& s : elf_.sections())
      if (s.alloc() && !s.executable() && s.has_file_data() && s.size >= 4 && elf_.contains(s.addr, s.size)) {
        scan(s.addr, s.size);
        any_section = true;
      }
    if (!any_section)
      for (const ElfSegment& seg : elf_.segments())
        if (seg.loadable() && !seg.executable() && seg.filesz >= 4) scan(seg.vaddr, seg.filesz);

    // (2) addresses built in code: lui + addiu / ori
    std::array<bool, 32> known{};
    std::array<std::uint32_t, 32> high{};
    auto forget_all = [&] { known.fill(false); };
    bool after_jump = false, after_call = false;
    for (std::uint32_t pc = d_.text_begin; pc < d_.text_end; pc += 4) {
      const Insn& in = insn(pc);
      const bool was_jump = after_jump, was_call = after_call;
      after_jump = in.valid() && (in.flow == Flow::Jump || in.flow == Flow::JumpReg);
      after_call = in.valid() && (in.flow == Flow::Call || in.flow == Flow::CallReg);
      if (!in.valid()) {
        forget_all();
      } else if (in.op == Op::LUI) {
        known[in.rt()] = in.rt() != 0;
        high[in.rt()] = in.imm() << 16;
      } else if ((in.op == Op::ADDIU || in.op == Op::ORI) && known[in.rs()]) {
        note_address(in.op == Op::ADDIU ? high[in.rs()] + static_cast<std::uint32_t>(in.simm())
                                        : high[in.rs()] | in.imm());
        known[in.rt()] = false;  // rt now holds a complete address, not a high half
      } else if (const int dest = r5900::dest_gpr(in); dest > 0) {
        known[static_cast<std::size_t>(dest)] = false;
      }
      if (was_jump) forget_all();  // this was the delay slot of a jump: a new straight run begins
      if (was_call)                // ... of a call: the callee may change the caller-saved registers
        for (unsigned r : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 24u, 25u})
          known[r] = false;
    }
  }

  // Function starts that are certain.
  void seed(const DiscoverOptions& options) {
    for (std::uint32_t pc = d_.text_begin; pc < d_.text_end; pc += 4) {
      const Insn& in = insn(pc);
      if (in.flow != Flow::Call && in.flow != Flow::BranchLink) continue;
      const std::uint32_t target = r5900::target_of(pc, in);
      if (in.flow == Flow::BranchLink && target == pc + 8) continue;  // "bal" used only to read the PC
      if (d_.in_text(target)) starts_[target] = FuncOrigin::Call;
    }
    for (std::uint32_t a : options.extra_entries)
      if (d_.in_text(a)) starts_[a] = FuncOrigin::Hint;
    if (d_.in_text(elf_.entry())) starts_[elf_.entry()] = FuncOrigin::Entry;
  }

  // The constant in `reg` just before `limit`, when it was built by lui (+ addiu / ori).
  std::optional<std::uint32_t> constant_before(std::uint32_t limit, unsigned reg, int depth) const {
    std::uint32_t pc = limit;
    for (int k = 0; k < kBackwardWindow && pc >= d_.text_begin + 4; ++k) {
      pc -= 4;
      const Insn& in = insn(pc);
      if (!in.valid()) break;
      if (r5900::dest_gpr(in) != static_cast<int>(reg)) continue;
      if (in.op == Op::LUI) return in.imm() << 16;
      if ((in.op == Op::ADDIU || in.op == Op::ORI) && depth < 3) {
        if (const auto base = constant_before(pc, in.rs(), depth + 1))
          return in.op == Op::ADDIU ? *base + static_cast<std::uint32_t>(in.simm()) : *base | in.imm();
      }
      break;
    }
    return std::nullopt;
  }

  // Recognises the compiler's switch:
  //     sltiu  x, index, N          the index is checked against the number of cases
  //     beq    x, zero, default
  //     sll    i, index, 2
  //     lui/addiu  t, table         the table address is a constant
  //     addu   p, i, t
  //     lw     r, 0(p)
  //     jr     r
  std::optional<JumpTable> resolve_table(std::uint32_t jump) const {
    const unsigned reg = insn(jump).rs();
    std::uint32_t pc = jump;
    const Insn* load = nullptr;
    for (int k = 0; k < kBackwardWindow && pc >= d_.text_begin + 4; ++k) {
      pc -= 4;
      const Insn& in = insn(pc);
      if (!in.valid()) return std::nullopt;
      if (r5900::dest_gpr(in) != static_cast<int>(reg)) continue;
      if (in.op == Op::LW) load = &in;
      break;
    }
    if (!load) return std::nullopt;

    const unsigned base = load->rs();
    std::optional<std::uint32_t> table;
    for (int k = 0; k < kBackwardWindow && pc >= d_.text_begin + 4; ++k) {
      pc -= 4;
      const Insn& in = insn(pc);
      if (!in.valid()) return std::nullopt;
      if (r5900::dest_gpr(in) != static_cast<int>(base)) continue;
      if (in.op == Op::ADDU) {
        const auto a = constant_before(pc, in.rs(), 0);
        const auto b = constant_before(pc, in.rt(), 0);
        if (a && !b) table = *a + static_cast<std::uint32_t>(load->simm());
        if (b && !a) table = *b + static_cast<std::uint32_t>(load->simm());
      }
      break;
    }
    if (!table || (*table & 3)) return std::nullopt;

    std::uint32_t cases = 0;
    pc = jump;
    for (int k = 0; k < 2 * kBackwardWindow && pc >= d_.text_begin + 4; ++k) {
      pc -= 4;
      const Insn& in = insn(pc);
      if (!in.valid()) break;
      if (in.op == Op::SLTIU && in.simm() > 0) {
        cases = static_cast<std::uint32_t>(in.simm());
        break;
      }
    }
    // Without the index check the length of the table is unknown: not recognised.
    if (cases == 0 || cases > kMaxTableEntries) return std::nullopt;

    JumpTable result{jump, *table, {}};
    for (std::uint32_t i = 0; i < cases; ++i) {
      const auto word = elf_.try_word(*table + i * 4);
      if (!word || !d_.in_text(*word)) return std::nullopt;  // not a table of code addresses after all
      result.targets.push_back(*word);
    }
    return result;
  }

  void resolve_switches() {
    for (std::uint32_t pc = d_.text_begin; pc < d_.text_end; pc += 4) {
      const Insn& in = insn(pc);
      if (in.flow != Flow::JumpReg || in.rs() == 31) continue;
      if (auto table = resolve_table(pc)) {
        for (std::uint32_t target : table->targets) permanent_[d_.index_of(target)] |= kTableTarget;
        tables_.emplace(pc, std::move(*table));
      } else {
        d_.unresolved_jumps.push_back(pc);
      }
    }
  }

  // Does straight-line execution stop before word i (it follows "jr", "j" or
  // "b" and the delay slot, or an "eret", or is the first word)?
  bool ends_straight_run(std::size_t i) const {
    if (i == 0) return true;
    if (d_.insns[i - 1].valid() && d_.insns[i - 1].flow == Flow::Eret) return true;
    if (i < 2) return false;
    const Insn& branch = d_.insns[i - 2];  // word i-1 is its delay slot
    return branch.valid() && (branch.flow == Flow::JumpReg || branch.flow == Flow::Jump ||
                              (branch.flow == Flow::Branch && branch.unconditional()));
  }

  // An instruction whose address is stored in data (or built in code), that
  // is not a switch case, and before which straight-line execution stops
  // (filler in between allowed): the first instruction of a function that is
  // called through a pointer - a virtual method, a callback. These starts
  // are taken as certain, like call targets.
  void seed_pointers() {
    for (std::uint32_t a : address_taken_) {
      const std::size_t i = d_.index_of(a);
      if (permanent_[i] & kTableTarget) continue;
      std::size_t j = i;
      for (int k = 0; k < 16 && j > 0 && !ends_straight_run(j) && d_.insns[j - 1].is_nop(); ++k) --j;
      if (ends_straight_run(j)) starts_.emplace(a, FuncOrigin::Pointer);
    }
  }

  // Is there a function start s with lo < s <= hi?
  bool start_between(std::uint32_t lo, std::uint32_t hi) const {
    const auto it = starts_.upper_bound(lo);
    return it != starts_.end() && it->first <= hi;
  }

  // A direct jump ("j") either stays inside its function or is a tail call.
  // It is a tail call when its target is a known function start, or when a
  // known function start lies between the jump and its target (a function
  // cannot contain another one); the target then becomes a function start,
  // which can turn further jumps into tail calls, hence the rounds.
  void classify_jumps() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> jumps;
    for (std::uint32_t pc = d_.text_begin; pc < d_.text_end; pc += 4) {
      const Insn& in = insn(pc);
      if (in.flow != Flow::Jump) continue;
      const std::uint32_t target = r5900::target_of(pc, in);
      if (d_.in_text(target)) jumps.emplace_back(pc, target);
    }
    bool changed = true;
    while (changed) {
      changed = false;
      ++d_.passes;
      for (const auto& [pc, target] : jumps) {
        if (starts_.count(target)) continue;
        if (start_between(std::min(pc, target), std::max(pc, target))) {
          starts_.emplace(target, FuncOrigin::Jump);
          changed = true;
        }
      }
    }

    // Everything that stays inside a function.
    for (const auto& [pc, target] : jumps)
      if (!starts_.count(target)) edges_.emplace_back(pc, target);
    for (std::uint32_t pc = d_.text_begin; pc < d_.text_end; pc += 4) {
      const Insn& in = insn(pc);
      if (in.flow != Flow::Branch) continue;
      const std::uint32_t target = r5900::target_of(pc, in);
      if (d_.in_text(target)) edges_.emplace_back(pc, target);
    }
    for (const auto& [jump, table] : tables_)
      for (std::uint32_t target : table.targets) edges_.emplace_back(jump, target);
  }

  // Function boundaries: where straight-line execution ends and no branch,
  // jump or switch passes over.
  void place_boundaries() {
    const std::size_t n = d_.insns.size();
    std::vector<std::int32_t> cover(n + 1, 0);
    std::vector<std::uint8_t> is_target(n, 0);
    for (const auto& [source, target] : edges_) {
      is_target[d_.index_of(target)] = 1;
      const std::size_t lo = d_.index_of(std::min(source, target)), hi = d_.index_of(std::max(source, target));
      if (lo == hi) continue;
      ++cover[lo + 1];  // the words lo+1 .. hi cannot begin a function
      --cover[hi + 1];
    }
    std::vector<std::uint8_t> blocked(n, 0);
    std::int32_t depth = 0;
    for (std::size_t i = 0; i < n; ++i) {
      depth += cover[i];
      blocked[i] = depth > 0;
    }

    for (std::size_t i = 0; i < n; ++i) {
      if (!ends_straight_run(i) || blocked[i]) continue;
      // Skip the filler the linker puts between functions.
      std::size_t k = i;
      while (k < n && d_.insns[k].is_nop() && !blocked[k] && !is_target[k] &&
             !(permanent_[k] & kAddrTaken) && !starts_.count(address_of(k)))
        ++k;
      if (k < n && blocked[k]) continue;  // the "filler" leads into code that something passes over
      for (std::size_t p = i; p < k; ++p) permanent_[p] |= kPadding;
      if (k == n) continue;
      starts_.emplace(address_of(k),
                      (permanent_[k] & kAddrTaken) ? FuncOrigin::Pointer : FuncOrigin::Unreferenced);
    }

    // Transfers that pass over a function start other than their own target
    // (a branch to a function's first instruction is a tail call): recorded,
    // not hidden. Only starts that are certain can be passed over.
    for (const auto& [source, target] : edges_) {
      const std::uint32_t lo = std::min(source, target), hi = std::max(source, target);
      for (auto it = starts_.upper_bound(lo); it != starts_.end() && it->first <= hi; ++it)
        if (it->first != target) {
          d_.cross_branches.emplace_back(source, target);
          break;
        }
    }

    for (auto it = starts_.begin(); it != starts_.end(); ++it) {
      Function f;
      f.start = it->first;
      f.origin = it->second;
      const auto next = std::next(it);
      f.end = next == starts_.end() ? d_.text_end : next->first;
      d_.functions.push_back(f);
    }
  }

  void leader(std::uint32_t addr) {
    if (d_.in_text(addr)) flag(addr) |= kLeader;
  }

  // Follows the control flow inside f, starting at `root`.
  void traverse(Function& f, std::uint32_t root) {
    std::vector<std::uint32_t> work{root};
    auto inside = [&](std::uint32_t a) { return a >= f.start && a < f.end; };
    auto go = [&](std::uint32_t source, std::uint32_t target) {
      if (!d_.in_text(target)) return;  // outside the program's code: nothing to follow
      leader(target);
      if (inside(target)) work.push_back(target);
      else if (!starts_.count(target)) d_.cross_branches.emplace_back(source, target);
      // else: a jump to another function's first instruction, i.e. a tail call
    };

    while (!work.empty()) {
      std::uint32_t pc = work.back();
      work.pop_back();
      while (inside(pc)) {
        const std::size_t i = d_.index_of(pc);
        if (visited_[i]) break;
        visited_[i] = 1;
        d_.flags[i] |= kReached;
        const Insn& in = d_.insns[i];
        if (!in.valid()) break;  // ran into something that is not code
        if (in.flow == Flow::None) {
          pc += 4;
          continue;
        }
        if (in.flow == Flow::Syscall || in.flow == Flow::Break) {
          leader(pc + 4);  // execution resumes here after the kernel returns
          pc += 4;
          continue;
        }
        if (in.flow == Flow::Eret) break;

        // Everything else has a delay slot, which always belongs to this run.
        if (d_.in_text(pc + 4)) flag(pc + 4) |= kReached | kDelaySlot;
        leader(pc + 8);
        bool falls_through = true;
        switch (in.flow) {
          case Flow::Branch:
            go(pc, r5900::target_of(pc, in));
            falls_through = !in.unconditional();
            break;
          case Flow::Jump:
            go(pc, r5900::target_of(pc, in));
            falls_through = false;
            break;
          case Flow::JumpReg:
            if (in.rs() != 31) {
              ++f.indirect_jumps;
              if (const auto table = tables_.find(pc); table != tables_.end()) {
                for (std::uint32_t target : table->second.targets) go(pc, target);
              } else {
                // Not recognised: assume it can reach any address-taken instruction of this function.
                for (auto it = address_taken_.lower_bound(f.start); it != address_taken_.end() && *it < f.end; ++it)
                  go(pc, *it);
              }
            }
            falls_through = false;
            break;
          default:  // Call, CallReg, BranchLink: the callee returns to pc + 8
            break;
        }
        if (!falls_through) break;
        pc += 8;
      }
    }
  }

  // Code that the control flow of its function never reaches. Nothing shows
  // how execution gets there (dead code, or a mechanism this analysis does
  // not see), so each such run becomes an entry point of its own: all code
  // is covered, and check-trace reports whether any of it was executed.
  void adopt_orphans() {
    for (Function& f : d_.functions)
      for (std::uint32_t pc = f.start; pc < f.end; pc += 4) {
        if (flag(pc) & (kReached | kPadding)) continue;
        const Insn& in = insn(pc);
        if (!in.valid()) continue;
        if (in.is_nop()) {
          flag(pc) |= kPadding;
          continue;
        }
        flag(pc) |= kOrphan | kLeader;
        traverse(f, pc);
      }
  }
};

}  // namespace

Discovery discover(const ElfImage& elf, const DiscoverOptions& options) {
  Discovery d;
  Discoverer(elf, d).run(options);
  return d;
}

}  // namespace hp2
