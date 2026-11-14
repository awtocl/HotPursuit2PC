// VU1 microcode translator: see hp2/vulift.h and docs/adr/0008-vu1-microcode.md.
//
// Each block is planned the way the reference core's CVuExecutor::PartitionFunction
// and CVuBasicBlock::CompileRange see it, then emitted pair by pair in the
// same order of effects:
//   dynamic stalls on the previous block's writes (first 3 pairs), Q flush for
//   a lower op that syncs Q, the static stall delay, Q check for an upper op
//   that reads Q, the "pre-upper" copy when the lower reads the upper's
//   destination, the integer-branch save, the upper op (then its D, T, I and
//   E bits), the first-pair branch substitution, a pending xgkick before a new
//   one, the lower op, the restores, a pending xgkick after the pair.
// At the block end: pipeline time, the FMAC writes that land in the next
// block, the trailing integer register, the cycle quota, the next program
// counter.
#include "hp2/vulift.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "hp2/vu.h"
#include "lift_internal.h"

namespace hp2 {

using namespace detail;
using vu::Lower;

namespace {

constexpr std::uint32_t kMaxBlockSize = 0x1000;  // CGenericMipsExecutor::MAX_BLOCK_SIZE
constexpr std::uint32_t kLatencyMac = 4;
constexpr std::uint32_t kNoAddress = 0xFFFFFFFFu;

struct IntegerDelay {
  std::uint32_t reg = 0;
  std::uint32_t save = kNoAddress, use = kNoAddress;
};

struct BlockPlan {
  std::uint32_t begin = 0, end = 0;  // end: byte address of the last word (the last pair's upper)
  std::vector<std::uint32_t> stalls;
  std::uint32_t write_times[32][4] = {};  // FMAC register write times relative to the block start
  IntegerDelay branch_delay, trailing;
  std::uint32_t pairs() const { return (end - begin + 4) / 8; }
};

struct Image {
  std::uint32_t address = 0;
  std::vector<std::uint32_t> words;
  std::vector<std::uint32_t> extra;
  std::map<std::uint32_t, BlockPlan> blocks;  // by start
  bool contains(std::uint32_t a, std::uint32_t size = 4) const {
    return a >= address && a + size <= address + words.size() * 4 && a + size > a;
  }
  std::uint32_t word(std::uint32_t a) const { return words[(a - address) / 4]; }
};

std::string function_name(std::size_t index) {
  return std::format("vu_image_{}", index);
}

// ---- planning --------------------------------------------------------------

class Planner {
 public:
  Planner(const Image& image, const VuConfig& config) : im_(image), cfg_(config) {}

  // Plans the block starting at `start`, or throws Unsupported.
  BlockPlan plan(std::uint32_t start) const {
    BlockPlan p;
    p.begin = start;
    p.end = partition(start);
    for (std::uint32_t a = p.begin; a <= p.end; a += 4)
      if (!im_.contains(a)) throw Unsupported{"block extends beyond the image"};
    check_supported(p);
    stall_delays(p);
    p.branch_delay = integer_branch_delay(p);
    p.trailing = trailing_delay(p);
    return p;
  }

  // The constant successors of a block (fall-through and a constant branch target).
  std::vector<std::uint32_t> successors(const BlockPlan& p) const {
    std::vector<std::uint32_t> next{p.end + 4};
    const std::uint32_t branch = p.end - 0xC;
    if (p.end - p.begin >= 0xC) {
      const vu::Pair pair = vu::decode(im_.word(branch), im_.word(branch + 4));
      if (pair.lower_is_branch() && pair.lo != Lower::JR && pair.lo != Lower::JALR)
        next.push_back(vu::branch_target(branch, pair.lower));
    }
    return next;
  }

 private:
  const Image& im_;
  const VuConfig& cfg_;

  std::uint32_t word(std::uint32_t a) const {
    if (!im_.contains(a)) throw Unsupported{"the block's rules read words outside the image"};
    return im_.word(a);
  }
  vu::Operands lo_ops(std::uint32_t a) const { return vu::lower_operands(word(a), word(a + 4)); }
  vu::Operands hi_ops(std::uint32_t a) const { return vu::upper_operands(word(a + 4)); }

  // CVuExecutor::PartitionFunction.
  std::uint32_t partition(std::uint32_t start) const {
    std::uint32_t end = std::min(start + kMaxBlockSize - 4, cfg_.micro_size - 4);
    for (std::uint32_t a = start; a < end; a += 8) {
      const std::uint32_t lower = word(a), upper = word(a + 4);
      const bool branch = vu::decode(lower, upper).lower_is_branch();
      if (upper & vu::kBitE) return a + 0xC;
      if (!branch && (upper & (vu::kBitD | vu::kBitT))) return a + 4;
      if (branch) return a + 0xC;
    }
    return end;
  }

