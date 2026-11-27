// Native settings overlay (ADR 0013): the graphics options, their storage (hp2native.ini next to the executable) and the
// in-game menu drawn in Hot Pursuit 2's visual language. All functions run on the window thread.
#pragma once
#include <cstdint>
#include <vector>

namespace ui {

struct Settings {
  int renderer = 1;    // 0 software GS, 1 GPU (ADR 0014)
  int resolution = 3;  // GPU internal resolution: 0 1x (native), 1 2x, 2 3x, 3 4x (default), 4 6x, 5 8x (above the screen's: supersampling)
  int frameRate = 1;   // 0 30 frames per second (the game's own), 1 60 (the game's 60 fps patch, ADR 0015)
  int fullscreen = 1;  // 0 window, 1 borderless full screen (default)
  int vsync = 1;       // 0 off (tearing allowed when the system supports it), 1 on
  int filter = 0;      // 0 sharp bilinear, 1 bilinear, 2 nearest
  int aspect = 0;      // 0 16:9 (the game's widescreen patches, ADR 0015), 1 4:3, 2 4:3 integer scale, 3 stretch to the window
  int sharpen = 0;     // 0 off, 1 low, 2 medium, 3 high
  int scanlines = 0;   // 0 off, 1 light, 2 strong
  int language = 0;    // i18n::Lang of the launcher and this menu (first start: Windows' display language)
  int padLayout = 0;   // controller layout (controls.h): 0 Classic (PS2), 1 Modern (triggers), 2 Custom (made in the launcher)
};

// The settings in menu order, with their ini keys (section [graphics]) and value names; shared with the launcher.
struct SettingInfo {
  const wchar_t* key;
  const wchar_t* label;
  int Settings::*field;
  int count;
  const wchar_t* names[6];
};
int SettingCount();
const SettingInfo& Setting(int i);

Settings Load();
void Save(const Settings& s);

bool Visible();
void Toggle();
// Menu navigation. Returns true when a setting changed (the caller applies and saves it).
enum class Input { Up, Down, Left, Right, Accept, Back };
bool Navigate(Input in, Settings& s);
// True once after Quit game was confirmed in the menu (the caller closes the window: the game ends as on Alt+F4).
bool TakeQuit();
// The game runs with its widescreen patches (decided at start): a change between 16:9 and 4:3 shows "(restart)".
void SetWidescreenActive(bool wide);
// Draws the menu (when visible) into a premultiplied BGRA image of the given size. Returns false when there is nothing to draw.
bool Render(const Settings& s, int width, int height, std::vector<std::uint32_t>& out);

}  // namespace ui
