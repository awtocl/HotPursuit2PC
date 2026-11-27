// Controls of the native game (ADR 0017): what the keyboard and an Xbox controller press on the PS2 controller, as
// presets (keyboard: Default; controller: Classic, Modern) and a custom layout of each, stored in hp2native.ini next
// to the game ([controls], [keyboard_custom], [controller_custom]). Shared by the game (PadPoll) and the launcher.
//
// The actions are the game's own (its Options > Controller > Configuration "A" screen): each is one PS2 button, so
// the labels follow that configuration. Each action takes two keys and two controller inputs. A trigger pressing a
// PS2 button also sets that button's pressure (the pad reports pressure-sensitive buttons) by how far it is pulled.
// The sticks pass through as the DualShock 2's sticks (the left one steers).
#pragma once
#include <cstdint>
#include <string>

namespace ctl {

enum Action : int {
  kAccelerate, kBrake, kSteerLeft, kSteerRight, kHandbrake, kHorn, kView, kLookBack, kBoost, kShiftUp, kShiftDown,
  kPause, kCycleHud, kMenuUp, kMenuDown,
  kActionCount
};

// Controller inputs (XInput, the first controller). Stick directions count as pressed past about half way.
enum Source : int {
  kNone, kA, kB, kX, kY, kLB, kRB, kLT, kRT, kLS, kRS, kBack, kStart, kDUp, kDDown, kDLeft, kDRight,
  kLStickLeft, kLStickRight, kLStickUp, kLStickDown, kRStickLeft, kRStickRight, kRStickUp, kRStickDown,
  kSourceCount
};

constexpr int kBindings = 2;  // per action and device
struct Layout {
  std::uint16_t keys[kActionCount][kBindings] = {};  // virtual-key codes, 0 none
  std::uint8_t pad[kActionCount][kBindings] = {};    // Source, kNone none
};

enum PadLayout : int { kClassic, kModern, kPadCustom };
enum KeyLayout : int { kKeyDefault, kKeyCustom };

const Layout& DefaultKeys();
const Layout& ClassicPad();
const Layout& ModernPad();

struct Config {
  int padLayout = kClassic;
  int keyLayout = kKeyDefault;
  Layout customPad;   // starts as a copy of Classic
  Layout customKeys;  // starts as a copy of Default
  // The layout in use.
  const std::uint8_t (*Pad() const)[kBindings];
  const std::uint16_t (*Keys() const)[kBindings];
};

// The controller layout in use in the game (the F1 menu changes it while playing).
void SetActivePadLayout(int layout);
int ActivePadLayout();

// hp2native.ini in `folder` (the game folder); missing values are the presets.
Config Load(const std::wstring& iniPath);
void Save(const std::wstring& iniPath, const Config& c);

// PS2 controller bit (libpad order) and the game's name of each action; the PS2 button's symbol.
std::uint16_t Ps2Bit(int action);
const wchar_t* ActionName(int action);  // English; the launcher translates through its own table
const wchar_t* Ps2Button(int action);   // e.g. L"✕", L"R1"
const wchar_t* SourceName(int source);  // e.g. L"A", L"RT", L"LS ←"
std::wstring KeyName(std::uint16_t vk);

// Live controller state -> is `source` pressed? (buttons: XINPUT_GAMEPAD bits; triggers 0..255; sticks -32768..32767)
struct PadState {
  std::uint16_t buttons = 0;
  std::uint8_t lt = 0, rt = 0;
  std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
bool Pressed(const PadState& p, int source);
// 0..255: how far `source` is pressed (triggers; stick directions past the dead zone; buttons 0 or 255).
int Amount(const PadState& p, int source);
// The first source pressed now that was not pressed in `before` (kNone when none): for "press a button" in the launcher.
int NewlyPressed(const PadState& now, const PadState& before);

}  // namespace ctl
