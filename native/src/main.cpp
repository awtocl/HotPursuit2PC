// HotPursuit2Native - the native runtime (ADR 0012). No emulator code: this
// program links none of the reference core. It loads the game's executable
// from the user's ROM folder into its own memory, loads the code package the
// translator built for its own state layout (hp2code.dll), and runs the game
// program as native x64 code. What the PlayStation 2 provided around the
// program is provided here, as host code:
//   - the kernel services the game calls (threads, semaphores, interrupt
//     handlers, vertical blank) - EE threads are Windows fibers;
//   - the hardware registers it touches (timers, interrupt and DMA
//     controllers, graphics status) - as far as implemented, logged otherwise;
//   - the system libraries' RPC calls to the I/O processor (ADR 0011) - host
//     services at the library's entry functions.
// Development build: it reports how far the game gets and why it stops.
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "hp2/code_package.h"
#include "hp2/elf.h"
#include "hp2/io.h"
#include "hp2/native_state.h"
#include "hp2/r5900.h"

using namespace hp2;

namespace {

constexpr std::uint32_t kReturn = 0xFFFFFFF0u;  // return address of threads and handlers: back to the runtime
constexpr int kSliceQuota = 20000;

using EeCode = void (*)(std::uint8_t* state, std::uint8_t* ram, std::uint8_t* spr, const void* runtime);

FILE* g_log = nullptr;
bool g_verbose = false;
void Log(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof text, format, args);
  va_end(args);
  printf("%s\n", text);
  if (g_log) {
    fprintf(g_log, "%s\n", text);
    fflush(g_log);
  }
}
std::set<std::string> g_once;
void LogOnce(const std::string& key, const char* format, ...) {
  if (!g_once.insert(key).second) return;
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof text, format, args);
  va_end(args);
  Log("%s", text);
}

// ---- memory ----------------------------------------------------------------
std::uint8_t* g_ram = nullptr;
std::uint8_t* g_spr = nullptr;
std::uint32_t Phys(std::uint32_t a) { return a & 0x1FFFFFFF; }
bool InRam(std::uint32_t a, std::uint32_t size = 4) { return Phys(a) + size <= kNativeRamSize; }
template <class T>
T Rd(std::uint32_t a) {
  T v{};
  if (InRam(a, sizeof(T))) memcpy(&v, g_ram + Phys(a), sizeof(T));
  return v;
}
template <class T>
void Wr(std::uint32_t a, T v) {
  if (InRam(a, sizeof(T))) memcpy(g_ram + Phys(a), &v, sizeof(T));
}
std::string GuestString(std::uint32_t a, std::size_t max = 200) {
  std::string s;
  for (; s.size() < max && InRam(a, 1) && g_ram[Phys(a)]; ++a) s += static_cast<char>(g_ram[Phys(a)]);
  return s;
}

// ---- code package ------------------------------------------------------------
const Hp2CodeTable* g_table = nullptr;
void* FindCode(std::uint32_t pc) {
  auto find = [&](const Hp2CodeEntry* list, std::uint32_t n) -> void* {
    const auto* end = list + n;
    const auto* it = std::lower_bound(list, end, pc, [](const Hp2CodeEntry& e, std::uint32_t a) { return e.address < a; });
    return it != end && it->address == pc ? it->code : nullptr;
  };
  if (void* c = find(g_table->functions, g_table->function_count)) return c;
  return find(g_table->entries, g_table->entry_count);
}

// ---- kernel objects ------------------------------------------------------------
enum ThreadStatus { kRun = 1, kReady = 2, kWait = 4, kSuspend = 8, kDormant = 0x10 };
struct Thread {
  int id = 0;
  bool used = false;
  NativeState s;
  int status = kDormant;
  int priority = 0;
  std::uint32_t entry = 0, stack = 0, stack_size = 0, gp = 0;
  int wakeups = 0;
  int wait_sema = -1;
  bool sleeping = false;
  void* fiber = nullptr;
};
struct Sema {
  bool used = false;
  int count = 0, max = 0;
  std::deque<int> waiters;
};
struct Handler {
  int id;
  std::uint32_t cause, function, arg;
};
std::vector<Thread*> g_threads;  // index = id
std::vector<Sema> g_semas(1);
std::vector<Handler> g_intc, g_dmac;
int g_nextHandler = 1;
Thread* g_current = nullptr;   // thread whose fiber is running (also while a handler interrupts it: null then)
bool g_inHandler = false;
void* g_mainFiber = nullptr;
bool g_resched = false;
std::uint32_t g_status = 0x70030C13;  // COP0 Status; bit 16 = EIE
std::uint32_t g_intcMask = 0, g_intcStat = 0, g_dmacStat = 0;
std::uint32_t g_vsyncFlag1 = 0, g_vsyncFlag2 = 0;
std::uint32_t g_heapStart = 0, g_heapEnd = 0;
std::deque<std::pair<std::uint32_t, std::uint32_t>> g_pendingDmac;  // channel, unused
std::map<std::uint32_t, std::uint32_t> g_hw;                       // hardware registers as last written
std::uint64_t g_vblanks = 0, g_steps = 0, g_syscalls = 0, g_rpcCalls = 0;
LARGE_INTEGER g_qpf, g_nextVblank;
std::uint32_t g_lastEntry = 0, g_sinceCheck = 0;
std::map<int, std::uint32_t> g_userSyscalls;  // system calls installed by the game (SetSyscall)
bool g_stop = false;
std::string g_stopReason;

