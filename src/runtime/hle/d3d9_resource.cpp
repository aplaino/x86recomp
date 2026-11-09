// d3d9 resources: IDirect3DSurface9, IDirect3DTexture9,
// IDirect3DCubeTexture9, vertex and index buffers, vertex declarations,
// shaders and queries, and the device methods that create and bind them.
//
// Texture levels keep a lock shadow in guest memory (allocated on first
// lock) and upload the locked rectangle on unlock. Vertex and index data
// live in guest memory; draws read them from there. Surfaces of a texture
// share its reference count, as in D3D9: AddRef/Release on them go to the
// texture. The device holds a reference to everything bound to it.

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>
#include <cstdio>
#include <cstring>

#include "runtime/guest_alloc.h"
#include "runtime/hle/d3d9_impl.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::d3d9 {

// ---- vertex and index buffers the GPU reads in place
//
// One heap of guest memory for the process (<ID>_GPU_HEAP_MB, default
// 512; 0 turns it off), committed whole and mapped into each graphics
// device that can read memory in place. The GPU reads a buffer while the
// game may already be filling it for a later frame, so a Lock that would
// write memory a frame not yet finished may still read (no NOOVERWRITE)
// gives the buffer a new block, as D3D9 drivers rename DISCARDed buffers
// (the old contents copied over without DISCARD); old blocks, and those of
// buffers released, are freed once the GPU has finished their last frame.

namespace {

struct SharedHeap {
  std::mutex mu;
  std::unique_ptr<GuestHeap> heap;
  uint64_t size = 0;
  struct Retired {
    gaddr mem;
    uint64_t serial;
    const gfx::Device* dev;
  };
  std::vector<Retired> retired;
};

SharedHeap& Shared() {
  static SharedHeap s;
  return s;
}

// Frees retired blocks `dev` has finished with (all of other devices':
// a device finishes its GPU work when it is destroyed). Under mu.
void Reclaim(SharedHeap& s, const gfx::Device* dev) {
  const uint64_t done = dev ? dev->CompletedSerial() : 0;
  std::erase_if(s.retired, [&](const SharedHeap::Retired& r) {
    if (r.dev == dev && r.serial > done) return false;
    s.heap->Free(r.mem);
    return true;
  });
}

// A block of the shared heap, or 0 if it is full.
gaddr SharedAlloc(SharedHeap& s, const gfx::Device* dev, uint32_t size, bool zero) {
  gaddr p = s.heap->Alloc(size, zero);
  if (!p && !s.retired.empty()) {
    Reclaim(s, dev);
    p = s.heap->Alloc(size, zero);
  }
  if (!p) {
    static bool warned = false;
    if (!warned) WarnF("d3d9: the shared buffer heap (%llu MB) is full\n",
                       (unsigned long long)(s.size >> 20));
    warned = true;
  }
  return p;
}

void Retire(SharedHeap& s, const gfx::Device* dev, gaddr mem, uint64_t serial) {
  if (!dev || serial <= dev->CompletedSerial())
    s.heap->Free(mem);
  else
    s.retired.push_back({mem, serial, dev});
}

}  // namespace


