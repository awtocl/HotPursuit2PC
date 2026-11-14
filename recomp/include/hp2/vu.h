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

// A microcode upload (VIF "MPG" packet) found in the executable.
struct Upload {
  std::uint32_t file_address = 0;  // where its words start in the executable's memory image
  std::uint32_t vu_address = 0;    // where it is loaded in VU microcode memory (bytes)
  std::uint32_t pairs = 0;
};
// Scans `.vutext` for MPG packets. Throws when the section is missing.
std::vector<Upload> find_uploads(const ElfImage& elf);

}  // namespace hp2::vu