std::uint64_t& G(NativeState& s, int r) { return s.gpr[r][0]; }
std::uint32_t A(NativeState& s, int n) { return static_cast<std::uint32_t>(s.gpr[4 + n][0]); }  // a0..a3, t0..t3
void Ret(NativeState& s, std::int32_t v) { s.gpr[2][0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(v)); }

void Stop(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof text, format, args);
  va_end(args);
  if (!g_stop) g_stopReason = text;
  g_stop = true;
}

// ---- DMA ---------------------------------------------------------------------------------
// Transfers complete at once. The data is walked, so that what the game waits for happens: for VIF1 the
// command stream is parsed (interrupt bit -> INTC VIF1 interrupt). Graphics and VU1 programs come later.
std::uint32_t g_intcPending = 0;  // INTC causes raised, waiting for the scheduler to deliver them
std::uint32_t g_vifCycle = 0x0101;  // CL | WL << 8 (STCYCL)
std::uint64_t g_vifCodes = 0, g_vifInterrupts = 0, g_vifStarts = 0;
std::uint32_t g_vifMark = 0;

void ParseVif1(const std::vector<std::uint8_t>& d) {
  std::size_t pos = 0;
  auto word = [&](std::size_t at) { std::uint32_t v = 0; if (at + 4 <= d.size()) memcpy(&v, d.data() + at, 4); return v; };
  while (pos + 4 <= d.size()) {
    const std::uint32_t code = word(pos);
    const std::uint32_t cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
    pos += 4;
    ++g_vifCodes;
    if ((code & 0x80000000u) && cmd != 0x07) {  // the interrupt bit (MARK excepted)
      ++g_vifInterrupts;
      g_intcPending |= 1u << 5;
      g_intcStat |= 1u << 5;
    }
    if (cmd == 0x07) g_vifMark = imm;                                    // MARK: the game polls VIF1_MARK for it
    else if (cmd == 0x01) g_vifCycle = imm;
    else if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) ++g_vifStarts;  // MSCAL, MSCALF, MSCNT: VU1 programs (not run yet)
    else if (cmd == 0x20) pos += 4;                                      // STMASK
    else if (cmd == 0x30 || cmd == 0x31) pos += 16;                      // STROW, STCOL
    else if (cmd == 0x4A) pos = ((pos + 7) & ~std::size_t{7}) + (num ? num : 256) * 8;  // MPG
    else if (cmd == 0x50 || cmd == 0x51) pos = ((pos + 15) & ~std::size_t{15}) + (imm ? imm : 0x10000) * 16;  // DIRECT(HL)
    else if (cmd >= 0x60) {                                              // UNPACK
      const std::uint32_t cl = g_vifCycle & 0xFF, wl = (g_vifCycle >> 8) & 0xFF, vn = (cmd >> 2) & 3, vl = cmd & 3;
      std::uint32_t count = num ? num : 256;
      if (wl > cl && cl > 0) count = (count / wl) * cl + (std::min)(count % wl, cl);
      const std::uint32_t bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
      pos += (static_cast<std::size_t>(count) * bits + 31) / 32 * 4;
    }
  }
}

void RunDma(std::uint32_t channel, std::uint32_t base, std::uint32_t chcr) {
  std::vector<std::uint8_t> data;
  auto take = [&](std::uint32_t address, std::uint32_t qwc) {
    const bool spr = (address & 0x80000000u) != 0;
    for (std::uint32_t i = 0; i < qwc; ++i) {
      const std::uint32_t a = (address & 0x7FFFFFFFu) + i * 16;
      const std::uint8_t* src = spr ? g_spr + (a & 0x3FFF) : g_ram + (InRam(a, 16) ? Phys(a) : 0);
      data.insert(data.end(), src, src + 16);
    }
  };
  if (((chcr >> 2) & 3) == 0) {
    take(g_hw[base + 0x10], g_hw[base + 0x20] & 0xFFFF);
  } else {
    std::uint32_t tadr = g_hw[base + 0x30];
    for (int guard = 0; guard < 100000; ++guard) {
      const std::uint64_t tag = Rd<std::uint64_t>(tadr);
      const std::uint32_t id = (tag >> 28) & 7, qwc = tag & 0xFFFF, addr = static_cast<std::uint32_t>(tag >> 32);
      if (chcr & 0x40) {  // TTE: the tag's upper half (two VIF codes) goes along
        const std::uint8_t* hi = g_ram + Phys(tadr) + 8;
        data.insert(data.end(), hi, hi + 8);
      }
      if (id == 1 || id == 7 || id == 2 || id == 5 || id == 6) take(tadr + 16, qwc);
      else take(addr, qwc);  // refe, ref, refs
      if (id == 0 || id == 7) break;
      tadr = (id == 2 || id == 5) ? addr : tadr + 16 + (id == 1 || id == 6 ? qwc * 16 : 0);
    }
  }
  if (channel == 1) ParseVif1(data);
}

