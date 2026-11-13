// Translator tests on a hand-assembled program (no game data): lift leaf
// functions with LLVM, run the generated x64 code on a state structure and a
// small memory, check the results against what the instructions mean.
#include "minitest.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "hp2/lift.h"

using namespace hp2;

namespace {

constexpr unsigned zero = 0, v0 = 2, v1 = 3, a0 = 4, a1 = 5, a2 = 6, t0 = 8, t1 = 9, ra = 31;

constexpr std::uint32_t itype(unsigned op, unsigned rs, unsigned rt, int imm) {
  return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<std::uint32_t>(imm) & 0xFFFF);
}
constexpr std::uint32_t rtype(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned fn) {
  return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn;
}
constexpr std::uint32_t nop = 0;
constexpr std::uint32_t addiu(unsigned rt, unsigned rs, int i) { return itype(0x09, rs, rt, i); }
constexpr std::uint32_t lw(unsigned rt, int off, unsigned base) { return itype(0x23, base, rt, off); }
constexpr std::uint32_t sw(unsigned rt, int off, unsigned base) { return itype(0x2B, base, rt, off); }
constexpr std::uint32_t lq(unsigned rt, int off, unsigned base) { return itype(0x1E, base, rt, off); }
constexpr std::uint32_t sq(unsigned rt, int off, unsigned base) { return itype(0x1F, base, rt, off); }
constexpr std::uint32_t jr(unsigned rs) { return rtype(rs, 0, 0, 0, 0x08); }
constexpr std::uint32_t addu(unsigned rd, unsigned rs, unsigned rt) { return rtype(rs, rt, rd, 0, 0x21); }
constexpr std::uint32_t daddu(unsigned rd, unsigned rs, unsigned rt) { return rtype(rs, rt, rd, 0, 0x2D); }
constexpr std::uint32_t mult(unsigned rd, unsigned rs, unsigned rt) { return rtype(rs, rt, rd, 0, 0x18); }
constexpr std::uint32_t div_(unsigned rs, unsigned rt) { return rtype(rs, rt, 0, 0, 0x1A); }
constexpr std::uint32_t mflo(unsigned rd) { return rtype(0, 0, rd, 0, 0x12); }
constexpr std::uint32_t mfhi(unsigned rd) { return rtype(0, 0, rd, 0, 0x10); }
constexpr std::uint32_t branch(unsigned op, unsigned rs, unsigned rt, std::uint32_t pc, std::uint32_t target) {
  return itype(op, rs, rt, static_cast<int>(target - (pc + 4)) / 4);
}
constexpr std::uint32_t mtc1(unsigned rt, unsigned fs) { return 0x44800000u | (rt << 16) | (fs << 11); }
constexpr std::uint32_t mfc1(unsigned rt, unsigned fs) { return 0x44000000u | (rt << 16) | (fs << 11); }
constexpr std::uint32_t div_s(unsigned fd, unsigned fs, unsigned ft) { return 0x46000003u | (ft << 16) | (fs << 11) | (fd << 6); }
constexpr std::uint32_t c_lt_s(unsigned fs, unsigned ft) { return 0x46000034u | (ft << 16) | (fs << 11); }

constexpr std::uint32_t kText = 0x00100000;
constexpr std::uint32_t kSum = 0x100000, kDiv = 0x100028, kFp = 0x100040, kQuad = 0x100058, kLoad = 0x100064;

std::vector<std::uint32_t> program() {
  return {
      // kSum: v0 = sum of a1 words at a0
      /* 100000 */ daddu(v0, zero, zero),
      /* 100004 */ branch(0x06, a1, 0, 0x100004, 0x100020),  // blez a1, end
      /* 100008 */ nop,
      /* 10000C */ lw(t0, 0, a0),
      /* 100010 */ addiu(a1, a1, -1),
      /* 100014 */ addiu(a0, a0, 4),
      /* 100018 */ branch(0x05, a1, zero, 0x100018, 0x10000C),  // bne a1, zero, loop
      /* 10001C */ addu(v0, v0, t0),  // delay slot
      /* 100020 */ jr(ra),
      /* 100024 */ nop,
      // kDiv: v0 = a0 / a1, v1 = a0 % a1, a2 = a0 * a1 (mult with rd)
      /* 100028 */ div_(a0, a1),
      /* 10002C */ mflo(v0),
      /* 100030 */ mfhi(v1),
      /* 100034 */ mult(a2, a0, a1),
      /* 100038 */ jr(ra),
      /* 10003C */ nop,
      // kFp: v0 = bits(f(a0) / f(a1)), condition = f(a0) < f(a1)
      /* 100040 */ mtc1(a0, 0),
      /* 100044 */ mtc1(a1, 1),
      /* 100048 */ div_s(2, 0, 1),
      /* 10004C */ c_lt_s(0, 1),
      /* 100050 */ jr(ra),
      /* 100054 */ mfc1(v0, 2),
      // kQuad: copy 16 bytes from a0 to a0 + 16
      /* 100058 */ lq(t1, 0, a0),
      /* 10005C */ jr(ra),
      /* 100060 */ sq(t1, 16, a0),
      // kLoad: v0 = word at a0
      /* 100064 */ jr(ra),
      /* 100068 */ lw(v0, 0, a0),
      // a caller, so that the functions above are call targets
      /* 10006C */ 0x0C000000u | (kSum >> 2), nop, 0x0C000000u | (kDiv >> 2), nop,
      /* 10007C */ 0x0C000000u | (kFp >> 2), nop, 0x0C000000u | (kQuad >> 2), nop,
      /* 10008C */ 0x0C000000u | (kLoad >> 2), nop, jr(ra), nop,
  };
}

