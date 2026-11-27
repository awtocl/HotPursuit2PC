// Native settings overlay (ADR 0013). Drawn in Hot Pursuit 2's own front-end style (hp2_style, shared with the launcher):
// a blueprint-grid card with the steel title strip, the outlined italic title, the red and white rules, menu rows with
// the selection in orange and underlined, and a steel ticker with the keys.
#include "overlay.h"

#include "hp2_style.h"
#include "i18n.h"

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
    {L"antialiasing", L"Anti-aliasing", &Settings::antialias, 2, {L"Off", L"FXAA"}},
    {L"controller_layout", L"Controller layout", &Settings::padLayout, 3, {L"Classic (PS2)", L"Modern (triggers)", L"Custom"}},
    {L"language", L"Language", &Settings::language, 6, {L"English", L"Español", L"Português (Brasil)", L"Français", L"Deutsch", L"Italiano"}},
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
  if (GetPrivateProfileIntW(L"graphics", L"language", -1, ini.c_str()) == static_cast<UINT>(-1)) s.language = i18n::Detect();
  i18n::Set(s.language);
  if (const char* e = std::getenv("HP2_ASPECT")) s.aspect = std::clamp(std::atoi(e), 0, 3);  // tests: without touching the file
  if (const char* e = std::getenv("HP2_FPS")) s.frameRate = std::atoi(e) >= 60 ? 1 : 0;
  if (const char* e = std::getenv("HP2_AA")) s.antialias = std::clamp(std::atoi(e), 0, 2);  // 2: right half only
  if (const char* e = std::getenv("HP2_RES")) s.resolution = std::clamp(std::atoi(e), 0, 5);  // 0 1x ... 5 8x
  if (const char* e = std::getenv("HP2_VSYNC")) s.vsync = std::atoi(e) ? 1 : 0;
  return s;
}

void Save(const Settings& s) {
  i18n::Set(s.language);
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
  using namespace Gdiplus;
  namespace st = hp2style;
  st::Startup();
  out.assign(static_cast<std::size_t>(width) * height, 0);
  {
    // premultiplied BGRA straight into `out`
    Bitmap bmp(width, height, width * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(out.data()));
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(Color(0x90, 0, 0, 0));  // the game picture, dimmed
    // a 1280x720 design space, centred and scaled to the window
    const float k = std::min(height / 720.0f, width / 1280.0f);
    g.TranslateTransform((width - 1280 * k) / 2, (height - 720 * k) / 2);
    g.ScaleTransform(k, k);

    const float x0 = 290, x1 = 990, y0 = 104, rowH = 34, gap = 10;
    const float listY = 206, y1 = listY + rowH * kRows + gap + 52;
    {
      const GraphicsState clip = g.Save();
      g.SetClip(RectF(x0, y0, x1 - x0, y1 - y0));
      st::Grid(g, RectF(x0, y0, x1 - x0, y1 - y0), 34, 0.94f);
      g.Restore(clip);
    }
    st::SteelStrip(g, RectF(x0, 136, x1 - x0, 42));
    st::Title(g, L"Settings", x0 + 28, 116, 56);
    st::Rules(g, x0, x1, 184, x0 + 16);

    for (int i = 0; i < kRows; ++i) {
      const float y = listY + i * rowH + (i == kItemCount ? gap : 0);
      const RectF row(x0 + 16, y, x1 - x0 - 32, rowH - 4);
      const bool sel = i == g_selected;
      st::Row(g, row, sel);
      const RectF label(row.X + 16, y, 300, row.Height), value(row.X + 300, y, row.Width - 316, row.Height);
      if (i == kItemCount) {  // Quit game
        st::Item(g, L"Quit Game", label, 23, sel);  // (the st:: functions translate)
        st::Item(g, g_quitArmed && sel ? L"Press again to quit" : L"Exit to Windows", value, 21, g_quitArmed && sel, st::Align::Right);
        continue;
      }
      st::Item(g, kItems[i].label, label, 23, sel);
      std::wstring name = i18n::T(kItems[i].names[s.*kItems[i].field]);
      if (kItems[i].field == &Settings::aspect && (s.aspect == 0) != g_wideActive) name += std::wstring(L" ") + i18n::T(L"(restart)");  // 16:9 <-> 4:3
      st::Item(g, sel ? L"\u2039  " + name + L"  \u203A" : name, value, 21, sel, st::Align::Right);
    }
    st::SteelStrip(g, RectF(x0, y1 - 38, x1 - x0, 30));
    st::Outlined(g, L"\u2191\u2193 Select    \u2190\u2192 / Enter / \u2715 Change    F1 / Esc / Select+Start Close",
                 RectF(x0 + 20, y1 - 38, x1 - x0 - 40, 30), st::ItemFamily(), FontStyleBold, 17, st::col::silver, st::Align::Center,
                 0.2f, 2.2f);
  }
  return true;
}

}  // namespace ui
