#include "gs.h"

#include "overlay.h"
#include "present.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gs {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

constexpr u32 kRamSize = 4u << 20;
std::vector<u8> g_mem;
Stats g_stats;
struct Ticks {  // adds the time of a scope to a counter (QPC ticks)
  u64& total;
  LARGE_INTEGER start;
  explicit Ticks(u64& t) : total(t) { QueryPerformanceCounter(&start); }
  ~Ticks() {
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    total += static_cast<u64>(end.QuadPart - start.QuadPart);
  }
  Ticks(const Ticks&) = delete;
  Ticks& operator=(const Ticks&) = delete;
};

u64 g_pageVersion[512] = {};  // bumped on every write into an 8 KB page of local memory (texture cache)
// ---- pixel storage formats -------------------------------------------------------------------
enum Psm : u32 {
  CT32 = 0x00, CT24 = 0x01, CT16 = 0x02, CT16S = 0x0A, T8 = 0x13, T4 = 0x14, T8H = 0x1B, T4HL = 0x24, T4HH = 0x2C,
  Z32 = 0x30, Z24 = 0x31, Z16 = 0x32, Z16S = 0x3A
};

const int kBlk32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
const int kBlkZ32[4][8] = {{24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15}, {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
const int kCol32[2][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}};
const int kBlk16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
const int kBlk16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
const int kBlkZ16[8][4] = {{24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23}, {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
const int kBlkZ16S[8][4] = {{24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3}, {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};
const int kCol16[2][16] = {{0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27}, {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31}};
const int kBlk4[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
const int kColWord[2][2][8] = {{{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}}, {{8, 9, 12, 13, 0, 1, 4, 5}, {10, 11, 14, 15, 2, 3, 6, 7}}};

enum class Kind { C32, C16, C16S, Z32, Z16, Z16S, I8, I4 };
Kind KindOf(u32 psm) {
  switch (psm) {
    case CT16: return Kind::C16;
    case CT16S: return Kind::C16S;
    case Z32: case Z24: return Kind::Z32;
    case Z16: return Kind::Z16;
    case Z16S: return Kind::Z16S;
    case T8: case T8H: return Kind::I8;
    case T4: case T4HL: case T4HH: return Kind::I4;
    default: return Kind::C32;
  }
}

struct PageGeometry { u32 w, h; };
PageGeometry PageOf(Kind k) {
  switch (k) {
    case Kind::C32: case Kind::Z32: return {64, 32};
    case Kind::I8: return {128, 64};
    case Kind::I4: return {128, 128};
    default: return {64, 64};
  }
}

// Offset of a pixel inside its 8 KB page, in bytes (for 4-bit pixels: in nibbles). Built once per format.
// (All eight tables are built together on the first use, once: several threads read them while drawing.)
std::vector<u32> g_pageOffset[8];
void BuildOffsetsOf(Kind k);
void BuildOffsets(Kind) {
  static std::once_flag once;
  std::call_once(once, [] {
    for (int i = 0; i < 8; ++i) BuildOffsetsOf(static_cast<Kind>(i));
  });
}
void BuildOffsetsOf(Kind k) {
  const PageGeometry pg = PageOf(k);
  std::vector<u32>& t = g_pageOffset[static_cast<int>(k)];
  if (!t.empty()) return;
  t.resize(pg.w * pg.h);
  for (u32 y = 0; y < pg.h; ++y)
    for (u32 x = 0; x < pg.w; ++x) {
      u32 off = 0;
      switch (k) {
        case Kind::C32: case Kind::Z32: {
          const int blk = (k == Kind::C32 ? kBlk32 : kBlkZ32)[y / 8][x / 8];
          const u32 col = (y % 8) / 2;
          off = static_cast<u32>(blk) * 256 + col * 64 + 4 * static_cast<u32>(kCol32[y & 1][x & 7]);
          break;
        }
        case Kind::C16: case Kind::C16S: case Kind::Z16: case Kind::Z16S: {
          const int blk = (k == Kind::C16 ? kBlk16 : k == Kind::C16S ? kBlk16S : k == Kind::Z16 ? kBlkZ16 : kBlkZ16S)[y / 8][x / 16];
          const u32 col = (y % 8) / 2;
          off = static_cast<u32>(blk) * 256 + col * 64 + 2 * static_cast<u32>(kCol16[y & 1][x & 15]);
          break;
        }
        case Kind::I8: {
          const int blk = kBlk32[y / 16][x / 16];
          u32 wy = y % 16, wx = x % 16;
          const u32 col = wy / 4;
          wy %= 4;
          u32 table = (wy & 2) >> 1;
          u32 byte = ((wx & 8) >> 2) + ((wy & 2) >> 1);
          table ^= (y / 4) & 1;
          wx &= 7;
          wy &= 1;
          off = static_cast<u32>(blk) * 256 + col * 64 + static_cast<u32>(kColWord[table][wy][wx]) * 4 + byte;
          break;
        }
        case Kind::I4: {
          const int blk = kBlk4[y / 16][x / 32];
          u32 wy = y % 16, wx = x % 32;
          const u32 col = wy / 4;
          wy %= 4;
          const u32 shift = (wx & 0x18) + ((wy & 2) << 1);
          const u32 nibble = shift / 4;
          const u32 sub = ((wy & 2) >> 1) ^ (col & 1);
          wx &= 7;
          wy &= 1;
          off = (col * 64 + static_cast<u32>(blk) * 256 + static_cast<u32>(kColWord[sub][wy][wx]) * 4) * 2 + nibble;
          break;
        }
      }
      t[y * pg.w + x] = off;
    }
}

// Address of a pixel: base block pointer (units of 256 bytes), buffer width in 64-pixel units. Returns the byte offset
// into local memory (for 4-bit pixels in nibbles).
u32 PixelAddress(u32 psm, u32 bp, u32 bw, u32 x, u32 y) {
  const Kind k = KindOf(psm);
  BuildOffsets(k);
  const PageGeometry pg = PageOf(k);
  const u32 perRow = std::max<u32>(1, bw * 64 / pg.w);
  const u32 page = x / pg.w + (y / pg.h) * perRow;
  const u32 off = g_pageOffset[static_cast<int>(k)][(y % pg.h) * pg.w + (x % pg.w)];
  const u32 base = bp * 256 + page * 8192;
  if (k == Kind::I4) return (base * 2 + off) & (kRamSize * 2 - 1);
  return (base + off) & (kRamSize - 1);
}

u32 ReadPixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y) {  // raw stored value
  const u32 a = PixelAddress(psm, bp, bw, x, y);
  u32 v = 0;
  switch (KindOf(psm)) {
    case Kind::C32: case Kind::Z32:
      memcpy(&v, &g_mem[a & ~3u], 4);
      return psm == CT24 || psm == Z24 ? (v & 0xFFFFFF) : v;
    case Kind::C16: case Kind::C16S: case Kind::Z16: case Kind::Z16S: {
      u16 h;
      memcpy(&h, &g_mem[a & ~1u], 2);
      return h;
    }
    case Kind::I8: {
      v = g_mem[a];
      return v;
    }
    case Kind::I4: {
      const u8 b = g_mem[a >> 1];
      return (a & 1) ? (b >> 4) : (b & 15);
    }
  }
  return 0;
}

void WritePixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 value, u32 mask = 0xFFFFFFFFu) {
  const u32 a = PixelAddress(psm, bp, bw, x, y);
  ++g_pageVersion[((KindOf(psm) == Kind::I4 ? a >> 1 : a) >> 13) & 511];
  switch (KindOf(psm)) {
    case Kind::C32: case Kind::Z32: {
      u32 old;
      memcpy(&old, &g_mem[a & ~3u], 4);
      u32 m = psm == CT24 || psm == Z24 ? (mask & 0x00FFFFFFu) : mask;
      old = (old & ~m) | (value & m);
      memcpy(&g_mem[a & ~3u], &old, 4);
      break;
    }
    case Kind::C16: case Kind::C16S: case Kind::Z16: case Kind::Z16S: {
      u16 old;
      memcpy(&old, &g_mem[a & ~1u], 2);
      const u16 m = static_cast<u16>(mask);
      old = static_cast<u16>((old & ~m) | (value & m));
      memcpy(&g_mem[a & ~1u], &old, 2);
      break;
    }
    case Kind::I8: g_mem[a] = static_cast<u8>(value); break;
    case Kind::I4: {
      u8& b = g_mem[a >> 1];
      b = (a & 1) ? static_cast<u8>((b & 0x0F) | ((value & 15) << 4)) : static_cast<u8>((b & 0xF0) | (value & 15));
      break;
    }
  }
}

u32 BitsOf(u32 psm) {
  switch (KindOf(psm)) {
    case Kind::C32: case Kind::Z32: return psm == CT24 || psm == Z24 ? 24 : 32;
    case Kind::I8: return 8;
    case Kind::I4: return 4;
    default: return 16;
  }
}


// ---- fast surface access (frame and depth buffers) and page change tracking (texture cache) ----------------
struct Surf { Kind kind; u32 psm, base, perRow, pw, ph; const u32* table; };
Surf MakeSurf(u32 psm, u32 bp, u32 bw) {
  Surf s = {};
  s.kind = KindOf(psm);
  BuildOffsets(s.kind);
  const PageGeometry pg = PageOf(s.kind);
  s.psm = psm; s.base = bp * 256; s.pw = pg.w; s.ph = pg.h;
  s.perRow = std::max<u32>(1, bw * 64 / pg.w);
  s.table = g_pageOffset[static_cast<int>(s.kind)].data();
  return s;
}
inline u32 SurfAddr(const Surf& s, u32 x, u32 y) {
  return (s.base + (x / s.pw + (y / s.ph) * s.perRow) * 8192 + s.table[(y % s.ph) * s.pw + x % s.pw]) & (kRamSize - 1);
}
inline u32 SurfRead(const Surf& s, u32 x, u32 y) {
  const u32 a = SurfAddr(s, x, y);
  if (s.kind == Kind::C32 || s.kind == Kind::Z32) {
    u32 v;
    memcpy(&v, &g_mem[a & ~3u], 4);
    return (s.psm == CT24 || s.psm == Z24) ? (v & 0xFFFFFF) : v;
  }
  u16 h;
  memcpy(&h, &g_mem[a & ~1u], 2);
  return h;
}
// Primitive drawing writes through SurfWrite from several threads (rows split between them): the page versions of the
// texture cache are bumped once per primitive over its rectangle (BumpRect) instead of per pixel. Writes are whole 16/32-bit
// units, so pixels of different rows never share a word.
inline void SurfWrite(const Surf& s, u32 x, u32 y, u32 value, u32 mask) {
  const u32 a = SurfAddr(s, x, y);
  if (s.kind == Kind::C32 || s.kind == Kind::Z32) {
    if (s.psm == CT24 || s.psm == Z24) mask &= 0x00FFFFFFu;
    u32 old;
    memcpy(&old, &g_mem[a & ~3u], 4);
    old = (old & ~mask) | (value & mask);
    memcpy(&g_mem[a & ~3u], &old, 4);
  } else {
    u16 old;
    memcpy(&old, &g_mem[a & ~1u], 2);
    old = static_cast<u16>((old & ~mask) | (value & mask));
    memcpy(&g_mem[a & ~1u], &old, 2);
  }
}
void BumpRect(const Surf& s, int x0, int x1, int y0, int y1) {  // every page of `s` that the rectangle touches
  if (x1 < x0 || y1 < y0) return;
  for (u32 py = static_cast<u32>(y0) / s.ph; py <= static_cast<u32>(y1) / s.ph; ++py)
    for (u32 px = static_cast<u32>(x0) / s.pw; px <= static_cast<u32>(x1) / s.pw; ++px)
      ++g_pageVersion[((s.base + (px + py * s.perRow) * 8192) & (kRamSize - 1)) >> 13];
}

// ---- row-parallel rasterisation ------------------------------------------------------------------------
// Large primitives are split into blocks of rows shared by a pool of worker threads (one per extra core, up to 15)
// and the calling thread. Small ones run on the calling thread: the hand-off costs more than they do.
class RowPool {
 public:
  RowPool() {
    const unsigned n = std::thread::hardware_concurrency();
    const char* env = std::getenv("HP2_GS_THREADS");
    const unsigned want = env ? static_cast<unsigned>(std::atoi(env)) : (n > 1 ? n - 1 : 0);
    for (unsigned i = 0; i < std::min(want, 15u); ++i) m_workers.emplace_back([this] { Worker(); });
  }
  ~RowPool() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_quit = true;
      ++m_generation;
    }
    m_wake.notify_all();
    for (auto& t : m_workers) t.join();
  }
  // Calls fn(y) for every row in [y0, y1]; returns once all rows are done.
  template <class Fn>
  void Run(int y0, int y1, Fn&& fn) {
    if (m_workers.empty()) {
      for (int y = y0; y <= y1; ++y) fn(y);
      return;
    }
    std::function<void(int)> f = std::forward<Fn>(fn);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_fn = &f;
      m_next = y0;
      m_end = y1;
      m_busy = static_cast<int>(m_workers.size());
      ++m_generation;
    }
    m_wake.notify_all();
    Take();
    std::unique_lock<std::mutex> lock(m_mutex);
    m_done.wait(lock, [this] { return m_busy == 0; });
    m_fn = nullptr;
  }

 private:
  static constexpr int kBlock = 4;  // rows per grab
  void Take() {
    for (;;) {
      const int y = m_next.fetch_add(kBlock);
      if (y > m_end) return;
      for (int r = y; r < y + kBlock && r <= m_end; ++r) (*m_fn)(r);
    }
  }
  void Worker() {
    u64 seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait(lock, [&] { return m_generation != seen; });
        seen = m_generation;
        if (m_quit) return;
      }
      Take();
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (--m_busy == 0) m_done.notify_one();
      }
    }
  }
  std::vector<std::thread> m_workers;
  std::mutex m_mutex;
  std::condition_variable m_wake, m_done;
  const std::function<void(int)>* m_fn = nullptr;
  std::atomic<int> m_next{0};
  int m_end = 0, m_busy = 0;
  u64 m_generation = 0;
  bool m_quit = false;
};
RowPool& Pool() {
  static RowPool pool;
  return pool;
}
constexpr i64 kParallelArea = 4096;  // bounding-box pixels from which a primitive is split between threads
std::atomic<u64> g_pixelCount{0}, g_texturedCount{0}, g_unsupportedTexels{0};
std::atomic<u64> g_targetPixels[512] = {};  // diagnostic: pixels written per frame-buffer base page (FBP / 32)