  void check_supported(const BlockPlan& p) const {
    for (std::uint32_t a = p.begin; a <= p.end; a += 8) {
      const std::uint32_t lower = word(a), upper = word(a + 4);
      const vu::Pair pair = vu::decode(lower, upper);
      if (pair.up == r5900::Op::Invalid) throw Unsupported{std::format("upper word {:08X}", upper)};
      const vu::Operands lo = lo_ops(a), hi = hi_ops(a);
      if (lo.read_mac || lo.sync_p || lo.read_p) throw Unsupported{"reads MAC/status flags or uses P"};
      if (lo.write_i >= 16 || lo.read_i0 >= 16 || lo.read_i1 >= 16 || lo.write_i_lsu >= 16)
        throw Unsupported{"integer register field above vi15"};
      (void)hi;
      if (!pair.i_bit() && lower != vu::kLowerNop) {
        const unsigned it = (lower >> 16) & 31, is = (lower >> 11) & 31;
        switch (pair.lo) {
          case Lower::LQ: case Lower::SQ: case Lower::ILW: case Lower::ISW: case Lower::IADDIU: case Lower::ISUBIU:
          case Lower::FCEQ: case Lower::FCSET: case Lower::FCAND: case Lower::FCOR: case Lower::FCGET:
          case Lower::B: case Lower::IBEQ: case Lower::IBNE:
          case Lower::IADD: case Lower::ISUB: case Lower::IADDI: case Lower::IAND: case Lower::IOR:
          case Lower::MOVE: case Lower::LQI: case Lower::DIV: case Lower::MTIR: case Lower::RNEXT:
          case Lower::MR32: case Lower::SQI: case Lower::SQRT: case Lower::MFIR: case Lower::RGET:
          case Lower::XITOP: case Lower::LQD: case Lower::RSQRT: case Lower::ILWR: case Lower::RINIT:
          case Lower::SQD: case Lower::WAITQ: case Lower::ISWR: case Lower::RXOR:
            break;
          case Lower::BAL: case Lower::XTOP:  // write the raw register array: vi0 would break its invariant
            if (it == 0) throw Unsupported{std::format("{} writes vi0", vu::mnemonic(pair.lo))};
            break;
          case Lower::JALR:
            if (it == 0) throw Unsupported{"jalr writes vi0"};
            break;
          case Lower::JR: case Lower::XGKICK: case Lower::IBLTZ: case Lower::IBGTZ: case Lower::IBLEZ:
          case Lower::IBGEZ:
            (void)is;
            break;
          default:
            throw Unsupported{std::format("lower op {}", vu::mnemonic(pair.lo))};
        }
      }
    }
    // Branches in a branch's delay slot and an end bit there have extra rules in the reference core.
    const std::uint32_t last = p.end - 4;
    if (vu::is_conditional_branch(word(last)) || vu::decode(word(last), word(last + 4)).lower_is_branch())
      throw Unsupported{"branch in the last pair"};
    if ((word(last + 4) & vu::kBitE) && last >= 8 && vu::is_conditional_branch(word(last - 8)))
      throw Unsupported{"end bit in a branch delay slot"};
  }

  // CVuBasicBlock::ComputeFmacStallDelays.
  void stall_delays(BlockPlan& p) const {
    std::uint32_t rel = 0;
    std::uint32_t wf[32][4] = {}, wi[32][4] = {};
    auto adjust = [](std::uint32_t t, const std::uint32_t (&times)[32][4], unsigned dest, unsigned reg) {
      if (reg == 0) return t;
      for (unsigned i = 0; i < 4; ++i)
        if (dest & (1u << i)) t = std::max(t, times[reg][i]);
      return t;
    };
    for (std::uint32_t a = p.begin; a <= p.end; a += 8) {
      const vu::Operands lo = lo_ops(a), hi = hi_ops(a);
      const unsigned lo_dest = (word(a) >> 21) & 15, hi_dest = (word(a + 4) >> 21) & 15;
      const std::uint32_t prev = rel;
      rel = adjust(rel, wf, lo.read_elem_f0, lo.read_f0);
      rel = adjust(rel, wf, lo.read_elem_f1, lo.read_f1);
      rel = adjust(rel, wf, hi.read_elem_f0, hi.read_f0);
      rel = adjust(rel, wf, hi.read_elem_f1, hi.read_f1);
      rel = adjust(rel, wi, 0xF, lo.read_i0);
      rel = adjust(rel, wi, 0xF, lo.read_i1);
      p.stalls.push_back(rel - prev);
      if (lo.write_f)
        for (unsigned i = 0; i < 4; ++i)
          if (lo_dest & (1u << i)) wf[lo.write_f][i] = rel + kLatencyMac;
      if (lo.write_i_lsu)
        for (unsigned i = 0; i < 4; ++i) wi[lo.write_i_lsu][i] = rel + kLatencyMac;
      if (hi.write_f)
        for (unsigned i = 0; i < 4; ++i)
          if (hi_dest & (1u << i)) wf[hi.write_f][i] = rel + kLatencyMac;
      ++rel;
    }
    std::memcpy(p.write_times, wf, sizeof wf);
  }

  // CVuBasicBlock::CheckIsSpecialIntegerLoop.
  bool special_integer_loop(const BlockPlan& p, unsigned reg) const {
    const std::uint32_t length = (p.end - p.begin) / 8;
    if (length != 4) return false;
    for (std::uint32_t index = 0; index <= length; ++index) {
      const std::uint32_t a = p.begin + index * 8;
      if (index == length - 1) {
        const vu::Pair pair = vu::decode(word(a), word(a + 4));
        if (!pair.lower_is_branch() || pair.lo == Lower::JR || pair.lo == Lower::JALR) return false;
        if (vu::branch_target(a, pair.lower) != p.begin) return false;
      } else if (lo_ops(a).write_i != reg) {
        return false;
      }
    }
    return true;
  }

  // CVuBasicBlock::ComputeIntegerBranchDelayInfo.
  IntegerDelay integer_branch_delay(const BlockPlan& p) const {
    IntegerDelay r;
    const std::uint32_t adjusted_end = p.end - 4;
    const std::uint32_t branch = adjusted_end - 8;
    if (!vu::is_conditional_branch(word(branch))) return r;
    if (p.stalls.size() < 2) throw Unsupported{"one-pair block after a conditional branch"};
    const std::uint32_t delay_on_branch = p.stalls[p.stalls.size() - 2];
    const vu::Operands prior = lo_ops(adjusted_end - 16);
    if (prior.write_i == 0 || prior.branch_value || delay_on_branch != 0) return r;
    const vu::Operands br = lo_ops(branch);
    if (br.read_i0 != prior.write_i && br.read_i1 != prior.write_i) return r;
    if (special_integer_loop(p, prior.write_i)) return r;
    r.reg = prior.write_i;
    r.save = static_cast<std::uint32_t>(std::max<std::int32_t>(static_cast<std::int32_t>(adjusted_end) - 5 * 8,
                                                              static_cast<std::int32_t>(p.begin)));
    r.use = adjusted_end - 8;
    return r;
  }

