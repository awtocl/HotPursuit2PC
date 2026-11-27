// Direct3D 11 presenter (ADR 0013, stage 1): flip-model swap chain on the runtime's window, the PS2 frame drawn through a
// post-processing pixel shader, the settings overlay blended on top. Shaders are compiled at start from the HLSL below
// with d3dcompiler_47.dll (a Windows system DLL), loaded at run time.
#include "present.h"

#include "gfx_device.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace present {
namespace {

template <class T>
void Release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

HWND g_hwnd = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain1* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_psFrame = nullptr;
ID3D11PixelShader* g_psOverlay = nullptr;
ID3D11SamplerState* g_point = nullptr;
ID3D11SamplerState* g_linear = nullptr;
ID3D11BlendState* g_premul = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11Buffer* g_cb = nullptr;
// Full-screen movies (Draw's fitContent): CSBounds finds the picture's non-black part; its result comes back through a ring
// of staging buffers read without waiting. The part only grows while the movie plays (dark scenes do not shrink it).
ID3D11ComputeShader* g_boundsCs = nullptr;
ID3D11Buffer* g_boundsBuf = nullptr;
ID3D11UnorderedAccessView* g_boundsUav = nullptr;
ID3D11Buffer* g_boundsStaging[3] = {};
bool g_boundsQueued[3] = {};
int g_boundsNext = 0, g_boundsW = 0, g_boundsH = 0;
float g_content[4] = {1, 1, 0, 0};  // x0, y0, x1, y1 of the picture (0..1); empty while x1 <= x0
bool g_tearing = false;

struct Tex {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  int w = 0, h = 0;
} g_frame, g_overlay;

const char kHlsl[] = R"(
Texture2D src : register(t0);
SamplerState pointS : register(s0);
SamplerState linearS : register(s1);
cbuffer Params : register(b0) {
  float4 srcSize;  // w, h, 1/w, 1/h
  float4 dstSize;  // viewport w, h
  float4 prm;      // filter (0 sharp bilinear, 1 bilinear, 2 nearest), sharpen amount, scanline strength
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
  V o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float3 Fetch(float2 uv) {
  // a picture larger than the screen (high internal resolution): the average of its pixels under each screen pixel
  // (bilinear taps spread over the footprint, up to 4 x 4: a box filter, so supersampling smooths instead of aliasing)
  const float2 ratio = srcSize.xy / dstSize.xy;
  if (max(ratio.x, ratio.y) > 1.05) {
    const int2 n = (int2)clamp(ceil(ratio), 1.0, 4.0);
    float3 sum = 0;
    [unroll] for (int y = 0; y < 4; ++y)
      [unroll] for (int x = 0; x < 4; ++x)
        if (x < n.x && y < n.y) sum += src.SampleLevel(linearS, uv + ((float2(x, y) + 0.5) / n - 0.5) * ratio * srcSize.zw, 0).rgb;
    return sum / (n.x * n.y);
  }
  if (prm.x < 0.5) {  // sharp bilinear: nearest inside each texel, a one-output-pixel blend at its edges
    float2 tex = uv * srcSize.xy;
    float2 scale = max(floor(dstSize.xy * srcSize.zw), 1.0);
    float2 f = frac(tex) - 0.5;
    float2 range = 0.5 - 0.5 / scale;
    f = (f - clamp(f, -range, range)) * scale + 0.5;
    return src.SampleLevel(linearS, (floor(tex) + f) * srcSize.zw, 0).rgb;
  }
  if (prm.x < 1.5) return src.SampleLevel(linearS, uv, 0).rgb;
  return src.SampleLevel(pointS, uv, 0).rgb;
}
float4 PSFrame(V i) : SV_Target {
  float3 c = Fetch(i.uv);
  if (prm.y > 0) {  // unsharp mask over the four neighbouring source texels
    float2 d = srcSize.zw;
    float3 n = Fetch(i.uv + float2(d.x, 0)) + Fetch(i.uv - float2(d.x, 0)) + Fetch(i.uv + float2(0, d.y)) + Fetch(i.uv - float2(0, d.y));
    c = saturate(c + (c - n * 0.25) * prm.y);
  }
  // scanlines: brightest at the centre of each source line; faded out below 2.5 output pixels per line (moire)
  float amount = prm.z * saturate(dstSize.y * srcSize.w - 1.5);
  if (amount > 0) {
    float s = 0.5 + 0.5 * cos(6.2831853 * (frac(i.uv.y * srcSize.y) - 0.5));
    c *= lerp(1.0, s, amount) * (1.0 + 0.25 * amount);
  }
  return float4(saturate(c), 1);
}
float4 PSOverlay(V i) : SV_Target { return src.SampleLevel(pointS, i.uv, 0); }
// Full-screen movies: the bounds of the picture's non-black part (every 4th pixel), as min x, min y, ~max x, ~max y (all
// minimums: a buffer clear writes one value to every element).
RWBuffer<uint> bounds : register(u0);
[numthreads(16, 16, 1)] void CSBounds(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  src.GetDimensions(w, h);
  const uint2 p = id.xy * 4;
  if (p.x >= w || p.y >= h) return;
  const float3 c = src.Load(int3(p, 0)).rgb;
  if (max(c.r, max(c.g, c.b)) < 0.1) return;
  InterlockedMin(bounds[0], p.x);
  InterlockedMin(bounds[1], p.y);
  InterlockedMin(bounds[2], ~p.x);
  InterlockedMin(bounds[3], ~p.y);
}
)";

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                   ID3DBlob**, ID3DBlob**);

