// GPU renderer of the GS command stream (ADR 0014). See gs_gpu.h.
//
// Targets: one texture per GS frame buffer (page, width in 64-pixel units, 32/16-bit class), RGBA8 holding the GS's 8-bit
// values (alpha raw: 0x80 = 128/255). Depth: 32-bit float per (Z page, size), Z / 2^32 (Z24 values exact). Vertices keep
// GS pixel coordinates; the vertex shader shifts them half a pixel so that Direct3D's pixel centres fall on the GS's
// integer sample positions. Pixel rules (texture function, wrap modes, alpha test) run in the pixel shader with integer
// texel addressing. Blending: the shader computes the source part of ((A - B) * C >> 7) + D and outputs C (0..128 scale ->
// 0..1) as the second source; the destination part is fixed-function (SRC1_ALPHA factors, add/subtract).
#include "gs_gpu.h"

#include "gfx_device.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace gpu {
namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

template <class T>
void Release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

const char kHlsl[] = R"(
cbuffer C : register(b0) {
  float4 target;  // target width, height (internal pixels), internal scale
  uint4 texi;     // tme, fst, tfx, tcc
  uint4 wrap;     // wms, wmt, bilinear, source conversion (1 CT16 target, 2 CT24 view of a CT32 target)
  uint4 region;   // minu, maxu, minv, maxv
  float4 texs;    // texture width, height, scale of the source (targets: internal scale)
  uint4 atest;    // ate, atst, aref, passed (0 every pixel, 1 passing only, 2 failing only)
  int4 blend;     // blending on, coefficient of C in the source part, constant of the source part, C (0 As, 1 Ad, 2 FIX)
  float4 blendf;  // FIX / 128, TA0, TA1, AEM
  uint4 misc;     // FBA, 16-bit target, source part is C (reverse subtract), source is a GPU target (alpha stored as a / 128)
};
Texture2D<float4> T : register(t0);
struct VIn { float2 xy : POSITION; float z : DEPTH; float4 c : COLOR; float3 stq : TEXCOORD0; float2 uv : TEXCOORD1; };
struct VOut { float4 pos : SV_Position; noperspective float4 c : COLOR; noperspective float3 stq : TEXCOORD0; noperspective float2 uv : TEXCOORD1; };
VOut VS(VIn i) {
  VOut o;
  float2 p = (i.xy * target.z + 0.5) / target.xy;
  o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, i.z, 1);
  o.c = i.c;
  o.stq = i.stq;
  o.uv = i.uv;
  return o;
}
int WrapC(int v, int size, uint mode, int mn, int mx) {
  if (mode == 0) return v & (size - 1);
  if (mode == 1) return clamp(v, 0, size - 1);
  if (mode == 2) return clamp(v, mn, mx);
  return (v & mn) | mx;
}
float4 Texel(int2 p) {
  int tw = int(texs.x), th = int(texs.y);
  p.x = WrapC(p.x, tw, wrap.x, int(region.x), int(region.y)) & (tw - 1);
  p.y = WrapC(p.y, th, wrap.y, int(region.z), int(region.w)) & (th - 1);
  float4 t = floor(T.Load(int3(p, 0)) * 255.0 + 0.5);
  return t;
}
float4 Converted(float4 t) {  // CT16 / CT24 views
  if (wrap.w == 1) {  // CT16 target: 5-bit colour, alpha bit through TEXA
    t.rgb = floor(t.rgb / 8) * 8;
    t.a = t.a >= 128 ? blendf.z : ((blendf.w != 0 && all(t.rgb == 0)) ? 0 : blendf.y);
  } else if (wrap.w == 2) {  // CT24: alpha from TEXA
    t.a = (blendf.w != 0 && all(t.rgb == 0)) ? 0 : blendf.y;
  }
  return t;
}
// A GPU target as texture, on its own (internal resolution) grid: GS texel coordinate u lands on fine coordinate
// (u - 0.5) * N + 0.5, so the game's u = x + 0.5 copies stay exact 1:1 copies at any resolution.
float4 TargetTexel(int2 p) {
  int2 size = int2(texs.xy * texs.z);
  float4 raw = T.Load(int3(clamp(p, int2(0, 0), size - 1), 0));
  float4 t = floor(raw * 255.0 + 0.5);
  t.a = floor(raw.a * 128.0 + 0.5);  // (targets keep alpha / 128)
  return Converted(t);
}
struct POut { float4 c : SV_Target0; float4 f : SV_Target1; };
POut PS(VOut i) {
  float4 c = floor(i.c);
  if (texi.x != 0) {
    float2 st = texi.y != 0 ? i.uv : (i.stq.z != 0 ? i.stq.xy / i.stq.z * texs.xy : float2(0, 0));
    float4 t;
    if (misc.w != 0) {
      float2 U = (st - 0.5) * texs.z + 0.5;
      if (wrap.z != 0) {
        float2 f = U - 0.5;
        float2 fl = floor(f);
        int2 p = int2(fl);
        float2 w = floor((f - fl) * 256) / 256;
        t = floor(lerp(lerp(TargetTexel(p), TargetTexel(p + int2(1, 0)), w.x), lerp(TargetTexel(p + int2(0, 1)), TargetTexel(p + int2(1, 1)), w.x), w.y));
      } else {
        t = TargetTexel(int2(floor(U)));
      }
    } else if (wrap.z != 0) {
      float2 f = st - 0.5;
      float2 fl = floor(f);
      int2 p = int2(fl);
      float2 w = floor((f - fl) * 256) / 256;
      t = floor(lerp(lerp(Texel(p), Texel(p + int2(1, 0)), w.x), lerp(Texel(p + int2(0, 1)), Texel(p + int2(1, 1)), w.x), w.y));
    } else {
      t = Texel(int2(floor(st)));
    }
    float4 r;
    if (texi.z == 0) {
      r.rgb = min(floor(t.rgb * c.rgb / 128), 255);
      r.a = texi.w != 0 ? min(floor(t.a * c.a / 128), 255) : c.a;
    } else if (texi.z == 1) {
      r.rgb = t.rgb;
      r.a = texi.w != 0 ? t.a : c.a;
    } else {
      r.rgb = min(floor(t.rgb * c.rgb / 128) + c.a, 255);
      r.a = texi.w != 0 ? min(t.a + c.a, 255) : c.a;
    }
    c = r;
  } else {
    c = clamp(c, 0, 255);
  }
  if (atest.x != 0) {
    float ref = float(atest.z);
    bool passed = true;
    switch (atest.y) {
      case 0: passed = false; break;
      case 2: passed = c.a < ref; break;
      case 3: passed = c.a <= ref; break;
      case 4: passed = c.a == ref; break;
      case 5: passed = c.a >= ref; break;
      case 6: passed = c.a > ref; break;
      case 7: passed = c.a != ref; break;
      default: passed = true; break;
    }
    if (atest.w == 1 && !passed) discard;
    if (atest.w == 2 && passed) discard;
  }
  float k = blend.w == 0 ? c.a / 128 : (blend.w == 2 ? blendf.x : 1.0);
  float sc = misc.z != 0 ? k : float(blend.y) * k + float(blend.z);
  float3 rgb = blend.x != 0 ? c.rgb * sc : c.rgb;
  if (misc.y != 0 && blend.x == 0) rgb = floor(rgb / 8) * 8;
  float a = c.a;
  if (misc.x != 0 && a < 128) a += 128;
  POut o;
  o.c = float4(rgb / 255, saturate(a / 128));  // (targets keep alpha / 128: DEST_ALPHA is then Ad's 0..128 scale)
  o.f = float4(0, 0, 0, k);
  return o;
}
)";

