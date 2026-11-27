// Direct3D 11 presenter of the native runtime (ADR 0013, stage 1). Owned by the window thread: the PS2 frame is uploaded as a
// texture, optionally anti-aliased (FXAA), and drawn with a post-processing shader (scaling filter, aspect, sharpening,
// scanlines); the settings overlay is blended on top. No D3D types leave d3d11_present.cpp.
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
// F12: the next Draw saves the window's picture as "Hot Pursuit 2 <date> <time>.png" in `folder` (created when missing).
void RequestScreenshot(const wchar_t* folder);
// gpuFrame (an ID3D11ShaderResourceView*, gpuW x gpuH): the GPU renderer's picture, shown instead of `frame` (ADR 0014).
void Draw(const std::uint32_t* frame, int frameW, int frameH, bool frameChanged, const std::uint32_t* overlay, int overlayW,
          int overlayH, bool overlayChanged, const ui::Settings& s, void* gpuFrame = nullptr, int gpuW = 0, int gpuH = 0,
          int movie = 0);  // movie: the full-screen movie playing (a number per movie, 0 none); its picture's non-black
                           // part fills the window at its own shape (the black around it may fall outside)

}  // namespace present