namespace {

using namespace guest;

constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IDirect3DResource9 = {0x05EEC05D, 0x8F7D, 0x4362, {0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04}};
constexpr GUID kIID_IDirect3DSurface9 = {0x0CFBAF3A, 0x9FF6, 0x429A, {0x99, 0xB3, 0xA2, 0x79, 0x6A, 0xF8, 0xB8, 0x9B}};
constexpr GUID kIID_IDirect3DBaseTexture9 = {0x580CA87E, 0x1D3C, 0x4D54, {0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE}};
constexpr GUID kIID_IDirect3DTexture9 = {0x85C31227, 0x3DE5, 0x4F00, {0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5}};
constexpr GUID kIID_IDirect3DCubeTexture9 = {0xFFF32F81, 0xD953, 0x473A, {0x92, 0x23, 0x93, 0xD6, 0x52, 0xAB, 0xA9, 0x3F}};
constexpr GUID kIID_IDirect3DVertexBuffer9 = {0xB64BB1B5, 0xFD70, 0x4DF6, {0xBF, 0x91, 0x19, 0xD0, 0xA1, 0x24, 0x55, 0xE3}};
constexpr GUID kIID_IDirect3DIndexBuffer9 = {0x7C9DD65E, 0xD3F7, 0x4529, {0xAC, 0xEE, 0x78, 0x58, 0x30, 0xAC, 0xDE, 0x35}};

// D3DQUERYTYPE
constexpr uint32_t D3DQUERYTYPE_EVENT = 8, D3DQUERYTYPE_OCCLUSION = 9;
constexpr uint32_t D3DISSUE_END = 0x1, D3DISSUE_BEGIN = 0x2;

uint32_t SurfaceVtable() {
  return com::Vtable(
      "d3d9.dll", "IDirect3DSurface9",
      {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
       "GetPrivateData", "FreePrivateData", "SetPriority", "GetPriority",
       "PreLoad", "GetType", "GetContainer", "GetDesc", "LockRect",
       "UnlockRect", "GetDC", "ReleaseDC"});
}

uint32_t TextureVtable() {
  return com::Vtable(
      "d3d9.dll", "IDirect3DTexture9",
      {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
       "GetPrivateData", "FreePrivateData", "SetPriority", "GetPriority",
       "PreLoad", "GetType", "SetLOD", "GetLOD", "GetLevelCount",
       "SetAutoGenFilterType", "GetAutoGenFilterType", "GenerateMipSubLevels",
       "GetLevelDesc", "GetSurfaceLevel", "LockRect", "UnlockRect",
       "AddDirtyRect"});
}

uint32_t CubeTextureVtable() {
  return com::Vtable(
      "d3d9.dll", "IDirect3DCubeTexture9",
      {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
       "GetPrivateData", "FreePrivateData", "SetPriority", "GetPriority",
       "PreLoad", "GetType", "SetLOD", "GetLOD", "GetLevelCount",
       "SetAutoGenFilterType", "GetAutoGenFilterType", "GenerateMipSubLevels",
       "GetLevelDesc", "GetCubeMapSurface", "LockRect", "UnlockRect",
       "AddDirtyRect"});
}

uint32_t BufferVtable(bool index) {
  return com::Vtable(
      "d3d9.dll", index ? "IDirect3DIndexBuffer9" : "IDirect3DVertexBuffer9",
      {"QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
       "GetPrivateData", "FreePrivateData", "SetPriority", "GetPriority",
       "PreLoad", "GetType", "Lock", "Unlock", "GetDesc"});
}

uint32_t VertexDeclVtable() {
  return com::Vtable("d3d9.dll", "IDirect3DVertexDeclaration9",
                     {"QueryInterface", "AddRef", "Release", "GetDevice",
                      "GetDeclaration"});
}

uint32_t ShaderVtable(bool pixel) {
  return com::Vtable("d3d9.dll",
                     pixel ? "IDirect3DPixelShader9" : "IDirect3DVertexShader9",
                     {"QueryInterface", "AddRef", "Release", "GetDevice",
                      "GetFunction"});
}

uint32_t QueryVtable() {
  return com::Vtable("d3d9.dll", "IDirect3DQuery9",
                     {"QueryInterface", "AddRef", "Release", "GetDevice",
                      "GetType", "GetDataSize", "Issue", "GetData"});
}

template <typename T>
T* Obj(Ctx& c, const char* iface) {
  T* o = com::As<T>(hle::Arg(c, 0));
  if (!o) FatalF("%s call on unknown object %08X", iface, uint32_t(hle::Arg(c, 0)));
  return o;
}

Surface* Surf(Ctx& c) { return Obj<Surface>(c, "IDirect3DSurface9"); }
Texture* Tex(Ctx& c) { return Obj<Texture>(c, "IDirect3D{Cube}Texture9"); }
Buffer* Buf(Ctx& c) { return Obj<Buffer>(c, "IDirect3D{Vertex,Index}Buffer9"); }

uint32_t GetDeviceOf(Ctx& c, uint32_t device) {
  com::AddRef(device);
  Wr32(hle::Arg(c, 1), device);
  return D3D_OK;
}

// The memory of a texture level's lock shadow, allocated on first use. A
// texture with a GPU side gets it from the shared heap when the device
// reads that in place: its uploads then copy nothing on the CPU (the game
// fills a level once, at load). SYSTEMMEM textures, staging a game
// refills again and again, keep their own memory: a lock there while the
// GPU still copies would mean copying the level to rename it.
uint32_t LevelMem(Texture& t, Texture::Level& l) {
  if (l.mem) return l.mem;
  Device* d = com::As<Device>(t.device);
  static const bool on = [] {  // <ID>_TEX_SHARED=0: the process heap (comparisons)
    const char* e = Env("TEX_SHARED");
    return !e || *e != '0';
  }();
  if (on && t.tex && d && d->shared_buffers) {
    SharedHeap& s = Shared();
    std::lock_guard<std::mutex> lock(s.mu);
    l.mem = uint32_t(SharedAlloc(s, d->gfx.get(), l.size, true));
    l.shared = l.mem != 0;
  }
  if (!l.mem) l.mem = HeapAllocProcess(l.size, true);
  return l.mem;
}

// A shared level about to be written while the GPU may still be copying
// an upload from it: a new block with the same contents, the old one freed
// once that frame has finished (as BufferLock renames buffers).
void RenameLevelIfInUse(Texture& t, Texture::Level& l) {
  Device* d = com::As<Device>(t.device);
  if (!l.shared || !d || !d->gfx || l.gpu_use <= d->gfx->CompletedSerial()) return;
  SharedHeap& s = Shared();
  std::lock_guard<std::mutex> lock(s.mu);
  if (gaddr n = SharedAlloc(s, d->gfx.get(), l.size, false)) {
    std::memcpy(GPtr(n), GPtr(l.mem), l.size);
    Retire(s, d->gfx.get(), l.mem, l.gpu_use);
    l.mem = uint32_t(n);
    l.gpu_use = 0;
  }
}

// Uploads `r` of a level from its lock shadow.
void UploadLevel(Texture& t, uint32_t face, uint32_t level, gfx::Rect r) {
  Texture::Level& l = t.At(face, level);
  uint32_t off;
  if (gfx::IsCompressed(t.fmt)) {
    r.left &= ~3;
    r.top &= ~3;
    off = uint32_t(r.top / 4) * l.pitch + uint32_t(r.left / 4) * gfx::RowPitch(t.fmt, 4);
  } else {
    off = uint32_t(r.top) * l.pitch + uint32_t(r.left) * gfx::BitsPerPixel(t.fmt) / 8;
  }
  gfx::Device* g = DeviceOf(t.device)->gfx.get();
  if (t.cube)
    g->UpdateCubeTexture(t.tex, int(face), int(level), r, GPtr(l.mem + off), l.pitch);
  else
    g->UpdateTexture(t.tex, int(level), r, GPtr(l.mem + off), l.pitch);
  if (l.shared) l.gpu_use = g->FrameSerial();  // the GPU reads it until then
}

// Locks a level of texture memory: rect (null = all), pitch and pointer.
uint32_t LockLevel(Texture& t, uint32_t face, uint32_t level, uint32_t out, uint32_t rect,
                   uint32_t flags) {
  if (level >= t.levels || face >= t.Faces()) return D3DERR_INVALIDCALL;
  Texture::Level& l = t.At(face, level);
  if (l.locked) return D3DERR_INVALIDCALL;
  if ((t.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && t.pool == D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;  // GPU-only
  LevelMem(t, l);
  if (!(flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE))) RenameLevelIfInUse(t, l);
  gfx::Rect r{0, 0, int32_t(l.w), int32_t(l.h)};
  if (rect) r = *G<gfx::Rect>(rect);
  uint32_t off;
  if (gfx::IsCompressed(t.fmt))
    off = uint32_t(r.top / 4) * l.pitch + uint32_t(r.left / 4) * gfx::RowPitch(t.fmt, 4);
  else
    off = uint32_t(r.top) * l.pitch + uint32_t(r.left) * gfx::BitsPerPixel(t.fmt) / 8;
  l.locked = true;
  l.locked_rect = r;
  l.lock_readonly = (flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)) != 0;
  *G<D3DLOCKED_RECT>(out) = {INT(l.pitch), l.mem + off};
  return D3D_OK;
}

uint32_t UnlockLevel(Texture& t, uint32_t face, uint32_t level) {
  if (level >= t.levels || face >= t.Faces() || !t.At(face, level).locked)
    return D3DERR_INVALIDCALL;
  Texture::Level& l = t.At(face, level);
  l.locked = false;
  if (t.tex && !l.lock_readonly) UploadLevel(t, face, level, l.locked_rect);
  return D3D_OK;
}

// The surface for a texture level, created on first use.
Surface& LevelSurface(Texture& t, uint32_t face, uint32_t level) {
  Texture::Level& l = t.At(face, level);
  if (!l.surface) {
    auto s = std::make_shared<Surface>();
    s->kind = Surface::Kind::TextureLevel;
    s->device = t.device;
    s->desc = {t.format, D3DRTYPE_SURFACE, t.usage, t.pool, 0, 0, l.w, l.h};
    s->texture = &t;
    s->container = t.guest;
    s->face = int(face);
    s->level = int(level);
    l.surface = s;
    com::Publish(s, SurfaceVtable());
  }
  return *l.surface;
}

// Vertex declaration elements up to and including D3DDECL_END.
std::vector<D3DVERTEXELEMENT9> ReadDecl(uint32_t va) {
  std::vector<D3DVERTEXELEMENT9> e;
  for (;; va += sizeof(D3DVERTEXELEMENT9)) {
    e.push_back(*G<D3DVERTEXELEMENT9>(va));
    if (e.back().Stream == 0xFF) break;  // D3DDECL_END
    if (e.size() > 64) FatalF("d3d9: vertex declaration without D3DDECL_END");
  }
  return e;
}

// A shader function up to and including the END token, stepped over
// instruction by instruction (hlsl::ReadBytecode).
std::vector<uint32_t> ReadFunction(uint32_t va) {
  std::vector<uint32_t> t = hlsl::ReadBytecode([va](size_t i) { return Rd32(va + 4 * uint32_t(i)); }, 1u << 20);
  if (t.empty()) FatalF("d3d9: shader function without END");
  return t;
}

}  // namespace

std::vector<uint32_t> TranslatedFunction(const hlsl::Program& p) {
  std::vector<uint8_t> data = hlsl::Serialize(p);
  uint32_t words = 2 + uint32_t((data.size() + 3) / 4);  // tag, byte count, data
  if (words > 0x7FFF) FatalF("d3dx9: translated shader of %zu bytes is too big for a comment", data.size());
  std::vector<uint32_t> t;
  t.push_back((p.stage == hlsl::Stage::Pixel ? 0xFFFF0000u : 0xFFFE0000u) | p.major << 8 | p.minor);
  t.push_back(words << 16 | 0xFFFE);  // D3DSIO_COMMENT
  t.push_back(kTranslatedTag);
  t.push_back(uint32_t(data.size()));
  size_t at = t.size();
  t.resize(at + words - 2, 0);
  std::memcpy(t.data() + at, data.data(), data.size());
  t.push_back(0x0000FFFF);  // D3DSIO_END
  return t;
}

std::shared_ptr<const hlsl::Program> ReadTranslated(const std::vector<uint32_t>& tokens) {
  if (tokens.size() < 5 || (tokens[1] & 0xFFFF) != 0xFFFE || tokens[2] != kTranslatedTag)
    return nullptr;
  uint32_t words = (tokens[1] >> 16) & 0x7FFF, size = tokens[3];
  if (words < 2 || 2 + words > tokens.size() || size > 4 * (words - 2)) return nullptr;
  auto p = std::make_shared<hlsl::Program>();
  if (!hlsl::Deserialize(reinterpret_cast<const uint8_t*>(tokens.data() + 4), size, p.get()))
    FatalF("d3d9: a translated shader that does not deserialise");
  return p;
}