// ---- register state -----------------------------------------------------------------------------
struct Vertex { i32 x, y; u32 z; float r, g, b, a, s, t, q, u, v; };
struct Ctx { u64 tex0 = 0, tex1 = 0, clamp = 0, xyoffset = 0, scissor = 0, alpha = 0, test = 0, frame = 0, zbuf = 0; };

u64 g_prim = 0;
Ctx g_ctx[2];
u64 g_rgbaq = 0, g_texa = 0, g_fogcol = 0, g_colclamp = 1, g_pabe = 0;
float g_q = 1.0f;
u64 g_st = 0, g_uv = 0, g_fba[2] = {0, 0};
u64 g_bitbltbuf = 0, g_trxpos = 0, g_trxreg = 0, g_trxdir = 0;
Vertex g_queue[3];
int g_queued = 0;

// image transfer state
struct Transfer { bool active = false; u32 psm = 0, bp = 0, bw = 0, x0 = 0, y0 = 0, w = 0, h = 0, pos = 0; u32 pending = 0, pendingBits = 0; } g_xfer;

// privileged registers
u64 g_csr = 0, g_imr = 0xFF00, g_siglblid = 0;
void CsrTrace(char kind, u64 value);  // diagnostic ring of event-register activity (defined with PrivRead)  // GS_CSR event bits (0 SIGNAL, 1 FINISH, 2 HSINT, 3 VSINT, 4 EDWINT), GS_IMR, GS_SIGLBLID
void (*g_irqCallback)() = nullptr;
u64 g_pmode = 0, g_dispfb[2] = {0, 0}, g_display[2] = {0, 0}, g_smode2 = 0;

// ---- window ------------------------------------------------------------------------------------
std::mutex g_frameMutex;
std::vector<u32> g_frame;  // BGRA
int g_frameW = 640, g_frameH = 448;
std::atomic<bool> g_closed{false};
std::atomic<int> g_scalePercent{100};
HWND g_hwnd = nullptr;
std::thread g_windowThread;
std::uint64_t g_frameSeq = 0;                                       // bumped by Present (under g_frameMutex)
std::string g_windowShot;                                           // development: next window picture to a file (under g_frameMutex)
HANDLE g_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // wakes the window thread for a new frame
std::atomic<bool> g_d3dActive{false};                               // the D3D11 presenter draws (else GDI WM_PAINT)
constexpr UINT kPadMessage = WM_APP + 1;                            // wParam: newly pressed pad buttons, bit 16 = menu toggle
// window thread only
ui::Settings g_settings;
bool g_overlayDirty = true, g_redraw = true;
std::vector<u32> g_overlayPixels;
int g_overlayW = 0, g_overlayH = 0;
RECT g_windowedRect = {};

void ApplyDisplayMode(HWND h) {  // borderless full screen on the window's monitor, or back to the remembered window
  const LONG style = GetWindowLongW(h, GWL_STYLE);
  if (g_settings.fullscreen && (style & WS_OVERLAPPEDWINDOW)) {
    GetWindowRect(h, &g_windowedRect);
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  } else if (!g_settings.fullscreen && !(style & WS_OVERLAPPEDWINDOW)) {
    SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    SetWindowPos(h, nullptr, g_windowedRect.left, g_windowedRect.top, g_windowedRect.right - g_windowedRect.left,
                 g_windowedRect.bottom - g_windowedRect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
  }
}
void SettingsChanged(HWND h) {
  ui::Save(g_settings);
  ApplyDisplayMode(h);
  g_overlayDirty = g_redraw = true;
}
void MenuInput(HWND h, ui::Input in) {
  if (ui::Navigate(in, g_settings)) SettingsChanged(h);
  g_overlayDirty = g_redraw = true;
}
void ToggleMenu() {
  if (!g_d3dActive) return;  // the menu needs the D3D11 presenter
  ui::Toggle();
  g_overlayDirty = g_redraw = true;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CLOSE: g_closed = true; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
      if (g_d3dActive) present::Resize();
      g_overlayDirty = g_redraw = true;
      return 0;
    case WM_SYSKEYDOWN:
      if (w == VK_RETURN && (l & (1 << 29))) {  // Alt+Enter
        g_settings.fullscreen ^= 1;
        SettingsChanged(h);
        return 0;
      }
      break;
    case WM_KEYDOWN:
      switch (w) {
        case VK_F1: ToggleMenu(); return 0;
        case VK_F11: g_settings.fullscreen ^= 1; SettingsChanged(h); return 0;
        default: break;
      }
      if (ui::Visible()) {
        switch (w) {
          case VK_UP: MenuInput(h, ui::Input::Up); return 0;
          case VK_DOWN: MenuInput(h, ui::Input::Down); return 0;
          case VK_LEFT: MenuInput(h, ui::Input::Left); return 0;
          case VK_RIGHT: MenuInput(h, ui::Input::Right); return 0;
          case VK_RETURN: case VK_SPACE: MenuInput(h, ui::Input::Accept); return 0;
          case VK_ESCAPE: case VK_BACK: MenuInput(h, ui::Input::Back); return 0;
          default: break;
        }
      }
      break;
    case kPadMessage:
      if (w & 0x10000) { ToggleMenu(); return 0; }
      if (w & 0x20000) {  // open from the game's menu
        if (!ui::Visible()) ToggleMenu();
        return 0;
      }
      if (ui::Visible()) {
        if (w & 0x0010) MenuInput(h, ui::Input::Up);
        if (w & 0x0040) MenuInput(h, ui::Input::Down);
        if (w & 0x0080) MenuInput(h, ui::Input::Left);
        if (w & 0x0020) MenuInput(h, ui::Input::Right);
        if (w & 0x4000) MenuInput(h, ui::Input::Accept);
        if (w & 0x2000) MenuInput(h, ui::Input::Back);
      }
      return 0;
    case WM_PAINT: {
      if (g_d3dActive) {
        ValidateRect(h, nullptr);
        g_redraw = true;
        return 0;
      }
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      std::lock_guard<std::mutex> lock(g_frameMutex);
      if (!g_frame.empty()) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = g_frameW;
        bi.bmiHeader.biHeight = -g_frameH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, g_frameW, g_frameH, g_frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
      }
      EndPaint(h, &ps);
      return 0;
    }
    default: break;
  }
  return DefWindowProcW(h, m, w, l);
}

