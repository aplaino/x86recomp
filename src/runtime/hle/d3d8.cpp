// d3d8: Direct3DCreate8 and the Direct3D 8 interfaces, on the Direct3D 9
// HLE (d3d9*.cpp).
//
// A Direct3D 8 object is the Direct3D 9 host object with a Direct3D 8
// vtable: methods whose arguments are the same in both versions are the
// d3d9 handlers themselves; the others are here. They convert what differs
// (present parameters, caps, surface descriptions, adapter identifiers)
// and call the d3d9 handlers through their thunks, or change the d3d9
// device's state directly. Every interface pointer handed out gets its
// Direct3D 8 vtable (As8), whichever d3d9 code made the object.
//
// What Direct3D 8 has and the d3d9 HLE does not is kept here per device:
// fixed-function lights, material and clip planes (sent to gfx::Device as
// they are set), shader handles, the base vertex index of SetIndices and
// state blocks.
//
// Shaders: a vertex shader is a declaration (D3DVSD tokens: streams,
// registers, constants) plus a vs_1_x function without dcl instructions.
// Each register vN the declaration loads becomes a D3D9 vertex element
// with usage TEXCOORD, index N, and the function gets the matching
// `dcl_texcoordN vN`, so the d3d9 draw path pairs them. Pixel shaders are
// the same tokens in both versions.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "runtime/display.h"
#include "runtime/hle/com.h"
#include "runtime/hle/d3d9_impl.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using namespace d3d9;
using namespace guest;

// ---- guest layouts ---------------------------------------------------------------

struct D3DPRESENT_PARAMETERS8 {
  UINT BackBufferWidth, BackBufferHeight;
  D3DFORMAT BackBufferFormat;
  UINT BackBufferCount;
  DWORD MultiSampleType, SwapEffect;
  HWND hDeviceWindow;
  BOOL Windowed, EnableAutoDepthStencil;
  D3DFORMAT AutoDepthStencilFormat;
  DWORD Flags;
  UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
};
static_assert(sizeof(D3DPRESENT_PARAMETERS8) == 52);

struct D3DSURFACE_DESC8 {
  D3DFORMAT Format;
  DWORD Type, Usage, Pool;
  UINT Size;
  DWORD MultiSampleType;
  UINT Width, Height;
};
static_assert(sizeof(D3DSURFACE_DESC8) == 32);

struct D3DADAPTER_IDENTIFIER8 {
  char Driver[512];
  char Description[512];
  uint32_t DriverVersionLowPart, DriverVersionHighPart;
  DWORD VendorId, DeviceId, SubSysId, Revision;
  GUID DeviceIdentifier;
  DWORD WHQLLevel;
};
static_assert(sizeof(D3DADAPTER_IDENTIFIER8) == 1068);

// D3DCAPS8 is D3DCAPS9 up to and including PixelShader1xMaxValue
// (MaxPixelShaderValue in D3D8).
constexpr uint32_t kCaps8Size = offsetof(D3DCAPS9, PixelShader1xMaxValue) + 4;
static_assert(kCaps8Size == 212);

struct GPOINT {
  int32_t x, y;
};

// D3DMATERIAL8 and D3DLIGHT8 are gfx::Material and gfx::Light.
static_assert(sizeof(gfx::Material) == 68 && sizeof(gfx::Light) == 104);

constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IDirect3D8 = {0x1DD9E8DA, 0x1C77, 0x4D40, {0xB0, 0xCF, 0x98, 0xFE, 0xFD, 0xFF, 0x95, 0x12}};
constexpr GUID kIID_IDirect3DDevice8 = {0x7385E5DF, 0x8FE8, 0x41D5, {0x86, 0xB6, 0xD7, 0xB4, 0x85, 0x47, 0xB6, 0xCF}};
constexpr GUID kIID_IDirect3DResource8 = {0x1B36BB7B, 0x09B7, 0x410A, {0xB4, 0x45, 0x7D, 0x14, 0x30, 0xD7, 0xB3, 0x3F}};
constexpr GUID kIID_IDirect3DVertexBuffer8 = {0x8AEEEAC7, 0x05F9, 0x44D4, {0xB5, 0x91, 0x00, 0x0B, 0x0D, 0xF1, 0xCB, 0x95}};
constexpr GUID kIID_IDirect3DSurface8 = {0xB96EEBCA, 0xB326, 0x4EA5, {0x88, 0x2F, 0x2F, 0xF5, 0xBA, 0xE0, 0x21, 0xDD}};
constexpr GUID kIID_IDirect3DIndexBuffer8 = {0x0E689C9A, 0x053D, 0x44A0, {0x9D, 0x92, 0xDB, 0x0E, 0x3D, 0x75, 0x0F, 0x86}};
constexpr GUID kIID_IDirect3DBaseTexture8 = {0xB4211CFA, 0x51B9, 0x4A9F, {0xAB, 0x78, 0xDB, 0x99, 0xB2, 0xBB, 0x67, 0x8E}};
constexpr GUID kIID_IDirect3DTexture8 = {0xE4CDD575, 0x2866, 0x4F01, {0xB1, 0x2E, 0x7E, 0xEC, 0xE1, 0xEC, 0x93, 0x58}};
constexpr GUID kIID_IDirect3DCubeTexture8 = {0x3EE5B968, 0x2ACA, 0x4C34, {0x8B, 0xB5, 0x7E, 0x0C, 0x3D, 0x19, 0xB7, 0x50}};

constexpr uint32_t D3DFMT_X8R8G8B8_ = 22;
constexpr uint32_t D3DPRESENT_INTERVAL_IMMEDIATE = 0x80000000;
constexpr uint32_t D3DERR_INVALIDCALL_ = D3DERR_INVALIDCALL;

// ---- calling the d3d9 HLE --------------------------------------------------------

// Guest memory for what a delegated call reads or writes (structures in
// their D3D9 layout, shader tokens), released at the end of the scope. A
// per-thread buffer, not the guest stack: the method's own arguments are
// read relative to esp, so esp must not move under them.
class Scratch {
 public:
  explicit Scratch(Ctx&) {
    if (!t_base) t_top = t_base = HeapAllocProcess(kSize, false);
    saved_ = t_top;
  }
  ~Scratch() { t_top = saved_; }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  uint32_t Alloc(uint32_t bytes) {
    uint32_t p = (t_top + 15) & ~15u;
    if (p + bytes > t_base + kSize) FatalF("d3d8: scratch of %u bytes does not fit", bytes);
    t_top = p + bytes;
    std::memset(GPtr(p), 0, bytes);
    return p;
  }

 private:
  static constexpr uint32_t kSize = 256 * 1024;
  static thread_local uint32_t t_base, t_top;
  uint32_t saved_;
};
thread_local uint32_t Scratch::t_base = 0, Scratch::t_top = 0;

// Calls the d3d9 handler `method` ("IDirect3DDevice9::Reset") with stdcall
// arguments (the interface pointer first).
uint32_t Call9(Ctx& c, const char* method, std::initializer_list<uint64_t> args) {
  // Thunks by method (string literals): looked up once per thread, not by
  // name on every call.
  thread_local std::unordered_map<const char*, gaddr> thunks;
  gaddr& t = thunks[method];
  if (!t) t = ThunkFor("d3d9.dll", method);
  return uint32_t(CallGuest(c, t, args));
}

// A vtable of d3d9 and d3d8 handlers: entries are "9:Iface::Method" or
// "8:Iface::Method". Built once per `key`.
uint32_t MixedVtable(const char* key, const std::vector<const char*>& entries) {
  static std::map<std::string, uint32_t> cache;
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  uint32_t vt = HeapAllocProcess(uint32_t(entries.size() * 4), false);
  for (size_t i = 0; i < entries.size(); i++) {
    const char* e = entries[i];
    Wr32(vt + 4 * uint32_t(i), ThunkFor(e[0] == '9' ? "d3d9.dll" : "d3d8.dll", e + 2));
  }
  cache[key] = vt;
  return vt;
}

uint32_t Direct3DVtable8() {
  return MixedVtable(
      "IDirect3D8",
      {"8:IDirect3D8::QueryInterface", "9:IDirect3D9::AddRef", "9:IDirect3D9::Release",
       "8:IDirect3D8::RegisterSoftwareDevice", "9:IDirect3D9::GetAdapterCount",
       "8:IDirect3D8::GetAdapterIdentifier", "8:IDirect3D8::GetAdapterModeCount",
       "8:IDirect3D8::EnumAdapterModes", "9:IDirect3D9::GetAdapterDisplayMode",
       "9:IDirect3D9::CheckDeviceType", "9:IDirect3D9::CheckDeviceFormat",
       "8:IDirect3D8::CheckDeviceMultiSampleType", "9:IDirect3D9::CheckDepthStencilMatch",
       "8:IDirect3D8::GetDeviceCaps", "8:IDirect3D8::GetAdapterMonitor",
       "8:IDirect3D8::CreateDevice"});
}