  // CVuBasicBlock::ComputeTrailingIntegerBranchDelayInfo.
  IntegerDelay trailing_delay(const BlockPlan& p) const {
    IntegerDelay r;
    const std::uint32_t adjusted_end = p.end - 4;
    const vu::Operands end_ops = lo_ops(adjusted_end);
    if (p.stalls.size() > 1) {
      const std::uint32_t delay_on_branch = p.stalls[p.stalls.size() - 2];
      if (end_ops.write_i != 0 && !end_ops.branch_value && delay_on_branch == 0) {
        r.reg = end_ops.write_i;
        r.save = adjusted_end;
      }
    }
    return r;
  }
};

// ---- code generation -------------------------------------------------------

class MicroLifter : public VectorCode {
 public:
  MicroLifter(llvm::Module& module, const StateLayout& layout, const VuLayout& vu_layout, const VuConfig& config,
              const Image& image, VuTranslateStats& stats)
      : VectorCode(module.getContext(), layout), m_(module), V_(vu_layout), cfg_(config), im_(image), stats_(stats) {
    mac_flags_ = false;
    buffer_flags_ = false;
  }

  // `blocks`: the blocks of this function. `continue_with`: null for an image
  // (it dispatches between its own blocks); for an entry function (one block),
  // the image function it continues in after that block.
  void lift(const std::string& name, const std::map<std::uint32_t, BlockPlan>& blocks,
            llvm::Function* continue_with = nullptr) {
    continue_with_ = continue_with;
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    fn_ = llvm::Function::Create(llvm::FunctionType::get(b_.getVoidTy(), {ptr, ptr, ptr}, false),
                                 llvm::Function::ExternalLinkage, name, m_);
    fn_->addParamAttr(0, llvm::Attribute::NoAlias);
    fn_->addFnAttr(llvm::Attribute::NoUnwind);
    fn_->addFnAttr("denormal-fp-math", "preserve-sign,preserve-sign");
    fn_->addFnAttr("denormal-fp-math-f32", "preserve-sign,preserve-sign");
    state_ = fn_->getArg(0);
    mem_ = fn_->getArg(1);
    rt_ = fn_->getArg(2);
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn_);
    exit_ = llvm::BasicBlock::Create(ctx_, "exit", fn_);
    b_.SetInsertPoint(exit_);
    b_.CreateRetVoid();
    for (const auto& [start, plan] : blocks)
      bb_[start] = llvm::BasicBlock::Create(ctx_, std::format("B{:04X}", start), fn_);
    b_.SetInsertPoint(entry);
    dispatch(ld(i32_, L_.pc), {});
    for (const auto& [start, plan] : blocks) {
      b_.SetInsertPoint(bb_.at(start));
      block(plan);
      ++stats_.blocks;
      stats_.pairs += plan.pairs();
    }
    if (llvm::verifyFunction(*fn_, &llvm::errs())) fail("translated microcode image does not verify");
  }

 private:
  llvm::Module& m_;
  const VuLayout& V_;
  const VuConfig& cfg_;
  const Image& im_;
  VuTranslateStats& stats_;
  llvm::Function* fn_ = nullptr;
  llvm::Value *mem_ = nullptr, *rt_ = nullptr;
  llvm::BasicBlock* exit_ = nullptr;
  std::map<std::uint32_t, llvm::BasicBlock*> bb_;
  llvm::Value* delayed_ = nullptr;  // the branch target chosen in this block (or the invalid PC)
  llvm::Function* continue_with_ = nullptr;

  // Continues at the block for `pc`, or returns (hands over) when there is none.
  // `candidates` empty: any block start of the image.
  void dispatch(llvm::Value* pc, const std::vector<std::uint32_t>& candidates) {
    auto* sw = b_.CreateSwitch(pc, exit_);
    if (candidates.empty()) {
      for (const auto& [start, block] : bb_) sw->addCase(b_.getInt32(start), block);
      return;
    }
    std::set<std::uint32_t> seen;
    for (std::uint32_t c : candidates)
      if (bb_.count(c) && seen.insert(c).second) sw->addCase(b_.getInt32(c), bb_.at(c));
  }

  // ---- registers ----
  llvm::Value* vi_raw(unsigned r) { return ld(i32_, L_.vi + r * 4); }  // nCOP2VI[r] as stored
  void set_vi_raw(unsigned r, llvm::Value* v) { st(v, L_.vi + r * 4); }
  llvm::Value* vi(unsigned r) {  // VUShared::PushIntegerRegister
    r &= 15;
    return r == 0 ? static_cast<llvm::Value*>(b_.getInt32(0)) : vi_raw(r);
  }
  void set_vi(unsigned r, llvm::Value* v) { set_vi_raw(r & 15, v); }  // PullIntegerRegister (r & 15 != 0)
  llvm::Value* vi_dynamic(llvm::Value* index) {
    return b_.CreateLoad(i32_, b_.CreateInBoundsGEP(b_.getInt8Ty(), state_,
                                                    b_.CreateZExt(b_.CreateAdd(b_.getInt32(L_.vi), b_.CreateShl(index, 2)), i64_)));
  }
  void set_vi_dynamic(llvm::Value* index, llvm::Value* v) {
    b_.CreateStore(v, b_.CreateInBoundsGEP(b_.getInt8Ty(), state_,
                                           b_.CreateZExt(b_.CreateAdd(b_.getInt32(L_.vi), b_.CreateShl(index, 2)), i64_)));
  }
  llvm::Value* bits128(std::uint32_t offset) { return b_.CreateAlignedLoad(v4i(), field(offset), llvm::MaybeAlign(4)); }
  void set_bits128(std::uint32_t offset, llvm::Value* v) { b_.CreateAlignedStore(v, field(offset), llvm::MaybeAlign(4)); }

