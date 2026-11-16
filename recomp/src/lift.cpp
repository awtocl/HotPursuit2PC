// Translator: see hp2/lift.h.
//
// The semantics follow the reference core instruction by instruction (it is
// the oracle the result is compared with), in particular:
//  - 32-bit operations sign-extend their result to 64 bits; operations other
//    than the 128-bit ones leave the upper 64 bits of a register unchanged;
//  - mult/multu (and the second unit's mult1/multu1, madd/madd1) also write
//    rd; div/divu by zero and INT_MIN / -1 give the reference core's values;
//  - FPU operands are "clamped" the way the reference core does it (infinity
//    and NaN become the largest finite value), division by zero gives the
//    largest finite value with the sign of the quotient, cvt.w.s gives
//    0x80000000 when out of range. Rounding and denormal handling come from
//    the thread's floating-point mode, which the reference core sets on its
//    thread to round-toward-zero with denormals flushed - the translated code
//    runs on that same thread;
//  - addresses: 0x70000000-0x70003FFF is the scratchpad, 0x30100000-0x31FFFFFF
//    is main memory plus 0x30000000, everything else is masked to 29 bits;
//    whatever is not main memory goes through the runtime's slow path.
//
// Registers live in the state structure; the state pointer is `noalias`, so
// LLVM keeps them in host registers between calls and stores them only where
// needed.
#include "lift_internal.h"

#include <map>
#include <set>
#include <thread>


namespace hp2 {

using r5900::Flow;
using r5900::Insn;
using r5900::Op;
using namespace detail;

namespace {

std::string function_name(std::uint32_t start) {
  return std::format("fn_{:08X}", start);
}

bool supported(const Insn& in, bool native) {
  switch (in.op) {
    case Op::EI: case Op::DI: case Op::MFC0: case Op::MTC0:
      return native;  // the native runtime's own system services (Runtime slot 2)
    case Op::SLL: case Op::SRL: case Op::SRA: case Op::SLLV: case Op::SRLV: case Op::SRAV:
    case Op::DSLL: case Op::DSRL: case Op::DSRA: case Op::DSLL32: case Op::DSRL32: case Op::DSRA32:
    case Op::DSLLV: case Op::DSRLV: case Op::DSRAV:
    case Op::ADD: case Op::ADDU: case Op::SUB: case Op::SUBU:
    case Op::DADD: case Op::DADDU: case Op::DSUB: case Op::DSUBU:
    case Op::ADDI: case Op::ADDIU: case Op::DADDI: case Op::DADDIU:
    case Op::AND: case Op::OR: case Op::XOR: case Op::NOR: case Op::ANDI: case Op::ORI: case Op::XORI:
    case Op::LUI: case Op::SLT: case Op::SLTU: case Op::SLTI: case Op::SLTIU: case Op::MOVZ: case Op::MOVN:
    case Op::MFHI: case Op::MFLO: case Op::MTHI: case Op::MTLO:
    case Op::MFHI1: case Op::MFLO1: case Op::MTHI1: case Op::MTLO1:
    case Op::MULT: case Op::MULTU: case Op::DIV: case Op::DIVU:
    case Op::MULT1: case Op::MULTU1: case Op::DIV1: case Op::DIVU1:
    case Op::MADD: case Op::MADDU: case Op::MADD1: case Op::MADDU1:
    case Op::LB: case Op::LBU: case Op::LH: case Op::LHU: case Op::LW: case Op::LWU: case Op::LD: case Op::LQ:
    case Op::SB: case Op::SH: case Op::SW: case Op::SD: case Op::SQ:
    case Op::LWL: case Op::LWR: case Op::LDL: case Op::LDR: case Op::SWL: case Op::SWR: case Op::SDL: case Op::SDR:
    case Op::LWC1: case Op::SWC1: case Op::MFC1: case Op::MTC1: case Op::CFC1: case Op::CTC1:
    case Op::ADD_S: case Op::SUB_S: case Op::MUL_S: case Op::DIV_S: case Op::SQRT_S: case Op::RSQRT_S:
    case Op::ABS_S: case Op::NEG_S: case Op::MOV_S: case Op::MAX_S: case Op::MIN_S:
    case Op::ADDA_S: case Op::SUBA_S: case Op::MULA_S: case Op::MADD_S: case Op::MSUB_S:
    case Op::MADDA_S: case Op::MSUBA_S:
    case Op::CVT_W_S: case Op::CVT_S_W: case Op::C_F_S: case Op::C_EQ_S: case Op::C_LT_S: case Op::C_LE_S:
    case Op::POR: case Op::PAND: case Op::PXOR: case Op::PNOR: case Op::PCPYLD: case Op::PCPYUD:
    case Op::PEXTLW: case Op::PEXTUW: case Op::PADDW: case Op::PSUBW: case Op::PMULTH:
    case Op::LQC2: case Op::SQC2: case Op::QMFC2: case Op::QMTC2:
    case Op::SYNC: case Op::PREF: case Op::CACHE:
    // VU0 macro mode
    case Op::VADD: case Op::VSUB: case Op::VMUL: case Op::VMADD: case Op::VMSUB:
    case Op::VADDbc: case Op::VSUBbc: case Op::VMULbc: case Op::VMADDbc: case Op::VMSUBbc:
    case Op::VADDi: case Op::VSUBi: case Op::VMULi: case Op::VMADDi: case Op::VMSUBi:
    case Op::VADDq: case Op::VSUBq: case Op::VMULq: case Op::VMADDq: case Op::VMSUBq:
    case Op::VADDA: case Op::VSUBA: case Op::VMULA: case Op::VMADDA: case Op::VMSUBA:
    case Op::VADDAbc: case Op::VSUBAbc: case Op::VMULAbc: case Op::VMADDAbc: case Op::VMSUBAbc:
    case Op::VADDAi: case Op::VSUBAi: case Op::VMULAi: case Op::VMADDAi: case Op::VMSUBAi:
    case Op::VADDAq: case Op::VSUBAq: case Op::VMULAq: case Op::VMADDAq: case Op::VMSUBAq:
    case Op::VMAX: case Op::VMAXbc: case Op::VMAXi: case Op::VMINI: case Op::VMINIbc: case Op::VMINIi:
    case Op::VOPMULA: case Op::VOPMSUB: case Op::VCLIP: case Op::VMOVE: case Op::VMR32: case Op::VABS:
    case Op::VFTOI0: case Op::VFTOI4: case Op::VFTOI12: case Op::VFTOI15:
    case Op::VITOF0: case Op::VITOF4: case Op::VITOF12: case Op::VITOF15:
    case Op::VMTIR: case Op::VMFIR: case Op::VIADD: case Op::VISUB: case Op::VIAND: case Op::VIOR: case Op::VIADDI:
    case Op::VDIV: case Op::VSQRT: case Op::VRSQRT: case Op::VWAITQ: case Op::VNOP:
      return true;
    case Op::CFC2: case Op::CTC2: {
      const unsigned reg = in.rd();
      return reg < 16 || reg == 17 || reg == 18 || reg == 20 || reg == 21 || reg == 22;
    }
    default:
      return false;
  }
}

bool branch_supported(const Insn& in) {
  switch (in.op) {
    case Op::BEQ: case Op::BNE: case Op::BLEZ: case Op::BGTZ: case Op::BLTZ: case Op::BGEZ:
    case Op::BEQL: case Op::BNEL: case Op::BLEZL: case Op::BGTZL: case Op::BLTZL: case Op::BGEZL:
    case Op::BC1F: case Op::BC1T: case Op::BC1FL: case Op::BC1TL:
      return true;
    default:
      return false;
  }
}

llvm::Function* declare_function(llvm::Module& m, const std::string& name) {
  if (llvm::Function* existing = m.getFunction(name)) return existing;
  auto& ctx = m.getContext();
  auto* ptr = llvm::PointerType::get(ctx, 0);
  auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptr, ptr, ptr, ptr}, false),
                                    llvm::Function::ExternalLinkage, name, m);
  fn->addParamAttr(0, llvm::Attribute::NoAlias);
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  // The reference core's thread runs with denormals flushed to zero (inputs and
  // outputs); tell LLVM, so that it never folds a comparison as if they were not.
  fn->addFnAttr("denormal-fp-math", "preserve-sign,preserve-sign");
  fn->addFnAttr("denormal-fp-math-f32", "preserve-sign,preserve-sign");
  return fn;
}