void WindowThread() {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"HP2NativeWindow";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  g_settings = ui::Load();
  RECT r = {0, 0, 640 * g_scalePercent / 100, 480 * g_scalePercent / 100};
  if (g_scalePercent == 100) {  // default: 4:3 at 80% of the primary monitor's work area height
    RECT work = {};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) && work.bottom - work.top > 600) {
      r.bottom = (work.bottom - work.top) * 8 / 10;
      r.right = r.bottom * 4 / 3;
    }
  }
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  g_hwnd = CreateWindowW(L"HP2NativeWindow", L"Need for Speed: Hot Pursuit 2 - native runtime (development)   [F1 settings]",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                         wc.hInstance, nullptr);
  g_d3dActive = !std::getenv("HP2_GDI") && present::Init(g_hwnd);
  ApplyDisplayMode(g_hwnd);
  if (g_d3dActive && std::getenv("HP2_MENU")) ui::Toggle();  // development: start with the settings menu open
  const char* shotEnv = std::getenv("HP2_PRESENT_SHOT");     // development: back buffer to .cache/native/present.bmp after N frames
  std::uint64_t shotAt = shotEnv ? std::strtoull(shotEnv, nullptr, 10) : 0, drawn = 0;
  std::vector<u32> frame;
  int fw = 0, fh = 0;
  std::uint64_t seen = 0;
  for (;;) {
    // one present per new game frame (at most the game's 60; more only with real in-between frames, ADR 0013 §4)
    MsgWaitForMultipleObjects(1, &g_frameEvent, FALSE, 50, QS_ALLINPUT);
    MSG msg;
    bool quit = false;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) { quit = true; break; }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (quit) break;
    if (!g_d3dActive) continue;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_frameMutex);
      if (!g_windowShot.empty()) {
        static std::string pending;  // the presenter keeps the pointer until its next Draw
        pending = g_windowShot;
        g_windowShot.clear();
        present::RequestShot(pending.c_str());
        g_redraw = true;
      }
      if (g_frameSeq != seen && !g_frame.empty()) {
        frame = g_frame;
        fw = g_frameW;
        fh = g_frameH;
        seen = g_frameSeq;
        changed = true;
      }
    }
    const bool menu = ui::Visible();
    bool menuChanged = false;
    if (menu && g_overlayDirty) {
      RECT rc = {};
      GetClientRect(g_hwnd, &rc);
      if (ui::Render(g_settings, rc.right, rc.bottom, g_overlayPixels)) {
        g_overlayW = rc.right;
        g_overlayH = rc.bottom;
        menuChanged = true;
      }
      g_overlayDirty = false;
    }
    if (changed || menuChanged || g_redraw) {
      if (shotAt && ++drawn == shotAt) present::RequestShot(".cache/native/present.bmp");
      present::Draw(frame.empty() ? nullptr : frame.data(), fw, fh, changed, menu && !g_overlayPixels.empty() ? g_overlayPixels.data() : nullptr,
                    g_overlayW, g_overlayH, menuChanged, g_settings);
      g_redraw = false;
    }
  }
  if (g_d3dActive) present::Shutdown();
  g_d3dActive = false;
  g_closed = true;
}

// ---- rasteriser -----------------------------------------------------------------------------------
struct Rgba { int r, g, b, a; };

Rgba Unpack32(u32 v) { return {static_cast<int>(v & 255), static_cast<int>((v >> 8) & 255), static_cast<int>((v >> 16) & 255), static_cast<int>(v >> 24)}; }
u32 Pack32(Rgba c) { return static_cast<u32>(c.r) | (static_cast<u32>(c.g) << 8) | (static_cast<u32>(c.b) << 16) | (static_cast<u32>(c.a) << 24); }
Rgba Unpack16(u32 v, int a1, int a0) {
  return {static_cast<int>((v & 31) << 3), static_cast<int>(((v >> 5) & 31) << 3), static_cast<int>(((v >> 10) & 31) << 3), (v & 0x8000) ? a1 : a0};
}
int Clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// ---- CLUT buffer ---------------------------------------------------------------------------------
// The GS keeps a 256-entry palette buffer that is filled from local memory when TEX0/TEX2 is written with CLD != 0;
// later changes to the memory do not reach it until the next load. Entries are kept raw (32-bit, or 16-bit in the low half).
u32 g_clut[256] = {};
bool g_clut16 = false;        // the buffer holds 16-bit entries (CPSM CT16/CT16S)
u64 g_clutVersion = 1;        // bumped at every load (texture cache)
u64 g_texclut = 0;            // TEXCLUT: CBW [0:5], COU [6:11], COV [12:21] (CSM2 only)
u32 g_lastCbp[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};

bool IsIndexed(u32 psm) { return psm == T8 || psm == T4 || psm == T8H || psm == T4HL || psm == T4HH; }

void LoadClut(u64 tex0) {
  const u32 psm = static_cast<u32>((tex0 >> 20) & 0x3F);
  if (!IsIndexed(psm)) return;
  const u32 cbp = static_cast<u32>((tex0 >> 37) & 0x3FFF), cpsm = static_cast<u32>((tex0 >> 51) & 15), csa = static_cast<u32>((tex0 >> 56) & 31);
  const bool csm2 = ((tex0 >> 55) & 1) != 0;
  const bool is4 = KindOf(psm) == Kind::I4;
  const bool c16 = cpsm == CT16 || cpsm == CT16S;
  const u32 entries = is4 ? 16 : 256;
  g_clut16 = c16;
  const u32 fmt = cpsm == CT16S ? CT16S : (c16 ? CT16 : CT32);
  for (u32 e = 0; e < entries; ++e) {
    u32 x, y, bw = 1;
    if (csm2) {  // CSM2: entries in a line of a buffer whose width and start come from TEXCLUT
      bw = static_cast<u32>(g_texclut & 63);
      x = static_cast<u32>((g_texclut >> 6) & 63) * 16 + e;
      y = static_cast<u32>((g_texclut >> 12) & 0x3FF);
    } else if (is4) {  // CSM1, 4-bit: 16 entries; CT32 as 8x2 groups tiled in a 16x16 area, CT16 as one row of 16 per group
      if (c16) { x = e; y = csa; }
      else { x = (csa & 1) * 8 + (e & 7); y = (csa >> 1) * 2 + (e >> 3); }
    } else {  // CSM1, 8-bit: 16x16 entries, index bits 3 and 4 swapped
      const u32 i = (e & 0xE7) | ((e & 8) << 1) | ((e & 16) >> 1);
      x = i & 15; y = i >> 4;
    }
    g_clut[((is4 ? csa * 16 : 0) + e) & 255] = ReadPixel(fmt, cbp, bw, x, y);
  }
  // the version is the palette's content: games reload the same palette with every TEX0 write, which must not
  // invalidate the decoded indexed textures (hundreds of needless decodes per frame in races)
  u64 h = 1469598103934665603ull;
  for (u32 v : g_clut) h = (h ^ v) * 1099511628211ull;
  g_clutVersion = h | 1;
}

// CLD: 0 keep the buffer, 1 load, 2 load and remember CBP as CBP0, 3 load and remember it as CBP1,
// 4 load if CBP differs from CBP0 (and remember), 5 the same with CBP1.
void ApplyCld(u64 tex0) {
  const u32 cld = static_cast<u32>(tex0 >> 61) & 7;
  const u32 cbp = static_cast<u32>((tex0 >> 37) & 0x3FFF);
  switch (cld) {
    case 1: LoadClut(tex0); break;
    case 2: g_lastCbp[0] = cbp; LoadClut(tex0); break;
    case 3: g_lastCbp[1] = cbp; LoadClut(tex0); break;
    case 4: if (g_lastCbp[0] != cbp) { g_lastCbp[0] = cbp; LoadClut(tex0); } break;
    case 5: if (g_lastCbp[1] != cbp) { g_lastCbp[1] = cbp; LoadClut(tex0); } break;
    default: break;
  }
}

Rgba ClutLookup(const Ctx& c, u32 index) {
  const u32 psm = static_cast<u32>((c.tex0 >> 20) & 0x3F);
  const u32 csa = static_cast<u32>((c.tex0 >> 56) & 31);
  const u32 i = (KindOf(psm) == Kind::I4 ? csa * 16 + (index & 15) : index) & 255;
  return g_clut16 ? Unpack16(g_clut[i], static_cast<int>((g_texa >> 32) & 255), static_cast<int>(g_texa & 255)) : Unpack32(g_clut[i]);
}