  // ---- data memory ----
  llvm::Value* mem(llvm::Value* masked) { return b_.CreateInBoundsGEP(b_.getInt8Ty(), mem_, b_.CreateZExt(masked, i64_)); }
  llvm::Value* mask(llvm::Value* a) { return b_.CreateAnd(a, b_.getInt32(cfg_.data_mask)); }
  // VUShared::ComputeMemAccessAddr
  llvm::Value* address(unsigned base, std::int32_t offset, std::uint32_t dest_offset) {
    llvm::Value* a = vi(base);
    if (offset != 0) a = b_.CreateAdd(a, b_.getInt32(static_cast<std::uint32_t>(offset)));
    a = b_.CreateShl(a, 4);
    if (dest_offset != 0) a = b_.CreateAdd(a, b_.getInt32(dest_offset));
    return mask(a);
  }
  static std::int32_t imm11(std::uint32_t lower) {
    std::int32_t v = static_cast<std::int32_t>(lower & 0x7FF);
    return (v & 0x400) ? v - 0x800 : v;
  }
  static std::uint32_t dest_offset(unsigned dest) {  // VUShared::GetDestOffset
    if (dest & 1) return 0xC;
    if (dest & 2) return 0x8;
    if (dest & 4) return 0x4;
    return 0;
  }
  void load_quad(llvm::Value* masked, unsigned dest, unsigned it) {  // LQbase
    if (it == 0 || dest == 0) return;
    vstore(it, b_.CreateAlignedLoad(v4f(), mem(masked), llvm::MaybeAlign(4)), dest);
  }
  void store_quad(llvm::Value* masked, unsigned dest, unsigned is) {  // SQbase
    if (dest == 0) return;
    llvm::Value* v = b_.CreateBitCast(vf(is), v4i());
    if (dest == 0xF) {
      b_.CreateAlignedStore(v, mem(masked), llvm::MaybeAlign(4));
      return;
    }
    for (unsigned i = 0; i < 4; ++i)
      if (dest & (8u >> i))
        b_.CreateAlignedStore(b_.CreateExtractElement(v, i), mem(b_.CreateAdd(masked, b_.getInt32(i * 4))),
                              llvm::MaybeAlign(4));
  }
  void store_int(llvm::Value* masked, unsigned dest, unsigned it) {  // ISWbase
    llvm::Value* v = b_.CreateAnd(vi(it), b_.getInt32(0xFFFF));
    for (unsigned i = 0; i < 4; ++i)
      if (dest & (8u >> i))
        b_.CreateAlignedStore(v, mem(b_.CreateAdd(masked, b_.getInt32(i * 4))), llvm::MaybeAlign(4));
  }

  // ---- pipelines ----
  llvm::Value* now(std::uint32_t rel) { return b_.CreateAdd(ld(i32_, L_.pipe_time), b_.getInt32(rel)); }
  void flush_q() {  // VUShared::FlushPipeline
    st(b_.getInt32(0), L_.pipe_q);
    st(ld(i32_, L_.pipe_q + 4), L_.vq);
  }
  void check_q(std::uint32_t rel) {  // VUShared::CheckPipeline
    llvm::Value* counter = ld(i32_, L_.pipe_q);
    llvm::Value* due = b_.CreateICmpSLE(counter, now(rel));
    st(b_.CreateSelect(due, b_.getInt32(0), counter), L_.pipe_q);
    st(b_.CreateSelect(due, ld(i32_, L_.pipe_q + 4), ld(i32_, L_.vq)), L_.vq);
  }
  void check_clip(std::uint32_t rel) {  // VUShared::CheckFlagPipeline
    llvm::Value* t = now(rel);
    llvm::Value* value = ld(i32_, L_.vcf);
    llvm::Value* base = ld(i32_, L_.pipe_clip);
    for (unsigned i = 0; i < 8; ++i) {
      llvm::Value* index = b_.CreateAnd(b_.CreateAdd(base, b_.getInt32(i)), b_.getInt32(7));
      llvm::Value* due = b_.CreateICmpSLE(b_.CreateLoad(i32_, pipe_slot(L_.pipe_clip, kPipeTimes, index)), t);
      value = b_.CreateSelect(due, b_.CreateLoad(i32_, pipe_slot(L_.pipe_clip, kPipeValues, index)), value);
    }
    st(value, L_.vcf);
  }
  void set_exception_if_none(std::uint32_t code) {
    llvm::Value* e = ld(i32_, L_.has_exception);
    st(b_.CreateSelect(b_.CreateICmpEQ(e, b_.getInt32(0)), b_.getInt32(code), e), L_.has_exception);
  }
  void xgkick() {  // CVuBasicBlock::EmitXgKick
    auto* type = llvm::FunctionType::get(b_.getVoidTy(), {b_.getPtrTy(), i32_, i32_}, false);
    llvm::Value* write = b_.CreateLoad(b_.getPtrTy(), b_.CreateConstInBoundsGEP1_32(b_.getPtrTy(), rt_, 1));
    b_.CreateCall(type, write, {state_, b_.getInt32(cfg_.addr_xgkick), ld(i32_, V_.xgkick_address)});
  }
  llvm::Value* port_read(std::uint32_t address) {
    auto* type = llvm::FunctionType::get(i32_, {b_.getPtrTy(), i32_}, false);
    llvm::Value* read = b_.CreateLoad(b_.getPtrTy(), b_.CreateConstInBoundsGEP1_32(b_.getPtrTy(), rt_, 0));
    return b_.CreateCall(type, read, {state_, b_.getInt32(address)});
  }

