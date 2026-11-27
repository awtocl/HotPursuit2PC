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

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <atomic>
#include <filesystem>
#include <thread>

#include <algorithm>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <algorithm>

#include "virtual_disc.h"
#include "audio.h"
#include "gs.h"
#include "gs_gpu.h"
#include "memcard.h"
#include <set>
#include <string>
#include <vector>

#include "hp2/code_package.h"
#include "hp2/elf.h"
#include "hp2/game_patches.h"
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

// ---- development: sampling profiler of the game's thread (HP2_SAMPLE=<script s>: from then until the end) ------------
// Every millisecond a helper thread stops the game's thread for an instant and notes where it is. At the end the places are
// named: runtime functions by the build's symbols (dev preset), translated game code by the guest function it belongs to.
std::atomic<bool> g_sampleOn{false}, g_sampleQuit{false};
std::thread g_sampler;
struct SampleKey {
  std::uint64_t rip, caller;  // caller: the first return address into this executable on the stack (system calls: who made them)
  std::string group;          // "game" (the game's thread) or a group of the GS's threads (gs::ProfiledThreads)
  bool operator==(const SampleKey& o) const { return rip == o.rip && caller == o.caller && group == o.group; }
};
struct SampleKeyHash {
  std::size_t operator()(const SampleKey& k) const { return std::hash<std::uint64_t>()(k.rip * 31 + k.caller) ^ std::hash<std::string>()(k.group); }
};
std::unordered_map<SampleKey, std::uint32_t, SampleKeyHash> g_samples;
void StartSampler() {
  HANDLE self = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                  THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
  g_sampler = std::thread([self] {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(dos) + dos->e_lfanew);
    std::uint64_t exeLo = reinterpret_cast<std::uint64_t>(dos), exeHi = exeLo + nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)  // code only (stack values pointing at data are not callers)
      if (memcmp(sec[i].Name, ".text", 5) == 0) {
        exeHi = exeLo + sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
        exeLo += sec[i].VirtualAddress;
      }
    std::vector<std::pair<std::string, void*>> threads;
    for (unsigned tick = 0; !g_sampleQuit; ++tick) {
      Sleep(1);
      if (!g_sampleOn) continue;
      if (tick % 256 == 0) {  // the GS's threads (they start late)
        threads = gs::ProfiledThreads();
        threads.insert(threads.begin(), {"game", self});
      }
      for (const auto& [group, handle] : threads) {
        HANDLE h = static_cast<HANDLE>(handle);
        if (SuspendThread(h) == static_cast<DWORD>(-1)) continue;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL;
        const bool ok = GetThreadContext(h, &c) != 0;
        std::uint64_t stack[96] = {};
        if (ok) {  // (a shallow stack ends sooner: ReadProcessMemory fails instead of faulting; then a shorter read)
          SIZE_T got = 0;
          if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c.Rsp), stack, sizeof stack, &got))
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c.Rsp), stack, 64, &got);
        }
        ResumeThread(h);  // (before touching the heap: the thread may hold its lock)
        if (!ok) continue;
        std::uint64_t caller = 0;
        if (c.Rip < exeLo || c.Rip >= exeHi)
          for (std::uint64_t v : stack)
            if (v >= exeLo && v < exeHi) { caller = v; break; }
        ++g_samples[{c.Rip, caller, group}];
      }
    }
    CloseHandle(self);
  });
}
void ReportSamples() {
  if (!g_sampler.joinable()) return;
  g_sampleQuit = true;
  g_sampler.join();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  std::vector<std::pair<std::uintptr_t, std::uint32_t>> starts;  // host code of each translated game function -> guest address
  for (std::uint32_t i = 0; i < g_table->function_count; ++i)
    starts.emplace_back(reinterpret_cast<std::uintptr_t>(g_table->functions[i].code), g_table->functions[i].address);
  std::sort(starts.begin(), starts.end());
  HMODULE codeModule = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(g_table->functions[0].code), &codeModule);
  auto nameOf = [&](std::uint64_t rip) -> std::string {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(rip), &m);
    char name[256];
    if (m && m == codeModule) {
      auto it = std::upper_bound(starts.begin(), starts.end(), std::make_pair(static_cast<std::uintptr_t>(rip), ~0u));
      snprintf(name, sizeof name, "game %08X", it == starts.begin() ? 0u : std::prev(it)->second);
      return name;
    }
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 200] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 199;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym)) return sym->Name;
    wchar_t file[MAX_PATH] = L"?";
    if (m) GetModuleFileNameW(m, file, MAX_PATH);
    snprintf(name, sizeof name, "[%ls]", std::filesystem::path(file).filename().c_str());
    return name;
  };
  std::map<std::string, std::map<std::string, std::uint64_t>> byGroup;  // group -> name -> samples
  std::map<std::string, std::uint64_t> totals;
  for (const auto& [key, n] : g_samples) {
    totals[key.group] += n;
    byGroup[key.group][key.caller ? nameOf(key.rip) + " <- " + nameOf(key.caller) : nameOf(key.rip)] += n;
  }
  for (const auto& [group, names] : byGroup) {
    std::vector<std::pair<std::uint64_t, std::string>> top;
    for (const auto& [k, v] : names) top.emplace_back(v, k);
    std::sort(top.rbegin(), top.rend());
    const std::uint64_t total = totals[group];
    Log("profile: %s threads: %llu samples (1 ms apart, all threads of the group together)", group.c_str(), static_cast<unsigned long long>(total));
    for (std::size_t i = 0; i < top.size() && i < 30; ++i)
      Log("profile: %s %5.1f%%  %s", group.c_str(), 100.0 * static_cast<double>(top[i].first) / static_cast<double>(total ? total : 1), top[i].second.c_str());
  }
  SymCleanup(GetCurrentProcess());
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
void PadPoll();
std::uint64_t g_vblanks = 0, g_steps = 0, g_syscalls = 0, g_rpcCalls = 0;
std::unordered_map<std::uint32_t, std::uint64_t> g_funcCount;  // function -> entries (traced packages)
std::vector<std::uint32_t> g_funcOrder;                        // functions in order of their first entry
LARGE_INTEGER g_qpf, g_nextVblank;
std::uint64_t g_idleTicks = 0;  // time with every game thread waiting (perf line: what is left of each refresh)
std::uint64_t g_codeTicks = 0, g_vu1Ticks = 0;  // time in translated EE code (everything it starts included), in VU1 microprograms
// Tests (HP2_TURBO=n or max): refreshes n times faster than real time, or as soon as every game thread waits. The game's
// logic follows the refresh count, so a scripted run (HP2_PRESS, --until) gives the same result in less time. Silent.
double g_turbo = 1.0;
bool g_turboMax = false;
std::uint64_t g_untilVblank = 0;  // --until <game seconds>: stop at this refresh
std::uint64_t g_turboUntil = 0;   // HP2_TURBO_UNTIL=<game seconds>: back to real time there (menus animate on real-time timers)
std::uint32_t g_lastEntry = 0, g_sinceCheck = 0;
std::map<int, std::uint32_t> g_userSyscalls;  // system calls installed by the game (SetSyscall)
bool g_stop = false;
std::string g_stopReason;
bool g_menuMusic = false;        // the menus' music has started (stream 0 on voice 2E): the title has been passed
// Tests: with HP2_FAST_BOOT every scripted time (HP2_PRESS, HP2_DUMP_AT, HP2_SHOT_AT, --until) counts from the main menu.
const bool g_fastBoot = std::getenv("HP2_FAST_BOOT") != nullptr;
std::uint64_t g_timeBase = 0;    // refresh of the main menu's arrival (fast boot), else 0
bool ScriptTime(std::uint64_t& t) {  // refreshes since the script's origin; false while a fast boot is still on its way
  if (g_fastBoot && !g_timeBase) return false;
  t = g_vblanks - g_timeBase;
  return true;
}
bool g_calibrateScreen = false;  // the graphics settings were opened from the game's calibration screen (confirm it on close)

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
void RaiseGsInterrupt();  // INTC cause 0 (GS), used by GS SIGNAL/FINISH
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

// ---- GIF / GS register stream (the renderer comes later; for now the packets are decoded and counted) ----
std::uint64_t g_gsReg[0x80] = {};        // last value written to each GS register
std::uint64_t g_gsRegCount[0x80] = {};   // writes per register
std::uint64_t g_gifTags = 0, g_gifImageBytes = 0, g_gsVertices = 0, g_gsKicks = 0, g_gsPrims = 0;
struct GifStream {  // streaming decoder: packets may continue across calls
  std::uint32_t nloop = 0, nreg = 0, flg = 0, index = 0;
  std::uint64_t regs = 0;
  bool eop = true;
  std::uint64_t imageLeft = 0;
} g_gif[3];  // path 1 (VU1 XGKICK), path 2 (VIF1 DIRECT), path 3 (DMA channel 2)

void GsRegWrite(std::uint32_t reg, std::uint64_t value) {
  reg &= 0x7F;
  gs::RegWrite(reg, value);
  g_gsReg[reg] = value;
  ++g_gsRegCount[reg];
  if (reg == 0x00) ++g_gsPrims;                                     // PRIM
  if (reg == 0x04 || reg == 0x05) { ++g_gsVertices; ++g_gsKicks; }  // XYZF2, XYZ2 (drawing kicks)
  if (reg == 0x0C || reg == 0x0D) ++g_gsVertices;                   // XYZF3, XYZ3 (no kick)
}

void GifFeed(int path, const std::uint8_t* d, std::size_t bytes) {
  GifStream& g = g_gif[path];
  for (std::size_t pos = 0; pos + 16 <= bytes; pos += 16) {
    std::uint64_t lo, hi;
    memcpy(&lo, d + pos, 8);
    memcpy(&hi, d + pos + 8, 8);
    if (g.imageLeft) { --g.imageLeft; g_gifImageBytes += 16; gs::ImageData(d + pos, 16); continue; }
    if (g.nloop == 0) {  // a new tag
      g.nloop = lo & 0x7FFF;
      g.eop = ((lo >> 15) & 1) != 0;
      g.flg = (lo >> 58) & 3;
      g.nreg = (lo >> 60) & 15;
      if (g.nreg == 0) g.nreg = 16;
      g.regs = hi;
      g.index = 0;
      ++g_gifTags;
      static int shown[3] = {};
      if ((g.flg >= 2 && g.nloop > 2000) || shown[path] < 6) {  // diagnostic: the first tags of each path and every big transfer
        static int big = 0;
        if (shown[path] < 6 || big++ < 12) Log("gif: path %d tag nloop=%u eop=%d flg=%u nreg=%u regs=%016llX", path + 1, g.nloop, g.eop, g.flg, g.nreg, static_cast<unsigned long long>(g.regs));
        ++shown[path];
      }
      if (((lo >> 46) & 1) && g.flg == 0) GsRegWrite(0x00, (lo >> 47) & 0x7FF);  // PRE: the tag's PRIM
      if (g.nloop != 0 && g.flg >= 2) { g.imageLeft = g.nloop; g.nloop = 0; }
      continue;
    }
    const std::uint32_t reg = (g.regs >> (g.index * 4)) & 15;
    if (g.flg == 0) {  // PACKED
      std::uint32_t w[4];
      memcpy(w, d + pos, 16);
      const std::uint64_t xy = (w[0] & 0xFFFFull) | ((w[1] & 0xFFFFull) << 16);
      switch (reg) {
        case 0: GsRegWrite(0x00, lo & 0x7FF); break;
        case 1: GsRegWrite(0x01, (w[0] & 0xFFull) | ((w[1] & 0xFFull) << 8) | ((w[2] & 0xFFull) << 16) | ((w[3] & 0xFFull) << 24)); break;
        case 2: GsRegWrite(0x02, lo); gs::StQ(w[0], w[1], w[2]); break;
        case 3: GsRegWrite(0x03, (w[0] & 0x3FFFull) | ((w[1] & 0x3FFFull) << 16)); break;
        case 4: GsRegWrite((w[3] & 0x8000) ? 0x0C : 0x04, xy | (((w[2] >> 4) & 0xFFFFFFull) << 32) | (((w[3] >> 4) & 0xFFull) << 56)); break;
        case 5: GsRegWrite((w[3] & 0x8000) ? 0x0D : 0x05, xy | (static_cast<std::uint64_t>(w[2]) << 32)); break;
        case 6: GsRegWrite(0x06, lo); break;
        case 7: GsRegWrite(0x07, lo); break;
        case 8: GsRegWrite(0x08, lo); break;
        case 9: GsRegWrite(0x09, lo); break;
        case 10: GsRegWrite(0x0A, (static_cast<std::uint64_t>(w[3] >> 4) & 0xFF) << 56); break;
        case 12: GsRegWrite(0x0C, xy | (((w[2] >> 4) & 0xFFFFFFull) << 32) | (((w[3] >> 4) & 0xFFull) << 56)); break;
        case 13: GsRegWrite(0x0D, xy | (static_cast<std::uint64_t>(w[2]) << 32)); break;
        case 14: GsRegWrite(static_cast<std::uint32_t>(hi & 0x7F), lo); break;  // A+D
        default: break;
      }
      if (++g.index == g.nreg) { g.index = 0; --g.nloop; }
    } else {  // REGLIST: two 64-bit values per quadword
      GsRegWrite(reg, lo);
      if (++g.index == g.nreg) { g.index = 0; --g.nloop; }
      if (g.nloop) {
        GsRegWrite((g.regs >> (g.index * 4)) & 15, hi);
        if (++g.index == g.nreg) { g.index = 0; --g.nloop; }
      }
    }
  }
}

