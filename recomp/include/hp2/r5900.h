// Decoder for the PS2 main processor's instruction set (MIPS R5900:
// MIPS III, parts of MIPS IV, the 128-bit multimedia instructions, the
// single-precision FPU and the VU0 macro instructions).
//
// Source of the encodings: the public MIPS III/R5900 instruction tables as
// documented in the reference core's sources (BSD 2-Clause, docs/prior-art.md).
// Checked against this game: every instruction the reference core executed
// in the traced session must decode here (docs/recomp-analysis.md).
#pragma once

#include "hp2/common.h"

namespace hp2::r5900 {

// How an instruction's operands are written (disassembly) and which general
// register, if any, it writes (see dest_gpr()).
enum class Fmt : std::uint8_t {
  None,        // no operands
  Code,        // syscall, break
  RdRsRt,      // addu rd, rs, rt
  RdRtSa,      // sll rd, rt, sa
  RdRtRs,      // sllv rd, rt, rs
  RdRt,        // pabsw rd, rt
  RdRs,        // jalr rd, rs
  Rs,          // jr rs, mthi rs
  Rd,          // mfhi rd
  RsRt,        // div rs, rt
  RtRsSimm,    // addiu rt, rs, simm
  RtRsImm,     // ori rt, rs, imm
  RtImm,       // lui rt, imm
  RsRtBranch,  // beq rs, rt, target
  RsBranch,    // bltz rs, target
  Branch,      // bc1t target
  Jump,        // j target
  RtMem,       // lw rt, simm(rs)       (rt is written by loads only)
  FtMem,       // lwc1 ft, simm(rs)
  VtMem,       // lqc2 vt, simm(rs)
  HintMem,     // cache op, simm(rs)
  RsSimm,      // tgei rs, simm
  RtCop0,      // mfc0 rt, reg
  RtFs,        // mfc1 rt, fs
  FdFsFt,      // add.s fd, fs, ft
  FdFs,        // abs.s fd, fs
  FdFt,        // sqrt.s fd, ft
  FsFt,        // c.eq.s fs, ft
  RtVd,        // qmfc2 rt, vf
  Vu,          // VU0 macro instruction (fields printed raw)
};

// X(Name, "mnemonic", Fmt)
#define HP2_R5900_OPS(X)                                                                           \
  X(Invalid, "(invalid)", None)                                                                    \
  /* SPECIAL */                                                                                    \
  X(SLL, "sll", RdRtSa) X(SRL, "srl", RdRtSa) X(SRA, "sra", RdRtSa)                                \
  X(SLLV, "sllv", RdRtRs) X(SRLV, "srlv", RdRtRs) X(SRAV, "srav", RdRtRs)                          \
  X(JR, "jr", Rs) X(JALR, "jalr", RdRs) X(MOVZ, "movz", RdRsRt) X(MOVN, "movn", RdRsRt)            \
  X(SYSCALL, "syscall", Code) X(BREAK, "break", Code) X(SYNC, "sync", None)                        \
  X(MFHI, "mfhi", Rd) X(MTHI, "mthi", Rs) X(MFLO, "mflo", Rd) X(MTLO, "mtlo", Rs)                  \
  X(DSLLV, "dsllv", RdRtRs) X(DSRLV, "dsrlv", RdRtRs) X(DSRAV, "dsrav", RdRtRs)                    \
  X(MULT, "mult", RdRsRt) X(MULTU, "multu", RdRsRt) X(DIV, "div", RsRt) X(DIVU, "divu", RsRt)      \
  X(ADD, "add", RdRsRt) X(ADDU, "addu", RdRsRt) X(SUB, "sub", RdRsRt) X(SUBU, "subu", RdRsRt)      \
  X(AND, "and", RdRsRt) X(OR, "or", RdRsRt) X(XOR, "xor", RdRsRt) X(NOR, "nor", RdRsRt)            \
  X(MFSA, "mfsa", Rd) X(MTSA, "mtsa", Rs) X(SLT, "slt", RdRsRt) X(SLTU, "sltu", RdRsRt)            \
  X(DADD, "dadd", RdRsRt) X(DADDU, "daddu", RdRsRt) X(DSUB, "dsub", RdRsRt)                        \
  X(DSUBU, "dsubu", RdRsRt)                                                                        \
  X(TGE, "tge", RsRt) X(TGEU, "tgeu", RsRt) X(TLT, "tlt", RsRt) X(TLTU, "tltu", RsRt)              \
  X(TEQ, "teq", RsRt) X(TNE, "tne", RsRt)                                                          \
  X(DSLL, "dsll", RdRtSa) X(DSRL, "dsrl", RdRtSa) X(DSRA, "dsra", RdRtSa)                          \
  X(DSLL32, "dsll32", RdRtSa) X(DSRL32, "dsrl32", RdRtSa) X(DSRA32, "dsra32", RdRtSa)              \
  /* REGIMM */                                                                                     \
  X(BLTZ, "bltz", RsBranch) X(BGEZ, "bgez", RsBranch) X(BLTZL, "bltzl", RsBranch)                  \
  X(BGEZL, "bgezl", RsBranch)                                                                      \
  X(TGEI, "tgei", RsSimm) X(TGEIU, "tgeiu", RsSimm) X(TLTI, "tlti", RsSimm)                        \
  X(TLTIU, "tltiu", RsSimm) X(TEQI, "teqi", RsSimm) X(TNEI, "tnei", RsSimm)                        \
  X(BLTZAL, "bltzal", RsBranch) X(BGEZAL, "bgezal", RsBranch) X(BLTZALL, "bltzall", RsBranch)      \
  X(BGEZALL, "bgezall", RsBranch) X(MTSAB, "mtsab", RsSimm) X(MTSAH, "mtsah", RsSimm)              \
  /* primary */                                                                                    \
  X(J, "j", Jump) X(JAL, "jal", Jump)                                                              \
  X(BEQ, "beq", RsRtBranch) X(BNE, "bne", RsRtBranch) X(BLEZ, "blez", RsBranch)                    \
  X(BGTZ, "bgtz", RsBranch)                                                                        \
  X(ADDI, "addi", RtRsSimm) X(ADDIU, "addiu", RtRsSimm) X(SLTI, "slti", RtRsSimm)                  \
  X(SLTIU, "sltiu", RtRsSimm) X(ANDI, "andi", RtRsImm) X(ORI, "ori", RtRsImm)                      \
  X(XORI, "xori", RtRsImm) X(LUI, "lui", RtImm)                                                    \
  X(BEQL, "beql", RsRtBranch) X(BNEL, "bnel", RsRtBranch) X(BLEZL, "blezl", RsBranch)              \
  X(BGTZL, "bgtzl", RsBranch)                                                                      \
  X(DADDI, "daddi", RtRsSimm) X(DADDIU, "daddiu", RtRsSimm) X(LDL, "ldl", RtMem)                   \
  X(LDR, "ldr", RtMem) X(LQ, "lq", RtMem) X(SQ, "sq", RtMem)                                       \
  X(LB, "lb", RtMem) X(LH, "lh", RtMem) X(LWL, "lwl", RtMem) X(LW, "lw", RtMem)                    \
  X(LBU, "lbu", RtMem) X(LHU, "lhu", RtMem) X(LWR, "lwr", RtMem) X(LWU, "lwu", RtMem)              \
  X(SB, "sb", RtMem) X(SH, "sh", RtMem) X(SWL, "swl", RtMem) X(SW, "sw", RtMem)                    \
  X(SDL, "sdl", RtMem) X(SDR, "sdr", RtMem) X(SWR, "swr", RtMem) X(CACHE, "cache", HintMem)        \
  X(LWC1, "lwc1", FtMem) X(PREF, "pref", HintMem) X(LQC2, "lqc2", VtMem) X(LD, "ld", RtMem)        \
  X(SWC1, "swc1", FtMem) X(SQC2, "sqc2", VtMem) X(SD, "sd", RtMem)                                 \
  /* MMI */                                                                                        \
  X(MADD, "madd", RdRsRt) X(MADDU, "maddu", RdRsRt) X(PLZCW, "plzcw", RdRs)                        \
  X(MFHI1, "mfhi1", Rd) X(MTHI1, "mthi1", Rs) X(MFLO1, "mflo1", Rd) X(MTLO1, "mtlo1", Rs)          \
  X(MULT1, "mult1", RdRsRt) X(MULTU1, "multu1", RdRsRt) X(DIV1, "div1", RsRt)                      \
  X(DIVU1, "divu1", RsRt) X(MADD1, "madd1", RdRsRt) X(MADDU1, "maddu1", RdRsRt)                    \
  X(PMFHL, "pmfhl", Rd) X(PMTHL, "pmthl", Rs)                                                      \
  X(PSLLH, "psllh", RdRtSa) X(PSRLH, "psrlh", RdRtSa) X(PSRAH, "psrah", RdRtSa)                    \
  X(PSLLW, "psllw", RdRtSa) X(PSRLW, "psrlw", RdRtSa) X(PSRAW, "psraw", RdRtSa)                    \
  /* MMI0 */                                                                                       \
  X(PADDW, "paddw", RdRsRt) X(PSUBW, "psubw", RdRsRt) X(PCGTW, "pcgtw", RdRsRt)                    \
  X(PMAXW, "pmaxw", RdRsRt) X(PADDH, "paddh", RdRsRt) X(PSUBH, "psubh", RdRsRt)                    \
  X(PCGTH, "pcgth", RdRsRt) X(PMAXH, "pmaxh", RdRsRt) X(PADDB, "paddb", RdRsRt)                    \
  X(PSUBB, "psubb", RdRsRt) X(PCGTB, "pcgtb", RdRsRt) X(PADDSW, "paddsw", RdRsRt)                  \
  X(PSUBSW, "psubsw", RdRsRt) X(PEXTLW, "pextlw", RdRsRt) X(PPACW, "ppacw", RdRsRt)                \
  X(PADDSH, "paddsh", RdRsRt) X(PSUBSH, "psubsh", RdRsRt) X(PEXTLH, "pextlh", RdRsRt)              \
  X(PPACH, "ppach", RdRsRt) X(PADDSB, "paddsb", RdRsRt) X(PSUBSB, "psubsb", RdRsRt)                \
  X(PEXTLB, "pextlb", RdRsRt) X(PPACB, "ppacb", RdRsRt) X(PEXT5, "pext5", RdRt)                    \
  X(PPAC5, "ppac5", RdRt)                                                                          \
  /* MMI1 */                                                                                       \
  X(PABSW, "pabsw", RdRt) X(PCEQW, "pceqw", RdRsRt) X(PMINW, "pminw", RdRsRt)                      \
  X(PADSBH, "padsbh", RdRsRt) X(PABSH, "pabsh", RdRt) X(PCEQH, "pceqh", RdRsRt)                    \
  X(PMINH, "pminh", RdRsRt) X(PCEQB, "pceqb", RdRsRt) X(PADDUW, "padduw", RdRsRt)                  \
  X(PSUBUW, "psubuw", RdRsRt) X(PEXTUW, "pextuw", RdRsRt) X(PADDUH, "padduh", RdRsRt)              \
  X(PSUBUH, "psubuh", RdRsRt) X(PEXTUH, "pextuh", RdRsRt) X(PADDUB, "paddub", RdRsRt)              \
  X(PSUBUB, "psubub", RdRsRt) X(PEXTUB, "pextub", RdRsRt) X(QFSRV, "qfsrv", RdRsRt)                \
  /* MMI2 */                                                                                       \
  X(PMADDW, "pmaddw", RdRsRt) X(PSLLVW, "psllvw", RdRtRs) X(PSRLVW, "psrlvw", RdRtRs)              \
  X(PMSUBW, "pmsubw", RdRsRt) X(PMFHI, "pmfhi", Rd) X(PMFLO, "pmflo", Rd)                          \
  X(PINTH, "pinth", RdRsRt) X(PMULTW, "pmultw", RdRsRt) X(PDIVW, "pdivw", RsRt)                    \
  X(PCPYLD, "pcpyld", RdRsRt) X(PMADDH, "pmaddh", RdRsRt) X(PHMADH, "phmadh", RdRsRt)              \
  X(PAND, "pand", RdRsRt) X(PXOR, "pxor", RdRsRt) X(PMSUBH, "pmsubh", RdRsRt)                      \
  X(PHMSBH, "phmsbh", RdRsRt) X(PEXEH, "pexeh", RdRt) X(PREVH, "prevh", RdRt)                      \
  X(PMULTH, "pmulth", RdRsRt) X(PDIVBW, "pdivbw", RsRt) X(PEXEW, "pexew", RdRt)                    \
  X(PROT3W, "prot3w", RdRt)                                                                        \
  /* MMI3 */                                                                                       \
  X(PMADDUW, "pmadduw", RdRsRt) X(PSRAVW, "psravw", RdRtRs) X(PMTHI, "pmthi", Rs)                  \
  X(PMTLO, "pmtlo", Rs) X(PINTEH, "pinteh", RdRsRt) X(PMULTUW, "pmultuw", RdRsRt)                  \
  X(PDIVUW, "pdivuw", RsRt) X(PCPYUD, "pcpyud", RdRsRt) X(POR, "por", RdRsRt)                      \
  X(PNOR, "pnor", RdRsRt) X(PEXCH, "pexch", RdRt) X(PCPYH, "pcpyh", RdRt)                          \
  X(PEXCW, "pexcw", RdRt)                                                                          \
  /* COP0 */                                                                                       \
  X(MFC0, "mfc0", RtCop0) X(MTC0, "mtc0", RtCop0)                                                  \
  X(BC0F, "bc0f", Branch) X(BC0T, "bc0t", Branch) X(BC0FL, "bc0fl", Branch)                        \
  X(BC0TL, "bc0tl", Branch)                                                                        \
  X(TLBR, "tlbr", None) X(TLBWI, "tlbwi", None) X(TLBWR, "tlbwr", None) X(TLBP, "tlbp", None)      \
  X(ERET, "eret", None) X(EI, "ei", None) X(DI, "di", None)                                        \
  /* COP1 */                                                                                       \
  X(MFC1, "mfc1", RtFs) X(CFC1, "cfc1", RtFs) X(MTC1, "mtc1", RtFs) X(CTC1, "ctc1", RtFs)          \
  X(BC1F, "bc1f", Branch) X(BC1T, "bc1t", Branch) X(BC1FL, "bc1fl", Branch)                        \
  X(BC1TL, "bc1tl", Branch)                                                                        \
  X(ADD_S, "add.s", FdFsFt) X(SUB_S, "sub.s", FdFsFt) X(MUL_S, "mul.s", FdFsFt)                    \
  X(DIV_S, "div.s", FdFsFt) X(SQRT_S, "sqrt.s", FdFt) X(ABS_S, "abs.s", FdFs)                      \
  X(MOV_S, "mov.s", FdFs) X(NEG_S, "neg.s", FdFs) X(RSQRT_S, "rsqrt.s", FdFsFt)                    \
  X(ADDA_S, "adda.s", FsFt) X(SUBA_S, "suba.s", FsFt) X(MULA_S, "mula.s", FsFt)                    \
  X(MADD_S, "madd.s", FdFsFt) X(MSUB_S, "msub.s", FdFsFt) X(MADDA_S, "madda.s", FsFt)              \
  X(MSUBA_S, "msuba.s", FsFt) X(CVT_W_S, "cvt.w.s", FdFs) X(MAX_S, "max.s", FdFsFt)                \
  X(MIN_S, "min.s", FdFsFt) X(C_F_S, "c.f.s", FsFt) X(C_EQ_S, "c.eq.s", FsFt)                      \
  X(C_LT_S, "c.lt.s", FsFt) X(C_LE_S, "c.le.s", FsFt) X(CVT_S_W, "cvt.s.w", FdFs)                  \
  /* COP2 (VU0 macro mode) */                                                                      \
  X(QMFC2, "qmfc2", RtVd) X(CFC2, "cfc2", RtVd) X(QMTC2, "qmtc2", RtVd) X(CTC2, "ctc2", RtVd)      \
  X(BC2F, "bc2f", Branch) X(BC2T, "bc2t", Branch) X(BC2FL, "bc2fl", Branch)                        \
  X(BC2TL, "bc2tl", Branch)                                                                        \
  X(VADDbc, "vaddbc", Vu) X(VSUBbc, "vsubbc", Vu) X(VMADDbc, "vmaddbc", Vu)                        \
  X(VMSUBbc, "vmsubbc", Vu) X(VMAXbc, "vmaxbc", Vu) X(VMINIbc, "vminibc", Vu)                      \
  X(VMULbc, "vmulbc", Vu) X(VMULq, "vmulq", Vu) X(VMAXi, "vmaxi", Vu) X(VMULi, "vmuli", Vu)        \
  X(VMINIi, "vminii", Vu) X(VADDq, "vaddq", Vu) X(VMADDq, "vmaddq", Vu) X(VADDi, "vaddi", Vu)      \
  X(VMADDi, "vmaddi", Vu) X(VSUBq, "vsubq", Vu) X(VMSUBq, "vmsubq", Vu) X(VSUBi, "vsubi", Vu)      \
  X(VMSUBi, "vmsubi", Vu) X(VADD, "vadd", Vu) X(VMADD, "vmadd", Vu) X(VMUL, "vmul", Vu)            \
  X(VMAX, "vmax", Vu) X(VSUB, "vsub", Vu) X(VMSUB, "vmsub", Vu) X(VOPMSUB, "vopmsub", Vu)          \
  X(VMINI, "vmini", Vu) X(VIADD, "viadd", Vu) X(VISUB, "visub", Vu) X(VIADDI, "viaddi", Vu)        \
  X(VIAND, "viand", Vu) X(VIOR, "vior", Vu) X(VCALLMS, "vcallms", Vu)                              \
  X(VCALLMSR, "vcallmsr", Vu)                                                                      \
  X(VADDAbc, "vaddabc", Vu) X(VSUBAbc, "vsubabc", Vu) X(VMADDAbc, "vmaddabc", Vu)                  \
  X(VMSUBAbc, "vmsubabc", Vu) X(VITOF0, "vitof0", Vu) X(VITOF4, "vitof4", Vu)                      \
  X(VITOF12, "vitof12", Vu) X(VITOF15, "vitof15", Vu) X(VFTOI0, "vftoi0", Vu)                      \
  X(VFTOI4, "vftoi4", Vu) X(VFTOI12, "vftoi12", Vu) X(VFTOI15, "vftoi15", Vu)                      \
  X(VMULAbc, "vmulabc", Vu) X(VMULAq, "vmulaq", Vu) X(VABS, "vabs", Vu) X(VMULAi, "vmulai", Vu)    \
  X(VCLIP, "vclip", Vu) X(VADDAq, "vaddaq", Vu) X(VMADDAq, "vmaddaq", Vu)                          \
  X(VADDAi, "vaddai", Vu) X(VMADDAi, "vmaddai", Vu) X(VSUBAq, "vsubaq", Vu)                        \
  X(VMSUBAq, "vmsubaq", Vu) X(VSUBAi, "vsubai", Vu) X(VMSUBAi, "vmsubai", Vu)                      \
  X(VADDA, "vadda", Vu) X(VMADDA, "vmadda", Vu) X(VMULA, "vmula", Vu) X(VSUBA, "vsuba", Vu)        \
  X(VMSUBA, "vmsuba", Vu) X(VOPMULA, "vopmula", Vu) X(VNOP, "vnop", Vu) X(VMOVE, "vmove", Vu)      \
  X(VMR32, "vmr32", Vu) X(VLQI, "vlqi", Vu) X(VSQI, "vsqi", Vu) X(VLQD, "vlqd", Vu)                \
  X(VSQD, "vsqd", Vu) X(VDIV, "vdiv", Vu) X(VSQRT, "vsqrt", Vu) X(VRSQRT, "vrsqrt", Vu)            \
  X(VWAITQ, "vwaitq", Vu) X(VMTIR, "vmtir", Vu) X(VMFIR, "vmfir", Vu) X(VILWR, "vilwr", Vu)        \
  X(VISWR, "viswr", Vu) X(VRNEXT, "vrnext", Vu) X(VRGET, "vrget", Vu) X(VRINIT, "vrinit", Vu)      \
  X(VRXOR, "vrxor", Vu)

enum class Op : std::uint16_t {
#define HP2_X(name, text, fmt) name,
  HP2_R5900_OPS(HP2_X)
#undef HP2_X
  Count_
};
inline constexpr std::size_t kOpCount = static_cast<std::size_t>(Op::Count_);

// What an instruction does to the flow of control.
enum class Flow : std::uint8_t {
  None,
  Branch,      // conditional, PC-relative; the next instruction (delay slot) runs first
  BranchLink,  // bltzal/bgezal: conditional call
  Jump,        // j
  Call,        // jal
  JumpReg,     // jr
  CallReg,     // jalr
  Syscall,     // syscall: the kernel takes over, execution resumes after it
  Break,       // break
  Eret,        // return from exception
};

struct Insn {
  std::uint32_t raw = 0;
  Op op = Op::Invalid;
  Flow flow = Flow::None;
  bool likely = false;  // "likely" branch: the delay slot runs only when the branch is taken