bool Compile(CompileFn compile, const char* entry, const char* target, ID3DBlob** out) {
  ID3DBlob* errors = nullptr;
  const HRESULT hr = compile(kHlsl, sizeof kHlsl - 1, "present.hlsl", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             out, &errors);
  if (errors) {
    OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    errors->Release();
  }
  return SUCCEEDED(hr);
}

bool CreateTargets() {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
  const HRESULT hr = g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
  back->Release();
  return SUCCEEDED(hr);
}

bool Upload(Tex& t, const std::uint32_t* pixels, int w, int h) {
  if (t.w != w || t.h != h || !t.tex) {
    Release(t.srv);
    Release(t.tex);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t.tex))) return false;
    if (FAILED(g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv))) return false;
    t.w = w;
    t.h = h;
  }
  D3D11_MAPPED_SUBRESOURCE m = {};
  if (FAILED(g_ctx->Map(t.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
  for (int y = 0; y < h; ++y)
    std::memcpy(static_cast<std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch, pixels + static_cast<std::size_t>(y) * w,
                static_cast<std::size_t>(w) * 4);
  g_ctx->Unmap(t.tex, 0);
  return true;
}

const char* g_shotPath = nullptr;

std::wstring g_screenshotFolder;  // F12: the next Draw saves the picture as a PNG file in this folder

// PNG through the Windows Imaging Component, on its own thread (a 4K picture takes a moment to compress).
void WritePng(std::wstring path, std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t> bgr) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  const std::wstring partial = path + L".part";
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(partial.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
            format == GUID_WICPixelFormat24bppBGR &&
            SUCCEEDED(frame->WritePixels(h, w * 3, static_cast<UINT>(bgr.size()), bgr.data())) && SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(encoder->Commit());
  if (frame) frame->Release();
  if (encoder) encoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  if (ok) ok = MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;  // the launcher never sees half a file
  if (!ok) DeleteFileW(partial.c_str());  // our own temporary file
  if (SUCCEEDED(co)) CoUninitialize();
}

void SaveScreenshot(const std::wstring& folder) {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return;
  D3D11_TEXTURE2D_DESC d = {};
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &staging))) {
    g_ctx->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      std::vector<std::uint8_t> bgr(static_cast<std::size_t>(d.Width) * d.Height * 3);
      for (std::uint32_t y = 0; y < d.Height; ++y) {
        const std::uint8_t* src = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
        std::uint8_t* dst = bgr.data() + static_cast<std::size_t>(y) * d.Width * 3;
        for (std::uint32_t x = 0; x < d.Width; ++x, src += 4, dst += 3) {
          dst[0] = src[0];
          dst[1] = src[1];
          dst[2] = src[2];
        }
      }
      g_ctx->Unmap(staging, 0);
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      const std::time_t now = std::time(nullptr);
      std::tm t = {};
      localtime_s(&t, &now);
      wchar_t name[64];
      wcsftime(name, 64, L"Hot Pursuit 2 %Y-%m-%d %H-%M-%S.png", &t);
      std::thread(WritePng, (std::filesystem::path(folder) / name).wstring(), d.Width, d.Height, std::move(bgr)).detach();
    }
    staging->Release();
  }
  back->Release();
}

