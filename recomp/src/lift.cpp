// Translator spike: see hp2/lift.h.
//
// The semantics follow the reference core instruction by instruction (it is
// the oracle the result is compared with), in particular:
//  - 32-bit operations sign-extend their result to 64 bits; operations other
//    than the 128-bit ones leave the upper 64 bits of a register unchanged;
//  - mult/multu also write rd; div/divu by zero and INT_MIN / -1 give the
//    values the reference core gives;
//  - FPU operands are "clamped" the way the reference core does it (infinity
//    and NaN become the largest finite value), division by zero gives the
//    largest finite value with the sign of the quotient, cvt.w.s gives
//    0x80000000 when out of range. Rounding and denormal handling come from
//    the thread's floating-point mode, which the reference core sets on its
//    thread to round-toward-zero with denormals flushed - the translated code
//    runs on that same thread.
#include "hp2/lift.h"

#include <map>
#include <set>

#pragma warning(push, 0)
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#pragma warning(pop)

namespace hp2 {

using r5900::Flow;
using r5900::Insn;
using r5900::Op;

namespace {

void check(llvm::Error error, const char* what) {
  if (error) fail("LLVM: {}: {}", what, llvm::toString(std::move(error)));
}
template <class T>
T check(llvm::Expected<T> value, const char* what) {
  if (!value) fail("LLVM: {}: {}", what, llvm::toString(value.takeError()));
  return std::move(*value);
}

struct Unsupported {
  std::string why;
};
[[noreturn]] void unsupported(std::string why) {
  throw Unsupported{std::move(why)};
}

// Lifts one function.
class FunctionLifter {
 public:
  FunctionLifter(const Discovery& d, const StateLayout& layout, std::uint32_t ram_size, llvm::Module& module,
                 const Function& f)
      : d_(d), L_(layout), ram_size_(ram_size), m_(module), ctx_(module.getContext()), b_(ctx_), f_(f) {}

  // Throws Unsupported before anything is added to the module.
  void precheck() const {
    for (std::uint32_t pc = f_.start; pc < f_.end; pc += 4) {
      if (!(d_.flags_at(pc) & kReached)) continue;
      const Insn& in = d_.insns[d_.index_of(pc)];
      if (in.flow == Flow::Call || in.flow == Flow::CallReg || in.flow == Flow::BranchLink)
        unsupported(std::format("call at {:08X} (the spike lifts leaf functions only)", pc));
      if (!supported(in)) unsupported(std::format("instruction at {:08X}: {}", pc, r5900::disassemble(pc, in.raw)));
      switch (in.flow) {
        case Flow::None:
          break;
        case Flow::Branch:
        case Flow::Jump: {
          const std::uint32_t t = r5900::target_of(pc, in);
          if (t < f_.start || t >= f_.end) unsupported(std::format("branch at {:08X} leaves the function", pc));
          break;
        }
        case Flow::JumpReg:
          if (in.rs() != 31) unsupported(std::format("indirect jump at {:08X}", pc));
          break;
        case Flow::Call:
        case Flow::CallReg:
        case Flow::BranchLink:
          unsupported(std::format("call at {:08X} (the spike lifts leaf functions only)", pc));
        default:
          unsupported(std::format("{} at {:08X}", r5900::mnemonic(in.op), pc));
      }
      if (in.has_delay_slot()) {
        const Insn& slot = d_.insns[d_.index_of(pc + 4)];
        if (slot.flow != Flow::None) unsupported(std::format("branch in the delay slot at {:08X}", pc + 4));
        if (pc + 4 >= f_.end) unsupported("delay slot outside the function");
      }
    }
  }

  void lift(const std::string& name) {
    auto* ptr = b_.getPtrTy();
    fn_ = llvm::Function::Create(llvm::FunctionType::get(b_.getVoidTy(), {ptr, ptr}, false),
                                 llvm::Function::ExternalLinkage, name, m_);
    fn_->addParamAttr(0, llvm::Attribute::NoAlias);
    fn_->addParamAttr(1, llvm::Attribute::NoAlias);
    fn_->addFnAttr(llvm::Attribute::NoUnwind);
    state_ = fn_->getArg(0);
    ram_ = fn_->getArg(1);

    entry_ = llvm::BasicBlock::Create(ctx_, "entry", fn_);
    b_.SetInsertPoint(entry_);
    i64_ = b_.getInt64Ty();
    i32_ = b_.getInt32Ty();
    f32_ = b_.getFloatTy();
    for (int r = 0; r < 32; ++r) {
      gpr_[r] = b_.CreateAlloca(i64_, nullptr, std::format("r{}", r));
      gprh_[r] = b_.CreateAlloca(i64_, nullptr, std::format("r{}h", r));
      fpr_[r] = b_.CreateAlloca(i32_, nullptr, std::format("f{}", r));
    }
    hi_ = b_.CreateAlloca(i64_, nullptr, "hi");
    lo_ = b_.CreateAlloca(i64_, nullptr, "lo");
    fcsr_ = b_.CreateAlloca(i32_, nullptr, "fcsr");
    acc_ = b_.CreateAlloca(i32_, nullptr, "acc");
    // Load everything; values that are never used are removed by the optimiser.
    for (int r = 1; r < 32; ++r) {
      b_.CreateStore(b_.CreateLoad(i64_, field(L_.gpr + r * L_.gpr_stride)), gpr_[r]);
      b_.CreateStore(b_.CreateLoad(i64_, field(L_.gpr + r * L_.gpr_stride + 8)), gprh_[r]);
    }
    for (int r = 0; r < 32; ++r) b_.CreateStore(b_.CreateLoad(i32_, field(L_.fpr + r * 4)), fpr_[r]);
    b_.CreateStore(b_.CreateLoad(i64_, field(L_.hi)), hi_);
    b_.CreateStore(b_.CreateLoad(i64_, field(L_.lo)), lo_);
    b_.CreateStore(b_.CreateLoad(i32_, field(L_.fcsr)), fcsr_);
    b_.CreateStore(b_.CreateLoad(i32_, field(L_.fpacc)), acc_);

    trap_ = llvm::BasicBlock::Create(ctx_, "trap", fn_);
    b_.SetInsertPoint(trap_);
    b_.CreateStore(b_.getInt32(kTrapPc), field(L_.pc));
    b_.CreateRetVoid();

    b_.SetInsertPoint(entry_);
    b_.CreateBr(block(f_.start));
    while (!pending_.empty()) {
      const std::uint32_t pc = pending_.back();
      pending_.pop_back();
      b_.SetInsertPoint(blocks_.at(pc));
      run(pc);
    }
    if (llvm::verifyFunction(*fn_, &llvm::errs())) fail("lifted function {} does not verify", name);
  }

