#include "hp2/vu.h"

#include <array>

namespace hp2::vu {

namespace {

using enum Lower;
constexpr Lower xx = Lower::Invalid;

constexpr std::array<std::string_view, kLowerCount> kNames = {
#define HP2_X(name, text) text,
    HP2_VU_LOWER_OPS(HP2_X)
#undef HP2_X
};

// Indexed by bits 31..25.
constexpr std::array<Lower, 0x30> kGeneral = {
    LQ,   SQ,    xx,    xx,   ILW,   ISW,  xx,    xx,     // 00
    IADDIU, ISUBIU, xx, xx,   xx,    xx,   xx,    xx,     // 08
    FCEQ, FCSET, FCAND, FCOR, xx,    FSSET, FSAND, FSOR,  // 10
    FMEQ, xx,    FMAND, FMOR, FCGET, xx,   xx,    xx,     // 18
    B,    BAL,   xx,    xx,   JR,    JALR, xx,    xx,     // 20
    IBEQ, IBNE,  xx,    xx,   IBLTZ, IBGTZ, IBLEZ, IBGEZ, // 28
};
// Bits 31..25 = 0x40, bits 5..0 < 0x3C.
constexpr std::array<Lower, 8> kSpecial30 = {IADD, ISUB, IADDI, xx, IAND, IOR, xx, xx};
// Bits 5..0 = 0x3C + n, indexed by bits 10..6.
constexpr std::array<std::array<Lower, 0x20>, 4> kVector = {{
    {xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, MOVE, LQI, DIV, MTIR,
     RNEXT, xx, xx, xx, xx, xx, xx, xx, xx, MFP, XTOP, XGKICK, ESADD, EATANxy, ESQRT, ESIN},
    {xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, MR32, SQI, SQRT, MFIR,
     RGET, xx, xx, xx, xx, xx, xx, xx, xx, xx, XITOP, xx, ERSADD, EATANxz, ERSQRT, xx},
    {xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, LQD, RSQRT, ILWR,
     RINIT, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, ELENG, ESUM, ERCPR, EEXP},
    {xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, SQD, WAITQ, ISWR,
     RXOR, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, ERLENG, xx, WAITP, xx},
}};

}  // namespace

r5900::Op decode_upper(std::uint32_t word) {
  // Same layout as the COP2 macro instructions: rebuild one and use that decoder.
  const r5900::Op op = r5900::decode(0x4A000000u | (word & 0x01FFFFFFu)).op;
  switch (op) {  // macro-only operations do not exist in the upper word
    case r5900::Op::VIADD: case r5900::Op::VISUB: case r5900::Op::VIADDI: case r5900::Op::VIAND:
    case r5900::Op::VIOR: case r5900::Op::VCALLMS: case r5900::Op::VCALLMSR: case r5900::Op::VMOVE:
    case r5900::Op::VMR32: case r5900::Op::VLQI: case r5900::Op::VSQI: case r5900::Op::VLQD: case r5900::Op::VSQD:
    case r5900::Op::VDIV: case r5900::Op::VSQRT: case r5900::Op::VRSQRT: case r5900::Op::VWAITQ:
    case r5900::Op::VMTIR: case r5900::Op::VMFIR: case r5900::Op::VILWR: case r5900::Op::VISWR:
    case r5900::Op::VRNEXT: case r5900::Op::VRGET: case r5900::Op::VRINIT: case r5900::Op::VRXOR:
      return r5900::Op::Invalid;
    default:
      return op;
  }
}

Lower decode_lower(std::uint32_t word) {
  const std::uint32_t op = word >> 25;
  if (op < kGeneral.size()) return kGeneral[op];
  if (op != 0x40) return xx;
  const std::uint32_t fn = word & 0x3F;
  if (fn >= 0x3C) return kVector[fn & 3][(word >> 6) & 0x1F];
  if (fn >= 0x30) return kSpecial30[fn - 0x30];
  return xx;
}

Pair decode(std::uint32_t lower, std::uint32_t upper) {
  Pair p;
  p.lower = lower;
  p.upper = upper;
  p.up = decode_upper(upper);
  p.lo = p.i_bit() ? Lower::Invalid : decode_lower(lower);
  return p;
}

bool Pair::lower_is_branch() const {
  if (i_bit()) return false;
  switch (lo) {
    case B: case BAL: case JR: case JALR: case IBEQ: case IBNE: case IBLTZ: case IBGTZ: case IBLEZ: case IBGEZ:
      return true;
    default:
      return false;
  }
}

bool Pair::lower_reads_flags() const {
  if (i_bit()) return false;
  switch (lo) {
    case FCEQ: case FCAND: case FCOR: case FCGET: case FSAND: case FSOR: case FMEQ: case FMAND: case FMOR:
      return true;
    default:
      return false;
  }
}

std::string_view mnemonic(Lower op) {
  return kNames[static_cast<std::size_t>(op)];
}

std::uint32_t branch_target(std::uint32_t address, std::uint32_t lower) {
  // 11-bit signed offset in instruction pairs, relative to the pair after the branch.
  std::int32_t imm = static_cast<std::int32_t>(lower & 0x7FF);
  if (imm & 0x400) imm -= 0x800;
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(address) + 8 + imm * 8);
}

