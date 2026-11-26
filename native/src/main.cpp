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
#include <unordered_map>
#include <memory>
#include <algorithm>

#include "virtual_disc.h"
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
std::unordered_map<std::uint32_t, std::uint64_t> g_funcCount;  // function -> entries (traced packages)
std::vector<std::uint32_t> g_funcOrder;                        // functions in order of their first entry
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
std::uint8_t g_vu1Micro[0x4000], g_vu1Data[0x4000], g_vu0Micro[0x1000], g_vu0Data[0x1000];  // vector unit memories

// The CPU sees the vector units' memories at 11000000 (VU0 code), 11004000 (VU0 data), 11008000 (VU1 code), 1100C000 (VU1 data).
std::uint8_t* VuMemory(std::uint32_t a) {
  if (a < 0x11000000 || a >= 0x11010000) return nullptr;
  if (a >= 0x1100C000) return g_vu1Data + (a - 0x1100C000);
  if (a >= 0x11008000) return g_vu1Micro + (a - 0x11008000);
  if (a >= 0x11005000 || (a >= 0x11001000 && a < 0x11004000)) return nullptr;
  return a >= 0x11004000 ? g_vu0Data + (a - 0x11004000) : g_vu0Micro + (a - 0x11000000);
}

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
    static int unknown = 0;
    if (!(cmd <= 0x07 || cmd == 0x10 || cmd == 0x11 || cmd == 0x13 || cmd == 0x14 || cmd == 0x15 || cmd == 0x17 || cmd == 0x20 ||
          cmd == 0x30 || cmd == 0x31 || cmd == 0x4A || cmd == 0x50 || cmd == 0x51 || cmd >= 0x60) && unknown++ < 20)
      Log("hw: VIF1 stream: unknown code %08X at byte %zu of %zu", code, pos - 4, d.size());
    if (cmd == 0x07) g_vifMark = imm;                                    // MARK: the game polls VIF1_MARK for it
    else if (cmd == 0x01) g_vifCycle = imm;
    else if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) ++g_vifStarts;  // MSCAL, MSCALF, MSCNT: VU1 programs (not run yet)
    else if (cmd == 0x20) pos += 4;                                      // STMASK
    else if (cmd == 0x30 || cmd == 0x31) pos += 16;                      // STROW, STCOL
    else if (cmd == 0x4A) {                                                // MPG: microprogram into VU1 code memory
      const std::size_t bytes = static_cast<std::size_t>(num ? num : 256) * 8;
      for (std::size_t i = 0; i < bytes && pos + i < d.size(); ++i) g_vu1Micro[(imm * 8 + i) & 0x3FFF] = d[pos + i];
      pos += bytes;
    } else if (cmd == 0x50 || cmd == 0x51) pos += static_cast<std::size_t>(imm ? imm : 0x10000) * 16;  // DIRECT(HL)
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
  if (std::uint8_t* p = VuMemory(a)) {  // vector unit code/data memories are plain memory to the CPU
    std::uint64_t v = 0;
    memcpy(&v, p, size > 8 ? 8 : size);
    return v;
  }
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
  if (std::uint8_t* p = VuMemory(a)) {
    memcpy(p, &value, size > 8 ? 8 : size);
    return;
  }
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
  if ((raw >> 26) != 0x10) {  // not a COP0 instruction: a function entry (package built with HP2_TRACE_ENTRIES)
    if (++g_funcCount[raw] == 1) g_funcOrder.push_back(raw);
    return;
  }
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
bool g_profile = true;