  constexpr unsigned rs() const { return (raw >> 21) & 31; }
  constexpr unsigned rt() const { return (raw >> 16) & 31; }
  constexpr unsigned rd() const { return (raw >> 11) & 31; }
  constexpr unsigned sa() const { return (raw >> 6) & 31; }
  constexpr std::int32_t simm() const { return static_cast<std::int16_t>(raw & 0xFFFF); }
  constexpr std::uint32_t imm() const { return raw & 0xFFFF; }

  constexpr bool valid() const { return op != Op::Invalid; }
  constexpr bool is_nop() const { return raw == 0; }
  // Followed by a delay slot (every branch and jump on this processor).
  constexpr bool has_delay_slot() const {
    return flow == Flow::Branch || flow == Flow::BranchLink || flow == Flow::Jump ||
           flow == Flow::Call || flow == Flow::JumpReg || flow == Flow::CallReg;
  }
  // Control never continues past the delay slot ("j", "jr", "b").
  bool unconditional() const;
};

Insn decode(std::uint32_t word);

// Destination of a Branch, BranchLink, Jump or Call at address pc.
std::uint32_t target_of(std::uint32_t pc, const Insn& insn);

// The general register the instruction writes, or -1. Register 0 is never
// reported (writes to it are discarded by the processor).
int dest_gpr(const Insn& insn);

std::string_view mnemonic(Op op);
std::string_view gpr_name(unsigned reg);
std::string disassemble(std::uint32_t pc, std::uint32_t word);

}  // namespace hp2::r5900