std::string disassemble(std::uint32_t address, std::uint32_t lower, std::uint32_t upper) {
  const Pair p = decode(lower, upper);
  std::string flags;
  if (upper & kBitI) flags += 'I';
  if (upper & kBitE) flags += 'E';
  if (upper & kBitM) flags += 'M';
  if (upper & kBitD) flags += 'D';
  if (upper & kBitT) flags += 'T';
  const unsigned dest = (upper >> 21) & 15, ft = (upper >> 16) & 31, fs = (upper >> 11) & 31, fd = (upper >> 6) & 31;
  static constexpr char kXyzw[] = "xyzw";
  std::string d;
  for (int i = 0; i < 4; ++i)
    if (dest & (8 >> i)) d += kXyzw[i];
  std::string up = p.up == r5900::Op::Invalid ? std::format("?{:08X}", upper & 0x07FFFFFF)
                                              : std::format("{}.{} vf{}, vf{}, vf{}", r5900::mnemonic(p.up), d, fd, fs, ft);
  if (p.up == r5900::Op::VNOP) up = "nop";
  std::string lo;
  if (p.i_bit()) {
    lo = std::format("loi 0x{:08X}", lower);
  } else {
    const unsigned ldest = (lower >> 21) & 15, it = (lower >> 16) & 31, is = (lower >> 11) & 31;
    std::string ld;
    for (int i = 0; i < 4; ++i)
      if (ldest & (8 >> i)) ld += kXyzw[i];
    if (p.lower_is_branch() && p.lo != JR && p.lo != JALR)
      lo = std::format("{} vi{}, vi{}, 0x{:04X}", mnemonic(p.lo), it, is, branch_target(address, lower));
    else if (p.lo == MOVE && ldest == 0 && it == 0 && is == 0)
      lo = "nop";
    else
      lo = std::format("{}.{} {} {} 0x{:03X}", mnemonic(p.lo), ld, it, is, lower & 0x7FF);
  }
  return std::format("{:04X}  {:08X} {:08X}  {:<5} {:<36} {}", address, upper, lower, flags, up, lo);
}

std::vector<Upload> find_uploads(const ElfImage& elf) {
  const ElfSection* s = elf.section(".vutext");
  if (!s || s->size < 8) fail("the executable has no .vutext section");
  std::vector<Upload> uploads;
  std::uint32_t a = s->addr;
  const std::uint32_t end = s->addr + (s->size & ~3u);
  while (a + 4 <= end) {
    const std::uint32_t code = elf.word(a);
    const std::uint32_t cmd = (code >> 24) & 0x7F;
    if (cmd == 0x4A) {  // MPG: NUM pairs follow, to be loaded at IMMEDIATE (in pairs)
      std::uint32_t pairs = (code >> 16) & 0xFF;
      if (pairs == 0) pairs = 256;
      const std::uint32_t data = (a + 4 + 7) & ~7u;  // the data is aligned to 64 bits
      if (data + pairs * 8 > end) break;
      uploads.push_back({data, (code & 0xFFFF) * 8, pairs});
      a = data + pairs * 8;
    } else {
      a += 4;  // NOP and the other VIF codes the packets are wrapped in
    }
  }
  return uploads;
}

}  // namespace hp2::vu