// ---- VU1 ---------------------------------------------------------------------------------
NativeState g_vu1;
std::uint32_t g_vifBase = 0, g_vifOfst = 0, g_vifTops = 0, g_vifTop = 0, g_vifItops = 0, g_vifItop = 0, g_vifMode = 0, g_vifDbf = 0;
std::uint32_t g_vifMask = 0, g_vifRow[4] = {}, g_vifCol[4] = {};
std::vector<int> g_vuImageState;  // 0 unknown, 1 microcode memory equals the image, -1 it does not
std::uint64_t g_vuRuns = 0, g_vuCalls = 0, g_vuMissing = 0, g_xgkicks = 0;

std::uint32_t VuPortRead(std::uint8_t*, std::uint32_t address) {
  return address == native_code_layout().addr_top ? g_vifTop : address == native_code_layout().addr_itop ? g_vifItop : 0;
}
void VuPortWrite(std::uint8_t*, std::uint32_t address, std::uint32_t value) {
  if (address != native_code_layout().addr_xgkick) return;
  ++g_xgkicks;
  std::uint32_t a = (value & 0x3FF) * 16;  // (the register holds a quadword index) a packet in VU1 data memory, ends at its tag's EOP (wraps at 16 KB)
  for (int guard = 0; guard < 4096; ++guard) {
    GifFeed(0, g_vu1Data + a, 16);
    a = (a + 16) & 0x3FFF;
    if (g_gif[0].nloop == 0 && g_gif[0].imageLeft == 0 && g_gif[0].eop) break;
  }
}
struct VuRuntimeTable {
  std::uint32_t (*read)(std::uint8_t*, std::uint32_t);
  void (*write)(std::uint8_t*, std::uint32_t, std::uint32_t);
};
const VuRuntimeTable g_vuRuntime{&VuPortRead, &VuPortWrite};
using VuCodeFn = void (*)(std::uint8_t*, std::uint8_t*, const void*);

VuCodeFn VuCodeAt(std::uint32_t pc) {
  if (!g_table) return nullptr;
  if (g_vuImageState.size() != g_table->vu_image_count) g_vuImageState.assign(g_table->vu_image_count, 0);
  for (std::uint32_t i = 0; i < g_table->vu_image_count; ++i) {
    const Hp2VuImageTable& im = g_table->vu_images[i];
    if (pc < im.address || pc >= im.address + im.word_count * 4) continue;
    if (g_vuImageState[i] == 0) g_vuImageState[i] = memcmp(g_vu1Micro + im.address, im.words, im.word_count * 4) == 0 ? 1 : -1;
    if (g_vuImageState[i] != 1) continue;
    if (std::binary_search(im.block_starts, im.block_starts + im.block_count, pc)) return reinterpret_cast<VuCodeFn>(im.code);
    const Hp2CodeEntry* end = im.entries + im.entry_count;
    const Hp2CodeEntry* it = std::lower_bound(im.entries, end, pc, [](const Hp2CodeEntry& e, std::uint32_t a) { return e.address < a; });
    return it != end && it->address == pc ? reinterpret_cast<VuCodeFn>(it->code) : nullptr;
  }
  return nullptr;
}

void RunVu1(std::uint32_t pc) {
  ++g_vuRuns;
  struct Timer {  // (the GS work its XGKICKs start is included)
    LARGE_INTEGER t0;
    Timer() { QueryPerformanceCounter(&t0); }
    ~Timer() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); g_vu1Ticks += static_cast<std::uint64_t>(t1.QuadPart - t0.QuadPart); }
  } timer;
  g_vu1.pc = pc & 0x3FFF;
  g_vu1.has_exception = 0;
  for (int guard = 0; guard < 200000; ++guard) {
    g_vu1.cycle_quota = 1 << 20;
    VuCodeFn code = VuCodeAt(g_vu1.pc);
    if (!code) {
      ++g_vuMissing;
      static int shown = 0;
      if (shown++ < 10) Log("vu1: no translated microcode at %04X (microprogram not among the executable's images)", g_vu1.pc);
      break;
    }
    code(reinterpret_cast<std::uint8_t*>(&g_vu1), g_vu1Data, &g_vuRuntime);
    ++g_vuCalls;
    if (g_vu1.has_exception & ~kNativeQuotaDone) break;  // E-bit / D-bit / T-bit: the program ended
    g_vu1.has_exception = 0;
  }
  g_vu1.has_exception = 0;
}

void VifUnpack(std::uint32_t cmd, std::uint32_t num, std::uint32_t imm, const std::vector<std::uint8_t>& d, std::size_t& pos) {
  const std::uint32_t cl = g_vifCycle & 0xFF, wl = (g_vifCycle >> 8) & 0xFF, vn = (cmd >> 2) & 3, vl = cmd & 3;
  const bool usn = (imm & 0x4000) != 0;
  std::uint32_t dest = imm & 0x3FF;
  if (imm & 0x8000) dest += g_vifTops;
  const std::uint32_t total = num ? num : 256;
  const std::uint32_t bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
  const std::size_t in = pos;
  auto fetch = [&](std::uint32_t bitOffset, std::uint32_t width) -> std::uint32_t {  // one element, sign/zero extended
    std::uint32_t v = 0;
    const std::size_t at = in + bitOffset / 8;
    for (std::uint32_t i = 0; i < width / 8 && at + i < d.size(); ++i) v |= static_cast<std::uint32_t>(d[at + i]) << (8 * i);
    if (!usn && width < 32) v = static_cast<std::uint32_t>(static_cast<std::int32_t>(v << (32 - width)) >> (32 - width));
    return v;
  };
  std::uint32_t consumed = 0;  // input vectors read so far
  const std::uint32_t wlz = wl ? wl : 1;
  for (std::uint32_t j = 0; j < total; ++j) {
    std::uint32_t slot = j;
    bool real = true;
    if (wl > cl) real = (j % wl) < cl;  // fill-write: cycles beyond CL write only mask/row/col values
    else if (cl > wl) slot = (j / wlz) * cl + (j % wlz);
    std::uint32_t e[4] = {0, 0, 0, 0};
    if (real) {
      const std::uint32_t o = consumed * bits;
      if (vn == 3 && vl == 3) {  // V4-5: RGBA5551
        const std::uint32_t p = fetch(o, 16) & 0xFFFF;
        e[0] = (p & 31) << 3; e[1] = ((p >> 5) & 31) << 3; e[2] = ((p >> 10) & 31) << 3; e[3] = ((p >> 15) & 1) << 7;
      } else {
        const std::uint32_t width = 32u >> vl;
        for (std::uint32_t k = 0; k <= vn; ++k) e[k] = fetch(o + k * width, width);
        if (vn == 0) e[1] = e[2] = e[3] = e[0];  // S: replicated
      }
      ++consumed;
    }
    const std::uint32_t addr = ((dest + slot) & 0x3FF) * 16;
    const std::uint32_t mi = (std::min)(j % wlz, 3u);
    for (std::uint32_t k = 0; k < 4; ++k) {
      const std::uint32_t m = (g_vifMask >> ((mi * 4 + k) * 2)) & 3;  // 0 data, 1 row, 2 col, 3 write-protected
      if (m == 3) continue;
      std::uint32_t value = e[k];
      if (m == 1) value = g_vifRow[k];
      else if (m == 2) value = g_vifCol[mi];
      else if (g_vifMode == 1) value += g_vifRow[k];
      else if (g_vifMode == 2) { value += g_vifRow[k]; g_vifRow[k] = value; }
      memcpy(g_vu1Data + addr + k * 4, &value, 4);
    }
  }
  pos += (static_cast<std::size_t>(consumed) * bits + 31) / 32 * 4;
}

// Bytes a VIF command takes in the stream, its code word included. The stream is continuous: a command whose data is
// not all there yet (the DMA packet ended) waits in g_vifCarry for the next transfer.
std::vector<std::uint8_t> g_vifCarry;
std::uint64_t g_vifCarried = 0;
std::size_t VifCommandBytes(std::uint32_t code) {
  const std::uint32_t cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
  if (cmd == 0x20) return 8;                                                   // STMASK
  if (cmd == 0x30 || cmd == 0x31) return 20;                                   // STROW, STCOL
  if (cmd == 0x4A) return 4 + static_cast<std::size_t>(num ? num : 256) * 8;   // MPG
  if (cmd == 0x50 || cmd == 0x51) return 4 + static_cast<std::size_t>(imm ? imm : 0x10000) * 16;  // DIRECT(HL)
  if (cmd >= 0x60) {                                                           // UNPACK
    const std::uint32_t cl = g_vifCycle & 0xFF, wl = (g_vifCycle >> 8) & 0xFF, vn = (cmd >> 2) & 3, vl = cmd & 3;
    const std::uint32_t total = num ? num : 256;
    const std::uint32_t read = (wl > cl && wl) ? (total / wl) * cl + (std::min)(total % wl, cl) : total;  // vectors taken from the stream
    const std::uint32_t bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
    return 4 + (static_cast<std::size_t>(read) * bits + 31) / 32 * 4;
  }
  return 4;
}