 private:
  const Discovery& d_;
  const StateLayout& L_;
  std::uint32_t ram_size_;
  llvm::Module& m_;
  llvm::LLVMContext& ctx_;
  llvm::IRBuilder<> b_;
  const Function& f_;
  llvm::Function* fn_ = nullptr;
  llvm::Value *state_ = nullptr, *ram_ = nullptr;
  llvm::Type *i64_ = nullptr, *i32_ = nullptr, *f32_ = nullptr;
  llvm::AllocaInst *gpr_[32] = {}, *gprh_[32] = {}, *fpr_[32] = {};
  llvm::AllocaInst *hi_ = nullptr, *lo_ = nullptr, *fcsr_ = nullptr, *acc_ = nullptr;
  llvm::BasicBlock *entry_ = nullptr, *trap_ = nullptr;
  std::map<std::uint32_t, llvm::BasicBlock*> blocks_;
  std::vector<std::uint32_t> pending_;
  bool written_[32] = {}, writtenh_[32] = {}, writtenf_[32] = {};
  bool written_hilo_ = false, written_fcsr_ = false, written_acc_ = false;

  static bool supported(const Insn& in) {
    switch (in.op) {
      case Op::SLL: case Op::SRL: case Op::SRA: case Op::SLLV: case Op::SRLV: case Op::SRAV:
      case Op::DSLL: case Op::DSRL: case Op::DSRA: case Op::DSLL32: case Op::DSRL32: case Op::DSRA32:
      case Op::DSLLV: case Op::DSRLV: case Op::DSRAV:
      case Op::ADD: case Op::ADDU: case Op::SUB: case Op::SUBU:
      case Op::DADD: case Op::DADDU: case Op::DSUB: case Op::DSUBU:
      case Op::ADDI: case Op::ADDIU: case Op::DADDI: case Op::DADDIU:
      case Op::AND: case Op::OR: case Op::XOR: case Op::NOR: case Op::ANDI: case Op::ORI: case Op::XORI:
      case Op::LUI: case Op::SLT: case Op::SLTU: case Op::SLTI: case Op::SLTIU: case Op::MOVZ: case Op::MOVN:
      case Op::MFHI: case Op::MFLO: case Op::MTHI: case Op::MTLO:
      case Op::MULT: case Op::MULTU: case Op::DIV: case Op::DIVU:
      case Op::LB: case Op::LBU: case Op::LH: case Op::LHU: case Op::LW: case Op::LWU: case Op::LD: case Op::LQ:
      case Op::SB: case Op::SH: case Op::SW: case Op::SD: case Op::SQ:
      case Op::LWC1: case Op::SWC1: case Op::MFC1: case Op::MTC1: case Op::CFC1: case Op::CTC1:
      case Op::ADD_S: case Op::SUB_S: case Op::MUL_S: case Op::DIV_S: case Op::SQRT_S: case Op::RSQRT_S:
      case Op::ABS_S: case Op::NEG_S: case Op::MOV_S: case Op::MAX_S: case Op::MIN_S:
      case Op::ADDA_S: case Op::SUBA_S: case Op::MULA_S: case Op::MADD_S: case Op::MSUB_S:
      case Op::MADDA_S: case Op::MSUBA_S:
      case Op::CVT_W_S: case Op::CVT_S_W: case Op::C_F_S: case Op::C_EQ_S: case Op::C_LT_S: case Op::C_LE_S:
      case Op::POR: case Op::PAND: case Op::PXOR: case Op::PNOR: case Op::PCPYLD: case Op::PCPYUD:
      case Op::PEXTLW: case Op::PEXTUW: case Op::PADDW: case Op::PSUBW:
      case Op::SYNC: case Op::PREF:
      // control flow (checked separately)
      case Op::BEQ: case Op::BNE: case Op::BLEZ: case Op::BGTZ: case Op::BLTZ: case Op::BGEZ:
      case Op::BEQL: case Op::BNEL: case Op::BLEZL: case Op::BGTZL: case Op::BLTZL: case Op::BGEZL:
      case Op::BC1F: case Op::BC1T: case Op::BC1FL: case Op::BC1TL: case Op::J: case Op::JR:
        return true;
      default:
        return false;
    }
  }