class FunctionLifter : public VectorCode {
 public:
  FunctionLifter(const Discovery& d, const StateLayout& layout, std::uint32_t ram_size, llvm::Module& module,
                 const Function& f, TranslateStats& stats, std::map<std::string, std::size_t>& reasons,
                 std::vector<std::uint32_t>& entries)
      : VectorCode(module.getContext(), layout), d_(d), ram_size_(ram_size), m_(module), f_(f),
        stats_(stats), reasons_(reasons), entries_(entries) {}

  void lift() {
    fn_ = declare_function(m_, function_name(f_.start));
    if (!fn_->empty()) fail("function {:08X} translated twice", f_.start);
    state_ = fn_->getArg(0);
    ram_ = fn_->getArg(1);
    spr_ = fn_->getArg(2);
    rt_ = fn_->getArg(3);
    // Entry: the function start, or any other place where a straight run of
    // code begins and the translated code can make progress (the reference
    // core re-enters translated code there after it ran something itself).
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn_);
    b_.SetInsertPoint(entry);
    auto* sw = b_.CreateSwitch(ld(i32_, L_.pc), block(f_.start));
    for (std::uint32_t pc = f_.start + 4; pc < f_.end; pc += 4)
      if ((d_.flags_at(pc) & kLeader) && inside(pc) && can_enter(pc)) {
        sw->addCase(b_.getInt32(pc), block(pc));
        entries_.push_back(pc);
      }
    while (!pending_.empty()) {
      const std::uint32_t pc = pending_.back();
      pending_.pop_back();
      b_.SetInsertPoint(blocks_.at(pc));
      run(pc);
    }
    if (llvm::verifyFunction(*fn_, &llvm::errs())) fail("translated function {:08X} does not verify", f_.start);
    for (std::uint32_t pc = f_.start; pc < f_.end; pc += 4)
      if (d_.flags_at(pc) & kReached) ++stats_.instructions;
  }

 private:
  const Discovery& d_;
  std::uint32_t ram_size_;
  llvm::Module& m_;
  const Function& f_;
  TranslateStats& stats_;
  std::map<std::string, std::size_t>& reasons_;
  std::vector<std::uint32_t>& entries_;
  llvm::Function* fn_ = nullptr;
  llvm::Value *ram_ = nullptr, *spr_ = nullptr, *rt_ = nullptr;
  std::map<std::uint32_t, llvm::BasicBlock*> blocks_;
  std::vector<std::uint32_t> pending_;

  const Insn& insn(std::uint32_t pc) const { return d_.insns[d_.index_of(pc)]; }
  bool inside(std::uint32_t pc) const { return pc >= f_.start && pc < f_.end && (d_.flags_at(pc) & kReached); }
  // A waiting loop, recognised with the reference core's own rule: one straight
  // run ending in beq/bne back to its start, made only of loads and simple
  // arithmetic, that redefines everything it compares. The reference core
  // skips the rest of its time slice in such a loop; translated code leaves
  // these loops to it rather than spinning.
  bool is_idle_loop(std::uint32_t head) const {
    for (std::uint32_t pc = head; pc < f_.end && inside(pc); pc += 4) {
      const Insn& in = insn(pc);
      if (in.flow == Flow::None) continue;
      if (in.flow != Flow::Branch || (in.op != Op::BEQ && in.op != Op::BNE)) return false;
      if (r5900::target_of(pc, in) != head || !inside(pc + 4)) return false;
      std::uint32_t defined = 0, used = 0;
      for (std::uint32_t a = head; a <= pc + 4; a += 4) {
        if (a == pc) continue;
        const Insn& i = insn(a);
        if (i.raw == 0) continue;
        std::uint32_t def = 0, use = 0;
        switch (i.op) {
          case Op::SLL: case Op::DSLL32: case Op::DSRL32:
            use = 1u << i.rt();
            def = 1u << i.rd();
            break;
          case Op::ADDU: case Op::SLT: case Op::SLTU:
            use = (1u << i.rs()) | (1u << i.rt());
            def = 1u << i.rd();
            break;
          case Op::LUI:
            def = 1u << i.rt();
            break;
          case Op::LW: case Op::LQ: case Op::LBU: case Op::ADDIU: case Op::SLTIU: case Op::XORI:
            use = 1u << i.rs();
            def = 1u << i.rt();
            break;
          default:
            return false;
        }
        use &= ~defined;
        used |= use;
        if (used & def) return false;
        defined |= def;
      }
      return (defined & ((1u << in.rs()) | (1u << in.rt()))) != 0;
    }
    return false;
  }

  // Does translated code entered at `pc` execute at least one instruction before it could stop?
  bool can_enter(std::uint32_t pc) const {
    if (is_idle_loop(pc) && !L_.yield_in_idle_loops) return false;
    const Insn& in = insn(pc);
    if (in.flow == Flow::None) return supported(in, L_.yield_in_idle_loops);
    if (in.flow == Flow::Syscall || in.flow == Flow::Break || in.flow == Flow::Eret || in.flow == Flow::BranchLink)
      return false;
    if (in.flow == Flow::Branch && !branch_supported(in)) return false;
    if (!inside(pc + 4)) return false;
    const Insn& slot = insn(pc + 4);
    return slot.flow == Flow::None && supported(slot, L_.yield_in_idle_loops);
  }

  llvm::BasicBlock* block(std::uint32_t pc) {
    auto [it, fresh] = blocks_.try_emplace(pc, nullptr);
    if (fresh) {
      it->second = llvm::BasicBlock::Create(ctx_, std::format("L{:08X}", pc), fn_);
      pending_.push_back(pc);
    }
    return it->second;
  }
  llvm::BasicBlock* fresh_block() { return llvm::BasicBlock::Create(ctx_, "", fn_); }

  // ---- state access ----
  std::uint32_t gpr(unsigned r, unsigned half = 0) const { return L_.gpr + r * L_.gpr_stride + half * 8; }

  llvm::Value* R(unsigned r) { return r == 0 ? static_cast<llvm::Value*>(b_.getInt64(0)) : ld(i64_, gpr(r)); }
  llvm::Value* RH(unsigned r) { return r == 0 ? static_cast<llvm::Value*>(b_.getInt64(0)) : ld(i64_, gpr(r, 1)); }
  llvm::Value* R32(unsigned r) { return b_.CreateTrunc(R(r), i32_); }
  void W(unsigned r, llvm::Value* v) {
    if (r != 0) st(v, gpr(r));
  }
  void WH(unsigned r, llvm::Value* v) {
    if (r != 0) st(v, gpr(r, 1));
  }
  void W32(unsigned r, llvm::Value* v32) {
    if (r != 0) W(r, b_.CreateSExt(v32, i64_));
  }
  llvm::Value* F(unsigned r) { return ld(i32_, L_.fpr + r * 4); }
  void WF(unsigned r, llvm::Value* bits) { st(bits, L_.fpr + r * 4); }
  void set_pc(llvm::Value* pc) { st(pc, L_.pc); }
  void set_pc(std::uint32_t pc) { set_pc(b_.getInt32(pc)); }

  llvm::Value* clamp(llvm::Value* bits) {
    bits = b_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, bits, b_.getInt32(0x7F7FFFFF));
    bits = b_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, bits, b_.getInt32(0xFF7FFFFFu));
    return asf(bits);
  }
  llvm::Value* CF(unsigned r) { return clamp(F(r)); }
  void set_cc(llvm::Value* cond) {
    llvm::Value* fcsr = ld(i32_, L_.fcsr);
    st(b_.CreateSelect(cond, b_.CreateOr(fcsr, b_.getInt32(0x00800000)), b_.CreateAnd(fcsr, b_.getInt32(~0x00800000u))),
       L_.fcsr);
  }
  llvm::Value* cc() { return b_.CreateICmpNE(b_.CreateAnd(ld(i32_, L_.fcsr), b_.getInt32(0x00800000)), b_.getInt32(0)); }

  // ---- leaving the function ----
  // Stops here; the reference core continues at `pc` with the current state.
  void stop(std::uint32_t pc, std::string_view why) {
    if (pc == f_.start) throw Unsupported{std::format("its first instruction would stop: {}", why)};
    ++stats_.stops;
    ++reasons_[std::string(why)];
    flush_flags(false);
    set_pc(pc);
    b_.CreateRetVoid();
  }
  void stop_dynamic(llvm::Value* pc) {
    flush_flags(false);
    set_pc(pc);
    b_.CreateRetVoid();
  }

  llvm::Value* runtime_fn(unsigned index) {
    llvm::Value* slot = b_.CreateConstInBoundsGEP1_32(b_.getPtrTy(), rt_, index);
    return b_.CreateLoad(b_.getPtrTy(), slot);
  }

  // ---- memory ----
  // Calls `fast(pointer)` for main memory and the scratchpad, `slow()` otherwise;
  // returns the merged value (or nullptr when `type` is null).
  template <class Fast, class Slow>
  llvm::Value* access(llvm::Value* a, unsigned size, llvm::Type* type, Fast fast, Slow slow) {
    auto* spr_bb = fresh_block();
    auto* chk_bb = fresh_block();
    auto* ram_bb = fresh_block();
    auto* slow_bb = fresh_block();
    auto* join = fresh_block();
    b_.CreateCondBr(b_.CreateICmpEQ(b_.CreateAnd(a, b_.getInt32(0xFFFFC000u)), b_.getInt32(0x70000000)), spr_bb, chk_bb);

    b_.SetInsertPoint(spr_bb);
    llvm::Value* v_spr =
        fast(b_.CreateInBoundsGEP(b_.getInt8Ty(), spr_, b_.CreateZExt(b_.CreateAnd(a, b_.getInt32(0x3FFF)), i64_)));
    auto* spr_end = b_.GetInsertBlock();
    b_.CreateBr(join);

    b_.SetInsertPoint(chk_bb);
    llvm::Value* fast30 = b_.CreateICmpULT(b_.CreateSub(a, b_.getInt32(0x30100000)), b_.getInt32(0x01F00000));
    llvm::Value* phys =
        b_.CreateSelect(fast30, b_.CreateSub(a, b_.getInt32(0x30000000)), b_.CreateAnd(a, b_.getInt32(0x1FFFFFFF)));
    b_.CreateCondBr(b_.CreateICmpULE(phys, b_.getInt32(ram_size_ - size)), ram_bb, slow_bb);

    b_.SetInsertPoint(ram_bb);
    llvm::Value* v_ram = fast(b_.CreateInBoundsGEP(b_.getInt8Ty(), ram_, b_.CreateZExt(phys, i64_)));
    auto* ram_end = b_.GetInsertBlock();
    b_.CreateBr(join);

    b_.SetInsertPoint(slow_bb);
    llvm::Value* v_slow = slow();
    auto* slow_end = b_.GetInsertBlock();
    b_.CreateBr(join);

    b_.SetInsertPoint(join);
    if (!type) return nullptr;
    auto* phi = b_.CreatePHI(type, 3);
    phi->addIncoming(v_spr, spr_end);
    phi->addIncoming(v_ram, ram_end);
    phi->addIncoming(v_slow, slow_end);
    return phi;
  }

  llvm::Value* slow_read(llvm::Value* a, unsigned size) {
    auto* type = llvm::FunctionType::get(i64_, {b_.getPtrTy(), i32_, i32_}, false);
    return b_.CreateCall(type, runtime_fn(0), {state_, a, b_.getInt32(size)});
  }
  void slow_write(llvm::Value* a, llvm::Value* v64, unsigned size) {
    auto* type = llvm::FunctionType::get(b_.getVoidTy(), {b_.getPtrTy(), i32_, i64_, i32_}, false);
    b_.CreateCall(type, runtime_fn(1), {state_, a, v64, b_.getInt32(size)});
  }

  // Loads `size` bytes (1, 2, 4, 8) as an integer of that width.
  llvm::Value* read(llvm::Value* a, unsigned size) {
    llvm::Type* t = b_.getIntNTy(size * 8);
    return access(
        a, size, t, [&](llvm::Value* p) { return static_cast<llvm::Value*>(b_.CreateAlignedLoad(t, p, llvm::MaybeAlign(1))); },
        [&]() { return b_.CreateTrunc(slow_read(a, size), t); });
  }
  // Stores; after a slow-path store that raised a pending exception, stops at
  // `next` (not inside a delay slot, where the store simply completes).
  void write(llvm::Value* a, llvm::Value* v, unsigned size, std::uint32_t next, bool in_delay_slot) {
    access(
        a, size, nullptr,
        [&](llvm::Value* p) {
          b_.CreateAlignedStore(v, p, llvm::MaybeAlign(1));
          return static_cast<llvm::Value*>(nullptr);
        },
        [&]() {
          slow_write(a, b_.CreateZExt(v, i64_), size);
          if (!in_delay_slot && inside(next)) {
            auto* raise = fresh_block();
            auto* go = fresh_block();
            b_.CreateCondBr(b_.CreateICmpNE(ld(i32_, L_.has_exception), b_.getInt32(0)), raise, go);
            b_.SetInsertPoint(raise);
            stop_dynamic(b_.getInt32(next));
            b_.SetInsertPoint(go);
          }
          return static_cast<llvm::Value*>(nullptr);
        });
  }
  llvm::Value* ea(const Insn& in) { return b_.CreateAdd(R32(in.rs()), b_.getInt32(static_cast<std::uint32_t>(in.simm()))); }

  // ---- control flow ----
  void run(std::uint32_t pc) {
    pipe_buffers_.clear();  // nothing buffered carries over from another path
    const std::uint32_t first = pc;
    for (;;) {
      if (!inside(pc)) {
        stop(pc, "code outside the analysed function");
        return;
      }
      if (pc != first && (d_.flags_at(pc) & kLeader)) {
        flush_flags(true);
        b_.CreateBr(block(pc));
        return;
      }
      const Insn& in = insn(pc);
      if (in.flow == Flow::None) {
        if (!supported(in, L_.yield_in_idle_loops)) {
          stop(pc, r5900::mnemonic(in.op));
          return;
        }
        emit(in, pc, false);
        pc += 4;
        continue;
      }
      control(pc, in);
      return;
    }
  }

  // Taken edge from the branch at `pc` to `target`. Backward edges inside the
  // function count against the time slice, so that a waiting loop gives the
  // rest of the machine a turn.
  void go_to(std::uint32_t pc, std::uint32_t target) {
    flush_flags(true);
    if (inside(target)) {
      if (target <= pc && is_idle_loop(target)) {
        stop_dynamic(b_.getInt32(target));
        return;
      }
      if (target <= pc) {
        const std::uint32_t cost = (pc - target) / 4 + 2;
        llvm::Value* q = b_.CreateSub(ld(i32_, L_.cycle_quota), b_.getInt32(cost));
        st(q, L_.cycle_quota);
        auto* out = fresh_block();
        b_.CreateCondBr(b_.CreateICmpSLE(q, b_.getInt32(0)), out, block(target));
        b_.SetInsertPoint(out);
        stop_dynamic(b_.getInt32(target));
      } else {
        b_.CreateBr(block(target));
      }
      return;
    }
    // Leaves the function: a tail call when it is a function start, otherwise
    // the reference core takes over there.
    set_pc(target);
    if (d_.flags_at(target) & kFuncStart) {
      call_function(target);
      ++stats_.calls;
    }
    b_.CreateRetVoid();
  }

  void call_function(std::uint32_t target) {
    flush_flags(true);
    b_.CreateCall(declare_function(m_, function_name(target)), {state_, ram_, spr_, rt_});
  }
  void call_dispatch() {
    flush_flags(true);
    b_.CreateCall(declare_function(m_, "hp2_dispatch"), {state_, ram_, spr_, rt_});
  }
  // After a call: continue at `back` if execution came back there, else stop
  // (the state is already current).
  void after_call(std::uint32_t back) {
    if (!inside(back)) {
      b_.CreateRetVoid();
      return;
    }
    auto* out = fresh_block();
    b_.CreateCondBr(b_.CreateICmpEQ(ld(i32_, L_.pc), b_.getInt32(back)), block(back), out);
    b_.SetInsertPoint(out);
    b_.CreateRetVoid();
  }
  llvm::Value* link_value(std::uint32_t pc) {
    return b_.getInt64(static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(pc + 8))));
  }

  void control(std::uint32_t pc, const Insn& in) {
    if (in.flow == Flow::Syscall || in.flow == Flow::Break || in.flow == Flow::Eret) {
      stop(pc, r5900::mnemonic(in.op));
      return;
    }
    if (!inside(pc + 4)) {
      stop(pc, "delay slot outside the function");
      return;
    }
    const Insn& slot = insn(pc + 4);
    // The compiler's division check: "beql divisor, zero, +1; break 7". The trap
    // sits in the delay slot of a likely branch, so it runs only when the branch
    // is taken; otherwise execution simply continues after it.
    if (in.flow == Flow::Branch && in.likely && branch_supported(in) && slot.flow == Flow::Break && pc != f_.start) {
      llvm::Value* c = condition(in);
      auto* trap = fresh_block();
      flush_flags(true);
      b_.CreateCondBr(c, trap, block(pc + 8));
      b_.SetInsertPoint(trap);
      stop(pc, "break");
      return;
    }
    if (slot.flow != Flow::None || !supported(slot, L_.yield_in_idle_loops)) {
      stop(pc, slot.flow != Flow::None ? "branch in a delay slot" : r5900::mnemonic(slot.op));
      return;
    }
    switch (in.flow) {
      case Flow::Branch: {
        if (!branch_supported(in)) {
          stop(pc, r5900::mnemonic(in.op));
          return;
        }
        const std::uint32_t target = r5900::target_of(pc, in);
        llvm::Value* c = condition(in);
        auto* taken = fresh_block();
        if (in.likely) {
          flush_flags(true);
          b_.CreateCondBr(c, taken, block(pc + 8));
          b_.SetInsertPoint(taken);
          emit(slot, pc + 4, true);
        } else {
          emit(slot, pc + 4, true);
          flush_flags(true);
          b_.CreateCondBr(c, taken, block(pc + 8));
          b_.SetInsertPoint(taken);
        }
        go_to(pc, target);
        return;
      }
      case Flow::Jump:
        emit(slot, pc + 4, true);
        go_to(pc, r5900::target_of(pc, in));
        return;
      case Flow::Call: {
        const std::uint32_t target = r5900::target_of(pc, in);
        W(31, link_value(pc));
        emit(slot, pc + 4, true);
        set_pc(target);
        call_function(target);
        ++stats_.calls;
        after_call(pc + 8);
        return;
      }
      case Flow::CallReg: {
        llvm::Value* target = R32(in.rs());
        W(in.rd(), link_value(pc));
        emit(slot, pc + 4, true);
        set_pc(target);
        call_dispatch();
        after_call(pc + 8);
        return;
      }
      case Flow::JumpReg: {
        llvm::Value* target = R32(in.rs());
        emit(slot, pc + 4, true);
        if (in.rs() == 31) {
          stop_dynamic(target);  // return
          return;
        }
        const JumpTable* table = nullptr;
        for (const JumpTable& t : d_.jump_tables)
          if (t.jump == pc) table = &t;
        flush_flags(true);
        if (table) {
          auto* other = fresh_block();
          auto* sw = b_.CreateSwitch(target, other);
          std::set<std::uint32_t> seen;
          for (std::uint32_t t : table->targets)
            if (seen.insert(t).second && inside(t)) sw->addCase(b_.getInt32(t), block(t));
          b_.SetInsertPoint(other);
        }
        set_pc(target);
        call_dispatch();
        b_.CreateRetVoid();
        return;
      }
      default:
        stop(pc, r5900::mnemonic(in.op));
        return;
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
      default: fail("unexpected branch {}", r5900::mnemonic(in.op));
    }
  }

  void cfc2(const Insn& in) {
    const unsigned rt = in.rt(), reg = in.rd();
    if (rt == 0) return;
    llvm::Value* v = nullptr;
    if (reg < 16) v = b_.CreateAnd(ld(i32_, L_.vi + reg * 4), b_.getInt32(0xFFFF));
    else if (reg == 17) { pipe_check(L_.pipe_mac, L_.vmf); v = ld(i32_, L_.vmf); }
    else if (reg == 18) { pipe_check(L_.pipe_clip, L_.vcf); v = ld(i32_, L_.vcf); }
    else if (reg == 20) v = ld(i32_, L_.vr);
    else if (reg == 21) v = ld(i32_, L_.vi_i);
    else v = ld(i32_, L_.vq);  // 22
    W(rt, b_.CreateSExt(v, i64_));
  }
  void ctc2(const Insn& in) {
    const unsigned reg = in.rd();
    llvm::Value* v = R32(in.rt());
    if (reg == 0 || reg == 17) return;
    if (reg < 16) { st(b_.CreateAnd(v, b_.getInt32(0xFFFF)), L_.vi + reg * 4); return; }
    if (reg == 18) {
      v = b_.CreateAnd(v, b_.getInt32(0xFFFFFF));
      st(v, L_.vcf);
      pipe_reset(L_.pipe_clip, v);
      return;
    }
    if (reg == 20) { st(b_.CreateAnd(v, b_.getInt32(0x7FFFFF)), L_.vr); return; }
    if (reg == 21) { st(v, L_.vi_i); return; }
    // 22: Q
    st(v, L_.pipe_q + 4);
    st(b_.getInt32(0), L_.pipe_q);
    st(v, L_.vq);
  }

  // ---- instructions ----
  llvm::Value* sx64(std::int64_t v) { return b_.getInt64(static_cast<std::uint64_t>(v)); }
  // 32-bit word i (0..3) of a 128-bit register.
  llvm::Value* word(unsigned r, int i) {
    llvm::Value* half = i < 2 ? R(r) : RH(r);
    return b_.CreateTrunc((i & 1) ? b_.CreateLShr(half, 32) : half, i32_);
  }
  llvm::Value* pair(llvm::Value* lo32, llvm::Value* hi32) {
    return b_.CreateOr(b_.CreateZExt(lo32, i64_), b_.CreateShl(b_.CreateZExt(hi32, i64_), 32));
  }

  void multiply(const Insn& in, bool sgn, bool second_unit, bool accumulate) {
    auto ext = [&](llvm::Value* v) { return sgn ? b_.CreateSExt(v, i64_) : b_.CreateZExt(v, i64_); };
    const std::uint32_t lo_off = second_unit ? L_.lo1 : L_.lo, hi_off = second_unit ? L_.hi1 : L_.hi;
    llvm::Value* p = b_.CreateMul(ext(R32(in.rs())), ext(R32(in.rt())));
    if (accumulate)
      p = b_.CreateAdd(p, pair(b_.CreateTrunc(ld(i64_, lo_off), i32_), b_.CreateTrunc(ld(i64_, hi_off), i32_)));
    llvm::Value* lo = b_.CreateSExt(b_.CreateTrunc(p, i32_), i64_);
    llvm::Value* hi = b_.CreateSExt(b_.CreateTrunc(b_.CreateLShr(p, 32), i32_), i64_);
    st(lo, lo_off);
    st(hi, hi_off);
    W(in.rd(), lo);
  }

  void divide(const Insn& in, bool sgn, bool second_unit) {
    llvm::Value* n = R32(in.rs());
    llvm::Value* dv = R32(in.rt());
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
    st(b_.CreateSExt(q, i64_), second_unit ? L_.lo1 : L_.lo);
    st(b_.CreateSExt(r, i64_), second_unit ? L_.hi1 : L_.hi);
  }

  // Unaligned word/doubleword loads and stores, as the reference core does them.
  void unaligned(const Insn& in, std::uint32_t pc, bool in_delay_slot) {
    const bool dbl = in.op == Op::LDL || in.op == Op::LDR || in.op == Op::SDL || in.op == Op::SDR;
    const unsigned size = dbl ? 8 : 4;
    llvm::Type* t = dbl ? i64_ : i32_;
    const std::uint64_t ones = dbl ? ~0ull : 0xFFFFFFFFull;
    const unsigned top = size - 1;
    llvm::Value* a = ea(in);
    llvm::Value* aligned = b_.CreateAnd(a, b_.getInt32(~top));
    llvm::Value* off = b_.CreateZExtOrTrunc(b_.CreateAnd(a, b_.getInt32(top)), t);
    llvm::Value* eight = llvm::ConstantInt::get(t, 8);
    llvm::Value* topv = llvm::ConstantInt::get(t, top);
    auto mask_right = [&](llvm::Value* k) {  // ones >> 8 >> (k * 8); zero for k == top
      return b_.CreateSelect(b_.CreateICmpEQ(k, topv), llvm::ConstantInt::get(t, 0),
                             b_.CreateLShr(llvm::ConstantInt::get(t, ones >> 8), b_.CreateMul(k, eight)));
    };
    auto mask_left = [&](llvm::Value* k) {  // ones << 8 << (k * 8); zero for k == top
      return b_.CreateSelect(b_.CreateICmpEQ(k, topv), llvm::ConstantInt::get(t, 0),
                             b_.CreateShl(llvm::ConstantInt::get(t, (ones << 8) & ones), b_.CreateMul(k, eight)));
    };
    llvm::Value* inv = b_.CreateSub(topv, off);
    llvm::Value* rt = dbl ? R(in.rt()) : R32(in.rt());
    llvm::Value* mem = read(aligned, size);
    llvm::Value* result = nullptr;
    switch (in.op) {
      case Op::LWL: case Op::LDL:
        result = b_.CreateOr(b_.CreateShl(mem, b_.CreateMul(inv, eight)), b_.CreateAnd(rt, mask_right(off)));
        break;
      case Op::LWR: case Op::LDR:
        result = b_.CreateOr(b_.CreateLShr(mem, b_.CreateMul(off, eight)), b_.CreateAnd(rt, mask_left(inv)));
        break;
      case Op::SWL: case Op::SDL:
        result = b_.CreateOr(b_.CreateLShr(rt, b_.CreateMul(inv, eight)), b_.CreateAnd(mem, mask_left(off)));
        break;
      default:  // SWR, SDR
        result = b_.CreateOr(b_.CreateShl(rt, b_.CreateMul(off, eight)), b_.CreateAnd(mem, mask_right(inv)));
        break;
    }
    if (in.op == Op::LWL || in.op == Op::LWR) W32(in.rt(), result);
    else if (in.op == Op::LDL || in.op == Op::LDR) W(in.rt(), result);
    else write(aligned, result, size, pc + 4, in_delay_slot);
  }

  void read128(llvm::Value* a, std::uint32_t lo_offset_dest, bool to_gpr, unsigned reg) {
    a = b_.CreateAnd(a, b_.getInt32(~15u));
    llvm::Value* lo = read(a, 8);
    llvm::Value* hi = read(b_.CreateAdd(a, b_.getInt32(8)), 8);
    if (to_gpr) {
      W(reg, lo);
      WH(reg, hi);
    } else {
      st(lo, lo_offset_dest);
      st(hi, lo_offset_dest + 8);
    }
  }
  void write128(llvm::Value* a, llvm::Value* lo, llvm::Value* hi, std::uint32_t next, bool in_delay_slot) {
    a = b_.CreateAnd(a, b_.getInt32(~15u));
    write(a, lo, 8, next, true);
    write(b_.CreateAdd(a, b_.getInt32(8)), hi, 8, next, in_delay_slot);
  }

  void emit(const Insn& in, std::uint32_t pc, bool in_delay_slot) {
    const unsigned rs = in.rs(), rt = in.rt(), rd = in.rd(), sa = in.sa();
    const unsigned fs = rd, ft = rt, fd = sa;
    const std::uint32_t next = pc + 4;
    switch (in.op) {
      // ---- shifts ----
      case Op::SLL: W32(rd, b_.CreateShl(R32(rt), sa)); break;
      case Op::SRL: W32(rd, b_.CreateLShr(R32(rt), sa)); break;
      case Op::SRA: W32(rd, b_.CreateAShr(R32(rt), sa)); break;
      case Op::SLLV: W32(rd, b_.CreateShl(R32(rt), b_.CreateAnd(R32(rs), 31))); break;
      case Op::SRLV: W32(rd, b_.CreateLShr(R32(rt), b_.CreateAnd(R32(rs), 31))); break;
      case Op::SRAV: W32(rd, b_.CreateAShr(R32(rt), b_.CreateAnd(R32(rs), 31))); break;
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
      case Op::ADD: case Op::ADDU: W32(rd, b_.CreateAdd(R32(rs), R32(rt))); break;
      case Op::SUB: case Op::SUBU: W32(rd, b_.CreateSub(R32(rs), R32(rt))); break;
      case Op::DADD: case Op::DADDU: W(rd, b_.CreateAdd(R(rs), R(rt))); break;
      case Op::DSUB: case Op::DSUBU: W(rd, b_.CreateSub(R(rs), R(rt))); break;
      case Op::ADDI: case Op::ADDIU: W32(rt, b_.CreateAdd(R32(rs), b_.getInt32(static_cast<std::uint32_t>(in.simm())))); break;
      case Op::DADDI: case Op::DADDIU: W(rt, b_.CreateAdd(R(rs), sx64(in.simm()))); break;
      case Op::AND: W(rd, b_.CreateAnd(R(rs), R(rt))); break;
      case Op::OR: W(rd, b_.CreateOr(R(rs), R(rt))); break;
      case Op::XOR: W(rd, b_.CreateXor(R(rs), R(rt))); break;
      case Op::NOR: W(rd, b_.CreateNot(b_.CreateOr(R(rs), R(rt)))); break;
      case Op::ANDI: W(rt, b_.CreateAnd(R(rs), b_.getInt64(in.imm()))); break;
      case Op::ORI: W(rt, b_.CreateOr(R(rs), b_.getInt64(in.imm()))); break;
      case Op::XORI: W(rt, b_.CreateXor(R(rs), b_.getInt64(in.imm()))); break;
      case Op::LUI: W(rt, sx64(static_cast<std::int32_t>(in.imm() << 16))); break;
      case Op::SLT: W(rd, b_.CreateZExt(b_.CreateICmpSLT(R(rs), R(rt)), i64_)); break;
      case Op::SLTU: W(rd, b_.CreateZExt(b_.CreateICmpULT(R(rs), R(rt)), i64_)); break;
      case Op::SLTI: W(rt, b_.CreateZExt(b_.CreateICmpSLT(R(rs), sx64(in.simm())), i64_)); break;
      case Op::SLTIU: W(rt, b_.CreateZExt(b_.CreateICmpULT(R(rs), sx64(in.simm())), i64_)); break;
      case Op::MOVZ: if (rd) W(rd, b_.CreateSelect(b_.CreateICmpEQ(R(rt), b_.getInt64(0)), R(rs), R(rd))); break;
      case Op::MOVN: if (rd) W(rd, b_.CreateSelect(b_.CreateICmpNE(R(rt), b_.getInt64(0)), R(rs), R(rd))); break;
      case Op::MFHI: W(rd, ld(i64_, L_.hi)); break;
      case Op::MFLO: W(rd, ld(i64_, L_.lo)); break;
      case Op::MTHI: st(R(rs), L_.hi); break;
      case Op::MTLO: st(R(rs), L_.lo); break;
      case Op::MFHI1: W(rd, ld(i64_, L_.hi1)); break;
      case Op::MFLO1: W(rd, ld(i64_, L_.lo1)); break;
      case Op::MTHI1: st(R(rs), L_.hi1); break;
      case Op::MTLO1: st(R(rs), L_.lo1); break;
      case Op::MULT: multiply(in, true, false, false); break;
      case Op::MULTU: multiply(in, false, false, false); break;
      case Op::MULT1: multiply(in, true, true, false); break;
      case Op::MULTU1: multiply(in, false, true, false); break;
      case Op::MADD: multiply(in, true, false, true); break;
      case Op::MADDU: multiply(in, false, false, true); break;
      case Op::MADD1: multiply(in, true, true, true); break;
      case Op::MADDU1: multiply(in, false, true, true); break;
      case Op::DIV: divide(in, true, false); break;
      case Op::DIVU: divide(in, false, false); break;
      case Op::DIV1: divide(in, true, true); break;
      case Op::DIVU1: divide(in, false, true); break;
      // ---- memory ----
      case Op::LB: if (rt) W(rt, b_.CreateSExt(read(ea(in), 1), i64_)); break;
      case Op::LBU: if (rt) W(rt, b_.CreateZExt(read(ea(in), 1), i64_)); break;
      case Op::LH: if (rt) W(rt, b_.CreateSExt(read(ea(in), 2), i64_)); break;
      case Op::LHU: if (rt) W(rt, b_.CreateZExt(read(ea(in), 2), i64_)); break;
      case Op::LW: if (rt) W(rt, b_.CreateSExt(read(ea(in), 4), i64_)); break;
      case Op::LWU: if (rt) W(rt, b_.CreateZExt(read(ea(in), 4), i64_)); break;
      case Op::LD: if (rt) W(rt, read(ea(in), 8)); break;
      case Op::LQ: if (rt) read128(ea(in), 0, true, rt); break;
      case Op::SB: write(ea(in), b_.CreateTrunc(R(rt), b_.getInt8Ty()), 1, next, in_delay_slot); break;
      case Op::SH: write(ea(in), b_.CreateTrunc(R(rt), b_.getInt16Ty()), 2, next, in_delay_slot); break;
      case Op::SW: write(ea(in), R32(rt), 4, next, in_delay_slot); break;
      case Op::SD: write(ea(in), R(rt), 8, next, in_delay_slot); break;
      case Op::SQ: { auto* lo = R(rt); auto* hi = RH(rt); write128(ea(in), lo, hi, next, in_delay_slot); break; }
      case Op::LWL: case Op::LWR: case Op::LDL: case Op::LDR: case Op::SWL: case Op::SWR: case Op::SDL: case Op::SDR:
        unaligned(in, pc, in_delay_slot);
        break;
      case Op::LWC1: WF(ft, read(ea(in), 4)); break;
      case Op::SWC1: write(ea(in), F(ft), 4, next, in_delay_slot); break;
      case Op::LQC2: if (ft) read128(ea(in), L_.vf + ft * 16, false, 0); break;
      case Op::SQC2: {
        llvm::Value* lo = ld(i64_, L_.vf + ft * 16);
        llvm::Value* hi = ld(i64_, L_.vf + ft * 16 + 8);
        write128(ea(in), lo, hi, next, in_delay_slot);
        break;
      }
      case Op::QMFC2: {
        llvm::Value* lo = ld(i64_, L_.vf + rd * 16);
        llvm::Value* hi = ld(i64_, L_.vf + rd * 16 + 8);
        W(rt, lo);
        WH(rt, hi);
        break;
      }
      case Op::QMTC2:
        if (rd) {
          llvm::Value* lo = R(rt);
          llvm::Value* hi = RH(rt);
          st(lo, L_.vf + rd * 16);
          st(hi, L_.vf + rd * 16 + 8);
        }
        break;
      case Op::SYNC: case Op::PREF: case Op::CACHE: break;
      // ---- FPU ----
      case Op::MFC1: W(rt, b_.CreateSExt(F(fs), i64_)); break;
      case Op::MTC1: WF(fs, R32(rt)); break;
      case Op::CFC1:
        W(rt, fs < 16 ? static_cast<llvm::Value*>(b_.getInt64(0x2E30)) : b_.CreateSExt(ld(i32_, L_.fcsr), i64_));
        break;
      case Op::CTC1:
        if (fs == 31) {
          constexpr std::uint32_t mask = 0x0083C078;
          st(b_.CreateOr(b_.CreateAnd(R32(rt), b_.getInt32(mask)), b_.CreateAnd(ld(i32_, L_.fcsr), b_.getInt32(~mask))),
             L_.fcsr);
        }
        break;
      case Op::ADD_S: WF(fd, asi(b_.CreateFAdd(CF(fs), CF(ft)))); break;
      case Op::SUB_S: WF(fd, asi(b_.CreateFSub(CF(fs), CF(ft)))); break;
      case Op::MUL_S: WF(fd, asi(b_.CreateFMul(CF(fs), CF(ft)))); break;
      case Op::DIV_S: case Op::RSQRT_S: {
        llvm::Value* a = F(fs);
        llvm::Value* bb = F(ft);
        llvm::Value* zero = b_.CreateICmpEQ(b_.CreateAnd(bb, b_.getInt32(0x7F800000)), b_.getInt32(0));
        llvm::Value* inf =
            b_.CreateOr(b_.getInt32(0x7F7FFFFF), b_.CreateAnd(b_.CreateXor(a, bb), b_.getInt32(0x80000000u)));
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
      case Op::ADDA_S: st(asi(b_.CreateFAdd(CF(fs), CF(ft))), L_.fpacc); break;
      case Op::SUBA_S: st(asi(b_.CreateFSub(CF(fs), CF(ft))), L_.fpacc); break;
      case Op::MULA_S: st(asi(b_.CreateFMul(CF(fs), CF(ft))), L_.fpacc); break;
      case Op::MADD_S: case Op::MSUB_S: case Op::MADDA_S: case Op::MSUBA_S: {
        llvm::Value* acc = clamp(ld(i32_, L_.fpacc));
        llvm::Value* prod = b_.CreateFMul(CF(fs), CF(ft));
        const bool add = in.op == Op::MADD_S || in.op == Op::MADDA_S;
        llvm::Value* r = asi(add ? b_.CreateFAdd(acc, prod) : b_.CreateFSub(acc, prod));
        if (in.op == Op::MADD_S || in.op == Op::MSUB_S) WF(fd, r);
        else st(r, L_.fpacc);
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
      case Op::POR: { auto* lo = b_.CreateOr(R(rs), R(rt)); auto* hi = b_.CreateOr(RH(rs), RH(rt)); W(rd, lo); WH(rd, hi); break; }
      case Op::PAND: { auto* lo = b_.CreateAnd(R(rs), R(rt)); auto* hi = b_.CreateAnd(RH(rs), RH(rt)); W(rd, lo); WH(rd, hi); break; }
      case Op::PXOR: { auto* lo = b_.CreateXor(R(rs), R(rt)); auto* hi = b_.CreateXor(RH(rs), RH(rt)); W(rd, lo); WH(rd, hi); break; }
      case Op::PNOR: {
        auto* lo = b_.CreateNot(b_.CreateOr(R(rs), R(rt)));
        auto* hi = b_.CreateNot(b_.CreateOr(RH(rs), RH(rt)));
        W(rd, lo);
        WH(rd, hi);
        break;
      }
      case Op::PCPYLD: { auto* lo = R(rt); auto* hi = R(rs); W(rd, lo); WH(rd, hi); break; }
      case Op::PCPYUD: { auto* lo = RH(rs); auto* hi = RH(rt); W(rd, lo); WH(rd, hi); break; }
      case Op::PEXTLW: case Op::PEXTUW: {
        const int base = in.op == Op::PEXTLW ? 0 : 2;
        llvm::Value* lo = pair(word(rt, base), word(rs, base));
        llvm::Value* hi = pair(word(rt, base + 1), word(rs, base + 1));
        W(rd, lo);
        WH(rd, hi);
        break;
      }
      case Op::PADDW: case Op::PSUBW: {
        llvm::Value* w[4];
        for (int i = 0; i < 4; ++i)
          w[i] = in.op == Op::PADDW ? b_.CreateAdd(word(rs, i), word(rt, i)) : b_.CreateSub(word(rs, i), word(rt, i));
        llvm::Value* lo = pair(w[0], w[1]);
        llvm::Value* hi = pair(w[2], w[3]);
        W(rd, lo);
        WH(rd, hi);
        break;
      }
      case Op::PMULTH: {
        llvm::Value* low[4];
        llvm::Value* high[4];
        auto s16 = [&](llvm::Value* v) { return b_.CreateSExt(b_.CreateTrunc(v, b_.getInt16Ty()), i32_); };
        for (int i = 0; i < 4; ++i) {
          llvm::Value* a = word(rs, i);
          llvm::Value* c = word(rt, i);
          low[i] = b_.CreateMul(s16(a), s16(c));
          high[i] = b_.CreateMul(b_.CreateAShr(a, 16), b_.CreateAShr(c, 16));
        }
        st(pair(low[0], high[0]), L_.lo);
        st(pair(low[1], high[1]), L_.hi);
        st(pair(low[2], high[2]), L_.lo1);
        st(pair(low[3], high[3]), L_.hi1);
        if (rd) {
          W(rd, pair(low[0], low[1]));
          WH(rd, pair(low[2], low[3]));
        }
        break;
      }
      case Op::EI: case Op::DI: case Op::MFC0: case Op::MTC0: {
        // Native runtime only: system instructions are carried out by the runtime.
        auto* type = llvm::FunctionType::get(b_.getVoidTy(), {b_.getPtrTy(), i32_}, false);
        b_.CreateCall(type, runtime_fn(2), {state_, b_.getInt32(in.raw)});
        break;
      }
      case Op::CFC2: cfc2(in); break;
      case Op::CTC2: ctc2(in); break;
      default:
        if (!emit_vu(in)) fail("no translation for {}", r5900::mnemonic(in.op));
    }
  }
};

}  // namespace