uint32_t DeviceVtable8() {
  return MixedVtable(
      "IDirect3DDevice8",
      {"8:IDirect3DDevice8::QueryInterface", "9:IDirect3DDevice9::AddRef",
       "9:IDirect3DDevice9::Release", "9:IDirect3DDevice9::TestCooperativeLevel",
       "9:IDirect3DDevice9::GetAvailableTextureMem",
       "8:IDirect3DDevice8::ResourceManagerDiscardBytes", "9:IDirect3DDevice9::GetDirect3D",
       "8:IDirect3DDevice8::GetDeviceCaps", "8:IDirect3DDevice8::GetDisplayMode",
       "9:IDirect3DDevice9::GetCreationParameters", "9:IDirect3DDevice9::SetCursorProperties",
       "9:IDirect3DDevice9::SetCursorPosition", "9:IDirect3DDevice9::ShowCursor",
       "8:IDirect3DDevice8::CreateAdditionalSwapChain", "8:IDirect3DDevice8::Reset",
       "9:IDirect3DDevice9::Present", "8:IDirect3DDevice8::GetBackBuffer",
       "8:IDirect3DDevice8::GetRasterStatus", "8:IDirect3DDevice8::SetGammaRamp",
       "8:IDirect3DDevice8::GetGammaRamp", "8:IDirect3DDevice8::CreateTexture",
       "8:IDirect3DDevice8::CreateVolumeTexture", "8:IDirect3DDevice8::CreateCubeTexture",
       "8:IDirect3DDevice8::CreateVertexBuffer", "8:IDirect3DDevice8::CreateIndexBuffer",
       "8:IDirect3DDevice8::CreateRenderTarget",
       "8:IDirect3DDevice8::CreateDepthStencilSurface",
       "8:IDirect3DDevice8::CreateImageSurface", "8:IDirect3DDevice8::CopyRects",
       "8:IDirect3DDevice8::UpdateTexture", "8:IDirect3DDevice8::GetFrontBuffer",
       "8:IDirect3DDevice8::SetRenderTarget", "8:IDirect3DDevice8::GetRenderTarget",
       "8:IDirect3DDevice8::GetDepthStencilSurface", "9:IDirect3DDevice9::BeginScene",
       "9:IDirect3DDevice9::EndScene", "9:IDirect3DDevice9::Clear",
       "8:IDirect3DDevice8::SetTransform", "9:IDirect3DDevice9::GetTransform",
       "8:IDirect3DDevice8::MultiplyTransform", "8:IDirect3DDevice8::SetViewport",
       "9:IDirect3DDevice9::GetViewport", "8:IDirect3DDevice8::SetMaterial",
       "8:IDirect3DDevice8::GetMaterial", "8:IDirect3DDevice8::SetLight",
       "8:IDirect3DDevice8::GetLight", "8:IDirect3DDevice8::LightEnable",
       "8:IDirect3DDevice8::GetLightEnable", "8:IDirect3DDevice8::SetClipPlane",
       "8:IDirect3DDevice8::GetClipPlane", "8:IDirect3DDevice8::SetRenderState",
       "9:IDirect3DDevice9::GetRenderState", "8:IDirect3DDevice8::BeginStateBlock",
       "8:IDirect3DDevice8::EndStateBlock", "8:IDirect3DDevice8::ApplyStateBlock",
       "8:IDirect3DDevice8::CaptureStateBlock", "8:IDirect3DDevice8::DeleteStateBlock",
       "8:IDirect3DDevice8::CreateStateBlock", "8:IDirect3DDevice8::SetClipStatus",
       "8:IDirect3DDevice8::GetClipStatus", "8:IDirect3DDevice8::GetTexture",
       "8:IDirect3DDevice8::SetTexture", "8:IDirect3DDevice8::GetTextureStageState",
       "8:IDirect3DDevice8::SetTextureStageState", "8:IDirect3DDevice8::ValidateDevice",
       "8:IDirect3DDevice8::GetInfo", "8:IDirect3DDevice8::SetPaletteEntries",
       "8:IDirect3DDevice8::GetPaletteEntries", "8:IDirect3DDevice8::SetCurrentTexturePalette",
       "8:IDirect3DDevice8::GetCurrentTexturePalette", "9:IDirect3DDevice9::DrawPrimitive",
       "8:IDirect3DDevice8::DrawIndexedPrimitive", "9:IDirect3DDevice9::DrawPrimitiveUP",
       "9:IDirect3DDevice9::DrawIndexedPrimitiveUP", "8:IDirect3DDevice8::ProcessVertices",
       "8:IDirect3DDevice8::CreateVertexShader", "8:IDirect3DDevice8::SetVertexShader",
       "8:IDirect3DDevice8::GetVertexShader", "8:IDirect3DDevice8::DeleteVertexShader",
       "8:IDirect3DDevice8::SetVertexShaderConstant",
       "9:IDirect3DDevice9::GetVertexShaderConstantF",
       "8:IDirect3DDevice8::GetVertexShaderDeclaration",
       "8:IDirect3DDevice8::GetVertexShaderFunction", "8:IDirect3DDevice8::SetStreamSource",
       "8:IDirect3DDevice8::GetStreamSource", "8:IDirect3DDevice8::SetIndices",
       "8:IDirect3DDevice8::GetIndices", "8:IDirect3DDevice8::CreatePixelShader",
       "8:IDirect3DDevice8::SetPixelShader", "8:IDirect3DDevice8::GetPixelShader",
       "8:IDirect3DDevice8::DeletePixelShader", "8:IDirect3DDevice8::SetPixelShaderConstant",
       "9:IDirect3DDevice9::GetPixelShaderConstantF",
       "8:IDirect3DDevice8::GetPixelShaderFunction", "8:IDirect3DDevice8::DrawRectPatch",
       "8:IDirect3DDevice8::DrawTriPatch", "8:IDirect3DDevice8::DeletePatch"});
}

uint32_t SurfaceVtable8() {
  return MixedVtable(
      "IDirect3DSurface8",
      {"8:IDirect3DSurface8::QueryInterface", "9:IDirect3DSurface9::AddRef",
       "9:IDirect3DSurface9::Release", "9:IDirect3DSurface9::GetDevice",
       "8:IDirect3DSurface8::SetPrivateData", "8:IDirect3DSurface8::GetPrivateData",
       "8:IDirect3DSurface8::FreePrivateData", "8:IDirect3DSurface8::GetContainer",
       "8:IDirect3DSurface8::GetDesc", "9:IDirect3DSurface9::LockRect",
       "9:IDirect3DSurface9::UnlockRect"});
}

uint32_t TextureVtable8(bool cube) {
  if (cube)
    return MixedVtable(
        "IDirect3DCubeTexture8",
        {"8:IDirect3DTexture8::QueryInterface", "9:IDirect3DCubeTexture9::AddRef",
         "9:IDirect3DCubeTexture9::Release", "9:IDirect3DCubeTexture9::GetDevice",
         "8:IDirect3DTexture8::SetPrivateData", "8:IDirect3DTexture8::GetPrivateData",
         "8:IDirect3DTexture8::FreePrivateData", "9:IDirect3DCubeTexture9::SetPriority",
         "9:IDirect3DCubeTexture9::GetPriority", "9:IDirect3DCubeTexture9::PreLoad",
         "9:IDirect3DCubeTexture9::GetType", "9:IDirect3DCubeTexture9::SetLOD",
         "9:IDirect3DCubeTexture9::GetLOD", "9:IDirect3DCubeTexture9::GetLevelCount",
         "8:IDirect3DTexture8::GetLevelDesc", "8:IDirect3DCubeTexture8::GetCubeMapSurface",
         "9:IDirect3DCubeTexture9::LockRect", "9:IDirect3DCubeTexture9::UnlockRect",
         "9:IDirect3DCubeTexture9::AddDirtyRect"});
  return MixedVtable(
      "IDirect3DTexture8",
      {"8:IDirect3DTexture8::QueryInterface", "9:IDirect3DTexture9::AddRef",
       "9:IDirect3DTexture9::Release", "9:IDirect3DTexture9::GetDevice",
       "8:IDirect3DTexture8::SetPrivateData", "8:IDirect3DTexture8::GetPrivateData",
       "8:IDirect3DTexture8::FreePrivateData", "9:IDirect3DTexture9::SetPriority",
       "9:IDirect3DTexture9::GetPriority", "9:IDirect3DTexture9::PreLoad",
       "9:IDirect3DTexture9::GetType", "9:IDirect3DTexture9::SetLOD",
       "9:IDirect3DTexture9::GetLOD", "9:IDirect3DTexture9::GetLevelCount",
       "8:IDirect3DTexture8::GetLevelDesc", "8:IDirect3DTexture8::GetSurfaceLevel",
       "9:IDirect3DTexture9::LockRect", "9:IDirect3DTexture9::UnlockRect",
       "9:IDirect3DTexture9::AddDirtyRect"});
}

uint32_t BufferVtable8(bool index) {
  if (index)
    return MixedVtable(
        "IDirect3DIndexBuffer8",
        {"8:IDirect3DIndexBuffer8::QueryInterface", "9:IDirect3DIndexBuffer9::AddRef",
         "9:IDirect3DIndexBuffer9::Release", "9:IDirect3DIndexBuffer9::GetDevice",
         "8:IDirect3DIndexBuffer8::SetPrivateData", "8:IDirect3DIndexBuffer8::GetPrivateData",
         "8:IDirect3DIndexBuffer8::FreePrivateData", "9:IDirect3DIndexBuffer9::SetPriority",
         "9:IDirect3DIndexBuffer9::GetPriority", "9:IDirect3DIndexBuffer9::PreLoad",
         "9:IDirect3DIndexBuffer9::GetType", "9:IDirect3DIndexBuffer9::Lock",
         "9:IDirect3DIndexBuffer9::Unlock", "9:IDirect3DIndexBuffer9::GetDesc"});
  return MixedVtable(
      "IDirect3DVertexBuffer8",
      {"8:IDirect3DVertexBuffer8::QueryInterface", "9:IDirect3DVertexBuffer9::AddRef",
       "9:IDirect3DVertexBuffer9::Release", "9:IDirect3DVertexBuffer9::GetDevice",
       "8:IDirect3DVertexBuffer8::SetPrivateData", "8:IDirect3DVertexBuffer8::GetPrivateData",
       "8:IDirect3DVertexBuffer8::FreePrivateData", "9:IDirect3DVertexBuffer9::SetPriority",
       "9:IDirect3DVertexBuffer9::GetPriority", "9:IDirect3DVertexBuffer9::PreLoad",
       "9:IDirect3DVertexBuffer9::GetType", "9:IDirect3DVertexBuffer9::Lock",
       "9:IDirect3DVertexBuffer9::Unlock", "9:IDirect3DVertexBuffer9::GetDesc"});
}

// Gives an object the d3d9 code made its Direct3D 8 vtable; returns it.
uint32_t As8(uint32_t obj) {
  if (!obj) return 0;
  if (com::As<Surface>(obj)) {
    Wr32(obj, SurfaceVtable8());
  } else if (auto* t = com::As<Texture>(obj)) {
    Wr32(obj, TextureVtable8(t->cube));
  } else if (auto* b = com::As<Buffer>(obj)) {
    Wr32(obj, BufferVtable8(b->index));
  } else if (com::As<Device>(obj)) {
    Wr32(obj, DeviceVtable8());
  } else {
    FatalF("d3d8: object %08X of a type Direct3D 8 does not have", obj);
  }
  return obj;
}
// The interface pointer a delegated call wrote to guest `out`.
void Out8(uint32_t out) {
  if (out) As8(Rd32(out));
}

