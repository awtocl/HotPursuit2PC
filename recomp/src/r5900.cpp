#include "hp2/r5900.h"

#include <array>

namespace hp2::r5900 {

namespace {

using enum Op;
constexpr Op xx = Op::Invalid;

struct OpInfo {
  std::string_view text;
  Fmt fmt;
};
constexpr std::array<OpInfo, kOpCount> kInfo = {{
#define HP2_X(name, text, fmt) {text, Fmt::fmt},
    HP2_R5900_OPS(HP2_X)
#undef HP2_X
}};

// Indexed by bits 31..26. xx here also stands for the six classes decoded below.
constexpr std::array<Op, 64> kPrimary = {
    xx,   xx,    J,    JAL,   BEQ,  BNE,  BLEZ,  BGTZ,   // 00
    ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI,  XORI,  LUI,    // 08
    xx,   xx,    xx,   xx,    BEQL, BNEL, BLEZL, BGTZL,  // 10
    DADDI, DADDIU, LDL, LDR,  xx,   xx,   LQ,    SQ,     // 18
    LB,   LH,    LWL,  LW,    LBU,  LHU,  LWR,   LWU,    // 20
    SB,   SH,    SWL,  SW,    SDL,  SDR,  SWR,   CACHE,  // 28
    xx,   LWC1,  xx,   PREF,  xx,   xx,   LQC2,  LD,     // 30
    xx,   SWC1,  xx,   xx,    xx,   xx,   SQC2,  SD,     // 38
};

// SPECIAL, indexed by bits 5..0.
constexpr std::array<Op, 64> kSpecial = {
    SLL,  xx,    SRL,  SRA,  SLLV,    xx,    SRLV,   SRAV,    // 00
    JR,   JALR,  MOVZ, MOVN, SYSCALL, BREAK, xx,     SYNC,    // 08
    MFHI, MTHI,  MFLO, MTLO, DSLLV,   xx,    DSRLV,  DSRAV,   // 10
    MULT, MULTU, DIV,  DIVU, xx,      xx,    xx,     xx,      // 18
    ADD,  ADDU,  SUB,  SUBU, AND,     OR,    XOR,    NOR,     // 20
    MFSA, MTSA,  SLT,  SLTU, DADD,    DADDU, DSUB,   DSUBU,   // 28
    TGE,  TGEU,  TLT,  TLTU, TEQ,     xx,    TNE,    xx,      // 30
    DSLL, xx,    DSRL, DSRA, DSLL32,  xx,    DSRL32, DSRA32,  // 38
};

// REGIMM, indexed by bits 20..16.
constexpr std::array<Op, 32> kRegimm = {
    BLTZ,   BGEZ,   BLTZL,   BGEZL,   xx,   xx, xx,   xx,  // 00
    TGEI,   TGEIU,  TLTI,    TLTIU,   TEQI, xx, TNEI, xx,  // 08
    BLTZAL, BGEZAL, BLTZALL, BGEZALL, xx,   xx, xx,   xx,  // 10
    MTSAB,  MTSAH,  xx,      xx,      xx,   xx, xx,   xx,  // 18
};

// MMI, indexed by bits 5..0. MMI0..MMI3 are decoded from bits 10..6.
constexpr std::array<Op, 64> kMmi = {
    MADD,  MADDU,  xx,    xx,     PLZCW, xx, xx,    xx,     // 00
    xx,    xx,     xx,    xx,     xx,    xx, xx,    xx,     // 08 (MMI0 at 08, MMI2 at 09)
    MFHI1, MTHI1,  MFLO1, MTLO1,  xx,    xx, xx,    xx,     // 10
    MULT1, MULTU1, DIV1,  DIVU1,  xx,    xx, xx,    xx,     // 18
    MADD1, MADDU1, xx,    xx,     xx,    xx, xx,    xx,     // 20
    xx,    xx,     xx,    xx,     xx,    xx, xx,    xx,     // 28 (MMI1 at 28, MMI3 at 29)
    PMFHL, PMTHL,  xx,    xx,     PSLLH, xx, PSRLH, PSRAH,  // 30
    xx,    xx,     xx,    xx,     PSLLW, xx, PSRLW, PSRAW,  // 38
};
constexpr std::array<Op, 32> kMmi0 = {
    PADDW,  PSUBW,  PCGTW,  PMAXW, PADDH,  PSUBH,  PCGTH,  PMAXH,  // 00
    PADDB,  PSUBB,  PCGTB,  xx,    xx,     xx,     xx,     xx,     // 08
    PADDSW, PSUBSW, PEXTLW, PPACW, PADDSH, PSUBSH, PEXTLH, PPACH,  // 10
    PADDSB, PSUBSB, PEXTLB, PPACB, xx,     xx,     PEXT5,  PPAC5,  // 18
};
constexpr std::array<Op, 32> kMmi1 = {
    xx,     PABSW,  PCEQW,  PMINW, PADSBH, PABSH,  PCEQH,  PMINH,  // 00
    xx,     xx,     PCEQB,  xx,    xx,     xx,     xx,     xx,     // 08
    PADDUW, PSUBUW, PEXTUW, xx,    PADDUH, PSUBUH, PEXTUH, xx,     // 10
    PADDUB, PSUBUB, PEXTUB, QFSRV, xx,     xx,     xx,     xx,     // 18
};
constexpr std::array<Op, 32> kMmi2 = {
    PMADDW, xx,     PSLLVW, PSRLVW, PMSUBW, xx,     xx,     xx,      // 00
    PMFHI,  PMFLO,  PINTH,  xx,     PMULTW, PDIVW,  PCPYLD, xx,      // 08
    PMADDH, PHMADH, PAND,   PXOR,   PMSUBH, PHMSBH, xx,     xx,      // 10
    xx,     xx,     PEXEH,  PREVH,  PMULTH, PDIVBW, PEXEW,  PROT3W,  // 18
};
constexpr std::array<Op, 32> kMmi3 = {
    PMADDUW, xx,    xx,     PSRAVW, xx,      xx,     xx,     xx,  // 00
    PMTHI,   PMTLO, PINTEH, xx,     PMULTUW, PDIVUW, PCPYUD, xx,  // 08
    xx,      xx,    POR,    PNOR,   xx,      xx,     xx,     xx,  // 10
    xx,      xx,    PEXCH,  PCPYH,  xx,      xx,     PEXCW,  xx,  // 18
};

// COP1, format S, indexed by bits 5..0.
constexpr std::array<Op, 64> kCop1S = {
    ADD_S,   SUB_S,  MUL_S,  DIV_S, SQRT_S, ABS_S,  MOV_S,   NEG_S,    // 00
    xx,      xx,     xx,     xx,    xx,     xx,     xx,      xx,       // 08
    xx,      xx,     xx,     xx,    xx,     xx,     RSQRT_S, xx,       // 10
    ADDA_S,  SUBA_S, MULA_S, xx,    MADD_S, MSUB_S, MADDA_S, MSUBA_S,  // 18
    xx,      xx,     xx,     xx,    CVT_W_S, xx,    xx,      xx,       // 20
    MAX_S,   MIN_S,  xx,     xx,    xx,     xx,     xx,      xx,       // 28
    C_F_S,   xx,     C_EQ_S, xx,    C_LT_S, xx,     C_LE_S,  xx,       // 30
    xx,      xx,     xx,     xx,    xx,     xx,     xx,      xx,       // 38
};

// COP2 with bit 25 set, indexed by bits 5..0; 3C..3F select the second table.
constexpr std::array<Op, 64> kVuSpecial1 = {
    VADDbc,  VADDbc,   VADDbc,  VADDbc,  VSUBbc,  VSUBbc,  VSUBbc,  VSUBbc,   // 00
    VMADDbc, VMADDbc,  VMADDbc, VMADDbc, VMSUBbc, VMSUBbc, VMSUBbc, VMSUBbc,  // 08
    VMAXbc,  VMAXbc,   VMAXbc,  VMAXbc,  VMINIbc, VMINIbc, VMINIbc, VMINIbc,  // 10
    VMULbc,  VMULbc,   VMULbc,  VMULbc,  VMULq,   VMAXi,   VMULi,   VMINIi,   // 18
    VADDq,   VMADDq,   VADDi,   VMADDi,  VSUBq,   VMSUBq,  VSUBi,   VMSUBi,   // 20
    VADD,    VMADD,    VMUL,    VMAX,    VSUB,    VMSUB,   VOPMSUB, VMINI,    // 28
    VIADD,   VISUB,    VIADDI,  xx,      VIAND,   VIOR,    xx,      xx,       // 30
    VCALLMS, VCALLMSR, xx,      xx,      xx,      xx,      xx,      xx,       // 38
};
// Indexed by (bits 10..6 << 2) | bits 1..0.
constexpr std::array<Op, 128> kVuSpecial2 = {
    VADDAbc,  VADDAbc,  VADDAbc,  VADDAbc,  VSUBAbc,  VSUBAbc,  VSUBAbc,  VSUBAbc,   // 00
    VMADDAbc, VMADDAbc, VMADDAbc, VMADDAbc, VMSUBAbc, VMSUBAbc, VMSUBAbc, VMSUBAbc,  // 08
    VITOF0,   VITOF4,   VITOF12,  VITOF15,  VFTOI0,   VFTOI4,   VFTOI12,  VFTOI15,   // 10
    VMULAbc,  VMULAbc,  VMULAbc,  VMULAbc,  VMULAq,   VABS,     VMULAi,   VCLIP,     // 18
    VADDAq,   VMADDAq,  VADDAi,   VMADDAi,  VSUBAq,   VMSUBAq,  VSUBAi,   VMSUBAi,   // 20
    VADDA,    VMADDA,   VMULA,    xx,       VSUBA,    VMSUBA,   VOPMULA,  VNOP,      // 28
    VMOVE,    VMR32,    xx,       xx,       VLQI,     VSQI,     VLQD,     VSQD,      // 30
    VDIV,     VSQRT,    VRSQRT,   VWAITQ,   VMTIR,    VMFIR,    VILWR,    VISWR,     // 38
    VRNEXT,   VRGET,    VRINIT,   VRXOR,    xx,       xx,       xx,       xx,        // 40
    // 48..7F: xx (value-initialised)
};

Op decode_op(std::uint32_t w) {
  const unsigned opc = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, sa = (w >> 6) & 31,
                 fn = w & 63;
  switch (opc) {
    case 0x00:
      return kSpecial[fn];
    case 0x01:
      return kRegimm[rt];
    case 0x10:  // COP0
      switch (rs) {
        case 0x00: return MFC0;
        case 0x04: return MTC0;
        case 0x08: {
          constexpr Op bc[4] = {BC0F, BC0T, BC0FL, BC0TL};
          return rt < 4 ? bc[rt] : xx;
        }
        case 0x10:
          switch (fn) {
            case 0x01: return TLBR;
            case 0x02: return TLBWI;
            case 0x06: return TLBWR;
            case 0x08: return TLBP;
            case 0x18: return ERET;
            case 0x38: return EI;
            case 0x39: return DI;
            default: return xx;
          }
        default: return xx;
      }
    case 0x11:  // COP1
      switch (rs) {
        case 0x00: return MFC1;
        case 0x02: return CFC1;
        case 0x04: return MTC1;
        case 0x06: return CTC1;
        case 0x08: {
          constexpr Op bc[4] = {BC1F, BC1T, BC1FL, BC1TL};
          return rt < 4 ? bc[rt] : xx;
        }
        case 0x10: return kCop1S[fn];
        case 0x14: return fn == 0x20 ? CVT_S_W : xx;
        default: return xx;
      }
    case 0x12:  // COP2
      if (rs & 0x10) {
        if (fn >= 0x3C) return kVuSpecial2[(sa << 2) | (fn & 3)];
        return kVuSpecial1[fn];
      }
      switch (rs) {
        case 0x01: return QMFC2;
        case 0x02: return CFC2;
        case 0x05: return QMTC2;
        case 0x06: return CTC2;
        case 0x08: {
          constexpr Op bc[4] = {BC2F, BC2T, BC2FL, BC2TL};
          return rt < 4 ? bc[rt] : xx;
        }
        default: return xx;
      }
    case 0x1C:  // MMI
      switch (fn) {
        case 0x08: return kMmi0[sa];
        case 0x09: return kMmi2[sa];
        case 0x28: return kMmi1[sa];
        case 0x29: return kMmi3[sa];
        default: return kMmi[fn];
      }
    default:
      return kPrimary[opc];
  }
}

}  // namespace

bool Insn::unconditional() const {
  if (flow == Flow::Jump || flow == Flow::JumpReg) return true;
  if (flow != Flow::Branch) return false;
  if (op == Op::BEQ && rs() == rt()) return true;                        // "b"
  if ((op == Op::BGEZ || op == Op::BLEZ) && rs() == 0) return true;      // zero >= 0, zero <= 0
  return false;
}

Insn decode(std::uint32_t word) {
  Insn i;
  i.raw = word;
  i.op = decode_op(word);
  switch (i.op) {
    case BEQL: case BNEL: case BLEZL: case BGTZL: case BLTZL: case BGEZL:
    case BC0FL: case BC0TL: case BC1FL: case BC1TL: case BC2FL: case BC2TL:
      i.likely = true;
      [[fallthrough]];
    case BEQ: case BNE: case BLEZ: case BGTZ: case BLTZ: case BGEZ:
    case BC0F: case BC0T: case BC1F: case BC1T: case BC2F: case BC2T:
      i.flow = Flow::Branch;
      break;
    case BLTZALL: case BGEZALL:
      i.likely = true;
      [[fallthrough]];
    case BLTZAL: case BGEZAL:
      i.flow = Flow::BranchLink;
      break;
    case J: i.flow = Flow::Jump; break;
    case JAL: i.flow = Flow::Call; break;
    case JR: i.flow = Flow::JumpReg; break;
    case JALR: i.flow = Flow::CallReg; break;
    case SYSCALL: i.flow = Flow::Syscall; break;
    case BREAK: i.flow = Flow::Break; break;
    case ERET: i.flow = Flow::Eret; break;
    default: break;
  }
  return i;
}

std::uint32_t target_of(std::uint32_t pc, const Insn& insn) {
  if (insn.flow == Flow::Jump || insn.flow == Flow::Call)
    return ((pc + 4) & 0xF0000000u) | ((insn.raw & 0x03FFFFFFu) << 2);
  return pc + 4 + (static_cast<std::uint32_t>(insn.simm()) << 2);
}

int dest_gpr(const Insn& insn) {
  int reg = -1;
  switch (insn.op) {
    case JAL: case BLTZAL: case BGEZAL: case BLTZALL: case BGEZALL:
      return 31;
    case SB: case SH: case SW: case SD: case SQ: case SWL: case SWR: case SDL: case SDR:
    case MTC0: case MTC1: case CTC1: case QMTC2: case CTC2:
      return -1;  // rt is a source here
    case DIV: case DIVU: case DIV1: case DIVU1: case PDIVW: case PDIVUW: case PDIVBW:
      return -1;  // results go to HI/LO only
    default:
      break;
  }
  switch (kInfo[static_cast<std::size_t>(insn.op)].fmt) {
    case Fmt::RdRsRt: case Fmt::RdRtSa: case Fmt::RdRtRs: case Fmt::RdRt: case Fmt::RdRs:
    case Fmt::Rd:
      reg = static_cast<int>(insn.rd());
      break;
    case Fmt::RtRsSimm: case Fmt::RtRsImm: case Fmt::RtImm: case Fmt::RtMem: case Fmt::RtCop0:
    case Fmt::RtFs: case Fmt::RtVd:
      reg = static_cast<int>(insn.rt());
      break;
    default:
      break;
  }
  return reg <= 0 ? -1 : reg;
}

std::string_view mnemonic(Op op) {
  return kInfo[static_cast<std::size_t>(op)].text;
}

std::string_view gpr_name(unsigned reg) {
  constexpr std::array<std::string_view, 32> names = {
      "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
      "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
  return names[reg & 31];
}

std::string disassemble(std::uint32_t pc, std::uint32_t word) {
  const Insn i = decode(word);
  if (!i.valid()) return std::format(".word 0x{:08X}", word);
  if (i.is_nop()) return "nop";
  const std::string_view m = mnemonic(i.op);
  const auto rs = gpr_name(i.rs()), rt = gpr_name(i.rt()), rd = gpr_name(i.rd());
  const unsigned fs = i.rd(), ft = i.rt(), fd = i.sa();  // FPU register fields (this hides hp2::fs here)
  switch (kInfo[static_cast<std::size_t>(i.op)].fmt) {
    case Fmt::None: return std::string(m);
    case Fmt::Code: return std::format("{} 0x{:X}", m, (word >> 6) & 0xFFFFF);
    case Fmt::RdRsRt: return std::format("{} {}, {}, {}", m, rd, rs, rt);
    case Fmt::RdRtSa: return std::format("{} {}, {}, {}", m, rd, rt, i.sa());
    case Fmt::RdRtRs: return std::format("{} {}, {}, {}", m, rd, rt, rs);
    case Fmt::RdRt: return std::format("{} {}, {}", m, rd, rt);
    case Fmt::RdRs: return std::format("{} {}, {}", m, rd, rs);
    case Fmt::Rs: return std::format("{} {}", m, rs);
    case Fmt::Rd: return std::format("{} {}", m, rd);
    case Fmt::RsRt: return std::format("{} {}, {}", m, rs, rt);
    case Fmt::RtRsSimm: return std::format("{} {}, {}, {}", m, rt, rs, i.simm());
    case Fmt::RtRsImm: return std::format("{} {}, {}, 0x{:X}", m, rt, rs, i.imm());
    case Fmt::RtImm: return std::format("{} {}, 0x{:X}", m, rt, i.imm());
    case Fmt::RsRtBranch: return std::format("{} {}, {}, 0x{:08X}", m, rs, rt, target_of(pc, i));
    case Fmt::RsBranch: return std::format("{} {}, 0x{:08X}", m, rs, target_of(pc, i));
    case Fmt::Branch: return std::format("{} 0x{:08X}", m, target_of(pc, i));
    case Fmt::Jump: return std::format("{} 0x{:08X}", m, target_of(pc, i));
    case Fmt::RtMem: return std::format("{} {}, {}({})", m, rt, i.simm(), rs);
    case Fmt::FtMem: return std::format("{} f{}, {}({})", m, ft, i.simm(), rs);
    case Fmt::VtMem: return std::format("{} vf{}, {}({})", m, ft, i.simm(), rs);
    case Fmt::HintMem: return std::format("{} 0x{:X}, {}({})", m, i.rt(), i.simm(), rs);
    case Fmt::RsSimm: return std::format("{} {}, {}", m, rs, i.simm());
    case Fmt::RtCop0: return std::format("{} {}, c0r{}", m, rt, i.rd());
    case Fmt::RtFs: return std::format("{} {}, f{}", m, rt, fs);
    case Fmt::FdFsFt: return std::format("{} f{}, f{}, f{}", m, fd, fs, ft);
    case Fmt::FdFs: return std::format("{} f{}, f{}", m, fd, fs);
    case Fmt::FdFt: return std::format("{} f{}, f{}", m, fd, ft);
    case Fmt::FsFt: return std::format("{} f{}, f{}", m, fs, ft);
    case Fmt::RtVd: return std::format("{} {}, vr{}", m, rt, i.rd());
    case Fmt::Vu:
      return std::format("{} dest={:X} ft={} fs={} fd={} bc={}", m, (word >> 21) & 15, i.rt(), i.rd(),
                         i.sa(), word & 3);
  }
  return std::string(m);
}

}  // namespace hp2::r5900