void SaveBackBuffer(const char* path) {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return;
  D3D11_TEXTURE2D_DESC d = {};
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &staging))) {
    g_ctx->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE m = {};
    FILE* f = nullptr;
    if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)) ) {
      if (fopen_s(&f, path, "wb") == 0 && f) {
        const std::uint32_t w = d.Width, h = d.Height, size = 54 + w * h * 4, off = 54, dib = 40;
        const std::int32_t negH = -static_cast<std::int32_t>(h);
        std::uint8_t hdr[54] = {'B', 'M'};
        std::memcpy(hdr + 2, &size, 4);
        std::memcpy(hdr + 10, &off, 4);
        std::memcpy(hdr + 14, &dib, 4);
        std::memcpy(hdr + 18, &w, 4);
        std::memcpy(hdr + 22, &negH, 4);
        hdr[26] = 1;
        hdr[28] = 32;
        fwrite(hdr, 1, 54, f);
        for (std::uint32_t y = 0; y < h; ++y) fwrite(static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch, 4, w, f);
        fclose(f);
      }
      g_ctx->Unmap(staging, 0);
    }
    staging->Release();
  }
  back->Release();
}

}  // namespace
}  // namespace present

namespace gfx {
std::recursive_mutex& Lock() {
  static std::recursive_mutex m;
  return m;
}
bool Device(ID3D11Device** dev, ID3D11DeviceContext** ctx) {
  static std::mutex creating;
  static ID3D11Device* s_dev = nullptr;
  static ID3D11DeviceContext* s_ctx = nullptr;
  static bool tried = false;
  std::lock_guard<std::mutex> lock(creating);
  if (!tried) {
    tried = true;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                 &s_dev, nullptr, &s_ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1,
                                 D3D11_SDK_VERSION, &s_dev, nullptr, &s_ctx))) {
      s_dev = nullptr;
      s_ctx = nullptr;
    }
    ID3D11Multithread* mt = nullptr;  // (each call locked inside too: resource creation from both threads)
    if (s_ctx && SUCCEEDED(s_ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt)))) {
      mt->SetMultithreadProtected(TRUE);
      mt->Release();
    }
  }
  if (!s_dev) return false;
  s_dev->AddRef();
  s_ctx->AddRef();
  *dev = s_dev;
  *ctx = s_ctx;
  return true;
}
}  // namespace gfx

