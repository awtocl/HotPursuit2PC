#include "gs.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
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
std::vector<u32> g_pageOffset[8];
void BuildOffsets(Kind k) {
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
u64 g_pmode = 0, g_dispfb[2] = {0, 0}, g_display[2] = {0, 0}, g_smode2 = 0;

// ---- window ------------------------------------------------------------------------------------
std::mutex g_frameMutex;
std::vector<u32> g_frame;  // BGRA
int g_frameW = 640, g_frameH = 448;
std::atomic<bool> g_closed{false};
std::atomic<int> g_scalePercent{100};
HWND g_hwnd = nullptr;
std::thread g_windowThread;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CLOSE: g_closed = true; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
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
    default: return DefWindowProcW(h, m, w, l);
  }
}

void WindowThread() {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"HP2NativeWindow";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  RECT r = {0, 0, 640 * g_scalePercent / 100 * 1, 448 * g_scalePercent / 100 * 1};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  g_hwnd = CreateWindowW(L"HP2NativeWindow", L"Need for Speed: Hot Pursuit 2 - native runtime (development)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
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

Rgba ClutLookup(const Ctx& c, u32 index) {
  const u32 cbp = static_cast<u32>((c.tex0 >> 37) & 0x3FFF), cpsm = static_cast<u32>((c.tex0 >> 51) & 15), csa = static_cast<u32>((c.tex0 >> 56) & 31);
  const u32 psm = static_cast<u32>((c.tex0 >> 20) & 0x3F);
  const bool is4 = KindOf(psm) == Kind::I4;
  u32 i = index;
  const bool csm2 = ((c.tex0 >> 55) & 1) != 0;
  u32 x, y;
  if (csm2) {  // CSM2: a line of the buffer, linear
    x = (is4 ? csa * 16 : 0) + i;
    y = 0;
    const u32 bw = 1;
    const u32 v = ReadPixel(cpsm == 0 ? CT32 : CT16, cbp, bw, x & 15, 0);
    (void)y;
    return cpsm == 0 ? Unpack32(v) : Unpack16(v, 128, 0);
  }
  if (!is4) i = (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1);  // 8-bit CLUT: entry index bits 3 and 4 swap
  if (is4) i += csa * 16 & 0xFF;
  if (cpsm == 0) { x = i & 15; y = i >> 4; return Unpack32(ReadPixel(CT32, cbp, 1, x, y)); }
  x = i & 15; y = i >> 4;
  return Unpack16(ReadPixel(CT16, cbp, 1, x, y), 128, 0);
}

Rgba TexelAt(const Ctx& c, i32 tx, i32 ty) {
  const u32 tbp = static_cast<u32>(c.tex0 & 0x3FFF), tbw = static_cast<u32>((c.tex0 >> 14) & 63), psm = static_cast<u32>((c.tex0 >> 20) & 0x3F);
  const u32 tw = 1u << ((c.tex0 >> 26) & 15), th = 1u << ((c.tex0 >> 30) & 15);
  const u64 clamp = c.clamp;
  const u32 wms = static_cast<u32>(clamp & 3), wmt = static_cast<u32>((clamp >> 2) & 3);
  auto wrap = [](i32 v, u32 size, u32 mode, u32 minv, u32 maxv) -> u32 {
    switch (mode) {
      case 0: return static_cast<u32>(v) & (size - 1);
      case 1: return v < 0 ? 0 : (static_cast<u32>(v) >= size ? size - 1 : static_cast<u32>(v));
      case 2: return static_cast<u32>(std::clamp<i32>(v, static_cast<i32>(minv), static_cast<i32>(maxv)));
      default: return (static_cast<u32>(v) & minv) | maxv;
    }
  };
  const u32 x = wrap(tx, tw, wms, static_cast<u32>((clamp >> 4) & 0x3FF), static_cast<u32>((clamp >> 14) & 0x3FF));
  const u32 y = wrap(ty, th, wmt, static_cast<u32>((clamp >> 24) & 0x3FF), static_cast<u32>((clamp >> 34) & 0x3FF));
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
    case T8: t = ClutLookup(c, ReadPixel(T8, tbp, tbw, x, y)); break;
    case T4: t = ClutLookup(c, ReadPixel(T4, tbp, tbw, x, y)); break;
    case T8H: t = ClutLookup(c, ReadPixel(CT32, tbp, tbw, x, y) >> 24); break;
    case T4HL: t = ClutLookup(c, (ReadPixel(CT32, tbp, tbw, x, y) >> 24) & 15); break;
    case T4HH: t = ClutLookup(c, ReadPixel(CT32, tbp, tbw, x, y) >> 28); break;
    default: ++g_stats.unsupported; break;
  }
  return t;
}

void DrawTriangle(const Vertex& v0, const Vertex& v1_, const Vertex& v2_, bool flat) {
  const Ctx& c = g_ctx[(g_prim >> 9) & 1];
  const bool tme = (g_prim >> 4) & 1, abe = (g_prim >> 6) & 1, fst = (g_prim >> 8) & 1;
  const u32 fbp = static_cast<u32>(c.frame & 0x1FF) * 32, fbw = static_cast<u32>((c.frame >> 16) & 63), fpsm = static_cast<u32>((c.frame >> 24) & 0x3F);
  const u32 fbmsk = static_cast<u32>(c.frame >> 32);
  const u32 zbp = static_cast<u32>(c.zbuf & 0x1FF) * 32, zpsm = 0x30 | static_cast<u32>((c.zbuf >> 24) & 15);
  const bool zmsk = ((c.zbuf >> 32) & 1) != 0;
  const bool zte = ((c.test >> 16) & 1) != 0;
  const u32 ztst = static_cast<u32>((c.test >> 17) & 3);
  const bool ate = (c.test & 1) != 0;
  const u32 atst = static_cast<u32>((c.test >> 1) & 7), aref = static_cast<u32>((c.test >> 4) & 255);
  const int sx0 = static_cast<int>(c.scissor & 0x7FF), sx1 = static_cast<int>((c.scissor >> 16) & 0x7FF);
  const int sy0 = static_cast<int>((c.scissor >> 32) & 0x7FF), sy1 = static_cast<int>((c.scissor >> 48) & 0x7FF);
  const u32 tfx = static_cast<u32>((c.tex0 >> 35) & 3);
  const bool tcc = ((c.tex0 >> 34) & 1) != 0;
  const u32 tw = 1u << ((c.tex0 >> 26) & 15), th = 1u << ((c.tex0 >> 30) & 15);
  const bool linear = ((c.tex1 >> 5) & 1) != 0;

  Vertex a = v0, b = v1_, d = v2_;
  const i64 area = static_cast<i64>(b.x - a.x) * (d.y - a.y) - static_cast<i64>(b.y - a.y) * (d.x - a.x);
  if (area == 0) return;
  if (area < 0) std::swap(b, d);
  const i64 ar = area < 0 ? -area : area;
  const int minx = std::max((std::min({a.x, b.x, d.x}) >> 4), sx0), maxx = std::min((std::max({a.x, b.x, d.x}) + 15) >> 4, sx1);
  const int miny = std::max((std::min({a.y, b.y, d.y}) >> 4), sy0), maxy = std::min((std::max({a.y, b.y, d.y}) + 15) >> 4, sy1);
  auto bias = [](const Vertex& p, const Vertex& q) {  // top-left rule for edge p->q (counter-clockwise in y-down space after the swap)
    const i32 dx = q.x - p.x, dy = q.y - p.y;
    return (dy > 0 || (dy == 0 && dx < 0)) ? 0 : -1;
  };
  const int b01 = bias(a, b), b12 = bias(b, d), b20 = bias(d, a);
  for (int py = miny; py <= maxy; ++py)
    for (int px = minx; px <= maxx; ++px) {
      const i64 X = px * 16 + 8, Y = py * 16 + 8;
      const i64 w2 = (static_cast<i64>(b.x - a.x) * (Y - a.y) - static_cast<i64>(b.y - a.y) * (X - a.x)) + b01;  // weight of d
      const i64 w0 = (static_cast<i64>(d.x - b.x) * (Y - b.y) - static_cast<i64>(d.y - b.y) * (X - b.x)) + b12;  // weight of a
      const i64 w1 = (static_cast<i64>(a.x - d.x) * (Y - d.y) - static_cast<i64>(a.y - d.y) * (X - d.x)) + b20;  // weight of b
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      const double l0 = static_cast<double>(w0) / static_cast<double>(ar), l1 = static_cast<double>(w1) / static_cast<double>(ar), l2 = static_cast<double>(w2) / static_cast<double>(ar);
      const u32 z = static_cast<u32>(std::clamp(l0 * a.z + l1 * b.z + l2 * d.z, 0.0, 4294967295.0));
      // depth test
      if (zte && ztst != 1) {
        const u32 old = ReadPixel(zpsm, zbp, fbw, static_cast<u32>(px), static_cast<u32>(py));
        const u32 zv = (zpsm == Z24) ? (z >> 8) : (zpsm == Z32 ? z : (z >> 16) & 0xFFFF);
        if (ztst == 0) continue;                    // never
        if (ztst == 2 && zv < old) continue;        // GEQUAL
        if (ztst == 3 && zv <= old) continue;       // GREATER
      }
      Rgba col;
      if (flat) {
        col = {static_cast<int>(d.r), static_cast<int>(d.g), static_cast<int>(d.b), static_cast<int>(d.a)};
      } else {
        col = {static_cast<int>(l0 * a.r + l1 * b.r + l2 * d.r), static_cast<int>(l0 * a.g + l1 * b.g + l2 * d.g),
               static_cast<int>(l0 * a.b + l1 * b.b + l2 * d.b), static_cast<int>(l0 * a.a + l1 * b.a + l2 * d.a)};
      }
      if (tme) {
        double tu, tv;
        if (fst) {
          tu = (l0 * a.u + l1 * b.u + l2 * d.u) / 16.0;
          tv = (l0 * a.v + l1 * b.v + l2 * d.v) / 16.0;
        } else {
          const double q = l0 * a.q + l1 * b.q + l2 * d.q;
          const double s = l0 * a.s + l1 * b.s + l2 * d.s, t = l0 * a.t + l1 * b.t + l2 * d.t;
          tu = q != 0.0 ? s / q * tw : 0.0;
          tv = q != 0.0 ? t / q * th : 0.0;
        }
        Rgba tex;
        if (linear) {
          const double fx = tu - 0.5, fy = tv - 0.5;
          const i32 ix = static_cast<i32>(std::floor(fx)), iy = static_cast<i32>(std::floor(fy));
          const double ux = fx - ix, uy = fy - iy;
          const Rgba t00 = TexelAt(c, ix, iy), t10 = TexelAt(c, ix + 1, iy), t01 = TexelAt(c, ix, iy + 1), t11 = TexelAt(c, ix + 1, iy + 1);
          auto mix = [&](int p00, int p10, int p01, int p11) {
            return static_cast<int>((p00 * (1 - ux) + p10 * ux) * (1 - uy) + (p01 * (1 - ux) + p11 * ux) * uy);
          };
          tex = {mix(t00.r, t10.r, t01.r, t11.r), mix(t00.g, t10.g, t01.g, t11.g), mix(t00.b, t10.b, t01.b, t11.b), mix(t00.a, t10.a, t01.a, t11.a)};
        } else {
          tex = TexelAt(c, static_cast<i32>(std::floor(tu)), static_cast<i32>(std::floor(tv)));
        }
        ++g_stats.textured;
        switch (tfx) {
          case 0: col = {Clamp8(tex.r * col.r >> 7), Clamp8(tex.g * col.g >> 7), Clamp8(tex.b * col.b >> 7), tcc ? Clamp8(tex.a * col.a >> 7) : col.a}; break;
          case 1: col = {tex.r, tex.g, tex.b, tcc ? tex.a : col.a}; break;
          default: col = {Clamp8((tex.r * col.r >> 7) + col.a), Clamp8((tex.g * col.g >> 7) + col.a), Clamp8((tex.b * col.b >> 7) + col.a), tcc ? Clamp8(tex.a + col.a) : col.a}; break;
        }
      }
      col = {Clamp8(col.r), Clamp8(col.g), Clamp8(col.b), Clamp8(col.a)};
      // alpha test
      if (ate) {
        const u32 av = static_cast<u32>(col.a);
        bool pass = true;
        switch (atst) {
          case 0: pass = false; break;
          case 2: pass = av < aref; break;
          case 3: pass = av <= aref; break;
          case 4: pass = av == aref; break;
          case 5: pass = av >= aref; break;
          case 6: pass = av > aref; break;
          case 7: pass = av != aref; break;
          default: break;
        }
        if (!pass) {
          const u32 afail = static_cast<u32>((c.test >> 12) & 3);
          if (afail == 0) continue;  // KEEP
          // other fail modes (FB only, ZB only, RGB only): the frame is left alone for now
          continue;
        }
      }
      // blend
      if (abe && (!((g_pabe) & 1) || col.a & 0x80)) {
        const Rgba dst = fpsm == CT16 || fpsm == CT16S ? Unpack16(ReadPixel(fpsm, fbp, fbw, static_cast<u32>(px), static_cast<u32>(py)), 128, 0)
                                                        : Unpack32(ReadPixel(fpsm, fbp, fbw, static_cast<u32>(px), static_cast<u32>(py)));
        const u32 A = static_cast<u32>(c.alpha & 3), B = static_cast<u32>((c.alpha >> 2) & 3), C = static_cast<u32>((c.alpha >> 4) & 3), D = static_cast<u32>((c.alpha >> 6) & 3);
        const int fix = static_cast<int>((c.alpha >> 32) & 255);
        auto pick = [&](u32 sel, int cs, int cd) { return sel == 0 ? cs : sel == 1 ? cd : 0; };
        const int ac = C == 0 ? col.a : C == 1 ? dst.a : fix;
        auto channel = [&](int cs, int cd) { return Clamp8(((pick(A, cs, cd) - pick(B, cs, cd)) * ac >> 7) + pick(D, cs, cd)); };
        col = {channel(col.r, dst.r), channel(col.g, dst.g), channel(col.b, dst.b), col.a};
      }
      u32 out;
      if (fpsm == CT16 || fpsm == CT16S)
        out = static_cast<u32>((col.r >> 3) | ((col.g >> 3) << 5) | ((col.b >> 3) << 10) | ((col.a & 0x80) << 8));
      else
        out = Pack32(col);
      WritePixel(fpsm, fbp, fbw, static_cast<u32>(px), static_cast<u32>(py), out, ~fbmsk);
      if (!zmsk && zte) {
        const u32 zv = (zpsm == Z24) ? (z >> 8) : (zpsm == Z32 ? z : (z >> 16) & 0xFFFF);
        WritePixel(zpsm, zbp, fbw, static_cast<u32>(px), static_cast<u32>(py), zv);
      }
      ++g_stats.pixels;
    }
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

void Kick(bool hasF, u64 xyz) {
  const u32 type = static_cast<u32>(g_prim & 7);
  const bool flat = ((g_prim >> 3) & 1) == 0;
  Vertex v = MakeVertex(xyz, hasF);
  switch (type) {
    case 0: ++g_stats.points; break;
    case 1: case 2: ++g_stats.lines; break;  // lines are not drawn yet
    case 3:  // triangle list
      g_queue[g_queued++] = v;
      if (g_queued == 3) { DrawTriangle(g_queue[0], g_queue[1], g_queue[2], flat); ++g_stats.triangles; g_queued = 0; }
      break;
    case 4:  // strip
      if (g_queued < 3) g_queue[g_queued++] = v;
      else { g_queue[0] = g_queue[1]; g_queue[1] = g_queue[2]; g_queue[2] = v; }
      if (g_queued == 3) { DrawTriangle(g_queue[0], g_queue[1], g_queue[2], flat); ++g_stats.triangles; }
      break;
    case 5:  // fan
      if (g_queued < 3) g_queue[g_queued++] = v;
      else { g_queue[1] = g_queue[2]; g_queue[2] = v; }
      if (g_queued == 3) { DrawTriangle(g_queue[0], g_queue[1], g_queue[2], flat); ++g_stats.triangles; }
      break;
    case 6: {  // sprite: two corners, flat colour of the second vertex
      g_queue[g_queued++] = v;
      if (g_queued == 2) {
        Vertex a = g_queue[0], b = g_queue[1];
        a.r = b.r; a.g = b.g; a.b = b.b; a.a = b.a;
        Vertex c = b, d = a;
        c.x = b.x; c.y = a.y; c.s = b.s; c.t = a.t; c.q = b.q; c.u = b.u; c.v = a.v; c.z = b.z;
        d.x = a.x; d.y = b.y; d.s = a.s; d.t = b.t; d.q = a.q; d.u = a.u; d.v = b.v; d.z = b.z;
        a.z = b.z;
        DrawTriangle(a, c, b, true);
        DrawTriangle(a, b, d, true);
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
    case 0x04: case 0x0C: Kick(true, v); break;
    case 0x05: case 0x0D: Kick(false, v); break;
    case 0x06: g_ctx[0].tex0 = v; break;
    case 0x07: g_ctx[1].tex0 = v; break;
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
    case 0x54: {  // HWREG: 64 bits of image data
      u8 b[8];
      memcpy(b, &v, 8);
      ImageData(b, 8);
      break;
    }
    default: break;
  }
}

void PrivWrite(std::uint32_t address, std::uint64_t value) {
  switch (address & 0xFFF) {
    case 0x000: g_pmode = value; break;
    case 0x020: g_smode2 = value; break;
    case 0x070: g_dispfb[0] = value; break;
    case 0x080: g_display[0] = value; break;
    case 0x090: g_dispfb[1] = value; break;
    case 0x0A0: g_display[1] = value; break;
    default: break;
  }
}

void Present() {
  if (g_mem.empty()) return;
  if (!g_windowThread.joinable()) g_windowThread = std::thread(WindowThread);
  // circuit 2 over circuit 1 when both are enabled (the game normally enables one); pick the first enabled
  const int c = (g_pmode & 1) ? 0 : (g_pmode & 2) ? 1 : -1;
  if (c < 0) return;
  const u64 fb = g_dispfb[c], dp = g_display[c];
  const u32 fbp = static_cast<u32>(fb & 0x1FF) * 32, fbw = static_cast<u32>((fb >> 9) & 63), psm = static_cast<u32>((fb >> 15) & 31);
  const u32 dbx = static_cast<u32>((fb >> 32) & 0x7FF), dby = static_cast<u32>((fb >> 43) & 0x7FF);
  const u32 magh = static_cast<u32>((dp >> 23) & 15), magv = static_cast<u32>((dp >> 27) & 3);
  const u32 dw = static_cast<u32>((dp >> 32) & 0xFFF) + 1, dh = static_cast<u32>((dp >> 44) & 0x7FF) + 1;
  const u32 w = std::clamp<u32>(dw / (magh + 1), 1, 1024), h = std::clamp<u32>(dh / (magv + 1), 1, 1024);
  std::vector<u32> img(static_cast<std::size_t>(w) * h);
  for (u32 y = 0; y < h; ++y)
    for (u32 x = 0; x < w; ++x) {
      const u32 raw = ReadPixel(psm, fbp, fbw, dbx + x, dby + y);
      Rgba px = (psm == CT16 || psm == CT16S) ? Unpack16(raw, 128, 0) : Unpack32(raw);
      img[y * w + x] = static_cast<u32>(px.b) | (static_cast<u32>(px.g) << 8) | (static_cast<u32>(px.r) << 16);
    }
  {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    g_frame.swap(img);
    g_frameW = static_cast<int>(w);
    g_frameH = static_cast<int>(h);
  }
  ++g_stats.frames;
  if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
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
const Stats& GetStats() { return g_stats; }
void SetWindowScale(int percent) { g_scalePercent = percent; }

}  // namespace gs