void put16(Bytes& b, std::size_t off, std::uint16_t v) {
  b[off] = static_cast<std::uint8_t>(v);
  b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(Bytes& b, std::size_t off, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// A minimal executable: one code segment with a .text section.
Bytes make_elf(const std::vector<std::uint32_t>& text, std::uint32_t entry) {
  const std::string names = std::string("\0.text\0.shstrtab\0", 17);
  const std::uint32_t text_off = 0x100, text_size = static_cast<std::uint32_t>(text.size() * 4);
  const std::uint32_t str_off = text_off + text_size;
  const std::uint32_t sh_off = (str_off + static_cast<std::uint32_t>(names.size()) + 3) & ~3u;
  Bytes b(sh_off + 3 * 40, 0);
  b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 1; b[5] = 1; b[6] = 1;
  put16(b, 16, 2); put16(b, 18, 8); put32(b, 20, 1); put32(b, 24, entry);
  put32(b, 28, 52); put32(b, 32, sh_off); put16(b, 40, 52);
  put16(b, 42, 32); put16(b, 44, 1); put16(b, 46, 40); put16(b, 48, 3); put16(b, 50, 2);
  put32(b, 52, 1); put32(b, 56, text_off); put32(b, 60, kText); put32(b, 64, kText);
  put32(b, 68, text_size); put32(b, 72, text_size); put32(b, 76, 5);
  for (std::size_t i = 0; i < text.size(); ++i) put32(b, text_off + i * 4, text[i]);
  std::copy(names.begin(), names.end(), b.begin() + str_off);
  auto section = [&](unsigned index, std::uint32_t name, std::uint32_t type, std::uint32_t flags, std::uint32_t addr,
                     std::uint32_t off, std::uint32_t size) {
    const std::size_t p = sh_off + index * 40;
    put32(b, p, name); put32(b, p + 4, type); put32(b, p + 8, flags); put32(b, p + 12, addr);
    put32(b, p + 16, off); put32(b, p + 20, size);
  };
  section(1, 1, 1, 0x6, kText, text_off, text_size);
  section(2, 7, 3, 0, 0, str_off, static_cast<std::uint32_t>(names.size()));
  return b;
}

struct State {
  std::uint32_t pc, pad[3];
  std::uint64_t gpr[32][2];
  std::uint64_t hi, lo;
  std::uint32_t fpr[32];
  std::uint32_t fcsr, acc;
};

StateLayout layout() {
  StateLayout l;
  l.pc = offsetof(State, pc);
  l.gpr = offsetof(State, gpr);
  l.gpr_stride = 16;
  l.hi = offsetof(State, hi);
  l.lo = offsetof(State, lo);
  l.fpr = offsetof(State, fpr);
  l.fcsr = offsetof(State, fcsr);
  l.fpacc = offsetof(State, acc);
  return l;
}

constexpr std::uint32_t kRam = 1 << 16;

struct Fixture {
  ElfImage elf = ElfImage::parse(make_elf(program(), 0x10006C));
  Discovery d = discover(elf);
  Translator t{elf, d, layout(), kRam};
  std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(kRam, 0);
  std::optional<std::string> caller_refused;
  Fixture() {
    for (std::uint32_t f : {kSum, kDiv, kFp, kQuad, kLoad}) {
      const auto why = t.add_function(f);
      if (why) std::printf("    could not lift %08X: %s\n", f, why->c_str());
    }
    caller_refused = t.add_function(0x10006C);
    if (std::getenv("HP2_DUMP_IR")) std::fprintf(stderr, "%s", t.ir().c_str());
    t.compile();
  }
  State call(std::uint32_t f, State s) {
    s.gpr[ra][0] = 0x00100090;
    t.lookup(f)(reinterpret_cast<std::uint8_t*>(&s), ram.data());
    return s;
  }
};

Fixture& fixture() {
  static Fixture f;
  return f;
}

std::uint32_t bits(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

}  // namespace

TEST(lift_all_test_functions) {
  Fixture& f = fixture();
  CHECK_EQ(f.t.size(), static_cast<std::size_t>(5));
  for (std::uint32_t fn : {kSum, kDiv, kFp, kQuad, kLoad}) CHECK(f.t.lookup(fn) != nullptr);
  // The caller is not a leaf function; nothing can be added after compile().
  REQUIRE(f.caller_refused.has_value());
  std::printf("    caller refused: %s\n", f.caller_refused->c_str());
  CHECK(f.caller_refused->find("call at") != std::string::npos);
  CHECK(f.t.add_function(kSum).has_value());
}

TEST(lift_loop_with_delay_slots) {
  Fixture& f = fixture();
  for (std::uint32_t i = 0; i < 10; ++i) {
    const std::uint32_t v = (i + 1) * 1000;
    std::memcpy(&f.ram[0x1000 + 4 * i], &v, 4);
  }
  State s{};
  s.gpr[a0][0] = 0x1000;
  s.gpr[a1][0] = 10;
  s.gpr[a0][1] = 0x1234;  // upper 64 bits must be left alone
  State r = f.call(kSum, s);
  CHECK_EQ(r.pc, 0x00100090u);
  CHECK_EQ(r.gpr[v0][0], 55000ull);
  CHECK_EQ(r.gpr[a1][0], 0ull);
  CHECK_EQ(r.gpr[a0][0], 0x1028ull);
  CHECK_EQ(r.gpr[a0][1], 0x1234ull);
  s.gpr[a1][0] = 0;  // blez taken: nothing summed
  r = f.call(kSum, s);
  CHECK_EQ(r.gpr[v0][0], 0ull);
}

TEST(lift_division_and_multiplication_like_the_reference) {
  Fixture& f = fixture();
  State s{};
  s.gpr[a0][0] = static_cast<std::uint64_t>(-7);
  s.gpr[a1][0] = 2;
  State r = f.call(kDiv, s);
  CHECK_EQ(r.gpr[v0][0], static_cast<std::uint64_t>(-3));
  CHECK_EQ(r.gpr[v1][0], static_cast<std::uint64_t>(-1));
  CHECK_EQ(r.gpr[a2][0], static_cast<std::uint64_t>(-14));  // mult writes rd on this processor
  s.gpr[a1][0] = 0;  // division by zero: lo = 1 for a negative dividend, hi = dividend
  r = f.call(kDiv, s);
  CHECK_EQ(r.gpr[v0][0], 1ull);
  CHECK_EQ(r.gpr[v1][0], static_cast<std::uint64_t>(-7));
  s.gpr[a0][0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(INT32_MIN));
  s.gpr[a1][0] = static_cast<std::uint64_t>(-1);  // overflow: no crash, lo = 0x80000000, hi = 0
  r = f.call(kDiv, s);
  CHECK_EQ(r.gpr[v0][0], 0xFFFFFFFF80000000ull);
  CHECK_EQ(r.gpr[v1][0], 0ull);
}

TEST(lift_fpu_division_and_compare) {
  Fixture& f = fixture();
  State s{};
  s.gpr[a0][0] = bits(1.0f);
  s.gpr[a1][0] = bits(4.0f);
  State r = f.call(kFp, s);
  CHECK_EQ(static_cast<std::uint32_t>(r.gpr[v0][0]), bits(0.25f));
  CHECK((r.fcsr & 0x00800000) != 0);  // 1 < 4
  s.gpr[a0][0] = bits(-3.0f);
  s.gpr[a1][0] = 0;  // division by zero: largest finite value with the quotient's sign
  r = f.call(kFp, s);
  CHECK_EQ(static_cast<std::uint32_t>(r.gpr[v0][0]), 0xFF7FFFFFu);
  s.gpr[a0][0] = 0x7F800000;  // +infinity is clamped to the largest finite value first
  s.gpr[a1][0] = bits(1.0f);
  r = f.call(kFp, s);
  CHECK_EQ(static_cast<std::uint32_t>(r.gpr[v0][0]), 0x7F7FFFFFu);
  CHECK((r.fcsr & 0x00800000) == 0);
}

TEST(lift_128_bit_load_and_store) {
  Fixture& f = fixture();
  for (int i = 0; i < 16; ++i) f.ram[0x2000 + i] = static_cast<std::uint8_t>(i + 1);
  State s{};
  s.gpr[a0][0] = 0x2008;  // lq/sq ignore the low four address bits
  f.call(kQuad, s);
  CHECK(std::memcmp(&f.ram[0x2000], &f.ram[0x2010], 16) == 0);
}

TEST(lift_traps_outside_main_memory) {
  Fixture& f = fixture();
  State s{};
  s.gpr[a0][0] = 0x3000;
  const std::uint32_t word = 0x5A;
  std::memcpy(&f.ram[0x3000], &word, 4);
  State r = f.call(kLoad, s);
  CHECK_EQ(r.gpr[v0][0], 0x5Aull);
  s.gpr[a0][0] = 0x20003000;  // uncached mirror of main memory
  r = f.call(kLoad, s);
  CHECK_EQ(r.gpr[v0][0], 0x5Aull);
  s.gpr[a0][0] = 0x70000000;  // scratchpad: not modelled by the spike
  r = f.call(kLoad, s);
  CHECK_EQ(r.pc, kTrapPc);
}

MINITEST_MAIN()
