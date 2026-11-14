// Vector unit microcode (VU1, stage R1 of ADR 0006/0007): decoder,
// disassembler, and the microprograms stored in the executable.
//
// A VU instruction is a pair of 32-bit words: lower (at the even word) and
// upper. The upper word carries the FMAC operation (same encoding as the
// EE's VU0 macro instructions, see r5900.h) and five flag bits; the lower
// word integer, load/store, branch, divide/EFU and transfer operations.
// Encodings as in the reference core's VU tables (docs/prior-art.md).
#pragma once

#include "hp2/elf.h"
#include "hp2/r5900.h"

namespace hp2::vu {

// X(Name, "mnemonic")
#define HP2_VU_LOWER_OPS(X)                                                                              \
  X(Invalid, "(invalid)") X(LQ, "lq") X(SQ, "sq") X(ILW, "ilw") X(ISW, "isw") X(IADDIU, "iaddiu")      \
  X(ISUBIU, "isubiu") X(FCEQ, "fceq") X(FCSET, "fcset") X(FCAND, "fcand") X(FCOR, "fcor")              \
  X(FSSET, "fsset") X(FSAND, "fsand") X(FSOR, "fsor") X(FMEQ, "fmeq") X(FMAND, "fmand") X(FMOR, "fmor") \
  X(FCGET, "fcget") X(B, "b") X(BAL, "bal") X(JR, "jr") X(JALR, "jalr") X(IBEQ, "ibeq") X(IBNE, "ibne") \
  X(IBLTZ, "ibltz") X(IBGTZ, "ibgtz") X(IBLEZ, "iblez") X(IBGEZ, "ibgez")                              \
  X(IADD, "iadd") X(ISUB, "isub") X(IADDI, "iaddi") X(IAND, "iand") X(IOR, "ior")                      \
  X(MOVE, "move") X(LQI, "lqi") X(DIV, "div") X(MTIR, "mtir") X(RNEXT, "rnext") X(MFP, "mfp")          \
  X(XTOP, "xtop") X(XGKICK, "xgkick") X(ESADD, "esadd") X(EATANxy, "eatanxy") X(ESQRT, "esqrt")       \
  X(ESIN, "esin") X(MR32, "mr32") X(SQI, "sqi") X(SQRT, "sqrt") X(MFIR, "mfir") X(RGET, "rget")        \
  X(XITOP, "xitop") X(ERSADD, "ersadd") X(EATANxz, "eatanxz") X(ERSQRT, "ersqrt") X(LQD, "lqd")       \
  X(RSQRT, "rsqrt") X(ILWR, "ilwr") X(RINIT, "rinit") X(ELENG, "eleng") X(ESUM, "esum")               \
  X(ERCPR, "ercpr") X(EEXP, "eexp") X(SQD, "sqd") X(WAITQ, "waitq") X(ISWR, "iswr") X(RXOR, "rxor")    \
  X(ERLENG, "erleng") X(WAITP, "waitp")

enum class Lower : std::uint8_t {
#define HP2_X(name, text) name,
  HP2_VU_LOWER_OPS(HP2_X)
#undef HP2_X
  Count_
};
inline constexpr std::size_t kLowerCount = static_cast<std::size_t>(Lower::Count_);

// Upper-word flag bits.
inline constexpr std::uint32_t kBitI = 1u << 31;  // the lower word is a 32-bit immediate for I
inline constexpr std::uint32_t kBitE = 1u << 30;  // end of the microprogram (after the next pair)
inline constexpr std::uint32_t kBitM = 1u << 29;
inline constexpr std::uint32_t kBitD = 1u << 28;
inline constexpr std::uint32_t kBitT = 1u << 27;

struct Pair {
  std::uint32_t lower = 0, upper = 0;
  r5900::Op up = r5900::Op::Invalid;  // FMAC operation (VADDbc ... VNOP); Invalid if not an upper op
  Lower lo = Lower::Invalid;          // ignored when the I bit is set
  bool i_bit() const { return (upper & kBitI) != 0; }
  bool e_bit() const { return (upper & kBitE) != 0; }
  bool lower_is_branch() const;
  bool lower_reads_flags() const;  // fceq ... fcget
};

r5900::Op decode_upper(std::uint32_t word);
Lower decode_lower(std::uint32_t word);
Pair decode(std::uint32_t lower, std::uint32_t upper);
std::string_view mnemonic(Lower op);
// Branch target (in bytes of microcode memory) of a branching lower word at byte address `address`.
std::uint32_t branch_target(std::uint32_t address, std::uint32_t lower);
std::string disassemble(std::uint32_t address, std::uint32_t lower, std::uint32_t upper);

// The registers an instruction word reads and writes, as the reference core's
// VU reflection tables give them (CMA_VU::GetAffectedOperands). They drive its
// pipeline model - FMAC stall delays, the integer-branch rule, Q/P syncs - so
// they are reproduced entry by entry, quirks included (for example `addabc`
// reads ft with the destination mask, not the broadcast element, and `subaq`
// has no entry at all). Element masks use the destination-field order:
// bit 3 = x ... bit 0 = w. The launcher checks these against the reference
// core for every word of the game's microcode.
struct Operands {
  std::uint8_t write_f = 0, read_f0 = 0, read_elem_f0 = 0, read_f1 = 0, read_elem_f1 = 0;
  std::uint8_t write_i = 0, write_i_lsu = 0, read_i0 = 0, read_i1 = 0;
  bool sync_q = false, read_q = false, sync_p = false, read_p = false;
  bool read_mac = false, write_mac = false, branch_value = false;
  bool operator==(const Operands&) const = default;
};
inline constexpr std::uint32_t kLowerNop = 0x8000033C;  // the assembler's lower-word nop
Operands upper_operands(std::uint32_t upper);
// Operands of a lower word; none when the pair's I bit makes it an immediate, or for the nop encoding.
Operands lower_operands(std::uint32_t lower, std::uint32_t upper);
// Conditional integer branch (ibeq ... ibgez), judged on the raw word as the reference core does.
inline bool is_conditional_branch(std::uint32_t lower) {
  const std::uint32_t id = (lower >> 25) & 0x7F;
  return id >= 0x28 && id < 0x30;
}

// A microcode upload (VIF "MPG" packet) found in the executable.
struct Upload {
  std::uint32_t file_address = 0;  // where its words start in the executable's memory image
  std::uint32_t vu_address = 0;    // where it is loaded in VU microcode memory (bytes)
  std::uint32_t pairs = 0;
};
// Scans `.vutext` for MPG packets. Throws when the section is missing.
std::vector<Upload> find_uploads(const ElfImage& elf);

// Microcode memory as the uploads leave it when applied in order (later ones
// overwrite earlier ones): one image per contiguous range they cover. This is
// what the game runs (checked against the reference core's memory dumps).
struct MicroImage {
  std::uint32_t vu_address = 0;
  std::vector<std::uint32_t> words;  // lower, upper, lower, upper ...
};
std::vector<MicroImage> microcode_images(const ElfImage& elf, std::uint32_t memory_size = 0x4000);

}  // namespace hp2::vu
