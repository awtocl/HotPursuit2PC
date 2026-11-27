// Controls of the native game (ADR 0017). See controls.h.
#include "controls.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <type_traits>

namespace ctl {
namespace {

// XINPUT_GAMEPAD button bits.
constexpr std::uint16_t kXUp = 0x0001, kXDown = 0x0002, kXLeft = 0x0004, kXRight = 0x0008, kXStart = 0x0010, kXBack = 0x0020,
                        kXLS = 0x0040, kXRS = 0x0080, kXLB = 0x0100, kXRB = 0x0200, kXA = 0x1000, kXB = 0x2000, kXX = 0x4000,
                        kXY = 0x8000;
constexpr int kStickHalf = 16000;

struct ActionInfo {
  const wchar_t* key;   // ini key
  const wchar_t* name;  // the game's name (Configuration A)
  const wchar_t* ps2;
  std::uint16_t bit;    // PS2 controller bit (libpad order: 1 select, 2 L3, 4 R3, 8 start, 10 up, 20 right, 40 down,
                        // 80 left, 100 L2, 200 R2, 400 L1, 800 R1, 1000 triangle, 2000 circle, 4000 cross, 8000 square)
};
const ActionInfo kActions[kActionCount] = {
    {L"accelerate", L"Accelerate", L"✕", 0x4000},
    {L"brake", L"Brake / Reverse", L"□", 0x8000},
    {L"steer_left", L"Steer left", L"←", 0x0080},
    {L"steer_right", L"Steer right", L"→", 0x0020},
    {L"handbrake", L"Handbrake", L"R1", 0x0800},
    {L"horn", L"Horn / Siren", L"○", 0x2000},
    {L"view", L"Change view", L"△", 0x1000},
    {L"look_back", L"Look back", L"L1", 0x0400},
    {L"boost", L"Speed boost", L"R3", 0x0004},
    {L"shift_up", L"Shift up / Zone Zoom", L"R2", 0x0200},
    {L"shift_down", L"Shift down / Zone Freeze", L"L2", 0x0100},
    {L"pause", L"Pause", L"Start", 0x0008},
    {L"cycle_hud", L"Cycle HUD", L"Select", 0x0001},
    {L"menu_up", L"Menu up", L"↑", 0x0010},
    {L"menu_down", L"Menu down", L"↓", 0x0040},
};

const wchar_t* kSourceNames[kSourceCount] = {
    L"-",      L"A",      L"B",       L"X",       L"Y",       L"LB",      L"RB",      L"LT",      L"RT",
    L"LS",     L"RS",     L"Back",    L"Start",   L"D-pad ↑", L"D-pad ↓", L"D-pad ←", L"D-pad →",
    L"LS ←", L"LS →", L"LS ↑", L"LS ↓", L"RS ←", L"RS →", L"RS ↑", L"RS ↓"};

Layout MakeDefaultKeys() {
  Layout l;
  auto set = [&](int a, std::uint16_t k1, std::uint16_t k2 = 0) { l.keys[a][0] = k1; l.keys[a][1] = k2; };
  set(kAccelerate, 'Z');
  set(kBrake, 'A');
  set(kSteerLeft, VK_LEFT);
  set(kSteerRight, VK_RIGHT);
  set(kHandbrake, 'W');
  set(kHorn, 'X');
  set(kView, 'S');
  set(kLookBack, 'Q');
  set(kBoost, 'E');
  set(kShiftUp, '2');
  set(kShiftDown, '1');
  set(kPause, VK_RETURN, VK_SPACE);
  set(kCycleHud, VK_BACK, VK_RSHIFT);
  set(kMenuUp, VK_UP);
  set(kMenuDown, VK_DOWN);
  return l;
}
// The PS2 layout on an Xbox controller, button for button (as the game shipped).
Layout MakeClassic() {
  Layout l;
  const std::uint8_t pad[kActionCount] = {kA, kX, kDLeft, kDRight, kRB, kB, kY, kLB, kRS, kRT, kLT, kStart, kBack, kDUp, kDDown};
  for (int a = 0; a < kActionCount; ++a) l.pad[a][0] = pad[a];
  return l;
}
// Today's racing games: throttle and brake on the triggers (A also accelerates, as it selects in the menus), handbrake
// on B, speed boost on X, shifting on the bumpers, horn on the left stick's click, look back on the right stick's click.
Layout MakeModern() {
  Layout l;
  const std::uint8_t pad[kActionCount][2] = {{kRT, kA}, {kLT}, {kDLeft}, {kDRight}, {kB}, {kLS}, {kY}, {kRS}, {kX},
                                             {kRB}, {kLB}, {kStart}, {kBack}, {kDUp}, {kDDown}};
  for (int a = 0; a < kActionCount; ++a) { l.pad[a][0] = pad[a][0]; l.pad[a][1] = pad[a][1]; }
  return l;
}

std::wstring ReadIni(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
  wchar_t buf[256] = {};
  GetPrivateProfileStringW(section, key, L"", buf, 256, ini.c_str());
  return buf;
}
int ReadInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
  return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
}

}  // namespace

