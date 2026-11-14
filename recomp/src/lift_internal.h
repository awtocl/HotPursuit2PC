// Internal to the translator: pieces shared by the game-program translator
// (lift.cpp) and the VU1 microcode translator (vulift.cpp).
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "hp2/lift.h"

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

namespace hp2::detail {

using r5900::Insn;
using r5900::Op;

inline void check(llvm::Error error, const char* what) {
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

// ---- code generation (codegen.cpp) ----
void initialise_llvm();
// `cpu` empty: this machine's CPU (in-process use); otherwise an LLVM CPU name such as "x86-64-v3".
std::unique_ptr<llvm::TargetMachine> make_target(const std::string& cpu);
void optimise(llvm::Module& module, llvm::TargetMachine* target);  // -O2
// Optimises each module and generates its object code, modules in parallel
// (up to `threads` at once; 0 = one per hardware thread). The modules must
// each have their own context.
std::vector<std::vector<char>> compile_modules(const std::vector<llvm::Module*>& modules, const std::string& cpu,
                                               unsigned threads);

// Code generation for the vector unit's floating-point side (VU0 macro mode
// and the upper words of VU microcode), on a state structure in the
// reference core's layout.
class VectorCode {
 public:
  VectorCode(llvm::LLVMContext& ctx, const StateLayout& layout)
      : ctx_(ctx), b_(ctx), L_(layout), i64_(b_.getInt64Ty()), i32_(b_.getInt32Ty()), f32_(b_.getFloatTy()) {}

 protected:
  llvm::LLVMContext& ctx_;
  llvm::IRBuilder<> b_;
  const StateLayout& L_;
  llvm::Value* state_ = nullptr;
  llvm::Type *i64_ = nullptr, *i32_ = nullptr, *f32_ = nullptr;
  // VU1 (ADR 0008): no MAC/sticky flags; flag values queued at once; pipeline
  // times relative to the start of the current block.
  bool mac_flags_ = true;
  bool buffer_flags_ = true;
  std::uint32_t rel_ = 0;

  // ---- state access ----
  llvm::Value* field(std::uint32_t offset) { return b_.CreateConstInBoundsGEP1_32(b_.getInt8Ty(), state_, offset); }
  llvm::Value* ld(llvm::Type* t, std::uint32_t offset) { return b_.CreateLoad(t, field(offset)); }
  void st(llvm::Value* v, std::uint32_t offset) { b_.CreateStore(v, field(offset)); }
  llvm::Value* asf(llvm::Value* bits) { return b_.CreateBitCast(bits, f32_); }
  llvm::Value* asi(llvm::Value* f) { return b_.CreateBitCast(f, i32_); }

  // ---- VU0 macro mode ----
  // Follows the reference core's VUShared/COP_VU code: which operands are
  // clamped depends on the operation; MAC/sticky/clip flags go through
  // 8-slot pipelines with a latency of 4 (the EE's pipeTime does not advance,
  // so a queued value becomes visible to the next cfc2); Q is written to a
  // held value that vwaitq (or ctc2) moves into Q.
  static constexpr unsigned kLatencyMac = 4;
  static constexpr unsigned kPipeValues = 4, kPipeTimes = 4 + 32;  // offsets inside a flag pipeline

  llvm::Type* v4f() { return llvm::FixedVectorType::get(f32_, 4); }
  llvm::Type* v4i() { return llvm::FixedVectorType::get(i32_, 4); }
  std::uint32_t vfo(unsigned r, unsigned lane = 0) const { return L_.vf + r * 16 + lane * 4; }
  llvm::Value* splatf(llvm::Value* f) { return b_.CreateVectorSplat(4, f); }
  llvm::Value* splatf(float f) { return splatf(llvm::ConstantFP::get(f32_, f)); }
  llvm::Value* splati(std::uint32_t v) { return b_.CreateVectorSplat(4, b_.getInt32(v)); }
  llvm::Value* vf(unsigned r) {
    if (r == 0) {
      return llvm::ConstantVector::get({llvm::ConstantFP::get(f32_, 0.0), llvm::ConstantFP::get(f32_, 0.0),
                                        llvm::ConstantFP::get(f32_, 0.0), llvm::ConstantFP::get(f32_, 1.0)});
    }
    return b_.CreateAlignedLoad(v4f(), field(vfo(r)), llvm::MaybeAlign(4));
  }
  llvm::Value* acc() { return b_.CreateAlignedLoad(v4f(), field(L_.vacc), llvm::MaybeAlign(4)); }
  llvm::Value* lane(unsigned r, unsigned i) {
    if (r == 0) return llvm::ConstantFP::get(f32_, i == 3 ? 1.0 : 0.0);
    return ld(f32_, vfo(r, i));
  }
  // Stores the lanes selected by `dest` (bit 3 = x ... bit 0 = w) into the vector at `offset`.
  void vstore_at(std::uint32_t offset, llvm::Value* v, unsigned dest) {
    if (dest == 0) return;
    if (dest == 0xF) {
      b_.CreateAlignedStore(v, field(offset), llvm::MaybeAlign(4));
      return;
    }
    llvm::Value* old = b_.CreateAlignedLoad(v4f(), field(offset), llvm::MaybeAlign(4));
    std::vector<llvm::Constant*> mask;
    for (unsigned i = 0; i < 4; ++i) mask.push_back(b_.getInt1((dest >> (3 - i)) & 1));
    b_.CreateAlignedStore(b_.CreateSelect(llvm::ConstantVector::get(mask), v, old), field(offset), llvm::MaybeAlign(4));
  }
  void vstore(unsigned r, llvm::Value* v, unsigned dest) {
    if (r != 0) vstore_at(vfo(r), v, dest);
  }
  llvm::Value* vclamp(llvm::Value* v) {
    llvm::Value* bits = b_.CreateBitCast(v, v4i());
    bits = b_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, bits, splati(0x7F7FFFFF));
    bits = b_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, bits, splati(0xFF7FFFFFu));
    return b_.CreateBitCast(bits, v4f());
  }