Rgba TexelRaw(const Ctx& c, u32 x, u32 y) {  // texel (x, y) of the texture of context `c`, no wrapping (inside the texture)
  const u32 tbp = static_cast<u32>(c.tex0 & 0x3FFF), tbw = static_cast<u32>((c.tex0 >> 14) & 63), psm = static_cast<u32>((c.tex0 >> 20) & 0x3F);
  const int ta0 = static_cast<int>(g_texa & 255), ta1 = static_cast<int>((g_texa >> 32) & 255);
  const bool aem = ((g_texa >> 15) & 1) != 0;
  Rgba t = {0, 0, 0, 0};
  switch (psm) {
    case CT32: t = Unpack32(ReadPixel(CT32, tbp, tbw, x, y)); break;
    case CT24: {
      const u32 v = ReadPixel(CT24, tbp, tbw, x, y);
      t = Unpack32(v & 0xFFFFFF);
      t.a = (aem && (v & 0xFFFFFF) == 0) ? 0 : ta0;
      break;
    }
    case CT16: case CT16S: t = Unpack16(ReadPixel(psm, tbp, tbw, x, y), ta1, ta0); if (aem && (t.r | t.g | t.b) == 0 && t.a == ta0) t.a = 0; break;
    case Z32: t = Unpack32(ReadPixel(Z32, tbp, tbw, x, y)); break;  // the depth buffer read back as colours (post effects)
    case Z24: { const u32 v = ReadPixel(Z24, tbp, tbw, x, y); t = Unpack32(v & 0xFFFFFF); t.a = (aem && (v & 0xFFFFFF) == 0) ? 0 : ta0; break; }
    case Z16: case Z16S: t = Unpack16(ReadPixel(psm, tbp, tbw, x, y), ta1, ta0); if (aem && (t.r | t.g | t.b) == 0 && t.a == ta0) t.a = 0; break;
    case T8: t = ClutLookup(c, ReadPixel(T8, tbp, tbw, x, y)); break;
    case T4: t = ClutLookup(c, ReadPixel(T4, tbp, tbw, x, y)); break;
    case T8H: t = ClutLookup(c, ReadPixel(CT32, tbp, tbw, x, y) >> 24); break;
    case T4HL: t = ClutLookup(c, (ReadPixel(CT32, tbp, tbw, x, y) >> 24) & 15); break;
    case T4HH: t = ClutLookup(c, ReadPixel(CT32, tbp, tbw, x, y) >> 28); break;
    default: {  // (called from several threads while a texture is decoded: atomic counter and note)
      g_unsupportedTexels.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<bool> noted{false};  // development note, once: which format reached the texture decoder
      if (!noted.exchange(true)) {
        if (FILE* f = fopen(".cache/native/gs_unsupported.txt", "w")) { fprintf(f, "TexelRaw: psm %02X tex0 %016llX\n", psm, static_cast<unsigned long long>(c.tex0)); fclose(f); }
      }
      break;
    }
  }
  return t;
}

// A decoded copy of a texture (and its palette), valid while the GS memory pages it came from are unchanged.
struct TexCache {
  u64 tex0 = 0, texa = 0;
  u32 w = 0, h = 0, firstPage = 0, pages = 0, clutPage = 0;
  u64 version = 0;
  u64 used = 0;
  std::vector<u32> pix;  // packed RGBA, row after row
};
// Keyed by TEX0 (without CLD) and TEXA. A race uses far more than the 48 textures the first cache held: with a
// round-robin use LRU missed every time (hundreds of decodes per frame). Now a hash table with a memory budget.
struct TexKey {
  u64 tex0, texa;
  bool operator==(const TexKey& o) const { return tex0 == o.tex0 && texa == o.texa; }
};
struct TexKeyHash {
  std::size_t operator()(const TexKey& k) const { return std::hash<u64>()(k.tex0 * 0x9E3779B97F4A7C15ull ^ k.texa); }
};
std::unordered_map<TexKey, TexCache, TexKeyHash> g_texCache;
u64 g_texClock = 0, g_texBytes = 0;
constexpr u64 kTexBudget = 512ull << 20;  // decoded texels kept (bytes)

u64 PageVersionSum(u32 first, u32 count) {
  u64 s = 0;
  for (u32 i = 0; i < count; ++i) s += g_pageVersion[(first + i) & 511];
  return s;
}

