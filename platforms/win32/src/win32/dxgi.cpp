// dxgi.dll (DXGI 1.1) for x64 guests: one factory, one adapter (the host
// GPU, with the video memory GameConfig::video_memory_mb names) and one
// output (the host desktop), whose display modes are the desktop's own
// size and the common 16:9 sizes below it. A swap chain presents a
// Direct3D 11 device's frames to its window: the first one makes the
// device's gfx::Device there. Methods are handlers named
// "<Interface>::<Method>", as in d3d11.cpp.

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "runtime/display.h"
#include "runtime/env.h"
#include "runtime/gfx/present.h"
#include "runtime/gfx/shader_compile.h"
#include "runtime/log.h"
#include "win32/com.h"
#include "win32/d3d11_impl.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"
#include "x86/runtime.h"

namespace recomp::hle::d3d11 {

namespace {

constexpr const char* kDll = "dxgi.dll";
constexpr uint32_t kFormatR8G8B8A8Unorm = 28, kFormatB8G8R8A8Unorm = 87;

constexpr guest::GUID kIID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr guest::GUID kIID_IDXGIObject = {
    0xaec22fb8, 0x76f3, 0x4639, {0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e}};
constexpr guest::GUID kIID_IDXGIFactory = {
    0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
constexpr guest::GUID kIID_IDXGIFactory1 = {
    0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
constexpr guest::GUID kIID_IDXGIAdapter = {
    0x2411e7e1, 0x12ac, 0x4ccf, {0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0}};
constexpr guest::GUID kIID_IDXGIAdapter1 = {
    0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};
constexpr guest::GUID kIID_IDXGIOutput = {
    0xae02eedb, 0xc735, 0x4690, {0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa}};

struct Factory : com::Object {};
struct Adapter : com::Object {};
struct Output : com::Object {};

gaddr FactoryVtable() {
  return com::Vtable(kDll, "IDXGIFactory1",
                     {"QueryInterface", "AddRef", "Release", "SetPrivateData",
                      "SetPrivateDataInterface", "GetPrivateData", "GetParent", "EnumAdapters",
                      "MakeWindowAssociation", "GetWindowAssociation", "CreateSwapChain",
                      "CreateSoftwareAdapter", "EnumAdapters1", "IsCurrent"});
}
gaddr AdapterVtable() {
  return com::Vtable(kDll, "IDXGIAdapter1",
                     {"QueryInterface", "AddRef", "Release", "SetPrivateData",
                      "SetPrivateDataInterface", "GetPrivateData", "GetParent", "EnumOutputs",
                      "GetDesc", "CheckInterfaceSupport", "GetDesc1"});
}
gaddr OutputVtable() {
  return com::Vtable(kDll, "IDXGIOutput",
                     {"QueryInterface", "AddRef", "Release", "SetPrivateData",
                      "SetPrivateDataInterface", "GetPrivateData", "GetParent", "GetDesc",
                      "GetDisplayModeList", "FindClosestMatchingMode", "WaitForVBlank",
                      "TakeOwnership", "ReleaseOwnership", "GetGammaControlCapabilities",
                      "SetGammaControl", "GetGammaControl", "SetDisplaySurface",
                      "GetDisplaySurfaceData", "GetFrameStatistics"});
}

// One of each, kept: every EnumAdapters/EnumOutputs hands out the same
// object with a reference added, until the guest has released it for good;
// the next one is new.
std::shared_ptr<Factory> g_factory;
std::shared_ptr<Adapter> g_adapter;
std::shared_ptr<Output> g_output;

template <typename T>
bool Alive(const std::shared_ptr<T>& o) {
  return o && com::Lookup(o->guest).get() == o.get();
}

gaddr TheAdapter() {
  if (!Alive(g_adapter)) {
    g_adapter = std::make_shared<Adapter>();
    com::Publish(g_adapter, AdapterVtable());
  }
  return g_adapter->guest;
}
gaddr TheOutput() {
  if (!Alive(g_output)) {
    g_output = std::make_shared<Output>();
    com::Publish(g_output, OutputVtable());
  }
  return g_output->guest;
}

// QueryInterface for an object answering `iids`: the same pointer.
uint64_t QueryAs(Ctx& c, std::initializer_list<const guest::GUID*> iids, const char* what) {
  const gaddr iid = Arg(c, 1), out = Arg(c, 2);
  for (const guest::GUID* g : iids)
    if (com::GuidEq(iid, *g)) {
      com::AddRef(Arg(c, 0));
      Wr64(out, Arg(c, 0));
      return kS_OK;
    }
  guest::GUID g = com::ReadGuid(iid);
  WarnF("%s::QueryInterface({%08X-%04X-%04X-...}): not supported\n", what, g.Data1, g.Data2,
        g.Data3);
  Wr64(out, 0);
  return com::kE_NOINTERFACE;
}

// The display modes: the desktop's size, and the common 16:9 sizes that
// fit in it, smallest first, at the desktop's refresh rate.
std::vector<std::pair<int, int>> Modes() {
  int dw, dh;
  DesktopSize(&dw, &dh);
  std::vector<std::pair<int, int>> m;
  for (auto [w, h] : {std::pair{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}})
    if (w <= dw && h <= dh && !(w == dw && h == dh)) m.push_back({w, h});
  m.push_back({dw, dh});
  return m;
}

// DXGI_MODE_DESC {Width, Height, RefreshRate {Numerator, Denominator},
// Format, ScanlineOrdering, Scaling}, 28 bytes.
void PutMode(gaddr p, int w, int h, uint32_t format) {
  std::memset(GPtr(p), 0, 28);
  Wr32(p, uint32_t(w));
  Wr32(p + 4, uint32_t(h));
  Wr32(p + 8, uint32_t(DesktopRefreshRate()));
  Wr32(p + 12, 1);
  Wr32(p + 16, format);
}

}  // namespace

// IDXGIDevice1, the DXGI side of the D3D11 device: its adapter and frame
// latency. QueryInterface back to ID3D11Device gives the device.
struct DxgiDevice : com::Object {
  Device* device = nullptr;
  uint32_t max_latency = 3;
};
constexpr guest::GUID kIID_IDXGIDevice = {
    0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
constexpr guest::GUID kIID_IDXGIDevice1 = {
    0x77db970f, 0x6276, 0x48ba, {0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c}};
constexpr guest::GUID kIID_ID3D11Device = {
    0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};

gaddr DxgiDeviceOf(Device& d) {
  if (!Alive(d.dxgi)) {
    auto x = std::make_shared<DxgiDevice>();
    x->device = &d;
    com::Publish(x, com::Vtable(kDll, "IDXGIDevice1",
                                {"QueryInterface", "AddRef", "Release", "SetPrivateData",
                                 "SetPrivateDataInterface", "GetPrivateData", "GetParent",
                                 "GetAdapter", "CreateSurface", "QueryResourceResidency",
                                 "SetGPUThreadPriority", "GetGPUThreadPriority",
                                 "SetMaximumFrameLatency", "GetMaximumFrameLatency"}));
    d.dxgi = x;
  } else {
    com::AddRef(d.dxgi->guest);
  }
  return d.dxgi->guest;
}

// CreateDXGIFactory(riid, &factory), CreateDXGIFactory1(riid, &factory);
// the adapter's GetParent(riid, &factory).
uint64_t CreateFactory(gaddr iid, gaddr out) {
  if (!com::GuidEq(iid, kIID_IDXGIFactory) && !com::GuidEq(iid, kIID_IDXGIFactory1) &&
      !com::GuidEq(iid, kIID_IUnknown)) {
    Wr64(out, 0);
    return com::kE_NOINTERFACE;
  }
  if (!Alive(g_factory)) {
    g_factory = std::make_shared<Factory>();
    com::Publish(g_factory, FactoryVtable());
  } else {
    com::AddRef(g_factory->guest);
  }
  Wr64(out, g_factory->guest);
  return kS_OK;
}
HLE64(kDll, CreateDXGIFactory, kCdecl) { return CreateFactory(Arg(c, 0), Arg(c, 1)); }
HLE64(kDll, CreateDXGIFactory1, kCdecl) { return CreateFactory(Arg(c, 0), Arg(c, 1)); }

#define DXGI_METHOD(iface, name) HLE_NAMED_X(kDll, #iface "::" #name, iface##_##name, kCdecl, true)

// ---- IDXGIFactory1 ---------------------------------------------------------------

DXGI_METHOD(IDXGIFactory1, QueryInterface) {
  return QueryAs(c, {&kIID_IUnknown, &kIID_IDXGIObject, &kIID_IDXGIFactory, &kIID_IDXGIFactory1},
                 "IDXGIFactory1");
}
DXGI_METHOD(IDXGIFactory1, AddRef) { return com::AddRef(Arg(c, 0)); }
DXGI_METHOD(IDXGIFactory1, Release) { return com::Release(Arg(c, 0)); }
uint64_t EnumAdapters(Ctx& c) {
  if (uint32_t(Arg(c, 1)) != 0) {
    Wr64(Arg(c, 2), 0);
    return kDXGI_ERROR_NOT_FOUND;
  }
  gaddr a = TheAdapter();
  com::AddRef(a);
  Wr64(Arg(c, 2), a);
  return kS_OK;
}
DXGI_METHOD(IDXGIFactory1, EnumAdapters) { return EnumAdapters(c); }
DXGI_METHOD(IDXGIFactory1, EnumAdapters1) { return EnumAdapters(c); }
uint64_t CreateSwapChain(Ctx& c);
DXGI_METHOD(IDXGIFactory1, CreateSwapChain) { return CreateSwapChain(c); }
// The window's Alt+Enter and Print Screen handling are the host's anyway.
DXGI_METHOD(IDXGIFactory1, MakeWindowAssociation) { return kS_OK; }
DXGI_METHOD(IDXGIFactory1, IsCurrent) { return 1; }

// ---- IDXGIAdapter1 ---------------------------------------------------------------

DXGI_METHOD(IDXGIAdapter1, QueryInterface) {
  return QueryAs(c, {&kIID_IUnknown, &kIID_IDXGIObject, &kIID_IDXGIAdapter, &kIID_IDXGIAdapter1},
                 "IDXGIAdapter1");
}
DXGI_METHOD(IDXGIAdapter1, AddRef) { return com::AddRef(Arg(c, 0)); }
DXGI_METHOD(IDXGIAdapter1, Release) { return com::Release(Arg(c, 0)); }
DXGI_METHOD(IDXGIAdapter1, GetParent) { return CreateFactory(Arg(c, 1), Arg(c, 2)); }
DXGI_METHOD(IDXGIAdapter1, EnumOutputs) {
  if (uint32_t(Arg(c, 1)) != 0) {
    Wr64(Arg(c, 2), 0);
    return kDXGI_ERROR_NOT_FOUND;
  }
  gaddr o = TheOutput();
  com::AddRef(o);
  Wr64(Arg(c, 2), o);
  return kS_OK;
}
// DXGI_ADAPTER_DESC(1): Description WCHAR[128], VendorId, DeviceId,
// SubSysId, Revision, DedicatedVideoMemory @272, DedicatedSystemMemory,
// SharedSystemMemory, AdapterLuid @296 (304 bytes); DESC1 adds Flags @304.
uint64_t AdapterDesc(Ctx& c, bool desc1) {
  const gaddr d = Arg(c, 1);
  const win32::GameConfig& g = Game();
  std::memset(GPtr(d), 0, desc1 ? 312 : 304);
  PutWStr(d, g.adapter.description);
  Wr32(d + 256, g.adapter.vendor_id);
  Wr32(d + 260, g.adapter.device_id);
  Wr64(d + 272, uint64_t(g.video_memory_mb) << 20);
  Wr64(d + 288, 8ull << 30);  // shared system memory
  Wr32(d + 296, 1);           // a LUID other than 0
  return kS_OK;
}
DXGI_METHOD(IDXGIAdapter1, GetDesc) { return AdapterDesc(c, false); }
DXGI_METHOD(IDXGIAdapter1, GetDesc1) { return AdapterDesc(c, true); }

// ---- IDXGIOutput --------------------------------------------------------------------

DXGI_METHOD(IDXGIOutput, QueryInterface) {
  return QueryAs(c, {&kIID_IUnknown, &kIID_IDXGIObject, &kIID_IDXGIOutput}, "IDXGIOutput");
}
DXGI_METHOD(IDXGIOutput, AddRef) { return com::AddRef(Arg(c, 0)); }
DXGI_METHOD(IDXGIOutput, Release) { return com::Release(Arg(c, 0)); }
// DXGI_OUTPUT_DESC: DeviceName WCHAR[32], DesktopCoordinates RECT @64,
// AttachedToDesktop @80, Rotation @84 (DXGI_MODE_ROTATION_IDENTITY 1),
// Monitor @88; 96 bytes.
DXGI_METHOD(IDXGIOutput, GetDesc) {
  const gaddr d = Arg(c, 1);
  int w, h;
  DesktopSize(&w, &h);
  std::memset(GPtr(d), 0, 96);
  PutWStr(d, "\\\\.\\DISPLAY1");
  Wr32(d + 72, uint32_t(w));
  Wr32(d + 76, uint32_t(h));
  Wr32(d + 80, 1);
  Wr32(d + 84, 1);
  Wr64(d + 88, 0x10001);  // the monitor's handle (user32's primary monitor)
  return kS_OK;
}
// GetDisplayModeList(format, flags, &count, modes): the modes for the two
// back buffer formats games use; count only when modes is null.
DXGI_METHOD(IDXGIOutput, GetDisplayModeList) {
  const uint32_t format = uint32_t(Arg(c, 1));
  const gaddr count = Arg(c, 3), out = Arg(c, 4);
  std::vector<std::pair<int, int>> m;
  if (format == kFormatR8G8B8A8Unorm || format == kFormatB8G8R8A8Unorm ||
      format == kFormatR8G8B8A8Unorm + 1 /* _SRGB */ || format == kFormatB8G8R8A8Unorm + 4)
    m = Modes();
  if (!out) {
    Wr32(count, uint32_t(m.size()));
    return kS_OK;
  }
  const uint32_t room = Rd32(count);
  const uint32_t n = std::min<uint32_t>(room, uint32_t(m.size()));
  for (uint32_t i = 0; i < n; i++) PutMode(out + 28 * i, m[i].first, m[i].second, format);
  Wr32(count, n);
  return n < m.size() ? 0x887A0003u /* DXGI_ERROR_MORE_DATA */ : kS_OK;
}
// FindClosestMatchingMode(&wanted, &closest, device): the listed mode of
// the wanted size, or else the desktop's.
DXGI_METHOD(IDXGIOutput, FindClosestMatchingMode) {
  const gaddr want = Arg(c, 1), out = Arg(c, 2);
  const int w = int(Rd32(want)), h = int(Rd32(want + 4));
  uint32_t format = Rd32(want + 16);
  if (!format) format = kFormatR8G8B8A8Unorm;
  std::vector<std::pair<int, int>> m = Modes();
  std::pair<int, int> pick = m.back();
  for (auto mode : m)
    if (mode.first == w && mode.second == h) pick = mode;
  PutMode(out, pick.first, pick.second, format);
  return kS_OK;
}

}  // namespace recomp::hle::d3d11

namespace recomp::hle::d3d11 {

// ---- IDXGIDevice1 -----------------------------------------------------------------------

DXGI_METHOD(IDXGIDevice1, QueryInterface) {
  if (com::GuidEq(Arg(c, 1), kIID_ID3D11Device)) {
    gaddr dev = com::As<DxgiDevice>(Arg(c, 0))->device->guest;
    com::AddRef(dev);
    Wr64(Arg(c, 2), dev);
    return kS_OK;
  }
  return QueryAs(c, {&kIID_IUnknown, &kIID_IDXGIObject, &kIID_IDXGIDevice, &kIID_IDXGIDevice1},
                 "IDXGIDevice1");
}
DXGI_METHOD(IDXGIDevice1, AddRef) { return com::AddRef(Arg(c, 0)); }
DXGI_METHOD(IDXGIDevice1, Release) { return com::Release(Arg(c, 0)); }
DXGI_METHOD(IDXGIDevice1, GetAdapter) {
  gaddr a = TheAdapter();
  com::AddRef(a);
  Wr64(Arg(c, 1), a);
  return kS_OK;
}
// GetParent(riid, &parent): the adapter.
DXGI_METHOD(IDXGIDevice1, GetParent) {
  if (!com::GuidEq(Arg(c, 1), kIID_IDXGIAdapter) && !com::GuidEq(Arg(c, 1), kIID_IDXGIAdapter1)) {
    Wr64(Arg(c, 2), 0);
    return com::kE_NOINTERFACE;
  }
  gaddr a = TheAdapter();
  com::AddRef(a);
  Wr64(Arg(c, 2), a);
  return kS_OK;
}
DXGI_METHOD(IDXGIDevice1, SetGPUThreadPriority) { return kS_OK; }
DXGI_METHOD(IDXGIDevice1, GetGPUThreadPriority) {
  Wr32(Arg(c, 1), 0);
  return kS_OK;
}
DXGI_METHOD(IDXGIDevice1, SetMaximumFrameLatency) {
  com::As<DxgiDevice>(Arg(c, 0))->max_latency = uint32_t(Arg(c, 1)) ? uint32_t(Arg(c, 1)) : 3;
  return kS_OK;
}
DXGI_METHOD(IDXGIDevice1, GetMaximumFrameLatency) {
  Wr32(Arg(c, 1), com::As<DxgiDevice>(Arg(c, 0))->max_latency);
  return kS_OK;
}

}  // namespace recomp::hle::d3d11

namespace recomp::hle::d3d11 {

// ---- IDXGISwapChain ------------------------------------------------------------------

namespace {

constexpr guest::GUID kIID_IDXGIDeviceSubObject = {
    0x3d3e0379, 0xf9de, 0x4d58, {0xbb, 0x6c, 0x18, 0xd6, 0x29, 0x92, 0xf1, 0xa6}};
constexpr guest::GUID kIID_IDXGISwapChain = {
    0x310d36a0, 0xd2e7, 0x4c0a, {0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a}};
constexpr guest::GUID kIID_ID3D11Resource = {
    0xdc8e63f3, 0xd12b, 0x4952, {0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d}};
constexpr guest::GUID kIID_ID3D11Texture2D = {
    0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
constexpr uint32_t kUsageShaderInput = 0x10, kPresentTest = 0x1;

// DXGI_SWAP_CHAIN_DESC as the guest gave it, with zero sizes resolved:
// BufferDesc (DXGI_MODE_DESC) @0, SampleDesc @28, BufferUsage @36,
// BufferCount @40, OutputWindow @48, Windowed @56, SwapEffect @60,
// Flags @64; 72 bytes. Buffer 0, the only one the guest draws to, is
// `buffer`.
struct SwapChain : com::Object {
  Device* device = nullptr;
  uint8_t desc[72] = {};
  std::shared_ptr<Resource> buffer;
  int mode_w = 0, mode_h = 0;  // the target ResizeTarget set; 0: the buffers' size
  uint32_t D(int at) const { return *reinterpret_cast<const uint32_t*>(desc + at); }
  void SetD(int at, uint32_t v) { *reinterpret_cast<uint32_t*>(desc + at) = v; }
  uint32_t Hwnd() const { return uint32_t(*reinterpret_cast<const uint64_t*>(desc + 48)); }
  int W() const { return int(D(0)); }
  int H() const { return int(D(4)); }
  bool Windowed() const { return D(56) != 0; }
};

bool Vsync() {
  if (const char* v = Env("VSYNC")) return *v && *v != '0';
  return true;
}

void ApplyWindowMode(SwapChain& s) {
  if (s.Windowed()) {
    SetExclusiveMode(s.Hwnd(), 0, 0);
    return;
  }
  SetExclusiveMode(s.Hwnd(), s.mode_w ? s.mode_w : s.W(), s.mode_h ? s.mode_h : s.H());
  ShowWindowNoActivate(s.Hwnd());
}

// (Re)makes the back buffer for the description; the device's gfx::Device
// is made on the window the first time. Window and Metal layer calls go
// to the main thread, as the guest may present from any thread.
void SetUp(SwapChain& s) {
  if (!s.W() || !s.H()) {
    int w = 0, h = 0;
    ClientAreaSize(s.Hwnd(), &w, &h);
    if (!s.W()) s.SetD(0, uint32_t(w > 0 ? w : 640));
    if (!s.H()) s.SetD(4, uint32_t(h > 0 ? h : 480));
  }
  if (!s.D(40)) s.SetD(40, 1);
  ApplyWindowMode(s);
  Device& d = *s.device;
  RunOnUiThread([&] {
    if (!d.gfx) {
      SDL_Window* w = SdlWindowFor(s.Hwnd());
      if (!w) FatalF("dxgi: CreateSwapChain: no SDL window for hwnd %08X", s.Hwnd());
      gfx::SetShaderCacheDir(DataDir() + "/shadercache");
      d.gfx = gfx::CreateChosenDevice(w);
    }
    d.gfx->ResetSwapChain(s.W(), s.H(), Vsync());
  });
  if (s.buffer) com::Release(s.buffer->guest);
  s.buffer = NewBackBuffer(uint32_t(s.W()), uint32_t(s.H()), s.D(16),
                           kBindRenderTarget | (s.D(36) & kUsageShaderInput ? kBindShaderResource : 0));
  std::fprintf(stderr, "dxgi: swap chain %dx%d format %u x%u %s, effect %u, flags %X\n", s.W(), s.H(),
               s.D(16), s.D(40), s.Windowed() ? "windowed" : "fullscreen", s.D(60), s.D(64));
}

SwapChain* Chain(Ctx& c) { return com::As<SwapChain>(Arg(c, 0)); }

}  // namespace

// CreateSwapChain(device, &desc, &swap chain): the device is the
// ID3D11Device or its IDXGIDevice.
uint64_t CreateSwapChain(Ctx& c) {
  const gaddr dev = Arg(c, 1), desc = Arg(c, 2), out = Arg(c, 3);
  Device* d = com::As<Device>(dev);
  if (!d)
    if (auto* x = com::As<DxgiDevice>(dev)) d = x->device;
  if (!d || !desc || !out) return kDXGI_ERROR_INVALID_CALL;
  auto s = std::make_shared<SwapChain>();
  s->device = d;
  std::memcpy(s->desc, GPtr(desc), sizeof s->desc);
  SetUp(*s);
  Wr64(out, com::Publish(s, com::Vtable(kDll, "IDXGISwapChain",
                                         {"QueryInterface", "AddRef", "Release", "SetPrivateData",
                                          "SetPrivateDataInterface", "GetPrivateData", "GetParent",
                                          "GetDevice", "Present", "GetBuffer", "SetFullscreenState",
                                          "GetFullscreenState", "GetDesc", "ResizeBuffers",
                                          "ResizeTarget", "GetContainingOutput",
                                          "GetFrameStatistics", "GetLastPresentCount"})));
  return kS_OK;
}

DXGI_METHOD(IDXGISwapChain, QueryInterface) {
  return QueryAs(c, {&kIID_IUnknown, &kIID_IDXGIObject, &kIID_IDXGIDeviceSubObject,
                     &kIID_IDXGISwapChain},
                 "IDXGISwapChain");
}
DXGI_METHOD(IDXGISwapChain, AddRef) { return com::AddRef(Arg(c, 0)); }
// The last reference drops the swap chain's own on its back buffer.
DXGI_METHOD(IDXGISwapChain, Release) {
  SwapChain* s = Chain(c);
  if (s->refs == 1 && s->buffer) {
    com::Release(s->buffer->guest);
    s->buffer.reset();
  }
  return com::Release(Arg(c, 0));
}
// GetBuffer(index, riid, &surface): buffer 0 as a texture.
DXGI_METHOD(IDXGISwapChain, GetBuffer) {
  SwapChain* s = Chain(c);
  const gaddr iid = Arg(c, 2), out = Arg(c, 3);
  if (uint32_t(Arg(c, 1)) != 0 || !s->buffer) {
    Wr64(out, 0);
    return kDXGI_ERROR_INVALID_CALL;
  }
  if (!com::GuidEq(iid, kIID_ID3D11Texture2D) && !com::GuidEq(iid, kIID_ID3D11Resource) &&
      !com::GuidEq(iid, kIID_IUnknown)) {
    guest::GUID g = com::ReadGuid(iid);
    WarnF("IDXGISwapChain::GetBuffer({%08X-%04X-%04X-...}): not supported\n", g.Data1, g.Data2,
          g.Data3);
    Wr64(out, 0);
    return com::kE_NOINTERFACE;
  }
  com::AddRef(s->buffer->guest);
  Wr64(out, s->buffer->guest);
  return kS_OK;
}
// Present(sync interval, flags): what was drawn into the back buffer.
// DXGI_PRESENT_TEST only asks whether the window is visible.
DXGI_METHOD(IDXGISwapChain, Present) {
  SwapChain* s = Chain(c);
  if (uint32_t(Arg(c, 2)) & kPresentTest) return kS_OK;
  GpuPresent(*s->device, *s->buffer, s->W(), s->H());
  return kS_OK;
}
DXGI_METHOD(IDXGISwapChain, GetDesc) {
  std::memcpy(GPtr(Arg(c, 1)), Chain(c)->desc, sizeof Chain(c)->desc);
  return kS_OK;
}
// ResizeBuffers(count, width, height, format, flags): zeros keep the
// count and format and take the window's size. The guest must have
// released every reference it had to the old buffer.
DXGI_METHOD(IDXGISwapChain, ResizeBuffers) {
  SwapChain* s = Chain(c);
  if (s->buffer && s->buffer->refs > 1) return kDXGI_ERROR_INVALID_CALL;
  if (uint32_t(Arg(c, 1))) s->SetD(40, uint32_t(Arg(c, 1)));
  s->SetD(0, uint32_t(Arg(c, 2)));
  s->SetD(4, uint32_t(Arg(c, 3)));
  if (uint32_t(Arg(c, 4))) s->SetD(16, uint32_t(Arg(c, 4)));
  s->SetD(64, uint32_t(Arg(c, 5)));
  SetUp(*s);
  return kS_OK;
}
// SetFullscreenState(fullscreen, output): the window mode only; the guest
// resizes the buffers itself afterwards, as on Windows.
// ResizeTarget(DXGI_MODE_DESC*): the display mode (fullscreen) and the
// window's client area to the mode's size; the guest resizes its buffers
// when WM_SIZE comes, as on Windows, where the window covers the new mode.
// The target stays the mode fullscreen uses (SetFullscreenState). A zero
// size keeps the target.
DXGI_METHOD(IDXGISwapChain, ResizeTarget) {
  SwapChain* s = Chain(c);
  const gaddr mode = Arg(c, 1);
  const int w = int(Rd32(mode)), h = int(Rd32(mode + 4));
  if (!w || !h) return kS_OK;
  s->mode_w = w;
  s->mode_h = h;
  if (!s->Windowed()) SetExclusiveMode(s->Hwnd(), w, h);
  ResizeClientArea(s->Hwnd(), w, h);
  return kS_OK;
}

DXGI_METHOD(IDXGISwapChain, SetFullscreenState) {
  SwapChain* s = Chain(c);
  s->SetD(56, Arg(c, 1) ? 0 : 1);
  ApplyWindowMode(*s);
  return kS_OK;
}
DXGI_METHOD(IDXGISwapChain, GetFullscreenState) {
  SwapChain* s = Chain(c);
  if (Arg(c, 1)) Wr32(Arg(c, 1), s->Windowed() ? 0 : 1);
  if (Arg(c, 2)) {
    gaddr o = 0;
    if (!s->Windowed()) {
      o = TheOutput();
      com::AddRef(o);
    }
    Wr64(Arg(c, 2), o);
  }
  return kS_OK;
}

}  // namespace recomp::hle::d3d11
