// VU1 microcode translator tests on hand-assembled microcode (no game data):
// translate a small image, run it on a state structure laid out like the
// reference core's, check results and the pipeline model's visible effects
// (stall timing, Q and clip latencies, the integer-branch rule, xgkick order,
// the cycle quota). The behaviour itself was checked against the reference
// core on the game's own microcode (docs/recomp-analysis.md section 13); these
// keep it pinned.
#include "minitest.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include "hp2/vulift.h"

using namespace hp2;

namespace {

struct alignas(16) VuState {
  std::uint32_t pc, delayed, exception;
  std::int32_t quota;
  float vf[32][4];
  float acc[4];
  std::uint32_t vi[16];
  std::uint32_t q, i, r, cf, mf, df, t;
  std::uint32_t pipe_q[2];
  std::uint32_t pipe_mac[17], pipe_sticky[17], pipe_clip[17];
  std::uint32_t pipe_time;
  std::uint32_t saved_index, saved_value, xgkick;
  alignas(16) std::uint32_t fmac_write[12];
};

constexpr std::uint32_t kEbit = 10, kQuotaDone = 0x80, kInvalidPc = 1;

// ---- encoders ----
constexpr std::uint32_t kE = 1u << 30;
constexpr std::uint32_t unop = 0x000002FF;
constexpr std::uint32_t lnop = 0x8000033C;
constexpr std::uint32_t up(std::uint32_t fn, unsigned dest, unsigned ft, unsigned fs, unsigned fd) {
  return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn;
}
constexpr std::uint32_t vadd(unsigned dest, unsigned fd, unsigned fs, unsigned ft) { return up(0x28, dest, ft, fs, fd); }
constexpr std::uint32_t vmulq(unsigned dest, unsigned fd, unsigned fs) { return up(0x1C, dest, 0, fs, fd); }
constexpr std::uint32_t vclip(unsigned fs, unsigned ft) { return (0xEu << 21) | (ft << 16) | (fs << 11) | 0x1FF; }
constexpr std::uint32_t lq(unsigned dest, unsigned ft, unsigned base, int imm) {
  return (0x00u << 25) | (dest << 21) | (ft << 16) | (base << 11) | (static_cast<std::uint32_t>(imm) & 0x7FF);
}
constexpr std::uint32_t sq(unsigned dest, unsigned fs, unsigned base, int imm) {
  return (0x01u << 25) | (dest << 21) | (base << 16) | (fs << 11) | (static_cast<std::uint32_t>(imm) & 0x7FF);
}
constexpr std::uint32_t iaddiu(unsigned it, unsigned is, std::uint32_t imm) {
  return (0x08u << 25) | (((imm >> 11) & 0xF) << 21) | (it << 16) | (is << 11) | (imm & 0x7FF);
}
constexpr std::uint32_t isubiu(unsigned it, unsigned is, std::uint32_t imm) {
  return (0x09u << 25) | (((imm >> 11) & 0xF) << 21) | (it << 16) | (is << 11) | (imm & 0x7FF);
}
// ibne: `at` is the branch's address, `target` where it goes
constexpr std::uint32_t ibne(unsigned it, unsigned is, std::uint32_t at, std::uint32_t target) {
  return (0x29u << 25) | (it << 16) | (is << 11) | ((static_cast<std::uint32_t>(static_cast<std::int32_t>(target - at - 8) / 8)) & 0x7FF);
}
constexpr std::uint32_t div(unsigned fs, unsigned fsf, unsigned ft, unsigned ftf) {
  return 0x80000000u | (ftf << 23) | (fsf << 21) | (ft << 16) | (fs << 11) | (0x0Eu << 6) | 0x3C;
}
constexpr std::uint32_t waitq = 0x800003BF;
constexpr std::uint32_t fcget(unsigned it) { return (0x1Cu << 25) | (it << 16); }
constexpr std::uint32_t xgkick(unsigned is) { return 0x800006FC | (is << 11); }

std::uint32_t bits(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

struct Kick {
  std::uint32_t value, word;
};
std::vector<Kick> g_kicks;
std::uint8_t* g_mem = nullptr;
std::uint32_t PortRead(std::uint8_t*, std::uint32_t) { return 0x1234; }
void PortWrite(std::uint8_t*, std::uint32_t, std::uint32_t value) {
  std::uint32_t word;
  std::memcpy(&word, g_mem + 0x100, 4);
  g_kicks.push_back({value, word});
}
const VuRuntime g_runtime{&PortRead, &PortWrite};

struct Fixture {
  VuTranslator t;
  std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(0x4000, 0);
  VuState s{};

  static StateLayout layout() {
    StateLayout l;
    l.pc = offsetof(VuState, pc);
    l.cycle_quota = offsetof(VuState, quota);
    l.has_exception = offsetof(VuState, exception);
    l.vf = offsetof(VuState, vf);
    l.vacc = offsetof(VuState, acc);
    l.vi = offsetof(VuState, vi);
    l.vq = offsetof(VuState, q);
    l.vi_i = offsetof(VuState, i);
    l.vr = offsetof(VuState, r);
    l.vcf = offsetof(VuState, cf);
    l.vmf = offsetof(VuState, mf);
    l.vdf = offsetof(VuState, df);
    l.vt = offsetof(VuState, t);
    l.pipe_q = offsetof(VuState, pipe_q);
    l.pipe_mac = offsetof(VuState, pipe_mac);
    l.pipe_sticky = offsetof(VuState, pipe_sticky);
    l.pipe_clip = offsetof(VuState, pipe_clip);
    l.pipe_time = offsetof(VuState, pipe_time);
    return l;
  }
  static VuLayout vu_layout() {
    VuLayout v;
    v.delayed_jump = offsetof(VuState, delayed);
    v.fmac_write = offsetof(VuState, fmac_write);
    v.saved_next_index = offsetof(VuState, saved_index);
    v.saved_next_value = offsetof(VuState, saved_value);
    v.xgkick_address = offsetof(VuState, xgkick);
    return v;
  }
  static VuConfig config() {
    VuConfig c;
    c.exception_dbit = 8;
    c.exception_tbit = 9;
    c.exception_ebit = kEbit;
    c.quota_done = kQuotaDone;
    c.invalid_pc = kInvalidPc;
    return c;
  }

  explicit Fixture(std::vector<std::uint32_t> pairs) : t(layout(), vu_layout(), config()) {
    t.add_image(0, std::move(pairs));
    t.compile();
    s.vf[0][3] = 1.0f;
    s.delayed = kInvalidPc;
    g_mem = mem.data();
    g_kicks.clear();
  }

  // As the reference core's executor: blocks until the exception word is set.
  void run(std::uint32_t pc, std::int32_t quota = 5000) {
    s.pc = pc;
    s.quota = quota;
    s.exception = 0;
    while (s.exception == 0) {
      const VuImageCode code = t.is_block_start(0, s.pc) ? t.code(0) : t.entry(0, s.pc);
      if (!code) break;
      code(reinterpret_cast<std::uint8_t*>(&s), mem.data(), &g_runtime);
    }
  }
  float mem_float(std::uint32_t a) const {
    float f;
    std::memcpy(&f, &mem[a], 4);
    return f;
  }
};

}  // namespace

TEST(vu1_program_runs_to_its_end_bit_with_the_reference_stall_timing) {
  Fixture f({lq(0xF, 1, 0, 0), unop,         // vf1 = mem[0]
             lnop, vadd(0xF, 2, 1, 1),       // vf2 = vf1 + vf1 (stalls 3 cycles on vf1)
             sq(0xF, 2, 0, 1), unop | kE,    // mem[16] = vf2 (stalls 3 cycles on vf2); end
             lnop, unop});                   // end bit's delay slot
  const float in[4] = {1, 2, 3, 4};
  std::memcpy(f.mem.data(), in, 16);
  f.run(0);
  CHECK_EQ(f.s.exception, kEbit);
  CHECK_EQ(f.s.pc, 0x20u);
  CHECK_EQ(f.mem_float(16), 2.0f);
  CHECK_EQ(f.mem_float(28), 8.0f);
  CHECK_EQ(f.s.pipe_time, 10u);   // 4 pairs + 3 + 3 stall cycles
  CHECK_EQ(f.s.quota, 5000 - 8);  // 8 words
  for (std::uint32_t w : f.s.fmac_write) CHECK_EQ(w, 0u);
}

TEST(vu1_q_is_visible_after_its_latency_or_waitq) {
  Fixture f({div(1, 0, 2, 0), unop,       // Q <- vf1.x / vf2.x, due 7 cycles later
             lnop, vmulq(0x8, 3, 4),      // vf3.x = vf4.x * Q: still the old Q
             waitq, unop,                 // wait for the division
             lnop, vmulq(0x4, 3, 4) | kE, // vf3.y = vf4.y * Q: the new Q
             lnop, unop});
  f.s.vf[1][0] = 6;
  f.s.vf[2][0] = 3;
  for (float& v : f.s.vf[4]) v = 1;
  f.s.q = bits(10.0f);
  f.s.pipe_q[1] = bits(10.0f);
  f.run(0);
  CHECK_EQ(f.s.exception, kEbit);
  CHECK_EQ(f.s.vf[3][0], 10.0f);
  CHECK_EQ(f.s.vf[3][1], 2.0f);
  CHECK_EQ(f.s.q, bits(2.0f));
  CHECK_EQ(f.s.pipe_q[0], 0u);
}

TEST(vu1_loop_gives_up_at_the_cycle_quota_and_continues) {
  Fixture f({iaddiu(1, 0, 100), unop,          // 00
             isubiu(1, 1, 1), unop,            // 08 loop
             lnop, unop,                       // 10
             ibne(1, 0, 0x18, 0x08), unop,     // 18
             lnop, unop,                       // 20 delay slot
             lnop, unop | kE,                  // 28
             lnop, unop});                     // 30
  f.run(0, 20);  // block 00 (10 words), block 08 (8 words), block 08 again: quota used up
  CHECK_EQ(f.s.exception, kQuotaDone);
  CHECK_EQ(f.s.pc, 0x08u);
  CHECK_EQ(f.s.vi[1], 97u);
  f.run(f.s.pc, 1 << 30);
  CHECK_EQ(f.s.exception, kEbit);
  CHECK_EQ(f.s.pc, 0x38u);
  CHECK_EQ(f.s.vi[1], 0u);
}

TEST(vu1_branch_right_after_its_register_is_written_sees_the_earlier_value) {
  // The reference core's rule (CVuBasicBlock::ComputeIntegerBranchDelayInfo):
  // the branch reads vi1 as it was 4 pairs earlier or at the block start.
  Fixture f({iaddiu(1, 0, 100), unop,          // 00
             isubiu(1, 1, 1), unop,            // 08
             ibne(1, 0, 0x10, 0x00), unop,     // 10: sees vi1 = 0 (block start), not 99
             lnop, unop,                       // 18 delay slot
             lnop, unop | kE,                  // 20
             lnop, unop});                     // 28
  f.run(0);
  CHECK_EQ(f.s.exception, kEbit);
  CHECK_EQ(f.s.pc, 0x30u);
  CHECK_EQ(f.s.vi[1], 99u);
}

TEST(vu1_entry_where_no_block_starts) {
  Fixture f({lq(0xF, 1, 0, 0), unop,
             lnop, vadd(0xF, 2, 1, 1),
             sq(0xF, 2, 0, 1), unop | kE,
             lnop, unop});
  for (float& v : f.s.vf[1]) v = 1;
  CHECK(!f.t.is_block_start(0, 0x08));
  f.s.pc = 0x08;
  f.s.exception = 0;
  f.t.code(0)(reinterpret_cast<std::uint8_t*>(&f.s), f.mem.data(), &g_runtime);  // not a block start: returns at once
  CHECK_EQ(f.s.pc, 0x08u);
  CHECK_EQ(f.s.exception, 0u);
  REQUIRE(f.t.entry(0, 0x08) != nullptr);
  f.run(0x08);
  CHECK_EQ(f.s.exception, kEbit);
  CHECK_EQ(f.mem_float(16), 2.0f);
  CHECK_EQ(f.t.entry_functions(), 1u);
}

TEST(vu1_xgkick_happens_after_the_next_pairs_lower_op) {
  Fixture f({xgkick(1), unop,              // 00: kick at vi1 (pending)
             sq(0xF, 1, 2, 0), unop,       // 08: store vf1 at 0x100 - before the kick reaches the GIF
             lnop, unop | kE,
             lnop, unop});
  f.s.vi[1] = 5;
  f.s.vi[2] = 0x10;
  f.s.vf[1][0] = 7;
  f.run(0);
  REQUIRE(g_kicks.size() == 1);
  CHECK_EQ(g_kicks[0].value, 5u);
  CHECK_EQ(g_kicks[0].word, bits(7.0f));
  CHECK_EQ(f.s.xgkick, 5u);
}

TEST(vu1_clip_flags_are_visible_after_4_cycles) {
  Fixture f({lnop, vclip(1, 2),            // 00: clip vf1 against |vf2.w|
             fcget(2), unop,               // 08: too early
             lnop, unop,
             lnop, unop,
             lnop, unop,
             fcget(3), unop,               // 28: 5 cycles later
             lnop, unop | kE,
             lnop, unop});
  f.s.vf[1][0] = 2;
  f.s.vf[1][1] = -2;
  f.s.vf[1][2] = 0.5f;
  f.s.vf[2][3] = 1;
  f.run(0);
  CHECK_EQ(f.s.vi[2], 0u);
  CHECK_EQ(f.s.vi[3], 9u);  // +x and -y
  CHECK_EQ(f.s.cf, 9u);
}

MINITEST_MAIN()