namespace present {

void RequestShot(const char* path) { g_shotPath = path; }
void RequestScreenshot(const wchar_t* folder) { g_screenshotFolder = folder; }

bool Init(HWND__* window) {
  g_hwnd = window;
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  const auto compile = compiler ? reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile"))) : nullptr;
  if (!compile) return false;

  if (!gfx::Device(&g_dev, &g_ctx)) return false;  // (shared with the GPU renderer of the GS)
  std::lock_guard<std::recursive_mutex> gfxLock(gfx::Lock());
  IDXGIDevice* dxgiDev = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory2* factory = nullptr;
  bool ok = SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) &&
            SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)));
  if (ok) {
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
      BOOL allow = FALSE;
      g_tearing = SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)) && allow;
      f5->Release();
    }
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ok = SUCCEEDED(factory->CreateSwapChainForHwnd(g_dev, window, &sd, nullptr, nullptr, &g_swap));
    if (ok) factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  }
  Release(factory);
  Release(adapter);
  Release(dxgiDev);
  if (!ok || !CreateTargets()) {
    Shutdown();
    return false;
  }

  {  // movie bounds (optional: without them movies keep the picture's own fit)
    ID3DBlob* cs = nullptr;
    if (Compile(compile, "CSBounds", "cs_5_0", &cs) && SUCCEEDED(g_dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &g_boundsCs))) {
      D3D11_BUFFER_DESC bd = {};
      bd.ByteWidth = 16;
      bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
      ud.Format = DXGI_FORMAT_R32_UINT;
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Buffer.NumElements = 4;
      bool made = SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_boundsBuf)) &&
                  SUCCEEDED(g_dev->CreateUnorderedAccessView(g_boundsBuf, &ud, &g_boundsUav));
      bd.BindFlags = 0;
      bd.Usage = D3D11_USAGE_STAGING;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      for (ID3D11Buffer*& b : g_boundsStaging) made = made && SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &b));
      if (!made) Release(g_boundsCs);
    }
    Release(cs);
  }
  ID3DBlob *vs = nullptr, *psF = nullptr, *psO = nullptr;
  ok = Compile(compile, "VS", "vs_5_0", &vs) && Compile(compile, "PSFrame", "ps_5_0", &psF) && Compile(compile, "PSOverlay", "ps_5_0", &psO) &&
       SUCCEEDED(g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
       SUCCEEDED(g_dev->CreatePixelShader(psF->GetBufferPointer(), psF->GetBufferSize(), nullptr, &g_psFrame)) &&
       SUCCEEDED(g_dev->CreatePixelShader(psO->GetBufferPointer(), psO->GetBufferSize(), nullptr, &g_psOverlay));
  Release(vs);
  Release(psF);
  Release(psO);
  if (ok) {
    D3D11_SAMPLER_DESC s = {};
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ok = SUCCEEDED(g_dev->CreateSamplerState(&s, &g_point));
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ok = ok && SUCCEEDED(g_dev->CreateSamplerState(&s, &g_linear));
    D3D11_BLEND_DESC b = {};
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlend = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(g_dev->CreateBlendState(&b, &g_premul));
    D3D11_RASTERIZER_DESC r = {};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    ok = ok && SUCCEEDED(g_dev->CreateRasterizerState(&r, &g_raster));
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = 48;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(g_dev->CreateBuffer(&cb, nullptr, &g_cb));
  }
  if (!ok) Shutdown();
  return ok;
}

void Shutdown() {
  std::lock_guard<std::recursive_mutex> gfxLock(gfx::Lock());
  Release(g_frame.srv);
  Release(g_frame.tex);
  Release(g_overlay.srv);
  Release(g_overlay.tex);
  Release(g_cb);
  for (ID3D11Buffer*& b : g_boundsStaging) Release(b);
  Release(g_boundsUav);
  Release(g_boundsBuf);
  Release(g_boundsCs);
  Release(g_raster);
  Release(g_premul);
  Release(g_linear);
  Release(g_point);
  Release(g_psOverlay);
  Release(g_psFrame);
  Release(g_vs);
  Release(g_rtv);
  Release(g_swap);
  if (g_ctx) g_ctx->ClearState();
  Release(g_ctx);
  Release(g_dev);
}

void Resize() {
  if (!g_swap) return;
  std::lock_guard<std::recursive_mutex> gfxLock(gfx::Lock());
  g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
  Release(g_rtv);
  g_swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  CreateTargets();
}