struct Translator::Impl {
  // Functions are spread over several modules (each with its own context) so
  // that they can be optimised and compiled in parallel.
  struct Shard {
    std::unique_ptr<llvm::LLVMContext> context = std::make_unique<llvm::LLVMContext>();
    std::unique_ptr<llvm::Module> module;
  };
  const ElfImage& elf;
  const Discovery& d;
  StateLayout layout;
  std::uint32_t ram_size;
  unsigned threads = 0;
  std::vector<Shard> shards;
  bool finalised = false;
  std::unique_ptr<llvm::orc::LLJIT> jit;
  std::set<std::uint32_t> added;
  std::map<std::uint32_t, LiftedFunction> code;
  std::map<std::uint32_t, std::uint32_t> entry_function;  // entry other than a start -> its function's start
  TranslateStats stats;
  std::map<std::string, std::size_t> reasons;

  // Ends translation: the shared dispatcher, bodies for calls to functions that were not translated.
  void finalise() {
    if (finalised) fail("the translator was already compiled");
    finalised = true;
    // Calls through a pointer: a switch over every translated function (in the first module).
    {
      llvm::Module& mod = *shards[0].module;
      llvm::LLVMContext& ctx = *shards[0].context;
      llvm::IRBuilder<> b(ctx);
      llvm::Function* dispatch = declare_function(mod, "hp2_dispatch");
      auto* entry = llvm::BasicBlock::Create(ctx, "entry", dispatch);
      auto* none = llvm::BasicBlock::Create(ctx, "none", dispatch);
      b.SetInsertPoint(none);
      b.CreateRetVoid();
      b.SetInsertPoint(entry);
      llvm::Value* pc =
          b.CreateLoad(b.getInt32Ty(), b.CreateConstInBoundsGEP1_32(b.getInt8Ty(), dispatch->getArg(0), layout.pc));
      auto* sw = b.CreateSwitch(pc, none);
      for (std::uint32_t start : added) {
        auto* bb = llvm::BasicBlock::Create(ctx, "", dispatch);
        sw->addCase(b.getInt32(start), bb);
        b.SetInsertPoint(bb);
        b.CreateCall(declare_function(mod, function_name(start)),
                     {dispatch->getArg(0), dispatch->getArg(1), dispatch->getArg(2), dispatch->getArg(3)});
        b.CreateRetVoid();
      }
    }
    // Calls to functions that were not translated: a body that returns at once,
    // leaving the program counter on the callee, so that the caller stops there.
    for (Shard& s : shards) {
      llvm::IRBuilder<> b(*s.context);
      for (llvm::Function& fn : *s.module)
        if (fn.isDeclaration() && fn.getName().starts_with("fn_") &&
            !added.count(static_cast<std::uint32_t>(std::stoul(fn.getName().substr(3).str(), nullptr, 16)))) {
          b.SetInsertPoint(llvm::BasicBlock::Create(*s.context, "untranslated", &fn));
          b.CreateRetVoid();
          fn.setLinkage(llvm::Function::InternalLinkage);
        }
    }
  }