Surface::~Surface() {
  Device* d = com::As<Device>(device);
  if (d && depth) d->gfx->DestroyDepth(depth);
  if (d && tex) d->gfx->DestroyTexture(tex);
  if (mem) HeapFreeProcess(mem);
}

Shader::~Shader() {
  Device* d = com::As<Device>(device);
  if (d && id) d->gfx->DestroyShader(id);
}

Texture::~Texture() {
  Device* d = com::As<Device>(device);
  if (d && tex) d->gfx->DestroyTexture(tex);
  for (Level& l : lv) {
    if (l.mem && l.shared) {
      SharedHeap& s = Shared();
      std::lock_guard<std::mutex> lock(s.mu);
      Retire(s, d && d->gfx ? d->gfx.get() : nullptr, l.mem, l.gpu_use);
    } else if (l.mem) {
      HeapFreeProcess(l.mem);
    }
    if (l.surface) com::Release(l.surface->guest);
  }
}

void SetUpSharedBuffers(Device& d) {
  if (!d.gfx->SharesMemory()) return;
  SharedHeap& s = Shared();
  std::lock_guard<std::mutex> l(s.mu);
  if (!s.heap) {
    const char* e = Env("GPU_HEAP_MB");
    const uint64_t mb = e ? std::strtoull(e, nullptr, 10) : 512;
    if (!mb) return;
    s.heap = std::make_unique<GuestHeap>(g_proc->vm, mb << 20, mb << 20);
    if (!s.heap->handle()) {
      WarnF("d3d9: no room for a %llu MB shared buffer heap\n", (unsigned long long)mb);
      s.heap.reset();
      return;
    }
    s.size = mb << 20;
  }
  Reclaim(s, nullptr);
  d.shared_buffers = d.gfx->MapSharedMemory(GPtr(s.heap->handle()), s.size);
  if (d.shared_buffers)
    std::fprintf(stderr, "d3d9: vertex and index buffers in %llu MB the GPU reads in place\n",
                 (unsigned long long)(s.size >> 20));
}

Buffer::~Buffer() {
  if (!mem) return;
  if (!shared) {
    HeapFreeProcess(mem);
    return;
  }
  SharedHeap& s = Shared();
  Device* d = com::As<Device>(device);
  std::lock_guard<std::mutex> l(s.mu);
  Retire(s, d && d->gfx ? d->gfx.get() : nullptr, mem, last_use);
}

uint32_t NewTexture(Device& d, uint32_t device, bool cube, uint32_t w, uint32_t h,
                    uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool,
                    uint32_t* out) {
  *out = 0;
  gfx::Format f = FormatFromD3D(fmt);
  // A depth texture: INTZ, or a depth-stencil format, which D3D9 reads by
  // comparing (gfx::Device::SetDepthCompare).
  const bool intz = (fmt == D3DFMT_INTZ || IsDepthFormat(f)) && (usage & D3DUSAGE_DEPTHSTENCIL) &&
                    pool == D3DPOOL_DEFAULT && !cube;
  if (intz) levels = 1;
  if (!w || !h || f == gfx::Format::Unknown || (IsDepthFormat(f) && !intz) ||
      ((usage & D3DUSAGE_RENDERTARGET) && (pool != D3DPOOL_DEFAULT || cube))) {
    WarnF("d3d9: Create%sTexture %ux%u fmt %X usage %X pool %u: not supported\n",
          cube ? "Cube" : "", w, h, fmt, usage, pool);
    return D3DERR_INVALIDCALL;
  }
  uint32_t full = 1;
  for (uint32_t s = std::max(w, h); s > 1; s >>= 1) full++;
  if (!levels || levels > full) levels = full;
  if (usage & D3DUSAGE_AUTOGENMIPMAP) levels = 1;  // the app sees one level
  auto t = std::make_shared<Texture>();
  t->device = device;
  t->cube = cube;
  t->width = w;
  t->height = h;
  t->levels = levels;
  t->usage = usage;
  t->format = fmt;
  t->pool = pool;
  t->fmt = f;
  for (uint32_t face = 0; face < t->Faces(); face++)
    for (uint32_t i = 0; i < levels; i++) {
      Texture::Level l;
      l.w = std::max(1u, w >> i);
      l.h = std::max(1u, h >> i);
      l.pitch = gfx::RowPitch(f, l.w);
      l.size = l.pitch * gfx::RowCount(f, l.h);
      t->lv.push_back(l);
    }
  if (intz) {
    t->tex = d.gfx->CreateDepthTexture(int(w), int(h), &t->depth);
    if (!t->tex) return 0x8876017C;  // D3DERR_OUTOFVIDEOMEMORY
    if (fmt != D3DFMT_INTZ) d.gfx->SetDepthCompare(t->tex, true);
  } else if (pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH) {
    t->tex = cube ? d.gfx->CreateCubeTexture(int(w), int(levels), f)
                  : d.gfx->CreateTexture(int(w), int(h), int(levels), f,
                                         (usage & D3DUSAGE_RENDERTARGET) != 0);
    if (!t->tex) return 0x8876017C;  // D3DERR_OUTOFVIDEOMEMORY
  }
  *out = com::Publish(t, cube ? CubeTextureVtable() : TextureVtable());
  RECOMP_TRACE("  Create%sTexture %ux%u x%u fmt %X usage %X pool %u -> %08X\n",
               cube ? "Cube" : "", w, h, levels, fmt, usage, pool, *out);
  return D3D_OK;
}

void FillLevel(Texture& t, uint32_t face, uint32_t level, const uint8_t* src,
               uint32_t pitch) {
  Texture::Level& l = t.At(face, level);
  LevelMem(t, l);
  RenameLevelIfInUse(t, l);
  uint32_t row = gfx::RowPitch(t.fmt, l.w), rows = gfx::RowCount(t.fmt, l.h);
  for (uint32_t y = 0; y < rows; y++)
    std::memcpy(GPtr(l.mem + y * l.pitch), src + size_t(y) * pitch, row);
  if (t.tex) UploadLevel(t, face, level, {0, 0, int32_t(l.w), int32_t(l.h)});
}

// IDirect3DDevice9::UpdateTexture(src, dst): every level of a SYSTEMMEM
// texture to the matching level of a DEFAULT one (a source with more levels
// gives its smaller ones: level i of the destination is level i + extra of
// the source). Uploaded from the source's memory directly, without a lock
// shadow on the destination (texture streaming would keep a second copy of
// everything in the guest's address space). Dirty regions are not tracked:
// whole levels are sent.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::UpdateTexture", D3DDev9_UpdateTexture, 3) {
  Texture* src = com::As<Texture>(hle::Arg(c, 1));
  Texture* dst = com::As<Texture>(hle::Arg(c, 2));
  if (!src || !dst || src->cube != dst->cube || src->fmt != dst->fmt ||
      src->pool != D3DPOOL_SYSTEMMEM || dst->pool != D3DPOOL_DEFAULT ||
      src->levels < dst->levels)
    return D3DERR_INVALIDCALL;
  const uint32_t extra = src->levels - dst->levels;
  const uint32_t levels = (dst->usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : dst->levels;
  gfx::Device* g = DeviceOf(dst->device)->gfx.get();
  for (uint32_t face = 0; face < dst->Faces(); face++)
    for (uint32_t level = 0; level < levels; level++) {
      const Texture::Level& s = src->At(face, level + extra);
      const Texture::Level& d = dst->At(face, level);
      if (!s.mem || s.w != d.w || s.h != d.h) continue;  // never written
      const gfx::Rect r{0, 0, int32_t(d.w), int32_t(d.h)};
      if (dst->cube)
        g->UpdateCubeTexture(dst->tex, int(face), int(level), r, GPtr(s.mem), s.pitch);
      else
        g->UpdateTexture(dst->tex, int(level), r, GPtr(s.mem), s.pitch);
    }
  return D3D_OK;
}