// ---- the disc (R2b): the owner's ROM folder as an ISO 9660 disc, read through the project's own virtual disc ----
std::shared_ptr<hp2::VirtualDisc> g_disc;
bool DiscRead(std::uint64_t offset, void* out, std::uint64_t count) {
  if (!g_disc) return false;
  try {
    return g_disc->Read(offset, out, count) == count;
  } catch (const std::exception&) {
    return false;
  }
}
// Finds a file by its path ("cdrom:\NFSHP2\FILE.BIN;1"): first sector and size.
bool DiscFind(std::string path, std::uint32_t& lsn, std::uint32_t& size) {
  if (const auto colon = path.find(':'); colon != std::string::npos) path.erase(0, colon + 1);
  std::vector<std::string> parts;
  std::string part;
  for (char c : path + "\\") {
    if (c == '\\' || c == '/') {
      if (!part.empty()) {
        if (const auto semi = part.find(';'); semi != std::string::npos) part.erase(semi);
        parts.push_back(part);
        part.clear();
      }
    } else {
      part += c;
    }
  }
  std::uint8_t sector[2048];
  if (!DiscRead(16 * 2048ull, sector, 2048)) return false;
  auto le32 = [](const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24); };
  lsn = le32(sector + 156 + 2);
  size = le32(sector + 156 + 10);
  for (std::size_t k = 0; k < parts.size(); ++k) {
    std::vector<std::uint8_t> dir(size);
    if (!DiscRead(lsn * 2048ull, dir.data(), size)) return false;
    bool found = false;
    for (std::size_t o = 0; o < dir.size();) {
      const std::uint8_t len = dir[o];
      if (len == 0) {  // records do not cross sectors
        o = (o / 2048 + 1) * 2048;
        continue;
      }
      std::string name(reinterpret_cast<const char*>(&dir[o + 33]), dir[o + 32]);
      if (const auto semi = name.find(';'); semi != std::string::npos) name.erase(semi);
      if (name.size() == parts[k].size() &&
          std::equal(name.begin(), name.end(), parts[k].begin(), [](char a, char b) { return toupper(a) == toupper(b); })) {
        lsn = le32(&dir[o + 2]);
        size = le32(&dir[o + 10]);
        found = true;
        break;
      }
      o += len;
    }
    if (!found) return false;
  }
  return !parts.empty();
}
struct OpenFile {
  std::uint32_t lsn = 0, size = 0, pos = 0;
};
std::map<int, OpenFile> g_files;
std::uint64_t g_discReads = 0;
int g_nextFd = 3;
int FileOpen(std::uint32_t path_address) {
  const std::string path = GuestString(path_address, 300);
  OpenFile f;
  if (!DiscFind(path, f.lsn, f.size)) {
    Log("io: sceOpen(\"%s\") - not on the disc", path.c_str());
    return -2;
  }
  Log("io: sceOpen(\"%s\") -> fd %d (sector %u, %u bytes)", path.c_str(), g_nextFd, f.lsn, f.size);
  g_files[g_nextFd] = f;
  return g_nextFd++;
}
int FileSeek(int fd, std::int32_t offset, int whence) {
  const auto it = g_files.find(fd);
  if (it == g_files.end()) return -9;
  OpenFile& f = it->second;
  const std::int64_t base = whence == 0 ? 0 : whence == 1 ? f.pos : f.size;
  f.pos = static_cast<std::uint32_t>(std::clamp<std::int64_t>(base + offset, 0, f.size));
  return static_cast<int>(f.pos);
}
int FileRead(int fd, std::uint32_t buffer, std::uint32_t size) {
  const auto it = g_files.find(fd);
  if (it == g_files.end()) return -9;
  OpenFile& f = it->second;
  const std::uint32_t n = std::min(size, f.size - f.pos);
  if (n == 0 || !InRam(buffer, n)) return 0;
  if (!DiscRead(f.lsn * 2048ull + f.pos, g_ram + Phys(buffer), n)) return -5;
  f.pos += n;
  return static_cast<int>(n);
}