  llvm::Value* field(std::uint32_t offset) { return b_.CreateConstInBoundsGEP1_32(b_.getInt8Ty(), state_, offset); }

  llvm::BasicBlock* block(std::uint32_t pc) {
    auto [it, fresh] = blocks_.try_emplace(pc, nullptr);
    if (fresh) {
      it->second = llvm::BasicBlock::Create(ctx_, std::format("L{:08X}", pc), fn_);
      pending_.push_back(pc);
    }
    return it->second;
  }

  // ---- registers ----
  llvm::Value* R(unsigned r) { return r == 0 ? static_cast<llvm::Value*>(b_.getInt64(0)) : b_.CreateLoad(i64_, gpr_[r]); }
  llvm::Value* RH(unsigned r) { return r == 0 ? static_cast<llvm::Value*>(b_.getInt64(0)) : b_.CreateLoad(i64_, gprh_[r]); }
  llvm::Value* R32(unsigned r) { return b_.CreateTrunc(R(r), i32_); }
  void W(unsigned r, llvm::Value* v) {
    if (r == 0) return;
    b_.CreateStore(v, gpr_[r]);
    written_[r] = true;
  }
  void WH(unsigned r, llvm::Value* v) {
    if (r == 0) return;
    b_.CreateStore(v, gprh_[r]);
    writtenh_[r] = true;
  }
  void W32(unsigned r, llvm::Value* v32) { W(r, b_.CreateSExt(v32, i64_)); }
  llvm::Value* F(unsigned r) { return b_.CreateLoad(i32_, fpr_[r]); }
  void WF(unsigned r, llvm::Value* bits) {
    b_.CreateStore(bits, fpr_[r]);
    writtenf_[r] = true;
  }
  llvm::Value* asf(llvm::Value* bits) { return b_.CreateBitCast(bits, f32_); }
  llvm::Value* asi(llvm::Value* f) { return b_.CreateBitCast(f, i32_); }
  // The reference core's clamp: +inf/+NaN -> +max, -inf/-NaN -> -max (integer min on the bit pattern).
  llvm::Value* clamp(llvm::Value* bits) {
    bits = b_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, bits, b_.getInt32(0x7F7FFFFF));
    bits = b_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, bits, b_.getInt32(0xFF7FFFFFu));
    return asf(bits);
  }
  llvm::Value* CF(unsigned r) { return clamp(F(r)); }

  void set_cc(llvm::Value* cond) {
    llvm::Value* fcsr = b_.CreateLoad(i32_, fcsr_);
    fcsr = b_.CreateSelect(cond, b_.CreateOr(fcsr, b_.getInt32(0x00800000)), b_.CreateAnd(fcsr, b_.getInt32(~0x00800000u)));
    b_.CreateStore(fcsr, fcsr_);
    written_fcsr_ = true;
  }
  llvm::Value* cc() {
    return b_.CreateICmpNE(b_.CreateAnd(b_.CreateLoad(i32_, fcsr_), b_.getInt32(0x00800000)), b_.getInt32(0));
  }

  // ---- memory ----
  // Main memory only: the address (and its uncached mirrors) is masked to 29
  // bits; anything that is not inside main memory leaves the function.
  llvm::Value* address(const Insn& in, unsigned size, std::uint32_t align_mask = 0) {
    llvm::Value* a = b_.CreateAdd(R32(in.rs()), b_.getInt32(static_cast<std::uint32_t>(in.simm())));
    if (align_mask) a = b_.CreateAnd(a, b_.getInt32(~align_mask));
    a = b_.CreateAnd(a, b_.getInt32(0x1FFFFFFF));
    llvm::Value* ok = b_.CreateICmpULE(a, b_.getInt32(ram_size_ - size));
    auto* next = llvm::BasicBlock::Create(ctx_, "", fn_);
    b_.CreateCondBr(ok, next, trap_);
    b_.SetInsertPoint(next);
    return b_.CreateInBoundsGEP(b_.getInt8Ty(), ram_, b_.CreateZExt(a, i64_));
  }
  llvm::Value* load(llvm::Type* t, llvm::Value* p) { return b_.CreateAlignedLoad(t, p, llvm::MaybeAlign(1)); }
  void store(llvm::Value* v, llvm::Value* p) { b_.CreateAlignedStore(v, p, llvm::MaybeAlign(1)); }

  // ---- emission ----
  void run(std::uint32_t pc) {
    const std::uint32_t first = pc;
    for (;;) {
      if (pc >= f_.end || !(d_.flags_at(pc) & kReached)) {
        b_.CreateBr(trap_);  // runs off the end of what the analysis reached
        return;
      }
      if (pc != first && (d_.flags_at(pc) & kLeader)) {
        b_.CreateBr(block(pc));
        return;
      }
      const Insn& in = d_.insns[d_.index_of(pc)];
      if (in.flow == Flow::None) {
        emit(in);
        pc += 4;
        continue;
      }
      control(pc, in);
      return;
    }
  }