  // ---- the block ----
  void block(const BlockPlan& p) {
    delayed_ = b_.getInt32(cfg_.invalid_pc);
    std::uint32_t rel = 0;
    bool pending_kick = false;
    llvm::Value* saved_int = nullptr;  // the integer-branch rule's saved register (savedIntReg)
    unsigned index = 0;
    for (std::uint32_t a = p.begin; a <= p.end; a += 8, ++index) {
      const std::uint32_t lower = im_.word(a), upper = im_.word(a + 4);
      const vu::Pair pair = vu::decode(lower, upper);
      const vu::Operands lo = vu::lower_operands(lower, upper), hi = vu::upper_operands(upper);
      const bool lo_is_kick = (lower & ~(0x1Fu << 11)) == 0x800006FCu;

      if (index < 3) fmac_write_stalls(index, lo, hi);
      if (lo.sync_q) flush_q();
      rel += p.stalls[index];
      if (hi.read_q) check_q(rel);

      unsigned saved = 0;
      llvm::Value* pre_upper = nullptr;
      if (hi.write_f != 0 && (hi.write_f == lo.read_f0 || hi.write_f == lo.read_f1)) {
        saved = hi.write_f;
        pre_upper = bits128(vfo(saved));
      }
      if (a == p.branch_delay.save) saved_int = vi_raw(p.branch_delay.reg);
      if (a == p.trailing.save) st(vi_raw(p.trailing.reg), V_.saved_next_value);

      rel_ = rel;
      upper_word(pair);

      llvm::Value* upper_result = nullptr;
      if (saved) {
        upper_result = bits128(vfo(saved));
        set_bits128(vfo(saved), pre_upper);
      }
      llvm::Value* int_temp = nullptr;
      if (a == p.branch_delay.use) {
        int_temp = vi_raw(p.branch_delay.reg);
        if (!saved_int) fail("integer-branch rule used before its value was saved");
        set_vi_raw(p.branch_delay.reg, saved_int);
      }
      llvm::Value *first_match = nullptr, *next_index = nullptr, *next_temp = nullptr;
      if (a == p.begin && vu::is_conditional_branch(lower)) {
        // The previous block's last pair wrote a register this branch reads: use the value from before.
        next_index = ld(i32_, V_.saved_next_index);
        first_match = b_.CreateOr(b_.CreateICmpEQ(next_index, b_.getInt32(lo.read_i0)),
                                  b_.CreateICmpEQ(next_index, b_.getInt32(lo.read_i1)));
        auto* yes = llvm::BasicBlock::Create(ctx_, "", fn_);
        auto* join = llvm::BasicBlock::Create(ctx_, "", fn_);
        auto* from = b_.GetInsertBlock();
        b_.CreateCondBr(first_match, yes, join);
        b_.SetInsertPoint(yes);
        llvm::Value* temp = vi_dynamic(next_index);
        set_vi_dynamic(next_index, ld(i32_, V_.saved_next_value));
        b_.CreateBr(join);
        b_.SetInsertPoint(join);
        auto* phi = b_.CreatePHI(i32_, 2);
        phi->addIncoming(temp, yes);
        phi->addIncoming(b_.getInt32(0), from);
        next_temp = phi;
      }

      if (lo_is_kick && pending_kick) {
        xgkick();
        pending_kick = false;
      }
      lower_word(a, pair, rel);

      if (int_temp) set_vi_raw(p.branch_delay.reg, int_temp);
      if (first_match) {
        auto* yes = llvm::BasicBlock::Create(ctx_, "", fn_);
        auto* join = llvm::BasicBlock::Create(ctx_, "", fn_);
        b_.CreateCondBr(first_match, yes, join);
        b_.SetInsertPoint(yes);
        set_vi_dynamic(next_index, next_temp);
        b_.CreateBr(join);
        b_.SetInsertPoint(join);
      }
      if (saved) set_bits128(vfo(saved), upper_result);
      if (pending_kick) {
        xgkick();
        pending_kick = false;
      }
      if (lo_is_kick) pending_kick = true;
      ++rel;
    }
    if (pending_kick) xgkick();

    // Epilogue (CompileRange's end, then CBasicBlock::CompileEpilog).
    st(b_.CreateAdd(ld(i32_, L_.pipe_time), b_.getInt32(rel)), L_.pipe_time);
    const unsigned pairs = p.pairs();
    for (unsigned i = pairs; i < 3; ++i) set_bits128(V_.fmac_write + i * 16, llvm::ConstantAggregateZero::get(v4i()));
    for (unsigned extra = 0; extra < 3; ++extra) {
      const std::uint32_t t = rel + extra;
      std::uint32_t stall[4] = {};
      for (unsigned bit = 0; bit < 128; ++bit)
        if (p.write_times[bit / 4][bit & 3] > t) stall[bit / 32] |= 1u << (bit & 31);
      for (unsigned i = 0; i < 4; ++i)
        if (stall[i] != 0) st(b_.getInt32(stall[i]), V_.fmac_write + extra * 16 + i * 4);
    }
    st(b_.getInt32(p.trailing.reg), V_.saved_next_index);

    llvm::Value* quota = b_.CreateSub(ld(i32_, L_.cycle_quota), b_.getInt32((p.end - p.begin) / 4 + 1));
    st(quota, L_.cycle_quota);
    llvm::Value* e = ld(i32_, L_.has_exception);
    e = b_.CreateSelect(b_.CreateICmpSLE(quota, b_.getInt32(0)), b_.CreateOr(e, b_.getInt32(cfg_.quota_done)), e);
    st(e, L_.has_exception);
    llvm::Value* jumped = b_.CreateICmpNE(delayed_, b_.getInt32(cfg_.invalid_pc));
    llvm::Value* next = b_.CreateSelect(jumped, delayed_, b_.getInt32(p.end + 4));
    st(next, L_.pc);
    auto* go_on = llvm::BasicBlock::Create(ctx_, "", fn_);
    b_.CreateCondBr(b_.CreateICmpNE(e, b_.getInt32(0)), exit_, go_on);
    b_.SetInsertPoint(go_on);
    if (continue_with_) {
      b_.CreateCall(continue_with_, {state_, mem_, rt_});
      b_.CreateBr(exit_);
      return;
    }
    const std::uint32_t branch = p.end - 0xC;
    bool dynamic = false;
    std::vector<std::uint32_t> candidates{p.end + 4};
    if (p.end - p.begin >= 0xC) {
      const vu::Pair bp = vu::decode(im_.word(branch), im_.word(branch + 4));
      if (bp.lower_is_branch()) {
        if (bp.lo == Lower::JR || bp.lo == Lower::JALR)
          dynamic = true;
        else
          candidates.push_back(vu::branch_target(branch, bp.lower));
      }
    }
    if (dynamic)
      dispatch(next, {});
    else
      dispatch(next, candidates);
  }