const TexCache& GetTexture(const Ctx& c) {
  const u32 tbp = static_cast<u32>(c.tex0 & 0x3FFF), tbw = static_cast<u32>((c.tex0 >> 14) & 63), psm = static_cast<u32>((c.tex0 >> 20) & 0x3F);
  const u32 tw = 1u << ((c.tex0 >> 26) & 15), th = 1u << ((c.tex0 >> 30) & 15);
  const Kind k = KindOf(psm == T8H || psm == T4HL || psm == T4HH ? CT32 : psm);
  const PageGeometry pg = PageOf(k);
  const u32 perRow = std::max<u32>(1, tbw * 64 / pg.w);
  const u32 first = tbp * 256 / 8192, pages = perRow * ((th + pg.h - 1) / pg.h) + 1;
  const u32 cbp = static_cast<u32>((c.tex0 >> 37) & 0x3FFF);
  const bool indexed = psm == T8 || psm == T4 || psm == T8H || psm == T4HL || psm == T4HH;
  const u32 clutPage = cbp * 256 / 8192;  // (kept for reference; the buffer version decides)
  const u64 ver = PageVersionSum(first, pages) + (indexed ? g_clutVersion * 7 : 0);
  const u64 key = c.tex0 & ~(0x7ull << 61);  // CLD (clut load control) does not change the pixels
  const TexKey cacheKey{key, g_texa};
  auto found = g_texCache.find(cacheKey);
  static const bool noCache = std::getenv("HP2_NOTEXCACHE") != nullptr;  // diagnostic: decode at every use
  if (found != g_texCache.end() && found->second.version == ver && !noCache) {
    found->second.used = ++g_texClock;
    return found->second;
  }
  const u64 bytes = static_cast<u64>(tw) * th * 4;
  if (found == g_texCache.end()) {
    while (g_texBytes + bytes > kTexBudget && !g_texCache.empty()) {  // over budget: drop the least recently used
      auto lru = std::min_element(g_texCache.begin(), g_texCache.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
      g_texBytes -= lru->second.pix.size() * 4;
      g_texCache.erase(lru);
    }
    found = g_texCache.emplace(cacheKey, TexCache{}).first;
  } else {
    g_texBytes -= found->second.pix.size() * 4;  // stale: decoded again in place
  }
  g_texBytes += bytes;
  TexCache* slot = &found->second;
  slot->tex0 = key; slot->texa = g_texa; slot->version = ver; slot->used = ++g_texClock;
  slot->w = tw; slot->h = th; slot->firstPage = first; slot->pages = pages; slot->clutPage = clutPage;
  Ticks timer(g_stats.textureTicks);
  ++g_stats.textureDecodes;
  slot->pix.resize(static_cast<std::size_t>(tw) * th);
  u32* out = slot->pix.data();
  auto row = [&](int y) {  // rows are independent (read-only GS memory and CLUT): large textures are split between threads
    for (u32 x = 0; x < tw; ++x) out[static_cast<std::size_t>(y) * tw + x] = Pack32(TexelRaw(c, x, static_cast<u32>(y)));
  };
  if (static_cast<i64>(tw) * th >= kParallelArea) Pool().Run(0, static_cast<int>(th) - 1, row);
  else for (u32 y = 0; y < th; ++y) row(static_cast<int>(y));
  return *slot;
}

// Everything a pixel needs, decoded once per primitive.
struct Pipe {
  const Ctx* c;
  bool tme, abe, fst, flat, linear, tcc, ate, zte, zmsk, aem, date, datm, fba, is16;
  u32 tfx, atst, aref, ztst, zpsm, fpsm, fbmsk, afail;
  int sx0, sx1, sy0, sy1;
  u32 tw, th, wms, wmt, minu, maxu, minv, maxv;
  const TexCache* tex;
  Surf fb, zb;
};

u32 WrapCoord(i32 v, u32 size, u32 mode, u32 minv, u32 maxv) {
  switch (mode) {
    case 0: return static_cast<u32>(v) & (size - 1);
    case 1: return v < 0 ? 0 : (static_cast<u32>(v) >= size ? size - 1 : static_cast<u32>(v));
    case 2: return static_cast<u32>(std::clamp<i32>(v, static_cast<i32>(minv), static_cast<i32>(maxv)));
    default: return (static_cast<u32>(v) & minv) | maxv;
  }
}
inline Rgba Texel(const Pipe& p, i32 x, i32 y) {
  const u32 ux = WrapCoord(x, p.tw, p.wms, p.minu, p.maxu), uy = WrapCoord(y, p.th, p.wmt, p.minv, p.maxv);
  return Unpack32(p.tex->pix[static_cast<std::size_t>(uy & (p.th - 1)) * p.tw + (ux & (p.tw - 1))]);
}

Pipe MakePipe(bool flat) {
  Pipe p = {};
  const Ctx& c = g_ctx[(g_prim >> 9) & 1];
  p.c = &c;
  p.tme = (g_prim >> 4) & 1; p.abe = (g_prim >> 6) & 1; p.fst = (g_prim >> 8) & 1; p.flat = flat;
  const u32 fbp = static_cast<u32>(c.frame & 0x1FF) * 32, fbw = static_cast<u32>((c.frame >> 16) & 63);
  p.fpsm = static_cast<u32>((c.frame >> 24) & 0x3F);
  p.fbmsk = static_cast<u32>(c.frame >> 32);
  p.zpsm = 0x30 | static_cast<u32>((c.zbuf >> 24) & 15);
  p.zmsk = ((c.zbuf >> 32) & 1) != 0;
  p.zte = ((c.test >> 16) & 1) != 0;
  p.ztst = static_cast<u32>((c.test >> 17) & 3);
  p.ate = (c.test & 1) != 0;
  p.atst = static_cast<u32>((c.test >> 1) & 7); p.aref = static_cast<u32>((c.test >> 4) & 255);
  p.afail = static_cast<u32>((c.test >> 12) & 3);
  p.date = ((c.test >> 14) & 1) != 0;  // destination alpha test: only where the frame buffer's alpha bit equals DATM
  p.datm = ((c.test >> 15) & 1) != 0;
  p.fba = (g_fba[(g_prim >> 9) & 1] & 1) != 0;  // FBA: written pixels get their alpha bit set (masks for later passes)
  p.is16 = p.fpsm == CT16 || p.fpsm == CT16S;
  p.sx0 = static_cast<int>(c.scissor & 0x7FF); p.sx1 = static_cast<int>((c.scissor >> 16) & 0x7FF);
  p.sy0 = static_cast<int>((c.scissor >> 32) & 0x7FF); p.sy1 = static_cast<int>((c.scissor >> 48) & 0x7FF);
  p.tfx = static_cast<u32>((c.tex0 >> 35) & 3);
  p.tcc = ((c.tex0 >> 34) & 1) != 0;
  p.tw = 1u << ((c.tex0 >> 26) & 15); p.th = 1u << ((c.tex0 >> 30) & 15);
  p.linear = ((c.tex1 >> 5) & 1) != 0;
  p.wms = static_cast<u32>(c.clamp & 3); p.wmt = static_cast<u32>((c.clamp >> 2) & 3);
  p.minu = static_cast<u32>((c.clamp >> 4) & 0x3FF); p.maxu = static_cast<u32>((c.clamp >> 14) & 0x3FF);
  p.minv = static_cast<u32>((c.clamp >> 24) & 0x3FF); p.maxv = static_cast<u32>((c.clamp >> 34) & 0x3FF);
  p.fb = MakeSurf(p.fpsm, fbp, fbw);
  p.zb = MakeSurf(p.zpsm, static_cast<u32>(c.zbuf & 0x1FF) * 32, fbw);
  p.tex = p.tme ? &GetTexture(c) : nullptr;
  return p;
}

// One pixel: texture, depth/alpha tests, blending, write. `col` is the interpolated vertex colour, (tu, tv) the texel position.
inline bool Shade(const Pipe& p, int px, int py, u32 z, Rgba col, float tu, float tv) {
  const u32 zv = p.zpsm == Z24 ? (z >> 8) : (p.zpsm == Z32 ? z : (z >> 16) & 0xFFFF);
  if (p.zte && p.ztst != 1) {
    if (p.ztst == 0) return false;
    const u32 old = SurfRead(p.zb, static_cast<u32>(px), static_cast<u32>(py));
    if (p.ztst == 2 ? zv < old : zv <= old) return false;  // GEQUAL / GREATER
  }
  if (p.date) {  // destination alpha test (stencil-like masks: car shadows, effects): a failed pixel is not written at all
    const u32 d = SurfRead(p.fb, static_cast<u32>(px), static_cast<u32>(py));
    if (((d & (p.is16 ? 0x8000u : 0x80000000u)) != 0) != p.datm) return false;
  }
  if (p.tme) {
    Rgba tex;
    if (p.linear) {
      const float fx = tu - 0.5f, fy = tv - 0.5f;
      const i32 ix = static_cast<i32>(std::floor(fx)), iy = static_cast<i32>(std::floor(fy));
      const float ux = fx - static_cast<float>(ix), uy = fy - static_cast<float>(iy);
      const Rgba t00 = Texel(p, ix, iy), t10 = Texel(p, ix + 1, iy), t01 = Texel(p, ix, iy + 1), t11 = Texel(p, ix + 1, iy + 1);
      auto mix = [&](int a, int b, int c, int d) { return static_cast<int>((static_cast<float>(a) * (1 - ux) + static_cast<float>(b) * ux) * (1 - uy) + (static_cast<float>(c) * (1 - ux) + static_cast<float>(d) * ux) * uy); };
      tex = {mix(t00.r, t10.r, t01.r, t11.r), mix(t00.g, t10.g, t01.g, t11.g), mix(t00.b, t10.b, t01.b, t11.b), mix(t00.a, t10.a, t01.a, t11.a)};
    } else {
      i32 ix = static_cast<i32>(tu), iy = static_cast<i32>(tv);
      if (tu < 0 && static_cast<float>(ix) != tu) --ix;
      if (tv < 0 && static_cast<float>(iy) != tv) --iy;
      tex = Texel(p, ix, iy);
    }
    switch (p.tfx) {
      case 0: col = {Clamp8(tex.r * col.r >> 7), Clamp8(tex.g * col.g >> 7), Clamp8(tex.b * col.b >> 7), p.tcc ? Clamp8(tex.a * col.a >> 7) : col.a}; break;
      case 1: col = {tex.r, tex.g, tex.b, p.tcc ? tex.a : col.a}; break;
      default: col = {Clamp8((tex.r * col.r >> 7) + col.a), Clamp8((tex.g * col.g >> 7) + col.a), Clamp8((tex.b * col.b >> 7) + col.a), p.tcc ? Clamp8(tex.a + col.a) : col.a}; break;
    }
  } else {
    col = {Clamp8(col.r), Clamp8(col.g), Clamp8(col.b), Clamp8(col.a)};
  }
  bool writeZ = true;  // (alpha test fail modes can drop the depth write or the alpha channel)
  u32 keep = 0;
  if (p.ate) {
    const u32 av = static_cast<u32>(col.a);
    bool pass = true;
    switch (p.atst) {
      case 0: pass = false; break;
      case 2: pass = av < p.aref; break;
      case 3: pass = av <= p.aref; break;
      case 4: pass = av == p.aref; break;
      case 5: pass = av >= p.aref; break;
      case 6: pass = av > p.aref; break;
      case 7: pass = av != p.aref; break;
      default: break;
    }
    if (!pass) {  // AFAIL: what a failed pixel still updates
      switch (p.afail) {
        case 1: writeZ = false; break;  // FB_ONLY: colour and alpha, no depth
        case 2:                         // ZB_ONLY: depth only
          if (!p.zmsk && p.zte) SurfWrite(p.zb, static_cast<u32>(px), static_cast<u32>(py), zv, 0xFFFFFFFFu);
          return true;
        case 3: writeZ = false; keep |= p.is16 ? 0x8000u : 0xFF000000u; break;  // RGB_ONLY: no alpha, no depth
        default: return false;  // KEEP
      }
    }
  }
  const bool is16 = p.is16;
  if (p.abe && (!(g_pabe & 1) || (col.a & 0x80))) {
    const u32 raw = SurfRead(p.fb, static_cast<u32>(px), static_cast<u32>(py));
    const Rgba dst = is16 ? Unpack16(raw, 128, 0) : Unpack32(raw);
    const Ctx& c = *p.c;
    const u32 A = static_cast<u32>(c.alpha & 3), B = static_cast<u32>((c.alpha >> 2) & 3), C = static_cast<u32>((c.alpha >> 4) & 3), D = static_cast<u32>((c.alpha >> 6) & 3);
    const int fix = static_cast<int>((c.alpha >> 32) & 255);
    auto pick = [&](u32 sel, int cs, int cd) { return sel == 0 ? cs : sel == 1 ? cd : 0; };
    const int ac = C == 0 ? col.a : C == 1 ? dst.a : fix;
    auto channel = [&](int cs, int cd) { return Clamp8(((pick(A, cs, cd) - pick(B, cs, cd)) * ac >> 7) + pick(D, cs, cd)); };
    col = {channel(col.r, dst.r), channel(col.g, dst.g), channel(col.b, dst.b), col.a};
  }
  if (p.fba) col.a |= 0x80;
  const u32 out = is16 ? static_cast<u32>((col.r >> 3) | ((col.g >> 3) << 5) | ((col.b >> 3) << 10) | ((col.a & 0x80) << 8)) : Pack32(col);
  SurfWrite(p.fb, static_cast<u32>(px), static_cast<u32>(py), out, ~(p.fbmsk | keep));
  if (writeZ && !p.zmsk && p.zte) SurfWrite(p.zb, static_cast<u32>(px), static_cast<u32>(py), zv, 0xFFFFFFFFu);
  return true;
}

// Diagnostic (SetPrimLog): one line per primitive whose rectangle covers at least 2048 pixels.
bool g_primLog = false;
std::string g_primLogText;
void PrimLogLine(const char* kind, const Pipe& p, int minx, int maxx, int miny, int maxy, u64 written) {
  if (!g_primLog || g_primLogText.size() > 2000000) return;
  const Ctx& c = *p.c;
  char line[320];
  snprintf(line, sizeof line,
           "%s %d,%d-%d,%d fb %u/%02X msk %08X z %u/%02X zte%d ztst%u zmsk%d tex%d %u/%02X %ux%u tfx%u tcc%d abe%d alpha %llX ate%d atst%u aref%u afail%u date%d datm%d fba%d px %llu\n",
           kind, minx, miny, maxx, maxy, static_cast<u32>(c.frame & 0x1FF), p.fpsm, p.fbmsk, static_cast<u32>(c.zbuf & 0x1FF), p.zpsm, p.zte, p.ztst,
           p.zmsk, p.tme, static_cast<u32>(c.tex0 & 0x3FFF), static_cast<u32>((c.tex0 >> 20) & 0x3F), p.tw, p.th, p.tfx, p.tcc, p.abe,
           static_cast<unsigned long long>(c.alpha), p.ate, p.atst, p.aref, p.afail, p.date, p.datm, p.fba, static_cast<unsigned long long>(written));
  g_primLogText += line;
}

void DrawTriangle(const Vertex& v0, const Vertex& v1_, const Vertex& v2_, bool flat) {
  static const bool skip = getenv("HP2_NOGS") != nullptr;  // development: measure the rest of the runtime without rasterising
  if (skip) return;
  const Pipe p = MakePipe(flat);
  Vertex a = v0, b = v1_, d = v2_;
  const i64 area = static_cast<i64>(b.x - a.x) * (d.y - a.y) - static_cast<i64>(b.y - a.y) * (d.x - a.x);
  if (area == 0) return;
  if (area < 0) std::swap(b, d);
  const double inv = 1.0 / static_cast<double>(area < 0 ? -area : area);
  const int minx = std::max((std::min({a.x, b.x, d.x}) >> 4), p.sx0), maxx = std::min((std::max({a.x, b.x, d.x}) + 15) >> 4, p.sx1);
  const int miny = std::max((std::min({a.y, b.y, d.y}) >> 4), p.sy0), maxy = std::min((std::max({a.y, b.y, d.y}) + 15) >> 4, p.sy1);
  if (minx > maxx || miny > maxy) {  // outside the scissor: logged with its unclipped bounds (pixels, offset removed)
    if (g_primLog)
      PrimLogLine("offtri", p, std::min({a.x, b.x, d.x}) >> 4, std::max({a.x, b.x, d.x}) >> 4, std::min({a.y, b.y, d.y}) >> 4,
                  std::max({a.y, b.y, d.y}) >> 4, 0);
    return;
  }
  auto bias = [](const Vertex& s, const Vertex& e) {  // top-left rule
    const i32 dx = e.x - s.x, dy = e.y - s.y;
    return (dy > 0 || (dy == 0 && dx < 0)) ? 0 : -1;
  };
  const i64 bias2 = bias(a, b), bias0 = bias(b, d), bias1 = bias(d, a);
  // edge functions: w2 (weight of d) on a->b, w0 (weight of a) on b->d, w1 (weight of b) on d->a; each steps linearly in x and y
  const i64 dx2 = -(static_cast<i64>(b.y - a.y)) * 16, dy2 = static_cast<i64>(b.x - a.x) * 16;
  const i64 dx0 = -(static_cast<i64>(d.y - b.y)) * 16, dy0 = static_cast<i64>(d.x - b.x) * 16;
  const i64 dx1 = -(static_cast<i64>(a.y - d.y)) * 16, dy1 = static_cast<i64>(a.x - d.x) * 16;
  const i64 X0 = minx * 16 + 8, Y0 = miny * 16 + 8;
  i64 r2 = static_cast<i64>(b.x - a.x) * (Y0 - a.y) - static_cast<i64>(b.y - a.y) * (X0 - a.x) + bias2;
  i64 r0 = static_cast<i64>(d.x - b.x) * (Y0 - b.y) - static_cast<i64>(d.y - b.y) * (X0 - b.x) + bias0;
  i64 r1 = static_cast<i64>(a.x - d.x) * (Y0 - d.y) - static_cast<i64>(a.y - d.y) * (X0 - d.x) + bias1;
  const float tw = static_cast<float>(p.tw), th = static_cast<float>(p.th);
  auto row = [&](int py) {
    const i64 k = py - miny;
    i64 w0 = r0 + dy0 * k, w1 = r1 + dy1 * k, w2 = r2 + dy2 * k;
    u64 written = 0;
    for (int px = minx; px <= maxx; ++px, w0 += dx0, w1 += dx1, w2 += dx2) {
      if ((w0 | w1 | w2) < 0) continue;
      const float l0 = static_cast<float>(static_cast<double>(w0) * inv), l1 = static_cast<float>(static_cast<double>(w1) * inv), l2 = 1.0f - l0 - l1;
      const u32 z = static_cast<u32>(std::clamp(static_cast<double>(l0) * a.z + static_cast<double>(l1) * b.z + static_cast<double>(l2) * d.z, 0.0, 4294967295.0));
      Rgba col;
      if (p.flat) col = {static_cast<int>(d.r), static_cast<int>(d.g), static_cast<int>(d.b), static_cast<int>(d.a)};
      else col = {static_cast<int>(l0 * a.r + l1 * b.r + l2 * d.r), static_cast<int>(l0 * a.g + l1 * b.g + l2 * d.g),
                  static_cast<int>(l0 * a.b + l1 * b.b + l2 * d.b), static_cast<int>(l0 * a.a + l1 * b.a + l2 * d.a)};
      float tu = 0, tv = 0;
      if (p.tme) {
        if (p.fst) {
          tu = (l0 * a.u + l1 * b.u + l2 * d.u) * (1.0f / 16.0f);
          tv = (l0 * a.v + l1 * b.v + l2 * d.v) * (1.0f / 16.0f);
        } else {
          const float q = l0 * a.q + l1 * b.q + l2 * d.q;
          if (q != 0.0f) {
            const float rq = 1.0f / q;
            tu = (l0 * a.s + l1 * b.s + l2 * d.s) * rq * tw;
            tv = (l0 * a.t + l1 * b.t + l2 * d.t) * rq * th;
          }
        }
      }
      written += Shade(p, px, py, z, col, tu, tv) ? 1 : 0;
    }
    if (written) g_pixelCount.fetch_add(written, std::memory_order_relaxed);
    if (written) g_targetPixels[(p.fb.base >> 13) & 511].fetch_add(written, std::memory_order_relaxed);
    if (written && p.tme) g_texturedCount.fetch_add(written, std::memory_order_relaxed);
  };
  const u64 before = g_pixelCount.load();
  if (static_cast<i64>(maxx - minx + 1) * (maxy - miny + 1) >= kParallelArea) Pool().Run(miny, maxy, row);
  else for (int py = miny; py <= maxy; ++py) row(py);
  PrimLogLine("tri", p, minx, maxx, miny, maxy, g_pixelCount.load() - before);
  BumpRect(p.fb, minx, maxx, miny, maxy);
  if (!p.zmsk && p.zte) BumpRect(p.zb, minx, maxx, miny, maxy);
}

// An axis-aligned sprite: corners (a = upper left, b = lower right) with the texture coordinates of each, flat colour.
void DrawSprite(const Vertex& a, const Vertex& b) {
  static const bool skip = getenv("HP2_NOGS") != nullptr;
  if (skip) return;
  {  // The race's depth fog: the frame buffer read as T8H (its alpha, filled with depth bits through 16-bit views of the
     // Z buffer, indexes a fog palette) blended over the scene. Until those depth-to-alpha copies are exact the index
     // comes out wrong and the fog colour covers the whole scene: the pass is skipped (HP2_KEEP_DEPTH_FOG=1 keeps it).
    static const bool keepFog = getenv("HP2_KEEP_DEPTH_FOG") != nullptr;
    const Ctx& c = g_ctx[(g_prim >> 9) & 1];
    if (!keepFog && ((g_prim >> 4) & 1) && ((g_prim >> 6) & 1) && ((c.tex0 >> 20) & 0x3F) == T8H) {
      static bool noted = false;
      if (!noted) { noted = true; ++g_stats.skippedDepthFog; }
      return;
    }
  }
  const Pipe p = MakePipe(true);
  const i32 x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
  if (x1 == x0 || y1 == y0) return;
  const int minx = std::max((x0 + 15) >> 4, p.sx0), maxx = std::min(((x1 + 15) >> 4) - 1, p.sx1);  // pixel centres inside [x0, x1)
  const int miny = std::max((y0 + 15) >> 4, p.sy0), maxy = std::min(((y1 + 15) >> 4) - 1, p.sy1);
  const Rgba col = {static_cast<int>(b.r), static_cast<int>(b.g), static_cast<int>(b.b), static_cast<int>(b.a)};
  const float tw = static_cast<float>(p.tw), th = static_cast<float>(p.th);
  float su0, su1, sv0, sv1;  // texel coordinates at the corners
  if (p.fst) { su0 = a.u / 16.0f; su1 = b.u / 16.0f; sv0 = a.v / 16.0f; sv1 = b.v / 16.0f; }
  else {
    su0 = a.q != 0 ? a.s / a.q * tw : 0; su1 = b.q != 0 ? b.s / b.q * tw : 0;
    sv0 = a.q != 0 ? a.t / a.q * th : 0; sv1 = b.q != 0 ? b.t / b.q * th : 0;
  }
  const float fw = static_cast<float>(x1 - x0), fh = static_cast<float>(y1 - y0);
  if (minx > maxx || miny > maxy) return;
  auto row = [&](int py) {
    const float fy = (static_cast<float>(py * 16 + 8 - y0)) / fh;
    const float tv = sv0 + (sv1 - sv0) * fy;
    u64 written = 0;
    for (int px = minx; px <= maxx; ++px) {
      const float fx = (static_cast<float>(px * 16 + 8 - x0)) / fw;
      written += Shade(p, px, py, b.z, col, su0 + (su1 - su0) * fx, tv) ? 1 : 0;
    }
    if (written) g_pixelCount.fetch_add(written, std::memory_order_relaxed);
    if (written) g_targetPixels[(p.fb.base >> 13) & 511].fetch_add(written, std::memory_order_relaxed);
    if (written && p.tme) g_texturedCount.fetch_add(written, std::memory_order_relaxed);
  };
  const u64 before = g_pixelCount.load();
  if (static_cast<i64>(maxx - minx + 1) * (maxy - miny + 1) >= kParallelArea) Pool().Run(miny, maxy, row);
  else for (int py = miny; py <= maxy; ++py) row(py);
  PrimLogLine("sprite", p, minx, maxx, miny, maxy, g_pixelCount.load() - before);
  BumpRect(p.fb, minx, maxx, miny, maxy);
  if (!p.zmsk && p.zte) BumpRect(p.zb, minx, maxx, miny, maxy);
}

Vertex MakeVertex(u64 xyz, bool hasF) {
  Vertex v = {};
  const Ctx& c = g_ctx[(g_prim >> 9) & 1];
  const i32 ofx = static_cast<i32>(c.xyoffset & 0xFFFF), ofy = static_cast<i32>((c.xyoffset >> 32) & 0xFFFF);
  v.x = static_cast<i32>(xyz & 0xFFFF) - ofx;
  v.y = static_cast<i32>((xyz >> 16) & 0xFFFF) - ofy;
  v.z = hasF ? static_cast<u32>((xyz >> 32) & 0xFFFFFF) << 8 : static_cast<u32>(xyz >> 32);
  v.r = static_cast<float>(g_rgbaq & 255);
  v.g = static_cast<float>((g_rgbaq >> 8) & 255);
  v.b = static_cast<float>((g_rgbaq >> 16) & 255);
  v.a = static_cast<float>((g_rgbaq >> 24) & 255);
  float s, t;
  const u32 sb = static_cast<u32>(g_st), tb = static_cast<u32>(g_st >> 32);
  memcpy(&s, &sb, 4);
  memcpy(&t, &tb, 4);
  v.s = s; v.t = t; v.q = g_q;
  v.u = static_cast<float>(g_uv & 0x3FFF);
  v.v = static_cast<float>((g_uv >> 16) & 0x3FFF);
  return v;
}

// draw=false (XYZ3/XYZF3): the vertex is queued but no primitive is emitted (the vertex-kick is only XYZ2/XYZF2).
void Kick(bool hasF, u64 xyz, bool draw) {
  Ticks timer(g_stats.drawTicks);
  const u32 type = static_cast<u32>(g_prim & 7);
  const bool flat = ((g_prim >> 3) & 1) == 0;
  Vertex v = MakeVertex(xyz, hasF);
  if (g_primLog && !draw && g_primLogText.size() < 2000000) {  // diagnostic: no-draw vertices (VU1 clipping marks them with ADC)
    char line[96];
    snprintf(line, sizeof line, "adc prim %u fb %u %d,%d z %08X\n", type, static_cast<u32>(g_ctx[(g_prim >> 9) & 1].frame & 0x1FF),
             v.x >> 4, v.y >> 4, v.z);
    g_primLogText += line;
  }
  switch (type) {
    case 0: ++g_stats.points; break;
    case 1: case 2: ++g_stats.lines; break;  // lines are not drawn yet
    case 3:  // triangle list
      g_queue[g_queued++] = v;
      if (g_queued == 3 && !draw) g_queued = 0;  // (no-kick vertex completing a triangle: dropped)
      if (g_queued == 3) { DrawTriangle(g_queue[0], g_queue[1], g_queue[2], flat); ++g_stats.triangles; g_queued = 0; }
      break;
    case 4:  // strip
      if (g_queued < 3) g_queue[g_queued++] = v;
      else { g_queue[0] = g_queue[1]; g_queue[1] = g_queue[2]; g_queue[2] = v; }
      if (g_queued == 3 && draw) { DrawTriangle(g_queue[0], g_queue[1], g_queue[2], flat); ++g_stats.triangles; }
      break;
    case 5:  // fan
      if (g_queued < 3) g_queue[g_queued++] = v;
      else { g_queue[1] = g_queue[2]; g_queue[2] = v; }
      if (g_queued == 3 && draw) { DrawTriangle(g_queue[0], g_queue[1], g_queue[2], flat); ++g_stats.triangles; }
      break;
    case 6: {  // sprite: two corners, flat colour of the second vertex
      g_queue[g_queued++] = v;
      if (g_queued == 2 && draw) {
        DrawSprite(g_queue[0], g_queue[1]);
        ++g_stats.sprites;
        g_queued = 0;
      }
      break;
    }
    default: ++g_stats.unsupported; break;
  }
}

// ---- image transfer ------------------------------------------------------------------------------
void BeginTransfer() {
  g_xfer.active = true;
  g_xfer.bp = static_cast<u32>((g_bitbltbuf >> 32) & 0x3FFF);
  g_xfer.bw = static_cast<u32>((g_bitbltbuf >> 48) & 63);
  g_xfer.psm = static_cast<u32>((g_bitbltbuf >> 56) & 0x3F);
  g_xfer.x0 = static_cast<u32>((g_trxpos >> 32) & 0x7FF);
  g_xfer.y0 = static_cast<u32>((g_trxpos >> 48) & 0x7FF);
  g_xfer.w = static_cast<u32>(g_trxreg & 0xFFF);
  g_xfer.h = static_cast<u32>((g_trxreg >> 32) & 0xFFF);
  g_xfer.pos = 0;
  g_xfer.pending = 0;
  g_xfer.pendingBits = 0;
  ++g_stats.transfers;
}

void PutPixelStream(u32 value) {  // one pixel (already extracted) of the running transfer
  if (g_xfer.w == 0) return;
  const u32 x = g_xfer.x0 + g_xfer.pos % g_xfer.w, y = g_xfer.y0 + g_xfer.pos / g_xfer.w;
  ++g_xfer.pos;
  if (g_xfer.pos / g_xfer.w >= g_xfer.h && g_xfer.pos % g_xfer.w == 0) g_xfer.active = false;
  const u32 psm = g_xfer.psm;
  if (psm == T8H) { WritePixel(CT32, g_xfer.bp, g_xfer.bw, x, y, value << 24, 0xFF000000u); return; }
  if (psm == T4HL) { WritePixel(CT32, g_xfer.bp, g_xfer.bw, x, y, value << 24, 0x0F000000u); return; }
  if (psm == T4HH) { WritePixel(CT32, g_xfer.bp, g_xfer.bw, x, y, value << 28, 0xF0000000u); return; }
  WritePixel(psm, g_xfer.bp, g_xfer.bw, x, y, value);
}

}  // namespace