  void control(std::uint32_t pc, const Insn& in) {
    const Insn& slot = d_.insns[d_.index_of(pc + 4)];
    if (in.flow == Flow::JumpReg) {  // jr ra
      llvm::Value* target = R32(in.rs());
      emit(slot);
      leave(target);
      return;
    }
    const std::uint32_t target = r5900::target_of(pc, in);
    if (in.flow == Flow::Jump) {
      emit(slot);
      b_.CreateBr(block(target));
      return;
    }
    llvm::Value* c = condition(in);
    if (in.likely) {
      auto* taken = llvm::BasicBlock::Create(ctx_, "", fn_);
      b_.CreateCondBr(c, taken, block(pc + 8));
      b_.SetInsertPoint(taken);
      emit(slot);
      b_.CreateBr(block(target));
    } else {
      emit(slot);
      b_.CreateCondBr(c, block(target), block(pc + 8));
    }
  }

  llvm::Value* condition(const Insn& in) {
    llvm::Value* s = R(in.rs());
    llvm::Value* zero = b_.getInt64(0);
    switch (in.op) {
      case Op::BEQ: case Op::BEQL: return b_.CreateICmpEQ(s, R(in.rt()));
      case Op::BNE: case Op::BNEL: return b_.CreateICmpNE(s, R(in.rt()));
      case Op::BLEZ: case Op::BLEZL: return b_.CreateICmpSLE(s, zero);
      case Op::BGTZ: case Op::BGTZL: return b_.CreateICmpSGT(s, zero);
      case Op::BLTZ: case Op::BLTZL: return b_.CreateICmpSLT(s, zero);
      case Op::BGEZ: case Op::BGEZL: return b_.CreateICmpSGE(s, zero);
      case Op::BC1T: case Op::BC1TL: return cc();
      case Op::BC1F: case Op::BC1FL: return b_.CreateNot(cc());
      default: unsupported(std::string(r5900::mnemonic(in.op)));
    }
  }

  // Writes back everything the function may have changed and returns.
  void leave(llvm::Value* pc) {
    for (int r = 1; r < 32; ++r) {
      if (written_[r]) b_.CreateStore(b_.CreateLoad(i64_, gpr_[r]), field(L_.gpr + r * L_.gpr_stride));
      if (writtenh_[r]) b_.CreateStore(b_.CreateLoad(i64_, gprh_[r]), field(L_.gpr + r * L_.gpr_stride + 8));
    }
    for (int r = 0; r < 32; ++r)
      if (writtenf_[r]) b_.CreateStore(b_.CreateLoad(i32_, fpr_[r]), field(L_.fpr + r * 4));
    if (written_hilo_) {
      b_.CreateStore(b_.CreateLoad(i64_, hi_), field(L_.hi));
      b_.CreateStore(b_.CreateLoad(i64_, lo_), field(L_.lo));
    }
    if (written_fcsr_) b_.CreateStore(b_.CreateLoad(i32_, fcsr_), field(L_.fcsr));
    if (written_acc_) b_.CreateStore(b_.CreateLoad(i32_, acc_), field(L_.fpacc));
    b_.CreateStore(pc, field(L_.pc));
    b_.CreateRetVoid();
  }
  // Exits are emitted while lifting, before every write is known: patch them afterwards.
  // (Simplest correct approach: every exit stores every register class that is written
  // anywhere in the function. The written_ flags are computed in a pre-pass instead.)

 public:
  void prescan_writes() {
    for (std::uint32_t pc = f_.start; pc < f_.end; pc += 4) {
      if (!(d_.flags_at(pc) & kReached)) continue;
      const Insn& in = d_.insns[d_.index_of(pc)];
      const int dest = r5900::dest_gpr(in);
      if (dest > 0) written_[dest] = true;
      const unsigned fd = in.sa(), ft = in.rt(), fs = in.rd();
      switch (in.op) {
        case Op::LQ: case Op::POR: case Op::PAND: case Op::PXOR: case Op::PNOR: case Op::PCPYLD:
        case Op::PCPYUD: case Op::PEXTLW: case Op::PEXTUW: case Op::PADDW: case Op::PSUBW:
          if (dest > 0) writtenh_[dest] = true;
          break;
        case Op::MULT: case Op::MULTU: case Op::DIV: case Op::DIVU: case Op::MTHI: case Op::MTLO:
          written_hilo_ = true;
          break;
        case Op::LWC1: writtenf_[ft] = true; break;
        case Op::MTC1: writtenf_[fs] = true; break;
        case Op::CTC1: written_fcsr_ = true; break;
        case Op::C_F_S: case Op::C_EQ_S: case Op::C_LT_S: case Op::C_LE_S: written_fcsr_ = true; break;
        case Op::ADDA_S: case Op::SUBA_S: case Op::MULA_S: case Op::MADDA_S: case Op::MSUBA_S:
          written_acc_ = true;
          break;
        case Op::SQRT_S: case Op::ABS_S: case Op::NEG_S: case Op::MOV_S: case Op::CVT_W_S: case Op::CVT_S_W:
        case Op::ADD_S: case Op::SUB_S: case Op::MUL_S: case Op::DIV_S: case Op::RSQRT_S: case Op::MAX_S:
        case Op::MIN_S: case Op::MADD_S: case Op::MSUB_S:
          writtenf_[fd] = true;
          break;
        default:
          break;
      }
    }
  }