struct Consts {
  float target[4];
  u32 texi[4];
  u32 wrap[4];
  u32 region[4];
  float texs[4];
  u32 atest[4];
  std::int32_t blend[4];
  float blendf[4];
  u32 misc[4];
};
struct GpuVertex {
  float x, y, z, r, g, b, a, s, t, q, u, v;
};

struct Target {
  u32 page = 0, fbw = 0;
  bool is16 = false;
  int w = 0, h = 0;  // GS pixels
  int usedRows = 0;  // rows drawn or uploaded so far (the GS memory the target stands for)
  ID3D11Texture2D* tex = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
};
struct Depth {
  u32 page = 0;
  int w = 0, h = 0;
  ID3D11Texture2D* tex = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;  // (R32_FLOAT view for ReadDepth)
};
struct GpuTexture {
  std::shared_ptr<const u32[]> keep;  // (the decoded buffer stays allocated while it is cached here: its address is the key)
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  u64 used = 0;
};

// Everything a batch shares; primitives with an equal key are drawn together.
struct Key {
  Target* rt = nullptr;
  Depth* ds = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  bool srvIsRt = false;  // (sampling the target being drawn: copied first)
  Consts c = {};
  u32 srcBlend = 0, destBlend = 0, op = 0, writeMask = 0;
  u32 depthFunc = 0;  // 0 off, else D3D11_COMPARISON_*
  bool depthWrite = false;
  u32 failPass = 0;   // second pass for failing pixels: 0 none, 1 colour only (no depth), 2 depth only, 3 RGB only
  D3D11_RECT scissor = {};
};
bool SameKey(const Key& a, const Key& b) {
  return a.rt == b.rt && a.ds == b.ds && a.srv == b.srv && a.srvIsRt == b.srvIsRt && std::memcmp(&a.c, &b.c, sizeof a.c) == 0 &&
         a.srcBlend == b.srcBlend && a.destBlend == b.destBlend && a.op == b.op && a.writeMask == b.writeMask && a.depthFunc == b.depthFunc &&
         a.depthWrite == b.depthWrite && a.failPass == b.failPass && a.scissor.left == b.scissor.left && a.scissor.top == b.scissor.top &&
         a.scissor.right == b.scissor.right && a.scissor.bottom == b.scissor.bottom;
}

int g_scale = 1;        // internal resolution (Graphics Settings; SetScale applies it between frames)
bool g_active = false;  // the renderer draws (SetActive: the Renderer setting or HP2_GPU)
constexpr int kTargetHeight = 1024;
constexpr std::size_t kMaxVertices = 1 << 16;

bool g_tried = false, g_ok = false;
std::string g_error;  // why the renderer is unavailable
// ReadDepth: a compute shader gathers the rectangle's Z values (one sample per GS pixel) into a small buffer read back.
const char kReadHlsl[] = R"(
Texture2D<float> depth : register(t0);
RWBuffer<uint> zout : register(u0);
cbuffer P : register(b0) { uint4 rect; uint4 scale; };  // x, y, w, h (GS pixels); internal scale
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= rect.z || id.y >= rect.w) return;
  const uint n = scale.x;
  const float v = depth.Load(int3((rect.x + id.x) * n + n / 2, (rect.y + id.y) * n + n / 2, 0));
  zout[id.y * rect.z + id.x] = (uint)min(v * 4294967296.0, 4294967040.0);  // (depth is stored as Z / 2^32)
}
)";
ID3D11ComputeShader* g_readCs = nullptr;
ID3D11Buffer *g_readCb = nullptr, *g_readBuf = nullptr, *g_readStaging = nullptr;
ID3D11UnorderedAccessView* g_readUav = nullptr;
UINT g_readSize = 0;  // elements in g_readBuf
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_vb = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
std::map<u64, ID3D11BlendState*> g_blends;
std::map<u64, ID3D11DepthStencilState*> g_depths;
std::vector<Target*> g_targets;
std::vector<Depth*> g_depthBuffers;
std::unordered_map<const u32*, GpuTexture> g_textures;
u64 g_clock = 0;
ID3D11Texture2D* g_copyTex = nullptr;  // a copy of a target sampled while being drawn
ID3D11ShaderResourceView* g_copySrv = nullptr;
int g_copyW = 0, g_copyH = 0;

Key g_key;
std::vector<GpuVertex> g_verts;

// presentation: three rotating copies of the shown rectangle; the window thread takes the newest
struct Picture {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  int w = 0, h = 0;  // texture size (GS pixels x internal resolution)
};
Picture g_pictures[3];
int g_newest = -1, g_next = 0;
std::mutex g_pictureMutex;

