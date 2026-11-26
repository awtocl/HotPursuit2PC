// Direct3D 11 presenter (ADR 0013, stage 1): flip-model swap chain on the runtime's window, the PS2 frame drawn through a
// post-processing pixel shader, the settings overlay blended on top. Shaders are compiled at start from the HLSL below
// with d3dcompiler_47.dll (a Windows system DLL), loaded at run time.
#include "present.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

void RequestShot(const char* path) { g_shotPath = path; }

bool Init(HWND__* window) {
  g_hwnd = window;
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  const auto compile = compiler ? reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile"))) : nullptr;
  if (!compile) return false;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                               &g_dev, nullptr, &g_ctx))) {
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1,
                                 D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx)))
      return false;
  }
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
  Release(g_frame.srv);
  Release(g_frame.tex);
  Release(g_overlay.srv);
  Release(g_overlay.tex);
  Release(g_cb);
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
  g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
  Release(g_rtv);
  g_swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  CreateTargets();
}

void Draw(const std::uint32_t* frame, int frameW, int frameH, bool frameChanged, const std::uint32_t* overlay, int overlayW, int overlayH,
          bool overlayChanged, const ui::Settings& s) {
  if (!g_swap || !g_rtv) return;
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

  if (g_frame.srv) {
    // destination rectangle: the PS2 picture is 4:3 whatever its pixel size
    float vw = static_cast<float>(cw), vh = static_cast<float>(ch);
    if (s.aspect == 0) {
      if (vw / vh > 4.0f / 3.0f) vw = vh * 4.0f / 3.0f; else vh = vw * 3.0f / 4.0f;
    } else if (s.aspect == 2) {
      const int k = std::max(1, std::min(cw / g_frame.w, ch / g_frame.h));
      vw = static_cast<float>(g_frame.w * k);
      vh = static_cast<float>(g_frame.h * k);
    }
    D3D11_VIEWPORT vp = {std::floor((static_cast<float>(cw) - vw) / 2), std::floor((static_cast<float>(ch) - vh) / 2), vw, vh, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      static const float kSharpen[4] = {0.0f, 0.35f, 0.7f, 1.1f}, kScan[3] = {0.0f, 0.35f, 0.7f};
      const float p[12] = {static_cast<float>(g_frame.w), static_cast<float>(g_frame.h), 1.0f / static_cast<float>(g_frame.w),
                           1.0f / static_cast<float>(g_frame.h), vw, vh, 0, 0, static_cast<float>(s.filter), kSharpen[s.sharpen & 3],
                           kScan[std::min(s.scanlines, 2)], 0};
      std::memcpy(m.pData, p, sizeof p);
      g_ctx->Unmap(g_cb, 0);
    }
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->PSSetShader(g_psFrame, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_frame.srv);
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
  if (s.vsync) g_swap->Present(1, 0);
  else g_swap->Present(0, g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

}  // namespace present