// ---- hardware registers (slow path of the translated code) -----------------------
std::uint64_t HwRead(std::uint8_t*, std::uint32_t address, std::uint32_t size) {
  const std::uint32_t a = Phys(address);
  if (a >= 0x1FC00000 && a < 0x20000000) return 0;  // boot ROM area
  switch (a) {
    case 0x1000F000: return g_intcStat;
    case 0x1000F010: return g_intcMask;
    case 0x1000E010: return g_dmacStat;
    case 0x1000F230: return 0x00060000;  // SIF: the I/O side reports itself initialised
    case 0x12001000: return 0x00000008 | ((g_vblanks & 1) << 13);  // GS CSR: FIFO empty, field
    case 0x10003020: return 0;  // GIF_STAT: idle
    case 0x10003C00: return 0;  // VIF1_STAT: idle
    case 0x10003C30: return g_vifMark;  // VIF1_MARK
    case 0x10003800: return 0;  // VIF0_STAT
    case 0x1000F520: return g_hw[0x1000F590];  // D_ENABLER
    default: break;
  }
  if (a >= 0x10000000 && a < 0x10002000) {  // timers: Tn_COUNT from host time
    if ((a & 0xFF) == 0x00) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      return static_cast<std::uint32_t>(now.QuadPart * 9765 / g_qpf.QuadPart * 16) & 0xFFFF;  // ~bus clock / 256
    }
    return g_hw[a];
  }
  if ((a >= 0x10008000 && a < 0x1000E000)) return g_hw[a] & ~0x100u;  // DMA channels: transfers end at once
  LogOnce("r" + std::to_string(a), "hw: read of %08X (size %u) - not implemented, 0 returned", address, size);
  return g_hw.count(a) ? g_hw[a] : 0;
}

void HwWrite(std::uint8_t*, std::uint32_t address, std::uint64_t value, std::uint32_t size) {
  const std::uint32_t a = Phys(address);
  const std::uint32_t v = static_cast<std::uint32_t>(value);
  switch (a) {
    case 0x1000F000: g_intcStat &= ~v; return;
    case 0x1000F010: g_intcMask ^= v; return;
    case 0x1000E010: g_dmacStat = (g_dmacStat & ~(v & 0xFFFF)) ^ (v & 0xFFFF0000); return;
    case 0x12001000: return;  // GS CSR: event acknowledge
    default: break;
  }
  if (a >= 0x10008000 && a < 0x1000E000 && (a & 0xFF) == 0 && (v & 0x100)) {
    // DMA transfer started. Nothing consumes the data yet (graphics and sound
    // come later): it completes at once and the channel's interrupt is raised.
    static const std::uint32_t bases[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                            0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
    for (std::uint32_t ch = 0; ch < 10; ++ch)
      if (bases[ch] == a) {
        RunDma(ch, a, v);
        g_dmacStat |= 1u << ch;
        g_pendingDmac.emplace_back(ch, 0);
        LogOnce("dma" + std::to_string(ch), "hw: first DMA transfer on channel %u (data not consumed yet)", ch);
      }
    g_hw[a] = v & ~0x100u;
    return;
  }
  if (!((a >= 0x10000000 && a < 0x10002000) || (a >= 0x10008000 && a < 0x1000E100) || (a >= 0x12000000 && a < 0x12002000) ||
        a == 0x1000F590 || a == 0x1000F200 || a == 0x1000F220 || a == 0x1000F240 || a == 0x1000F260))
    LogOnce("w" + std::to_string(a), "hw: write of %08X = %08X (size %u) - not implemented, stored", address, v, size);
  g_hw[a] = v;
}

// ei, di, mfc0, mtc0 (translated code calls this for them).
void System(std::uint8_t* state, std::uint32_t raw) {
  auto& s = *reinterpret_cast<NativeState*>(state);
  if (raw == 0x42000038) { g_status |= 0x10000; return; }   // ei
  if (raw == 0x42000039) { g_status &= ~0x10000u; return; }  // di
  const unsigned rt = (raw >> 16) & 31, rd = (raw >> 11) & 31;
  if (((raw >> 21) & 31) == 0) {  // mfc0
    std::uint32_t v = rd == 12 ? g_status : rd == 15 ? 0x2E20 /* processor id */ : s.cop0[rd];
    if (rd == 9) {  // Count: CPU cycles at 294.912 MHz, from the host clock
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      const std::uint64_t whole = static_cast<std::uint64_t>(now.QuadPart / g_qpf.QuadPart);
      const std::uint64_t rest = static_cast<std::uint64_t>(now.QuadPart % g_qpf.QuadPart);
      v = static_cast<std::uint32_t>(whole * 294912000ull + rest * 294912000ull / static_cast<std::uint64_t>(g_qpf.QuadPart));
    }
    if (rt) s.gpr[rt][0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(v)));
  } else {  // mtc0
    if (rd == 12) g_status = static_cast<std::uint32_t>(s.gpr[rt][0]);
    else s.cop0[rd] = static_cast<std::uint32_t>(s.gpr[rt][0]);
  }
}

struct NativeRuntimeTable {
  std::uint64_t (*read)(std::uint8_t*, std::uint32_t, std::uint32_t);
  void (*write)(std::uint8_t*, std::uint32_t, std::uint64_t, std::uint32_t);
  void (*system)(std::uint8_t*, std::uint32_t);
};
const NativeRuntimeTable g_runtime{&HwRead, &HwWrite, &System};