struct Counters {
  u64 targetsDropped = 0, prims = 0, batches = 0, textureUploads = 0, targetsCreated = 0, feedbackCopies = 0, approximations = 0, uploads = 0, presents = 0,
      readbacks = 0;
} g_count, g_reported;
std::map<u64, u64> g_approxAlpha;
std::map<std::string, u64> g_notes;  // unimplemented features met (Note)  // ALPHA register values that were approximated (diagnostic)

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

bool Init() {
  g_tried = true;
  if (!gfx::Device(&g_dev, &g_ctx)) {
    g_error = "no Direct3D 11 device";
    return false;
  }
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  const auto compile = compiler ? reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile"))) : nullptr;
  if (!compile) {
    g_error = "d3dcompiler_47.dll not found";
    return false;
  }
  ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(compile(kHlsl, sizeof kHlsl - 1, "gs_gpu", nullptr, nullptr, "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &err));
  if (err) {
    g_error = std::string("vertex shader: ") + static_cast<const char*>(err->GetBufferPointer());
    Release(err);
  }
  ok = ok && SUCCEEDED(compile(kHlsl, sizeof kHlsl - 1, "gs_gpu", nullptr, nullptr, "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &err));
  if (err) {
    g_error += std::string(" pixel shader: ") + static_cast<const char*>(err->GetBufferPointer());
    Release(err);
  }
  ID3DBlob* cs = nullptr;  // (ReadDepth's; without it the sun test reads GS memory)
  if (ok && SUCCEEDED(compile(kReadHlsl, sizeof kReadHlsl - 1, "gs_read", nullptr, nullptr, "CS", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err))) {
    g_dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &g_readCs);
    D3D11_BUFFER_DESC cd = {};
    cd.ByteWidth = 32;
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cd, nullptr, &g_readCb);
  }
  Release(cs);
  Release(err);
  std::lock_guard<std::recursive_mutex> lock(gfx::Lock());
  ok = ok && SUCCEEDED(g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
       SUCCEEDED(g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps));
  const D3D11_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"DEPTH", 0, DXGI_FORMAT_R32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  ok = ok && SUCCEEDED(g_dev->CreateInputLayout(layout, 5, vs->GetBufferPointer(), vs->GetBufferSize(), &g_layout));
  Release(vs);
  Release(ps);
  D3D11_BUFFER_DESC b = {};
  b.ByteWidth = static_cast<UINT>(kMaxVertices * sizeof(GpuVertex));
  b.Usage = D3D11_USAGE_DYNAMIC;
  b.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ok = ok && SUCCEEDED(g_dev->CreateBuffer(&b, nullptr, &g_vb));
  b.ByteWidth = sizeof(Consts);
  b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ok = ok && SUCCEEDED(g_dev->CreateBuffer(&b, nullptr, &g_cb));
  D3D11_RASTERIZER_DESC r = {};
  r.FillMode = D3D11_FILL_SOLID;
  r.CullMode = D3D11_CULL_NONE;
  r.ScissorEnable = TRUE;
  r.DepthClipEnable = FALSE;
  ok = ok && SUCCEEDED(g_dev->CreateRasterizerState(&r, &g_raster));
  if (!ok && g_error.empty()) g_error = "creating the shaders, layout or buffers failed";
  return ok;
}

ID3D11BlendState* BlendState(u32 srcBlend, u32 destBlend, u32 op, u32 writeMask, bool on) {
  const u64 key = (static_cast<u64>(on) << 40) | (static_cast<u64>(srcBlend) << 24) | (static_cast<u64>(destBlend) << 16) |
                  (static_cast<u64>(op) << 8) | writeMask;
  auto& s = g_blends[key];
  if (!s) {
    D3D11_BLEND_DESC d = {};
    auto& t = d.RenderTarget[0];
    t.BlendEnable = on;
    t.SrcBlend = static_cast<D3D11_BLEND>(srcBlend ? srcBlend : D3D11_BLEND_ONE);
    t.DestBlend = static_cast<D3D11_BLEND>(destBlend ? destBlend : D3D11_BLEND_ZERO);
    t.BlendOp = static_cast<D3D11_BLEND_OP>(op ? op : D3D11_BLEND_OP_ADD);
    t.SrcBlendAlpha = D3D11_BLEND_ONE;  // the stored alpha is the source's (the GS does not blend alpha)
    t.DestBlendAlpha = D3D11_BLEND_ZERO;
    t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    t.RenderTargetWriteMask = static_cast<UINT8>(writeMask);
    g_dev->CreateBlendState(&d, &s);
  }
  return s;
}
ID3D11DepthStencilState* DepthState(u32 func, bool write) {
  const u64 key = (static_cast<u64>(func) << 1) | (write ? 1 : 0);
  auto& s = g_depths[key];
  if (!s) {
    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = func != 0;
    d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(func ? func : D3D11_COMPARISON_ALWAYS);
    g_dev->CreateDepthStencilState(&d, &s);
  }
  return s;
}

Target* FindTarget(u32 page, u32 fbw, bool is16) {
  for (Target* t : g_targets)
    if (t->page == page && t->fbw == fbw && t->is16 == is16) return t;
  return nullptr;
}
Target* GetTarget(u32 page, u32 fbw, bool is16) {
  if (Target* t = FindTarget(page, fbw, is16)) return t;
  auto* t = new Target;
  t->page = page;
  t->fbw = fbw;
  t->is16 = is16;
  t->w = static_cast<int>(std::max<u32>(fbw, 1) * 64);
  t->h = kTargetHeight;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = static_cast<UINT>(t->w * g_scale);
  d.Height = static_cast<UINT>(t->h * g_scale);
  d.MipLevels = d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  g_dev->CreateTexture2D(&d, nullptr, &t->tex);
  g_dev->CreateRenderTargetView(t->tex, nullptr, &t->rtv);
  g_dev->CreateShaderResourceView(t->tex, nullptr, &t->srv);
  const float zero[4] = {0, 0, 0, 0};
  if (t->rtv) g_ctx->ClearRenderTargetView(t->rtv, zero);
  g_targets.push_back(t);
  ++g_count.targetsCreated;
  return t;
}
Depth* GetDepth(u32 page, int w, int h) {
  for (Depth* z : g_depthBuffers)
    if (z->page == page && z->w == w && z->h == h) return z;
  auto* z = new Depth;
  z->page = page;
  z->w = w;
  z->h = h;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = static_cast<UINT>(w * g_scale);
  d.Height = static_cast<UINT>(h * g_scale);
  d.MipLevels = d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R32_TYPELESS;  // depth view D32_FLOAT, shader view R32_FLOAT (ReadDepth)
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  g_dev->CreateTexture2D(&d, nullptr, &z->tex);
  D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
  dv.Format = DXGI_FORMAT_D32_FLOAT;
  dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  if (z->tex) g_dev->CreateDepthStencilView(z->tex, &dv, &z->dsv);
  D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.Format = DXGI_FORMAT_R32_FLOAT;
  sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sv.Texture2D.MipLevels = 1;
  if (z->tex) g_dev->CreateShaderResourceView(z->tex, &sv, &z->srv);
  if (z->dsv) g_ctx->ClearDepthStencilView(z->dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
  g_depthBuffers.push_back(z);
  return z;
}
ID3D11ShaderResourceView* GetTexture(const Texture& t) {
  auto& e = g_textures[t.pix.get()];
  e.used = g_clock;
  if (e.srv) return e.srv;
  e.keep = t.pix;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = t.w;
  d.Height = t.h;
  d.MipLevels = d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_IMMUTABLE;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data = {t.pix.get(), t.w * 4, 0};
  g_dev->CreateTexture2D(&d, &data, &e.tex);
  if (e.tex) g_dev->CreateShaderResourceView(e.tex, nullptr, &e.srv);
  ++g_count.textureUploads;
  if (g_textures.size() > 3000) {  // forget textures not used for a while (their decoded buffers go with them)
    for (auto it = g_textures.begin(); it != g_textures.end();)
      if (g_clock - it->second.used > 120 && it->second.srv != e.srv) {
        Release(it->second.srv);
        Release(it->second.tex);
        it = g_textures.erase(it);
      } else {
        ++it;
      }
  }
  return e.srv;
}

void Flush() {
  if (g_verts.empty()) return;
  std::lock_guard<std::recursive_mutex> lock(gfx::Lock());
  const Key& k = g_key;
  ++g_count.batches;
  ID3D11ShaderResourceView* srv = k.srv;
  if (k.srvIsRt && k.rt) {  // the target samples itself: draw from a copy
    if (!g_copyTex || g_copyW != k.rt->w || g_copyH != k.rt->h) {
      Release(g_copySrv);
      Release(g_copyTex);
      D3D11_TEXTURE2D_DESC d = {};
      k.rt->tex->GetDesc(&d);
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      g_dev->CreateTexture2D(&d, nullptr, &g_copyTex);
      if (g_copyTex) g_dev->CreateShaderResourceView(g_copyTex, nullptr, &g_copySrv);
      g_copyW = k.rt->w;
      g_copyH = k.rt->h;
    }
    if (g_copyTex) g_ctx->CopyResource(g_copyTex, k.rt->tex);
    srv = g_copySrv;
    ++g_count.feedbackCopies;
  }
  D3D11_MAPPED_SUBRESOURCE m = {};
  if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    g_verts.clear();
    return;
  }
  std::memcpy(m.pData, g_verts.data(), g_verts.size() * sizeof(GpuVertex));
  g_ctx->Unmap(g_vb, 0);
  ID3D11ShaderResourceView* none = nullptr;
  g_ctx->PSSetShaderResources(0, 1, &none);  // (a target is unbound as texture before being bound for drawing)
  g_ctx->OMSetRenderTargets(1, &k.rt->rtv, k.ds ? k.ds->dsv : nullptr);
  D3D11_VIEWPORT vp = {0, 0, static_cast<float>(k.rt->w * g_scale), static_cast<float>(k.rt->h * g_scale), 0, 1};
  g_ctx->RSSetViewports(1, &vp);
  g_ctx->RSSetScissorRects(1, &k.scissor);
  g_ctx->RSSetState(g_raster);
  const UINT stride = sizeof(GpuVertex), offset = 0;
  g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
  g_ctx->IASetInputLayout(g_layout);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->VSSetShader(g_vs, nullptr, 0);
  g_ctx->PSSetShader(g_ps, nullptr, 0);
  g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
  if (srv) g_ctx->PSSetShaderResources(0, 1, &srv);
  auto draw = [&](const Consts& c, ID3D11BlendState* bs, ID3D11DepthStencilState* dss) {
    D3D11_MAPPED_SUBRESOURCE cm = {};
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
      std::memcpy(cm.pData, &c, sizeof c);
      g_ctx->Unmap(g_cb, 0);
    }
    g_ctx->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(dss, 0);
    g_ctx->Draw(static_cast<UINT>(g_verts.size()), 0);
  };
  const bool blendOn = k.c.blend[0] != 0;
  Consts c = k.c;
  if (k.failPass) c.atest[3] = 1;  // first the passing pixels, as configured
  draw(c, BlendState(k.srcBlend, k.destBlend, k.op, k.writeMask, blendOn), DepthState(k.depthFunc, k.depthWrite));
  if (k.failPass) {  // then the failing ones, with what AFAIL still lets them update
    c.atest[3] = 2;
    u32 mask = k.writeMask;
    bool zwrite = false;
    if (k.failPass == 2) { mask = 0; zwrite = k.depthWrite; }
    else if (k.failPass == 3) mask &= D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    draw(c, BlendState(k.srcBlend, k.destBlend, k.op, mask, blendOn), DepthState(k.depthFunc, zwrite));
  }
  g_verts.clear();
}

// The state of a primitive as a batch key (false: nothing to draw).
bool MakeKey(const State& st, const Texture& tex, Key& k) {
  const u64 frame = st.frame, test = st.test, tex0 = st.tex0;
  const u32 fbw = static_cast<u32>((frame >> 16) & 63), fpsm = static_cast<u32>((frame >> 24) & 0x3F);
  if (fbw == 0) return false;
  const bool is16 = fpsm == 0x02 || fpsm == 0x0A;
  k.rt = GetTarget(static_cast<u32>(frame & 0x1FF), fbw, is16);
  if (!k.rt || !k.rt->rtv) return false;
  const bool zte = ((test >> 16) & 1) != 0;
  const u32 ztst = static_cast<u32>((test >> 17) & 3);
  const bool zmsk = ((st.zbuf >> 32) & 1) != 0;
  k.depthFunc = 0;
  k.depthWrite = false;
  k.ds = nullptr;
  if (zte && !(ztst == 1 && zmsk)) {
    k.ds = GetDepth(static_cast<u32>(st.zbuf & 0x1FF), k.rt->w, k.rt->h);
    static const u32 kFunc[4] = {D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_GREATER};
    k.depthFunc = kFunc[ztst];
    k.depthWrite = !zmsk;
  }
  Consts& c = k.c;
  c = {};
  c.target[0] = static_cast<float>(k.rt->w * g_scale);
  c.target[1] = static_cast<float>(k.rt->h * g_scale);
  c.target[2] = static_cast<float>(g_scale);
  const bool tme = ((st.prim >> 4) & 1) != 0;
  k.srv = nullptr;
  k.srvIsRt = false;
  if (tme) {
    c.texi[0] = 1;
    c.texi[1] = (st.prim >> 8) & 1;
    c.texi[2] = static_cast<u32>((tex0 >> 35) & 3);
    c.texi[3] = static_cast<u32>((tex0 >> 34) & 1);
    c.wrap[0] = static_cast<u32>(st.clamp & 3);
    c.wrap[1] = static_cast<u32>((st.clamp >> 2) & 3);
    c.wrap[2] = static_cast<u32>((st.tex1 >> 5) & 1);
    c.region[0] = static_cast<u32>((st.clamp >> 4) & 0x3FF);
    c.region[1] = static_cast<u32>((st.clamp >> 14) & 0x3FF);
    c.region[2] = static_cast<u32>((st.clamp >> 24) & 0x3FF);
    c.region[3] = static_cast<u32>((st.clamp >> 34) & 0x3FF);
    c.texs[0] = static_cast<float>(1u << ((tex0 >> 26) & 15));
    c.texs[1] = static_cast<float>(1u << ((tex0 >> 30) & 15));
    c.texs[2] = 1;
    c.blendf[1] = static_cast<float>(st.texa & 255);
    c.blendf[2] = static_cast<float>((st.texa >> 32) & 255);
    c.blendf[3] = static_cast<float>((st.texa >> 15) & 1);
    if (tex.target) {
      const u32 tbw = static_cast<u32>((tex0 >> 14) & 63), tpsm = static_cast<u32>((tex0 >> 20) & 0x3F);
      const bool t16 = tpsm == 0x02 || tpsm == 0x0A;
      Target* src = FindTarget(static_cast<u32>((tex0 & 0x3FFF) / 32), tbw, t16);
      if (!src) return false;
      k.srv = src->srv;
      k.srvIsRt = src == k.rt;
      c.texs[2] = static_cast<float>(g_scale);
      c.wrap[3] = t16 ? 1 : (tpsm == 0x01 ? 2 : 0);
      c.misc[3] = 1;
    } else {
      if (!tex.pix) return false;
      k.srv = GetTexture(tex);
    }
  }
  // alpha test and what failing pixels still update (AFAIL)
  const bool ate = (test & 1) != 0;
  const u32 atst = static_cast<u32>((test >> 1) & 7), afail = static_cast<u32>((test >> 12) & 3);
  k.failPass = 0;
  if (ate && atst != 1) {
    c.atest[0] = 1;
    c.atest[1] = atst;
    c.atest[2] = static_cast<u32>((test >> 4) & 255);
    if (afail == 0) c.atest[3] = 1;                              // KEEP: failing pixels are dropped
    else if (afail == 1 && !k.depthWrite) c.atest[3] = 0;          // FB_ONLY without depth writes: every pixel writes colour
    else k.failPass = afail;                                       // a second pass for the failing pixels
  }
  // blending: ((A - B) * C >> 7) + D, A/B/D 0 Cs, 1 Cd, 2 zero; C 0 As, 1 Ad, 2 FIX
  const bool abe = ((st.prim >> 6) & 1) != 0;
  k.srcBlend = D3D11_BLEND_ONE;
  k.destBlend = D3D11_BLEND_ZERO;
  k.op = D3D11_BLEND_OP_ADD;
  if (abe) {
    const u32 A = static_cast<u32>(st.alpha & 3), B = static_cast<u32>((st.alpha >> 2) & 3), Cs = static_cast<u32>((st.alpha >> 4) & 3),
              D = static_cast<u32>((st.alpha >> 6) & 3);
    const int scC = (A == 0 ? 1 : 0) - (B == 0 ? 1 : 0), sc0 = D == 0 ? 1 : 0;
    const int dcC = (A == 1 ? 1 : 0) - (B == 1 ? 1 : 0), dc0 = D == 1 ? 1 : 0;
    c.blend[0] = 1;
    c.blend[1] = scC;
    c.blend[2] = sc0;
    c.blend[3] = static_cast<std::int32_t>(Cs > 2 ? 2 : Cs);
    c.blendf[0] = static_cast<float>((st.alpha >> 32) & 255) / 128.0f;
    const bool ad = Cs == 1;  // C = Ad: DEST_ALPHA factors (targets store alpha / 128), on the source side as well
    const u32 f = ad ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_SRC1_ALPHA, invf = ad ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_SRC1_ALPHA;
    if (ad) {  // the shader passes Cs as it is; the source factor comes from the table
      c.blend[1] = 0;
      c.blend[2] = 1;
      k.srcBlend = scC == 0 ? (sc0 ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO)
                   : scC == 1 ? (sc0 ? D3D11_BLEND_ONE : D3D11_BLEND_DEST_ALPHA)
                              : (sc0 ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_DEST_ALPHA);
      if (scC == 1 && sc0) { ++g_count.approximations; ++g_approxAlpha[st.alpha]; }  // 1 + Ad
    }
    if (scC == -1 && sc0 == 0) {  // Cd*dc - Cs*C
      c.misc[2] = ad ? 0 : 1;
      k.op = D3D11_BLEND_OP_REV_SUBTRACT;
      k.destBlend = dcC == 1 ? f : dc0 ? (dcC == -1 ? invf : D3D11_BLEND_ONE) : D3D11_BLEND_ZERO;
    } else if (dcC == -1 && dc0 == 0) {  // Cs*sc - Cd*C
      k.op = D3D11_BLEND_OP_SUBTRACT;
      k.destBlend = f;
    } else {
      if (dcC == 0) k.destBlend = dc0 ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
      else if (dcC == 1 && dc0 == 0) k.destBlend = f;
      else if (dcC == -1) k.destBlend = invf;
      else { k.destBlend = D3D11_BLEND_ONE; ++g_count.approximations; ++g_approxAlpha[st.alpha]; }  // 1 + C
    }
  }
  if ((test >> 14) & 1) Note("DATE");
  if (abe && (st.pabe & 1)) Note("PABE");
  c.misc[0] = static_cast<u32>(st.fba & 1);
  c.misc[1] = is16 ? 1 : 0;
  // frame buffer mask: whole channels only
  const u32 fbmsk = static_cast<u32>(frame >> 32);
  u32 mask = 0;
  if (is16) {
    if ((fbmsk & 0x001F) != 0x001F) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if ((fbmsk & 0x03E0) != 0x03E0) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if ((fbmsk & 0x7C00) != 0x7C00) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if ((fbmsk & 0x8000) == 0) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
  } else {
    for (int ch = 0; ch < 4; ++ch) {
      const u32 m = (fbmsk >> (ch * 8)) & 0xFF;
      if (m != 0xFF) mask |= 1u << ch;
      if (m != 0xFF && m != 0) Note("partial FBMSK");
    }
    if (fpsm == 0x01) mask &= ~static_cast<u32>(D3D11_COLOR_WRITE_ENABLE_ALPHA);  // CT24: no alpha
  }
  k.writeMask = mask;
  const int sx0 = static_cast<int>(st.scissor & 0x7FF), sx1 = static_cast<int>((st.scissor >> 16) & 0x7FF);
  const int sy0 = static_cast<int>((st.scissor >> 32) & 0x7FF), sy1 = static_cast<int>((st.scissor >> 48) & 0x7FF);
  k.scissor = {sx0 * g_scale, sy0 * g_scale, (sx1 + 1) * g_scale, (sy1 + 1) * g_scale};
  return true;
}

GpuVertex Gv(const Vertex& v) {
  return {v.x, v.y, static_cast<float>(static_cast<double>(v.z) / 4294967296.0), v.r, v.g, v.b, v.a, v.s, v.t, v.q, v.u, v.v};
}

void Add(const State& st, const Texture& tex, const GpuVertex* v, int n) {
  Key k;
  if (!MakeKey(st, tex, k)) return;
  if (!g_verts.empty() && (!SameKey(k, g_key) || g_verts.size() + static_cast<std::size_t>(n) > kMaxVertices)) Flush();
  if (g_verts.empty()) g_key = k;
  float maxY = 0;  // rows the primitive reaches (inside the scissor): the GS memory the target now stands for
  for (int i = 0; i < n; ++i) maxY = std::max(maxY, v[i].y);
  const int rows = std::min(static_cast<int>(maxY) + 1, static_cast<int>(k.scissor.bottom / g_scale));
  k.rt->usedRows = std::max(k.rt->usedRows, std::min(rows, k.rt->h));
  g_verts.insert(g_verts.end(), v, v + n);
  ++g_count.prims;
}

}  // namespace