  // ---- flag pipelines ----
  llvm::Value* pipe_slot(std::uint32_t pipe, unsigned array, llvm::Value* index) {
    llvm::Value* off = b_.CreateAdd(b_.getInt32(pipe + array), b_.CreateShl(index, 2));
    return b_.CreateInBoundsGEP(b_.getInt8Ty(), state_, b_.CreateZExt(off, i64_));
  }
  // Values queued into a flag pipeline during the current straight run are
  // kept here and written at its end (or before anything that could look at
  // them): of more than 8 queued values only the last 8 are ever visible, so
  // the others are never stored. The resulting state is the same.
  struct PipeBuffer {
    llvm::Value* index = nullptr;  // pipeline index when the run began queuing
    std::vector<llvm::Value*> values;
  };
  std::map<std::uint32_t, PipeBuffer> pipe_buffers_;

  PipeBuffer& buffer(std::uint32_t pipe) { return pipe_buffers_[pipe]; }
  void flush_flags(bool clear) {
    for (auto& [pipe, buf] : pipe_buffers_) {
      if (buf.values.empty()) continue;
      const std::size_t n = buf.values.size();
      llvm::Value* time = b_.CreateAdd(ld(i32_, L_.pipe_time), b_.getInt32(kLatencyMac));
      for (std::size_t j = n > 8 ? n - 8 : 0; j < n; ++j) {
        llvm::Value* index = b_.CreateAnd(b_.CreateAdd(buf.index, b_.getInt32(static_cast<std::uint32_t>(j))), b_.getInt32(7));
        b_.CreateStore(time, pipe_slot(pipe, kPipeTimes, index));
        b_.CreateStore(buf.values[j], pipe_slot(pipe, kPipeValues, index));
      }
      st(b_.CreateAnd(b_.CreateAdd(buf.index, b_.getInt32(static_cast<std::uint32_t>(n))), b_.getInt32(7)), pipe);
    }
    if (clear) pipe_buffers_.clear();
  }