std::atomic<int> g_activePad{-1};
void SetActivePadLayout(int layout) { g_activePad = std::clamp(layout, 0, 2); }
int ActivePadLayout() { return g_activePad; }

const Layout& DefaultKeys() { static const Layout l = MakeDefaultKeys(); return l; }
const Layout& ClassicPad() { static const Layout l = MakeClassic(); return l; }
const Layout& ModernPad() { static const Layout l = MakeModern(); return l; }

const std::uint8_t (*Config::Pad() const)[kBindings] {
  return padLayout == kModern ? ModernPad().pad : padLayout == kPadCustom ? customPad.pad : ClassicPad().pad;
}
const std::uint16_t (*Config::Keys() const)[kBindings] {
  return keyLayout == kKeyCustom ? customKeys.keys : DefaultKeys().keys;
}

Config Load(const std::wstring& ini) {
  Config c;
  // the controller layout is also an item of the in-game settings menu, so it lives with the others in [graphics]
  c.padLayout = std::clamp(ReadInt(ini, L"graphics", L"controller_layout", kClassic), 0, 2);
  c.keyLayout = std::clamp(ReadInt(ini, L"controls", L"keyboard_layout", kKeyDefault), 0, 1);
  c.customPad = ClassicPad();
  c.customKeys = DefaultKeys();
  auto pair = [](const std::wstring& v, int max, auto* out) {  // "n" or "n,n"
    out[0] = static_cast<std::remove_reference_t<decltype(out[0])>>(std::clamp(_wtoi(v.c_str()), 0, max));
    const auto comma = v.find(L',');
    out[1] = static_cast<std::remove_reference_t<decltype(out[0])>>(comma == std::wstring::npos ? 0 : std::clamp(_wtoi(v.c_str() + comma + 1), 0, max));
  };
  for (int a = 0; a < kActionCount; ++a) {
    const std::wstring p = ReadIni(ini, L"controller_custom", kActions[a].key);
    if (!p.empty()) pair(p, kSourceCount - 1, c.customPad.pad[a]);
    const std::wstring k = ReadIni(ini, L"keyboard_custom", kActions[a].key);
    if (!k.empty()) pair(k, 255, c.customKeys.keys[a]);
  }
  return c;
}

void Save(const std::wstring& ini, const Config& c) {
  WritePrivateProfileStringW(L"graphics", L"controller_layout", std::to_wstring(c.padLayout).c_str(), ini.c_str());
  WritePrivateProfileStringW(L"controls", L"keyboard_layout", std::to_wstring(c.keyLayout).c_str(), ini.c_str());
  for (int a = 0; a < kActionCount; ++a) {
    auto pair = [](unsigned x, unsigned y) { return std::to_wstring(x) + (y ? L"," + std::to_wstring(y) : L""); };
    WritePrivateProfileStringW(L"controller_custom", kActions[a].key, pair(c.customPad.pad[a][0], c.customPad.pad[a][1]).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"keyboard_custom", kActions[a].key, pair(c.customKeys.keys[a][0], c.customKeys.keys[a][1]).c_str(), ini.c_str());
  }
}