void Note(const char* what) { ++g_notes[what]; }

void DropAll() {  // every target and depth buffer (a new internal resolution, or the software GS takes over)
  std::lock_guard<std::recursive_mutex> lock(gfx::Lock());
  for (Target* t : g_targets) {
    Release(t->srv);
    Release(t->rtv);
    Release(t->tex);
    delete t;
  }
  g_targets.clear();
  for (Depth* z : g_depthBuffers) {
    Release(z->dsv);
    Release(z->tex);
    Release(z->srv);
    delete z;
  }
  g_depthBuffers.clear();
  Release(g_copySrv);
  Release(g_copyTex);
  g_copyW = g_copyH = 0;
}

bool Covers(const Target* t, const std::vector<std::uint16_t>& pages) {
  const int pageRows = t->is16 ? 64 : 32;  // (CT32 pages are 64x32 pixels, CT16 64x64)
  const u32 count = t->fbw * static_cast<u32>((t->usedRows + pageRows - 1) / pageRows);
  for (std::uint16_t p : pages)
    if (p >= t->page && p < t->page + count) return true;
  return false;
}
bool CoversPages(const std::vector<std::uint16_t>& pages) {
  for (const Target* t : g_targets)
    if (Covers(t, pages)) return true;
  return false;
}
void MemoryWritten(const std::vector<std::uint16_t>& pages) {
  bool flushed = false;
  for (auto it = g_targets.begin(); it != g_targets.end();) {
    Target* t = *it;
    if (!Covers(t, pages)) {
      ++it;
      continue;
    }
    if (!flushed) {  // (a pending batch may draw into it)
      Flush();
      flushed = true;
    }
    std::lock_guard<std::recursive_mutex> lock(gfx::Lock());
    Release(t->srv);
    Release(t->rtv);
    Release(t->tex);
    delete t;
    it = g_targets.erase(it);
    ++g_count.targetsDropped;
  }
}