  // Dynamic stalls on writes the previous block left in flight (CompileRange, first 3 pairs).
  void fmac_write_stalls(unsigned index, const vu::Operands& lo, const vu::Operands& hi) {
    std::uint32_t mask[4] = {};
    auto set = [&](unsigned reg, unsigned dest) {
      if (reg != 0) mask[(reg * 4) / 32] |= dest << ((reg * 4) & 31);
    };
    set(hi.read_f0, hi.read_elem_f0);
    set(hi.read_f1, hi.read_elem_f1);
    set(lo.read_f0, lo.read_elem_f0);
    set(lo.read_f1, lo.read_elem_f1);
    for (unsigned i = index; i < 3; ++i)
      for (unsigned w = 0; w < 4; ++w) {
        if (mask[w] == 0) continue;
        const std::uint32_t slot = V_.fmac_write + i * 16;
        llvm::Value* hit = b_.CreateICmpNE(b_.CreateAnd(ld(i32_, slot + w * 4), b_.getInt32(mask[w])), b_.getInt32(0));
        auto* yes = llvm::BasicBlock::Create(ctx_, "", fn_);
        auto* join = llvm::BasicBlock::Create(ctx_, "", fn_);
        b_.CreateCondBr(hit, yes, join);
        b_.SetInsertPoint(yes);
        set_bits128(slot, llvm::ConstantAggregateZero::get(v4i()));
        st(b_.CreateAdd(ld(i32_, L_.pipe_time), b_.getInt32(1)), L_.pipe_time);
        b_.CreateBr(join);
        b_.SetInsertPoint(join);
      }
    set_bits128(V_.fmac_write + index * 16, llvm::ConstantAggregateZero::get(v4i()));
  }

  void upper_word(const vu::Pair& pair) {
    Insn in;
    in.raw = pair.upper;
    in.op = pair.up;
    if (!emit_vu(in)) fail("no translation for the upper op {}", r5900::mnemonic(pair.up));
    if (pair.upper & vu::kBitD) set_exception_if_none(cfg_.exception_dbit);
    if (pair.upper & vu::kBitT) set_exception_if_none(cfg_.exception_tbit);
    if (pair.upper & vu::kBitI) st(b_.getInt32(pair.lower), L_.vi_i);  // LOI
    if (pair.upper & vu::kBitE) st(b_.getInt32(cfg_.exception_ebit), L_.has_exception);
  }

  // Lower ops that are written the same way as the VU0 macro instructions.
  void as_macro(Op op, std::uint32_t lower) {
    Insn in;
    in.raw = lower;
    in.op = op;
    if (!emit_vu(in)) fail("no translation for {}", r5900::mnemonic(op));
  }