  llvm::Value* pipe_last(std::uint32_t pipe) {
    PipeBuffer& buf = buffer(pipe);
    if (!buf.values.empty()) return buf.values.back();
    llvm::Value* index = b_.CreateAnd(b_.CreateSub(ld(i32_, pipe), b_.getInt32(1)), b_.getInt32(7));
    return b_.CreateLoad(i32_, pipe_slot(pipe, kPipeValues, index));
  }
  void pipe_queue(std::uint32_t pipe, llvm::Value* value) {
    PipeBuffer& buf = buffer(pipe);
    if (buf.values.empty()) buf.index = ld(i32_, pipe);
    buf.values.push_back(value);
  }
  // Clip flags: VU0 buffers them per straight run (see PipeBuffer); VU1 queues
  // each value at once, due `rel_` pipeline cycles into the block plus the latency.
  void queue_clip(llvm::Value* value) {
    if (buffer_flags_) {
      pipe_queue(L_.pipe_clip, value);
      return;
    }
    llvm::Value* index = ld(i32_, L_.pipe_clip);
    llvm::Value* time = b_.CreateAdd(ld(i32_, L_.pipe_time), b_.getInt32(rel_ + kLatencyMac));
    b_.CreateStore(time, pipe_slot(L_.pipe_clip, kPipeTimes, index));
    b_.CreateStore(value, pipe_slot(L_.pipe_clip, kPipeValues, index));
    st(b_.CreateAnd(b_.CreateAdd(index, b_.getInt32(1)), b_.getInt32(7)), L_.pipe_clip);
  }
  // CheckFlagPipeline: every slot whose time has come updates the flag register, in pipeline order.
  void pipe_check(std::uint32_t pipe, std::uint32_t value_offset) {
    flush_flags(true);
    llvm::Value* now = b_.CreateAdd(ld(i32_, L_.pipe_time), b_.getInt32(kLatencyMac));
    llvm::Value* value = ld(i32_, value_offset);
    llvm::Value* base = ld(i32_, pipe);
    for (unsigned i = 0; i < 8; ++i) {
      llvm::Value* index = b_.CreateAnd(b_.CreateAdd(base, b_.getInt32(i)), b_.getInt32(7));
      llvm::Value* due = b_.CreateICmpSLE(b_.CreateLoad(i32_, pipe_slot(pipe, kPipeTimes, index)), now);
      value = b_.CreateSelect(due, b_.CreateLoad(i32_, pipe_slot(pipe, kPipeValues, index)), value);
    }
    st(value, value_offset);
  }
  void pipe_reset(std::uint32_t pipe, llvm::Value* value) {
    flush_flags(true);
    for (unsigned i = 0; i < 8; ++i) {
      st(b_.getInt32(0), pipe + kPipeTimes + i * 4);
      st(value, pipe + kPipeValues + i * 4);
    }
  }
  // Sign and zero flags of the lanes in `dest` (S: bits 7..4 = x..w, Z: bits 3..0 = x..w).
  void sz_flags(llvm::Value* result, unsigned dest) {
    if (!mac_flags_) return;  // VU1: never read (ADR 0008)
    llvm::Value* bits = b_.CreateBitCast(result, v4i());
    llvm::Value* zero = b_.CreateFCmpOEQ(result, splatf(0.0f));
    llvm::Value* flags = b_.getInt32(0);
    for (unsigned i = 0; i < 4; ++i) {
      llvm::Value* s = b_.CreateLShr(b_.CreateExtractElement(bits, i), 31);
      llvm::Value* z = b_.CreateZExt(b_.CreateExtractElement(zero, i), i32_);
      flags = b_.CreateOr(flags, b_.CreateOr(b_.CreateShl(s, 7 - i), b_.CreateShl(z, 3 - i)));
    }
    flags = b_.CreateAnd(flags, b_.getInt32((dest << 4) | dest));
    pipe_queue(L_.pipe_sticky, b_.CreateOr(pipe_last(L_.pipe_sticky), flags));
    pipe_queue(L_.pipe_mac, flags);
  }

  enum class FKind { Add, Sub, Mul, Madd, Msub };
  enum class FSrc { Vec, Bc, I, Q };