void Reset() {
  g_mem.assign(kRamSize, 0);
  g_frame.clear();
}

void StQ(std::uint32_t s, std::uint32_t t, std::uint32_t q) {
  g_st = s | (static_cast<u64>(t) << 32);
  memcpy(&g_q, &q, 4);
}

void ImageData(const std::uint8_t* data, std::size_t bytes) {
  if (!g_xfer.active) return;
  Ticks timer(g_stats.transferTicks);
  if (g_mem.empty()) Reset();
  const u32 bits = BitsOf(g_xfer.psm == T8H ? T8 : g_xfer.psm == T4HL || g_xfer.psm == T4HH ? T4 : g_xfer.psm);
  g_stats.transferBytes += bytes;
  if (bits == 32) {
    for (std::size_t i = 0; i + 4 <= bytes && g_xfer.active; i += 4) { u32 v; memcpy(&v, data + i, 4); PutPixelStream(v); }
  } else if (bits == 24) {
    // 3 bytes per pixel, packed
    for (std::size_t i = 0; i + 3 <= bytes && g_xfer.active; i += 3) PutPixelStream(data[i] | (static_cast<u32>(data[i + 1]) << 8) | (static_cast<u32>(data[i + 2]) << 16));
  } else if (bits == 16) {
    for (std::size_t i = 0; i + 2 <= bytes && g_xfer.active; i += 2) { u16 v; memcpy(&v, data + i, 2); PutPixelStream(v); }
  } else if (bits == 8) {
    for (std::size_t i = 0; i < bytes && g_xfer.active; ++i) PutPixelStream(data[i]);
  } else {
    for (std::size_t i = 0; i < bytes && g_xfer.active; ++i) { PutPixelStream(data[i] & 15); if (g_xfer.active) PutPixelStream(data[i] >> 4); }
  }
}