bool Enabled() {
  if (!g_active) return false;
  if (!g_tried) g_ok = Init();
  return g_ok;
}

void SetActive(bool on) {
  if (on == g_active) return;
  if (!on) {  // back to the software GS: its own memory is the truth again (targets are redrawn when the GPU comes back)
    Flush();
    DropAll();
  }
  g_active = on;
}

void SetScale(int n) {
  n = std::clamp(n, 1, 8);
  if (n == g_scale) return;
  Flush();
  DropAll();  // (targets are recreated at the new size; the game redraws them in the next frames)
  g_scale = n;
}

void Triangle(const State& st, const Texture& tex, const Vertex v[3]) {
  const GpuVertex g[3] = {Gv(v[0]), Gv(v[1]), Gv(v[2])};
  Add(st, tex, g, 3);
}

void Sprite(const State& st, const Texture& tex, const Vertex& a, const Vertex& b) {
  // two triangles; colour and depth of the second vertex, texture coordinates from both corners
  GpuVertex p = Gv(b), q = Gv(a);
  q.z = p.z;
  q.r = p.r; q.g = p.g; q.b = p.b; q.a = p.a;
  GpuVertex c1 = q, c2 = q;  // (b.x, a.y) and (a.x, b.y)
  c1.x = p.x; c1.s = p.s; c1.u = p.u;
  c2.y = p.y; c2.t = p.t; c2.v = p.v;
  c1.q = c2.q = q.q;  // (STQ sprites: Q is the same at the corners in practice)
  const GpuVertex g[6] = {q, c1, c2, c1, p, c2};
  Add(st, tex, g, 6);
}

