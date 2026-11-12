// Recompiler tests against the owner's real executable (brief §9). It is
// opened read-only. Without a dump every test reports "skipped" (exit 77).
// The expected numbers are the ones recorded in docs/recomp-analysis.md.
#include "minitest.h"
#include "testkit.h"

#include "hp2/discover.h"
#include "hp2/io.h"

using namespace hp2;

namespace {

const ElfImage& the_elf() {
  static const ElfImage elf = [] {
    const auto dump = testkit::find_dump(HP2_SOURCE_DIR);
    if (!dump) SKIP("no dump found (put it in ROM/ or set HP2_ROM_DIR)");
    return ElfImage::parse(read_whole_file(*dump / "SLUS_203.62", 64ull << 20));
  }();
  return elf;
}

const Discovery& the_discovery() {
  static const Discovery d = discover(the_elf());
  return d;
}

}  // namespace

TEST(dump_elf_layout_is_the_documented_one) {
  const ElfImage& elf = the_elf();
  CHECK_EQ(elf.entry(), 0x00100008u);
  REQUIRE(elf.section(".text") != nullptr);
  CHECK_EQ(elf.section(".text")->addr, 0x00100000u);
  CHECK_EQ(elf.section(".text")->size, 0x001A5904u);
  REQUIRE(elf.section(".vutext") != nullptr);
  CHECK_EQ(elf.section(".vutext")->size, 0x00005090u);
  CHECK_EQ(elf.image_begin(), 0x00100000u);
  CHECK_EQ(elf.image_end(), 0x0033D8FCu);
}

TEST(dump_code_section_is_all_instructions) {
  const Discovery& d = the_discovery();
  CHECK_EQ(d.insns.size(), static_cast<std::size_t>(431681));
  CHECK_EQ(d.text_begin, 0x00100000u);
}

TEST(dump_discovery_covers_the_code_section) {
  const Discovery& d = the_discovery();
  REQUIRE(!d.functions.empty());
  // Functions are sorted, do not overlap, and tile the section.
  CHECK_EQ(d.functions.front().start, d.text_begin);
  CHECK_EQ(d.functions.back().end, d.text_end);
  for (std::size_t i = 1; i < d.functions.size(); ++i) CHECK_EQ(d.functions[i].start, d.functions[i - 1].end);
  // The entry point is a function start.
  CHECK(d.flags_at(the_elf().entry()) & kFuncStart);
  // Every word is an instruction, and is either reached or filler.
  CHECK_EQ(d.invalid_words(), static_cast<std::size_t>(0));
  CHECK_EQ(d.count(kReached), static_cast<std::size_t>(426800));
  CHECK_EQ(d.count(kPadding), static_cast<std::size_t>(4881));
  CHECK_EQ(d.count(kReached) + d.count(kPadding), d.insns.size());
}

TEST(dump_discovery_numbers_are_the_documented_ones) {
  const Discovery& d = the_discovery();
  std::size_t by_origin[6] = {};
  for (const Function& f : d.functions) ++by_origin[static_cast<std::size_t>(f.origin)];
  CHECK_EQ(d.functions.size(), static_cast<std::size_t>(10146));
  CHECK_EQ(by_origin[static_cast<std::size_t>(FuncOrigin::Entry)], static_cast<std::size_t>(1));
  CHECK_EQ(by_origin[static_cast<std::size_t>(FuncOrigin::Call)], static_cast<std::size_t>(5507));
  CHECK_EQ(by_origin[static_cast<std::size_t>(FuncOrigin::Jump)], static_cast<std::size_t>(553));
  CHECK_EQ(by_origin[static_cast<std::size_t>(FuncOrigin::Pointer)], static_cast<std::size_t>(1297));
  CHECK_EQ(by_origin[static_cast<std::size_t>(FuncOrigin::Unreferenced)], static_cast<std::size_t>(2788));
  CHECK_EQ(d.count(kLeader), static_cast<std::size_t>(90612));
  CHECK_EQ(d.count(kAddrTaken), static_cast<std::size_t>(2714));
  CHECK_EQ(d.count(kOrphan), static_cast<std::size_t>(349));
}

TEST(dump_switches_are_recognised) {
  const Discovery& d = the_discovery();
  CHECK_EQ(d.jump_tables.size(), static_cast<std::size_t>(193));
  std::size_t cases = 0;
  for (const JumpTable& t : d.jump_tables) {
    cases += t.targets.size();
    // Every case lies in the function that contains the jump.
    const Function* f = d.function_at(t.jump);
    REQUIRE(f != nullptr);
    for (std::uint32_t target : t.targets) CHECK(target >= f->start && target < f->end);
  }
  CHECK_EQ(cases, static_cast<std::size_t>(3077));
  // The three indirect jumps that are not switches belong to the C++
  // exception unwinder of the compiler's runtime library (they restore a
  // saved stack pointer and jump to a saved address).
  REQUIRE(d.unresolved_jumps.size() == 3);
  CHECK_EQ(d.unresolved_jumps[0], 0x0029EFF0u);
  CHECK_EQ(d.unresolved_jumps[1], 0x0029F9C8u);
  CHECK_EQ(d.unresolved_jumps[2], 0x0029FBB0u);
  // ... and the only branches between functions are in that same code.
  CHECK_EQ(d.cross_branches.size(), static_cast<std::size_t>(6));
  for (const auto& [source, target] : d.cross_branches) CHECK(source >= 0x0029EF00u && source < 0x002A2300u);
}

MINITEST_MAIN()