void RegWrite(std::uint32_t reg, std::uint64_t v) {
  if (g_mem.empty()) Reset();
  switch (reg) {
    case 0x00: g_prim = v & 0x7FF; g_queued = 0; break;
    case 0x01: g_rgbaq = v; break;
    case 0x02: g_st = v; break;
    case 0x03: g_uv = v; break;
    case 0x04: Kick(true, v, true); break;
    case 0x0C: Kick(true, v, false); break;
    case 0x05: Kick(false, v, true); break;
    case 0x0D: Kick(false, v, false); break;
    case 0x06: g_ctx[0].tex0 = v; ApplyCld(v); break;
    case 0x07: g_ctx[1].tex0 = v; ApplyCld(v); break;
    case 0x16: case 0x17: {  // TEX2: only PSM, CBP, CPSM, CSM, CSA and CLD of TEX0 change
      Ctx& c = g_ctx[reg - 0x16];
      const u64 mask = (0x3Full << 20) | (0x7FFFFFull << 37);  // bits 20-25 and 37-63
      c.tex0 = (c.tex0 & ~mask) | (v & mask);
      ApplyCld(c.tex0);
      break;
    }
    case 0x1C: g_texclut = v; break;
    case 0x08: g_ctx[0].clamp = v; break;
    case 0x09: g_ctx[1].clamp = v; break;
    case 0x14: g_ctx[0].tex1 = v; break;
    case 0x15: g_ctx[1].tex1 = v; break;
    case 0x18: g_ctx[0].xyoffset = v; break;
    case 0x19: g_ctx[1].xyoffset = v; break;
    case 0x3B: g_texa = v; break;
    case 0x3D: g_fogcol = v; break;
    case 0x40: g_ctx[0].scissor = v; break;
    case 0x41: g_ctx[1].scissor = v; break;
    case 0x42: g_ctx[0].alpha = v; break;
    case 0x43: g_ctx[1].alpha = v; break;
    case 0x46: g_colclamp = v; break;
    case 0x47: g_ctx[0].test = v; break;
    case 0x48: g_ctx[1].test = v; break;
    case 0x49: g_pabe = v; break;
    case 0x4A: g_fba[0] = v; break;
    case 0x4B: g_fba[1] = v; break;
    case 0x4C: g_ctx[0].frame = v; break;
    case 0x4D: g_ctx[1].frame = v; break;
    case 0x4E: g_ctx[0].zbuf = v; break;
    case 0x4F: g_ctx[1].zbuf = v; break;
    case 0x50: g_bitbltbuf = v; break;
    case 0x51: g_trxpos = v; break;
    case 0x52: g_trxreg = v; break;
    case 0x53:  // TRXDIR
      g_trxdir = v;
      if ((v & 3) == 0) BeginTransfer();
      else if ((v & 3) == 2) {  // local to local copy
        const u32 sbp = static_cast<u32>(g_bitbltbuf & 0x3FFF), sbw = static_cast<u32>((g_bitbltbuf >> 16) & 63), spsm = static_cast<u32>((g_bitbltbuf >> 24) & 0x3F);
        const u32 dbp = static_cast<u32>((g_bitbltbuf >> 32) & 0x3FFF), dbw = static_cast<u32>((g_bitbltbuf >> 48) & 63), dpsm = static_cast<u32>((g_bitbltbuf >> 56) & 0x3F);
        const u32 sx = static_cast<u32>(g_trxpos & 0x7FF), sy = static_cast<u32>((g_trxpos >> 16) & 0x7FF);
        const u32 dx = static_cast<u32>((g_trxpos >> 32) & 0x7FF), dy = static_cast<u32>((g_trxpos >> 48) & 0x7FF);
        const u32 w = static_cast<u32>(g_trxreg & 0xFFF), h = static_cast<u32>((g_trxreg >> 32) & 0xFFF);
        ++g_stats.transfers;
        for (u32 y = 0; y < h; ++y)
          for (u32 x = 0; x < w; ++x) WritePixel(dpsm, dbp, dbw, dx + x, dy + y, ReadPixel(spsm, sbp, sbw, sx + x, sy + y));
      }
      break;
    case 0x60: {  // SIGNAL: SIGID = (SIGID & ~mask) | (id & mask); raises CSR.SIGNAL
      const u32 id = static_cast<u32>(v), mask = static_cast<u32>(v >> 32);
      g_siglblid = (g_siglblid & ~static_cast<u64>(0xFFFFFFFFu)) | ((static_cast<u32>(g_siglblid) & ~mask) | (id & mask));
      g_csr |= 1;
      if (!(g_imr & 0x100) && g_irqCallback) g_irqCallback();
      break;
    }
    case 0x61:  // FINISH: raises CSR.FINISH once everything before it is drawn (drawing here is immediate)
      g_csr |= 2;
      CsrTrace('F', g_imr);
      if (!(g_imr & 0x200) && g_irqCallback) g_irqCallback();
      break;
    case 0x62: g_siglblid = (g_siglblid & 0xFFFFFFFFull) | (v << 32); break;  // LABEL: LBLID
    case 0x54: {  // HWREG: 64 bits of image data
      u8 b[8];
      memcpy(b, &v, 8);
      ImageData(b, 8);
      break;
    }
    default: break;
  }
}