bool IsTarget(std::uint64_t tex0) {
  const u32 tbp = static_cast<u32>(tex0 & 0x3FFF), tbw = static_cast<u32>((tex0 >> 14) & 63), psm = static_cast<u32>((tex0 >> 20) & 0x3F);
  if (tbp % 32) return false;
  if (psm == 0x00 || psm == 0x01) return FindTarget(tbp / 32, tbw, false) != nullptr;
  if (psm == 0x02 || psm == 0x0A) return FindTarget(tbp / 32, tbw, true) != nullptr;
  return false;
}

bool HoldsBuffer(std::uint32_t bp, std::uint32_t bw) {
  if (bp % 32) return false;
  return FindTarget(bp / 32, bw, false) != nullptr;
}

void Uploaded(std::uint32_t bp, std::uint32_t bw, int x, int y, int w, int h, const std::vector<std::uint32_t>& rgba) {
  Target* t = FindTarget(bp / 32, bw, false);
  if (!t || w <= 0 || h <= 0) return;
  const int stride = w;  // (rgba rows: the upload's width)
  w = std::min(w, t->w - x);
  h = std::min(h, t->h - y);
  if (w <= 0 || h <= 0) return;
  Flush();
  // each pixel N x N at internal resolution N
  const int n = g_scale;
  std::vector<u32> scaled(static_cast<std::size_t>(w) * n * h * n);
  for (int yy = 0; yy < h * n; ++yy)
    for (int xx = 0; xx < w * n; ++xx)
      scaled[static_cast<std::size_t>(yy) * w * n + xx] = rgba[static_cast<std::size_t>(yy / n) * stride + xx / n];
  std::lock_guard<std::recursive_mutex> lock(gfx::Lock());
  const D3D11_BOX box = {static_cast<UINT>(x * n), static_cast<UINT>(y * n), 0, static_cast<UINT>((x + w) * n), static_cast<UINT>((y + h) * n), 1};
  g_ctx->UpdateSubresource(t->tex, 0, &box, scaled.data(), static_cast<UINT>(w * n * 4), 0);
  t->usedRows = std::max(t->usedRows, std::min(y + h, t->h));
  ++g_count.uploads;
}