uint32_t NewSurface(Device& d, uint32_t device, Surface::Kind kind, uint32_t w,
                    uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
                    std::shared_ptr<Surface>* keep) {
  auto s = std::make_shared<Surface>();
  s->kind = kind;
  s->device = device;
  s->desc = {fmt, D3DRTYPE_SURFACE, usage, pool, 0, 0, w, h};
  gfx::Format f = FormatFromD3D(fmt);
  switch (kind) {
    case Surface::Kind::DepthStencil:
      s->depth = d.gfx->CreateDepth(int(w), int(h), f);
      break;
    case Surface::Kind::RenderTarget:
      s->tex = d.gfx->CreateTexture(int(w), int(h), 1, f, true);
      break;
    case Surface::Kind::Offscreen:
      s->pitch = gfx::RowPitch(f, w);
      s->mem = HeapAllocProcess(s->pitch * gfx::RowCount(f, h), true);
      break;
    default:
      break;
  }
  if (keep) *keep = s;
  return com::Publish(s, SurfaceVtable());
}

uint32_t SurfaceRefTarget(uint32_t surface) {
  Surface* s = com::As<Surface>(surface);
  return s && s->container ? s->container : surface;
}

void BindTargets(Device& d) {
  Surface* z = d.depth_stencil.obj;
  gfx::ColorTarget color[4] = {};
  for (int i = 0; i < 4; i++) {
    Surface* rt = d.render_targets[i].obj;
    if (!rt) break;
    if (rt->kind == Surface::Kind::RenderTarget) color[i].tex = rt->tex;
    if (rt->kind == Surface::Kind::TextureLevel) {
      if (rt->texture->cube)
        FatalF("d3d9: render target %d is a cube face (not supported)", i);
      color[i] = {rt->texture->tex, int(rt->level)};
    }
    if (i > 0 && !color[i].tex)
      FatalF("d3d9: render target %d is a %s surface", i,
             rt->kind == Surface::Kind::BackBuffer ? "back buffer" : "non-target");
  }
  // A depth surface is the device's own or level 0 of an INTZ texture.
  const gfx::DepthId depth =
      !z ? 0 : z->kind == Surface::Kind::TextureLevel ? z->texture->depth : z->depth;
  d.gfx->SetRenderTargets(color, depth);
  // A backend may give a new target its whole size as the viewport and the
  // scissor rectangle (D3D9's SetRenderTarget does): the next draw sends both
  // again, as otherwise only their changes are sent.
  d.synced = false;
}

}  // namespace recomp::d3d9

namespace recomp::hle {
namespace {

using namespace d3d9;
using namespace guest;

Device* Dev(Ctx& c) { return DeviceOf(Arg(c, 0)); }

// ---- IDirect3DDevice9: creating resources -----------------------------------------

// (Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateTexture", D3DDev9_CreateTexture, 9) {
  uint32_t out;
  uint32_t hr = NewTexture(*Dev(c), Arg(c, 0), false, Arg(c, 1), Arg(c, 2), Arg(c, 3),
                           Arg(c, 4), Arg(c, 5), Arg(c, 6), &out);
  Wr32(Arg(c, 7), out);
  return hr;
}

// (EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateCubeTexture", D3DDev9_CreateCubeTexture, 8) {
  uint32_t out;
  uint32_t hr = NewTexture(*Dev(c), Arg(c, 0), true, Arg(c, 1), Arg(c, 1), Arg(c, 2),
                           Arg(c, 3), Arg(c, 4), Arg(c, 5), &out);
  Wr32(Arg(c, 6), out);
  return hr;
}

uint32_t CreateBuffer(Ctx& c, bool index) {
  // (Length, Usage, FVF or Format, Pool, ppBuffer, pSharedHandle)
  uint32_t length = Arg(c, 1), out = Arg(c, 5);
  if (!length) {
    Wr32(out, 0);
    return D3DERR_INVALIDCALL;
  }
  auto b = std::make_shared<Buffer>();
  b->device = Arg(c, 0);
  b->index = index;
  b->length = length;
  b->usage = Arg(c, 2);
  b->fvf_or_format = Arg(c, 3);
  b->pool = Arg(c, 4);
  Device* d = com::As<Device>(b->device);
  if (d && d->shared_buffers) {
    SharedHeap& s = Shared();
    std::lock_guard<std::mutex> l(s.mu);
    b->mem = uint32_t(SharedAlloc(s, d->gfx.get(), length, true));
    b->shared = b->mem != 0;
  }
  if (!b->mem) b->mem = HeapAllocProcess(length, true);
  Wr32(out, com::Publish(b, BufferVtable(index)));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateVertexBuffer", D3DDev9_CreateVertexBuffer, 7) {
  return CreateBuffer(c, false);
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateIndexBuffer", D3DDev9_CreateIndexBuffer, 7) {
  return CreateBuffer(c, true);
}

// (Width, Height, Format, MultiSample, MultisampleQuality, Lockable,
// ppSurface, pSharedHandle)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateRenderTarget", D3DDev9_CreateRenderTarget, 9) {
  Device* d = Dev(c);
  uint32_t fmt = Arg(c, 3), out = Arg(c, 7);
  if (!IsRenderTargetFormat(fmt) || Arg(c, 4) != 0) {
    Wr32(out, 0);
    return D3DERR_INVALIDCALL;
  }
  Wr32(out, NewSurface(*d, Arg(c, 0), Surface::Kind::RenderTarget, Arg(c, 1),
                       Arg(c, 2), fmt, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
                       nullptr));
  return D3D_OK;
}

// (Width, Height, Format, MultiSample, MultisampleQuality, Discard,
// ppSurface, pSharedHandle)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateDepthStencilSurface", D3DDev9_CreateDepthStencilSurface, 9) {
  Device* d = Dev(c);
  uint32_t fmt = Arg(c, 3), out = Arg(c, 7);
  if (!IsDepthFormat(FormatFromD3D(fmt)) || Arg(c, 4) != 0) {
    Wr32(out, 0);
    return D3DERR_INVALIDCALL;
  }
  Wr32(out, NewSurface(*d, Arg(c, 0), Surface::Kind::DepthStencil, Arg(c, 1),
                       Arg(c, 2), fmt, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT,
                       nullptr));
  return D3D_OK;
}

// (Width, Height, Format, Pool, ppSurface, pSharedHandle)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateOffscreenPlainSurface", D3DDev9_CreateOffscreenPlainSurface, 7) {
  Device* d = Dev(c);
  uint32_t fmt = Arg(c, 3), out = Arg(c, 5);
  gfx::Format f = FormatFromD3D(fmt);
  if (f == gfx::Format::Unknown || IsDepthFormat(f)) {
    Wr32(out, 0);
    return D3DERR_INVALIDCALL;
  }
  Wr32(out, NewSurface(*d, Arg(c, 0), Surface::Kind::Offscreen, Arg(c, 1),
                       Arg(c, 2), fmt, 0, Arg(c, 4), nullptr));
  return D3D_OK;
}

// (pVertexElements, ppDecl)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateVertexDeclaration", D3DDev9_CreateVertexDeclaration, 3) {
  auto v = std::make_shared<VertexDecl>();
  v->device = Arg(c, 0);
  v->elements = ReadDecl(Arg(c, 1));
  Wr32(Arg(c, 2), com::Publish(v, VertexDeclVtable()));
  return D3D_OK;
}

// <ID>_SHADER_DUMP=dir: shaders created from compiled bytecode are written
// to dir/<FNV-1a>.vso / .pso, as D3D9 tokens (recomp_hlslc reads them).
void DumpBytecode(const Shader& s) {
  const char* dir = Env("SHADER_DUMP");
  if (!dir || !*dir) return;
  uint64_t h = 1469598103934665603ull;
  for (uint32_t t : s.tokens)
    for (int i = 0; i < 4; i++) h = (h ^ ((t >> (8 * i)) & 0xFF)) * 1099511628211ull;
  char path[512];
  std::snprintf(path, sizeof path, "%s/%016llx.%s", dir, (unsigned long long)h,
                s.pixel ? "pso" : "vso");
  if (FILE* f = std::fopen(path, "wb")) {
    std::fwrite(s.tokens.data(), 4, s.tokens.size(), f);
    std::fclose(f);
  }
}