 private:
  void emit(const Insn& in) {
    const unsigned rs = in.rs(), rt = in.rt(), rd = in.rd(), sa = in.sa();
    const unsigned fs = rd, ft = rt, fd = sa;
    auto sx = [&](std::int64_t v) { return b_.getInt64(static_cast<std::uint64_t>(v)); };
    switch (in.op) {
      // ---- shifts ----
      case Op::SLL: if (rd) W32(rd, b_.CreateShl(R32(rt), sa)); break;
      case Op::SRL: if (rd) W32(rd, b_.CreateLShr(R32(rt), sa)); break;
      case Op::SRA: if (rd) W32(rd, b_.CreateAShr(R32(rt), sa)); break;
      case Op::SLLV: if (rd) W32(rd, b_.CreateShl(R32(rt), b_.CreateAnd(R32(rs), 31))); break;
      case Op::SRLV: if (rd) W32(rd, b_.CreateLShr(R32(rt), b_.CreateAnd(R32(rs), 31))); break;
      case Op::SRAV: if (rd) W32(rd, b_.CreateAShr(R32(rt), b_.CreateAnd(R32(rs), 31))); break;
      case Op::DSLL: W(rd, b_.CreateShl(R(rt), sa)); break;
      case Op::DSRL: W(rd, b_.CreateLShr(R(rt), sa)); break;
      case Op::DSRA: W(rd, b_.CreateAShr(R(rt), sa)); break;
      case Op::DSLL32: W(rd, b_.CreateShl(R(rt), sa + 32)); break;
      case Op::DSRL32: W(rd, b_.CreateLShr(R(rt), sa + 32)); break;
      case Op::DSRA32: W(rd, b_.CreateAShr(R(rt), sa + 32)); break;
      case Op::DSLLV: W(rd, b_.CreateShl(R(rt), b_.CreateAnd(R(rs), 63))); break;
      case Op::DSRLV: W(rd, b_.CreateLShr(R(rt), b_.CreateAnd(R(rs), 63))); break;
      case Op::DSRAV: W(rd, b_.CreateAShr(R(rt), b_.CreateAnd(R(rs), 63))); break;
      // ---- arithmetic and logic ----
      case Op::ADD: case Op::ADDU: if (rd) W32(rd, b_.CreateAdd(R32(rs), R32(rt))); break;
      case Op::SUB: case Op::SUBU: if (rd) W32(rd, b_.CreateSub(R32(rs), R32(rt))); break;
      case Op::DADD: case Op::DADDU: W(rd, b_.CreateAdd(R(rs), R(rt))); break;
      case Op::DSUB: case Op::DSUBU: W(rd, b_.CreateSub(R(rs), R(rt))); break;
      case Op::ADDI: case Op::ADDIU:
        if (rt) W32(rt, b_.CreateAdd(R32(rs), b_.getInt32(static_cast<std::uint32_t>(in.simm()))));
        break;
      case Op::DADDI: case Op::DADDIU: W(rt, b_.CreateAdd(R(rs), sx(in.simm()))); break;
      case Op::AND: W(rd, b_.CreateAnd(R(rs), R(rt))); break;
      case Op::OR: W(rd, b_.CreateOr(R(rs), R(rt))); break;
      case Op::XOR: W(rd, b_.CreateXor(R(rs), R(rt))); break;
      case Op::NOR: W(rd, b_.CreateNot(b_.CreateOr(R(rs), R(rt)))); break;
      case Op::ANDI: W(rt, b_.CreateAnd(R(rs), b_.getInt64(in.imm()))); break;
      case Op::ORI: W(rt, b_.CreateOr(R(rs), b_.getInt64(in.imm()))); break;
      case Op::XORI: W(rt, b_.CreateXor(R(rs), b_.getInt64(in.imm()))); break;
      case Op::LUI: W(rt, sx(static_cast<std::int32_t>(in.imm() << 16))); break;
      case Op::SLT: W(rd, b_.CreateZExt(b_.CreateICmpSLT(R(rs), R(rt)), i64_)); break;
      case Op::SLTU: W(rd, b_.CreateZExt(b_.CreateICmpULT(R(rs), R(rt)), i64_)); break;
      case Op::SLTI: W(rt, b_.CreateZExt(b_.CreateICmpSLT(R(rs), sx(in.simm())), i64_)); break;
      case Op::SLTIU: W(rt, b_.CreateZExt(b_.CreateICmpULT(R(rs), sx(in.simm())), i64_)); break;
      case Op::MOVZ: if (rd) W(rd, b_.CreateSelect(b_.CreateICmpEQ(R(rt), b_.getInt64(0)), R(rs), R(rd))); break;
      case Op::MOVN: if (rd) W(rd, b_.CreateSelect(b_.CreateICmpNE(R(rt), b_.getInt64(0)), R(rs), R(rd))); break;
      case Op::MFHI: W(rd, b_.CreateLoad(i64_, hi_)); break;
      case Op::MFLO: W(rd, b_.CreateLoad(i64_, lo_)); break;
      case Op::MTHI: b_.CreateStore(R(rs), hi_); break;
      case Op::MTLO: b_.CreateStore(R(rs), lo_); break;
      case Op::MULT: case Op::MULTU: {
        const bool sgn = in.op == Op::MULT;
        auto ext = [&](llvm::Value* v) { return sgn ? b_.CreateSExt(v, i64_) : b_.CreateZExt(v, i64_); };
        llvm::Value* p = b_.CreateMul(ext(R32(rs)), ext(R32(rt)));
        llvm::Value* lo = b_.CreateSExt(b_.CreateTrunc(p, i32_), i64_);
        llvm::Value* hi = b_.CreateSExt(b_.CreateTrunc(b_.CreateLShr(p, 32), i32_), i64_);
        b_.CreateStore(lo, lo_);
        b_.CreateStore(hi, hi_);
        W(rd, lo);
        break;
      }
      case Op::DIV: case Op::DIVU: {
        const bool sgn = in.op == Op::DIV;
        llvm::Value* n = R32(rs);
        llvm::Value* dv = R32(rt);
        llvm::Value* by_zero = b_.CreateICmpEQ(dv, b_.getInt32(0));
        llvm::Value* overflow = sgn ? b_.CreateAnd(b_.CreateICmpEQ(n, b_.getInt32(0x80000000u)),
                                                   b_.CreateICmpEQ(dv, b_.getInt32(0xFFFFFFFFu)))
                                    : static_cast<llvm::Value*>(b_.getFalse());
        llvm::Value* safe = b_.CreateSelect(b_.CreateOr(by_zero, overflow), b_.getInt32(1), dv);
        llvm::Value* q = sgn ? b_.CreateSDiv(n, safe) : b_.CreateUDiv(n, safe);
        llvm::Value* r = sgn ? b_.CreateSRem(n, safe) : b_.CreateURem(n, safe);
        llvm::Value* zq = sgn ? b_.CreateSelect(b_.CreateICmpSLT(n, b_.getInt32(0)), b_.getInt32(1), b_.getInt32(~0u))
                              : static_cast<llvm::Value*>(b_.getInt32(~0u));
        q = b_.CreateSelect(by_zero, zq, b_.CreateSelect(overflow, b_.getInt32(0x80000000u), q));
        r = b_.CreateSelect(by_zero, n, b_.CreateSelect(overflow, b_.getInt32(0), r));
        b_.CreateStore(b_.CreateSExt(q, i64_), lo_);
        b_.CreateStore(b_.CreateSExt(r, i64_), hi_);
        break;
      }
      // ---- memory ----
      case Op::LB: { auto* p = address(in, 1); W(rt, b_.CreateSExt(load(b_.getInt8Ty(), p), i64_)); break; }
      case Op::LBU: { auto* p = address(in, 1); W(rt, b_.CreateZExt(load(b_.getInt8Ty(), p), i64_)); break; }
      case Op::LH: { auto* p = address(in, 2); W(rt, b_.CreateSExt(load(b_.getInt16Ty(), p), i64_)); break; }
      case Op::LHU: { auto* p = address(in, 2); W(rt, b_.CreateZExt(load(b_.getInt16Ty(), p), i64_)); break; }
      case Op::LW: { auto* p = address(in, 4); W(rt, b_.CreateSExt(load(i32_, p), i64_)); break; }
      case Op::LWU: { auto* p = address(in, 4); W(rt, b_.CreateZExt(load(i32_, p), i64_)); break; }
      case Op::LD: { auto* p = address(in, 8); W(rt, load(i64_, p)); break; }
      case Op::LQ: {
        auto* p = address(in, 16, 15);
        llvm::Value* lo = load(i64_, p);
        llvm::Value* hi = load(i64_, b_.CreateConstInBoundsGEP1_32(b_.getInt8Ty(), p, 8));
        W(rt, lo);
        WH(rt, hi);
        break;
      }
      case Op::SB: { auto* p = address(in, 1); store(b_.CreateTrunc(R(rt), b_.getInt8Ty()), p); break; }
      case Op::SH: { auto* p = address(in, 2); store(b_.CreateTrunc(R(rt), b_.getInt16Ty()), p); break; }
      case Op::SW: { auto* p = address(in, 4); store(R32(rt), p); break; }
      case Op::SD: { auto* p = address(in, 8); store(R(rt), p); break; }
      case Op::SQ: {
        auto* p = address(in, 16, 15);
        store(R(rt), p);
        store(RH(rt), b_.CreateConstInBoundsGEP1_32(b_.getInt8Ty(), p, 8));
        break;
      }
      case Op::LWC1: { auto* p = address(in, 4); WF(ft, load(i32_, p)); break; }
      case Op::SWC1: { auto* p = address(in, 4); store(F(ft), p); break; }
      case Op::SYNC: case Op::PREF: break;
      // ---- FPU ----
      case Op::MFC1: W(rt, b_.CreateSExt(F(fs), i64_)); break;
      case Op::MTC1: WF(fs, R32(rt)); break;
      case Op::CFC1:
        W(rt, fs < 16 ? static_cast<llvm::Value*>(b_.getInt64(0x2E30))
                      : b_.CreateSExt(b_.CreateLoad(i32_, fcsr_), i64_));
        break;
      case Op::CTC1:
        if (fs == 31) {
          constexpr std::uint32_t mask = 0x0083C078;
          llvm::Value* v = b_.CreateOr(b_.CreateAnd(R32(rt), b_.getInt32(mask)),
                                       b_.CreateAnd(b_.CreateLoad(i32_, fcsr_), b_.getInt32(~mask)));
          b_.CreateStore(v, fcsr_);
          written_fcsr_ = true;
        }
        break;
      case Op::ADD_S: WF(fd, asi(b_.CreateFAdd(CF(fs), CF(ft)))); break;
      case Op::SUB_S: WF(fd, asi(b_.CreateFSub(CF(fs), CF(ft)))); break;
      case Op::MUL_S: WF(fd, asi(b_.CreateFMul(CF(fs), CF(ft)))); break;
      case Op::DIV_S: case Op::RSQRT_S: {
        llvm::Value* a = F(fs);
        llvm::Value* bb = F(ft);
        llvm::Value* zero = b_.CreateICmpEQ(b_.CreateAnd(bb, b_.getInt32(0x7F800000)), b_.getInt32(0));
        llvm::Value* inf = b_.CreateOr(b_.getInt32(0x7F7FFFFF), b_.CreateAnd(b_.CreateXor(a, bb), b_.getInt32(0x80000000u)));
        llvm::Value* q;
        if (in.op == Op::DIV_S) {
          q = b_.CreateFDiv(clamp(a), clamp(bb));
        } else {
          llvm::Value* r = b_.CreateFDiv(llvm::ConstantFP::get(f32_, 1.0),
                                         b_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, clamp(bb)));
          q = b_.CreateFMul(clamp(a), r);
        }
        WF(fd, b_.CreateSelect(zero, inf, asi(q)));
        break;
      }
      case Op::SQRT_S:
        WF(fd, asi(b_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, b_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, CF(ft)))));
        break;
      case Op::ABS_S: WF(fd, b_.CreateAnd(F(fs), b_.getInt32(0x7FFFFFFF))); break;
      case Op::NEG_S: WF(fd, b_.CreateXor(F(fs), b_.getInt32(0x80000000u))); break;
      case Op::MOV_S: WF(fd, F(fs)); break;
      case Op::MAX_S: case Op::MIN_S: {
        llvm::Value* a = CF(fs);
        llvm::Value* bb = CF(ft);
        llvm::Value* pick = in.op == Op::MAX_S ? b_.CreateFCmpOGT(a, bb) : b_.CreateFCmpOLT(a, bb);
        WF(fd, asi(b_.CreateSelect(pick, a, bb)));  // what maxss / minss return
        break;
      }
      case Op::ADDA_S: b_.CreateStore(asi(b_.CreateFAdd(CF(fs), CF(ft))), acc_); written_acc_ = true; break;
      case Op::SUBA_S: b_.CreateStore(asi(b_.CreateFSub(CF(fs), CF(ft))), acc_); written_acc_ = true; break;
      case Op::MULA_S: b_.CreateStore(asi(b_.CreateFMul(CF(fs), CF(ft))), acc_); written_acc_ = true; break;
      case Op::MADD_S: case Op::MSUB_S: case Op::MADDA_S: case Op::MSUBA_S: {
        llvm::Value* acc = clamp(b_.CreateLoad(i32_, acc_));
        llvm::Value* prod = b_.CreateFMul(CF(fs), CF(ft));
        const bool add = in.op == Op::MADD_S || in.op == Op::MADDA_S;
        llvm::Value* r = asi(add ? b_.CreateFAdd(acc, prod) : b_.CreateFSub(acc, prod));
        if (in.op == Op::MADD_S || in.op == Op::MSUB_S) WF(fd, r);
        else { b_.CreateStore(r, acc_); written_acc_ = true; }
        break;
      }
      case Op::CVT_W_S: {
        llvm::Value* x = asf(F(fs));
        llvm::Value* in_range = b_.CreateAnd(b_.CreateFCmpOGE(x, llvm::ConstantFP::get(f32_, -2147483648.0)),
                                             b_.CreateFCmpOLT(x, llvm::ConstantFP::get(f32_, 2147483648.0)));
        WF(fd, b_.CreateSelect(in_range, b_.CreateFPToSI(x, i32_), b_.getInt32(0x80000000u)));  // as cvttss2si
        break;
      }
      case Op::CVT_S_W: WF(fd, asi(b_.CreateSIToFP(F(fs), f32_))); break;
      case Op::C_F_S: set_cc(b_.getFalse()); break;
      case Op::C_EQ_S: set_cc(b_.CreateFCmpOEQ(CF(fs), CF(ft))); break;
      case Op::C_LT_S: set_cc(b_.CreateFCmpOLT(CF(fs), CF(ft))); break;
      case Op::C_LE_S: set_cc(b_.CreateFCmpOLE(CF(fs), CF(ft))); break;
      // ---- 128-bit ----
      case Op::POR: W(rd, b_.CreateOr(R(rs), R(rt))); WH(rd, b_.CreateOr(RH(rs), RH(rt))); break;
      case Op::PAND: W(rd, b_.CreateAnd(R(rs), R(rt))); WH(rd, b_.CreateAnd(RH(rs), RH(rt))); break;
      case Op::PXOR: W(rd, b_.CreateXor(R(rs), R(rt))); WH(rd, b_.CreateXor(RH(rs), RH(rt))); break;
      case Op::PNOR:
        W(rd, b_.CreateNot(b_.CreateOr(R(rs), R(rt))));
        WH(rd, b_.CreateNot(b_.CreateOr(RH(rs), RH(rt))));
        break;
      case Op::PCPYLD: { auto* lo = R(rt); auto* hi = R(rs); W(rd, lo); WH(rd, hi); break; }
      case Op::PCPYUD: { auto* lo = RH(rs); auto* hi = RH(rt); W(rd, lo); WH(rd, hi); break; }
      case Op::PEXTLW: case Op::PEXTUW: {
        llvm::Value* s = in.op == Op::PEXTLW ? R(rs) : RH(rs);
        llvm::Value* t = in.op == Op::PEXTLW ? R(rt) : RH(rt);
        auto w = [&](llvm::Value* v, int i) { return b_.CreateAnd(i ? b_.CreateLShr(v, 32) : v, b_.getInt64(0xFFFFFFFF)); };
        llvm::Value* lo = b_.CreateOr(w(t, 0), b_.CreateShl(w(s, 0), 32));
        llvm::Value* hi = b_.CreateOr(w(t, 1), b_.CreateShl(w(s, 1), 32));
        W(rd, lo);
        WH(rd, hi);
        break;
      }
      case Op::PADDW: case Op::PSUBW: {
        auto lanes = [&](llvm::Value* x, llvm::Value* y) {
          auto* v2 = llvm::FixedVectorType::get(i32_, 2);
          auto* a = b_.CreateBitCast(x, v2);
          auto* c = b_.CreateBitCast(y, v2);
          return b_.CreateBitCast(in.op == Op::PADDW ? b_.CreateAdd(a, c) : b_.CreateSub(a, c), i64_);
        };
        llvm::Value* lo = lanes(R(rs), R(rt));
        llvm::Value* hi = lanes(RH(rs), RH(rt));
        W(rd, lo);
        WH(rd, hi);
        break;
      }
      default:
        unsupported(std::format("no translation for {}", r5900::mnemonic(in.op)));
    }
  }
};

}  // namespace