// ---- per-device Direct3D 8 state -------------------------------------------------------

struct VertexShader8 {
  std::vector<uint32_t> decl, func;  // as the app gave them (END included)
  uint32_t decl9 = 0, vs9 = 0;       // d3d9 objects (vs9 0: fixed function)
  std::vector<std::pair<uint32_t, std::array<float, 4>>> consts;  // D3DVSD_CONST
};

struct PixelShader8 {
  std::vector<uint32_t> func;
  uint32_t ps9 = 0;
};

// A state block: the states it holds and their values.
struct StateBlock {
  std::map<uint32_t, uint32_t> rs;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> tss;  // D3D8 numbering
  std::map<uint32_t, uint32_t> textures;
  std::map<uint32_t, std::array<float, 16>> transforms;
  std::optional<gfx::Material> material;
  std::map<uint32_t, gfx::Light> lights;
  std::map<uint32_t, bool> light_on;
  std::map<uint32_t, std::array<float, 4>> clip;
  std::optional<D3DVIEWPORT9> viewport;
  std::optional<uint32_t> vs, ps;
  std::map<uint32_t, std::array<float, 4>> vsc, psc;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> streams;  // buffer, stride
  std::optional<std::pair<uint32_t, uint32_t>> indices;        // buffer, base
};

struct Device8 {
  gfx::Material material{};
  std::map<uint32_t, gfx::Light> lights;
  std::map<uint32_t, bool> light_on;
  float clip[32][4] = {};
  uint32_t base_vertex = 0;
  uint32_t vs = 0, ps = 0;  // handles (vs: an FVF or a shader handle)
  std::map<uint32_t, VertexShader8> vshaders;
  std::map<uint32_t, PixelShader8> pshaders;
  uint32_t next_vs = 1, next_ps = 1, next_block = 1;
  std::map<uint32_t, StateBlock> blocks;
  StateBlock* recording = nullptr;
  StateBlock recorded;
};

std::map<uint32_t, Device8> g_dev8;  // by IDirect3DDevice8 guest pointer

Device* Dev(Ctx& c) { return DeviceOf(uint32_t(Arg(c, 0))); }
Device8& X(Ctx& c) { return g_dev8[uint32_t(Arg(c, 0))]; }

// Vertex shader handles are above every FVF code.
constexpr uint32_t kVsHandleBase = 0xF0000000;

// Texture stage states that Direct3D 9 made sampler states: the D3D9
// sampler state for a D3D8 stage state, 0 if it stays a stage state.
uint32_t SamplerStateFor(uint32_t tss) {
  switch (tss) {
    case 13: return 1;   // ADDRESSU
    case 14: return 2;   // ADDRESSV
    case 15: return 4;   // BORDERCOLOR
    case 16: return 5;   // MAGFILTER
    case 17: return 6;   // MINFILTER
    case 18: return 7;   // MIPFILTER
    case 19: return 8;   // MIPMAPLODBIAS
    case 20: return 9;   // MAXMIPLEVEL
    case 21: return 10;  // MAXANISOTROPY
    case 25: return 3;   // ADDRESSW
    default: return 0;
  }
}

// ---- state setters (shared by the methods and ApplyStateBlock) -----------------------

void SetRS(Device& d, uint32_t s, uint32_t v) {
  if (s < 256) d.rs.Set(s, v);
}
uint32_t GetTSS(Device& d, uint32_t stage, uint32_t t) {
  if (uint32_t ss = SamplerStateFor(t)) return d.ss[stage][ss];
  return d.tss[stage][t];
}
void SetTSS(Device& d, uint32_t stage, uint32_t t, uint32_t v) {
  if (stage >= 8 || t >= 33) return;
  if (uint32_t ss = SamplerStateFor(t))
    d.SetSamplerState(stage, ss, v);
  else
    d.tss[stage][t] = v;
}
void SetTex(Device& d, uint32_t stage, uint32_t tex) {
  if (stage < 8) Bind(d.textures[stage], tex);
}
void SetXform(Device& d, uint32_t t, const float* m) {
  std::array<float, 16> a;
  std::memcpy(a.data(), m, sizeof a);
  d.transforms[t] = a;
}
void SetMat(Device& d, Device8& x, const gfx::Material& m) {
  x.material = m;
  d.gfx->SetMaterial(m);
}
void SetLightAt(Device& d, Device8& x, uint32_t i, const gfx::Light& l) {
  x.lights[i] = l;
  d.gfx->SetLight(i, l);
}
// Enabling a light that was never set gives it the default light: white,
// directional, along +z.
void EnableLight(Device& d, Device8& x, uint32_t i, bool on) {
  if (on && !x.lights.count(i)) {
    gfx::Light l{};
    l.type = 3;  // D3DLIGHT_DIRECTIONAL
    l.diffuse = {1, 1, 1, 0};
    l.direction[2] = 1;
    SetLightAt(d, x, i, l);
  }
  x.light_on[i] = on;
  d.gfx->LightEnable(i, on);
}
void SetClip(Device& d, Device8& x, uint32_t i, const float* p) {
  if (i >= 32) return;
  std::memcpy(x.clip[i], p, 16);
  d.gfx->SetClipPlane(i, p);
}
void SetVs(Device& d, Device8& x, uint32_t h) {
  x.vs = h;
  if (h <= kVsHandleBase) {  // an FVF
    Bind(d.vs, 0);
    Bind(d.decl, 0);
    d.fvf = h;
    return;
  }
  auto it = x.vshaders.find(h);
  if (it == x.vshaders.end()) FatalF("d3d8: SetVertexShader with unknown handle %08X", h);
  const VertexShader8& s = it->second;
  Bind(d.decl, s.decl9);
  d.fvf = 0;
  Bind(d.vs, s.vs9);
  for (const auto& [reg, v] : s.consts)
    if (reg < 256) std::memcpy(d.vs_f[reg], v.data(), 16);
}
void SetPs(Device& d, Device8& x, uint32_t h) {
  x.ps = h;
  if (!h) {
    Bind(d.ps, 0);
    return;
  }
  auto it = x.pshaders.find(h);
  if (it == x.pshaders.end()) FatalF("d3d8: SetPixelShader with unknown handle %08X", h);
  Bind(d.ps, it->second.ps9);
}
void SetStream(Device& d, uint32_t n, uint32_t vb, uint32_t stride) {
  if (n >= 16) return;
  Bind(d.streams[n].buffer, vb);
  d.streams[n].offset = 0;
  d.streams[n].stride = stride;
}
void SetIdx(Device& d, Device8& x, uint32_t ib, uint32_t base) {
  Bind(d.indices, ib);
  x.base_vertex = base;
}

// Fills `b` with the current values of the states it holds.
void Capture(Device& d, Device8& x, StateBlock& b) {
  for (auto& [s, v] : b.rs) v = d.rs[s];
  for (auto& [k, v] : b.tss) v = GetTSS(d, k.first, k.second);
  for (auto& [s, v] : b.textures) v = d.textures[s].guest;
  for (auto& [t, m] : b.transforms) {
    auto it = d.transforms.find(t);
    if (it != d.transforms.end()) m = it->second;
  }
  if (b.material) b.material = x.material;
  for (auto& [i, l] : b.lights)
    if (x.lights.count(i)) l = x.lights[i];
  for (auto& [i, on] : b.light_on) on = x.light_on[i];
  for (auto& [i, p] : b.clip) std::memcpy(p.data(), x.clip[i], 16);
  if (b.viewport) b.viewport = d.viewport;
  if (b.vs) b.vs = x.vs;
  if (b.ps) b.ps = x.ps;
  for (auto& [r, v] : b.vsc) std::memcpy(v.data(), d.vs_f[r], 16);
  for (auto& [r, v] : b.psc) std::memcpy(v.data(), d.ps_f[r], 16);
  for (auto& [n, s] : b.streams) s = {d.streams[n].buffer.guest, d.streams[n].stride};
  if (b.indices) b.indices = std::make_pair(d.indices.guest, x.base_vertex);
}

void Apply(Device& d, Device8& x, const StateBlock& b) {
  for (const auto& [s, v] : b.rs) SetRS(d, s, v);
  for (const auto& [k, v] : b.tss) SetTSS(d, k.first, k.second, v);
  for (const auto& [s, v] : b.textures) SetTex(d, s, v);
  for (const auto& [t, m] : b.transforms) SetXform(d, t, m.data());
  if (b.material) SetMat(d, x, *b.material);
  for (const auto& [i, l] : b.lights) SetLightAt(d, x, i, l);
  for (const auto& [i, on] : b.light_on) EnableLight(d, x, i, on);
  for (const auto& [i, p] : b.clip) SetClip(d, x, i, p.data());
  if (b.viewport) d.viewport = *b.viewport;
  if (b.vs) SetVs(d, x, *b.vs);
  if (b.ps) SetPs(d, x, *b.ps);
  for (const auto& [r, v] : b.vsc) std::memcpy(d.vs_f[r], v.data(), 16);
  for (const auto& [r, v] : b.psc) std::memcpy(d.ps_f[r], v.data(), 16);
  for (const auto& [n, s] : b.streams) SetStream(d, n, s.first, s.second);
  if (b.indices) SetIdx(d, x, b.indices->first, b.indices->second);
}

// Every state (D3DSBT_ALL).
StateBlock AllStates(Device& d, Device8& x) {
  StateBlock b;
  for (uint32_t s = 0; s < 256; s++) b.rs[s] = 0;
  for (uint32_t st = 0; st < 8; st++) {
    for (uint32_t t = 1; t <= 28; t++) b.tss[{st, t}] = 0;
    b.textures[st] = 0;
  }
  for (const auto& [t, m] : d.transforms) b.transforms[t] = m;
  b.material = gfx::Material{};
  for (const auto& [i, l] : x.lights) b.lights[i] = l;
  for (const auto& [i, on] : x.light_on) b.light_on[i] = on;
  for (uint32_t i = 0; i < 6; i++) b.clip[i] = {};
  b.viewport = D3DVIEWPORT9{};
  b.vs = 0;
  b.ps = 0;
  for (uint32_t r = 0; r < 96; r++) b.vsc[r] = {};
  for (uint32_t r = 0; r < 8; r++) b.psc[r] = {};
  for (uint32_t n = 0; n < 16; n++) b.streams[n] = {};
  b.indices = std::make_pair(0u, 0u);
  Capture(d, x, b);
  return b;
}

