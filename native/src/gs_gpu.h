// GPU renderer of the GS command stream (ADR 0014): the primitives the software GS sets up, drawn with Direct3D 11 into
// render targets kept on the GPU per GS frame-buffer address. Opt-in with HP2_GPU=1 while it grows. Called from the GS thread
// only (presentation hand-over excepted); no D3D types leave gs_gpu.cpp.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gpu {

// x, y: GS pixels with the drawing offset removed (sub-pixel precision kept); z: the 32-bit depth the GS compares
// (Z24 values << 8); colour 0..255; s, t, q as sent (STQ); u, v in texels (UV, already divided by 16).
struct Vertex {
  float x, y;
  std::uint32_t z;
  float r, g, b, a, s, t, q, u, v;
};
// The drawing registers of the primitive's context (raw values).
struct State {
  std::uint64_t prim, frame, zbuf, test, alpha, tex0, tex1, clamp, scissor, texa, fba, pabe;
};
// Where the texels come from: a decoded texture of the CPU cache (RGBA, w x h) or, `target`, the GPU target at TEX0's base.
struct Texture {
  std::shared_ptr<const std::uint32_t[]> pix;
  std::uint32_t w = 0, h = 0;
  bool target = false;
};

bool Enabled();  // HP2_GPU=1 and a Direct3D 11 device
void Triangle(const State& st, const Texture& tex, const Vertex v[3]);
void Sprite(const State& st, const Texture& tex, const Vertex& a, const Vertex& b);
// TEX0's texture is a GPU target (same base, width and a format the target holds): sample it instead of GS memory.
bool IsTarget(std::uint64_t tex0);
// A host-to-local transfer wrote this rectangle (GS pixels) of a buffer: when a GPU target holds that buffer, its pixels
// (RGBA, w x h) go into the target.
bool HoldsBuffer(std::uint32_t bp, std::uint32_t bw);
void Uploaded(std::uint32_t bp, std::uint32_t bw, int x, int y, int w, int h, const std::vector<std::uint32_t>& rgba);
// The refresh's picture: copies the displayed rectangle of the target at DISPFB's base for the window thread. False: no
// GPU target there (the caller shows GS memory the software way).
bool Present(std::uint64_t dispfb, int x, int y, int w, int h);
// Window thread: the last picture (ID3D11ShaderResourceView*, AddRef'd; release with ReleaseFrame) or nullptr.
void* AcquireFrame(int* w, int* h);
void ReleaseFrame(void* srv);
// Development: the last picture on the CPU (BGRA), for test screenshots.
bool ReadFrame(std::vector<std::uint32_t>& bgra, int* w, int* h);
std::string StatsText();  // draws, batches, targets, textures, unsupported cases since the last call

}  // namespace gpu