// ---- scheduling ------------------------------------------------------------------
void WakeFromSema(Sema& sm) {
  if (sm.waiters.empty()) return;
  Thread* t = g_threads[sm.waiters.front()];
  sm.waiters.pop_front();
  t->wait_sema = -1;
  t->status = kReady;
  g_resched = true;
}
// The running thread gives up the processor (blocked, or a pass of a waiting loop).
void GiveUp() {
  if (g_inHandler || !g_current) return;
  SwitchToFiber(g_mainFiber);
}

void Step(NativeState& s);
void CallGuest(NativeState& s, std::uint32_t function, std::uint32_t a0, std::uint32_t a1) {
  s.pc = function;
  s.gpr[4][0] = a0;
  s.gpr[5][0] = a1;
  s.gpr[31][0] = kReturn;
  while (s.pc != kReturn && !g_stop) Step(s);
}

NativeState g_handlerState;
void RunHandlers(std::vector<Handler>& list, std::uint32_t cause) {
  g_inHandler = true;
  for (const Handler& h : std::vector<Handler>(list))
    if (h.cause == cause) {
      g_handlerState.gpr[29][0] = g_threads[1]->stack - 0x100;  // the handlers' own stack: the gap below the main stack
      g_handlerState.gpr[28][0] = g_threads.size() > 1 ? g_threads[1]->gp : 0;
      CallGuest(g_handlerState, h.function, cause, h.arg);
    }
  g_inHandler = false;
}