  std::vector<llvm::Module*> modules() {
    std::vector<llvm::Module*> list;
    for (Shard& s : shards) list.push_back(s.module.get());
    return list;
  }
};

Translator::Translator(const ElfImage& elf, const Discovery& discovery, const StateLayout& layout,
                       std::uint32_t ram_size, unsigned threads)
    : impl_(new Impl{elf, discovery, layout, ram_size}) {
  initialise_llvm();
  if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
  impl_->threads = threads;
  const auto target = make_target("");
  impl_->shards.resize(threads);
  for (std::size_t i = 0; i < impl_->shards.size(); ++i) {
    auto& s = impl_->shards[i];
    s.module = std::make_unique<llvm::Module>(std::format("hp2_{}", i), *s.context);
    s.module->setDataLayout(target->createDataLayout());
    s.module->setTargetTriple(target->getTargetTriple());
  }
}

Translator::~Translator() = default;

std::optional<std::string> Translator::add_function(std::uint32_t start) {
  Impl& m = *impl_;
  if (m.finalised) return std::string("the translator was already compiled");
  const Function* f = m.d.function_at(start);
  if (!f || f->start != start) return std::format("{:08X} is not a function start", start);
  if (m.added.count(start)) return std::nullopt;
  Impl::Shard& shard = m.shards[m.added.size() % m.shards.size()];
  // Try in a scratch module first, so that a refusal leaves nothing behind.
  TranslateStats stats;
  std::map<std::string, std::size_t> reasons;
  std::vector<std::uint32_t> entries;
  {
    llvm::Module scratch("scratch", *shard.context);
    FunctionLifter probe(m.d, m.layout, m.ram_size, scratch, *f, stats, reasons, entries);
    try {
      probe.lift();
    } catch (const Unsupported& u) {
      return u.why;
    }
  }
  stats = {};
  reasons.clear();
  entries.clear();
  FunctionLifter lifter(m.d, m.layout, m.ram_size, *shard.module, *f, stats, reasons, entries);
  lifter.lift();
  for (std::uint32_t e : entries) m.entry_function.emplace(e, start);
  m.stats.instructions += stats.instructions;
  m.stats.stops += stats.stops;
  m.stats.calls += stats.calls;
  for (const auto& [k, n] : reasons) m.reasons[k] += n;
  m.added.insert(start);
  return std::nullopt;
}