// ---- conversions --------------------------------------------------------------------

D3DPRESENT_PARAMETERS To9(const D3DPRESENT_PARAMETERS8& p) {
  D3DPRESENT_PARAMETERS q{};
  q.BackBufferWidth = p.BackBufferWidth;
  q.BackBufferHeight = p.BackBufferHeight;
  q.BackBufferFormat = p.BackBufferFormat;
  q.BackBufferCount = p.BackBufferCount;
  q.MultiSampleType = p.MultiSampleType;
  // D3DSWAPEFFECT_COPY_VSYNC (4) is D3D9's COPY with a vsynced present.
  q.SwapEffect = p.SwapEffect == 4 ? 3 : p.SwapEffect;
  q.hDeviceWindow = p.hDeviceWindow;
  q.Windowed = p.Windowed;
  q.EnableAutoDepthStencil = p.EnableAutoDepthStencil;
  q.AutoDepthStencilFormat = p.AutoDepthStencilFormat;
  q.Flags = p.Flags;
  q.FullScreen_RefreshRateInHz = p.FullScreen_RefreshRateInHz;
  // Windowed D3D8 presents do not wait for vsync unless COPY_VSYNC.
  q.PresentationInterval = p.Windowed ? (p.SwapEffect == 4 ? 0 : D3DPRESENT_INTERVAL_IMMEDIATE)
                                      : p.FullScreen_PresentationInterval;
  return q;
}

void Back8(const D3DPRESENT_PARAMETERS& q, D3DPRESENT_PARAMETERS8* p) {
  p->BackBufferWidth = q.BackBufferWidth;
  p->BackBufferHeight = q.BackBufferHeight;
  p->BackBufferFormat = q.BackBufferFormat;
  p->BackBufferCount = q.BackBufferCount;
}

D3DSURFACE_DESC8 Desc8(const D3DSURFACE_DESC& d) {
  gfx::Format f = FormatFromD3D(d.Format);
  uint32_t size = f == gfx::Format::Unknown ? 0 : gfx::RowPitch(f, d.Width) * gfx::RowCount(f, d.Height);
  return {d.Format, d.Type, d.Usage, d.Pool, size, d.MultiSampleType, d.Width, d.Height};
}

// D3DCAPS8 from the d3d9 caps: shader versions of the D3D8 era (vs_1_1,
// ps_1_3; ps_1_4 is not translated) and its constant count.
void Caps8(Ctx& c, uint32_t caps9, uint32_t out) {
  D3DCAPS9 k = *G<D3DCAPS9>(caps9);
  k.VertexShaderVersion = 0xFFFE0101;
  k.MaxVertexShaderConst = 96;
  k.PixelShaderVersion = 0xFFFF0103;
  k.PixelShader1xMaxValue = 1.0f;
  std::memcpy(GPtr(out), &k, kCaps8Size);
}

// Private data (SetPrivateData): bytes by object and GUID.
std::map<std::pair<uint32_t, std::string>, std::vector<uint8_t>> g_private;
std::string GuidKey(uint32_t va) { return std::string(reinterpret_cast<const char*>(GPtr(va)), 16); }

uint32_t SetPrivate(Ctx& c) {
  // (refguid, pData, SizeOfData, Flags); D3DSPD_IUNKNOWN (1) is kept as bytes.
  uint32_t data = Arg(c, 2), size = Arg(c, 3);
  uint8_t* p = GPtr(data);
  g_private[{uint32_t(Arg(c, 0)), GuidKey(Arg(c, 1))}].assign(p, p + size);
  return D3D_OK;
}
uint32_t GetPrivate(Ctx& c) {
  // (refguid, pData, pSizeOfData)
  auto it = g_private.find({uint32_t(Arg(c, 0)), GuidKey(Arg(c, 1))});
  if (it == g_private.end()) return D3DERR_NOTFOUND;
  uint32_t cap = Rd32(Arg(c, 3)), n = uint32_t(it->second.size());
  Wr32(Arg(c, 3), n);
  if (!Arg(c, 2)) return D3D_OK;
  if (cap < n) return 0x88760870;  // D3DERR_MOREDATA
  std::memcpy(GPtr(Arg(c, 2)), it->second.data(), n);
  return D3D_OK;
}
uint32_t FreePrivate(Ctx& c) {
  return g_private.erase({uint32_t(Arg(c, 0)), GuidKey(Arg(c, 1))}) ? D3D_OK : D3DERR_NOTFOUND;
}

uint32_t QueryFor(Ctx& c, std::initializer_list<GUID> iids) {
  uint32_t riid = Arg(c, 1), out = Arg(c, 2);
  if (com::GuidEq(riid, kIID_IUnknown)) goto found;
  for (const GUID& g : iids)
    if (com::GuidEq(riid, g)) goto found;
  Wr32(out, 0);
  return com::kE_NOINTERFACE;
found:
  com::AddRef(SurfaceRefTarget(Arg(c, 0)));
  Wr32(out, Arg(c, 0));
  return D3D_OK;
}

// ---- vertex shader declarations -------------------------------------------------------

// Bytes of a D3DVSDT type: FLOAT1-4, D3DCOLOR, UBYTE4, SHORT2, SHORT4.
uint32_t VsdtSize(uint32_t t) {
  static const uint32_t kSize[8] = {4, 8, 12, 16, 4, 4, 4, 8};
  if (t >= 8) FatalF("d3d8: vertex declaration data type %u", t);
  return kSize[t];
}

// Reads a D3D8 declaration up to D3DVSD_END: the D3D9 elements (END
// included), the registers loaded and the constants it defines.
void ParseDecl(uint32_t va, VertexShader8* s, std::vector<D3DVERTEXELEMENT9>* elems,
               std::vector<uint32_t>* regs) {
  uint32_t stream = 0;
  std::map<uint32_t, uint32_t> offset;  // per stream
  for (uint32_t p = va;; p += 4) {
    uint32_t t = Rd32(p);
    s->decl.push_back(t);
    uint32_t type = t >> 29;
    if (t == 0xFFFFFFFF || type == 7) break;  // D3DVSD_END
    if (s->decl.size() > 1024) FatalF("d3d8: vertex declaration without D3DVSD_END");
    switch (type) {
      case 0: break;  // NOP
      case 1:         // STREAM (or STREAM_TESS: bit 28)
        if (t & 0x10000000) FatalF("d3d8: tessellator streams are not supported");
        stream = t & 0xF;
        break;
      case 2: {  // STREAMDATA: a register, or a skip
        if (t & 0x10000000) {
          offset[stream] += 4 * ((t >> 16) & 0xF);
          break;
        }
        uint32_t reg = t & 0x1F, dt = (t >> 16) & 0xF;
        elems->push_back({WORD(stream), WORD(offset[stream]), uint8_t(dt), 0, 5 /* TEXCOORD */,
                          uint8_t(reg)});
        regs->push_back(reg);
        offset[stream] += VsdtSize(dt);
        break;
      }
      case 3: FatalF("d3d8: vertex declaration tessellator tokens are not supported");
      case 4: {  // CONSTMEM: count float4s at an address, then their values
        uint32_t count = (t >> 25) & 0xF, reg = t & 0x7F;
        for (uint32_t i = 0; i < count; i++) {
          std::array<float, 4> v;
          for (int k = 0; k < 4; k++) {
            p += 4;
            uint32_t w = Rd32(p);
            s->decl.push_back(w);
            std::memcpy(&v[size_t(k)], &w, 4);
          }
          s->consts.push_back({reg + i, v});
        }
        break;
      }
      case 5: {  // EXT: skipped with its data
        uint32_t n = (t >> 24) & 0x1F;
        for (uint32_t i = 0; i < n; i++) {
          p += 4;
          s->decl.push_back(Rd32(p));
        }
        break;
      }
      default: FatalF("d3d8: vertex declaration token %08X", t);
    }
  }
  elems->push_back({0xFF, 0, 17, 0, 0, 0});  // D3DDECL_END
}

// A D3D8 function up to and including END, stepped over instruction by
// instruction (hlsl::ReadBytecode).
std::vector<uint32_t> ReadTokens(uint32_t va) {
  std::vector<uint32_t> t = hlsl::ReadBytecode([va](size_t i) { return Rd32(va + 4 * uint32_t(i)); }, 1u << 20);
  if (t.empty()) FatalF("d3d8: shader function without END");
  return t;
}

}  // namespace

// ---- Direct3DCreate8, IDirect3D8 -----------------------------------------------------