// EA's streamer (IOP server 00012345, function 0): a batch of 16-bit commands [count][id, nargs, args...]... in,
// a status block out. Modelled from the reference core's traffic (docs/recomp-analysis.md section 18): word 1 counts the
// batches, word 2 is -1, word 4 holds the result of the last command 0x0E ((a0 << 16) | a1) and, once command 0x34 has
// been seen, words 66-70 describe two buffers. Streams (word 0 > 0) are not modelled yet: no sound.
void StreamerService(std::uint32_t send, std::uint32_t recv) {
  static std::uint32_t batch = 0, result = 0, buf = 0;
  static bool have34 = false;
  const std::uint16_t* c = reinterpret_cast<const std::uint16_t*>(g_ram + Phys(send));
  std::uint32_t i = 1;
  for (std::uint32_t n = c[0]; n > 0 && i + 1 < 2048; --n) {
    const std::uint16_t id = c[i], na = c[i + 1];
    if (id == 0x0E && na >= 2) result = (static_cast<std::uint32_t>(c[i + 2]) << 16) | c[i + 3];
    if (id == 0x34 && na >= 2 && !have34) {
      have34 = true;
      buf = (static_cast<std::uint32_t>(c[i + 2]) << 16) | c[i + 3];
    }
    i += 2u + na;
  }
  std::uint32_t* w = reinterpret_cast<std::uint32_t*>(g_ram + Phys(recv));
  w[1] = ++batch;
  w[2] = 0xFFFFFFFFu;
  w[4] = result;
  if (have34) {
    w[66] = 2;
    w[67] = 3;
    w[68] = buf;
    w[70] = buf + 0x20000;
  }
}
std::map<std::uint32_t, std::uint64_t> g_entryCount;  // entry place -> times entered (what is the game doing?)
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
      Log("io: first call of server %08X function %u (send %u bytes at %08X, receive %u at %08X) - no host service yet, zeros returned",
          server, fn, A(s, 4), A(s, 3), rsize, recv);
    if (g_rpcCount[(static_cast<std::uint64_t>(server) << 32) | fn] == 1 && A(s, 3) && A(s, 4) && InRam(A(s, 3), std::min<std::uint32_t>(A(s, 4), 96))) {
      char hex[400] = {};
      const std::uint32_t n = std::min<std::uint32_t>(A(s, 4), 96);
      for (std::uint32_t i = 0; i < n; ++i) snprintf(hex + i * 3, 4, "%02X ", g_ram[Phys(A(s, 3)) + i]);
      Log("io:   send data of server %08X function %u: %s", server, fn, hex);
    }
    if (server == 0x00012345 && g_rpcCount[(static_cast<std::uint64_t>(server) << 32) | fn] <= 3 && InRam(A(s, 3), 64)) {
      char hex[200] = {};
      for (int i = 0; i < 64; ++i) snprintf(hex + i * 3, 4, "%02X ", g_ram[Phys(A(s, 3)) + i]);
      Log("io:   streamer command #%llu: %s", static_cast<unsigned long long>(g_rpcCount[(static_cast<std::uint64_t>(server) << 32) | fn]), hex);
    }
    if (recv && rsize && InRam(recv, rsize)) memset(g_ram + Phys(recv), 0, rsize);
    if (server == 0x80000100 && fn == 1 && recv && rsize >= 24 && InRam(recv, 24)) {  // pad library, function 1: the reference's answer
      static const std::uint32_t reply[6] = {0x0A, 1, 0, 0x0FFF, 1, 1};
      memcpy(g_ram + Phys(recv), reply, sizeof reply);
    }
    if (server == 0x80000400 && fn == 254 && recv && rsize >= 12 && InRam(recv, 12)) {
      // The game's file manager (0x0028DD40) checks the I/O side's version words: word 1 >= 522 and word 2 >= 526,
      // otherwise it prints an error and drops the server (every later open returns -100).
      static const std::uint32_t reply[3] = {0, 522, 526};
      memcpy(g_ram + Phys(recv), reply, sizeof reply);
    }
    if (server == 0x80000593 && recv && InRam(recv, 4)) {  // libcdvd system commands
      if (fn == 3) Wr<std::uint32_t>(recv, 0x14);          // sceCdGetDiskType: PS2 DVD
      else if (fn == 4) Wr<std::uint32_t>(recv, 0);        // sceCdGetError: none
      else if (fn == 34) Wr<std::uint32_t>(recv, 1);       // (unidentified command: success)
    }
    if (server == 0x80000597 && fn == 0 && A(s, 3) && InRam(A(s, 3), 300) && recv && InRam(recv, 4)) {  // sceCdSearchFile
      const std::uint32_t buffer = A(s, 3);
      const std::string path = GuestString(buffer + 36, 250);
      std::uint32_t lsn = 0, size = 0;
      const bool found = DiscFind(path, lsn, size);
      Log("io: sceCdSearchFile(\"%s\") -> %s (sector %u, %u bytes)", path.c_str(), found ? "found" : "NOT FOUND", lsn, size);
      if (found) {
        Wr<std::uint32_t>(buffer, lsn);
        Wr<std::uint32_t>(buffer + 4, size);
        std::string name = path.substr(path.find_last_of("\\/") + 1);
        if (const auto semi = name.find(';'); semi != std::string::npos) name.erase(semi);
        for (int i = 0; i < 16; ++i) g_ram[Phys(buffer) + 8 + i] = i < static_cast<int>(name.size()) && i < 15 ? static_cast<std::uint8_t>(name[i]) : 0;
        memset(g_ram + Phys(buffer) + 24, 0, 12);
      }
      Wr<std::uint32_t>(recv, found ? 1 : 0);
    }
    if (server == 0x80000595 && fn == 15 && A(s, 3) && InRam(A(s, 3), 788)) {  // sceCdReadChain: (sector, count, address) x n, ended by -1
      const std::uint32_t tags = A(s, 3);
      for (std::uint32_t i = 0; i < 64; ++i) {
        const std::uint32_t lsn = Rd<std::uint32_t>(tags + 12 * i), count = Rd<std::uint32_t>(tags + 12 * i + 4);
        const std::uint32_t address = Rd<std::uint32_t>(tags + 12 * i + 8) & ~1u;
        if (lsn == 0xFFFFFFFFu) break;
        if (static_cast<std::int32_t>(lsn) < 0 || !InRam(address, count * 2048) || !DiscRead(lsn * 2048ull, g_ram + Phys(address), count * 2048ull))
          Log("io: sceCdReadChain entry (sector %d, %u sectors, %08X) not served", static_cast<std::int32_t>(lsn), count, address);
        else
          ++g_discReads;
      }
    }
    if (server == 0x80000595 && fn == 1 && A(s, 4) >= 12 && InRam(A(s, 3), 12)) {  // sceCdRead(lsn, sectors, buffer, ...)
      const std::uint32_t lsn = Rd<std::uint32_t>(A(s, 3)), sectors = Rd<std::uint32_t>(A(s, 3) + 4), buf = Rd<std::uint32_t>(A(s, 3) + 8);
      if (InRam(buf, sectors * 2048) && DiscRead(lsn * 2048ull, g_ram + Phys(buf), sectors * 2048ull))
        ++g_discReads;
      else
        Log("io: sceCdRead(lsn %u, %u sectors, %08X) failed", lsn, sectors, buf);
    }
    if (server == 0x00012345 && fn == 0 && recv && rsize >= 0x120 && InRam(recv, rsize) && InRam(A(s, 3), 4096)) StreamerService(A(s, 3), recv);
    if (const std::uint32_t endfn = A(s, 7)) {  // asynchronous call: the I/O processor's answer runs the end function
      const std::uint32_t sp = static_cast<std::uint32_t>(s.gpr[29][0]);
      const std::uint32_t endarg = InRam(sp, 4) ? Rd<std::uint32_t>(sp) : 0;  // ninth argument: first stack word
      NativeState tmp = s;
      tmp.gpr[29][0] -= 0x80;
      CallGuest(tmp, endfn, endarg, 0);
    }
    Ret(s, 0);
  } else if (pc == kHookOpen) {  // sceOpen(path, flags)
    Ret(s, FileOpen(A(s, 0)));
  } else if (pc == kHookClose) {  // sceClose(fd)
    g_files.erase(static_cast<int>(A(s, 0)));
    Ret(s, 0);
  } else if (pc == kHookLseek) {  // sceLseek(fd, offset, whence)
    Ret(s, FileSeek(static_cast<int>(A(s, 0)), static_cast<std::int32_t>(A(s, 1)), static_cast<int>(A(s, 2))));
  } else if (pc == kHookRead) {  // sceRead(fd, buffer, size)
    Ret(s, FileRead(static_cast<int>(A(s, 0)), A(s, 1), A(s, 2)));
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
    if (g_profile) ++g_entryCount[pc];
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
    {
      std::string error;
      g_disc = hp2::VirtualDisc::Open(rom.wstring(), error);
      if (!g_disc) fail("cannot open the disc from the ROM folder: {}", error);
      Log("Disc: %s", g_disc->Description().c_str());
    }

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
    {
      std::vector<std::pair<std::uint64_t, std::uint32_t>> top;
      for (const auto& [pc, n] : g_entryCount) top.emplace_back(n, pc);
      std::sort(top.rbegin(), top.rend());
      if (FILE* f = fopen(".cache/native/entries.txt", "w")) {  // every entry place, for comparisons with the reference traces
        for (const auto& [n, pc] : top) fprintf(f, "%08X\t%llu\n", pc, static_cast<unsigned long long>(n));
        fclose(f);
      }
      if (!g_funcOrder.empty())
        if (FILE* f = fopen(".cache/native/functions.txt", "w")) {  // functions in order of first entry, with entry counts
          for (std::uint32_t fn : g_funcOrder) fprintf(f, "%08X\t%llu\n", fn, static_cast<unsigned long long>(g_funcCount[fn]));
          fclose(f);
        }
      Log("  distinct entry places: %zu; most entered:", top.size());
      for (std::size_t i = 0; i < top.size() && i < 24; ++i)
        Log("    %08X: %llu", top[i].second, static_cast<unsigned long long>(top[i].first));
    }
    Log("  VIF1: %llu codes, %llu interrupts requested, %llu VU1 starts (not run), mark %04X; INTC stat %04X mask %04X pending %04X; disc reads %llu",
        static_cast<unsigned long long>(g_vifCodes), static_cast<unsigned long long>(g_vifInterrupts),
        static_cast<unsigned long long>(g_vifStarts), g_vifMark, g_intcStat, g_intcMask, g_intcPending,
        static_cast<unsigned long long>(g_discReads));
    for (const auto& [key, n] : g_rpcCount)
      Log("  rpc server %08X function %u: %llu calls", static_cast<std::uint32_t>(key >> 32), static_cast<std::uint32_t>(key),
          static_cast<unsigned long long>(n));
    for (std::size_t i = 1; i < g_threads.size(); ++i) {
      const Thread& t = *g_threads[i];
      if (t.used)
        Log("  thread %zu: status %d, priority %d, at %08X (called from %08X)%s", i, t.status, t.priority, t.s.pc,
            static_cast<std::uint32_t>(t.s.gpr[31][0]), t.wait_sema >= 0 ? " waiting for a semaphore" : t.sleeping ? " sleeping" : "");
      if (t.used && t.s.pc == 0x00245830) {  // the state loop: show what it is running (diagnostic)
        const std::uint32_t v1 = static_cast<std::uint32_t>(t.s.gpr[3][0]), a3 = static_cast<std::uint32_t>(t.s.gpr[7][0]);
        if (InRam(v1 + 24) && InRam(a3 + 384))
          Log("    loop: v1=%08X target=%08X obj=%08X state292=%u state376=%08X", v1, Rd<std::uint32_t>(v1 + 20), a3,
              Rd<std::uint32_t>(a3 + 292), Rd<std::uint32_t>(a3 + 376));
        if (InRam(a3 + 512)) {
          std::string d = "    obj words:";
          for (std::uint32_t o = 0; o < 400; o += 4) {
            const std::uint32_t w = Rd<std::uint32_t>(a3 + o);
            if (w) d += " +" + std::to_string(o) + "=" + [&] { char b[12]; snprintf(b, sizeof b, "%X", w); return std::string(b); }();
          }
          Log("%s", d.c_str());
          const std::uint32_t o2 = Rd<std::uint32_t>(a3 + 380);
          if (InRam(o2 + 1200)) Log("    obj380=%08X +1160=%08X +1168=%08X", o2, Rd<std::uint32_t>(o2 + 1160), Rd<std::uint32_t>(o2 + 1168));
        }
      }
    }
    if (g_log) fclose(g_log);
    return 0;
  } catch (const std::exception& e) {
    Log("error: %s", e.what());
    return 3;
  }
}
