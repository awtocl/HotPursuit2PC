// Native settings overlay (ADR 0013). Visual language of Hot Pursuit 2's menus: steel-blue translucent panels with angled
// edges, light grey bold italic items, the selected item in orange with an angled highlight, an orange accent stripe.
#include "overlay.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

namespace ui {
namespace {

std::atomic<bool> g_visible{false};
int g_selected = 0;
bool g_quitArmed = false;           // the Quit row asked for its confirmation press
std::atomic<bool> g_quit{false};    // quit confirmed (TakeQuit)
std::atomic<bool> g_wideActive{false};  // the game runs with its widescreen patches (chosen at start)

using Item = SettingInfo;
const Item kItems[] = {
    {L"renderer", L"Renderer", &Settings::renderer, 2, {L"Software", L"GPU"}},
    {L"resolution", L"Internal resolution", &Settings::resolution, 6, {L"1x (native)", L"2x", L"3x", L"4x", L"6x", L"8x"}},
    {L"framerate", L"Frame rate", &Settings::frameRate, 2, {L"30 (original)", L"60"}},
    {L"fullscreen", L"Display mode", &Settings::fullscreen, 2, {L"Window", L"Full screen"}},
    {L"vsync", L"V-Sync", &Settings::vsync, 2, {L"Off", L"On"}},
    {L"filter", L"Scaling filter", &Settings::filter, 3, {L"Sharp bilinear", L"Bilinear", L"Nearest"}},
    {L"aspect_ratio", L"Aspect ratio", &Settings::aspect, 4, {L"16:9", L"4:3", L"4:3 integer scale", L"Stretch"}},
    {L"sharpen", L"Sharpening", &Settings::sharpen, 4, {L"Off", L"Low", L"Medium", L"High"}},
    {L"scanlines", L"Scanlines", &Settings::scanlines, 3, {L"Off", L"Light", L"Strong"}},
};
constexpr int kItemCount = static_cast<int>(sizeof kItems / sizeof kItems[0]);
constexpr int kRows = kItemCount + 1;  // the settings, then Quit game

std::wstring IniPath() {
  wchar_t path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path, n);
  const auto slash = p.find_last_of(L"\\/");
  return (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) + L"hp2native.ini";
}

const wchar_t* Key(int i) { return kItems[i].key; }

// GDI helpers. Every drawn colour keeps at least one non-zero channel: untouched pixels (0) become the dimmed backdrop.
COLORREF Rgb(int r, int g, int b) { return RGB(std::max(r, 1), g, b); }

void FillPoly(HDC dc, const POINT* p, int n, COLORREF fill) {
  HBRUSH br = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, 1, fill);
  HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
  Polygon(dc, p, n);
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(br);
  DeleteObject(pen);
}
void Parallelogram(HDC dc, int x, int y, int w, int h, int skew, COLORREF fill) {
  const POINT p[4] = {{x + skew, y}, {x + w + skew, y}, {x + w, y + h}, {x, y + h}};
  FillPoly(dc, p, 4, fill);
}
void Text(HDC dc, HFONT font, int x, int y, int w, int h, const wchar_t* s, COLORREF c, UINT align) {
  HGDIOBJ of = SelectObject(dc, font);
  SetTextColor(dc, c);
  RECT r = {x, y, x + w, y + h};
  DrawTextW(dc, s, -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
  SelectObject(dc, of);
}
HFONT Font(int height, int weight, const wchar_t* face) {
  return CreateFontW(-height, 0, 0, 0, weight, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                     ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

}  // namespace

int SettingCount() { return kItemCount; }
const SettingInfo& Setting(int i) { return kItems[i]; }

Settings Load() {
  Settings s;
  const std::wstring ini = IniPath();
  for (int i = 0; i < kItemCount; ++i) {
    const int v = static_cast<int>(GetPrivateProfileIntW(L"graphics", Key(i), s.*kItems[i].field, ini.c_str()));
    s.*kItems[i].field = std::clamp(v, 0, kItems[i].count - 1);
  }
  if (const char* e = std::getenv("HP2_ASPECT")) s.aspect = std::clamp(std::atoi(e), 0, 3);  // tests: without touching the file
  if (const char* e = std::getenv("HP2_FPS")) s.frameRate = std::atoi(e) >= 60 ? 1 : 0;
  return s;
}

void Save(const Settings& s) {
  const std::wstring ini = IniPath();
  for (int i = 0; i < kItemCount; ++i)
    WritePrivateProfileStringW(L"graphics", Key(i), std::to_wstring(s.*kItems[i].field).c_str(), ini.c_str());
}

bool Visible() { return g_visible; }
void Toggle() {
  g_visible = !g_visible;
  g_quitArmed = false;
}

bool TakeQuit() { return g_quit.exchange(false); }
void SetWidescreenActive(bool wide) { g_wideActive = wide; }

bool Navigate(Input in, Settings& s) {
  if (!g_visible) return false;
  if (in != Input::Accept) g_quitArmed = false;
  if (g_selected == kItemCount && in != Input::Up && in != Input::Down && in != Input::Back) {  // Quit game: Enter twice
    if (in == Input::Accept) {
      if (g_quitArmed) g_quit = true;
      g_quitArmed = !g_quitArmed;
    }
    return false;
  }
  if (in == Input::Back) {
    g_visible = false;
    return false;
  }
  if (in == Input::Up || in == Input::Down) {
    g_selected = (g_selected + (in == Input::Up ? kRows - 1 : 1)) % kRows;
    return false;
  }
  int& v = s.*kItems[g_selected].field;
  const int n = kItems[g_selected].count;
  switch (in) {
    case Input::Up: case Input::Down: case Input::Back: return false;
    case Input::Left: v = (v + n - 1) % n; return true;
    case Input::Right:
    case Input::Accept: v = (v + 1) % n; return true;
  }
  return false;
}

bool Render(const Settings& s, int width, int height, std::vector<std::uint32_t>& out) {
  if (!g_visible || width < 64 || height < 64) return false;
  out.assign(static_cast<std::size_t>(width) * height, 0);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) {
    DeleteDC(dc);
    return false;
  }
  HGDIOBJ old = SelectObject(dc, bmp);
  SetBkMode(dc, TRANSPARENT);

  // layout in a 1280x720 design space scaled to the client height
  const double k = std::min(height / 720.0, width / 1280.0);
  auto S = [k](int v) { return static_cast<int>(v * k + 0.5); };
  const int pw = S(760), rowH = S(52), headH = S(78), skew = S(26);
  const int gap = S(14), ph = headH + S(24) + rowH * kRows + gap + S(70);
  const int px = (width - pw) / 2, py = (height - ph) / 2;

  // panel: dark steel body, lighter rim, header band with the orange accent stripe
  Parallelogram(dc, px - S(4), py - S(4), pw + S(8), ph + S(8), skew, Rgb(96, 122, 156));
  Parallelogram(dc, px, py, pw, ph, skew, Rgb(16, 26, 42));
  auto edge = [&](int y) { return px + skew * (py + ph - y) / ph; };  // left edge of the slanted panel at height y
  Parallelogram(dc, edge(py + headH), py, pw, headH, skew * headH / ph, Rgb(34, 62, 104));
  Parallelogram(dc, edge(py + headH) + S(6), py + headH - S(6), pw - S(12), S(6), 1, Rgb(255, 146, 28));

  HFONT title = Font(S(40), FW_BLACK, L"Arial Black"), item = Font(S(27), FW_BOLD, L"Arial"),
        small = Font(S(19), FW_BOLD, L"Arial");
  Text(dc, title, edge(py + headH / 2) + S(36), py, pw - S(88), headH - S(6), L"SETTINGS", Rgb(236, 240, 246), DT_LEFT);

  const int listY = py + headH + S(24);
  for (int i = 0; i < kRows; ++i) {
    const int y = listY + i * rowH + (i == kItemCount ? gap : 0), x = edge(y + rowH / 2);
    const bool sel = i == g_selected;
    if (sel) {
      Parallelogram(dc, x + S(18), y + S(4), pw - S(40), rowH - S(8), skew * (rowH - S(8)) / ph, Rgb(46, 78, 120));
      const POINT chevron[3] = {{x + S(28), y + S(14)}, {x + S(42), y + rowH / 2}, {x + S(28), y + rowH - S(14)}};
      FillPoly(dc, chevron, 3, Rgb(255, 146, 28));
    }
    const COLORREF label = sel ? Rgb(255, 156, 40) : Rgb(204, 210, 220);
    if (i == kItemCount) {  // Quit game
      Parallelogram(dc, x + S(18), y - gap / 2 - S(1), pw - S(40), S(2), 1, Rgb(60, 84, 116));
      Text(dc, item, x + S(56), y, pw / 2, rowH, L"Quit game", label, DT_LEFT);
      Text(dc, item, x + pw / 2, y, pw / 2 - S(48), rowH, g_quitArmed && sel ? L"Press again to quit" : L"Exit to Windows",
           g_quitArmed && sel ? Rgb(255, 214, 150) : Rgb(150, 170, 196), DT_RIGHT);
      continue;
    }
    Text(dc, item, x + S(56), y, pw / 2, rowH, kItems[i].label, label, DT_LEFT);
    std::wstring name = kItems[i].names[s.*kItems[i].field];
    if (kItems[i].field == &Settings::aspect && (s.aspect == 0) != g_wideActive) name += L" (restart)";  // 16:9 <-> 4:3
    const std::wstring value = sel ? L"‹  " + name + L"  ›" : name;
    Text(dc, item, x + pw / 2, y, pw / 2 - S(48), rowH, value.c_str(), sel ? Rgb(255, 214, 150) : Rgb(150, 170, 196), DT_RIGHT);
  }
  Text(dc, small, edge(py + ph - S(38)) + S(30), py + ph - S(58), pw - S(80), S(40),
       L"↑↓ Select    ←→ / Enter / ✕ Change    F1 / Esc / Select+Start Close", Rgb(132, 150, 176), DT_CENTER);

  GdiFlush();
  const std::uint32_t* src = static_cast<const std::uint32_t*>(bits);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t p = src[i] & 0xFFFFFF;
    if (p == 0) {
      out[i] = 0x60000000u;  // dimmed game picture around the panel (premultiplied black, alpha 0x60)
    } else {
      const std::uint32_t a = 0xEE;
      const std::uint32_t b = (p & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, r = ((p >> 16) & 255) * a / 255;
      out[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteObject(title);
  DeleteObject(item);
  DeleteObject(small);
  DeleteDC(dc);
  return true;
}

}  // namespace ui