namespace {

struct Direct3D8 : com::Object {};

HLE("d3d8.dll", Direct3DCreate8, 1) {
  std::fprintf(stderr, "d3d8: Direct3DCreate8(SDK %u)\n", uint32_t(Arg(c, 0)));
  EnsureSdlVideo();
  return com::Publish(std::make_shared<Direct3D8>(), Direct3DVtable8());
}

HLE_NAMED("d3d8.dll", "IDirect3D8::QueryInterface", D3D8_QueryInterface, 3) {
  return QueryFor(c, {kIID_IDirect3D8});
}

HLE_NAMED("d3d8.dll", "IDirect3D8::RegisterSoftwareDevice", D3D8_RegisterSoftwareDevice, 2) {
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3D8::GetAdapterIdentifier", D3D8_GetAdapterIdentifier, 4) {
  if (Arg(c, 1) != 0) return D3DERR_INVALIDCALL;
  auto* id = G<D3DADAPTER_IDENTIFIER8>(Arg(c, 3));
  std::memset(id, 0, sizeof *id);
  std::snprintf(id->Driver, sizeof id->Driver, "recomp_d3d8.dll");
  std::snprintf(id->Description, sizeof id->Description, "x86recomp Direct3D 8");
  id->DriverVersionHighPart = (6u << 16) | 14u;  // 6.14.10.0 (a D3D8-era driver)
  id->DriverVersionLowPart = (10u << 16) | 0u;
  id->DeviceIdentifier = {0x7B2D3B1A, 0x5C8E, 0x4F4D, {0x9A, 0x1E, 0x52, 0x45, 0x43, 0x4F, 0x4D, 0x38}};
  id->WHQLLevel = 1;
  return D3D_OK;
}

// D3D8 lists the modes of every format; ours are X8R8G8B8.
HLE_NAMED("d3d8.dll", "IDirect3D8::GetAdapterModeCount", D3D8_GetAdapterModeCount, 2) {
  return Call9(c, "IDirect3D9::GetAdapterModeCount", {Arg(c, 0), Arg(c, 1), D3DFMT_X8R8G8B8_});
}

// (Adapter, Mode, pMode)
HLE_NAMED("d3d8.dll", "IDirect3D8::EnumAdapterModes", D3D8_EnumAdapterModes, 4) {
  return Call9(c, "IDirect3D9::EnumAdapterModes",
               {Arg(c, 0), Arg(c, 1), D3DFMT_X8R8G8B8_, Arg(c, 2), Arg(c, 3)});
}

// (Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType)
HLE_NAMED("d3d8.dll", "IDirect3D8::CheckDeviceMultiSampleType", D3D8_CheckDeviceMultiSampleType, 6) {
  return Call9(c, "IDirect3D9::CheckDeviceMultiSampleType",
               {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5), 0});
}

HLE_NAMED("d3d8.dll", "IDirect3D8::GetDeviceCaps", D3D8_GetDeviceCaps, 4) {
  Scratch s(c);
  uint32_t caps9 = s.Alloc(sizeof(D3DCAPS9));
  uint32_t hr = Call9(c, "IDirect3D9::GetDeviceCaps", {Arg(c, 0), Arg(c, 1), Arg(c, 2), caps9});
  if (hr == D3D_OK) Caps8(c, caps9, Arg(c, 3));
  return hr;
}

HLE_NAMED("d3d8.dll", "IDirect3D8::GetAdapterMonitor", D3D8_GetAdapterMonitor, 2) {
  return Arg(c, 1) == 0 ? 0x00010001u : 0u;  // an HMONITOR
}

// (Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
// ppReturnedDeviceInterface)
HLE_NAMED("d3d8.dll", "IDirect3D8::CreateDevice", D3D8_CreateDevice, 7) {
  auto* pp8 = G<D3DPRESENT_PARAMETERS8>(Arg(c, 5));
  uint32_t out = Arg(c, 6);
  Scratch s(c);
  uint32_t pp9 = s.Alloc(sizeof(D3DPRESENT_PARAMETERS));
  *G<D3DPRESENT_PARAMETERS>(pp9) = To9(*pp8);
  uint32_t hr = Call9(c, "IDirect3D9::CreateDevice",
                      {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), pp9, out});
  if (hr != D3D_OK) return hr;
  Back8(*G<D3DPRESENT_PARAMETERS>(pp9), pp8);
  uint32_t dev = Rd32(out);
  As8(dev);
  g_dev8[dev] = Device8{};
  // The fixed-function defaults Direct3D 8 starts with: material and
  // lights zero, no lights on.
  DeviceOf(dev)->gfx->SetMaterial(gfx::Material{});
  std::fprintf(stderr, "d3d8: CreateDevice -> %08X\n", dev);
  return D3D_OK;
}

// ---- IDirect3DDevice8: device, swap chain -------------------------------------------

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::QueryInterface", D3DDev8_QueryInterface, 3) {
  return QueryFor(c, {kIID_IDirect3DDevice8});
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::ResourceManagerDiscardBytes", D3DDev8_ResourceManagerDiscardBytes, 2) {
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetDeviceCaps", D3DDev8_GetDeviceCaps, 2) {
  Scratch s(c);
  uint32_t caps9 = s.Alloc(sizeof(D3DCAPS9));
  uint32_t hr = Call9(c, "IDirect3DDevice9::GetDeviceCaps", {Arg(c, 0), caps9});
  if (hr == D3D_OK) Caps8(c, caps9, Arg(c, 1));
  return hr;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetDisplayMode", D3DDev8_GetDisplayMode, 2) {
  return Call9(c, "IDirect3DDevice9::GetDisplayMode", {Arg(c, 0), 0, Arg(c, 1)});
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::Reset", D3DDev8_Reset, 2) {
  auto* pp8 = G<D3DPRESENT_PARAMETERS8>(Arg(c, 1));
  Scratch s(c);
  uint32_t pp9 = s.Alloc(sizeof(D3DPRESENT_PARAMETERS));
  *G<D3DPRESENT_PARAMETERS>(pp9) = To9(*pp8);
  uint32_t hr = Call9(c, "IDirect3DDevice9::Reset", {Arg(c, 0), pp9});
  if (hr != D3D_OK) return hr;
  Back8(*G<D3DPRESENT_PARAMETERS>(pp9), pp8);
  // Reset returns every state to its default.
  Device* d = Dev(c);
  Device8& x = X(c);
  for (const auto& [i, on] : x.light_on)
    if (on) d->gfx->LightEnable(i, false);
  std::map<uint32_t, VertexShader8> vs = std::move(x.vshaders);
  std::map<uint32_t, PixelShader8> ps = std::move(x.pshaders);
  uint32_t nvs = x.next_vs, nps = x.next_ps, nb = x.next_block;
  std::map<uint32_t, StateBlock> blocks = std::move(x.blocks);
  x = Device8{};
  x.vshaders = std::move(vs);
  x.pshaders = std::move(ps);
  x.blocks = std::move(blocks);
  x.next_vs = nvs;
  x.next_ps = nps;
  x.next_block = nb;
  d->gfx->SetMaterial(gfx::Material{});
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateAdditionalSwapChain", D3DDev8_CreateAdditionalSwapChain, 3) {
  FatalF("d3d8: CreateAdditionalSwapChain is not supported");
}

// (BackBuffer, Type, ppBackBuffer)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetBackBuffer", D3DDev8_GetBackBuffer, 4) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::GetBackBuffer", {Arg(c, 0), 0, Arg(c, 1), Arg(c, 2), Arg(c, 3)});
  Out8(Arg(c, 3));
  return hr;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetRasterStatus", D3DDev8_GetRasterStatus, 2) {
  return Call9(c, "IDirect3DDevice9::GetRasterStatus", {Arg(c, 0), 0, Arg(c, 1)});
}

// Gamma ramps are not applied; the ramp read back is the one set (identity
// until then).
std::array<uint16_t, 768> g_gamma = [] {
  std::array<uint16_t, 768> r{};
  for (int i = 0; i < 256; i++) r[size_t(i)] = r[size_t(256 + i)] = r[size_t(512 + i)] = uint16_t(i * 257);
  return r;
}();
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetGammaRamp", D3DDev8_SetGammaRamp, 3) {
  std::memcpy(g_gamma.data(), GPtr(Arg(c, 2)), sizeof g_gamma);
  return 0;
}
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetGammaRamp", D3DDev8_GetGammaRamp, 2) {
  std::memcpy(GPtr(Arg(c, 1)), g_gamma.data(), sizeof g_gamma);
  return 0;
}

// ---- IDirect3DDevice8: resources -------------------------------------------------------

// (Width, Height, Levels, Usage, Format, Pool, ppTexture)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateTexture", D3DDev8_CreateTexture, 8) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::CreateTexture",
                      {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5), Arg(c, 6),
                       Arg(c, 7), 0});
  Out8(Arg(c, 7));
  return hr;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateVolumeTexture", D3DDev8_CreateVolumeTexture, 9) {
  FatalF("d3d8: CreateVolumeTexture is not supported");
}

// (EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateCubeTexture", D3DDev8_CreateCubeTexture, 7) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::CreateCubeTexture",
                      {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5), Arg(c, 6), 0});
  Out8(Arg(c, 6));
  return hr;
}

// (Length, Usage, FVF or Format, Pool, ppBuffer)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateVertexBuffer", D3DDev8_CreateVertexBuffer, 6) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::CreateVertexBuffer",
                      {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5), 0});
  Out8(Arg(c, 5));
  return hr;
}
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateIndexBuffer", D3DDev8_CreateIndexBuffer, 6) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::CreateIndexBuffer",
                      {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5), 0});
  Out8(Arg(c, 5));
  return hr;
}

// (Width, Height, Format, MultiSample, Lockable, ppSurface)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateRenderTarget", D3DDev8_CreateRenderTarget, 7) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::CreateRenderTarget",
                      {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), 0, Arg(c, 5), Arg(c, 6), 0});
  Out8(Arg(c, 6));
  return hr;
}

// (Width, Height, Format, MultiSample, ppSurface)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateDepthStencilSurface", D3DDev8_CreateDepthStencilSurface, 6) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::CreateDepthStencilSurface",
                      {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), 0, 0, Arg(c, 5), 0});
  Out8(Arg(c, 5));
  return hr;
}

// (Width, Height, Format, ppSurface): a lockable system-memory surface.
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateImageSurface", D3DDev8_CreateImageSurface, 5) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::CreateOffscreenPlainSurface",
                      {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), D3DPOOL_SYSTEMMEM, Arg(c, 4), 0});
  Out8(Arg(c, 4));
  return hr;
}

// Memory behind a lockable surface: pointer and pitch of its pixels.
bool SurfaceMemory(Ctx& c, uint32_t surf, uint32_t* bits, uint32_t* pitch) {
  Surface* s = com::As<Surface>(surf);
  if (!s) return false;
  if (s->kind == Surface::Kind::Offscreen) {
    *bits = s->mem;
    *pitch = s->pitch;
    return true;
  }
  if (s->kind == Surface::Kind::TextureLevel && s->texture &&
      !((s->texture->usage & D3DUSAGE_RENDERTARGET) && s->texture->pool == D3DPOOL_DEFAULT))
    return true;  // through LockRect (it uploads on unlock)
  return false;
}