uint32_t CreateShader(Ctx& c, bool pixel) {
  // (pFunction, ppShader)
  if (!Arg(c, 1)) return D3DERR_INVALIDCALL;
  auto s = std::make_shared<Shader>();
  s->device = Arg(c, 0);
  s->pixel = pixel;
  s->tokens = ReadFunction(Arg(c, 1));
  if (const auto fix = recomp::Game().shader_tokens) fix(s->tokens);
  s->program = ReadTranslated(s->tokens);
  if (!s->program) {
    // Compiled bytecode: translated here.
    DumpBytecode(*s);
    auto p = std::make_shared<hlsl::Program>();
    std::string err;
    if (!hlsl::TranslateBytecode(s->tokens.data(), s->tokens.size(), p.get(), &err))
      FatalF("d3d9: Create%sShader: %s (<ID>_SHADER_DUMP writes the tokens)",
             pixel ? "Pixel" : "Vertex", err.c_str());
    s->program = std::move(p);
  }
  if ((s->program->stage == hlsl::Stage::Pixel) != pixel)
    FatalF("d3d9: Create%sShader with a %s shader", pixel ? "Pixel" : "Vertex",
           pixel ? "vertex" : "pixel");
  s->id = Dev(c)->gfx->CreateShader(*s->program);
  Wr32(Arg(c, 2), com::Publish(s, ShaderVtable(pixel)));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateVertexShader", D3DDev9_CreateVertexShader, 3) {
  return CreateShader(c, false);
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreatePixelShader", D3DDev9_CreatePixelShader, 3) {
  return CreateShader(c, true);
}

// (pRenderTarget, pDestSurface): the target's pixels into a system-memory
// surface of the same size and format (an offscreen plain surface or a
// level of a SYSTEMMEM texture).
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetRenderTargetData", D3DDev9_GetRenderTargetData, 3) {
  Device* d = Dev(c);
  Surface* src = com::As<Surface>(Arg(c, 1));
  Surface* dst = com::As<Surface>(Arg(c, 2));
  if (!src || !dst || src->desc.Width != dst->desc.Width ||
      src->desc.Height != dst->desc.Height || src->desc.Format != dst->desc.Format)
    return D3DERR_INVALIDCALL;
  gfx::ColorTarget from[4] = {};
  switch (src->kind) {
    case Surface::Kind::BackBuffer: break;
    case Surface::Kind::RenderTarget: from[0].tex = src->tex; break;
    case Surface::Kind::TextureLevel:
      if (src->texture->cube || !src->texture->tex) return D3DERR_INVALIDCALL;
      from[0] = {src->texture->tex, int(src->level)};
      break;
    default: return D3DERR_INVALIDCALL;
  }
  uint32_t mem, pitch;
  if (dst->kind == Surface::Kind::Offscreen) {
    mem = dst->mem;
    pitch = dst->pitch;
  } else if (dst->kind == Surface::Kind::TextureLevel && dst->texture->pool == D3DPOOL_SYSTEMMEM) {
    Texture::Level& l = dst->texture->At(uint32_t(dst->face), uint32_t(dst->level));
    LevelMem(*dst->texture, l);
    mem = l.mem;
    pitch = l.pitch;
  } else {
    return D3DERR_INVALIDCALL;
  }
  d->gfx->SetRenderTargets(from, 0);
  d->gfx->ReadBack({0, 0, int32_t(src->desc.Width), int32_t(src->desc.Height)}, GPtr(mem), pitch,
                   FormatFromD3D(src->desc.Format));
  BindTargets(*d);
  return D3D_OK;
}

// (Type, ppQuery): null ppQuery only asks whether the type is supported.
// EVENT and OCCLUSION so far, answered at once (draws are not tracked): an
// event is signalled, an occlusion query finds its draws fully visible.
// Games take occlusion support for granted (every D3D9 GPU has it) and
// keep the query without checking the result.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::CreateQuery", D3DDev9_CreateQuery, 3) {
  uint32_t type = Arg(c, 1), out = Arg(c, 2);
  if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION) {
    RECOMP_TRACE("  CreateQuery type %u: not available\n", type);
    if (out) Wr32(out, 0);
    return D3DERR_NOTAVAILABLE;
  }
  if (!out) return D3D_OK;
  auto q = std::make_shared<Query>();
  q->device = Arg(c, 0);
  q->type = type;
  Wr32(out, com::Publish(q, QueryVtable()));
  return D3D_OK;
}

// ---- IDirect3DDevice9: binding ------------------------------------------------------

// (RenderTargetIndex, pRenderTarget): setting target 0 also resets the
// viewport and scissor rectangle to the whole target.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetRenderTarget", D3DDev9_SetRenderTarget, 3) {
  Device* d = Dev(c);
  uint32_t i = Arg(c, 1), surf = Arg(c, 2);
  if (i >= 4 || (i == 0 && !surf)) return D3DERR_INVALIDCALL;
  if (surf && !com::As<Surface>(surf)) FatalF("d3d9: SetRenderTarget: %08X is not a surface", surf);
  Bind(d->render_targets[i], surf);
  if (i == 0) {
    Surface* s = d->render_targets[0].obj;
    d->viewport = {0, 0, s->desc.Width, s->desc.Height, 0.0f, 1.0f};
    d->scissor = {0, 0, LONG(s->desc.Width), LONG(s->desc.Height)};
  }
  BindTargets(*d);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetDepthStencilSurface", D3DDev9_SetDepthStencilSurface, 2) {
  Device* d = Dev(c);
  Bind(d->depth_stencil, Arg(c, 1));
  BindTargets(*d);
  return D3D_OK;
}

// (Sampler, pTexture)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetTexture", D3DDev9_SetTexture, 3) {
  int s = SamplerIndex(Arg(c, 1));
  if (s < 0) return D3DERR_INVALIDCALL;
  Bind(Dev(c)->textures[s], Arg(c, 2));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetTexture", D3DDev9_GetTexture, 3) {
  int s = SamplerIndex(Arg(c, 1));
  if (s < 0) return D3DERR_INVALIDCALL;
  uint32_t t = Dev(c)->textures[s].guest;
  if (t) com::AddRef(t);
  Wr32(Arg(c, 2), t);
  return D3D_OK;
}

// (StreamNumber, pStreamData, OffsetInBytes, Stride)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetStreamSource", D3DDev9_SetStreamSource, 5) {
  uint32_t n = Arg(c, 1);
  if (n >= 16) return D3DERR_INVALIDCALL;
  Device::Stream& s = Dev(c)->streams[n];
  Bind(s.buffer, Arg(c, 2));
  s.offset = Arg(c, 3);
  s.stride = Arg(c, 4);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetStreamSource", D3DDev9_GetStreamSource, 5) {
  uint32_t n = Arg(c, 1);
  if (n >= 16) return D3DERR_INVALIDCALL;
  const Device::Stream& s = Dev(c)->streams[n];
  if (s.buffer.guest) com::AddRef(s.buffer.guest);
  Wr32(Arg(c, 2), s.buffer.guest);
  Wr32(Arg(c, 3), s.offset);
  Wr32(Arg(c, 4), s.stride);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetStreamSourceFreq", D3DDev9_SetStreamSourceFreq, 3) {
  uint32_t n = Arg(c, 1);
  if (n >= 16) return D3DERR_INVALIDCALL;
  Dev(c)->streams[n].freq = Arg(c, 2);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetStreamSourceFreq", D3DDev9_GetStreamSourceFreq, 3) {
  uint32_t n = Arg(c, 1);
  if (n >= 16) return D3DERR_INVALIDCALL;
  Wr32(Arg(c, 2), Dev(c)->streams[n].freq);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetIndices", D3DDev9_SetIndices, 2) {
  Bind(Dev(c)->indices, Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetIndices", D3DDev9_GetIndices, 2) {
  uint32_t b = Dev(c)->indices.guest;
  if (b) com::AddRef(b);
  Wr32(Arg(c, 1), b);
  return D3D_OK;
}