void ParseVif1(const std::vector<std::uint8_t>& incoming) {
  std::vector<std::uint8_t> joined;
  if (!g_vifCarry.empty()) {  // continue the command that was cut at the end of the previous transfer
    joined.swap(g_vifCarry);
    joined.insert(joined.end(), incoming.begin(), incoming.end());
    ++g_vifCarried;
  }
  const std::vector<std::uint8_t>& d = joined.empty() ? incoming : joined;
  std::size_t pos = 0;
  auto word = [&](std::size_t at) { std::uint32_t v = 0; if (at + 4 <= d.size()) memcpy(&v, d.data() + at, 4); return v; };
  while (pos + 4 <= d.size()) {
    const std::uint32_t code = word(pos);
    const std::uint32_t cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
    if (pos + VifCommandBytes(code) > d.size()) {  // incomplete: keep it (no side effect yet) for the next transfer
      g_vifCarry.assign(d.begin() + static_cast<std::ptrdiff_t>(pos), d.end());
      return;
    }
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
    else if (cmd == 0x02) { g_vifOfst = imm & 0x3FF; g_vifDbf = 0; g_vifTops = g_vifBase; }  // OFFSET
    else if (cmd == 0x03) g_vifBase = imm & 0x3FF;                       // BASE
    else if (cmd == 0x04) g_vifItops = imm & 0x3FF;                      // ITOP
    else if (cmd == 0x05) g_vifMode = imm & 3;                           // STMOD
    else if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) {                // MSCAL, MSCALF, MSCNT: run the microprogram now
      ++g_vifStarts;
      g_vifItop = g_vifItops;
      g_vifTop = g_vifTops;
      g_vifTops = g_vifDbf ? g_vifBase : g_vifBase + g_vifOfst;
      g_vifDbf ^= 1;
      RunVu1(cmd == 0x17 ? g_vu1.pc : imm * 8);
    }
    else if (cmd == 0x20) { g_vifMask = word(pos); pos += 4; }          // STMASK
    else if (cmd == 0x30 || cmd == 0x31) {                               // STROW, STCOL
      for (int k = 0; k < 4; ++k) (cmd == 0x30 ? g_vifRow : g_vifCol)[k] = word(pos + k * 4);
      pos += 16;
    }
    else if (cmd == 0x4A) {                                                // MPG: microprogram into VU1 code memory
      const std::size_t bytes = static_cast<std::size_t>(num ? num : 256) * 8;
      for (std::size_t i = 0; i < bytes && pos + i < d.size(); ++i) g_vu1Micro[(imm * 8 + i) & 0x3FFF] = d[pos + i];
      std::fill(g_vuImageState.begin(), g_vuImageState.end(), 0);
      pos += bytes;
    } else if (cmd == 0x50 || cmd == 0x51) {                             // DIRECT(HL): GIF packets (path 2)
      const std::size_t bytes = static_cast<std::size_t>(imm ? imm : 0x10000) * 16;
      if (pos < d.size()) GifFeed(1, d.data() + pos, (std::min)(bytes, d.size() - pos));
      pos += bytes;
    } else if (cmd >= 0x60) VifUnpack(cmd, num, imm, d, pos);          // UNPACK
  }
}