// (pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface,
// pDestPointsArray): copies between lockable surfaces of one format.
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CopyRects", D3DDev8_CopyRects, 6) {
  uint32_t src = Arg(c, 1), rects = Arg(c, 2), count = Arg(c, 3), dst = Arg(c, 4), points = Arg(c, 5);
  Surface* ss = com::As<Surface>(src);
  Surface* ds = com::As<Surface>(dst);
  if (!ss || !ds || ss->desc.Format != ds->desc.Format) return D3DERR_INVALIDCALL;
  uint32_t b, p;
  if (!SurfaceMemory(c, src, &b, &p) || !SurfaceMemory(c, dst, &b, &p))
    FatalF("d3d8: CopyRects between surfaces of kinds %d and %d is not supported", int(ss->kind),
           int(ds->kind));
  gfx::Format f = FormatFromD3D(ss->desc.Format);
  Scratch s(c);
  uint32_t lr_src = s.Alloc(8), lr_dst = s.Alloc(8);
  if (Call9(c, "IDirect3DSurface9::LockRect", {src, lr_src, 0, D3DLOCK_READONLY}) != D3D_OK)
    return D3DERR_INVALIDCALL;
  if (Call9(c, "IDirect3DSurface9::LockRect", {dst, lr_dst, 0, 0}) != D3D_OK) {
    Call9(c, "IDirect3DSurface9::UnlockRect", {src});
    return D3DERR_INVALIDCALL;
  }
  const auto* l1 = G<D3DLOCKED_RECT>(lr_src);
  const auto* l2 = G<D3DLOCKED_RECT>(lr_dst);
  uint32_t n = rects ? count : 1;
  bool block = gfx::IsCompressed(f);
  uint32_t unit = block ? gfx::RowPitch(f, 4) : gfx::BitsPerPixel(f) / 8;  // bytes per pixel or block
  for (uint32_t i = 0; i < n; i++) {
    gfx::Rect r{0, 0, int32_t(ss->desc.Width), int32_t(ss->desc.Height)};
    if (rects) r = G<gfx::Rect>(rects)[i];
    int32_t dx = points ? G<GPOINT>(points)[i].x : r.left;
    int32_t dy = points ? G<GPOINT>(points)[i].y : r.top;
    int32_t w = r.right - r.left, h = r.bottom - r.top;
    if (block) {
      r.left /= 4; r.top /= 4; dx /= 4; dy /= 4;
      w = (w + 3) / 4; h = (h + 3) / 4;
    }
    for (int32_t y = 0; y < h; y++)
      std::memmove(GPtr(l2->pBits + uint32_t(dy + y) * uint32_t(l2->Pitch) + uint32_t(dx) * unit),
                   GPtr(l1->pBits + uint32_t(r.top + y) * uint32_t(l1->Pitch) + uint32_t(r.left) * unit),
                   size_t(w) * unit);
  }
  Call9(c, "IDirect3DSurface9::UnlockRect", {dst});
  Call9(c, "IDirect3DSurface9::UnlockRect", {src});
  return D3D_OK;
}

// (pSourceTexture, pDestinationTexture): every level the destination has,
// from the source's levels of the same size.
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::UpdateTexture", D3DDev8_UpdateTexture, 3) {
  uint32_t src = Arg(c, 1), dst = Arg(c, 2);
  Texture* st = com::As<Texture>(src);
  Texture* dt = com::As<Texture>(dst);
  if (!st || !dt || st->cube != dt->cube || st->format != dt->format) return D3DERR_INVALIDCALL;
  if (st->cube) FatalF("d3d8: UpdateTexture of cube textures is not supported");
  uint32_t skip = 0;
  while (skip < st->levels && st->At(0, skip).w != dt->width) skip++;
  Scratch s(c);
  uint32_t l1 = s.Alloc(8), l2 = s.Alloc(8);
  for (uint32_t lv = 0; lv < dt->levels && lv + skip < st->levels; lv++) {
    if (Call9(c, "IDirect3DTexture9::LockRect", {src, lv + skip, l1, 0, D3DLOCK_READONLY}) != D3D_OK)
      return D3DERR_INVALIDCALL;
    if (Call9(c, "IDirect3DTexture9::LockRect", {dst, lv, l2, 0, 0}) != D3D_OK) {
      Call9(c, "IDirect3DTexture9::UnlockRect", {src, lv + skip});
      return D3DERR_INVALIDCALL;
    }
    const Texture::Level& l = dt->At(0, lv);
    uint32_t row = gfx::RowPitch(dt->fmt, l.w), rows = gfx::RowCount(dt->fmt, l.h);
    const auto* a = G<D3DLOCKED_RECT>(l1);
    const auto* b = G<D3DLOCKED_RECT>(l2);
    for (uint32_t y = 0; y < rows; y++)
      std::memcpy(GPtr(b->pBits + y * uint32_t(b->Pitch)), GPtr(a->pBits + y * uint32_t(a->Pitch)), row);
    Call9(c, "IDirect3DTexture9::UnlockRect", {dst, lv});
    Call9(c, "IDirect3DTexture9::UnlockRect", {src, lv + skip});
  }
  return D3D_OK;
}

// (pDestSurface): the last presented frame into an A8R8G8B8 image surface
// of the screen's size.
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetFrontBuffer", D3DDev8_GetFrontBuffer, 2) {
  Surface* s = com::As<Surface>(Arg(c, 1));
  if (!s || s->kind != Surface::Kind::Offscreen || s->desc.Format != D3DFMT_A8R8G8B8)
    return D3DERR_INVALIDCALL;
  Device* d = Dev(c);
  uint32_t w = std::min(s->desc.Width, d->pp.BackBufferWidth);
  uint32_t h = std::min(s->desc.Height, d->pp.BackBufferHeight);
  d->gfx->ReadBackFront({0, 0, int32_t(w), int32_t(h)}, GPtr(s->mem), s->pitch, gfx::Format::A8R8G8B8);
  return D3D_OK;
}

// (pRenderTarget, pNewZStencil): a null target keeps the current one.
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetRenderTarget", D3DDev8_SetRenderTarget, 3) {
  uint32_t hr = D3D_OK;
  if (Arg(c, 1)) hr = Call9(c, "IDirect3DDevice9::SetRenderTarget", {Arg(c, 0), 0, Arg(c, 1)});
  if (hr != D3D_OK) return hr;
  return Call9(c, "IDirect3DDevice9::SetDepthStencilSurface", {Arg(c, 0), Arg(c, 2)});
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetRenderTarget", D3DDev8_GetRenderTarget, 2) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::GetRenderTarget", {Arg(c, 0), 0, Arg(c, 1)});
  Out8(Arg(c, 1));
  return hr;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetDepthStencilSurface", D3DDev8_GetDepthStencilSurface, 2) {
  uint32_t hr = Call9(c, "IDirect3DDevice9::GetDepthStencilSurface", {Arg(c, 0), Arg(c, 1)});
  Out8(Arg(c, 1));
  return hr;
}

// ---- IDirect3DDevice8: state (recorded while a state block is) ------------------------

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetTransform", D3DDev8_SetTransform, 3) {
  const float* m = reinterpret_cast<const float*>(GPtr(Arg(c, 2)));
  if (StateBlock* b = X(c).recording) {
    std::memcpy(b->transforms[uint32_t(Arg(c, 1))].data(), m, 64);
    return D3D_OK;
  }
  SetXform(*Dev(c), Arg(c, 1), m);
  return D3D_OK;
}

// (State, pMatrix): the transform becomes pMatrix * transform.
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::MultiplyTransform", D3DDev8_MultiplyTransform, 3) {
  Device* d = Dev(c);
  std::array<float, 16> a{};
  auto it = d->transforms.find(Arg(c, 1));
  if (it != d->transforms.end())
    a = it->second;
  else
    a[0] = a[5] = a[10] = a[15] = 1;
  const float* b = reinterpret_cast<const float*>(GPtr(Arg(c, 2)));
  float r[16];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += b[i * 4 + k] * a[size_t(k * 4 + j)];
      r[i * 4 + j] = s;
    }
  SetXform(*d, Arg(c, 1), r);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetViewport", D3DDev8_SetViewport, 2) {
  const D3DVIEWPORT9& v = *G<D3DVIEWPORT9>(Arg(c, 1));
  if (StateBlock* b = X(c).recording) {
    b->viewport = v;
    return D3D_OK;
  }
  Dev(c)->viewport = v;
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetMaterial", D3DDev8_SetMaterial, 2) {
  gfx::Material m;
  std::memcpy(&m, GPtr(Arg(c, 1)), sizeof m);
  if (StateBlock* b = X(c).recording) {
    b->material = m;
    return D3D_OK;
  }
  SetMat(*Dev(c), X(c), m);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetMaterial", D3DDev8_GetMaterial, 2) {
  std::memcpy(GPtr(Arg(c, 1)), &X(c).material, sizeof(gfx::Material));
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetLight", D3DDev8_SetLight, 3) {
  gfx::Light l;
  std::memcpy(&l, GPtr(Arg(c, 2)), sizeof l);
  if (StateBlock* b = X(c).recording) {
    b->lights[uint32_t(Arg(c, 1))] = l;
    return D3D_OK;
  }
  SetLightAt(*Dev(c), X(c), Arg(c, 1), l);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetLight", D3DDev8_GetLight, 3) {
  Device8& x = X(c);
  auto it = x.lights.find(Arg(c, 1));
  if (it == x.lights.end()) return D3DERR_INVALIDCALL;
  std::memcpy(GPtr(Arg(c, 2)), &it->second, sizeof(gfx::Light));
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::LightEnable", D3DDev8_LightEnable, 3) {
  if (StateBlock* b = X(c).recording) {
    b->light_on[uint32_t(Arg(c, 1))] = Arg(c, 2) != 0;
    return D3D_OK;
  }
  EnableLight(*Dev(c), X(c), Arg(c, 1), Arg(c, 2) != 0);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetLightEnable", D3DDev8_GetLightEnable, 3) {
  Device8& x = X(c);
  if (!x.lights.count(Arg(c, 1))) return D3DERR_INVALIDCALL;
  Wr32(Arg(c, 2), x.light_on[uint32_t(Arg(c, 1))] ? 1 : 0);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetClipPlane", D3DDev8_SetClipPlane, 3) {
  const float* p = reinterpret_cast<const float*>(GPtr(Arg(c, 2)));
  if (Arg(c, 1) >= 32) return D3DERR_INVALIDCALL;
  if (StateBlock* b = X(c).recording) {
    std::memcpy(b->clip[uint32_t(Arg(c, 1))].data(), p, 16);
    return D3D_OK;
  }
  SetClip(*Dev(c), X(c), Arg(c, 1), p);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetClipPlane", D3DDev8_GetClipPlane, 3) {
  if (Arg(c, 1) >= 32) return D3DERR_INVALIDCALL;
  std::memcpy(GPtr(Arg(c, 2)), X(c).clip[Arg(c, 1)], 16);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetRenderState", D3DDev8_SetRenderState, 3) {
  uint32_t s = Arg(c, 1), v = Arg(c, 2);
  if (s >= 256) return D3DERR_INVALIDCALL;
  if (StateBlock* b = X(c).recording) {
    b->rs[s] = v;
    return D3D_OK;
  }
  SetRS(*Dev(c), s, v);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetTexture", D3DDev8_GetTexture, 3) {
  if (Arg(c, 1) >= 8) return D3DERR_INVALIDCALL;
  uint32_t t = Dev(c)->textures[Arg(c, 1)].guest;
  if (t) com::AddRef(t);
  Wr32(Arg(c, 2), As8(t));
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetTexture", D3DDev8_SetTexture, 3) {
  if (Arg(c, 1) >= 8) return D3DERR_INVALIDCALL;
  if (StateBlock* b = X(c).recording) {
    b->textures[uint32_t(Arg(c, 1))] = Arg(c, 2);
    return D3D_OK;
  }
  SetTex(*Dev(c), Arg(c, 1), Arg(c, 2));
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetTextureStageState", D3DDev8_GetTextureStageState, 4) {
  uint32_t st = Arg(c, 1), t = Arg(c, 2);
  if (st >= 8 || t >= 33) return D3DERR_INVALIDCALL;
  Wr32(Arg(c, 3), GetTSS(*Dev(c), st, t));
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetTextureStageState", D3DDev8_SetTextureStageState, 4) {
  uint32_t st = Arg(c, 1), t = Arg(c, 2), v = Arg(c, 3);
  if (st >= 8 || t >= 33) return D3DERR_INVALIDCALL;
  if (StateBlock* b = X(c).recording) {
    b->tss[{st, t}] = v;
    return D3D_OK;
  }
  SetTSS(*Dev(c), st, t, v);
  return D3D_OK;
}