// A declaration replaces the FVF and the other way round (SetFVF).
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetVertexDeclaration", D3DDev9_SetVertexDeclaration, 2) {
  Bind(Dev(c)->decl, Arg(c, 1));
  Dev(c)->fvf = 0;
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetVertexDeclaration", D3DDev9_GetVertexDeclaration, 2) {
  uint32_t v = Dev(c)->decl.guest;
  if (v) com::AddRef(v);
  Wr32(Arg(c, 1), v);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetVertexShader", D3DDev9_SetVertexShader, 2) {
  Bind(Dev(c)->vs, Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetVertexShader", D3DDev9_GetVertexShader, 2) {
  uint32_t v = Dev(c)->vs.guest;
  if (v) com::AddRef(v);
  Wr32(Arg(c, 1), v);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetPixelShader", D3DDev9_SetPixelShader, 2) {
  Bind(Dev(c)->ps, Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetPixelShader", D3DDev9_GetPixelShader, 2) {
  uint32_t v = Dev(c)->ps.guest;
  if (v) com::AddRef(v);
  Wr32(Arg(c, 1), v);
  return D3D_OK;
}

// Shader constants: (StartRegister, pConstantData, Count) in units of
// float4 / int4 / BOOL. Float NaNs are kept as 0 (DXVK's float emulation
// does the same): D3D9 hardware multiplies them by 0 to 0, while here a
// NaN reaching a pixel spreads through everything computed from it.
template <typename T, size_t N, size_t W>
uint32_t SetConstants(Ctx& c, T (&regs)[N][W]) {
  uint32_t start = Arg(c, 1), count = Arg(c, 3);
  if (start > N || count > N - start) return D3DERR_INVALIDCALL;
  std::memcpy(regs[start], GPtr(Arg(c, 2)), size_t(count) * sizeof(T) * W);
  if constexpr (std::is_same_v<T, float>) {
    uint32_t* u = reinterpret_cast<uint32_t*>(regs[start]);
    for (size_t i = 0; i < size_t(count) * W; i++)
      if ((u[i] & 0x7FFFFFFFu) > 0x7F800000u) u[i] = 0;
  }
  return D3D_OK;
}
template <typename T, size_t N, size_t W>
uint32_t GetConstants(Ctx& c, T (&regs)[N][W]) {
  uint32_t start = Arg(c, 1), count = Arg(c, 3);
  if (start > N || count > N - start) return D3DERR_INVALIDCALL;
  std::memcpy(GPtr(Arg(c, 2)), regs[start], size_t(count) * sizeof(T) * W);
  return D3D_OK;
}
template <size_t N>
uint32_t SetBools(Ctx& c, BOOL (&regs)[N]) {
  uint32_t start = Arg(c, 1), count = Arg(c, 3);
  if (start > N || count > N - start) return D3DERR_INVALIDCALL;
  std::memcpy(&regs[start], GPtr(Arg(c, 2)), size_t(count) * sizeof(BOOL));
  return D3D_OK;
}
template <size_t N>
uint32_t GetBools(Ctx& c, BOOL (&regs)[N]) {
  uint32_t start = Arg(c, 1), count = Arg(c, 3);
  if (start > N || count > N - start) return D3DERR_INVALIDCALL;
  std::memcpy(GPtr(Arg(c, 2)), &regs[start], size_t(count) * sizeof(BOOL));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetVertexShaderConstantF", D3DDev9_SetVertexShaderConstantF, 4) {
  Dev(c)->vs_f_version++;
  return SetConstants(c, Dev(c)->vs_f);
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetVertexShaderConstantF", D3DDev9_GetVertexShaderConstantF, 4) { return GetConstants(c, Dev(c)->vs_f); }
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetVertexShaderConstantI", D3DDev9_SetVertexShaderConstantI, 4) {
  Dev(c)->vs_ib_version++;
  return SetConstants(c, Dev(c)->vs_i);
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetVertexShaderConstantI", D3DDev9_GetVertexShaderConstantI, 4) { return GetConstants(c, Dev(c)->vs_i); }
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetVertexShaderConstantB", D3DDev9_SetVertexShaderConstantB, 4) {
  Dev(c)->vs_ib_version++;
  return SetBools(c, Dev(c)->vs_b);
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetVertexShaderConstantB", D3DDev9_GetVertexShaderConstantB, 4) { return GetBools(c, Dev(c)->vs_b); }
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetPixelShaderConstantF", D3DDev9_SetPixelShaderConstantF, 4) {
  Dev(c)->ps_f_version++;
  return SetConstants(c, Dev(c)->ps_f);
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetPixelShaderConstantF", D3DDev9_GetPixelShaderConstantF, 4) { return GetConstants(c, Dev(c)->ps_f); }
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetPixelShaderConstantI", D3DDev9_SetPixelShaderConstantI, 4) {
  Dev(c)->ps_ib_version++;
  return SetConstants(c, Dev(c)->ps_i);
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetPixelShaderConstantI", D3DDev9_GetPixelShaderConstantI, 4) { return GetConstants(c, Dev(c)->ps_i); }
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetPixelShaderConstantB", D3DDev9_SetPixelShaderConstantB, 4) {
  Dev(c)->ps_ib_version++;
  return SetBools(c, Dev(c)->ps_b);
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetPixelShaderConstantB", D3DDev9_GetPixelShaderConstantB, 4) { return GetBools(c, Dev(c)->ps_b); }

// ---- IDirect3DSurface9 -------------------------------------------------------------

uint32_t RefTarget(uint32_t self) { return SurfaceRefTarget(self); }

HLE_NAMED("d3d9.dll", "IDirect3DSurface9::QueryInterface", D3DSurf9_QueryInterface, 3) {
  if (com::GuidEq(Arg(c, 1), kIID_IDirect3DSurface9) ||
      com::GuidEq(Arg(c, 1), kIID_IDirect3DResource9) ||
      com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(RefTarget(Arg(c, 0)));
    Wr32(Arg(c, 2), Arg(c, 0));
    return D3D_OK;
  }
  Wr32(Arg(c, 2), 0);
  return com::kE_NOINTERFACE;
}
HLE_NAMED("d3d9.dll", "IDirect3DSurface9::AddRef", D3DSurf9_AddRef, 1) { return com::AddRef(RefTarget(Arg(c, 0))); }
HLE_NAMED("d3d9.dll", "IDirect3DSurface9::Release", D3DSurf9_Release, 1) { return com::Release(RefTarget(Arg(c, 0))); }
HLE_NAMED("d3d9.dll", "IDirect3DSurface9::GetDevice", D3DSurf9_GetDevice, 2) { return GetDeviceOf(c, Surf(c)->device); }
HLE_NAMED("d3d9.dll", "IDirect3DSurface9::GetType", D3DSurf9_GetType, 1) { return D3DRTYPE_SURFACE; }
HLE_NAMED("d3d9.dll", "IDirect3DSurface9::SetPriority", D3DSurf9_SetPriority, 2) { return 0; }
HLE_NAMED("d3d9.dll", "IDirect3DSurface9::GetPriority", D3DSurf9_GetPriority, 1) { return 0; }
HLE_NAMED("d3d9.dll", "IDirect3DSurface9::PreLoad", D3DSurf9_PreLoad, 1) { return 0; }

HLE_NAMED("d3d9.dll", "IDirect3DSurface9::GetDesc", D3DSurf9_GetDesc, 2) {
  *G<D3DSURFACE_DESC>(Arg(c, 1)) = Surf(c)->desc;
  return D3D_OK;
}

// (riid, ppContainer): a texture level's texture (if asked for a texture
// interface), otherwise the device.
HLE_NAMED("d3d9.dll", "IDirect3DSurface9::GetContainer", D3DSurf9_GetContainer, 3) {
  Surface* s = Surf(c);
  uint32_t riid = Arg(c, 1), out = Arg(c, 2);
  if (s->container && (com::GuidEq(riid, s->texture->cube ? kIID_IDirect3DCubeTexture9
                                                           : kIID_IDirect3DTexture9) ||
                       com::GuidEq(riid, kIID_IDirect3DBaseTexture9) ||
                       com::GuidEq(riid, kIID_IDirect3DResource9))) {
    com::AddRef(s->container);
    Wr32(out, s->container);
    return D3D_OK;
  }
  if (!s->container) {
    com::AddRef(s->device);
    Wr32(out, s->device);
    return D3D_OK;
  }
  Wr32(out, 0);
  return com::kE_NOINTERFACE;
}

// (pLockedRect, pRect, Flags)
HLE_NAMED("d3d9.dll", "IDirect3DSurface9::LockRect", D3DSurf9_LockRect, 4) {
  Surface* s = Surf(c);
  if (s->kind == Surface::Kind::TextureLevel)
    return LockLevel(*s->texture, uint32_t(s->face), uint32_t(s->level), Arg(c, 1), Arg(c, 2),
                     Arg(c, 3));
  if (s->kind != Surface::Kind::Offscreen)
    FatalF("d3d9: LockRect of a %s surface not supported",
           s->kind == Surface::Kind::BackBuffer ? "back buffer" :
           s->kind == Surface::Kind::DepthStencil ? "depth stencil" : "render target");
  gfx::Format f = FormatFromD3D(s->desc.Format);
  gfx::Rect r{0, 0, int32_t(s->desc.Width), int32_t(s->desc.Height)};
  if (Arg(c, 2)) r = *G<gfx::Rect>(Arg(c, 2));
  uint32_t off = gfx::IsCompressed(f)
                     ? uint32_t(r.top / 4) * s->pitch + uint32_t(r.left / 4) * gfx::RowPitch(f, 4)
                     : uint32_t(r.top) * s->pitch + uint32_t(r.left) * gfx::BitsPerPixel(f) / 8;
  *G<D3DLOCKED_RECT>(Arg(c, 1)) = {INT(s->pitch), s->mem + off};
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DSurface9::UnlockRect", D3DSurf9_UnlockRect, 1) {
  Surface* s = Surf(c);
  if (s->kind == Surface::Kind::TextureLevel)
    return UnlockLevel(*s->texture, uint32_t(s->face), uint32_t(s->level));
  return D3D_OK;
}

// ---- IDirect3DTexture9 / IDirect3DCubeTexture9 ------------------------------------------

uint32_t TextureQueryInterface(Ctx& c) {
  Texture* t = Tex(c);
  if (com::GuidEq(Arg(c, 1), kIID_IDirect3DBaseTexture9) ||
      com::GuidEq(Arg(c, 1), kIID_IDirect3DResource9)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return D3D_OK;
  }
  return QueryInterfaceFor(c, t->cube ? kIID_IDirect3DCubeTexture9 : kIID_IDirect3DTexture9);
}

uint32_t SetPriority(Ctx& c) {
  Texture* t = Tex(c);
  uint32_t old = t->priority;
  t->priority = Arg(c, 1);
  return old;
}

// LOD: the most detailed level used (managed textures only); bookkeeping.
uint32_t SetLOD(Ctx& c) {
  Texture* t = Tex(c);
  uint32_t old = t->lod;
  if (t->pool == D3DPOOL_MANAGED) t->lod = std::min(uint32_t(Arg(c, 1)), t->levels - 1);
  return old;
}

uint32_t GetLevelDesc(Ctx& c) {
  Texture* t = Tex(c);
  uint32_t level = Arg(c, 1);
  if (level >= t->levels) return D3DERR_INVALIDCALL;
  const Texture::Level& l = t->At(0, level);
  *G<D3DSURFACE_DESC>(Arg(c, 2)) = {t->format, D3DRTYPE_SURFACE, t->usage, t->pool, 0, 0, l.w, l.h};
  return D3D_OK;
}

// (face, level, ppSurface): GetSurfaceLevel passes face 0.
uint32_t GetSurface(Ctx& c, uint32_t face, uint32_t level, uint32_t out) {
  Texture* t = Tex(c);
  if (level >= t->levels || face >= t->Faces()) {
    Wr32(out, 0);
    return D3DERR_INVALIDCALL;
  }
  Surface& s = LevelSurface(*t, face, level);
  com::AddRef(t->guest);
  Wr32(out, s.guest);
  return D3D_OK;
}

#define D3D9_TEXTURE_METHODS(Iface, Prefix, rtype)                                    \
  HLE_NAMED("d3d9.dll", Iface "::QueryInterface", Prefix##_QueryInterface, 3) {     \
    return TextureQueryInterface(c);                                                \
  }                                                                                 \
  HLE_NAMED("d3d9.dll", Iface "::AddRef", Prefix##_AddRef, 1) { return com::AddRef(Arg(c, 0)); } \
  HLE_NAMED("d3d9.dll", Iface "::Release", Prefix##_Release, 1) { return com::Release(Arg(c, 0)); } \
  HLE_NAMED("d3d9.dll", Iface "::GetDevice", Prefix##_GetDevice, 2) { return GetDeviceOf(c, Tex(c)->device); } \
  HLE_NAMED("d3d9.dll", Iface "::GetType", Prefix##_GetType, 1) { return rtype; }     \
  HLE_NAMED("d3d9.dll", Iface "::PreLoad", Prefix##_PreLoad, 1) { return 0; }        \
  HLE_NAMED("d3d9.dll", Iface "::GetLevelCount", Prefix##_GetLevelCount, 1) { return Tex(c)->levels; } \
  HLE_NAMED("d3d9.dll", Iface "::SetPriority", Prefix##_SetPriority, 2) { return SetPriority(c); } \
  HLE_NAMED("d3d9.dll", Iface "::GetPriority", Prefix##_GetPriority, 1) { return Tex(c)->priority; } \
  HLE_NAMED("d3d9.dll", Iface "::SetLOD", Prefix##_SetLOD, 2) { return SetLOD(c); }  \
  HLE_NAMED("d3d9.dll", Iface "::GetLOD", Prefix##_GetLOD, 1) { return Tex(c)->lod; } \
  HLE_NAMED("d3d9.dll", Iface "::SetAutoGenFilterType", Prefix##_SetAutoGenFilterType, 2) { \
    Tex(c)->autogen_filter = Arg(c, 1);                                             \
    return D3D_OK;                                                                  \
  }                                                                                 \
  HLE_NAMED("d3d9.dll", Iface "::GetAutoGenFilterType", Prefix##_GetAutoGenFilterType, 1) { \
    return Tex(c)->autogen_filter;                                                  \
  }                                                                                 \
  HLE_NAMED("d3d9.dll", Iface "::GetLevelDesc", Prefix##_GetLevelDesc, 3) { return GetLevelDesc(c); }

D3D9_TEXTURE_METHODS("IDirect3DTexture9", D3DTex9, D3DRTYPE_TEXTURE)
D3D9_TEXTURE_METHODS("IDirect3DCubeTexture9", D3DCube9, D3DRTYPE_CUBETEXTURE)

HLE_NAMED("d3d9.dll", "IDirect3DTexture9::GetSurfaceLevel", D3DTex9_GetSurfaceLevel, 3) {
  return GetSurface(c, 0, Arg(c, 1), Arg(c, 2));
}

// (Level, pLockedRect, pRect, Flags)
HLE_NAMED("d3d9.dll", "IDirect3DTexture9::LockRect", D3DTex9_LockRect, 5) {
  return LockLevel(*Tex(c), 0, Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4));
}

HLE_NAMED("d3d9.dll", "IDirect3DTexture9::UnlockRect", D3DTex9_UnlockRect, 2) {
  return UnlockLevel(*Tex(c), 0, Arg(c, 1));
}

// Locks upload what they cover on unlock, so there is nothing to mark.
HLE_NAMED("d3d9.dll", "IDirect3DTexture9::AddDirtyRect", D3DTex9_AddDirtyRect, 2) { return D3D_OK; }

// (FaceType, Level, ppCubeMapSurface)
HLE_NAMED("d3d9.dll", "IDirect3DCubeTexture9::GetCubeMapSurface", D3DCube9_GetCubeMapSurface, 4) {
  return GetSurface(c, Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

// (FaceType, Level, pLockedRect, pRect, Flags)
HLE_NAMED("d3d9.dll", "IDirect3DCubeTexture9::LockRect", D3DCube9_LockRect, 6) {
  return LockLevel(*Tex(c), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5));
}

HLE_NAMED("d3d9.dll", "IDirect3DCubeTexture9::UnlockRect", D3DCube9_UnlockRect, 3) {
  return UnlockLevel(*Tex(c), Arg(c, 1), Arg(c, 2));
}

HLE_NAMED("d3d9.dll", "IDirect3DCubeTexture9::AddDirtyRect", D3DCube9_AddDirtyRect, 3) { return D3D_OK; }

// ---- IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9 ------------------------------------

uint32_t BufferQueryInterface(Ctx& c) {
  Buffer* b = Buf(c);
  if (com::GuidEq(Arg(c, 1), kIID_IDirect3DResource9)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return D3D_OK;
  }
  return QueryInterfaceFor(c, b->index ? kIID_IDirect3DIndexBuffer9 : kIID_IDirect3DVertexBuffer9);
}

// (OffsetToLock, SizeToLock, ppbData, Flags): a pointer into the data.
uint32_t BufferLock(Ctx& c) {
  Buffer* b = Buf(c);
  uint32_t off = Arg(c, 1), size = Arg(c, 2), flags = Arg(c, 4);
  if (off > b->length || (size && size > b->length - off)) return D3DERR_INVALIDCALL;
  Device* d = com::As<Device>(b->device);
  if (b->shared && d && d->gfx && !(flags & (D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) &&
      b->last_use > d->gfx->CompletedSerial()) {
    SharedHeap& s = Shared();
    std::lock_guard<std::mutex> l(s.mu);
    if (gaddr n = SharedAlloc(s, d->gfx.get(), b->length, false)) {
      if (!(flags & D3DLOCK_DISCARD)) std::memcpy(GPtr(n), GPtr(b->mem), b->length);
      Retire(s, d->gfx.get(), b->mem, b->last_use);
      b->mem = uint32_t(n);
      b->last_use = 0;
    }
  }
  Wr32(Arg(c, 3), b->mem + off);
  return D3D_OK;
}

#define D3D9_BUFFER_METHODS(Iface, Prefix, is_index)                                  \
  HLE_NAMED("d3d9.dll", Iface "::QueryInterface", Prefix##_QueryInterface, 3) {     \
    return BufferQueryInterface(c);                                                 \
  }                                                                                 \
  HLE_NAMED("d3d9.dll", Iface "::AddRef", Prefix##_AddRef, 1) { return com::AddRef(Arg(c, 0)); } \
  HLE_NAMED("d3d9.dll", Iface "::Release", Prefix##_Release, 1) { return com::Release(Arg(c, 0)); } \
  HLE_NAMED("d3d9.dll", Iface "::GetDevice", Prefix##_GetDevice, 2) { return GetDeviceOf(c, Buf(c)->device); } \
  HLE_NAMED("d3d9.dll", Iface "::GetType", Prefix##_GetType, 1) {                   \
    return is_index ? D3DRTYPE_INDEXBUFFER : D3DRTYPE_VERTEXBUFFER;                 \
  }                                                                                 \
  HLE_NAMED("d3d9.dll", Iface "::PreLoad", Prefix##_PreLoad, 1) { return 0; }        \
  HLE_NAMED("d3d9.dll", Iface "::SetPriority", Prefix##_SetPriority, 2) {           \
    Buffer* b = Buf(c);                                                             \
    uint32_t old = b->priority;                                                     \
    b->priority = Arg(c, 1);                                                        \
    return old;                                                                     \
  }                                                                                 \
  HLE_NAMED("d3d9.dll", Iface "::GetPriority", Prefix##_GetPriority, 1) { return Buf(c)->priority; } \
  HLE_NAMED("d3d9.dll", Iface "::Lock", Prefix##_Lock, 5) { return BufferLock(c); }  \
  HLE_NAMED("d3d9.dll", Iface "::Unlock", Prefix##_Unlock, 1) { return D3D_OK; }

D3D9_BUFFER_METHODS("IDirect3DVertexBuffer9", D3DVB9, false)
D3D9_BUFFER_METHODS("IDirect3DIndexBuffer9", D3DIB9, true)

HLE_NAMED("d3d9.dll", "IDirect3DVertexBuffer9::GetDesc", D3DVB9_GetDesc, 2) {
  Buffer* b = Buf(c);
  *G<D3DVERTEXBUFFER_DESC>(Arg(c, 1)) = {0x64 /* D3DFMT_VERTEXDATA */, D3DRTYPE_VERTEXBUFFER,
                                         b->usage, b->pool, b->length, b->fvf_or_format};
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DIndexBuffer9::GetDesc", D3DIB9_GetDesc, 2) {
  Buffer* b = Buf(c);
  *G<D3DINDEXBUFFER_DESC>(Arg(c, 1)) = {b->fvf_or_format, D3DRTYPE_INDEXBUFFER, b->usage,
                                        b->pool, b->length};
  return D3D_OK;
}

// ---- vertex declarations, shaders, queries --------------------------------------------

HLE_NAMED("d3d9.dll", "IDirect3DVertexDeclaration9::AddRef", D3DDecl9_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3DVertexDeclaration9::Release", D3DDecl9_Release, 1) { return com::Release(Arg(c, 0)); }

// (pElement, pNumElements): the count includes D3DDECL_END.
HLE_NAMED("d3d9.dll", "IDirect3DVertexDeclaration9::GetDeclaration", D3DDecl9_GetDeclaration, 3) {
  VertexDecl* v = Obj<VertexDecl>(c, "IDirect3DVertexDeclaration9");
  uint32_t n = uint32_t(v->elements.size());
  if (Arg(c, 1))
    std::memcpy(GPtr(Arg(c, 1)), v->elements.data(), n * sizeof(D3DVERTEXELEMENT9));
  Wr32(Arg(c, 2), n);
  return D3D_OK;
}

uint32_t GetFunction(Ctx& c, const char* iface) {
  Shader* s = Obj<Shader>(c, iface);
  uint32_t size = uint32_t(s->tokens.size() * 4);
  if (Arg(c, 1)) {
    if (Rd32(Arg(c, 2)) < size) return D3DERR_INVALIDCALL;
    std::memcpy(GPtr(Arg(c, 1)), s->tokens.data(), size);
  }
  Wr32(Arg(c, 2), size);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DVertexShader9::AddRef", D3DVS9_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3DVertexShader9::Release", D3DVS9_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3DVertexShader9::GetFunction", D3DVS9_GetFunction, 3) {
  return GetFunction(c, "IDirect3DVertexShader9");
}
HLE_NAMED("d3d9.dll", "IDirect3DPixelShader9::AddRef", D3DPS9_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3DPixelShader9::Release", D3DPS9_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3DPixelShader9::GetFunction", D3DPS9_GetFunction, 3) {
  return GetFunction(c, "IDirect3DPixelShader9");
}

HLE_NAMED("d3d9.dll", "IDirect3DQuery9::AddRef", D3DQuery9_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3DQuery9::Release", D3DQuery9_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3DQuery9::GetType", D3DQuery9_GetType, 1) {
  return Obj<Query>(c, "IDirect3DQuery9")->type;
}
HLE_NAMED("d3d9.dll", "IDirect3DQuery9::GetDataSize", D3DQuery9_GetDataSize, 1) {
  return 4;  // EVENT: BOOL; OCCLUSION: DWORD
}
// An occlusion query is bracketed by BEGIN and END; an event takes END only.
HLE_NAMED("d3d9.dll", "IDirect3DQuery9::Issue", D3DQuery9_Issue, 2) {
  const uint32_t flags = Arg(c, 1);
  if (flags == D3DISSUE_END) return D3D_OK;
  if (flags == D3DISSUE_BEGIN && Obj<Query>(c, "IDirect3DQuery9")->type == D3DQUERYTYPE_OCCLUSION)
    return D3D_OK;
  return D3DERR_INVALIDCALL;
}
// (pData, dwSize, dwGetDataFlags): an EVENT is always signalled; an
// OCCLUSION query reports its draws visible (a count no draw reaches).
HLE_NAMED("d3d9.dll", "IDirect3DQuery9::GetData", D3DQuery9_GetData, 4) {
  const bool occlusion = Obj<Query>(c, "IDirect3DQuery9")->type == D3DQUERYTYPE_OCCLUSION;
  if (Arg(c, 1) && Arg(c, 2) >= 4) Wr32(Arg(c, 1), occlusion ? 0x7FFFFFFF : 1);
  return D3D_OK;
}

}  // namespace
}  // namespace recomp::hle
