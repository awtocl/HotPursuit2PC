// Native settings overlay (ADR 0013): the graphics options, their storage (hp2native.ini next to the executable) and the
// in-game menu drawn in Hot Pursuit 2's visual language. All functions run on the window thread.
#pragma once
#include <cstdint>
#include <vector>

namespace ui {

struct Settings {
  int fullscreen = 0;  // 0 window, 1 borderless full screen
  int vsync = 1;       // 0 off (tearing allowed when the system supports it), 1 on
  int filter = 0;      // 0 sharp bilinear, 1 bilinear, 2 nearest
  int aspect = 0;      // 0 4:3, 1 stretch to the window, 2 integer scale
  int sharpen = 0;     // 0 off, 1 low, 2 medium, 3 high
  int scanlines = 0;   // 0 off, 1 light, 2 strong
};

Settings Load();
void Save(const Settings& s);

bool Visible();
void Toggle();
// Menu navigation. Returns true when a setting changed (the caller applies and saves it).
enum class Input { Up, Down, Left, Right, Accept, Back };
bool Navigate(Input in, Settings& s);
// Draws the menu (when visible) into a premultiplied BGRA image of the given size. Returns false when there is nothing to draw.
bool Render(const Settings& s, int width, int height, std::vector<std::uint32_t>& out);

}  // namespace ui