// ---- state blocks ------------------------------------------------------------------

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::BeginStateBlock", D3DDev8_BeginStateBlock, 1) {
  Device8& x = X(c);
  if (x.recording) return D3DERR_INVALIDCALL;
  x.recorded = StateBlock{};
  x.recording = &x.recorded;
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::EndStateBlock", D3DDev8_EndStateBlock, 2) {
  Device8& x = X(c);
  if (!x.recording) return D3DERR_INVALIDCALL;
  x.recording = nullptr;
  uint32_t h = x.next_block++;
  x.blocks[h] = std::move(x.recorded);
  Wr32(Arg(c, 1), h);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::ApplyStateBlock", D3DDev8_ApplyStateBlock, 2) {
  Device8& x = X(c);
  auto it = x.blocks.find(Arg(c, 1));
  if (it == x.blocks.end() || x.recording) return D3DERR_INVALIDCALL;
  Apply(*Dev(c), x, it->second);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CaptureStateBlock", D3DDev8_CaptureStateBlock, 2) {
  Device8& x = X(c);
  auto it = x.blocks.find(Arg(c, 1));
  if (it == x.blocks.end() || x.recording) return D3DERR_INVALIDCALL;
  Capture(*Dev(c), x, it->second);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::DeleteStateBlock", D3DDev8_DeleteStateBlock, 2) {
  return X(c).blocks.erase(Arg(c, 1)) ? D3D_OK : D3DERR_INVALIDCALL;
}

// (Type, pToken): D3DSBT_ALL (1) only so far.
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateStateBlock", D3DDev8_CreateStateBlock, 3) {
  Device8& x = X(c);
  if (Arg(c, 1) != 1)
    FatalF("d3d8: CreateStateBlock type %u (only D3DSBT_ALL is supported)", uint32_t(Arg(c, 1)));
  uint32_t h = x.next_block++;
  x.blocks[h] = AllStates(*Dev(c), x);
  Wr32(Arg(c, 2), h);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetClipStatus", D3DDev8_SetClipStatus, 2) { return D3D_OK; }
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetClipStatus", D3DDev8_GetClipStatus, 2) {
  std::memset(GPtr(Arg(c, 1)), 0, 8);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::ValidateDevice", D3DDev8_ValidateDevice, 2) {
  Wr32(Arg(c, 1), 1);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetInfo", D3DDev8_GetInfo, 4) { return S_FALSE; }

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetPaletteEntries", D3DDev8_SetPaletteEntries, 3) { return D3D_OK; }
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetPaletteEntries", D3DDev8_GetPaletteEntries, 3) {
  return D3DERR_INVALIDCALL;
}
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetCurrentTexturePalette", D3DDev8_SetCurrentTexturePalette, 2) {
  return D3D_OK;
}
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetCurrentTexturePalette", D3DDev8_GetCurrentTexturePalette, 2) {
  Wr32(Arg(c, 1), 0);
  return D3D_OK;
}

// ---- drawing -------------------------------------------------------------------------

// (PrimitiveType, MinIndex, NumVertices, StartIndex, PrimitiveCount): the
// base vertex index is SetIndices'.
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::DrawIndexedPrimitive", D3DDev8_DrawIndexedPrimitive, 6) {
  return Call9(c, "IDirect3DDevice9::DrawIndexedPrimitive",
               {Arg(c, 0), Arg(c, 1), X(c).base_vertex, Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5)});
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::ProcessVertices", D3DDev8_ProcessVertices, 6) {
  FatalF("d3d8: ProcessVertices is not supported");
}

// ---- shaders --------------------------------------------------------------------------

// (pDeclaration, pFunction, pHandle, Usage)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreateVertexShader", D3DDev8_CreateVertexShader, 5) {
  uint32_t self = Arg(c, 0), func = Arg(c, 2), out = Arg(c, 3);
  Device8& x = X(c);
  VertexShader8 s;
  std::vector<D3DVERTEXELEMENT9> elems;
  std::vector<uint32_t> regs;
  ParseDecl(Arg(c, 1), &s, &elems, &regs);
  Scratch sc(c);
  uint32_t e9 = sc.Alloc(uint32_t(elems.size() * sizeof(D3DVERTEXELEMENT9)));
  std::memcpy(GPtr(e9), elems.data(), elems.size() * sizeof(D3DVERTEXELEMENT9));
  uint32_t pdecl = sc.Alloc(4);
  uint32_t hr = Call9(c, "IDirect3DDevice9::CreateVertexDeclaration", {self, e9, pdecl});
  if (hr != D3D_OK) return hr;
  s.decl9 = Rd32(pdecl);
  if (func) {
    s.func = ReadTokens(func);
    // The function with dcl_texcoordN vN for the registers the
    // declaration loads, after the version token.
    std::vector<uint32_t> t{s.func[0]};
    for (uint32_t r : regs) {
      t.push_back(0x0000001F);                     // D3DSIO_DCL
      t.push_back(0x80000000u | (r << 16) | 5u);   // usage TEXCOORD, index r
      t.push_back(0x900F0000u | r);                // v<r>, all components
    }
    t.insert(t.end(), s.func.begin() + 1, s.func.end());
    uint32_t f9 = sc.Alloc(uint32_t(t.size() * 4));
    std::memcpy(GPtr(f9), t.data(), t.size() * 4);
    uint32_t pvs = sc.Alloc(4);
    hr = Call9(c, "IDirect3DDevice9::CreateVertexShader", {self, f9, pvs});
    if (hr != D3D_OK) {
      com::Release(s.decl9);
      return hr;
    }
    s.vs9 = Rd32(pvs);
  }
  uint32_t h = kVsHandleBase + 2 * x.next_vs++;
  x.vshaders[h] = std::move(s);
  Wr32(out, h);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetVertexShader", D3DDev8_SetVertexShader, 2) {
  if (StateBlock* b = X(c).recording) {
    b->vs = uint32_t(Arg(c, 1));
    return D3D_OK;
  }
  SetVs(*Dev(c), X(c), Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetVertexShader", D3DDev8_GetVertexShader, 2) {
  Wr32(Arg(c, 1), X(c).vs);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::DeleteVertexShader", D3DDev8_DeleteVertexShader, 2) {
  Device8& x = X(c);
  auto it = x.vshaders.find(Arg(c, 1));
  if (it == x.vshaders.end()) return D3DERR_INVALIDCALL;
  if (it->second.vs9) com::Release(it->second.vs9);
  if (it->second.decl9) com::Release(it->second.decl9);
  x.vshaders.erase(it);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetVertexShaderConstant", D3DDev8_SetVertexShaderConstant, 4) {
  uint32_t reg = Arg(c, 1), data = Arg(c, 2), count = Arg(c, 3);
  if (reg > 256 || count > 256 - reg) return D3DERR_INVALIDCALL;
  if (StateBlock* b = X(c).recording) {
    for (uint32_t i = 0; i < count; i++) std::memcpy(b->vsc[reg + i].data(), GPtr(data + 16 * i), 16);
    return D3D_OK;
  }
  std::memcpy(Dev(c)->vs_f[reg], GPtr(data), size_t(count) * 16);
  return D3D_OK;
}

// (Handle, pData, pSizeOfData)
uint32_t CopyTokens(Ctx& c, const std::vector<uint32_t>& t) {
  uint32_t size = uint32_t(t.size() * 4);
  if (Arg(c, 2)) {
    if (Rd32(Arg(c, 3)) < size) {
      Wr32(Arg(c, 3), size);
      return 0x88760870;  // D3DERR_MOREDATA
    }
    std::memcpy(GPtr(Arg(c, 2)), t.data(), size);
  }
  Wr32(Arg(c, 3), size);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetVertexShaderDeclaration", D3DDev8_GetVertexShaderDeclaration, 4) {
  auto it = X(c).vshaders.find(Arg(c, 1));
  if (it == X(c).vshaders.end()) return D3DERR_INVALIDCALL;
  return CopyTokens(c, it->second.decl);
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetVertexShaderFunction", D3DDev8_GetVertexShaderFunction, 4) {
  auto it = X(c).vshaders.find(Arg(c, 1));
  if (it == X(c).vshaders.end()) return D3DERR_INVALIDCALL;
  return CopyTokens(c, it->second.func);
}

