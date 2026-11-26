// Direct3D 11 presenter of the native runtime (ADR 0013, stage 1). Owned by the window thread: the PS2 frame is uploaded as a
// texture and drawn with a post-processing shader (scaling filter, aspect, sharpening, scanlines); the settings overlay is
// blended on top. No D3D types leave d3d11_present.cpp.
#pragma once
#include <cstdint>

#include "overlay.h"

struct HWND__;

namespace present {

bool Init(HWND__* window);  // false: D3D11 or the shader compiler is unavailable (the caller keeps GDI)
void Shutdown();
void Resize();              // after the window's client size changed
// frame: BGRA (alpha ignored), overlay: premultiplied BGRA of the client size (nullptr when hidden).
// Development: the next Draw also writes the back buffer (before it is presented) as a BMP file.
void RequestShot(const char* path);
void Draw(const std::uint32_t* frame, int frameW, int frameH, bool frameChanged, const std::uint32_t* overlay, int overlayW,
          int overlayH, bool overlayChanged, const ui::Settings& s);

}  // namespace present
