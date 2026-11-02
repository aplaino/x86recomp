#pragma once
// Host objects behind the Direct3D 9 interfaces, shared by d3d9.cpp
// (IDirect3D9, the device's core) and d3d9_resource.cpp (surfaces,
// textures, buffers, vertex declarations, shaders, queries).

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/hlsl/hlsl.h"
#include "runtime/hle/com.h"
#include "runtime/hle/d3d9_types.h"
#include "runtime/ctx.h"
#include "runtime/perf.h"
#include "runtime/runtime.h"

namespace recomp::d3d9 {

constexpr uint32_t D3D_OK = 0;
constexpr uint32_t D3DERR_NOTFOUND = 0x88760866;
constexpr uint32_t D3DERR_NOTAVAILABLE = 0x8876086A;
constexpr uint32_t D3DERR_INVALIDCALL = 0x8876086C;
constexpr uint32_t S_FALSE = 1;

// D3DFORMAT values
enum : uint32_t {
  D3DFMT_UNKNOWN = 0, D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21,
  D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23, D3DFMT_X1R5G5B5 = 24,
  D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_R3G3B2 = 27,
  D3DFMT_A8 = 28, D3DFMT_X4R4G4B4 = 30, D3DFMT_P8 = 41, D3DFMT_L8 = 50,
  D3DFMT_A8L8 = 51, D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71,
  D3DFMT_D15S1 = 73, D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77, D3DFMT_D16 = 80,
  D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102, D3DFMT_R16F = 111,
  D3DFMT_G16R16F = 112, D3DFMT_A16B16G16R16F = 113, D3DFMT_R32F = 114,
  D3DFMT_G32R32F = 115, D3DFMT_A32B32G32R32F = 116,
  D3DFMT_DXT1 = 0x31545844, D3DFMT_DXT2 = 0x32545844,
  D3DFMT_DXT3 = 0x33545844, D3DFMT_DXT4 = 0x34545844,
  D3DFMT_DXT5 = 0x35545844,
  // Vendor depth formats: INTZ is a D24S8 depth-stencil texture shaders can
  // sample (supported); RAWZ, DF24 and DF16 are not.
  D3DFMT_INTZ = 0x5A544E49,
};
// D3DRESOURCETYPE
enum : uint32_t {
  D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3,
  D3DRTYPE_VOLUMETEXTURE = 4, D3DRTYPE_CUBETEXTURE = 5,
  D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7,
};
// D3DUSAGE
enum : uint32_t {
  D3DUSAGE_RENDERTARGET = 0x1, D3DUSAGE_DEPTHSTENCIL = 0x2,
  D3DUSAGE_WRITEONLY = 0x8, D3DUSAGE_DYNAMIC = 0x200,
  D3DUSAGE_AUTOGENMIPMAP = 0x400,
  D3DUSAGE_QUERY_SRGBREAD = 0x10000, D3DUSAGE_QUERY_FILTER = 0x20000,
  D3DUSAGE_QUERY_SRGBWRITE = 0x40000,
  D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING = 0x80000,
  D3DUSAGE_QUERY_VERTEXTEXTURE = 0x100000,
  D3DUSAGE_QUERY_WRAPANDMIP = 0x200000,
};
// D3DPOOL
enum : uint32_t {
  D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2,
  D3DPOOL_SCRATCH = 3,
};
// D3DLOCK
enum : uint32_t {
  D3DLOCK_READONLY = 0x10, D3DLOCK_NOOVERWRITE = 0x1000,
  D3DLOCK_DISCARD = 0x2000, D3DLOCK_NO_DIRTY_UPDATE = 0x8000,
};

gfx::Format FormatFromD3D(uint32_t f);
bool IsDepthFormat(gfx::Format f);
// Formats a render target can have (a swap chain: IsBackBufferFormat).
bool IsRenderTargetFormat(uint32_t f);
bool IsBackBufferFormat(uint32_t f);

template <typename T>
T* G(uint32_t va) {
  return reinterpret_cast<T*>(GPtr(va));
}

struct Texture;

struct Surface final : com::Tagged<com::TagOf("d3d9.Surface")> {
  enum class Kind {
    BackBuffer,    // the swap chain's: gfx render target 0
    DepthStencil,  // gfx depth buffer
    TextureLevel,  // a level of `texture` (its refcount is the texture's)
    RenderTarget,  // CreateRenderTarget: a gfx render-target texture
    Offscreen,     // CreateOffscreenPlainSurface: memory only
  };
  Kind kind = Kind::BackBuffer;
  uint32_t device = 0;  // IDirect3DDevice9 guest pointer
  guest::D3DSURFACE_DESC desc{};
  gfx::DepthId depth = 0;     // DepthStencil
  gfx::TextureId tex = 0;     // RenderTarget
  Texture* texture = nullptr;  // TextureLevel
  uint32_t container = 0;      // TextureLevel: the texture's guest pointer
  int face = 0, level = 0;
  uint32_t mem = 0;  // Offscreen: its pixels
  uint32_t pitch = 0;
  ~Surface() override;
};

struct Texture final : com::Tagged<com::TagOf("d3d9.Texture")> {
  uint32_t device = 0;
  bool cube = false;  // IDirect3DCubeTexture9: six faces of levels each
  uint32_t width = 0, height = 0, levels = 0, usage = 0, format = 0, pool = 0;
  gfx::Format fmt = gfx::Format::Unknown;
  gfx::TextureId tex = 0;  // none for SYSTEMMEM/SCRATCH
  gfx::DepthId depth = 0;  // INTZ: the depth buffer behind `tex` (level 0)
  struct Level {
    uint32_t w = 0, h = 0, pitch = 0, size = 0;
    uint32_t mem = 0;  // lock shadow, allocated on first lock
    // mem is in the shared heap (SetUpSharedBuffers): the gfx frame of the
    // last upload from it, which the GPU may still be copying.
    bool shared = false;
    uint64_t gpu_use = 0;
    bool locked = false;
    gfx::Rect locked_rect{};
    bool lock_readonly = false;
    std::shared_ptr<Surface> surface;  // GetSurfaceLevel, created lazily
  };
  std::vector<Level> lv;  // face * levels + level
  Level& At(uint32_t face, uint32_t level) { return lv[face * levels + level]; }
  uint32_t Faces() const { return cube ? 6 : 1; }
  uint32_t lod = 0, priority = 0, autogen_filter = 2;  // D3DTEXF_LINEAR
  ~Texture() override;
};

struct Buffer final : com::Tagged<com::TagOf("d3d9.Buffer")> {
  uint32_t device = 0;
  bool index = false;
  uint32_t length = 0, usage = 0, pool = 0;
  uint32_t fvf_or_format = 0;  // FVF (vertex) or D3DFORMAT (index)
  uint32_t mem = 0;            // the data, in guest memory
  uint32_t priority = 0;
  // mem is in the heap the GPU reads in place (SetUpSharedBuffers); the
  // gfx frame of the last draw that read it.
  bool shared = false;
  uint64_t last_use = 0;
  ~Buffer() override;
};

// A vertex shader's inputs (its input semantics matched to a vertex
// declaration's or an FVF's elements), worked out once per shader and
// declaration (DXVK: once per D3D9VertexDecl). The entry is found by the
// program's address, which a released program frees for another: the weak
// reference tells an entry of this program from one left by a program at the
// same address, without keeping a released program alive.
struct InputLayout {
  std::weak_ptr<const hlsl::Program> program;
  std::vector<gfx::VertexInput> inputs;
  // The entry is this program's (an entry left by a released one is not).
  bool Of(const hlsl::Program* p) const { return program.lock().get() == p; }
};

struct VertexDecl final : com::Tagged<com::TagOf("d3d9.VertexDecl")> {
  uint32_t device = 0;
  std::vector<guest::D3DVERTEXELEMENT9> elements;  // with the END marker
  // Its input layouts by vertex shader program (d3d9_draw.cpp).
  std::unordered_map<const hlsl::Program*, InputLayout> layouts;
};

struct Shader final : com::Tagged<com::TagOf("d3d9.Shader")> {
  uint32_t device = 0;
  bool pixel = false;
  std::vector<uint32_t> tokens;  // the function, END token included
  // A function D3DXCompileShader translated (TranslatedFunction), and
  // the gfx shader made from it.
  std::shared_ptr<const hlsl::Program> program;
  gfx::ShaderId id = 0;
  ~Shader() override;
};

struct Query final : com::Tagged<com::TagOf("d3d9.Query")> {
  uint32_t device = 0;
  uint32_t type = 0;
};

// The implicit swap chain (GetSwapChain(0)): methods act on its device.
struct SwapChain final : com::Tagged<com::TagOf("d3d9.SwapChain")> {
  uint32_t device = 0;
};

// A binding: the guest pointer the device holds a reference to, and the
// host object behind it as a T (null if it is not one), found once by
// Bind so that draws need no lookup. The device's reference keeps the
// object alive while it is bound.
template <typename T>
struct Slot {
  uint32_t guest = 0;
  T* obj = nullptr;
};

// The render states, and which have changed since gfx last had them
// (SyncState): Set marks a state whose value changes.
struct RenderStates {
  uint32_t v[256] = {};
  uint64_t dirty[4] = {~0ull, ~0ull, ~0ull, ~0ull};
  uint32_t operator[](uint32_t s) const { return v[s]; }
  void Set(uint32_t s, uint32_t value) {
    if (v[s] == value) return;
    v[s] = value;
    dirty[s >> 6] |= 1ull << (s & 63);
  }
  // All zero (Reset), all to be compared again.
  void Clear() {
    for (uint32_t& x : v) x = 0;
    for (uint64_t& d : dirty) d = ~0ull;
  }
};

struct Device final : com::Tagged<com::TagOf("d3d9.Device")> {
  uint32_t d3d = 0;  // IDirect3D9 guest pointer
  guest::D3DDEVICE_CREATION_PARAMETERS creation{};
  guest::D3DPRESENT_PARAMETERS pp{};
  uint32_t hwnd = 0;
  std::unique_ptr<gfx::Device> gfx;
  bool shared_buffers = false;  // vertex and index buffers in the shared heap
  std::shared_ptr<Surface> backbuffer, auto_depth;
  uint32_t swapchain = 0;  // IDirect3DSwapChain9 of GetSwapChain(0), made on first use
  // Bound objects (Bind).
  Slot<Surface> render_targets[4];
  Slot<Surface> depth_stencil;
  Slot<Texture> textures[20];  // samplers 0-15, then vertex samplers 0-3
  struct Stream {
    Slot<Buffer> buffer;
    uint32_t offset = 0, stride = 0, freq = 1;
  };
  Stream streams[16];
  Slot<Buffer> indices;
  Slot<VertexDecl> decl;
  Slot<Shader> vs, ps;
  RenderStates rs;
  uint32_t ss[20][14] = {};  // samplers 0-15 and the 4 vertex samplers
  // Each sampler's change counter (ShaderDraw::sampler_versions): taken
  // from ss_serial whenever one of its states changes.
  uint64_t ss_version[20] = {};
  uint64_t ss_serial = 0;
  void SetSamplerState(uint32_t s, uint32_t t, uint32_t v) {
    if (ss[s][t] == v) return;
    ss[s][t] = v;
    ss_version[s] = ++ss_serial;
  }
  // After ss is written as a whole.
  void SamplerStatesChanged() {
    for (uint64_t& v : ss_version) v = ++ss_serial;
  }
  uint32_t tss[8][33] = {};
  uint32_t fvf = 0;
  guest::BOOL software_vp = 0;
  std::map<uint32_t, std::array<float, 16>> transforms;
  float clip_planes[6][4] = {};  // SetClipPlane (D3DMAXUSERCLIPPLANES)
  guest::D3DVIEWPORT9 viewport{};
  guest::D3DRECT scissor{};
  float vs_f[256][4] = {}, ps_f[224][4] = {};
  uint64_t vs_f_version = 1, ps_f_version = 1;  // bumped by Set*ShaderConstantF
  uint64_t vs_ib_version = 1, ps_ib_version = 1;  // ... by Set*ShaderConstantI and B
  int32_t vs_i[16][4] = {}, ps_i[16][4] = {};
  guest::BOOL vs_b[16] = {}, ps_b[16] = {};
  int scene_depth = 0;
  // Render states, viewport and scissor rectangle as gfx has them
  // (SyncState); all sent on the first sync.
  uint32_t sent_rs[256] = {};
  guest::D3DVIEWPORT9 sent_viewport{};
  guest::D3DRECT sent_scissor{};
  bool synced = false;
  // The D3D cursor (SetCursorProperties / ShowCursor).
  bool cursor_shown = false, cursor_set = false;
  // Vertex shaders standing in for the fixed-function vertex stage in
  // draws with a pixel shader (d3d9_draw.cpp), by their HLSL source.
  struct FfVertexShader {
    std::shared_ptr<const hlsl::Program> program;
    gfx::ShaderId id = 0;
  };
  std::map<std::string, FfVertexShader> ff_vs;
  // The same for the fixed-function pixel stage after a vertex shader.
  std::map<std::string, FfVertexShader> ff_ps;
  // Those shaders by the state their source is made from, so that a draw
  // with a stand-in does not build the source (d3d9_draw.cpp).
  std::map<std::array<uint32_t, 17>, const FfVertexShader*> ff_vs_by_state;
  std::map<std::array<uint32_t, 97>, const FfVertexShader*> ff_ps_by_state;
  // Input layouts of FVF draws, by FVF (bit 32: for a stand-in vertex
  // shader) and vertex shader program; pruned of the programs that are gone
  // (d3d9_draw.cpp), so a game's released shaders are too.
  std::map<std::pair<uint64_t, const hlsl::Program*>, InputLayout> fvf_layouts;
};

// Vertex and index buffers in a heap of guest memory the graphics device
// reads in place (gfx::Device::MapSharedMemory), when it can: set up for
// `d` once d.gfx exists.
void SetUpSharedBuffers(Device& d);
// A draw of `d` reads buffer `b`.
inline void NoteBufferUse(Device& d, Buffer* b) {
  if (b->shared) b->last_use = d.gfx->FrameSerial();
}

// IUnknown::QueryInterface for an object with one interface `iid` (and
// IUnknown): (this, riid, ppvObject).
uint32_t QueryInterfaceFor(Ctx& c, const guest::GUID& iid);

// The device a method's object belongs to (fatal if `self` is not one).
Device* DeviceOf(uint32_t self);

// Sampler index: 0-15, or D3DVERTEXTEXTURESAMPLER0..3 (256..259) -> 16..19;
// -1 if neither.
int SamplerIndex(uint32_t s);

// What a reference to a bound object counts on: a texture level's count
// is its texture's (SurfaceRefTarget).
inline com::Object* RefObject(com::Object* o) { return o; }
inline com::Object* RefObject(Surface* s) {
  return s->container ? static_cast<com::Object*>(s->texture) : s;
}

// Counts a Bind for the <ID>_PERF counters (`same`: of what the slot held).
void CountBind(bool same);
// Bind's references for objects not found as the slot's type.
void BindByGuest(uint32_t old_guest, uint32_t obj);

// Replaces a bound object: references the new one, releases the old. The
// object the slot holds already: nothing to do (DXVK's SetStateTexture).
// The counts go through the host objects (com::AddRefHeld): the caller's
// reference keeps the new one alive, the slot's the old one.
template <typename T>
void Bind(Slot<T>& slot, uint32_t obj) {
  // Counted only when <ID>_PERF is on: with it off, this is the test, inlined
  // here, and no call (a frame makes thousands of Binds).
  if (perf::On()) CountBind(obj == slot.guest);
  if (obj == slot.guest) return;
  T* o = com::As<T>(obj);
  if ((obj && !o) || (slot.guest && !slot.obj)) {  // not a T: by the guest pointers
    BindByGuest(slot.guest, obj);
  } else {
    if (o) com::AddRefHeld(RefObject(o));
    if (slot.obj) com::ReleaseHeld(RefObject(slot.obj));
  }
  slot.guest = obj;
  slot.obj = o;
}

// <ID>_D3D9_CHECK_BINDINGS=1: every draw compares the host objects Bind
// found with a fresh lookup (fatal on a difference).
void CheckBindings(const Device& d);

// The runtime's <ID>_PERF counters for the front end, reported every 2 s
// from Present (per frame): draws and their time (the backend's recording
// included), draws with a stand-in vertex or pixel shader, Binds and the
// ones that bound what the slot already held.
struct PerfCounts {
  std::atomic<uint64_t> draws{0}, draw_ns{0}, standin_vs{0}, standin_ps{0}, binds{0}, rebinds{0};
};
extern PerfCounts g_perf;

// A standalone surface (back buffer, depth stencil, render target,
// offscreen plain): published with refcount 1, kept in *keep if given.
uint32_t NewSurface(Device& d, uint32_t device, Surface::Kind kind, uint32_t w,
                    uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
                    std::shared_ptr<Surface>* keep);

// What a reference to a surface handed to the app counts on: a texture
// level's count is its texture's (AddRef/Release of the surface go there).
uint32_t SurfaceRefTarget(uint32_t surface);

// The gfx render targets and depth buffer for the current bindings: when
// they change (SetRenderTarget, SetDepthStencilSurface, the swap chain's).
// Draws and clears do not bind again; gfx keeps the targets.
void BindTargets(Device& d);

// Sends gfx the render states that changed, and the viewport and the
// scissor rectangle if they changed (before draws and clears;
// d3d9_draw.cpp).
void SyncState(Device& d);

// CreateTexture / CreateCubeTexture (cube: w = h = edge length): the
// HRESULT; the new texture's guest pointer in *out (0 on failure).
uint32_t NewTexture(Device& d, uint32_t device, bool cube, uint32_t w, uint32_t h,
                    uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool,
                    uint32_t* out);

// D3DXCompileShader's output: a shader function holding the HLSL
// translation instead of shader model bytecode. Version token, one comment
// with kTranslatedTag and the serialised hlsl::Program, END. Create*Shader
// recognise it.
constexpr uint32_t kTranslatedTag = 0x504D4352;  // 'RCMP'
std::vector<uint32_t> TranslatedFunction(const hlsl::Program& p);
// The Program in such a function; null for any other function.
std::shared_ptr<const hlsl::Program> ReadTranslated(const std::vector<uint32_t>& tokens);

// Replaces a whole level's pixels (rows of `pitch` bytes, in the
// texture's format) and uploads them: D3DX loaders.
void FillLevel(Texture& t, uint32_t face, uint32_t level, const uint8_t* src,
               uint32_t pitch);

}  // namespace recomp::d3d9