struct Translator::Impl {
  const ElfImage& elf;
  const Discovery& d;
  StateLayout layout;
  std::uint32_t ram_size;
  std::unique_ptr<llvm::LLVMContext> context = std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> module;
  std::unique_ptr<llvm::orc::LLJIT> jit;
  std::unique_ptr<llvm::TargetMachine> target;
  std::map<std::uint32_t, std::string> names;
  std::map<std::uint32_t, LiftedFunction> code;
  std::string optimised_ir;
};

Translator::Translator(const ElfImage& elf, const Discovery& discovery, const StateLayout& layout,
                       std::uint32_t ram_size)
    : impl_(new Impl{elf, discovery, layout, ram_size}) {
  static const bool initialised = [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    return true;
  }();
  (void)initialised;
  auto builder = check(llvm::orc::JITTargetMachineBuilder::detectHost(), "detecting the host CPU");
  builder.setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
  impl_->target = check(builder.createTargetMachine(), "creating the target machine");
  impl_->jit = check(llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(builder).create(), "creating the JIT");
  impl_->module = std::make_unique<llvm::Module>("hp2", *impl_->context);
  impl_->module->setDataLayout(impl_->jit->getDataLayout());
  impl_->module->setTargetTriple(impl_->jit->getTargetTriple());
}