  void lower_word(std::uint32_t a, const vu::Pair& pair, std::uint32_t rel) {
    const std::uint32_t lower = pair.lower;
    if (pair.i_bit() || lower == vu::kLowerNop) return;
    const unsigned dest = (lower >> 21) & 15, it = (lower >> 16) & 31, is = (lower >> 11) & 31;
    const std::uint32_t imm15 = (lower & 0x7FF) | ((lower & 0x01E00000) >> 10);
    const std::uint32_t imm24 = lower & 0x00FFFFFF;
    const std::uint32_t link = (a + 0x10) >> 3;
    auto branch = [&](llvm::Value* taken) {
      const std::uint32_t target = vu::branch_target(a, lower);
      delayed_ = taken ? b_.CreateSelect(taken, b_.getInt32(target), delayed_) : b_.getInt32(target);
    };
    auto bool32 = [&](llvm::Value* c) { return b_.CreateZExt(c, i32_); };
    switch (pair.lo) {
      case Lower::LQ:
        if (dest == 0) return;
        load_quad(address(is, imm11(lower), 0), dest, it);
        return;
      case Lower::SQ: store_quad(address(it, imm11(lower), 0), dest, is); return;
      case Lower::ILW:
        if ((it & 15) == 0) return;
        set_vi(it, b_.CreateAlignedLoad(i32_, mem(address(is, imm11(lower), dest_offset(dest))), llvm::MaybeAlign(4)));
        return;
      case Lower::ISW: store_int(address(is, imm11(lower), 0), dest, it); return;
      case Lower::IADDIU:
        if ((it & 15) == 0) return;
        set_vi(it, b_.CreateAdd(vi(is), b_.getInt32(imm15 & 0xFFFF)));
        return;
      case Lower::ISUBIU:
        if ((it & 15) == 0) return;
        set_vi(it, b_.CreateSub(vi(is), b_.getInt32(imm15 & 0xFFFF)));
        return;
      case Lower::FCEQ:
        check_clip(rel);
        set_vi_raw(1, bool32(b_.CreateICmpEQ(b_.CreateAnd(ld(i32_, L_.vcf), b_.getInt32(0xFFFFFF)), b_.getInt32(imm24))));
        return;
      case Lower::FCSET:
        st(b_.getInt32(imm24), L_.vcf);
        pipe_reset(L_.pipe_clip, b_.getInt32(imm24));
        return;
      case Lower::FCAND:
        check_clip(rel);
        set_vi_raw(1, bool32(b_.CreateICmpNE(b_.CreateAnd(ld(i32_, L_.vcf), b_.getInt32(imm24)), b_.getInt32(0))));
        return;
      case Lower::FCOR:
        check_clip(rel);
        set_vi_raw(1, bool32(b_.CreateICmpEQ(
                          b_.CreateAnd(b_.CreateOr(ld(i32_, L_.vcf), b_.getInt32(imm24)), b_.getInt32(0xFFFFFF)),
                          b_.getInt32(0xFFFFFF))));
        return;
      case Lower::FCGET:
        if ((it & 15) == 0) return;
        check_clip(rel);
        set_vi(it, b_.CreateAnd(ld(i32_, L_.vcf), b_.getInt32(0xFFF)));
        return;
      case Lower::B: branch(nullptr); return;
      case Lower::BAL:
        set_vi_raw(it, b_.getInt32(link));
        branch(nullptr);
        return;
      case Lower::JR:
        delayed_ = b_.CreateShl(b_.CreateAnd(vi_raw(is), b_.getInt32(0xFFFF)), 3);
        return;
      case Lower::JALR:
        delayed_ = b_.CreateShl(b_.CreateAnd(vi_raw(is), b_.getInt32(0xFFFF)), 3);
        set_vi_raw(it, b_.getInt32(link));
        return;
      case Lower::IBEQ: case Lower::IBNE: {
        llvm::Value* s = b_.CreateAnd(vi(is), b_.getInt32(0xFFFF));
        llvm::Value* t = b_.CreateAnd(vi(it), b_.getInt32(0xFFFF));
        branch(pair.lo == Lower::IBEQ ? b_.CreateICmpEQ(s, t) : b_.CreateICmpNE(s, t));
        return;
      }
      case Lower::IBLTZ: case Lower::IBGEZ: {
        llvm::Value* neg = b_.CreateICmpNE(b_.CreateAnd(vi_raw(is), b_.getInt32(0x8000)), b_.getInt32(0));
        branch(pair.lo == Lower::IBLTZ ? neg : b_.CreateNot(neg));
        return;
      }
      case Lower::IBGTZ: case Lower::IBLEZ: {
        llvm::Value* v = b_.CreateSExt(b_.CreateTrunc(vi_raw(is), b_.getInt16Ty()), i32_);
        branch(pair.lo == Lower::IBGTZ ? b_.CreateICmpSGT(v, b_.getInt32(0)) : b_.CreateICmpSLE(v, b_.getInt32(0)));
        return;
      }
      case Lower::IADD: as_macro(Op::VIADD, lower); return;
      case Lower::ISUB: as_macro(Op::VISUB, lower); return;
      case Lower::IADDI: as_macro(Op::VIADDI, lower); return;
      case Lower::IAND: as_macro(Op::VIAND, lower); return;
      case Lower::IOR: as_macro(Op::VIOR, lower); return;
      case Lower::MOVE: as_macro(Op::VMOVE, lower); return;
      case Lower::MR32: as_macro(Op::VMR32, lower); return;
      case Lower::MTIR: as_macro(Op::VMTIR, lower); return;
      case Lower::MFIR: as_macro(Op::VMFIR, lower); return;
      case Lower::DIV: as_macro(Op::VDIV, lower); return;
      case Lower::SQRT: as_macro(Op::VSQRT, lower); return;
      case Lower::RSQRT: as_macro(Op::VRSQRT, lower); return;
      case Lower::WAITQ: as_macro(Op::VWAITQ, lower); return;
      case Lower::LQI:
        load_quad(mask(b_.CreateShl(vi(is), 4)), dest, it);
        if (is & 15) set_vi(is, b_.CreateAdd(vi(is), b_.getInt32(1)));
        return;
      case Lower::SQI:
        store_quad(mask(b_.CreateShl(vi(it), 4)), dest, is);
        if (it & 15) set_vi(it, b_.CreateAdd(vi(it), b_.getInt32(1)));
        return;
      case Lower::LQD:
        if (is & 15) set_vi(is, b_.CreateSub(vi(is), b_.getInt32(1)));
        load_quad(mask(b_.CreateShl(vi(is), 4)), dest, it);
        return;
      case Lower::SQD:
        if (it & 15) set_vi(it, b_.CreateSub(vi(it), b_.getInt32(1)));
        store_quad(mask(b_.CreateShl(vi(it), 4)), dest, is);
        return;
      case Lower::ILWR:
        if ((it & 15) == 0) return;
        set_vi(it, b_.CreateAlignedLoad(i32_, mem(mask(b_.CreateAdd(b_.CreateShl(vi(is), 4), b_.getInt32(dest_offset(dest))))),
                                        llvm::MaybeAlign(4)));
        return;
      case Lower::ISWR: store_int(mask(b_.CreateShl(vi(is), 4)), dest, it); return;
      case Lower::RINIT:
        st(b_.CreateAnd(b_.CreateBitCast(lane(is, dest & 3), i32_), b_.getInt32(0x7FFFFF)), L_.vr);
        return;
      case Lower::RXOR:
        st(b_.CreateAnd(b_.CreateXor(b_.CreateBitCast(lane(is, dest & 3), i32_), ld(i32_, L_.vr)), b_.getInt32(0x7FFFFF)),
           L_.vr);
        return;
      case Lower::RNEXT: {
        llvm::Value* r = ld(i32_, L_.vr);
        llvm::Value* bit4 = b_.CreateAnd(b_.CreateLShr(r, 4), b_.getInt32(1));
        llvm::Value* bit22 = b_.CreateAnd(b_.CreateLShr(r, 22), b_.getInt32(1));
        st(b_.CreateAnd(b_.CreateXor(b_.CreateXor(bit4, bit22), b_.CreateShl(r, 1)), b_.getInt32(0x7FFFFF)), L_.vr);
        [[fallthrough]];
      }
      case Lower::RGET: {
        if (it == 0) return;
        llvm::Value* v = b_.CreateOr(ld(i32_, L_.vr), b_.getInt32(0x3F800000));
        vstore(it, b_.CreateBitCast(b_.CreateVectorSplat(4, v), v4f()), dest);
        return;
      }
      case Lower::XTOP: set_vi_raw(it, port_read(cfg_.addr_top)); return;
      case Lower::XITOP:
        if ((it & 15) == 0) return;
        set_vi(it, port_read(cfg_.addr_itop));
        return;
      case Lower::XGKICK: st(vi_raw(is), V_.xgkick_address); return;
      default:
        fail("no translation for the lower op {}", vu::mnemonic(pair.lo));
    }
  }
};

}  // namespace

struct VuTranslator::Impl {
  StateLayout layout;
  VuLayout vu_layout;
  VuConfig config;
  std::unique_ptr<llvm::LLVMContext> context = std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> module;
  std::unique_ptr<llvm::orc::LLJIT> jit;
  std::unique_ptr<llvm::TargetMachine> target;
  std::vector<Image> images;
  std::vector<VuImageCode> code;
  VuTranslateStats stats;
  std::map<std::string, std::size_t> refusals;
  std::map<std::uint32_t, std::string> refused_starts;
  std::string optimised_ir;
  std::map<std::pair<std::size_t, std::uint32_t>, VuImageCode> entries;  // compiled on first use; null: refused
  std::size_t entry_functions = 0;
  double entry_seconds = 0;
};

namespace {

void optimise(llvm::Module& mod, llvm::TargetMachine* target) {
  llvm::PassBuilder passes(target);
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  passes.registerModuleAnalyses(mam);
  passes.registerCGSCCAnalyses(cgam);
  passes.registerFunctionAnalyses(fam);
  passes.registerLoopAnalyses(lam);
  passes.crossRegisterProxies(lam, fam, cgam, mam);
  passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(mod, mam);
}

}  // namespace