void Draw(const std::uint32_t* frame, int frameW, int frameH, bool frameChanged, const std::uint32_t* overlay, int overlayW, int overlayH,
          bool overlayChanged, const ui::Settings& s, void* gpuFrame, int gpuW, int gpuH, int movie) {
  if (!g_swap || !g_rtv) return;
  std::lock_guard<std::recursive_mutex> gfxLock(gfx::Lock());  // (the GS thread's renderer uses the same context)
  RECT rc = {};
  GetClientRect(g_hwnd, &rc);
  const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
  if (cw <= 0 || ch <= 0) return;
  if (frame && frameW > 0 && frameH > 0 && (frameChanged || !g_frame.tex)) Upload(g_frame, frame, frameW, frameH);
  if (overlay && overlayChanged) Upload(g_overlay, overlay, overlayW, overlayH);

  const float black[4] = {0, 0, 0, 1};
  g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
  g_ctx->ClearRenderTargetView(g_rtv, black);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->IASetInputLayout(nullptr);
  g_ctx->RSSetState(g_raster);
  g_ctx->VSSetShader(g_vs, nullptr, 0);
  ID3D11SamplerState* samplers[2] = {g_point, g_linear};
  g_ctx->PSSetSamplers(0, 2, samplers);
  g_ctx->PSSetConstantBuffers(0, 1, &g_cb);

  // the picture: the GPU renderer's copy of the shown target, or the software GS's frame uploaded above
  ID3D11ShaderResourceView* const picture = gpuFrame ? static_cast<ID3D11ShaderResourceView*>(gpuFrame) : g_frame.srv;
  const int pw = gpuFrame ? gpuW : g_frame.w, ph = gpuFrame ? gpuH : g_frame.h;
  const float* fit = nullptr;  // the picture's part that fills the window (full-screen movies)
  static int boundsMovie = 0;
  if (picture && pw > 0 && ph > 0 && movie && g_boundsCs) {
    if (pw != g_boundsW || ph != g_boundsH || movie != boundsMovie) {  // another movie or picture size: start over
      boundsMovie = movie;
      for (bool& q : g_boundsQueued) q = false;  // (results of the previous movie are dropped)
      g_content[0] = g_content[1] = 1;
      g_content[2] = g_content[3] = 0;
      g_boundsW = pw;
      g_boundsH = ph;
    }
    for (int k = 0; k < 3; ++k) {  // results of earlier frames that are ready
      if (!g_boundsQueued[k]) continue;
      D3D11_MAPPED_SUBRESOURCE m = {};
      if (g_ctx->Map(g_boundsStaging[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
      const std::uint32_t* b = static_cast<const std::uint32_t*>(m.pData);
      if (b[0] != 0xFFFFFFFFu && b[1] != 0xFFFFFFFFu) {  // (else nothing but black yet)
        const float box[4] = {static_cast<float>(b[0]) / static_cast<float>(pw), static_cast<float>(b[1]) / static_cast<float>(ph),
                              std::min(static_cast<float>(~b[2] + 4) / static_cast<float>(pw), 1.0f),
                              std::min(static_cast<float>(~b[3] + 4) / static_cast<float>(ph), 1.0f)};
        const bool empty = g_content[2] <= g_content[0];
        g_content[0] = empty ? box[0] : std::min(g_content[0], box[0]);
        g_content[1] = empty ? box[1] : std::min(g_content[1], box[1]);
        g_content[2] = empty ? box[2] : std::max(g_content[2], box[2]);
        g_content[3] = empty ? box[3] : std::max(g_content[3], box[3]);
      }
      g_ctx->Unmap(g_boundsStaging[k], 0);
      g_boundsQueued[k] = false;
    }
    const int k = g_boundsNext;
    if (!g_boundsQueued[k]) {  // this frame's bounds (read in a later frame)
      const UINT reset[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
      g_ctx->ClearUnorderedAccessViewUint(g_boundsUav, reset);
      g_ctx->CSSetShader(g_boundsCs, nullptr, 0);
      g_ctx->CSSetShaderResources(0, 1, &picture);
      g_ctx->CSSetUnorderedAccessViews(0, 1, &g_boundsUav, nullptr);
      g_ctx->Dispatch(static_cast<UINT>((pw / 4 + 15) / 16), static_cast<UINT>((ph / 4 + 15) / 16), 1);
      ID3D11ShaderResourceView* noSrv = nullptr;
      ID3D11UnorderedAccessView* noUav = nullptr;
      g_ctx->CSSetShaderResources(0, 1, &noSrv);
      g_ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
      g_ctx->CopyResource(g_boundsStaging[k], g_boundsBuf);
      g_boundsQueued[k] = true;
      g_boundsNext = (k + 1) % 3;
    }
    static const bool boundsLog = std::getenv("HP2_MOVIE_BOX_LOG") != nullptr;  // development
    static int logged = 0;
    if (boundsLog && logged++ < 300)
      if (FILE* f = std::fopen(".cache/native/movie_box.txt", logged == 1 ? "w" : "a")) {
        std::fprintf(f, "movie %d picture %dx%d content %.3f %.3f %.3f %.3f\n", movie, pw, ph, g_content[0], g_content[1], g_content[2], g_content[3]);
        std::fclose(f);
      }
    const float cw0 = g_content[2] - g_content[0], ch0 = g_content[3] - g_content[1];
    if (cw0 > 0.25f && ch0 > 0.25f && cw0 * ch0 < 0.97f) fit = g_content;  // (all of it: nothing to enlarge)
  } else if (!movie) {
    g_content[0] = g_content[1] = 1;
    g_content[2] = g_content[3] = 0;
    for (bool& q : g_boundsQueued) q = false;
  }
  if (picture && pw > 0 && ph > 0) {
    // destination rectangle: the PS2 picture is 4:3 whatever its pixel size, or 16:9 with the game's widescreen patches
    // (ADR 0015: the game then draws a wider view into the same frame, shown here at its real shape)
    float vw = static_cast<float>(cw), vh = static_cast<float>(ch);
    if (s.aspect == 0 || s.aspect == 1) {
      const float shape = s.aspect == 0 ? 16.0f / 9.0f : 4.0f / 3.0f;
      if (vw / vh > shape) vw = vh * shape; else vh = vw / shape;
    } else if (s.aspect == 2) {
      const int k = std::max(1, std::min(cw / pw, ch / ph));
      vw = static_cast<float>(pw * k);
      vh = static_cast<float>(ph * k);
    }
    D3D11_VIEWPORT vp = {std::floor((static_cast<float>(cw) - vw) / 2), std::floor((static_cast<float>(ch) - vh) / 2), vw, vh, 0, 1};
    if (fit && (s.aspect == 0 || s.aspect == 1)) {  // the rectangle at its own shape, as large as the window allows, centred
      const float shape = s.aspect == 0 ? 16.0f / 9.0f : 4.0f / 3.0f;  // the whole picture's
      const float bw = (fit[2] - fit[0]) * shape, bh = fit[3] - fit[1];  // the rectangle in picture heights
      const float k = std::min(static_cast<float>(cw) / bw, static_cast<float>(ch) / bh);  // window pixels per picture height
      vw = k * shape;
      vh = k;
      vp = {std::floor(static_cast<float>(cw) / 2 - (fit[0] + fit[2]) / 2 * vw), std::floor(static_cast<float>(ch) / 2 - (fit[1] + fit[3]) / 2 * vh),
            vw, vh, 0, 1};
    }
    g_ctx->RSSetViewports(1, &vp);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      static const float kSharpen[4] = {0.0f, 0.35f, 0.7f, 1.1f}, kScan[3] = {0.0f, 0.35f, 0.7f};
      const float p[12] = {static_cast<float>(pw), static_cast<float>(ph), 1.0f / static_cast<float>(pw),
                           1.0f / static_cast<float>(ph), vw, vh, 0, 0, static_cast<float>(s.filter), kSharpen[s.sharpen & 3],
                           kScan[std::min(s.scanlines, 2)], 0};
      std::memcpy(m.pData, p, sizeof p);
      g_ctx->Unmap(g_cb, 0);
    }
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->PSSetShader(g_psFrame, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &picture);
    g_ctx->Draw(3, 0);
  }
  if (overlay && g_overlay.srv && g_overlay.w == cw && g_overlay.h == ch) {
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(cw), static_cast<float>(ch), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetBlendState(g_premul, nullptr, 0xFFFFFFFF);
    g_ctx->PSSetShader(g_psOverlay, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_overlay.srv);
    g_ctx->Draw(3, 0);
  }
  if (g_shotPath) {
    SaveBackBuffer(g_shotPath);
    g_shotPath = nullptr;
  }
  if (!g_screenshotFolder.empty()) {
    SaveScreenshot(g_screenshotFolder);
    g_screenshotFolder.clear();
  }
  if (s.vsync) g_swap->Present(1, 0);
  else g_swap->Present(0, g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

}  // namespace present