void Translator::compile() {
  Impl& m = *impl_;
  m.finalise();
  const std::vector<std::vector<char>> objects = compile_modules(m.modules(), "", m.threads);
  auto builder = check(llvm::orc::JITTargetMachineBuilder::detectHost(), "detecting the host CPU");
  m.jit = check(llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(builder).create(), "creating the JIT");
  for (std::size_t i = 0; i < objects.size(); ++i)
    check(m.jit->addObjectFile(llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(objects[i].data(), objects[i].size()),
                                                                     std::format("hp2_{}", i))),
          "adding translated code");
  for (std::uint32_t start : m.added) {
    auto address = check(m.jit->lookup(function_name(start)), "looking up a function");
    m.code.emplace(start, address.toPtr<LiftedFunction>());
  }
  m.shards.clear();
}

std::vector<std::vector<char>> Translator::emit_objects(const std::string& cpu) {
  Impl& m = *impl_;
  m.finalise();
  auto objects = compile_modules(m.modules(), cpu, m.threads);
  m.shards.clear();
  return objects;
}

std::string Translator::symbol(std::uint32_t start) {
  return function_name(start);
}

const std::set<std::uint32_t>& Translator::functions() const {
  return impl_->added;
}

const std::map<std::uint32_t, std::uint32_t>& Translator::entry_functions() const {
  return impl_->entry_function;
}

LiftedFunction Translator::lookup(std::uint32_t start) const {
  const auto it = impl_->code.find(start);
  return it == impl_->code.end() ? nullptr : it->second;
}

LiftedFunction Translator::lookup_entry(std::uint32_t address) const {
  if (LiftedFunction f = lookup(address)) return f;
  const auto it = impl_->entry_function.find(address);
  return it == impl_->entry_function.end() ? nullptr : lookup(it->second);
}

std::size_t Translator::entries() const {
  return impl_->entry_function.size();
}

std::string Translator::ir() const {
  std::string text;
  llvm::raw_string_ostream out(text);
  for (const auto& s : impl_->shards) s.module->print(out, nullptr);
  return text;
}

std::size_t Translator::size() const {
  return impl_->added.size();
}

const TranslateStats& Translator::stats() const {
  return impl_->stats;
}

const std::map<std::string, std::size_t>& Translator::stop_reasons() const {
  return impl_->reasons;
}

}  // namespace hp2