  void fmac(const Insn& in, FKind kind, FSrc src, bool to_acc) {
    const unsigned raw = in.raw;
    const unsigned dest = (raw >> 21) & 15, ft = (raw >> 16) & 31, fs = (raw >> 11) & 31, fd = (raw >> 6) & 31,
                   bc = raw & 3;
    llvm::Value* result = nullptr;
    if (kind == FKind::Add && src == FSrc::Bc && !to_acc && fd != 0 && ft == 0 && bc != 3) {
      result = vf(fs);  // adding vf0's zero component: a plain move in the reference core
    } else if (kind == FKind::Sub && src == FSrc::Vec && !to_acc && fs == ft) {
      result = splatf(0.0f);  // the reference core clears instead of subtracting
    } else {
      bool clamp_fs = false, clamp_ft = false;
      switch (kind) {
        case FKind::Add: clamp_fs = !to_acc && src != FSrc::I; break;
        case FKind::Sub: clamp_fs = clamp_ft = !to_acc; break;
        case FKind::Mul: clamp_fs = true; clamp_ft = !to_acc; break;
        case FKind::Madd: clamp_fs = true; clamp_ft = to_acc || src != FSrc::Vec; break;
        case FKind::Msub: break;
      }
      llvm::Value* a = vf(fs);
      if (clamp_fs) a = vclamp(a);
      llvm::Value* b = nullptr;
      switch (src) {
        case FSrc::Vec: b = vf(ft); break;
        case FSrc::Bc: b = splatf(lane(ft, bc)); break;
        case FSrc::I: b = splatf(ld(f32_, L_.vi_i)); break;
        case FSrc::Q: b = splatf(ld(f32_, L_.vq)); break;
      }
      if (clamp_ft) b = vclamp(b);
      switch (kind) {
        case FKind::Add: result = b_.CreateFAdd(a, b); break;
        case FKind::Sub: result = b_.CreateFSub(a, b); break;
        case FKind::Mul: result = b_.CreateFMul(a, b); break;
        case FKind::Madd: result = b_.CreateFAdd(acc(), b_.CreateFMul(a, b)); break;
        case FKind::Msub: result = b_.CreateFSub(acc(), b_.CreateFMul(a, b)); break;
      }
    }
    if (to_acc) vstore_at(L_.vacc, result, dest);
    else vstore(fd, result, dest);
    sz_flags(result, dest);
  }

  // MAX/MINI compare float bit patterns as ordered integers (the reference core's MakeComparableFromFloat).
  llvm::Value* comparable(llvm::Value* v) {
    llvm::Value* bits = b_.CreateBitCast(v, v4i());
    llvm::Value* neg = b_.CreateICmpSLT(bits, splati(0));
    return b_.CreateSelect(neg, b_.CreateSub(splati(0xFFFFFFFFu), bits), b_.CreateAdd(splati(0x7FFFFFFF), bits));
  }
  void maxmin(const Insn& in, bool is_max, FSrc src) {
    const unsigned raw = in.raw;
    const unsigned dest = (raw >> 21) & 15, ft = (raw >> 16) & 31, fs = (raw >> 11) & 31, fd = (raw >> 6) & 31,
                   bc = raw & 3;
    llvm::Value* a = vf(fs);
    llvm::Value* b = src == FSrc::Vec ? vf(ft) : src == FSrc::Bc ? splatf(lane(ft, bc)) : splatf(ld(f32_, L_.vi_i));
    llvm::Value* pick_a = is_max ? b_.CreateICmpUGT(comparable(a), comparable(b)) : b_.CreateICmpUGT(comparable(b), comparable(a));
    vstore(fd, b_.CreateSelect(pick_a, a, b), dest);
  }

  llvm::Value* vi_reg(unsigned r) {
    r &= 15;
    return r == 0 ? static_cast<llvm::Value*>(b_.getInt32(0)) : ld(i32_, L_.vi + r * 4);
  }

  // Q pipeline: the result waits in a held value until vwaitq.
  void q_result(std::uint32_t latency, llvm::Value* value_bits, llvm::Value* division_by_zero) {
    st(b_.CreateAdd(ld(i32_, L_.pipe_time), b_.getInt32(latency + rel_)), L_.pipe_q);  // counter
    st(value_bits, L_.pipe_q + 4);                                                // held value
    st(b_.CreateZExt(division_by_zero, i32_), L_.vdf);
  }