void RunDma(std::uint32_t channel, std::uint32_t base, std::uint32_t chcr) {
  if (channel == 1 && (chcr & 1) == 0) {  // VIF1 towards memory: the GS's local-to-host transfer (the race's sun test)
    const std::uint32_t madr = g_hw[base + 0x10], bytes = (g_hw[base + 0x20] & 0xFFFF) * 16;
    LogOnce("gsread", "hw: GS local-to-host transfer read by DMA (%u bytes to %08X)", bytes, madr);
    if (madr & 0x80000000u) {
      if ((madr & 0x3FFF) + bytes <= 0x4000) gs::LocalToHost(g_spr + (madr & 0x3FFF), bytes);
    } else if (InRam(madr, bytes)) {
      gs::LocalToHost(g_ram + Phys(madr), bytes);
    }
    return;
  }
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
    std::uint32_t asr[2] = {0, 0};  // address stack (CALL/RET), two levels
    unsigned asp = 0;
    for (int guard = 0; guard < 100000; ++guard) {
      const bool inSpr = (tadr & 0x80000000u) != 0;  // tag in scratchpad (bit 31 of the tag address)
      const std::uint8_t* tp = inSpr ? g_spr + (tadr & 0x3FF0) : (InRam(tadr, 16) ? g_ram + Phys(tadr) : nullptr);
      if (!tp) { LogOnce("dmatag" + std::to_string(tadr), "hw: DMA tag address %08X outside memory: chain stopped", tadr); break; }
      std::uint64_t tag;
      memcpy(&tag, tp, 8);
      const std::uint32_t id = (tag >> 28) & 7, qwc = tag & 0xFFFF, addr = static_cast<std::uint32_t>(tag >> 32);
      if (chcr & 0x40) data.insert(data.end(), tp + 8, tp + 16);  // TTE: the tag's upper half (two VIF codes) goes along
      // ids: 0 refe, 1 cnt, 2 next, 3 ref, 4 refs, 5 call, 6 ret, 7 end
      const bool afterTag = id == 1 || id == 2 || id == 5 || id == 6 || id == 7;
      take(afterTag ? tadr + 16 : addr, qwc);
      if ((chcr & 0x80) && (tag >> 31 & 1)) break;  // TIE with the tag's IRQ bit: the transfer stops after this tag's data
      if (id == 0 || id == 7) break;
      if (id == 5) {  // CALL: push the address of the packet after this one's data
        if (asp >= 2) { LogOnce("dmaasr", "hw: DMA CALL with the address stack full: chain stopped"); break; }
        asr[asp++] = tadr + 16 + qwc * 16;
        tadr = addr;
      } else if (id == 6) {  // RET
        if (asp == 0) { LogOnce("dmaasr0", "hw: DMA RET with the address stack empty: chain ends"); break; }
        tadr = asr[--asp];
      } else if (id == 2) tadr = addr;
      else tadr += 16 + (id == 1 ? qwc * 16 : 0);
    }
  }
  if (channel == 1) ParseVif1(data);
  else if (channel == 2) GifFeed(2, data.data(), data.size());
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
    case 0x12001000: case 0x12001010: case 0x12001080: return gs::PrivRead(a, static_cast<std::uint32_t>(g_vblanks & 1));  // GS CSR, IMR, SIGLBLID
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
  if (a >= 0x12000000 && a < 0x12001100) gs::PrivWrite(a, value, size >= 8 ? 8 : 4);
  switch (a) {
    case 0x1000F000: g_intcStat &= ~v; return;
    case 0x1000F010: g_intcMask ^= v; return;
    case 0x1000E010: g_dmacStat = (g_dmacStat & ~(v & 0xFFFF)) ^ (v & 0xFFFF0000); return;
    case 0x12001000: case 0x12001010: case 0x12001080: return;  // GS CSR/IMR/SIGLBLID: handled by gs::PrivWrite
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

// VU0 is one unit shared by every thread: the console's kernel saves the CPU and FPU registers on a thread switch, not
// COP2 (vf/vi registers, ACC, Q, flags and their pipelines), so whatever one thread or handler leaves in VU0 the next
// one sees. Each NativeState carries its own copy; the state about to run takes over the current contents first.
NativeState* g_vu0Owner = nullptr;
void Vu0TakeOver(NativeState& to) {
  if (g_vu0Owner && g_vu0Owner != &to) {
    constexpr std::size_t from = offsetof(NativeState, vf), size = offsetof(NativeState, saved_next_index) - from;
    memcpy(reinterpret_cast<std::uint8_t*>(&to) + from, reinterpret_cast<const std::uint8_t*>(g_vu0Owner) + from, size);
  }
  g_vu0Owner = &to;
}

void Step(NativeState& s);
void CallGuest(NativeState& s, std::uint32_t function, std::uint32_t a0, std::uint32_t a1) {
  Vu0TakeOver(s);
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

void RaiseGsInterrupt() {
  g_intcPending |= 1u;
  g_intcStat |= 1u;
}

// Widescreen 16:9 (ADR 0015): the game patches of hp2/game_patches.h, written every refresh (as PCSX2 applies its patches: the
// game may write the data itself). The game lays its HUD out and squeezes its 2D front end into the middle of the frame
// from them early on, so the mode is chosen once, at the first refresh: switching between 16:9 and 4:3 needs a restart.
// The two HUD constants are the low halves of `lui` words the translation reads from memory.
// 60 frames per second: the frame loop's constant (hp2::patches::kFrameRate), written every refresh from the setting, which
// applies at once.
void ApplyGamePatches() {
  static const bool wide = [] {
    const bool w = gs::Widescreen();
    gs::SetWidePicture(w);
    Log("Display: %s", w ? "widescreen 16:9 (game patches on)" : "4:3");
    return w;
  }();
  if (wide) {
    for (const hp2::patches::DataWord& d : hp2::patches::kWidescreenWords)
      *reinterpret_cast<std::uint32_t*>(g_ram + d.address) = d.widescreen;
    for (const hp2::patches::Immediate& i : hp2::patches::kRuntimeImmediates)
      *reinterpret_cast<std::uint16_t*>(g_ram + i.address) = i.patched;
  }
  const bool fast = gs::FrameRate60();
  static int shown = -1;
  if (shown != (fast ? 1 : 0)) {
    shown = fast ? 1 : 0;
    Log("Frame rate: %s", fast ? "60 (game patch on)" : "30");
  }
  constexpr hp2::patches::Immediate f = hp2::patches::kFrameRate;
  *reinterpret_cast<std::uint16_t*>(g_ram + f.address) = fast ? f.patched : f.original;
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
    g_nextVblank.QuadPart += static_cast<LONGLONG>(static_cast<double>(g_qpf.QuadPart) * 1001.0 / 60000.0 / g_turbo);
    if (now.QuadPart > g_nextVblank.QuadPart) g_nextVblank.QuadPart = now.QuadPart;
    ++g_vblanks;
    ApplyGamePatches();
    if (g_turboUntil == ~0ull && g_menuMusic && g_turbo != 1.0) g_turboUntil = g_vblanks;  // HP2_TURBO_UNTIL=menu
    if (g_turboUntil && g_vblanks == g_turboUntil) {
      g_turbo = 1.0;
      g_turboMax = false;
      Log("Turbo: real time from game second %.0f", static_cast<double>(g_vblanks) * 1001.0 / 60000.0);
      // sound from here on (tests: HP2_AUDIO_SILENT plays it at volume 0, HP2_AUDIO_DUMP records it)
      if (!std::getenv("HP2_MUTE") && !audio::Available()) Log("Audio: %s", audio::Init() ? "XAudio2 ready" : audio::LastError());
    }
    std::uint64_t scriptTime = 0;
    {  // HP2_BENCH_AT=<script s>: from then on refreshes come 4x faster than real time, so the perf lines' refreshes/s is
       // how fast the runtime can go (60 = full speed at the 60 Hz cap)
      static const std::uint64_t benchAt = [] { const char* e = std::getenv("HP2_BENCH_AT"); return e ? static_cast<std::uint64_t>(atof(e) * 60000.0 / 1001.0) : 0ull; }();
      static bool benching = false;
      std::uint64_t t = 0;
      static const std::uint64_t sampleAt = [] { const char* e = std::getenv("HP2_SAMPLE"); return e ? static_cast<std::uint64_t>(atof(e) * 60000.0 / 1001.0) : 0ull; }();
      if (sampleAt && !g_sampleOn && ScriptTime(t) && t >= sampleAt) g_sampleOn = true;
      if (benchAt && !benching && ScriptTime(t) && t >= benchAt) {
        benching = true;
        g_turbo = 4.0;
        Log("Bench: refreshes 4x faster than real time from script second %.0f", static_cast<double>(t) * 1001.0 / 60000.0);
      }
    }
    if (g_untilVblank && ScriptTime(scriptTime) && scriptTime >= g_untilVblank) Stop("game time limit of the run (%.0f s of game time) reached", static_cast<double>(g_untilVblank) * 1001.0 / 60000.0);
    gs::VBlankStart();
    gs::Present();
    PadPoll();
    if (gs::WindowClosed()) Stop("the window was closed");
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
    case 0x77: {  // SifSetDma(list of {EE source, IOP destination, bytes, attributes}, count): accepted
      static const bool sifLog = std::getenv("HP2_SIF_LOG") != nullptr;  // development: every transfer
      if (sifLog)
        for (std::uint32_t k = 0, list = A(s, 0); k < A(s, 1) && k < 32; ++k) {
          const std::uint32_t at = list + k * 16;  // main memory or scratchpad
          const std::uint32_t* t = reinterpret_cast<const std::uint32_t*>(
              (at >> 28) == 7 ? g_spr + (at & 0x3FF0) : Phys(at) + 16 <= kNativeRamSize ? g_ram + Phys(at) : nullptr);
          if (!t) break;
          Log("sif: vblank %llu dma %08X -> IOP %08X, %u bytes, attr %X", static_cast<unsigned long long>(g_vblanks), t[0], t[1], t[2], t[3]);
        }
      Ret(s, 1);
      return;
    }
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
// ---- movie sound -------------------------------------------------------------------------------------------------------
// The game decodes its AVI movies (video "mv30", sound in "##wb" chunks) on the EE and hands the sound to its IOP sound
// library through a callback object the I/O side calls back into; the native streamer has no such side. Instead, when the
// game starts a movie's sound (stream 0 opened on sector 0 with size 0x7FFFFFFF, then command 01), the runtime plays the
// sound chunks of the AVI the game last started reading: PS-ADPCM, stereo, left and right alternating in 0x8000-byte blocks
// (the EA logo's track is 5 x 0x10000 bytes), at the pitch the game sets.
std::uint32_t g_aviLsn = 0;
bool g_musicIsMovie = false;  // the music channel plays a movie's sound
int g_movieCount = 0;         // movies started (each one's number tells the window a new picture to fit)  // first sector of the AVI file the game last read the start of (game thread)
std::mutex g_movieMutex;
std::vector<std::pair<std::uint64_t, std::uint32_t>> g_movieChunks;  // disc offset and size of each sound chunk, in order
std::vector<std::uint64_t> g_movieStarts;                            // offset of each chunk in the joined sound
void NoteDiscRead(std::uint32_t lsn, const std::uint8_t* data) {
  if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "AVI ", 4) == 0) g_aviLsn = lsn;
}
// Reads the joined sound of the current movie (the mixer's reader: its feeder thread).
bool MovieRead(std::uint64_t offset, void* out, std::uint64_t count) {
  std::lock_guard<std::mutex> lock(g_movieMutex);
  std::uint8_t* dst = static_cast<std::uint8_t*>(out);
  auto it = std::upper_bound(g_movieStarts.begin(), g_movieStarts.end(), offset);
  if (it == g_movieStarts.begin()) return false;
  for (std::size_t k = static_cast<std::size_t>(it - g_movieStarts.begin()) - 1; count > 0 && k < g_movieChunks.size(); ++k) {
    const std::uint64_t within = offset - g_movieStarts[k];
    const std::uint64_t n = std::min<std::uint64_t>(count, g_movieChunks[k].second - within);
    if (!DiscRead(g_movieChunks[k].first + within, dst, n)) return false;
    dst += n;
    offset += n;
    count -= n;
  }
  if (count > 0) std::memset(dst, 0, count);  // past the end: silence
  return true;
}
// Indexes the sound chunks of the AVI at `lsn` (from its idx1 index). Returns the joined sound's size, 0 if none.
std::uint32_t MovieIndex(std::uint32_t lsn) {
  const std::uint64_t base = lsn * 2048ull;
  std::uint8_t head[12] = {};
  if (!lsn || !DiscRead(base, head, 12) || std::memcmp(head, "RIFF", 4) != 0) return 0;
  std::uint32_t riff = 0;
  std::memcpy(&riff, head + 4, 4);
  // top-level chunks after "AVI ": LIST hdrl, LIST movi, idx1
  std::uint64_t at = 12, movi = 0, idx = 0;
  std::uint32_t idxSize = 0;
  while (at + 12 <= riff + 8ull) {
    std::uint8_t h[12] = {};
    if (!DiscRead(base + at, h, 12)) return 0;
    std::uint32_t size = 0;
    std::memcpy(&size, h + 4, 4);
    if (std::memcmp(h, "LIST", 4) == 0 && std::memcmp(h + 8, "movi", 4) == 0) movi = at + 8;  // idx1 offsets count from here
    if (std::memcmp(h, "idx1", 4) == 0) {
      idx = at + 8;
      idxSize = size;
    }
    at += 8ull + size + (size & 1);
  }
  if (!movi || !idx || idxSize < 16 || idxSize > 0x1000000) return 0;
  std::vector<std::uint8_t> entries(idxSize);
  if (!DiscRead(base + idx, entries.data(), idxSize)) return 0;
  std::vector<std::pair<std::uint64_t, std::uint32_t>> chunks;
  std::vector<std::uint64_t> starts;
  std::uint64_t total = 0;
  for (std::uint32_t e = 0; e + 16 <= idxSize; e += 16) {
    if (entries[e + 2] != 'w' || entries[e + 3] != 'b') continue;  // "##wb": sound
    std::uint32_t offset = 0, size = 0;
    std::memcpy(&offset, &entries[e + 8], 4);
    std::memcpy(&size, &entries[e + 12], 4);
    if (!size) continue;
    chunks.emplace_back(base + movi + offset + 8, size);  // (an idx1 offset points at the chunk's header)
    starts.push_back(total);
    total += size;
  }
  std::lock_guard<std::mutex> lock(g_movieMutex);
  g_movieChunks = std::move(chunks);
  g_movieStarts = std::move(starts);
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(total, 0x7FFFFFF0u));
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
// a status block out. Modelled from the reference core's traffic (docs/recomp-analysis.md section 18) and the game's own
// commands (HP2_STREAMER_LOG=1 logs every one): word 1 counts the batches, word 2 is -1, word 4 holds the result of the last
// command 0x0E ((a0 << 16) | a1) and, once command 0x34 has been seen, words 66-70 describe two buffers.
// Reply: word 0 = N entries, then 6 words each (parsed by the game at 0026B8C0): e1 = volume << 16 | voice << 10 |
// slot << 4 | buffer half << 2 | error, e2 = SPU play address, e3 bit 31 = playing, e4 = link byte, e5 = SPU buffer base,
// e6 = buffer wraps << 16; the words above follow at 6N + 1. The slot indexes the game's table of 48 voice states
// (0x322C20): a slot missing from a reply is free again. Slots 0-1 are the music's channels, 2 the speech channel, and a
// sound effect's slot is its SPU voice: effects missing from the reply were keyed off and restarted every 4 refreshes,
// the radio phrases restarted every 4 refreshes and the music player skipped to the next track every frame.
// Commands (stream = the word after nargs):
//   1E [stream, LSN hi, lo, file size hi, lo, 1, offset hi, lo]  opens a disc file on a stream
//   34 [stream, 0, SPU address hi, lo, bytes hi, lo]              copies the stream file's next bytes into sound memory
//                                                                 (the sound banks: 14 + 44 samples in the menus, ~90 in races)
//   1C [sample, SPU address hi, lo]                               a sample's start in sound memory
//   00 [sample, voice, left, right, pitch, ...]                   sets a voice up (volumes 0..0x3FFF, pitch 0x1000 = 48 kHz)
//   18, 19 [voice]                                                key on (18: effects the game keeps up, 19: one-shots)
//   04 [voice] key off; 03 [voice, pitch]; CB [voice, left, right, 6] volumes
//   29 [stream, offset hi, lo, length hi, lo]                     the part of the file to play
//   01 [stream, voice, volume, ?, pitch, ...]                     plays it: stream 0 = music (PS-ADPCM stereo, left and right
//                                                                 alternate in 0x8000-byte blocks), streams 2-5 = speech
//                                                                 (mono VAG clips, 24 kHz) on channel 2
//   2C [0x100 | stream, voice, ...]                               the music's second voice
//   1B [channel, count, (stream, length hi, lo, offset hi, lo) x count]  clips that follow on the channel (radio phrases)
//   05 [stream]                                                   stops a channel
// audio.cpp plays all of it. Without an audio device music and speech advance in game time and effects are not reported.
struct StreamerChannel {
  bool active = false, audioStarted = false;
  std::uint64_t start = 0, durationVblanks = 0;
  std::uint16_t voice[2] = {0x2E, 0x2F}, volume = 0x3FFF;
  std::uint32_t rate = 32332, bytes = 0;
} g_music, g_speech;
struct StreamerFile { std::uint32_t lsn = 0, size = 0, pos = 0, offset = 0, length = 0; } g_streamFile[8];
std::uint32_t g_sampleAddress[512];
std::uint64_t VblanksFor(std::uint32_t bytes, std::uint32_t rate) {  // PS-ADPCM bytes of one channel at `rate` Hz
  return static_cast<std::uint64_t>(bytes) / 16 * 28 * 60000 / 1001 / std::max<std::uint32_t>(rate, 1);
}
void StreamerService(std::uint32_t send, std::uint32_t recv) {
  static std::uint32_t batch = 0, result = 0, buf = 0;
  static bool have34 = false;
  const auto voiceStream = [](std::uint32_t v) {  // voices of the music and speech channels while they play
    return (g_music.active && (v == g_music.voice[0] || v == g_music.voice[1])) || (g_speech.active && v == g_speech.voice[0]);
  };
  const std::uint16_t* c = reinterpret_cast<const std::uint16_t*>(g_ram + Phys(send));
  std::uint32_t i = 1;
  for (std::uint32_t n = c[0]; n > 0 && i + 1 < 4096; --n) {
    const std::uint16_t id = c[i], na = c[i + 1];
    if (i + 2u + na > 4096) break;
    const std::uint16_t s = na > 0 ? c[i + 2] : 0;
    const auto word = [&](std::uint32_t k) { return (static_cast<std::uint32_t>(c[i + k]) << 16) | c[i + k + 1]; };
    switch (id) {
      case 0x0E:
        if (na >= 2) result = word(2);
        break;
      case 0x34:
        if (na >= 6 && s < 8) {  // [stream, 0, SPU address hi, lo, bytes hi, lo]
          if (!have34) {
            have34 = true;
            buf = word(2);
          }
          StreamerFile& f = g_streamFile[s];
          const std::uint32_t bytes = std::min<std::uint32_t>(word(6), 0x200000), at = word(4);
          if (f.lsn && bytes) {
            std::vector<std::uint8_t> data(bytes);
            if (DiscRead(f.lsn * 2048ull + f.pos, data.data(), bytes)) audio::SpuWrite(at, data.data(), bytes);
          }
          f.pos += bytes;
        }
        break;
      case 0x1E:
        if (na >= 8 && s < 8) g_streamFile[s] = {word(3), word(5), word(8), word(8), word(5) > word(8) ? word(5) - word(8) : 0};
        break;
      case 0x29:
        if (na >= 5 && s < 8) {
          g_streamFile[s].offset = word(3);
          g_streamFile[s].length = word(5);
        }
        break;
      case 0x1C:
        if (na >= 3 && s < 512) g_sampleAddress[s] = word(3);
        break;
      case 0x00:
        if (na >= 6 && s < 512 && c[i + 3] < 48) audio::VoiceSetup(c[i + 3], g_sampleAddress[s], c[i + 6], c[i + 4], c[i + 5]);
        break;
      case 0x18: case 0x19:
        if (na >= 1 && s < 48 && !voiceStream(s)) audio::VoiceKeyOn(s);
        break;
      case 0x04:
        if (na >= 1 && s < 48 && !voiceStream(s)) audio::VoiceKeyOff(s);
        break;
      case 0x03:
        if (na >= 2 && s < 48 && !voiceStream(s)) audio::VoicePitch(s, c[i + 3]);
        break;
      case 0xCB:
        if (na >= 3 && s < 48 && !voiceStream(s)) {
          audio::VoiceVolume(s, c[i + 3], c[i + 4]);
        } else if (na >= 3 && s < 48) {  // a stream's voice: the music's left voice sets its left side (the game pans it hard
          // left: [volume, 4]), the right voice its right side ([4, volume]); the speech voice both sides
          if (g_music.active && s == g_music.voice[0]) audio::StreamVolume(0, c[i + 3], -1);
          else if (g_music.active && s == g_music.voice[1]) audio::StreamVolume(0, -1, c[i + 4]);
          else audio::StreamVolume(2, c[i + 3], c[i + 4]);
        }
        break;
      case 0x01:
        if (na >= 5 && s < 8) {
          const StreamerFile& f = g_streamFile[s];
          StreamerChannel& ch = s == 0 ? g_music : g_speech;
          ch.active = true;
          ch.start = g_vblanks;
          ch.voice[0] = c[i + 3] & 0x3F;
          ch.volume = c[i + 4];
          if (s == 0 && ch.voice[0] == 0x2E) g_menuMusic = true;
          ch.rate = std::max<std::uint32_t>(static_cast<std::uint32_t>(c[i + 6]) * 48000u / 4096u, 4000);
          ch.bytes = f.length;
          ch.durationVblanks = f.length ? VblanksFor(s == 0 ? f.length / 2 : f.length, ch.rate) : ~0ull;
          const bool movie = s == 0 && f.lsn == 0 && f.size == 0x7FFFFFFFu;  // a movie's sound (see MovieRead)
          if (s == 0) g_musicIsMovie = movie;
          if (movie) ++g_movieCount;
          if (movie && audio::Available()) {
            const std::uint32_t bytes = MovieIndex(g_aviLsn);
            Log("streamer: vblank %llu movie sound: AVI at sector %u, %u bytes", static_cast<unsigned long long>(g_vblanks), g_aviLsn, bytes);
            ch.audioStarted = bytes != 0;
            if (bytes) audio::StreamStart(0, &MovieRead, 0, bytes, 2, 0x8000u, c[i + 6], c[i + 4], c[i + 4]);
            break;
          }
          ch.audioStarted = audio::Available() && f.lsn && f.length;
          if (ch.audioStarted)
            audio::StreamStart(s == 0 ? 0 : 2, &DiscRead, f.lsn * 2048ull + f.offset, f.length, s == 0 ? 2 : 1, 0x8000u, c[i + 6],
                               c[i + 4], s == 0 ? c[i + 4] : c[i + 5]);
        }
        break;
      case 0x2C:
        if (na >= 2 && (s & 0xFF) == 0) g_music.voice[1] = c[i + 3] & 0x3F;
        break;
      case 0x1B:
        if (na >= 2 && s == 2 && g_speech.active)
          for (std::uint32_t k = 0, m = c[i + 3]; k < m && 4 + k * 5 + 4 < na + 2u; ++k) {
            const std::uint32_t at = 4 + k * 5, st = c[i + at] & 7, len = word(at + 1), off = word(at + 3);
            if (g_speech.audioStarted && g_streamFile[st].lsn) audio::StreamQueue(2, g_streamFile[st].lsn * 2048ull + off, len);
            g_speech.durationVblanks += VblanksFor(len, g_speech.rate);
          }
        break;
      case 0x05:
        if (na >= 1 && (s == 0 || s == 2)) {
          StreamerChannel& ch = s == 0 ? g_music : g_speech;
          ch.active = false;
          audio::StreamStop(s);
        }
        break;
      default:
        break;
    }
    static const int allCommands = std::getenv("HP2_STREAMER_LOG") ? std::atoi(std::getenv("HP2_STREAMER_LOG")) : 0;  // development: 1 every command but the frequent 09 and 0E, 2 all
    if (allCommands ? (allCommands > 1 || (id != 0x0E && id != 0x09)) : (id == 0x01 || id == 0x05 || id == 0x1E || id == 0x29 || id == 0x2C || id == 0x1B)) {
      static int logged = 0;
      if (logged++ < (allCommands ? 200000 : 400)) {
        char args[128] = {};
        for (std::uint32_t k = 1, p = 0; k < na && k < 16 && p + 6 < sizeof args; ++k) p += static_cast<std::uint32_t>(snprintf(args + p, sizeof args - p, " %04X", c[i + 2 + k]));
        Log("streamer: vblank %llu cmd %02X stream %u args%s", static_cast<unsigned long long>(g_vblanks), id, s, args);
      }
    }
    i += 2u + na;
  }
  gs::SetFullScreenMovie(g_music.active && g_musicIsMovie ? g_movieCount : 0);  // (the window fills the screen with it)
  std::uint32_t* w = reinterpret_cast<std::uint32_t*>(g_ram + Phys(recv));
  std::uint32_t entries = 0;
  const auto entry = [&](std::uint32_t slot, std::uint32_t voice, std::uint32_t pos, std::uint32_t wraps, std::uint32_t base,
                         std::uint32_t at, bool first) {
    std::uint32_t* e = w + 1 + entries * 6;
    e[0] = 0x7FFF0000u | ((voice & 0x3F) << 10) | ((slot & 0x3F) << 4) | (pos >= 0x8000 ? 4u : 0u);
    e[1] = at;
    e[2] = first ? 0x80000000u : 0;
    e[3] = 0;
    e[4] = base;
    e[5] = first ? wraps << 16 : 0;
    ++entries;
  };
  for (int k = 0; k < 2; ++k) {  // music (slots 0 and 1) and speech (slot 2)
    StreamerChannel& ch = k == 0 ? g_music : g_speech;
    if (ch.active && (ch.audioStarted ? !audio::StreamPlaying(k == 0 ? 0 : 2) : g_vblanks - ch.start >= ch.durationVblanks)) {
      ch.active = false;  // it ended: the slot leaves the reply
      static int logged = 0;
      if (logged++ < 40) Log("streamer: vblank %llu %s ended", static_cast<unsigned long long>(g_vblanks), k == 0 ? "music" : "speech");
    }
    if (!ch.active) continue;
    const std::uint64_t bytes = ch.audioStarted ? audio::StreamFramesPlayed(k == 0 ? 0 : 2) * 16 / 28  // per voice, PS-ADPCM bytes
                                                : (g_vblanks - ch.start) * ch.rate * 16 / 28 * 1001 / 60000;
    const std::uint32_t pos = static_cast<std::uint32_t>(bytes & 0xFFFF), wraps = static_cast<std::uint32_t>(bytes >> 16);
    if (k == 0) {
      for (std::uint32_t c2 = 0; c2 < 2; ++c2) {
        const std::uint32_t base = 0x5010u + c2 * 0x10000u;
        entry(c2, ch.voice[c2], pos, wraps, base, base + pos, c2 == 0);
      }
    } else {
      entry(2, ch.voice[0], pos, wraps, 0x25010u, 0x25010u + pos, true);
    }
  }
  for (std::uint32_t v = 0; v < 48 && entries < 60; ++v) {  // sound effects: slot = voice
    std::uint32_t at = 0, start = 0;
    if (!voiceStream(v) && audio::VoicePlaying(static_cast<int>(v), &at, &start)) entry(v, v, 0, 0, start, at, true);
  }
  w[0] = entries;
  const std::uint32_t t = 1 + entries * 6;  // first word after the entries
  w[t] = ++batch;
  w[t + 1] = 0xFFFFFFFFu;
  w[t + 3] = result;
  if (have34) {
    w[t + 65] = 2;
    w[t + 66] = 3;
    w[t + 67] = buf;
    w[t + 69] = buf + 0x20000;
  }
}
// ---- pad (libpad's RPC server 80000100): the I/O side keeps a pad data area in the game's memory up to date -------------
struct PadPort { std::uint32_t address = 0; int type = 0; } g_pad[2];  // type 0: 0x40-byte structs, 1: 0x80-byte structs, 2: extended layout
std::uint16_t g_padButtons = 0xFFFF;                                   // 0 = pressed
std::uint8_t g_padAxes[4] = {0x7F, 0x7F, 0x7F, 0x7F};                  // right x, right y, left x, left y
std::uint64_t g_padOpens = 0;

void PadWrite(const PadPort& p, int index) {  // fill struct `index` of the pad area
  if (!p.address || !InRam(p.address, 0x100)) return;
  std::uint8_t* b = g_ram + Phys(p.address) + static_cast<std::size_t>(index) * (p.type == 0 ? 0x40 : 0x80);
  const std::size_t data = p.type == 2 ? 0 : 8;
  b[data + 0] = 0;
  b[data + 1] = 0x79;  // DualShock 2 in pressure mode: id 7, 9 halfwords (buttons, 4 axes, 12 pressures), as the real padman reports
  // libpad reads bytes 2-3 as a little-endian u16 in its own bit order (0x0008 START ... 0x4000 CROSS), the order of g_padButtons:
  // byte 2 = select, L3, R3, start, up, right, down, left; byte 3 = L2, R2, L1, R1, triangle, circle, cross, square
  b[data + 2] = static_cast<std::uint8_t>(g_padButtons);
  b[data + 3] = static_cast<std::uint8_t>(g_padButtons >> 8);
  for (int i = 0; i < 4; ++i) b[data + 4 + i] = g_padAxes[i];
  {  // pressure bytes (DualShock): right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2: 0xFF while pressed
    static const std::uint16_t order[12] = {0x20, 0x80, 0x10, 0x40, 0x1000, 0x2000, 0x4000, 0x8000, 0x400, 0x800, 0x100, 0x200};
    for (int i = 0; i < 12; ++i) b[data + 8 + i] = (g_padButtons & order[i]) ? 0x00 : 0xFF;
  }
  const std::size_t state = p.type == 2 ? 0x70 : 4;
  b[state] = 6;      // stable
  b[state + 1] = 0;  // no request pending
  b[state + 2] = 1;  // ok
}
void PadOpen(int port, std::uint32_t address) {
  if (port < 0 || port > 1 || !InRam(address, 0x100)) return;
  const std::uint8_t* a = g_ram + Phys(address);
  int type = 0;
  if (a[0x08] == 0xFF && a[0x88] == 0xFF) type = 1;
  if (a[0x00] == 0xFF && a[0x80] == 0xFF) type = 2;
  g_pad[port] = {address, type};
  ++g_padOpens;
  Log("io: pad port %d opened, data area %08X, layout %d", port, address, type);
  for (int i = 0; i < 2; ++i) {  // both structs start as stable, ok, length 32, sticks centred
    std::uint8_t* b = g_ram + Phys(address) + static_cast<std::size_t>(i) * (type == 0 ? 0x40 : 0x80);
    const std::size_t len = type == 2 ? 0x60 : 0x28;
    const std::uint32_t frame = static_cast<std::uint32_t>(i), n = 32;
    memcpy(b + (type == 2 ? 0x58 : 0), &frame, 4);
    memcpy(b + len, &n, 4);
    if (type == 2) {  // mode info as the real padman leaves it (reference RAM): current id 0x79, modes {4 digital, 7 DualShock}, current offset 1
      b[0x65] = 0x79; b[0x68] = 2; b[0x69] = 1;
      const std::uint16_t m[2] = {4, 7};
      memcpy(b + 0x50, m, 4);
    }
  }
  PadWrite(g_pad[port], 0);
  PadWrite(g_pad[port], 1);
}
void PadService(const std::uint32_t* args, std::uint32_t recv, std::uint32_t rsize) {  // args: the call's first eight send words
  std::uint32_t ret[8] = {};
  switch (args[0]) {
    case 0x00000001: case 0x80000100: PadOpen(static_cast<int>(args[1]), args[4]); ret[3] = 1; break;
    case 0x80000105: ret[3] = 1; break;                      // set main mode
    case 0x8000010D: if (args[1] < 2) g_pad[args[1]].address = 0; ret[3] = 1; break;  // close
    case 0x00000008: ret[5] = 1; break;                      // actuator align
    case 0x00000010: ret[3] = 1; break;                      // init
    case 0x00000012: ret[3] = 0x400; break;                  // module version
    default: LogOnce("padfn" + std::to_string(args[0]), "io: pad server: unknown method %08X", args[0]); break;
  }
  if (recv && rsize >= 32 && InRam(recv, 32)) memcpy(g_ram + Phys(recv), ret, 32);
}
// Keyboard and the first XInput controller, read at every vertical blank.
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
void PadPoll() {
  static XInputGetStateFn xinput = [] {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    return m ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
  }();
  std::uint16_t pressed = 0;  // 1 = pressed, in the PS2 bit order
  std::uint8_t axes[4] = {0x7F, 0x7F, 0x7F, 0x7F};
  auto key = [&](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
  if (gs::WindowFocused()) {
    if (key(VK_BACK) || key(VK_RSHIFT)) pressed |= 0x0001;  // select
    if (key(VK_RETURN) || key(VK_SPACE)) pressed |= 0x0008; // start
    if (key(VK_UP)) pressed |= 0x0010;
    if (key(VK_RIGHT)) { pressed |= 0x0020; axes[2] = 0xFF; }
    if (key(VK_DOWN)) pressed |= 0x0040;
    if (key(VK_LEFT)) { pressed |= 0x0080; axes[2] = 0x00; }
    if (key('1')) pressed |= 0x0100;  // L2
    if (key('2')) pressed |= 0x0200;  // R2
    if (key('Q')) pressed |= 0x0400;  // L1
    if (key('W')) pressed |= 0x0800;  // R1
    if (key('S')) pressed |= 0x1000;  // triangle
    if (key('X')) pressed |= 0x2000;  // circle
    if (key('Z')) pressed |= 0x4000;  // cross
    if (key('A')) pressed |= 0x8000;  // square
  }
  if (xinput) {
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st = {};
    if (xinput(0, &st) == 0) {
      struct PadMap { unsigned xi, ps; };
      static const PadMap map[] = {{0x0001, 0x0010}, {0x0008, 0x0020}, {0x0002, 0x0040}, {0x0004, 0x0080}, {0x0010, 0x0008},
                                                            {0x0020, 0x0001}, {0x0040, 0x0002}, {0x0080, 0x0004}, {0x0100, 0x0400}, {0x0200, 0x0800},
                                                            {0x1000, 0x4000}, {0x2000, 0x2000}, {0x4000, 0x8000}, {0x8000, 0x1000}};
      for (const PadMap& m : map) if (st.buttons & m.xi) pressed = static_cast<std::uint16_t>(pressed | m.ps);
      if (st.lt > 100) pressed |= 0x0100;
      if (st.rt > 100) pressed |= 0x0200;
      auto axis = [](SHORT v, bool invert) { int a = (v + 32768) >> 8; if (invert) a = 255 - a; return static_cast<std::uint8_t>(a < 0 ? 0 : a > 255 ? 255 : a); };
      if (std::abs(st.lx) > 7000) axes[2] = axis(st.lx, false);
      if (std::abs(st.ly) > 7000) axes[3] = axis(st.ly, true);
      if (std::abs(st.rx) > 7000) axes[0] = axis(st.rx, false);
      if (std::abs(st.ry) > 7000) axes[1] = axis(st.ry, true);
    }
  }
  {  // development: HP2_PRESS="start@10-11,cross@14-15" presses buttons at given seconds of the run (no keyboard needed);
     // with HP2_FAST_BOOT the seconds count from the arrival at the main menu
    static const std::string spec = [] { const char* e = getenv("HP2_PRESS"); return std::string(e ? e : ""); }();
    std::uint64_t scriptTime = 0;
    if (!spec.empty() && ScriptTime(scriptTime)) {
      struct NamedBit { const char* name; unsigned bit; };
      static const NamedBit names[] = {{"select", 1}, {"start", 8}, {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80}, {"l2", 0x100}, {"r2", 0x200},
                                                                    {"l1", 0x400}, {"r1", 0x800}, {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000}};
      const double now = static_cast<double>(scriptTime) / 59.94;
      for (std::size_t i = 0; i < spec.size();) {
        const std::size_t end = spec.find(',', i), stop = end == std::string::npos ? spec.size() : end;
        const std::string item = spec.substr(i, stop - i);
        const std::size_t at = item.find('@'), dash = item.find('-');
        if (at != std::string::npos && dash != std::string::npos) {
          const double from = atof(item.c_str() + at + 1), to = atof(item.c_str() + dash + 1);
          if (now >= from && now < to)
            for (const NamedBit& nb : names) if (item.compare(0, at, nb.name) == 0) pressed = static_cast<std::uint16_t>(pressed | nb.bit);
        }
        i = stop + 1;
      }
    }
    static const int dumpEvery = [] { const char* e = getenv("HP2_DUMP_EVERY"); return e ? atoi(e) : 0; }();
    if (dumpEvery > 0 && g_vblanks % static_cast<std::uint64_t>(dumpEvery * 60) == 0) gs::DumpFrame((".cache/native/frame_" + std::to_string(g_vblanks / 60) + ".bmp").c_str());
  }
  {  // tests: HP2_DUMP_AT="130,176.5" game frame at those game seconds (at_<s>.bmp); HP2_SHOT_AT: the window picture (shot_<s>.bmp)
    struct At { std::uint64_t vblank; std::string name; bool window; };
    static const std::vector<At> at = [] {
      std::vector<At> list;
      for (const char* var : {"HP2_DUMP_AT", "HP2_SHOT_AT"}) {
        const char* e = getenv(var);
        for (std::string item; e && *e; ++e) {
          if (*e != ',') item += *e;
          if (*e == ',' || !e[1]) {
            if (!item.empty()) list.push_back({static_cast<std::uint64_t>(atof(item.c_str()) * 60000.0 / 1001.0),
                                               std::string(var[4] == 'D' ? ".cache/native/at_" : ".cache/native/shot_") + item + ".bmp", var[4] == 'S'});
            item.clear();
          }
        }
      }
      return list;
    }();
    std::uint64_t now = 0;
    static const std::uint64_t primLogAt = [] {  // HP2_PRIMLOG_AT=<s>: every large primitive of 2 refreshes -> primlog.txt
      const char* e = getenv("HP2_PRIMLOG_AT");
      return e ? static_cast<std::uint64_t>(atof(e) * 60000.0 / 1001.0) : 0ull;
    }();
    if (primLogAt && ScriptTime(now)) {
      if (now == primLogAt) gs::SetPrimLog(true);
      if (now == primLogAt + 2) {
        gs::SetPrimLog(false);
        gs::DumpPage(0, 8, 512, 448, 0, ".cache/native/at_page0.bmp");  // the race's two frame buffers as they are now
        gs::DumpPage(128, 8, 512, 448, 0, ".cache/native/at_page128.bmp");
        if (FILE* f = nullptr; fopen_s(&f, ".cache/native/primlog.txt", "w") == 0 && f) {
          const std::string text = gs::PrimLogText();
          fwrite(text.data(), 1, text.size(), f);
          fclose(f);
        }
      }
    }
    if (ScriptTime(now))
      for (const At& a : at)
        if (a.vblank == now) {
        if (a.window) gs::RequestWindowShot(a.name.c_str());
        else gs::DumpFrame(a.name.c_str());
      }
  }
  if (g_vblanks % 600 == 300) {  // menu text: the console-only "Calibrate Display Offset" item opens the graphics settings
    static const struct { const char* from; const char* to; } kText[] = {
        {"Calibrate Display Offset", "Graphics Settings"},
        {"Adjust the picture's alignment", "Display and picture settings."},
    };
    for (const auto& t : kText) {
      const std::size_t n = strlen(t.from);
      std::uint8_t* const end = g_ram + kNativeRamSize - n - 128;
      for (std::uint8_t* p = g_ram; p < end; ++p) {
        p = static_cast<std::uint8_t*>(memchr(p, t.from[0], static_cast<std::size_t>(end - p)));
        if (!p) break;
        if (memcmp(p, t.from, n) != 0) continue;
        std::size_t len = n;  // the whole original string, up to its terminator
        while (len < 128 && p[len]) ++len;
        memset(p, 0, len);
        memcpy(p, t.to, std::min(strlen(t.to), len));
        LogOnce(t.from, "menu text at %08X: \"%s\" -> \"%s\"", static_cast<unsigned>(p - g_ram), t.from, t.to);
      }
    }
  }
  if (g_vblanks % 600 == 0) {  // speed: refreshes per real second over the last 600 (60 = full speed)
    static LARGE_INTEGER last = {};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    static gs::Stats prev;  // where the time went over these 600 refreshes (ms per refresh)
    static std::uint64_t prevIdle = 0, prevCode = 0, prevVu1 = 0;
    const gs::Stats st = gs::GetStats();
    const double ms = 1000.0 / static_cast<double>(g_qpf.QuadPart) / 600.0;
    if (last.QuadPart)
      Log("perf: vblank %llu, %.1f refreshes/s; per refresh: total %.1f ms, GS set-up %.1f ms (textures %.1f ms, %llu decodes), present %.1f ms, transfers %.1f ms, waiting for GS workers %.1f ms, %llu tris %llu sprites",
          static_cast<unsigned long long>(g_vblanks), 600.0 * static_cast<double>(g_qpf.QuadPart) / static_cast<double>(now.QuadPart - last.QuadPart),
          static_cast<double>(now.QuadPart - last.QuadPart) * ms, static_cast<double>(st.drawTicks - prev.drawTicks) * ms,
          static_cast<double>(st.textureTicks - prev.textureTicks) * ms, static_cast<unsigned long long>(st.textureDecodes - prev.textureDecodes),
          static_cast<double>(st.presentTicks - prev.presentTicks) * ms, static_cast<double>(st.transferTicks - prev.transferTicks) * ms,
          static_cast<double>(st.waitTicks - prev.waitTicks) * ms, static_cast<unsigned long long>((st.triangles - prev.triangles) / 600), static_cast<unsigned long long>((st.sprites - prev.sprites) / 600));
    prev = st;
    const double seconds = last.QuadPart ? static_cast<double>(now.QuadPart - last.QuadPart) / static_cast<double>(g_qpf.QuadPart) : 0.0;
    last = now;
    std::string threads;  // what each game thread is doing (status 1 run, 2 ready, 4 wait, 8 suspended, 16 dormant)
    for (const Thread* t : g_threads)
      if (t && t->used) {
        char one[96];
        snprintf(one, sizeof one, " t%d:%d@%08X%s", t->id, t->status, t->s.pc, t->wait_sema >= 0 && t->status == kWait ? (" s" + std::to_string(t->wait_sema)).c_str() : (t->sleeping ? " sleep" : ""));
        threads += one;
      }
    {
      static std::uint64_t prevFlips = 0;
      static std::uint64_t prevStale = 0, prevSame = 0;
      Log("perf:   game frames: %.1f per second (real time); textures with changed pages %llu, of those unchanged bytes %llu",
          seconds > 0 ? static_cast<double>(st.flips - prevFlips) / seconds : 0.0, static_cast<unsigned long long>(st.textureStale - prevStale),
          static_cast<unsigned long long>(st.textureSame - prevSame));
      {  // why the GS thread waited for its workers (ms per refresh, waits per refresh)
        static gs::Stats prevWait;
        char why[256] = {};
        static const char* const kNames[5] = {"texture", "upload", "palette", "display", "full"};
        for (int r = 0; r < 5; ++r)
          snprintf(why + strlen(why), sizeof why - strlen(why), " %s %.2f ms (%.1f)", kNames[r],
                   static_cast<double>(st.waitByReason[r] - prevWait.waitByReason[r]) * ms,
                   static_cast<double>(st.waitsByReason[r] - prevWait.waitsByReason[r]) / 600.0);
        Log("perf:   GS waits per refresh:%s; shaded %.0f, written %.0f thousand pixels per refresh; direct copies %.1f", why,
            static_cast<double>(st.shaded - prevWait.shaded) / 600e3, static_cast<double>(st.pixels - prevWait.pixels) / 600e3,
            static_cast<double>(st.directTextures - prevWait.directTextures) / 600.0);
        prevWait = st;
      }
      prevFlips = st.flips;
      prevStale = st.textureStale;
      prevSame = st.textureSame;
    }
    Log("perf:   threads%s; idle %.1f ms, in game code %.1f ms, in VU1 %.1f ms per refresh (both with the GS work they start)", threads.c_str(),
        static_cast<double>(g_idleTicks - prevIdle) * ms, static_cast<double>(g_codeTicks - prevCode) * ms, static_cast<double>(g_vu1Ticks - prevVu1) * ms);
    prevIdle = g_idleTicks;
    prevCode = g_codeTicks;
    prevVu1 = g_vu1Ticks;
    Log("perf:   targets%s", gs::TargetsText().c_str());
    {  // GIF vertices vs vertices that draw (XYZ2/XYZF2), XGKICKs, VU1 runs per refresh
      static std::uint64_t pv = 0, pk = 0, px = 0, pr = 0;
      Log("perf:   gpu: %s", gpu::StatsText().c_str());
      Log("perf:   per refresh: %llu GIF vertices, %llu drawing kicks, %llu XGKICKs, %llu VU1 runs", static_cast<unsigned long long>((g_gsVertices - pv) / 600),
          static_cast<unsigned long long>((g_gsKicks - pk) / 600), static_cast<unsigned long long>((g_xgkicks - px) / 600),
          static_cast<unsigned long long>((g_vuRuns - pr) / 600));
      pv = g_gsVertices; pk = g_gsKicks; px = g_xgkicks; pr = g_vuRuns;
    }
  }
  {  // tests (HP2_FAST_BOOT=1): Start pulses skip the intro movie and accept the title; the main menu is up at ~50 s of game
    if (g_fastBoot && !g_menuMusic) {  // time instead of ~150 s: pulses every 4 s from 8 s until the menus' music starts
      const double t = static_cast<double>(g_vblanks) * 1001.0 / 60000.0;
      if (t >= 8.0 && t - 4.0 * std::floor(t / 4.0) < 0.3) pressed |= 0x0008;
    }
    if (g_fastBoot && g_menuMusic && !g_timeBase) {
      g_timeBase = g_vblanks;
      Log("fast boot: main menu reached at game second %.1f; scripted times count from here", static_cast<double>(g_vblanks) * 1001.0 / 60000.0);
    }
  }
  gs::OverlayPad(static_cast<std::uint16_t>(pressed));  // settings overlay (Select+Start opens it); the game gets no input while it is open
  {
    static bool wasVisible = false, swallow = false, pendingConfirm = false;
    static int confirm = 0;  // vblanks of Cross still to send to the calibration screen
    const bool visible = gs::OverlayVisible();
    if (wasVisible && !visible) {
      swallow = true;  // the button that closed the settings must not reach the game
      if (g_calibrateScreen) pendingConfirm = true;
    }
    wasVisible = visible;
    if (visible) pressed = 0;
    if (swallow) {
      if (pressed == 0) swallow = false;
      pressed = 0;
    }
    if (!swallow && pendingConfirm) {  // then a fresh Cross: the calibration screen closes and the game returns to Display
      pendingConfirm = false;
      confirm = 12;
    }
    if (confirm > 0) pressed = (--confirm < 6) ? 0x4000 : 0;
  }
  if (static_cast<std::uint16_t>(~pressed) != g_padButtons) {  // log each change of the button mask (0 = pressed)
    static int logged = 0;
    if (logged++ < 60) Log("pad: vblank %llu buttons %04X -> %04X (ports opened: %llu)", static_cast<unsigned long long>(g_vblanks), g_padButtons, static_cast<std::uint16_t>(~pressed), static_cast<unsigned long long>(g_padOpens));
  }
  if (pressed && g_vblanks % 10 == 0) {  // diagnostic while a button is held: newest ring entry of port 0 and the player's device state
    static int logged = 0;
    const std::uint32_t w = Rd<std::uint32_t>(0x0032FBF0), e = 0x002DE640u + ((w - 1) & 31) * 84;  // gp-23808 = ring write index
    const std::uint32_t replay = Rd<std::uint32_t>(0x00330148);                                      // gp-22440: replay object (pads skipped if +32 != 0)
    if (logged++ < 40)
      Log("pad: vblank %llu ring w %u r %u entry %02X %02X %02X %02X +36 %d; player type %X/%X n %d listeners %08X; replay %08X(+32 %u)",
          static_cast<unsigned long long>(g_vblanks), w, Rd<std::uint32_t>(0x0032FBF4), Rd<std::uint8_t>(e), Rd<std::uint8_t>(e + 1), Rd<std::uint8_t>(e + 2),
          Rd<std::uint8_t>(e + 3), static_cast<int>(Rd<std::uint32_t>(e + 36)), Rd<std::uint32_t>(0x002F95EC), Rd<std::uint32_t>(0x002F95F0),
          static_cast<int>(Rd<std::uint32_t>(0x002F95F4)), Rd<std::uint32_t>(0x002F9530), replay, replay && InRam(replay, 64) ? Rd<std::uint32_t>(replay + 32) : 0u);
  }
  if (g_vblanks % 1800 == 0 && g_pad[0].address && InRam(g_pad[0].address, 0x100)) {  // diagnostic: does the game touch the pad area?
    const std::uint8_t* b = g_ram + Phys(g_pad[0].address);
    char hex[3 * 0x100 + 1] = {};
    for (int i = 0; i < 0x100; ++i) snprintf(hex + i * 3, 4, "%02X ", b[i]);
    Log("pad: vblank %llu game pad-setup states (gp-23776): port0 %d port1 %d", static_cast<unsigned long long>(g_vblanks), static_cast<int>(Rd<std::uint32_t>(0x0032FC10)), static_cast<int>(Rd<std::uint32_t>(0x0032FC14)));
    Log("pad: vblank %llu area of port 0: %s", static_cast<unsigned long long>(g_vblanks), hex);
  }
  g_padButtons = static_cast<std::uint16_t>(~pressed);
  memcpy(g_padAxes, axes, 4);
  {  // like the real pad library: every vblank the I/O side fills the other of the two structs and gives it a newer frame number
    static std::uint32_t frame = 2;
    ++frame;
    for (int port = 0; port < 2; ++port) {
      const PadPort& p = g_pad[port];
      if (!p.address || !InRam(p.address, 0x100)) continue;
      const int idx = static_cast<int>(frame & 1);
      PadWrite(p, idx);
      std::uint8_t* b = g_ram + Phys(p.address) + static_cast<std::size_t>(idx) * (p.type == 0 ? 0x40 : 0x80) + (p.type == 2 ? 0x58 : 0);
      memcpy(b, &frame, 4);
    }
  }
}

std::map<std::uint32_t, std::uint64_t> g_entryCount;  // entry place -> times entered (what is the game doing?)
std::map<std::uint32_t, std::uint32_t> g_rpcServer;  // client structure -> server number
std::map<std::uint64_t, std::uint64_t> g_rpcCount;   // (server, function) -> calls

void Hook(NativeState& s) {
  const std::uint32_t pc = s.pc;
  if (pc == kHookCalibrateScreen) {  // (event, item): the game's results, with the graphics settings instead of centring
    // Events (measured): opening 11 (item), 2, 4; every frame 18 and 19 (19 draws the instructions box: 0021E560 text,
    // 001451E0 box); 6-9 the directional buttons (offsets), 1 Cross and 14 cancel (item+12 = 5 closes the screen), 17 kind.
    const std::uint32_t ev = A(s, 0), item = A(s, 1), gp = static_cast<std::uint32_t>(s.gpr[28][0]);
    Wr<std::uint32_t>(item + 12, 0);
    Ret(s, 0);
    switch (ev) {
      case 11:  // the screen opens: the graphics settings instead of the centring
        Wr<std::uint32_t>(gp - 13356, Rd<std::uint32_t>(item));
        if (!g_calibrateScreen) {
          g_calibrateScreen = true;
          gs::OpenSettings();
          Log("menu: Options > Display > Graphics Settings opened (vblank %llu)", static_cast<unsigned long long>(g_vblanks));
        }
        break;
      case 1:  // Cross (sent by the runtime when the settings close): the game's "back" (00170320(1)), as a tail call
        Wr<std::uint32_t>(item + 12, 5);
        g_calibrateScreen = false;
        Log("menu: Graphics Settings closed, back to Display (vblank %llu)", static_cast<unsigned long long>(g_vblanks));
        s.gpr[4][0] = 1;
        s.pc = 0x00170320;  // returns straight to the caller of the handler
        return;
      case 14:  // cancel (the frontend goes back by itself)
        Wr<std::uint32_t>(item + 12, 5);
        g_calibrateScreen = false;
        break;
      case 17: Wr<std::uint32_t>(item + 12, 6); break;
      default: break;  // 19: no instructions box; 6-9: the display offsets stay as they are
    }
    s.pc = static_cast<std::uint32_t>(s.gpr[31][0]);
    return;
  }
  if (pc == kHookVu1Start) {  // the EE starts VU1 at a0 (CMSAR1): run it now, like an MSCAL
    static std::uint64_t calls = 0;
    if (++calls <= 5 || calls % 10000 == 0)
      Log("vu1: started by the EE at %04X (call %llu, vblank %llu)", (A(s, 0) & 0xFFFF) * 8, static_cast<unsigned long long>(calls),
          static_cast<unsigned long long>(g_vblanks));
    RunVu1((A(s, 0) & 0xFFFF) * 8);
    s.pc = static_cast<std::uint32_t>(s.gpr[31][0]);
    return;
  }
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
    std::uint32_t padArgs[8] = {};  // the pad server answers in the buffer it was called with: read the arguments first
    if (server == 0x80000100 && A(s, 3) && InRam(A(s, 3), 32)) memcpy(padArgs, g_ram + Phys(A(s, 3)), 32);
    std::vector<std::uint32_t> mcSend;  // memory card command, copied before the reply buffer is cleared
    if (server == 0x80000400 && fn != 254 && A(s, 3) && A(s, 4) >= 32 && InRam(A(s, 3), A(s, 4))) {
      mcSend.resize((std::min<std::uint32_t>(A(s, 4), 0x1000) + 3) / 4);
      memcpy(mcSend.data(), g_ram + Phys(A(s, 3)), std::min<std::uint32_t>(A(s, 4), 0x1000));
    }
    if (recv && rsize && InRam(recv, rsize)) memset(g_ram + Phys(recv), 0, rsize);
    if (!mcSend.empty() && recv && rsize >= 4 && InRam(recv, rsize)) {
      const bool served = mc::Service(fn, mcSend.data(), static_cast<std::uint32_t>(mcSend.size() * 4), reinterpret_cast<std::uint32_t*>(g_ram + Phys(recv)),
                                      rsize, g_ram, kNativeRamSize);
      static int logged = 0;
      if (logged++ < 200) {
        const std::string name = mcSend.size() > 5 ? GuestString(A(s, 3) + 20, 64) : std::string();
        Log("mc: fn %02X %s -> %d%s%s", fn, served ? "served" : "UNKNOWN", static_cast<int>(Rd<std::uint32_t>(recv)), name.empty() ? "" : " ", name.c_str());
        if (fn == 1 && mcSend.size() >= 8)
          Log("mc:   GetInfo want f/free/type %u %u %u buffer %08X: type %u free %u formatted %u", mcSend[3], mcSend[4], mcSend[5], mcSend[7],
              Rd<std::uint32_t>(mcSend[7]), Rd<std::uint32_t>(mcSend[7] + 4), Rd<std::uint32_t>(mcSend[7] + 0x90));
      }
    }
    if (server == 0x80000100 && fn == 1) PadService(padArgs, recv, rsize);
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
        else {
          ++g_discReads;
          if (count) NoteDiscRead(lsn, g_ram + Phys(address));
        }
      }
    }
    if (server == 0x80000595 && fn == 1 && A(s, 4) >= 12 && InRam(A(s, 3), 12)) {  // sceCdRead(lsn, sectors, buffer, ...)
      const std::uint32_t lsn = Rd<std::uint32_t>(A(s, 3)), sectors = Rd<std::uint32_t>(A(s, 3) + 4), buf = Rd<std::uint32_t>(A(s, 3) + 8);
      if (InRam(buf, sectors * 2048) && DiscRead(lsn * 2048ull, g_ram + Phys(buf), sectors * 2048ull)) {
        ++g_discReads;
        if (sectors) NoteDiscRead(lsn, g_ram + Phys(buf));
      } else
        Log("io: sceCdRead(lsn %u, %u sectors, %08X) failed", lsn, sectors, buf);
    }
    if (server == 0x00012345 && fn == 0 && recv && rsize >= 0x120 && InRam(recv, rsize) && InRam(A(s, 3), 4096)) StreamerService(A(s, 3), recv);
    if (const std::uint32_t endfn = A(s, 7)) {  // asynchronous call: the I/O processor's answer runs the end function
      const std::uint32_t sp = static_cast<std::uint32_t>(s.gpr[29][0]);
      std::uint32_t endarg = 0;  // ninth argument: first stack word (the stack may be in the scratchpad)
      if ((sp >> 28) == 7) memcpy(&endarg, g_spr + (sp & 0x3FFC), 4);
      else if (InRam(sp, 4)) endarg = Rd<std::uint32_t>(sp);
      if (server == 0x80000400 && fn == 1) LogOnce("mcsp", "mc:   RPC caller stack %08X", sp);
      NativeState tmp = s;
      tmp.gpr[29][0] -= 0x80;
      CallGuest(tmp, endfn, endarg, 0);
      Vu0TakeOver(s);  // (tmp is about to go away: the caller keeps what the end function left in VU0)
      if (server == 0x80000400 && fn == 1) {
        static int logged = 0;
        if (logged++ < 6)
          Log("mc:   end function %08X(arg %08X): game variables type %u free %u format %u", endfn, endarg, Rd<std::uint32_t>(Rd<std::uint32_t>(0x0033C0E8)),
              Rd<std::uint32_t>(Rd<std::uint32_t>(0x0033C0EC)), Rd<std::uint32_t>(Rd<std::uint32_t>(0x0033C0F0)));
      }
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
  // hooks first: a hooked function may still be in the package as an entry inside a neighbouring function
  for (std::uint32_t hook : kNativeHooks)
    if (pc == hook) {
      Hook(s);
      return;
    }
  if (void* code = FindCode(pc)) {
    s.cycle_quota = kSliceQuota;
    g_lastEntry = pc;
    if (g_profile) ++g_entryCount[pc];
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    reinterpret_cast<EeCode>(code)(reinterpret_cast<std::uint8_t*>(&s), g_ram, g_spr, &g_runtime);
    QueryPerformanceCounter(&t1);
    g_codeTicks += static_cast<std::uint64_t>(t1.QuadPart - t0.QuadPart);  // (GS and VU1 work started by the code included)
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
      else if (a == L"--until" && i + 1 < argc) g_untilVblank = static_cast<std::uint64_t>(_wtof(argv[++i]) * 60000.0 / 1001.0);
      else if (a == L"--verbose") g_verbose = true;
      else {
        printf("HotPursuit2Native [--rom <folder>] [--code <hp2code.dll>] [--seconds <real s>] [--until <game s>]\n");
        return 2;
      }
    }
    // the log is named after the executable (a test copy does not overwrite the log of a game being played)
    {
      wchar_t self[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, self, MAX_PATH);
      const std::wstring stem = std::filesystem::path(self).stem().wstring();
      g_log = _wfopen((exe_dir / (stem == L"HotPursuit2Native" ? std::wstring(L"native.log") : stem + L".log")).c_str(), L"w");
    }
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
      gs::SetInterruptCallback(&RaiseGsInterrupt);
      g_disc = hp2::VirtualDisc::Open(rom.wstring(), error);
      if (!g_disc) fail("cannot open the disc from the ROM folder: {}", error);
      Log("Disc: %s", g_disc->Description().c_str());
      {  // memory card: <folder of the executable>\saves\mc0
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::filesystem::path saves = std::filesystem::path(exe).parent_path() / L"saves";
        mc::SetRoot(saves);
        Log("Memory card: %s", (saves / L"mc0").string().c_str());
      }
      if (const char* t = std::getenv("HP2_TURBO")) {  // tests: faster than real time (silent)
        g_turboMax = std::string(t) == "max";
        g_turbo = g_turboMax ? 8.0 : std::max(1.0, atof(t));
        if (const char* u = std::getenv("HP2_TURBO_UNTIL"))  // game seconds, or "menu" (the main menu's arrival)
          g_turboUntil = std::string(u) == "menu" ? ~0ull : static_cast<std::uint64_t>(atof(u) * 60000.0 / 1001.0);
        Log("Turbo: refreshes x%.0f%s (no audio)", g_turbo, g_turboMax ? " and as soon as the game waits" : "");
      }
      if (!std::getenv("HP2_MUTE") && g_turbo == 1.0) Log("Audio: %s", audio::Init() ? "XAudio2 ready" : audio::LastError());
    }

    // ---- the translated code ----
    HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) fail("cannot load the code package '{}' (error {})", to_utf8(dll), GetLastError());
    g_table = reinterpret_cast<const Hp2CodeTable*>(GetProcAddress(module, kCodeTableSymbol));
    if (!g_table || g_table->magic != kCodeTableMagic || g_table->version != kCodeTableVersion)
      fail("'{}' is not a code package of this version", to_utf8(dll));
    if (g_table->layout_fingerprint != code_layout_fingerprint(native_code_layout()))
      fail("the code package was not built for the native runtime (hp2recomp package)");
    for (std::uint32_t hook : kNativeHooks)
      if (FindCode(hook)) Log("hook %08X also has translated code in the package (the runtime's version is used)", hook);    {
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
    if (std::getenv("HP2_SAMPLE")) StartSampler();
    if (std::getenv("HP2_GS_FILLTEST")) {  // development: GS fill rate, then exit (no game)
      gs::Reset();
      for (int round = 0; round < 2; ++round)  // (the first round warms up)
        Log("fill test%s: flat %.0f, bilinear %.0f, bilinear + blend %.0f million pixels/s", round ? "" : " (warm-up)",
            gs::FillTest(60, false, false), gs::FillTest(60, true, false), gs::FillTest(60, true, true));
      gs::Shutdown();
      if (g_log) fclose(g_log);
      return 0;
    }
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
        if (g_turboMax) g_nextVblank.QuadPart = 0;  // tests: nothing to run, the next refresh is due now
        else {                                     // all threads wait: until the next interrupt
          LARGE_INTEGER a, b;
          QueryPerformanceCounter(&a);
          Sleep(1);
          QueryPerformanceCounter(&b);
          g_idleTicks += static_cast<std::uint64_t>(b.QuadPart - a.QuadPart);
        }
      } else {
        rotate = static_cast<std::size_t>(best->id) + 1;
        g_current = best;
        best->status = kRun;
        Vu0TakeOver(best->s);
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
    ReportSamples();
    if (std::getenv("HP2_LOG_WAITS")) Log("%s", gs::WaitNotes().c_str());
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
    Log("  VIF1: %llu commands continued across DMA transfers, %zu bytes still waiting", static_cast<unsigned long long>(g_vifCarried), g_vifCarry.size());
    Log("  VU1: %llu runs, %llu translated calls, %llu without translated microcode, %llu XGKICKs", static_cast<unsigned long long>(g_vuRuns),
        static_cast<unsigned long long>(g_vuCalls), static_cast<unsigned long long>(g_vuMissing), static_cast<unsigned long long>(g_xgkicks));
    gs::DumpFrame(".cache/native/frame.bmp");
    {
      const std::string trace = gs::CsrTraceText();
      Log("GS event registers, last activity (F FINISH written, W CSR write, R CSR read, M IMR):");
      for (std::size_t p = 0; p < trace.size();) {
        const std::size_t e = trace.find('\n', p);
        Log("%s", trace.substr(p, e - p).c_str());
        p = e + 1;
      }
    }
    audio::Shutdown();
    if (std::getenv("HP2_DUMP_RAM")) {  // diagnostic: EE RAM at the end of the run, same layout as the reference launcher's ee-ram.bin
      if (FILE* f = nullptr; fopen_s(&f, ".cache/native/ee-ram.bin", "wb") == 0 && f) { fwrite(g_ram, 1, kNativeRamSize, f); fclose(f); }
    }
    { const gs::Stats& g = gs::GetStats(); Log("  GS: %llu frames shown, %llu triangles, %llu sprites, %llu pixels (%llu textured), %llu transfers (%llu bytes), %llu unsupported", static_cast<unsigned long long>(g.frames), static_cast<unsigned long long>(g.triangles), static_cast<unsigned long long>(g.sprites), static_cast<unsigned long long>(g.pixels), static_cast<unsigned long long>(g.textured), static_cast<unsigned long long>(g.transfers), static_cast<unsigned long long>(g.transferBytes), static_cast<unsigned long long>(g.unsupported)); }
    Log("  GIF: %llu tags, %llu image bytes, %llu PRIM writes, %llu vertices (%llu kicks)", static_cast<unsigned long long>(g_gifTags),
        static_cast<unsigned long long>(g_gifImageBytes), static_cast<unsigned long long>(g_gsPrims),
        static_cast<unsigned long long>(g_gsVertices), static_cast<unsigned long long>(g_gsKicks));
    for (std::uint32_t r = 0; r < 0x80; ++r)
      if (g_gsRegCount[r]) Log("    GS reg %02X: %llu writes, last %016llX", r, static_cast<unsigned long long>(g_gsRegCount[r]),
                               static_cast<unsigned long long>(g_gsReg[r]));
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
    gs::Shutdown();
    if (g_log) fclose(g_log);
    return 0;
  } catch (const std::exception& e) {
    Log("error: %s", e.what());
    gs::Shutdown();
    return 3;
  }
}