bool Present(std::uint64_t dispfb, int x, int y, int w, int h) {
  const u32 page = static_cast<u32>(dispfb & 0x1FF), fbw = static_cast<u32>((dispfb >> 9) & 63), psm = static_cast<u32>((dispfb >> 15) & 31);
  Target* t = FindTarget(page, fbw, psm == 0x02 || psm == 0x0A);
  if (!t || w <= 0 || h <= 0) return false;
  Flush();
  ++g_clock;
  ++g_count.presents;
  std::lock_guard<std::recursive_mutex> lock(gfx::Lock());
  Picture& p = g_pictures[g_next];
  if (!p.tex || p.w != w * g_scale || p.h != h * g_scale) {
    std::lock_guard<std::mutex> pl(g_pictureMutex);
    Release(p.srv);
    Release(p.tex);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = static_cast<UINT>(w * g_scale);
    d.Height = static_cast<UINT>(h * g_scale);
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&d, nullptr, &p.tex);
    if (p.tex) g_dev->CreateShaderResourceView(p.tex, nullptr, &p.srv);
    p.w = w * g_scale;
    p.h = h * g_scale;
  }
  if (!p.tex) return false;
  const D3D11_BOX box = {static_cast<UINT>(x * g_scale), static_cast<UINT>(y * g_scale), 0, static_cast<UINT>(std::min(x + w, t->w) * g_scale),
                         static_cast<UINT>(std::min(y + h, t->h) * g_scale), 1};
  g_ctx->CopySubresourceRegion(p.tex, 0, 0, 0, 0, t->tex, 0, &box);
  std::lock_guard<std::mutex> pl(g_pictureMutex);
  g_newest = g_next;
  g_next = (g_next + 1) % 3;
  return true;
}

void* AcquireFrame(int* w, int* h) {
  std::lock_guard<std::mutex> pl(g_pictureMutex);
  if (g_newest < 0 || !g_pictures[g_newest].srv) return nullptr;
  Picture& p = g_pictures[g_newest];
  p.srv->AddRef();
  *w = p.w;
  *h = p.h;
  return p.srv;
}
void ReleaseFrame(void* srv) {
  if (srv) static_cast<ID3D11ShaderResourceView*>(srv)->Release();
}