namespace {
void Merge(u64& reg, std::uint32_t address, u64 value, std::uint32_t size) {
  if (size >= 8) reg = value;
  else if (address & 4) reg = (reg & 0xFFFFFFFFull) | (value << 32);
  else reg = (reg & ~0xFFFFFFFFull) | (value & 0xFFFFFFFFull);
}
}  // namespace

// (0x1FF8: the event registers sit at 0x12001000-0x12001080; with 0xFF8 they aliased PMODE/DISPLAY1 and CSR read as 0,
// so a game polling CSR.FINISH, as the race start does, waited forever.)
void PrivWrite(std::uint32_t address, std::uint64_t value, std::uint32_t size) {
  switch (address & 0x1FF8) {
    case 0x000: Merge(g_pmode, address, value, size); break;
    case 0x020: Merge(g_smode2, address, value, size); break;
    case 0x070: Merge(g_dispfb[0], address, value, size); break;
    case 0x080: Merge(g_display[0], address, value, size); break;
    case 0x090: Merge(g_dispfb[1], address, value, size); break;
    case 0x0A0: Merge(g_display[1], address, value, size); break;
    case 0x1000: if (!(address & 4)) g_csr &= ~(value & 0x1F); CsrTrace('W', value); break;  // CSR: write 1 to clear the event bits (bit 8 RESET ignored)
    case 0x1010: Merge(g_imr, address, value, size); CsrTrace('M', g_imr); break;
    case 0x1080: Merge(g_siglblid, address, value, size); break;
    default: break;
  }
}

// Diagnostic: the last GS event-register activity (FINISH/SIGNAL writes, CSR/IMR writes and reads), repeats folded.
namespace {
struct CsrEvent { char kind; u64 value; u64 frame; u64 count; };
CsrEvent g_csrTrace[48] = {};
u64 g_csrPos = 0;
void CsrTrace(char kind, u64 value) {
  CsrEvent& last = g_csrTrace[(g_csrPos + 47) % 48];
  if (g_csrPos && last.kind == kind && last.value == value) { ++last.count; return; }
  g_csrTrace[g_csrPos % 48] = {kind, value, g_stats.frames, 1};
  ++g_csrPos;
}
}  // namespace
void SetPrimLog(bool on) { g_primLog = on; }
bool DumpPage(u32 page, u32 fbw, u32 w, u32 h, u32 psm, const char* path) {  // diagnostic: a GS-memory buffer as a BMP
  std::vector<u32> img(static_cast<std::size_t>(w) * h);
  for (u32 y = 0; y < h; ++y)
    for (u32 x = 0; x < w; ++x) {
      const u32 raw = ReadPixel(psm, page * 32, fbw, x, y);
      const Rgba px = (psm == CT16 || psm == CT16S) ? Unpack16(raw, 128, 0) : Unpack32(raw);
      img[static_cast<std::size_t>(y) * w + x] = static_cast<u32>(px.b) | (static_cast<u32>(px.g) << 8) | (static_cast<u32>(px.r) << 16);
    }
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f) return false;
  const u32 size = 54 + w * h * 4, off = 54, dib = 40;
  const i32 negH = -static_cast<i32>(h);
  u8 hdr[54] = {'B', 'M'};
  memcpy(hdr + 2, &size, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4); memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &negH, 4);
  hdr[26] = 1; hdr[28] = 32;
  fwrite(hdr, 1, 54, f);
  fwrite(img.data(), 4, img.size(), f);
  fclose(f);
  return true;
}
std::string PrimLogText() { return g_primLogText; }
std::string TargetsText() {  // diagnostic: the busiest frame buffers since the last call, and what the display shows
  std::vector<std::pair<u64, u32>> list;
  for (u32 i = 0; i < 512; ++i)
    if (const u64 n = g_targetPixels[i].exchange(0)) list.push_back({n, i});
  std::sort(list.begin(), list.end(), std::greater<>());
  std::string out;
  char one[64];
  for (std::size_t i = 0; i < list.size() && i < 4; ++i) {
    snprintf(one, sizeof one, " page %u: %llu px", list[i].second, static_cast<unsigned long long>(list[i].first));
    out += one;
  }
  snprintf(one, sizeof one, "; PMODE %llX DISPFB1 page %llu DISPFB2 page %llu", static_cast<unsigned long long>(g_pmode & 0xFFFF),
           static_cast<unsigned long long>(g_dispfb[0] & 0x1FF), static_cast<unsigned long long>(g_dispfb[1] & 0x1FF));
  return out + one;
}
std::string CsrTraceText() {
  std::string out;
  for (u64 i = g_csrPos > 48 ? g_csrPos - 48 : 0; i < g_csrPos; ++i) {
    const CsrEvent& e = g_csrTrace[i % 48];
    char line[96];
    snprintf(line, sizeof line, "  frame %llu %c %llX x%llu\n", static_cast<unsigned long long>(e.frame), e.kind,
             static_cast<unsigned long long>(e.value), static_cast<unsigned long long>(e.count));
    out += line;
  }
  return out;
}

std::uint64_t PrivRead(std::uint32_t address, std::uint32_t field) {
  switch (address & 0x1FF8) {
    case 0x1000: CsrTrace('R', g_csr & 0x1F); return (g_csr & 0x1F) | 0x4000 | (static_cast<u64>(field & 1) << 13) | (static_cast<u64>(0x1B) << 16);  // FIFO empty, field, revision
    case 0x1010: return g_imr;
    case 0x1080: return g_siglblid;
    default: return 0;
  }
}

void VBlankStart() { g_csr |= 8; }
void SetInterruptCallback(void (*callback)()) { g_irqCallback = callback; }

void Present() {
  if (g_mem.empty()) return;
  Ticks timer(g_stats.presentTicks);
  if (!g_windowThread.joinable()) g_windowThread = std::thread(WindowThread);
  // circuit 2 over circuit 1 when both are enabled (the game normally enables one); pick the first enabled
  const int c = (g_pmode & 1) ? 0 : (g_pmode & 2) ? 1 : -1;
  if (c < 0) return;
  const u64 fb = g_dispfb[c], dp = g_display[c];
  static const int forcedPage = [] { const char* e = getenv("HP2_FORCE_FBP"); return e ? atoi(e) : -1; }();  // diagnostic
  const u32 fbp = (forcedPage >= 0 ? static_cast<u32>(forcedPage) : static_cast<u32>(fb & 0x1FF)) * 32, fbw = static_cast<u32>((fb >> 9) & 63),
            psm = static_cast<u32>((fb >> 15) & 31);
  const u32 dbx = static_cast<u32>((fb >> 32) & 0x7FF), dby = static_cast<u32>((fb >> 43) & 0x7FF);
  const u32 magh = static_cast<u32>((dp >> 23) & 15), magv = static_cast<u32>((dp >> 27) & 3);
  const u32 dw = static_cast<u32>((dp >> 32) & 0xFFF) + 1, dh = static_cast<u32>((dp >> 44) & 0x7FF) + 1;
  const u32 w = std::clamp<u32>(dw / (magh + 1), 1, 1024), h = std::clamp<u32>(dh / (magv + 1), 1, 1024);
  std::vector<u32> img(static_cast<std::size_t>(w) * h);
  Pool().Run(0, static_cast<int>(h) - 1, [&](int yi) {  // display read-out, rows split between threads
    const u32 y = static_cast<u32>(yi);
    for (u32 x = 0; x < w; ++x) {
      const u32 raw = ReadPixel(psm, fbp, fbw, dbx + x, dby + y);
      Rgba px = (psm == CT16 || psm == CT16S) ? Unpack16(raw, 128, 0) : Unpack32(raw);
      img[y * w + x] = static_cast<u32>(px.b) | (static_cast<u32>(px.g) << 8) | (static_cast<u32>(px.r) << 16);
    }
  });
  {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    g_frame.swap(img);
    g_frameW = static_cast<int>(w);
    g_frameH = static_cast<int>(h);
    ++g_frameSeq;
  }
  ++g_stats.frames;
  if (g_d3dActive) SetEvent(g_frameEvent);
  else if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

bool DumpFrame(const char* path) {
  std::lock_guard<std::mutex> lock(g_frameMutex);
  if (g_frame.empty()) return false;
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f) return false;
  const u32 size = 54 + static_cast<u32>(g_frame.size()) * 4;
  u8 h[54] = {'B', 'M'};
  memcpy(h + 2, &size, 4);
  const u32 off = 54, dib = 40, w = static_cast<u32>(g_frameW), hh = static_cast<u32>(-g_frameH);
  memcpy(h + 10, &off, 4); memcpy(h + 14, &dib, 4); memcpy(h + 18, &w, 4); memcpy(h + 22, &hh, 4);
  h[26] = 1; h[28] = 32;
  fwrite(h, 1, 54, f);
  fwrite(g_frame.data(), 4, g_frame.size(), f);
  fclose(f);
  return true;
}
bool WindowClosed() { return g_closed; }
void Shutdown() {  // closes the window and waits for its thread (a running std::thread at exit would abort the process)
  if (!g_windowThread.joinable()) return;
  if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  g_windowThread.join();
}
bool WindowFocused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }
bool OverlayVisible() { return ui::Visible(); }
void RequestWindowShot(const char* path) {
  std::lock_guard<std::mutex> lock(g_frameMutex);
  g_windowShot = path;
}
void OpenSettings() {
  if (g_hwnd) PostMessageW(g_hwnd, kPadMessage, 0x20000, 0);
}
void OverlayPad(std::uint16_t pressed) {  // emulation thread: newly pressed buttons go to the window thread as a message
  static std::uint16_t previous = 0;
  const std::uint16_t edges = static_cast<std::uint16_t>(pressed & ~previous);
  previous = pressed;
  if (!edges || !g_hwnd) return;
  const bool toggle = (pressed & 0x0009) == 0x0009 && (edges & 0x0009);  // Select + Start
  PostMessageW(g_hwnd, kPadMessage, toggle ? 0x10000 : edges, 0);
}
const Stats& GetStats() {  // the per-pixel and texel counters are atomics (several threads draw)
  static Stats s;
  s = g_stats;
  s.pixels = g_pixelCount.load();
  s.textured = g_texturedCount.load();
  s.unsupported = g_stats.unsupported + g_unsupportedTexels.load();
  return s;
}
void SetWindowScale(int percent) { g_scalePercent = percent; }

}  // namespace gs