std::uint16_t Ps2Bit(int action) { return action >= 0 && action < kActionCount ? kActions[action].bit : 0; }
const wchar_t* ActionName(int action) { return kActions[action].name; }
const wchar_t* Ps2Button(int action) { return kActions[action].ps2; }
const wchar_t* SourceName(int source) { return source >= 0 && source < kSourceCount ? kSourceNames[source] : L"?"; }

std::wstring KeyName(std::uint16_t vk) {
  if (!vk) return L"-";
  switch (vk) {  // names Windows gives badly or in the system's language
    case VK_LEFT: return L"←";
    case VK_RIGHT: return L"→";
    case VK_UP: return L"↑";
    case VK_DOWN: return L"↓";
    case VK_RETURN: return L"Enter";
    case VK_SPACE: return L"Space";
    case VK_BACK: return L"Backspace";
    case VK_RSHIFT: return L"Right Shift";
    case VK_LSHIFT: return L"Left Shift";
    case VK_RCONTROL: return L"Right Ctrl";
    case VK_LCONTROL: return L"Left Ctrl";
    case VK_TAB: return L"Tab";
    default: break;
  }
  if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, static_cast<wchar_t>(vk));
  wchar_t name[64] = {};
  const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
  if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return name;
  return L"Key " + std::to_wstring(vk);
}

bool Pressed(const PadState& p, int s) {
  switch (s) {
    case kA: return p.buttons & kXA;
    case kB: return p.buttons & kXB;
    case kX: return p.buttons & kXX;
    case kY: return p.buttons & kXY;
    case kLB: return p.buttons & kXLB;
    case kRB: return p.buttons & kXRB;
    case kLT: return p.lt > 100;
    case kRT: return p.rt > 100;
    case kLS: return p.buttons & kXLS;
    case kRS: return p.buttons & kXRS;
    case kBack: return p.buttons & kXBack;
    case kStart: return p.buttons & kXStart;
    case kDUp: return p.buttons & kXUp;
    case kDDown: return p.buttons & kXDown;
    case kDLeft: return p.buttons & kXLeft;
    case kDRight: return p.buttons & kXRight;
    case kLStickLeft: return p.lx < -kStickHalf;
    case kLStickRight: return p.lx > kStickHalf;
    case kLStickUp: return p.ly > kStickHalf;
    case kLStickDown: return p.ly < -kStickHalf;
    case kRStickLeft: return p.rx < -kStickHalf;
    case kRStickRight: return p.rx > kStickHalf;
    case kRStickUp: return p.ry > kStickHalf;
    case kRStickDown: return p.ry < -kStickHalf;
    default: return false;
  }
}

int Amount(const PadState& p, int s) {
  auto stick = [](int v) { return std::clamp((v - 7000) * 255 / (32767 - 7000), 0, 255); };  // past the dead zone
  switch (s) {
    case kLT: return p.lt;
    case kRT: return p.rt;
    case kLStickLeft: return stick(-p.lx);
    case kLStickRight: return stick(p.lx);
    case kLStickUp: return stick(p.ly);
    case kLStickDown: return stick(-p.ly);
    case kRStickLeft: return stick(-p.rx);
    case kRStickRight: return stick(p.rx);
    case kRStickUp: return stick(p.ry);
    case kRStickDown: return stick(-p.ry);
    default: return Pressed(p, s) ? 255 : 0;
  }
}

int NewlyPressed(const PadState& now, const PadState& before) {
  for (int s = 1; s < kSourceCount; ++s)
    if (Pressed(now, s) && !Pressed(before, s)) return s;
  return kNone;
}

}  // namespace ctl