void DeliverInterrupts() {
  const bool enabled = (g_status & 0x10000) != 0;  // the game can disable interrupts: handlers wait, time does not
  while (enabled && !g_pendingDmac.empty() && !g_stop) {
    const std::uint32_t ch = g_pendingDmac.front().first;
    g_pendingDmac.pop_front();
    if (g_dmacStat & (0x10000u << ch)) RunHandlers(g_dmac, ch);
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (now.QuadPart >= g_nextVblank.QuadPart) {
    g_nextVblank.QuadPart += g_qpf.QuadPart * 1001 / 60000;
    if (now.QuadPart > g_nextVblank.QuadPart) g_nextVblank.QuadPart = now.QuadPart;
    ++g_vblanks;
    if (g_vsyncFlag1) Wr<std::uint32_t>(g_vsyncFlag1, 1);
    if (g_vsyncFlag2) Wr<std::uint64_t>(g_vsyncFlag2, (g_vblanks & 1) << 13);
    for (std::uint32_t cause : {2u, 3u}) {  // vertical blank start, end
      g_intcStat |= 1u << cause;
      if (enabled && (g_intcMask & (1u << cause))) RunHandlers(g_intc, cause);
    }
  }
  for (std::uint32_t cause = 0; cause < 16 && enabled && !g_stop; ++cause)  // raised by devices (VIF1 ...)
    if ((g_intcPending & (1u << cause)) && (g_intcMask & (1u << cause))) {
      g_intcPending &= ~(1u << cause);
      RunHandlers(g_intc, cause);
    }
}

// ---- kernel services ---------------------------------------------------------------
int NewThread() {
  for (std::size_t i = 1; i < g_threads.size(); ++i)
    if (!g_threads[i]->used) return static_cast<int>(i);
  g_threads.push_back(new Thread());
  g_threads.back()->id = static_cast<int>(g_threads.size() - 1);
  return g_threads.back()->id;
}
void CALLBACK ThreadFiber(void* param);

void Syscall(NativeState& s) {
  const std::int32_t raw = static_cast<std::int32_t>(s.gpr[3][0]);
  const int n = raw < 0 ? -raw : raw;
  ++g_syscalls;
  s.pc += 4;
  if (g_verbose && g_syscalls < 2000)
    Log("  syscall %d at %08X (a0 %08X a1 %08X a2 %08X) from %08X", raw, s.pc - 4, A(s, 0), A(s, 1), A(s, 2),
        static_cast<std::uint32_t>(s.gpr[31][0]));
  // The kernel's syscall table lives at 0x80002000 (one word per number, as on the console): entries the
  // game installed itself - or patched there, it searches kernel memory for them - are game functions.
  if (const std::uint32_t handler = n < 256 ? Rd<std::uint32_t>(0x80002000u + 4u * n) : 0) {
    const std::uint32_t resume = s.pc, saved_ra = static_cast<std::uint32_t>(s.gpr[31][0]);
    s.pc = handler;
    s.gpr[31][0] = kReturn;
    while (s.pc != kReturn && !g_stop) Step(s);
    s.pc = resume;
    s.gpr[31][0] = saved_ra;
    return;
  }
  auto thread = [&](std::uint32_t id) -> Thread* {
    return id < g_threads.size() && id > 0 && g_threads[id]->used ? g_threads[id] : nullptr;
  };
  auto sema = [&](std::uint32_t id) -> Sema* { return id < g_semas.size() && id > 0 && g_semas[id].used ? &g_semas[id] : nullptr; };
  switch (n) {
    case 0x02: Log("kernel: SetGsCrt(interlace %u, mode %u, field %u)", A(s, 0), A(s, 1), A(s, 2)); return;
    case 0x04: Stop("the game called Exit(%d)", static_cast<int>(A(s, 0))); return;
    case 0x10: case 0x12: {  // AddIntcHandler / AddDmacHandler(cause, handler, next, arg)
      auto& list = n == 0x10 ? g_intc : g_dmac;
      list.push_back({g_nextHandler, A(s, 0), A(s, 1), A(s, 3)});
      Log("kernel: %s handler %08X for cause %u", n == 0x10 ? "interrupt" : "DMA", A(s, 1), A(s, 0));
      Ret(s, g_nextHandler++);
      return;
    }
    case 0x11: case 0x13: {
      auto& list = n == 0x11 ? g_intc : g_dmac;
      list.erase(std::remove_if(list.begin(), list.end(), [&](const Handler& h) { return h.id == static_cast<int>(A(s, 1)); }), list.end());
      Ret(s, 0);
      return;
    }
    case 0x14: case 0x1A: g_intcMask |= 1u << A(s, 0); Ret(s, 1); return;      // EnableIntc
    case 0x15: case 0x1B: g_intcMask &= ~(1u << A(s, 0)); Ret(s, 1); return;   // DisableIntc
    case 0x16: case 0x1C: g_dmacStat |= 0x10000u << A(s, 0); Ret(s, 1); return;     // EnableDmac
    case 0x17: case 0x1D: g_dmacStat &= ~(0x10000u << A(s, 0)); Ret(s, 1); return;  // DisableDmac
    case 0x20: {  // CreateThread(param)
      const std::uint32_t p = A(s, 0);
      const int id = NewThread();
      Thread& t = *g_threads[id];
      t.used = true;
      t.status = kDormant;
      t.entry = Rd<std::uint32_t>(p + 4);
      t.stack = Rd<std::uint32_t>(p + 8);
      t.stack_size = Rd<std::uint32_t>(p + 12);
      t.gp = Rd<std::uint32_t>(p + 16);
      t.priority = static_cast<int>(Rd<std::uint32_t>(p + 20));
      Log("kernel: thread %d created (entry %08X, priority %d)", id, t.entry, t.priority);
      Ret(s, id);
      return;
    }
    case 0x21: if (Thread* t = thread(A(s, 0))) t->used = false; Ret(s, 0); return;
    case 0x22: {  // StartThread(id, arg)
      Thread* t = thread(A(s, 0));
      if (!t) { Ret(s, -1); return; }
      t->s = NativeState();
      t->s.pc = t->entry;
      t->s.gpr[4][0] = A(s, 1);
      t->s.gpr[28][0] = t->gp;
      t->s.gpr[29][0] = t->stack + t->stack_size - 0x20;
      t->s.gpr[31][0] = kReturn;
      t->status = kReady;
      t->wakeups = 0;
      if (!t->fiber) t->fiber = CreateFiber(1 << 20, &ThreadFiber, t);
      Ret(s, static_cast<int>(A(s, 0)));
      g_resched = true;
      return;
    }
    case 0x23: case 0x24:  // ExitThread / ExitDeleteThread
      if (g_current) {
        g_current->status = kDormant;
        if (n == 0x24) g_current->used = false;
        GiveUp();
      }
      return;
    case 0x29: case 0x2A: if (Thread* t = thread(A(s, 0) ? A(s, 0) : (g_current ? g_current->id : 0))) t->priority = static_cast<int>(A(s, 1)); Ret(s, 0); g_resched = true; return;
    case 0x2B: case 0x2C: Ret(s, 0); g_resched = true; return;  // RotateThreadReadyQueue
    case 0x2F: Ret(s, g_current ? g_current->id : 1); return;
    case 0x30: case 0x31: {  // ReferThreadStatus(id, info)
      Thread* t = thread(A(s, 0) ? A(s, 0) : (g_current ? g_current->id : 1));
      if (!t) { Ret(s, -1); return; }
      const int status = t == g_current ? kRun : t->status;
      if (const std::uint32_t p = A(s, 1)) {
        Wr<std::uint32_t>(p + 0, status);
        Wr<std::uint32_t>(p + 4, t->entry);
        Wr<std::uint32_t>(p + 8, t->stack);
        Wr<std::uint32_t>(p + 12, t->stack_size);
        Wr<std::uint32_t>(p + 16, t->gp);
        Wr<std::uint32_t>(p + 20, t->priority);
        Wr<std::uint32_t>(p + 24, t->priority);
      }
      Ret(s, status);
      return;
    }
    case 0x32:  // SleepThread
      Ret(s, g_current ? g_current->id : 0);
      if (g_current) {
        if (g_current->wakeups > 0) { --g_current->wakeups; return; }
        g_current->status = kWait;
        g_current->sleeping = true;
        GiveUp();
      }
      return;
    case 0x33: case 0x34: {  // WakeupThread
      Thread* t = thread(A(s, 0));
      if (!t) { Ret(s, -1); return; }
      if (t->status == kWait && t->sleeping) { t->sleeping = false; t->status = kReady; g_resched = true; }
      else ++t->wakeups;
      Ret(s, static_cast<int>(A(s, 0)));
      return;
    }
    case 0x37: case 0x38: if (Thread* t = thread(A(s, 0))) { t->status = kSuspend; if (t == g_current) GiveUp(); } Ret(s, 0); return;
    case 0x39: case 0x3A: if (Thread* t = thread(A(s, 0))) { if (t->status == kSuspend) t->status = kReady; g_resched = true; } Ret(s, 0); return;
    case 0x3C: {  // SetupThread(gp, stack, stack_size, args, root)
      std::uint32_t stack = A(s, 1), size = A(s, 2);
      if (stack == 0xFFFFFFFFu) stack = kNativeRamSize - size;  // at the top of memory, as on the console
      Thread& t = *g_threads[1];
      t.stack = stack;
      t.stack_size = size;
      t.gp = A(s, 0);
      Log("kernel: main thread set up (stack %08X + %X, gp %08X)", stack, size, t.gp);
      Ret(s, static_cast<std::int32_t>(stack + size));
      return;
    }
    case 0x3D: {  // SetupHeap(start, size)
      g_heapStart = A(s, 0);
      g_heapEnd = A(s, 1) == 0xFFFFFFFFu ? g_threads[1]->stack : A(s, 0) + A(s, 1);
      Log("kernel: heap %08X-%08X", g_heapStart, g_heapEnd);
      Ret(s, static_cast<std::int32_t>(g_heapEnd));
      return;
    }
    case 0x3E: Ret(s, static_cast<std::int32_t>(g_heapEnd)); return;  // EndOfHeap: the limit the game's sbrk (0028BA48) compares with
    case 0x40: {  // CreateSema(param)
      const std::uint32_t p = A(s, 0);
      int id = 0;
      for (std::size_t i = 1; i < g_semas.size(); ++i)
        if (!g_semas[i].used) id = static_cast<int>(i);
      if (!id) { g_semas.emplace_back(); id = static_cast<int>(g_semas.size() - 1); }
      g_semas[id] = Sema{true, static_cast<int>(Rd<std::uint32_t>(p + 8)), static_cast<int>(Rd<std::uint32_t>(p + 4)), {}};
      Ret(s, id);
      return;
    }
    case 0x41: if (Sema* sm = sema(A(s, 0))) { while (!sm->waiters.empty()) WakeFromSema(*sm); sm->used = false; } Ret(s, static_cast<int>(A(s, 0))); return;
    case 0x42: case 0x43: {  // SignalSema
      Sema* sm = sema(A(s, 0));
      if (!sm) { Ret(s, -1); return; }
      if (!sm->waiters.empty()) WakeFromSema(*sm);
      else ++sm->count;
      Ret(s, static_cast<int>(A(s, 0)));
      return;
    }
    case 0x44: {  // WaitSema
      Sema* sm = sema(A(s, 0));
      if (!sm) { Ret(s, -1); return; }
      Ret(s, static_cast<int>(A(s, 0)));
      if (sm->count > 0) { --sm->count; return; }
      if (!g_current) { LogOnce("waithandler", "kernel: WaitSema from a handler ignored"); return; }
      sm->waiters.push_back(g_current->id);
      g_current->wait_sema = static_cast<int>(A(s, 0));
      g_current->status = kWait;
      GiveUp();
      return;
    }
    case 0x45: case 0x46: {  // PollSema
      Sema* sm = sema(A(s, 0));
      if (!sm || sm->count <= 0) { Ret(s, -1); return; }
      --sm->count;
      Ret(s, static_cast<int>(A(s, 0)));
      return;
    }
    case 0x47: case 0x48: {  // ReferSemaStatus(id, info)
      Sema* sm = sema(A(s, 0));
      if (!sm) { Ret(s, -1); return; }
      Wr<std::uint32_t>(A(s, 1) + 0, sm->count);
      Wr<std::uint32_t>(A(s, 1) + 4, sm->max);
      Wr<std::uint32_t>(A(s, 1) + 12, static_cast<std::uint32_t>(sm->waiters.size()));
      Ret(s, 0);
      return;
    }
    case 0x74: {  // SetSyscall(number, function): the game's own system call
      const int num = static_cast<int>(A(s, 0));
      const std::uint32_t slot = 0x80002000u + 4u * static_cast<std::uint32_t>(num & 255);
      Ret(s, static_cast<std::int32_t>(Rd<std::uint32_t>(slot)));
      Wr<std::uint32_t>(slot, A(s, 1));
      Log("kernel: game system call %d installed at %08X", num, A(s, 1));
      return;
    }
    case 0x7C: Ret(s, -1); return;  // Deci2Call: no debugger attached
    case 0x64: case 0x68: Ret(s, 0); return;  // FlushCache: nothing to flush in translated code
    case 0x70: Ret(s, static_cast<std::int32_t>(g_hw[0x12001010])); return;  // GsGetIMR
    case 0x71: g_hw[0x12001010] = A(s, 0); Ret(s, 0); return;                 // GsPutIMR
    case 0x73: g_vsyncFlag1 = A(s, 0); g_vsyncFlag2 = A(s, 1); return;        // SetVSyncFlag
    case 0x76: Ret(s, -1); return;  // SifDmaStat: transfer finished
    case 0x77: Ret(s, 1); return;   // SifSetDma: accepted (the I/O side is served by host services)
    case 0x78: return;              // SifSetDChain
    case 0x79: g_hw[0xF0000000u + A(s, 0)] = A(s, 1); Ret(s, 0); return;  // SifSetReg
    case 0x7A: Ret(s, A(s, 0) == 4 ? 0x00060000 : static_cast<std::int32_t>(g_hw[0xF0000000u + A(s, 0)])); return;  // SifGetReg
    case 0x7F: Ret(s, kNativeRamSize); return;  // GetMemorySize
    default:
      LogOnce("sys" + std::to_string(n), "kernel: service %d (0x%02X) is not implemented yet (a0 %08X a1 %08X); 0 returned", raw,
              n, A(s, 0), A(s, 1));
      Ret(s, 0);
      return;
  }
}

// ---- I/O processor services (host services at the library's RPC functions, ADR 0011) ----
std::map<std::uint32_t, std::uint32_t> g_rpcServer;  // client structure -> server number
std::map<std::uint64_t, std::uint64_t> g_rpcCount;   // (server, function) -> calls

void Hook(NativeState& s) {
  const std::uint32_t pc = s.pc;
  if (pc == kHookSifInitRpc || pc == 0x0028511C) {  // sceSifInitRpc(mode)
    Wr<std::uint32_t>(0x00327F7C, 1);
    LogOnce("sifinit", "io: sceSifInitRpc served by the runtime (no I/O processor to wait for)");
    Ret(s, 0);
  } else if (pc == kHookSifBindRpc) {  // sceSifBindRpc(client, server number, mode)
    const std::uint32_t client = A(s, 0);
    g_rpcServer[client] = A(s, 1);
    Wr<std::uint32_t>(client + 0x24, 0x00001000);  // "server" pointer: bound
    Log("io: RPC client %08X bound to server %08X", client, A(s, 1));
    Ret(s, 0);
  } else if (pc == kHookSifCallRpc) {  // sceSifCallRpc(client, function, mode, send, ssize, recv, rsize, end function, end arg)
    const std::uint32_t server = g_rpcServer[A(s, 0)], fn = A(s, 1), recv = A(s, 5), rsize = A(s, 6);
    ++g_rpcCalls;
    if (++g_rpcCount[(static_cast<std::uint64_t>(server) << 32) | fn] == 1)
      Log("io: first call of server %08X function %u (send %u bytes, receive %u) - no host service yet, zeros returned", server,
          fn, A(s, 4), rsize);
    if (recv && rsize && InRam(recv, rsize)) memset(g_ram + Phys(recv), 0, rsize);
    Ret(s, 0);
  } else {  // sceSifCheckStatRpc(client): never busy, calls complete at once
    Ret(s, 0);
  }
  s.pc = static_cast<std::uint32_t>(s.gpr[31][0]);
}

// ---- execution -----------------------------------------------------------------------
void Step(NativeState& s) {
  ++g_steps;
  const std::uint32_t pc = s.pc;
  if (void* code = FindCode(pc)) {
    s.cycle_quota = kSliceQuota;
    g_lastEntry = pc;
    reinterpret_cast<EeCode>(code)(reinterpret_cast<std::uint8_t*>(&s), g_ram, g_spr, &g_runtime);
    bool due = false;
    if (++g_sinceCheck >= 64) {
      g_sinceCheck = 0;  // a refresh interrupt is due: let the scheduler deliver it
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      due = now.QuadPart >= g_nextVblank.QuadPart;
    }
    if (s.pc == pc || s.cycle_quota <= 0 || g_resched || due) {  // also: a pass of a waiting loop, time slice used up
      g_resched = false;
      GiveUp();
    }
    return;
  }
  for (std::uint32_t hook : kNativeHooks)
    if (pc == hook) {
      Hook(s);
      return;
    }
  if (!InRam(pc)) {
    Stop("execution reached %08X, outside memory (called from %08X)", pc, static_cast<std::uint32_t>(s.gpr[31][0]));
    return;
  }
  const std::uint32_t raw = Rd<std::uint32_t>(pc);
  if ((raw & 0xFC00003F) == 0x0000000C) {
    Syscall(s);
    return;
  }
  Stop("no native code at %08X: %s  (the translator does not cover this instruction yet)", pc,
       r5900::disassemble(pc, raw).c_str());
}

void CALLBACK ThreadFiber(void* param) {
  Thread* t = static_cast<Thread*>(param);
  for (;;) {
    while (t->s.pc != kReturn && !g_stop) Step(t->s);
    t->status = kDormant;  // returned from its entry function
    SwitchToFiber(g_mainFiber);
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    fs::path exe_dir;
    {
      std::wstring buffer(32768, L'\0');
      buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
      exe_dir = fs::path(buffer).parent_path();
    }
    fs::path rom = exe_dir.parent_path() / L"rom", dll = exe_dir / L"hp2code.dll";
    double seconds = 0;
    for (int i = 1; i < argc; ++i) {
      const std::wstring a = argv[i];
      if (a == L"--rom" && i + 1 < argc) rom = argv[++i];
      else if (a == L"--code" && i + 1 < argc) dll = argv[++i];
      else if (a == L"--seconds" && i + 1 < argc) seconds = _wtof(argv[++i]);
      else if (a == L"--verbose") g_verbose = true;
      else {
        printf("HotPursuit2Native [--rom <folder>] [--code <hp2code.dll>] [--seconds <n>]\n");
        return 2;
      }
    }
    g_log = _wfopen((exe_dir / L"native.log").c_str(), L"w");
    Log("HotPursuit2Native 0.2.0-dev.1 - native runtime, no emulator code. Development build.");

    // ---- the game's executable, from the user's ROM folder ----
    const Bytes file = read_whole_file(rom / L"SLUS_203.62", 64ull << 20);
    const ElfImage elf = ElfImage::parse(file);
    g_ram = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kNativeRamSize + 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_spr = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_ram || !g_spr) fail("cannot allocate the game's memory");
    for (const ElfSegment& seg : elf.segments())
      if (seg.loadable() && seg.filesz) memcpy(g_ram + Phys(seg.vaddr), elf.file().data() + seg.offset, seg.filesz);
    Log("Executable loaded: %ls (entry %08X)", (rom / L"SLUS_203.62").c_str(), elf.entry());

    // ---- the translated code ----
    HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) fail("cannot load the code package '{}' (error {})", to_utf8(dll), GetLastError());
    g_table = reinterpret_cast<const Hp2CodeTable*>(GetProcAddress(module, kCodeTableSymbol));
    if (!g_table || g_table->magic != kCodeTableMagic || g_table->version != kCodeTableVersion)
      fail("'{}' is not a code package of this version", to_utf8(dll));
    if (g_table->layout_fingerprint != code_layout_fingerprint(native_code_layout()))
      fail("the code package was not built for the native runtime (hp2recomp package)");
    {
      Sha256 h;
      h.update(file);
      const auto digest = h.finish();
      if (memcmp(digest.data(), g_table->elf_sha256, 32) != 0) fail("the code package was made from a different executable");
    }
    Log("Code package: %u functions, %u entry places, compiled for %s", g_table->function_count, g_table->entry_count,
        g_table->cpu);

    // ---- run ----
    // The game expects the console's floating-point mode: round toward zero, denormals flushed.
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_nextVblank);
    LARGE_INTEGER start = g_nextVblank;
    g_mainFiber = ConvertThreadToFiber(nullptr);
    g_threads.push_back(new Thread());  // id 0 unused
    g_threads.push_back(new Thread());
    Thread& main_thread = *g_threads[1];
    main_thread.id = 1;
    main_thread.used = true;
    main_thread.status = kReady;
    main_thread.priority = 1;
    main_thread.s.pc = elf.entry();
    main_thread.s.gpr[31][0] = kReturn;
    main_thread.fiber = CreateFiber(1 << 20, &ThreadFiber, &main_thread);

    // Watchdog: if the game code does not come back to the runtime in time, say where it is and end.
    static double watchdog_seconds = seconds;
    if (seconds > 0)
      CreateThread(nullptr, 0, [](void*) -> DWORD {
        Sleep(static_cast<DWORD>((watchdog_seconds + 3) * 1000));
        const Thread* t = g_current;
        Log("---");
        Log("Stopped by the watchdog: game code did not return to the runtime. Thread %d, last entered at %08X, "
            "return address %08X, stack %08X; %llu refreshes, %llu entries, %llu kernel calls",
            t ? t->id : 0, g_lastEntry, t ? static_cast<std::uint32_t>(t->s.gpr[31][0]) : 0,
            t ? static_cast<std::uint32_t>(t->s.gpr[29][0]) : 0, static_cast<unsigned long long>(g_vblanks),
            static_cast<unsigned long long>(g_steps), static_cast<unsigned long long>(g_syscalls));
        ExitProcess(4);
      }, nullptr, 0, nullptr);

    std::size_t rotate = 0;
    while (!g_stop) {
      DeliverInterrupts();
      Thread* best = nullptr;
      for (std::size_t k = 0; k < g_threads.size(); ++k) {
        Thread* t = g_threads[(rotate + k) % g_threads.size()];
        if (t->used && t->status == kReady && (!best || t->priority < best->priority)) best = t;
      }
      if (!best) {
        if (std::none_of(g_threads.begin(), g_threads.end(), [](Thread* t) { return t->used && t->status != kDormant; })) {
          Stop("every game thread has ended");
          break;
        }
        Sleep(1);  // all threads wait: until the next interrupt
      } else {
        rotate = static_cast<std::size_t>(best->id) + 1;
        g_current = best;
        best->status = kRun;
        SwitchToFiber(best->fiber);
        if (best->status == kRun) best->status = kReady;
        g_current = nullptr;
      }
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      if (seconds > 0 && (now.QuadPart - start.QuadPart) > seconds * g_qpf.QuadPart) Stop("time limit of the run (%.0f s) reached", seconds);
    }

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    Log("---");
    Log("Stopped: %s", g_stopReason.c_str());
    Log("Ran %.1f s: %llu refreshes, %llu entries into native game code, %llu kernel calls, %llu I/O calls",
        static_cast<double>(end.QuadPart - start.QuadPart) / g_qpf.QuadPart, static_cast<unsigned long long>(g_vblanks),
        static_cast<unsigned long long>(g_steps), static_cast<unsigned long long>(g_syscalls),
        static_cast<unsigned long long>(g_rpcCalls));
    for (std::size_t i = 1; i < g_threads.size(); ++i) {
      const Thread& t = *g_threads[i];
      if (t.used)
        Log("  thread %zu: status %d, priority %d, at %08X (called from %08X)%s", i, t.status, t.priority, t.s.pc,
            static_cast<std::uint32_t>(t.s.gpr[31][0]), t.wait_sema >= 0 ? " waiting for a semaphore" : t.sleeping ? " sleeping" : "");
    }
    if (g_log) fclose(g_log);
    return 0;
  } catch (const std::exception& e) {
    Log("error: %s", e.what());
    return 3;
  }
}