// (StreamNumber, pStreamData, Stride)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetStreamSource", D3DDev8_SetStreamSource, 4) {
  if (Arg(c, 1) >= 16) return D3DERR_INVALIDCALL;
  if (StateBlock* b = X(c).recording) {
    b->streams[uint32_t(Arg(c, 1))] = {uint32_t(Arg(c, 2)), uint32_t(Arg(c, 3))};
    return D3D_OK;
  }
  SetStream(*Dev(c), Arg(c, 1), Arg(c, 2), Arg(c, 3));
  return D3D_OK;
}

// (StreamNumber, ppStreamData, pStride)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetStreamSource", D3DDev8_GetStreamSource, 4) {
  if (Arg(c, 1) >= 16) return D3DERR_INVALIDCALL;
  const Device::Stream& s = Dev(c)->streams[Arg(c, 1)];
  if (s.buffer.guest) com::AddRef(s.buffer.guest);
  Wr32(Arg(c, 2), As8(s.buffer.guest));
  Wr32(Arg(c, 3), s.stride);
  return D3D_OK;
}

// (pIndexData, BaseVertexIndex)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetIndices", D3DDev8_SetIndices, 3) {
  if (StateBlock* b = X(c).recording) {
    b->indices = std::make_pair(uint32_t(Arg(c, 1)), uint32_t(Arg(c, 2)));
    return D3D_OK;
  }
  SetIdx(*Dev(c), X(c), Arg(c, 1), Arg(c, 2));
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetIndices", D3DDev8_GetIndices, 3) {
  uint32_t ib = Dev(c)->indices.guest;
  if (ib) com::AddRef(ib);
  Wr32(Arg(c, 1), As8(ib));
  Wr32(Arg(c, 2), X(c).base_vertex);
  return D3D_OK;
}

// (pFunction, pHandle)
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::CreatePixelShader", D3DDev8_CreatePixelShader, 3) {
  Device8& x = X(c);
  PixelShader8 s;
  s.func = ReadTokens(Arg(c, 1));
  Scratch sc(c);
  uint32_t pps = sc.Alloc(4);
  uint32_t hr = Call9(c, "IDirect3DDevice9::CreatePixelShader", {Arg(c, 0), Arg(c, 1), pps});
  if (hr != D3D_OK) return hr;
  s.ps9 = Rd32(pps);
  uint32_t h = x.next_ps++;
  x.pshaders[h] = std::move(s);
  Wr32(Arg(c, 2), h);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetPixelShader", D3DDev8_SetPixelShader, 2) {
  if (StateBlock* b = X(c).recording) {
    b->ps = uint32_t(Arg(c, 1));
    return D3D_OK;
  }
  SetPs(*Dev(c), X(c), Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetPixelShader", D3DDev8_GetPixelShader, 2) {
  Wr32(Arg(c, 1), X(c).ps);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::DeletePixelShader", D3DDev8_DeletePixelShader, 2) {
  Device8& x = X(c);
  auto it = x.pshaders.find(Arg(c, 1));
  if (it == x.pshaders.end()) return D3DERR_INVALIDCALL;
  if (it->second.ps9) com::Release(it->second.ps9);
  x.pshaders.erase(it);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::SetPixelShaderConstant", D3DDev8_SetPixelShaderConstant, 4) {
  uint32_t reg = Arg(c, 1), data = Arg(c, 2), count = Arg(c, 3);
  if (reg > 224 || count > 224 - reg) return D3DERR_INVALIDCALL;
  if (StateBlock* b = X(c).recording) {
    for (uint32_t i = 0; i < count; i++) std::memcpy(b->psc[reg + i].data(), GPtr(data + 16 * i), 16);
    return D3D_OK;
  }
  std::memcpy(Dev(c)->ps_f[reg], GPtr(data), size_t(count) * 16);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::GetPixelShaderFunction", D3DDev8_GetPixelShaderFunction, 4) {
  auto it = X(c).pshaders.find(Arg(c, 1));
  if (it == X(c).pshaders.end()) return D3DERR_INVALIDCALL;
  return CopyTokens(c, it->second.func);
}

HLE_NAMED("d3d8.dll", "IDirect3DDevice8::DrawRectPatch", D3DDev8_DrawRectPatch, 4) {
  FatalF("d3d8: DrawRectPatch is not supported");
}
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::DrawTriPatch", D3DDev8_DrawTriPatch, 4) {
  FatalF("d3d8: DrawTriPatch is not supported");
}
HLE_NAMED("d3d8.dll", "IDirect3DDevice8::DeletePatch", D3DDev8_DeletePatch, 2) { return D3D_OK; }

// ---- resources -------------------------------------------------------------------------

#define D3D8_PRIVATE_DATA(Iface, Prefix)                                                       \
  HLE_NAMED("d3d8.dll", Iface "::SetPrivateData", Prefix##_SetPrivateData, 5) { return SetPrivate(c); } \
  HLE_NAMED("d3d8.dll", Iface "::GetPrivateData", Prefix##_GetPrivateData, 4) { return GetPrivate(c); } \
  HLE_NAMED("d3d8.dll", Iface "::FreePrivateData", Prefix##_FreePrivateData, 2) { return FreePrivate(c); }

D3D8_PRIVATE_DATA("IDirect3DSurface8", D3DSurf8)
D3D8_PRIVATE_DATA("IDirect3DTexture8", D3DTex8)
D3D8_PRIVATE_DATA("IDirect3DVertexBuffer8", D3DVB8)
D3D8_PRIVATE_DATA("IDirect3DIndexBuffer8", D3DIB8)

HLE_NAMED("d3d8.dll", "IDirect3DSurface8::QueryInterface", D3DSurf8_QueryInterface, 3) {
  return QueryFor(c, {kIID_IDirect3DSurface8});
}

HLE_NAMED("d3d8.dll", "IDirect3DSurface8::GetDesc", D3DSurf8_GetDesc, 2) {
  Surface* s = com::As<Surface>(Arg(c, 0));
  if (!s) FatalF("IDirect3DSurface8 call on unknown object %08X", uint32_t(Arg(c, 0)));
  *G<D3DSURFACE_DESC8>(Arg(c, 1)) = Desc8(s->desc);
  return D3D_OK;
}

// (riid, ppContainer): a texture level's texture, otherwise the device.
HLE_NAMED("d3d8.dll", "IDirect3DSurface8::GetContainer", D3DSurf8_GetContainer, 3) {
  Surface* s = com::As<Surface>(Arg(c, 0));
  if (!s) FatalF("IDirect3DSurface8 call on unknown object %08X", uint32_t(Arg(c, 0)));
  uint32_t riid = Arg(c, 1), out = Arg(c, 2);
  if (s->container) {
    bool tex = com::GuidEq(riid, s->texture->cube ? kIID_IDirect3DCubeTexture8 : kIID_IDirect3DTexture8) ||
               com::GuidEq(riid, kIID_IDirect3DBaseTexture8) ||
               com::GuidEq(riid, kIID_IDirect3DResource8) || com::GuidEq(riid, kIID_IUnknown);
    if (!tex) {
      Wr32(out, 0);
      return com::kE_NOINTERFACE;
    }
    com::AddRef(s->container);
    Wr32(out, As8(s->container));
    return D3D_OK;
  }
  com::AddRef(s->device);
  Wr32(out, s->device);
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DTexture8::QueryInterface", D3DTex8_QueryInterface, 3) {
  Texture* t = com::As<Texture>(Arg(c, 0));
  return QueryFor(c, {t && t->cube ? kIID_IDirect3DCubeTexture8 : kIID_IDirect3DTexture8,
                      kIID_IDirect3DBaseTexture8, kIID_IDirect3DResource8});
}

// (Level, pDesc)
HLE_NAMED("d3d8.dll", "IDirect3DTexture8::GetLevelDesc", D3DTex8_GetLevelDesc, 3) {
  Texture* t = com::As<Texture>(Arg(c, 0));
  if (!t) FatalF("IDirect3DTexture8 call on unknown object %08X", uint32_t(Arg(c, 0)));
  uint32_t level = Arg(c, 1);
  if (level >= t->levels) return D3DERR_INVALIDCALL;
  const Texture::Level& l = t->At(0, level);
  *G<D3DSURFACE_DESC8>(Arg(c, 2)) =
      Desc8({t->format, D3DRTYPE_SURFACE, t->usage, t->pool, 0, 0, l.w, l.h});
  return D3D_OK;
}

HLE_NAMED("d3d8.dll", "IDirect3DTexture8::GetSurfaceLevel", D3DTex8_GetSurfaceLevel, 3) {
  uint32_t hr = Call9(c, "IDirect3DTexture9::GetSurfaceLevel", {Arg(c, 0), Arg(c, 1), Arg(c, 2)});
  Out8(Arg(c, 2));
  return hr;
}

// (FaceType, Level, ppCubeMapSurface)
HLE_NAMED("d3d8.dll", "IDirect3DCubeTexture8::GetCubeMapSurface", D3DCube8_GetCubeMapSurface, 4) {
  uint32_t hr = Call9(c, "IDirect3DCubeTexture9::GetCubeMapSurface",
                      {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3)});
  Out8(Arg(c, 3));
  return hr;
}

HLE_NAMED("d3d8.dll", "IDirect3DVertexBuffer8::QueryInterface", D3DVB8_QueryInterface, 3) {
  return QueryFor(c, {kIID_IDirect3DVertexBuffer8, kIID_IDirect3DResource8});
}
HLE_NAMED("d3d8.dll", "IDirect3DIndexBuffer8::QueryInterface", D3DIB8_QueryInterface, 3) {
  return QueryFor(c, {kIID_IDirect3DIndexBuffer8, kIID_IDirect3DResource8});
}

}  // namespace
}  // namespace recomp::hle