  bool emit_vu(const Insn& in) {
    const unsigned raw = in.raw;
    const unsigned dest = (raw >> 21) & 15, ft = (raw >> 16) & 31, fs = (raw >> 11) & 31, fd = (raw >> 6) & 31;
    const unsigned fsf = dest & 3, ftf = (dest >> 2) & 3;
    switch (in.op) {
      case Op::VADD: fmac(in, FKind::Add, FSrc::Vec, false); return true;
      case Op::VSUB: fmac(in, FKind::Sub, FSrc::Vec, false); return true;
      case Op::VMUL: fmac(in, FKind::Mul, FSrc::Vec, false); return true;
      case Op::VMADD: fmac(in, FKind::Madd, FSrc::Vec, false); return true;
      case Op::VMSUB: fmac(in, FKind::Msub, FSrc::Vec, false); return true;
      case Op::VADDbc: fmac(in, FKind::Add, FSrc::Bc, false); return true;
      case Op::VSUBbc: fmac(in, FKind::Sub, FSrc::Bc, false); return true;
      case Op::VMULbc: fmac(in, FKind::Mul, FSrc::Bc, false); return true;
      case Op::VMADDbc: fmac(in, FKind::Madd, FSrc::Bc, false); return true;
      case Op::VMSUBbc: fmac(in, FKind::Msub, FSrc::Bc, false); return true;
      case Op::VADDi: fmac(in, FKind::Add, FSrc::I, false); return true;
      case Op::VSUBi: fmac(in, FKind::Sub, FSrc::I, false); return true;
      case Op::VMULi: fmac(in, FKind::Mul, FSrc::I, false); return true;
      case Op::VMADDi: fmac(in, FKind::Madd, FSrc::I, false); return true;
      case Op::VMSUBi: fmac(in, FKind::Msub, FSrc::I, false); return true;
      case Op::VADDq: fmac(in, FKind::Add, FSrc::Q, false); return true;
      case Op::VSUBq: fmac(in, FKind::Sub, FSrc::Q, false); return true;
      case Op::VMULq: fmac(in, FKind::Mul, FSrc::Q, false); return true;
      case Op::VMADDq: fmac(in, FKind::Madd, FSrc::Q, false); return true;
      case Op::VMSUBq: fmac(in, FKind::Msub, FSrc::Q, false); return true;
      case Op::VADDA: fmac(in, FKind::Add, FSrc::Vec, true); return true;
      case Op::VSUBA: fmac(in, FKind::Sub, FSrc::Vec, true); return true;
      case Op::VMULA: fmac(in, FKind::Mul, FSrc::Vec, true); return true;
      case Op::VMADDA: fmac(in, FKind::Madd, FSrc::Vec, true); return true;
      case Op::VMSUBA: fmac(in, FKind::Msub, FSrc::Vec, true); return true;
      case Op::VADDAbc: fmac(in, FKind::Add, FSrc::Bc, true); return true;
      case Op::VSUBAbc: fmac(in, FKind::Sub, FSrc::Bc, true); return true;
      case Op::VMULAbc: fmac(in, FKind::Mul, FSrc::Bc, true); return true;
      case Op::VMADDAbc: fmac(in, FKind::Madd, FSrc::Bc, true); return true;
      case Op::VMSUBAbc: fmac(in, FKind::Msub, FSrc::Bc, true); return true;
      case Op::VADDAi: fmac(in, FKind::Add, FSrc::I, true); return true;
      case Op::VSUBAi: fmac(in, FKind::Sub, FSrc::I, true); return true;
      case Op::VMULAi: fmac(in, FKind::Mul, FSrc::I, true); return true;
      case Op::VMADDAi: fmac(in, FKind::Madd, FSrc::I, true); return true;
      case Op::VMSUBAi: fmac(in, FKind::Msub, FSrc::I, true); return true;
      case Op::VADDAq: fmac(in, FKind::Add, FSrc::Q, true); return true;
      case Op::VSUBAq: fmac(in, FKind::Sub, FSrc::Q, true); return true;
      case Op::VMULAq: fmac(in, FKind::Mul, FSrc::Q, true); return true;
      case Op::VMADDAq: fmac(in, FKind::Madd, FSrc::Q, true); return true;
      case Op::VMSUBAq: fmac(in, FKind::Msub, FSrc::Q, true); return true;
      case Op::VMAX: maxmin(in, true, FSrc::Vec); return true;
      case Op::VMAXbc: maxmin(in, true, FSrc::Bc); return true;
      case Op::VMAXi: maxmin(in, true, FSrc::I); return true;
      case Op::VMINI: maxmin(in, false, FSrc::Vec); return true;
      case Op::VMINIbc: maxmin(in, false, FSrc::Bc); return true;
      case Op::VMINIi: maxmin(in, false, FSrc::I); return true;
      case Op::VOPMULA: {
        // acc.x = fs.y * ft.z, acc.y = fs.z * ft.x, acc.z = fs.x * ft.y; no flags
        st(b_.CreateFMul(lane(fs, 1), lane(ft, 2)), L_.vacc + 0);
        st(b_.CreateFMul(lane(fs, 2), lane(ft, 0)), L_.vacc + 4);
        st(b_.CreateFMul(lane(fs, 0), lane(ft, 1)), L_.vacc + 8);
        return true;
      }
      case Op::VOPMSUB: {
        llvm::Value* x = b_.CreateFSub(ld(f32_, L_.vacc + 0), b_.CreateFMul(lane(fs, 1), lane(ft, 2)));
        llvm::Value* y = b_.CreateFSub(ld(f32_, L_.vacc + 4), b_.CreateFMul(lane(fs, 2), lane(ft, 0)));
        llvm::Value* z = b_.CreateFSub(ld(f32_, L_.vacc + 8), b_.CreateFMul(lane(fs, 0), lane(ft, 1)));
        llvm::Value* r = llvm::PoisonValue::get(v4f());
        r = b_.CreateInsertElement(r, x, 0ull);
        r = b_.CreateInsertElement(r, y, 1ull);
        r = b_.CreateInsertElement(r, z, 2ull);
        r = b_.CreateInsertElement(r, llvm::ConstantFP::get(f32_, 0.0), 3ull);  // masked out of the flags
        sz_flags(r, 0xE);
        vstore(fd, r, 0xE);
        return true;
      }
      case Op::VCLIP: {
        llvm::Value* bits = b_.getInt32(0);
        if (fs != 0 || ft != 0) {
          llvm::Value* w = ft == 0 ? static_cast<llvm::Value*>(llvm::ConstantFP::get(f32_, 1.0))
                                   : b_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, lane(ft, 3));
          llvm::Value* negw = b_.CreateFNeg(w);
          for (unsigned i = 0; i < 3; ++i) {
            llvm::Value* v = lane(fs, i);
            llvm::Value* gt = b_.CreateZExt(b_.CreateFCmpOGT(v, w), i32_);
            llvm::Value* lt = b_.CreateZExt(b_.CreateFCmpOLT(v, negw), i32_);
            bits = b_.CreateOr(bits, b_.CreateOr(b_.CreateShl(gt, 2 * i), b_.CreateShl(lt, 2 * i + 1)));
          }
        }
        queue_clip(b_.CreateOr(b_.CreateShl(pipe_last(L_.pipe_clip), 6), bits));
        return true;
      }
      case Op::VMOVE: if (ft) vstore(ft, vf(fs), dest); return true;
      case Op::VMR32: {
        if (!ft) return true;
        llvm::Value* x = lane(fs, 1);
        llvm::Value* y = lane(fs, 2);
        llvm::Value* z = lane(fs, 3);
        llvm::Value* w = lane(fs, 0);
        if (fs == ft) st(b_.CreateBitCast(w, i32_), L_.vt);  // the reference core's temporary
        llvm::Value* r = llvm::PoisonValue::get(v4f());
        r = b_.CreateInsertElement(r, x, 0ull);
        r = b_.CreateInsertElement(r, y, 1ull);
        r = b_.CreateInsertElement(r, z, 2ull);
        r = b_.CreateInsertElement(r, w, 3ull);
        vstore(ft, r, dest);
        return true;
      }
      case Op::VABS:
        if (ft) vstore(ft, b_.CreateBitCast(b_.CreateAnd(b_.CreateBitCast(vf(fs), v4i()), splati(0x7FFFFFFF)), v4f()), dest);
        return true;
      case Op::VFTOI0: case Op::VFTOI4: case Op::VFTOI12: case Op::VFTOI15: {
        if (!ft) return true;
        const float scale = in.op == Op::VFTOI0 ? 1.0f : in.op == Op::VFTOI4 ? 16.0f : in.op == Op::VFTOI12 ? 4096.0f : 32768.0f;
        llvm::Value* v = vf(fs);
        if (in.op != Op::VFTOI0) v = b_.CreateFMul(v, splatf(scale));
        llvm::Value* ok = b_.CreateAnd(b_.CreateFCmpOGE(v, splatf(-2147483648.0f)), b_.CreateFCmpOLT(v, splatf(2147483648.0f)));
        llvm::Value* ints = b_.CreateSelect(ok, b_.CreateFPToSI(v, v4i()), splati(0x80000000u));  // as cvttps2dq
        vstore(ft, b_.CreateBitCast(ints, v4f()), dest);
        return true;
      }
      case Op::VITOF0: case Op::VITOF4: case Op::VITOF12: case Op::VITOF15: {
        if (!ft) return true;
        const float scale = in.op == Op::VITOF0 ? 1.0f : in.op == Op::VITOF4 ? 16.0f : in.op == Op::VITOF12 ? 4096.0f : 32768.0f;
        llvm::Value* v = b_.CreateSIToFP(b_.CreateBitCast(vf(fs), v4i()), v4f());
        if (in.op != Op::VITOF0) v = b_.CreateFDiv(v, splatf(scale));
        vstore(ft, v, dest);
        return true;
      }
      case Op::VMTIR: if (ft & 15) st(b_.CreateBitCast(lane(fs, fsf), i32_), L_.vi + (ft & 15) * 4); return true;
      case Op::VMFIR: {
        if (!ft) return true;
        llvm::Value* v = b_.CreateSExt(b_.CreateTrunc(vi_reg(fs), b_.getInt16Ty()), i32_);
        vstore(ft, b_.CreateBitCast(b_.CreateVectorSplat(4, v), v4f()), dest);
        return true;
      }
      case Op::VIADD: if (fd & 15) st(b_.CreateAdd(vi_reg(fs), vi_reg(ft)), L_.vi + (fd & 15) * 4); return true;
      case Op::VISUB: if (fd & 15) st(b_.CreateSub(vi_reg(fs), vi_reg(ft)), L_.vi + (fd & 15) * 4); return true;
      case Op::VIAND: if (fd & 15) st(b_.CreateAnd(vi_reg(fs), vi_reg(ft)), L_.vi + (fd & 15) * 4); return true;
      case Op::VIOR: if (fd & 15) st(b_.CreateOr(vi_reg(fs), vi_reg(ft)), L_.vi + (fd & 15) * 4); return true;
      case Op::VIADDI:
        if (ft & 15) {
          const std::uint32_t imm = fd | ((fd & 0x10) ? 0xFFFFFFE0u : 0u);
          st(b_.CreateAdd(vi_reg(fs), b_.getInt32(imm)), L_.vi + (ft & 15) * 4);
        }
        return true;
      case Op::VDIV: case Op::VRSQRT: {
        llvm::Value* a = b_.CreateBitCast(lane(fs, fsf), i32_);
        llvm::Value* d = b_.CreateBitCast(lane(ft, ftf), i32_);
        llvm::Value* zero = b_.CreateICmpEQ(b_.CreateAnd(d, b_.getInt32(0x7F800000)), b_.getInt32(0));
        llvm::Value* inf = b_.CreateOr(b_.getInt32(0x7F7FFFFF), b_.CreateAnd(b_.CreateXor(a, d), b_.getInt32(0x80000000u)));
        llvm::Value* q;
        if (in.op == Op::VDIV) {
          q = b_.CreateFDiv(asf(a), asf(d));
        } else {
          q = b_.CreateFMul(asf(a), b_.CreateFDiv(llvm::ConstantFP::get(f32_, 1.0),
                                                  b_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, asf(d))));
        }
        q_result(in.op == Op::VDIV ? 7 : 13, b_.CreateSelect(zero, inf, asi(q)), zero);
        return true;
      }
      case Op::VSQRT: {
        llvm::Value* v = b_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, b_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, lane(ft, ftf)));
        q_result(7, asi(v), b_.getFalse());
        return true;
      }
      case Op::VWAITQ:
        st(b_.getInt32(0), L_.pipe_q);
        st(ld(i32_, L_.pipe_q + 4), L_.vq);
        return true;
      case Op::VNOP:
        return true;
      default:
        return false;
    }
  }
};

}  // namespace hp2::detail