bool ReadFrame(std::vector<std::uint32_t>& bgra, int* w, int* h) {
  std::lock_guard<std::recursive_mutex> lock(gfx::Lock());
  Picture* p = nullptr;
  {
    std::lock_guard<std::mutex> pl(g_pictureMutex);
    if (g_newest < 0) return false;
    p = &g_pictures[g_newest];
  }
  D3D11_TEXTURE2D_DESC d = {};
  p->tex->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging = nullptr;
  if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &staging))) return false;
  g_ctx->CopyResource(staging, p->tex);
  D3D11_MAPPED_SUBRESOURCE m = {};
  bool ok = SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
  if (ok) {
    bgra.resize(static_cast<std::size_t>(d.Width) * d.Height);
    for (UINT yy = 0; yy < d.Height; ++yy) {
      const u32* row = reinterpret_cast<const u32*>(static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(yy) * m.RowPitch);
      for (UINT xx = 0; xx < d.Width; ++xx) {
        const u32 v = row[xx];  // RGBA bytes -> BGRA
        bgra[static_cast<std::size_t>(yy) * d.Width + xx] = (v & 0x0000FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
      }
    }
    *w = static_cast<int>(d.Width);
    *h = static_cast<int>(d.Height);
    g_ctx->Unmap(staging, 0);
  }
  staging->Release();
  return ok;
}

bool ReadDepth(std::uint32_t bp, int x, int y, int w, int h, std::vector<std::uint32_t>& z) {
  if (bp % 32 || w <= 0 || h <= 0 || !g_readCs || !g_readCb) return false;
  Depth* src = nullptr;
  for (Depth* d : g_depthBuffers)
    if (d->page == bp / 32 && d->srv && x + w <= d->w && y + h <= d->h) src = d;
  if (!src) return false;
  Flush();  // (a pending batch may draw into it)
  std::lock_guard<std::recursive_mutex> lock(gfx::Lock());
  const UINT count = static_cast<UINT>(w * h);
  if (count > g_readSize) {  // buffers for the largest rectangle so far
    Release(g_readUav);
    Release(g_readBuf);
    Release(g_readStaging);
    g_readSize = 0;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = count * 4;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_readBuf))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = count;
    if (FAILED(g_dev->CreateUnorderedAccessView(g_readBuf, &ud, &g_readUav))) return false;
    bd.BindFlags = 0;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_readStaging))) return false;
    g_readSize = count;
  }
  D3D11_MAPPED_SUBRESOURCE m = {};
  if (FAILED(g_ctx->Map(g_readCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
  const u32 p[8] = {static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(w), static_cast<u32>(h), static_cast<u32>(g_scale), 0, 0, 0};
  std::memcpy(m.pData, p, sizeof p);
  g_ctx->Unmap(g_readCb, 0);
  g_ctx->OMSetRenderTargets(0, nullptr, nullptr);  // (the depth buffer is read, not bound for drawing; each batch binds again)
  g_ctx->CSSetShader(g_readCs, nullptr, 0);
  g_ctx->CSSetConstantBuffers(0, 1, &g_readCb);
  g_ctx->CSSetShaderResources(0, 1, &src->srv);
  g_ctx->CSSetUnorderedAccessViews(0, 1, &g_readUav, nullptr);
  g_ctx->Dispatch(static_cast<UINT>((w + 7) / 8), static_cast<UINT>((h + 7) / 8), 1);
  ID3D11ShaderResourceView* noSrv = nullptr;
  ID3D11UnorderedAccessView* noUav = nullptr;
  g_ctx->CSSetShaderResources(0, 1, &noSrv);
  g_ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
  D3D11_BOX box = {0, 0, 0, count * 4, 1, 1};
  g_ctx->CopySubresourceRegion(g_readStaging, 0, 0, 0, 0, g_readBuf, 0, &box);
  if (FAILED(g_ctx->Map(g_readStaging, 0, D3D11_MAP_READ, 0, &m))) return false;
  z.assign(static_cast<const std::uint32_t*>(m.pData), static_cast<const std::uint32_t*>(m.pData) + count);
  g_ctx->Unmap(g_readStaging, 0);
  ++g_count.readbacks;
  return true;
}

std::string StatsText() {
  if (g_tried && !g_ok) return "unavailable: " + g_error;
  if (!g_tried) return "not started (no primitive drawn yet)";
  const Counters& c = g_count;
  const Counters& r = g_reported;
  char line[320];
  std::snprintf(line, sizeof line,
                "%llu primitives in %llu batches, %llu presents; %llu texture uploads, %zu cached; %zu targets, %zu depth buffers; "
                "%llu self-sampling copies, %llu uploads into targets, %llu approximated blends, %llu targets dropped by uploads",
                static_cast<unsigned long long>(c.prims - r.prims), static_cast<unsigned long long>(c.batches - r.batches),
                static_cast<unsigned long long>(c.presents - r.presents), static_cast<unsigned long long>(c.textureUploads - r.textureUploads),
                g_textures.size(), g_targets.size(), g_depthBuffers.size(), static_cast<unsigned long long>(c.feedbackCopies - r.feedbackCopies),
                static_cast<unsigned long long>(c.uploads - r.uploads), static_cast<unsigned long long>(c.approximations - r.approximations),
                static_cast<unsigned long long>(c.targetsDropped - r.targetsDropped));
  const u64 readbacks = c.readbacks - r.readbacks;
  g_reported = g_count;
  std::string out = line;
  out += "; " + std::to_string(readbacks) + " depth read-backs";
  out += "; targets:";  // page (width, bits, rows used)
  for (const Target* t : g_targets) {
    char one[64];
    std::snprintf(one, sizeof one, " %u(w%u,%s,%d)", t->page, t->fbw, t->is16 ? "16" : "32", t->usedRows);
    out += one;
  }
  for (const auto& [reg, n] : g_approxAlpha) {  // which blends were approximated
    char one[64];
    std::snprintf(one, sizeof one, "; ALPHA %llX x%llu", static_cast<unsigned long long>(reg), static_cast<unsigned long long>(n));
    out += one;
  }
  g_approxAlpha.clear();
  std::vector<std::pair<u64, std::string>> notes;  // the most frequent first (log lines are limited)
  for (const auto& [what, n] : g_notes) notes.emplace_back(n, what);
  std::sort(notes.rbegin(), notes.rend());
  for (std::size_t i = 0; i < notes.size() && i < 6; ++i) {
    char one[128];
    std::snprintf(one, sizeof one, "; not rendered: %s x%llu", notes[i].second.c_str(), static_cast<unsigned long long>(notes[i].first));
    out += one;
  }
  g_notes.clear();
  return out;
}

}  // namespace gpu