Translator::~Translator() = default;

std::optional<std::string> Translator::add_function(std::uint32_t start) {
  Impl& m = *impl_;
  if (!m.module) return std::string("compile() was already called");
  const Function* f = m.d.function_at(start);
  if (!f || f->start != start) return std::format("{:08X} is not a function start", start);
  if (m.names.count(start)) return std::nullopt;
  FunctionLifter lifter(m.d, m.layout, m.ram_size, *m.module, *f);
  try {
    lifter.precheck();
  } catch (const Unsupported& u) {
    return u.why;
  }
  lifter.prescan_writes();
  const std::string name = std::format("fn_{:08X}", start);
  try {
    lifter.lift(name);
  } catch (const Unsupported& u) {
    if (llvm::Function* partial = m.module->getFunction(name)) partial->eraseFromParent();
    return u.why;
  }
  m.names.emplace(start, name);
  return std::nullopt;
}

void Translator::compile() {
  Impl& m = *impl_;
  llvm::PassBuilder passes(m.target.get());
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  passes.registerModuleAnalyses(mam);
  passes.registerCGSCCAnalyses(cgam);
  passes.registerFunctionAnalyses(fam);
  passes.registerLoopAnalyses(lam);
  passes.crossRegisterProxies(lam, fam, cgam, mam);
  passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(*m.module, mam);
  {
    llvm::raw_string_ostream out(m.optimised_ir);
    m.module->print(out, nullptr);
  }
  check(m.jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(m.module), std::move(m.context))),
        "adding the module");
  for (const auto& [start, name] : m.names) {
    auto address = check(m.jit->lookup(name), "looking up a function");
    m.code.emplace(start, address.toPtr<LiftedFunction>());
  }
}

LiftedFunction Translator::lookup(std::uint32_t start) const {
  const auto it = impl_->code.find(start);
  return it == impl_->code.end() ? nullptr : it->second;
}

std::string Translator::ir() const {
  if (impl_->module) {
    std::string text;
    llvm::raw_string_ostream out(text);
    impl_->module->print(out, nullptr);
    return text;
  }
  return impl_->optimised_ir;
}

std::size_t Translator::size() const {
  return impl_->names.size();
}

}  // namespace hp2