VuTranslator::VuTranslator(const StateLayout& layout, const VuLayout& vu_layout, const VuConfig& config)
    : impl_(new Impl{layout, vu_layout, config}) {
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
  impl_->module = std::make_unique<llvm::Module>("hp2vu", *impl_->context);
  impl_->module->setDataLayout(impl_->jit->getDataLayout());
  impl_->module->setTargetTriple(impl_->jit->getTargetTriple());
}

VuTranslator::~VuTranslator() = default;

std::size_t VuTranslator::add_image(std::uint32_t address, std::vector<std::uint32_t> words,
                                    const std::vector<std::uint32_t>& extra_entries) {
  Impl& m = *impl_;
  if (!m.module) fail("compile() was already called");
  if ((address & 7) || (words.size() & 1)) fail("a microcode image must hold whole instruction pairs");
  Image& im = m.images.emplace_back();
  im.address = address;
  im.words = std::move(words);
  // Block starts: the image start, branch targets, and what follows each block.
  Planner planner(im, m.config);
  std::vector<std::uint32_t> work{address};
  for (std::uint32_t a = address; a + 8 <= address + im.words.size() * 4; a += 8) {
    const vu::Pair pair = vu::decode(im.word(a), im.word(a + 4));
    if (pair.lower_is_branch() && pair.lo != Lower::JR && pair.lo != Lower::JALR)
      work.push_back(vu::branch_target(a, pair.lower));
  }
  for (std::uint32_t e : extra_entries) work.push_back(e);
  std::set<std::uint32_t> seen;
  while (!work.empty()) {
    const std::uint32_t start = work.back();
    work.pop_back();
    if (!seen.insert(start).second || !im.contains(start, 8) || (start & 7)) continue;
    try {
      BlockPlan plan = planner.plan(start);
      for (std::uint32_t next : planner.successors(plan)) work.push_back(next);
      im.blocks.emplace(start, std::move(plan));
    } catch (const Unsupported& u) {
      ++m.refusals[u.why];
      m.refused_starts.emplace(start, u.why);
    }
  }
  const std::size_t index = m.images.size() - 1;
  MicroLifter(*m.module, m.layout, m.vu_layout, m.config, im, m.stats).lift(function_name(index), im.blocks);
  return index;
}

void VuTranslator::compile() {
  Impl& m = *impl_;
  llvm::Module& mod = *m.module;
  if (llvm::verifyModule(mod, &llvm::errs())) fail("the translated microcode does not verify");
  optimise(mod, m.target.get());
  {
    llvm::raw_string_ostream out(m.optimised_ir);
    mod.print(out, nullptr);
  }
  check(m.jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(m.module), std::move(m.context))), "adding the module");
  for (std::size_t i = 0; i < m.images.size(); ++i) {
    auto address = check(m.jit->lookup(function_name(i)), "looking up an image");
    m.code.push_back(address.toPtr<VuImageCode>());
  }
}

VuImageCode VuTranslator::entry(std::size_t image, std::uint32_t address) {
  Impl& m = *impl_;
  if (image >= m.code.size()) return nullptr;
  const auto key = std::make_pair(image, address);
  if (const auto it = m.entries.find(key); it != m.entries.end()) return it->second;
  const auto t0 = std::chrono::steady_clock::now();
  Image& im = m.images[image];
  std::map<std::uint32_t, BlockPlan> blocks;
  try {
    if (!im.contains(address, 8) || (address & 7)) throw Unsupported{"outside the image"};
    blocks.emplace(address, Planner(im, m.config).plan(address));
  } catch (const Unsupported& u) {
    ++m.refusals["entry: " + u.why];
    m.refused_starts.emplace(address, "entry: " + u.why);
    m.entries.emplace(key, nullptr);
    return nullptr;
  }
  auto context = std::make_unique<llvm::LLVMContext>();
  auto module = std::make_unique<llvm::Module>("hp2vu_entry", *context);
  module->setDataLayout(m.jit->getDataLayout());
  module->setTargetTriple(m.jit->getTargetTriple());
  auto* ptr = llvm::PointerType::get(*context, 0);
  llvm::Function* image_fn = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {ptr, ptr, ptr}, false), llvm::Function::ExternalLinkage,
      function_name(image), *module);
  const std::string name = std::format("vu_entry_{}_{:04X}", image, address);
  VuTranslateStats stats;
  MicroLifter(*module, m.layout, m.vu_layout, m.config, im, stats).lift(name, blocks, image_fn);
  if (llvm::verifyModule(*module, &llvm::errs())) fail("a translated microcode entry does not verify");
  optimise(*module, m.target.get());
  check(m.jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))), "adding an entry");
  const auto code = check(m.jit->lookup(name), "looking up an entry").toPtr<VuImageCode>();
  m.entries.emplace(key, code);
  ++m.entry_functions;
  m.entry_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return code;
}

std::size_t VuTranslator::entry_functions() const { return impl_->entry_functions; }
double VuTranslator::entry_seconds() const { return impl_->entry_seconds; }

std::size_t VuTranslator::images() const { return impl_->images.size(); }
std::uint32_t VuTranslator::image_address(std::size_t image) const { return impl_->images.at(image).address; }
const std::vector<std::uint32_t>& VuTranslator::image_words(std::size_t image) const {
  return impl_->images.at(image).words;
}
VuImageCode VuTranslator::code(std::size_t image) const {
  return image < impl_->code.size() ? impl_->code[image] : nullptr;
}
bool VuTranslator::is_block_start(std::size_t image, std::uint32_t address) const {
  return impl_->images.at(image).blocks.count(address) != 0;
}
const VuTranslateStats& VuTranslator::stats() const { return impl_->stats; }
const std::map<std::string, std::size_t>& VuTranslator::refusals() const { return impl_->refusals; }
const std::map<std::uint32_t, std::string>& VuTranslator::refused_starts() const { return impl_->refused_starts; }
std::string VuTranslator::ir() const {
  if (impl_->module) {
    std::string text;
    llvm::raw_string_ostream out(text);
    impl_->module->print(out, nullptr);
    return text;
  }
  return impl_->optimised_ir;
}

}  // namespace hp2
