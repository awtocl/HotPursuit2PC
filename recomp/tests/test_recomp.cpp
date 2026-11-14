// Recompiler tests on synthetic input: hand-assembled instructions and a
// small program built in memory. No game data is involved.
#include "minitest.h"

#include <algorithm>

#include "hp2/discover.h"
#include "hp2/vu.h"

using namespace hp2;
using r5900::Flow;
using r5900::Op;

namespace {

// ---- a few instruction encoders (MIPS field layout) ------------------------------------------
constexpr unsigned zero = 0, at = 1, v0 = 2, a0 = 4, a1 = 5, sp = 29, ra = 31;

constexpr std::uint32_t itype(unsigned opcode, unsigned rs, unsigned rt, int imm) {
  return (opcode << 26) | (rs << 21) | (rt << 16) | (static_cast<std::uint32_t>(imm) & 0xFFFF);
}
constexpr std::uint32_t rtype(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned fn) {
  return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn;
}
constexpr std::uint32_t nop = 0;
constexpr std::uint32_t addiu(unsigned rt, unsigned rs, int imm) { return itype(0x09, rs, rt, imm); }
constexpr std::uint32_t sltiu(unsigned rt, unsigned rs, int imm) { return itype(0x0B, rs, rt, imm); }
constexpr std::uint32_t lui(unsigned rt, int imm) { return itype(0x0F, 0, rt, imm); }
constexpr std::uint32_t lw(unsigned rt, int off, unsigned base) { return itype(0x23, base, rt, off); }
constexpr std::uint32_t sw(unsigned rt, int off, unsigned base) { return itype(0x2B, base, rt, off); }
constexpr std::uint32_t jal(std::uint32_t target) { return 0x0C000000u | ((target >> 2) & 0x03FFFFFF); }
constexpr std::uint32_t jr(unsigned rs) { return rtype(rs, 0, 0, 0, 0x08); }
constexpr std::uint32_t sll(unsigned rd, unsigned rt, unsigned sa) { return rtype(0, rt, rd, sa, 0x00); }
constexpr std::uint32_t addu(unsigned rd, unsigned rs, unsigned rt) { return rtype(rs, rt, rd, 0, 0x21); }
// Branch at address `pc` to address `target`.
constexpr std::uint32_t beq(unsigned rs, unsigned rt, std::uint32_t pc, std::uint32_t target) {
  return itype(0x04, rs, rt, static_cast<int>(target - (pc + 4)) / 4);
}

// ---- a minimal ELF around a code section and a data section ---------------------------------
constexpr std::uint32_t kText = 0x00100000, kData = 0x00200000;

void put16(Bytes& b, std::size_t off, std::uint16_t v) {
  b[off] = static_cast<std::uint8_t>(v);
  b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(Bytes& b, std::size_t off, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

Bytes make_elf(const std::vector<std::uint32_t>& text, const std::vector<std::uint32_t>& data,
               std::uint32_t entry) {
  const std::string names = std::string("\0.text\0.rodata\0.shstrtab\0", 25);
  const std::uint32_t text_off = 0x100, text_size = static_cast<std::uint32_t>(text.size() * 4);
  const std::uint32_t data_off = text_off + text_size, data_size = static_cast<std::uint32_t>(data.size() * 4);
  const std::uint32_t str_off = data_off + data_size;
  const std::uint32_t sh_off = (str_off + static_cast<std::uint32_t>(names.size()) + 3) & ~3u;
  Bytes b(sh_off + 4 * 40, 0);
  b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
  b[4] = 1;  // 32-bit
  b[5] = 1;  // little-endian
  b[6] = 1;
  put16(b, 16, 2);   // executable
  put16(b, 18, 8);   // MIPS
  put32(b, 20, 1);
  put32(b, 24, entry);
  put32(b, 28, 52);      // program headers
  put32(b, 32, sh_off);  // section headers
  put16(b, 40, 52);
  put16(b, 42, 32); put16(b, 44, 2);
  put16(b, 46, 40); put16(b, 48, 4); put16(b, 50, 3);
  auto segment = [&](std::size_t at_off, std::uint32_t off, std::uint32_t vaddr, std::uint32_t size,
                     std::uint32_t flags) {
    put32(b, at_off, 1); put32(b, at_off + 4, off); put32(b, at_off + 8, vaddr); put32(b, at_off + 12, vaddr);
    put32(b, at_off + 16, size); put32(b, at_off + 20, size); put32(b, at_off + 24, flags);
  };
  segment(52, text_off, kText, text_size, 5);
  segment(84, data_off, kData, data_size, 6);
  for (std::size_t i = 0; i < text.size(); ++i) put32(b, text_off + i * 4, text[i]);
  for (std::size_t i = 0; i < data.size(); ++i) put32(b, data_off + i * 4, data[i]);
  std::copy(names.begin(), names.end(), b.begin() + str_off);
  auto section = [&](unsigned index, std::uint32_t name, std::uint32_t type, std::uint32_t flags,
                     std::uint32_t addr, std::uint32_t off, std::uint32_t size) {
    const std::size_t p = sh_off + index * 40;
    put32(b, p, name); put32(b, p + 4, type); put32(b, p + 8, flags); put32(b, p + 12, addr);
    put32(b, p + 16, off); put32(b, p + 20, size);
  };
  section(1, 1, 1, 0x6, kText, text_off, text_size);            // .text: alloc + exec
  section(2, 7, 1, 0x2, kData, data_off, data_size);            // .rodata: alloc
  section(3, 15, 3, 0, 0, str_off, static_cast<std::uint32_t>(names.size()));
  return b;
}

// The program used by the discovery tests. Addresses in the comments.
struct Program {
  std::vector<std::uint32_t> text, data;
};
Program make_program() {
  Program p;
  p.text = {
      /* 100000 main */ addiu(sp, sp, -16),
      /* 100004      */ jal(0x10001C),
      /* 100008      */ nop,
      /* 10000C      */ jal(0x100024),
      /* 100010      */ nop,
      /* 100014      */ jr(ra),
      /* 100018      */ addiu(sp, sp, 16),
      /* 10001C f1   */ jr(ra),
      /* 100020      */ nop,
      /* 100024 f2   */ sltiu(at, a0, 3),
      /* 100028      */ beq(at, zero, 0x100028, 0x100068),
      /* 10002C      */ sll(v0, a0, 2),
      /* 100030      */ lui(at, 0x0020),
      /* 100034      */ addu(at, at, v0),
      /* 100038      */ lw(v0, 0, at),
      /* 10003C      */ jr(v0),
      /* 100040      */ nop,
      /* 100044 case0*/ addiu(v0, zero, 10),
      /* 100048      */ beq(zero, zero, 0x100048, 0x100070),
      /* 10004C      */ nop,
      /* 100050 case1*/ addiu(v0, zero, 20),
      /* 100054      */ beq(zero, zero, 0x100054, 0x100070),
      /* 100058      */ nop,
      /* 10005C case2*/ addiu(v0, zero, 30),
      /* 100060      */ beq(zero, zero, 0x100060, 0x100070),
      /* 100064      */ nop,
      /* 100068 deflt*/ addiu(v0, zero, -1),
      /* 10006C      */ nop,
      /* 100070 end  */ jr(ra),
      /* 100074      */ nop,
      /* 100078      */ nop,  // filler
      /* 10007C      */ nop,  // filler
      /* 100080 f3   */ addiu(sp, sp, -8),  // only referenced by a pointer in data
      /* 100084      */ jr(ra),
      /* 100088      */ addiu(sp, sp, 8),
      /* 10008C f4   */ jr(ra),             // referenced by nothing
      /* 100090      */ nop,
  };
  // The switch table, immediately followed by a pointer to f3.
  p.data = {0x100044, 0x100050, 0x10005C, 0x100080, 0xDEADBEEF};
  return p;
}

}  // namespace

TEST(decode_known_encodings) {
  CHECK_EQ(r5900::disassemble(0, 0x27BDFFF0), std::string("addiu sp, sp, -16"));
  CHECK_EQ(r5900::disassemble(0, 0x03E00008), std::string("jr ra"));
  CHECK_EQ(r5900::disassemble(0x100004, 0x0C040007), std::string("jal 0x0010001C"));
  CHECK_EQ(r5900::disassemble(0, 0x00000000), std::string("nop"));
  CHECK_EQ(r5900::disassemble(0, 0x7FBF0000), std::string("sq ra, 0(sp)"));
  CHECK_EQ(r5900::disassemble(0, 0x46010000), std::string("add.s f0, f0, f1"));
  CHECK_EQ(r5900::disassemble(0, 0x70851008), std::string("paddw v0, a0, a1"));
  CHECK_EQ(r5900::disassemble(0, 0x708514A9), std::string("por v0, a0, a1"));
  CHECK_EQ(r5900::disassemble(0, lw(v0, -4, sp)), std::string("lw v0, -4(sp)"));
  CHECK(r5900::decode(0x0000000C).op == Op::SYSCALL);
  CHECK(r5900::decode(0x42000018).op == Op::ERET);
  CHECK(r5900::decode(0x4BE00028).op == Op::VADD);
  CHECK(r5900::decode(0x4A0003BC).op == Op::VDIV);
  CHECK(r5900::decode(0x4A000038).op == Op::VCALLMS);
  CHECK(!r5900::decode(0x74000000).valid());  // primary opcode 0x1D is not assigned
  CHECK(!r5900::decode(0x00000001).valid());  // SPECIAL function 1 is not assigned
}

TEST(decode_control_flow) {
  CHECK(r5900::decode(jr(ra)).flow == Flow::JumpReg);
  CHECK(r5900::decode(jal(0x100000)).flow == Flow::Call);
  CHECK(r5900::decode(0x0000000C).flow == Flow::Syscall);
  const r5900::Insn b = r5900::decode(beq(at, zero, 0x100028, 0x100068));
  CHECK(b.flow == Flow::Branch);
  CHECK(!b.unconditional());
  CHECK_EQ(r5900::target_of(0x100028, b), 0x100068u);
  CHECK(r5900::decode(beq(zero, zero, 0x100048, 0x100070)).unconditional());
  CHECK_EQ(r5900::target_of(0x100004, r5900::decode(jal(0x10001C))), 0x10001Cu);
  // "likely" branches: beql (opcode 0x14)
  const r5900::Insn l = r5900::decode(itype(0x14, at, zero, 3));
  CHECK(l.flow == Flow::Branch);
  CHECK(l.likely);
  CHECK(l.has_delay_slot());
}

TEST(decode_destination_register) {
  CHECK_EQ(r5900::dest_gpr(r5900::decode(lw(v0, 0, sp))), 2);
  CHECK_EQ(r5900::dest_gpr(r5900::decode(sw(v0, 0, sp))), -1);
  CHECK_EQ(r5900::dest_gpr(r5900::decode(jal(0x100000))), 31);
  CHECK_EQ(r5900::dest_gpr(r5900::decode(addiu(zero, zero, 1))), -1);
  CHECK_EQ(r5900::dest_gpr(r5900::decode(addu(a1, a0, v0))), 5);
  CHECK_EQ(r5900::dest_gpr(r5900::decode(jr(ra))), -1);
}

TEST(every_opcode_has_a_distinct_mnemonic) {
  std::vector<std::string_view> names;
  for (std::size_t i = 0; i < r5900::kOpCount; ++i) names.push_back(r5900::mnemonic(static_cast<Op>(i)));
  std::sort(names.begin(), names.end());
  CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
  CHECK(!names.front().empty());
}

TEST(elf_parse_reads_segments_and_sections) {
  const Program p = make_program();
  const ElfImage elf = ElfImage::parse(make_elf(p.text, p.data, kText));
  CHECK_EQ(elf.entry(), kText);
  CHECK_EQ(elf.segments().size(), static_cast<std::size_t>(2));
  REQUIRE(elf.section(".text") != nullptr);
  CHECK(elf.section(".text")->executable());
  CHECK_EQ(elf.section(".text")->size, static_cast<std::uint32_t>(p.text.size() * 4));
  REQUIRE(elf.section(".rodata") != nullptr);
  CHECK_EQ(elf.word(kText), p.text[0]);
  CHECK_EQ(elf.word(kData + 4), 0x100050u);
  CHECK(!elf.try_word(kData + static_cast<std::uint32_t>(p.data.size() * 4)).has_value());
  CHECK(!elf.contains(0x00300000));
  CHECK_EQ(elf.image_begin(), kText);
}

TEST(elf_parse_rejects_bad_files) {
  const Program p = make_program();
  const Bytes good = make_elf(p.text, p.data, kText);
  CHECK_THROWS(ElfImage::parse(Bytes(good.begin(), good.begin() + 40)));  // truncated header
  Bytes not_elf = good;
  not_elf[1] = 'X';
  CHECK_THROWS(ElfImage::parse(not_elf));
  Bytes wrong_machine = good;
  put16(wrong_machine, 18, 62);  // x86-64
  CHECK_THROWS(ElfImage::parse(wrong_machine));
  Bytes segment_outside = good;
  put32(segment_outside, 52 + 16, 0x7FFFFFFF);  // file size of the first segment
  CHECK_THROWS(ElfImage::parse(segment_outside));
}

TEST(discovery_finds_called_pointed_and_unreferenced_functions) {
  const Program p = make_program();
  const ElfImage elf = ElfImage::parse(make_elf(p.text, p.data, kText));
  const Discovery d = discover(elf);

  REQUIRE(d.functions.size() == 5);
  const std::uint32_t starts[5] = {0x100000, 0x10001C, 0x100024, 0x100080, 0x10008C};
  const FuncOrigin origins[5] = {FuncOrigin::Entry, FuncOrigin::Call, FuncOrigin::Call, FuncOrigin::Pointer,
                                 FuncOrigin::Unreferenced};
  for (int i = 0; i < 5; ++i) {
    CHECK_EQ(d.functions[i].start, starts[i]);
    CHECK(d.functions[i].origin == origins[i]);
  }
  CHECK_EQ(d.functions[0].end, 0x10001Cu);
  CHECK_EQ(d.functions[4].end, 0x100094u);
  CHECK_EQ(d.functions[0].instructions, 7u);
  CHECK_EQ(d.functions[2].instructions, 21u);  // 100024..100074, everything reached
  CHECK_EQ(d.functions[2].end, 0x100080u);     // the two filler words belong to its range
  CHECK_EQ(d.functions[2].indirect_jumps, 1u);

  CHECK_EQ(d.invalid_words(), static_cast<std::size_t>(0));
  CHECK_EQ(d.count(kPadding), static_cast<std::size_t>(2));
  CHECK_EQ(d.count(kOrphan), static_cast<std::size_t>(0));
  CHECK_EQ(d.count(kReached), p.text.size() - 2);
  CHECK(d.cross_branches.empty());
  CHECK(d.unresolved_jumps.empty());
  CHECK(d.function_at(0x100050) == &d.functions[2]);
  CHECK(d.function_at(0x0FFFFC) == nullptr);
}

TEST(discovery_recognises_the_switch) {
  const Program p = make_program();
  const Discovery d = discover(ElfImage::parse(make_elf(p.text, p.data, kText)));
  REQUIRE(d.jump_tables.size() == 1);
  const JumpTable& t = d.jump_tables[0];
  CHECK_EQ(t.jump, 0x10003Cu);
  CHECK_EQ(t.table, kData);
  // Three cases (the index check says so): the pointer to f3 that follows the table is not a case.
  REQUIRE(t.targets.size() == 3);
  CHECK_EQ(t.targets[0], 0x100044u);
  CHECK_EQ(t.targets[2], 0x10005Cu);
  for (std::uint32_t target : t.targets) CHECK((d.flags_at(target) & (kLeader | kTableTarget)) == (kLeader | kTableTarget));
  CHECK(!(d.flags_at(0x100080) & kTableTarget));
  CHECK(d.flags_at(0x100080) & kAddrTaken);
}

TEST(discovery_marks_leaders_and_delay_slots) {
  const Program p = make_program();
  const Discovery d = discover(ElfImage::parse(make_elf(p.text, p.data, kText)));
  for (std::uint32_t leader : {0x100000u, 0x10000Cu, 0x100014u, 0x10001Cu, 0x100024u, 0x100030u, 0x100068u, 0x100070u})
    CHECK(d.flags_at(leader) & kLeader);
  for (std::uint32_t not_leader : {0x100004u, 0x100008u, 0x100034u, 0x100038u, 0x10006Cu})
    CHECK(!(d.flags_at(not_leader) & kLeader));
  for (std::uint32_t slot : {0x100008u, 0x100018u, 0x10002Cu, 0x100040u})
    CHECK(d.flags_at(slot) & kDelaySlot);
  CHECK(d.flags_at(0x100078) & kPadding);
}

TEST(discovery_without_the_index_check_still_covers_the_cases) {
  Program p = make_program();
  p.text[9] = nop;  // remove "sltiu at, a0, 3": the table length is no longer known
  const Discovery d = discover(ElfImage::parse(make_elf(p.text, p.data, kText)));
  // The switch is not recognised, and is reported as such. The partition into
  // functions suffers (the cases, whose addresses are in data, are taken for
  // functions called through pointers, and the branches between them are
  // reported), but nothing is lost: every case is an entry point, and all
  // code is still covered.
  CHECK(d.jump_tables.empty());
  REQUIRE(d.unresolved_jumps.size() == 1);
  CHECK_EQ(d.unresolved_jumps[0], 0x10003Cu);
  for (std::uint32_t target : {0x100044u, 0x100050u, 0x10005Cu, 0x100080u}) {
    CHECK(d.flags_at(target) & kLeader);
    CHECK(d.flags_at(target) & kReached);
    CHECK(d.flags_at(target) & kFuncStart);
  }
  CHECK(!d.cross_branches.empty());
  CHECK_EQ(d.count(kReached), p.text.size() - 2);
}

TEST(discovery_adopts_code_that_nothing_leads_to) {
  Program p = make_program();
  // In f2, replace the default case's fall-through by a return, and remove the
  // branch that leads to the default case: 100068..10006C become unreachable
  // code in the middle of the function, with other code after them.
  p.text[10] = nop;  // was "beq at, zero, default"
  const Discovery d = discover(ElfImage::parse(make_elf(p.text, p.data, kText)));
  CHECK(d.flags_at(0x100068) & kOrphan);
  CHECK(d.flags_at(0x100068) & kLeader);
  CHECK(d.flags_at(0x100068) & kReached);
  CHECK(d.function_at(0x100068) == d.function_at(0x100024));  // still inside f2: the cases pass over it
  CHECK_EQ(d.count(kOrphan), static_cast<std::size_t>(1));
}

TEST(discovery_treats_a_jump_out_of_a_function_as_a_new_function) {
  Program p = make_program();
  // f1 becomes: "j f4; nop" (a tail call to code nobody else references).
  p.text[7] = 0x08000000u | (0x10008Cu >> 2);
  const Discovery d = discover(ElfImage::parse(make_elf(p.text, p.data, kText)));
  const Function* f4 = d.function_at(0x10008C);
  REQUIRE(f4 != nullptr);
  CHECK_EQ(f4->start, 0x10008Cu);
  CHECK(f4->origin == FuncOrigin::Jump);
  CHECK(d.cross_branches.empty());
}

TEST(vu_microcode_decoding) {
  // upper "nop" and lower "nop" (an empty move), as they pad most of the game's microcode
  const vu::Pair padding = vu::decode(0x8000033Cu, 0x000002FFu);
  CHECK(padding.up == r5900::Op::VNOP);
  CHECK(padding.lo == vu::Lower::MOVE);
  CHECK(!padding.lower_is_branch());
  // xgkick vi1 (lower special, vector0 index 0x1B)
  CHECK(vu::decode_lower(0x800006FCu | (1u << 11)) == vu::Lower::XGKICK);
  // b with offset -2 pairs from address 0x100: target 0x100 + 8 - 16
  const std::uint32_t b = (0x20u << 25) | 0x7FEu;
  CHECK(vu::decode_lower(b) == vu::Lower::B);
  CHECK_EQ(vu::branch_target(0x100, b), 0xF8u);
  // fcand reads flags; the I bit turns the lower word into an immediate
  const vu::Pair fc = vu::decode((0x12u << 25), 0x000002FFu);
  CHECK(fc.lower_reads_flags());
  const vu::Pair imm = vu::decode((0x12u << 25), 0x800002FFu);
  CHECK(imm.i_bit());
  CHECK(!imm.lower_reads_flags());
  // macro-only operations do not exist in the upper word
  CHECK(vu::decode_upper(0x00000030u) == r5900::Op::Invalid);  // index 0x30 = viadd in macro mode
}

MINITEST_MAIN()
