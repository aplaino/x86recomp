// Direct3D 11: what ID3D11Device creates (resources, views, shaders, input
// layouts, states, queries) and the methods those objects share. All are
// host records (d3d11_impl.h).

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#include "runtime/log.h"
#include "win32/com.h"
#include "win32/d3d11_impl.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "x86/runtime.h"

namespace recomp::hle::d3d11 {

namespace {

constexpr const char* kDll = "d3d11.dll";

constexpr guest::GUID kIID_IUnknown = {
    0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr guest::GUID kIID_ID3D11DeviceChild = {
    0x1841e5c8, 0x16b0, 0x489b, {0xbc, 0xc8, 0x44, 0xcf, 0xb0, 0xd5, 0xde, 0xae}};
constexpr guest::GUID kIID_ID3D11Resource = {
    0xdc8e63f3, 0xd12b, 0x4952, {0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d}};
constexpr guest::GUID kIID_ID3D11Buffer = {
    0x48570b85, 0xd1ee, 0x4fcd, {0xa2, 0x50, 0xeb, 0x35, 0x07, 0x22, 0xb0, 0x37}};
constexpr guest::GUID kIID_ID3D11Texture1D = {
    0xf8fb5c27, 0xc6b3, 0x4f75, {0xa4, 0xc8, 0x43, 0x9a, 0xf2, 0xef, 0x56, 0x4c}};
constexpr guest::GUID kIID_ID3D11Texture2D = {
    0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
constexpr guest::GUID kIID_ID3D11Texture3D = {
    0x037e866e, 0xf56d, 0x4357, {0xa8, 0xaf, 0x9d, 0xab, 0xbe, 0x6e, 0x25, 0x0e}};

const std::vector<const char*> kChildMethods = {
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData",
    "SetPrivateDataInterface"};

std::vector<const char*> With(std::initializer_list<const char*> more) {
  std::vector<const char*> v = kChildMethods;
  v.insert(v.end(), more);
  return v;
}

gaddr VtableFor(const char* iface, std::initializer_list<const char*> more) {
  return com::Vtable(kDll, iface, With(more));
}

const char* ResourceIface(Dim d) {
  switch (d) {
    case Dim::Buffer: return "ID3D11Buffer";
    case Dim::Texture1D: return "ID3D11Texture1D";
    case Dim::Texture2D: return "ID3D11Texture2D";
    default: return "ID3D11Texture3D";
  }
}

gaddr ResourceVtable(Dim d) {
  return VtableFor(ResourceIface(d), {"GetType", "SetEvictionPriority", "GetEvictionPriority",
                                      "GetDesc"});
}

const char* ViewIface(ViewKind k) {
  switch (k) {
    case ViewKind::ShaderResource: return "ID3D11ShaderResourceView";
    case ViewKind::RenderTarget: return "ID3D11RenderTargetView";
    case ViewKind::DepthStencil: return "ID3D11DepthStencilView";
    default: return "ID3D11UnorderedAccessView";
  }
}

const char* StateIface(StateKind k) {
  switch (k) {
    case StateKind::Blend: return "ID3D11BlendState";
    case StateKind::DepthStencil: return "ID3D11DepthStencilState";
    case StateKind::Rasterizer: return "ID3D11RasterizerState";
    default: return "ID3D11SamplerState";
  }
}

uint64_t Fnv64(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

// The resource behind a guest ID3D11Resource*, fatal if it is not one.
std::shared_ptr<Resource> ResourceAt(gaddr p) {
  auto o = std::dynamic_pointer_cast<Resource>(com::Lookup(p));
  if (!o) FatalF("d3d11: %08llX is not a resource", (unsigned long long)p);
  return o;
}

}  // namespace

// ---- formats ------------------------------------------------------------------------

uint32_t FormatBytes(uint32_t f, bool* block) {
  *block = false;
  switch (f) {
    case 1: case 2: case 3: case 4: return 16;                      // R32G32B32A32
    case 5: case 6: case 7: case 8: return 12;                      // R32G32B32
    case 9: case 10: case 11: case 12: case 13: case 14: return 8;  // R16G16B16A16
    case 15: case 16: case 17: case 18: return 8;                   // R32G32
    case 19: case 20: case 21: case 22: return 8;                   // R32G8X24
    case 23: case 24: case 25: case 26: return 4;                   // R10G10B10A2, R11G11B10
    case 27: case 28: case 29: case 30: case 31: case 32: return 4; // R8G8B8A8
    case 33: case 34: case 35: case 36: case 37: case 38: return 4; // R16G16
    case 39: case 40: case 41: case 42: case 43: return 4;          // R32
    case 44: case 45: case 46: case 47: return 4;                   // R24G8
    case 48: case 49: case 50: case 51: case 52: return 2;          // R8G8
    case 53: case 54: case 55: case 56: case 57: case 58: case 59: return 2;  // R16
    case 60: case 61: case 62: case 63: case 64: case 65: return 1; // R8, A8
    case 67: return 4;                                              // R9G9B9E5
    case 85: case 86: return 2;                                     // B5G6R5, B5G5R5A1
    case 87: case 88: case 89: case 90: case 91: case 92: case 93: return 4;  // B8G8R8A8/X8
    case 70: case 71: case 72: case 79: case 80: case 81: *block = true; return 8;  // BC1, BC4
    case 73: case 74: case 75: case 76: case 77: case 78: case 82: case 83: case 84:
    case 94: case 95: case 96: case 97: case 98: case 99: *block = true; return 16;  // BC2/3/5/6/7
    default: return 0;
  }
}

Child::Child() {
  static std::atomic<uint64_t> next{1};
  serial = next++;
}

Block::Block(uint64_t n) : size(n) {
  mem = HeapAllocProcess(n ? n : 1, true);
  if (!mem) FatalF("d3d11: no guest memory for %llu bytes of a resource", (unsigned long long)n);
}
Block::~Block() { HeapFreeProcess(mem); }
uint8_t* Block::Data() const { return GPtr(mem); }

std::shared_ptr<Block> Resource::Contents() const {
  std::lock_guard<std::mutex> l(mu_);
  return block_;
}
void Resource::SetContents(std::shared_ptr<Block> b) {
  std::lock_guard<std::mutex> l(mu_);
  block_ = std::move(b);
}

// The GPU objects go with the resource (any thread may release it).
Resource::~Resource() {
  Device* d = TheDevice();
  if (!d || !d->gfx || (!image && !depth_target && !gpu_buffer)) return;
  std::lock_guard<std::recursive_mutex> l(d->gpu_mu);
  if (image) d->gfx->DestroyTexture(image);
  if (depth_target) d->gfx->DestroyDepth(depth_target);
  if (gpu_buffer) d->gfx->DestroyBuffer(gpu_buffer);
}

uint32_t Resource::RowPitch(uint32_t mip) const {
  bool block;
  uint32_t b = FormatBytes(format, &block);
  uint32_t w = MipWidth(mip);
  return block ? ((w + 3) / 4) * b : w * b;
}
uint32_t Resource::SlicePitch(uint32_t mip) const {
  bool block;
  FormatBytes(format, &block);
  uint32_t h = MipHeight(mip);
  return RowPitch(mip) * (block ? (h + 3) / 4 : h);
}
uint64_t Resource::SubOffset(uint32_t sub) const {
  if (dim == Dim::Buffer) return 0;
  const uint32_t mip = sub % mips, slice = sub / mips;
  uint64_t per_slice = 0;  // every mip of one array slice
  for (uint32_t m = 0; m < mips; m++) per_slice += uint64_t(SlicePitch(m)) * MipDepth(m);
  uint64_t off = per_slice * slice;
  for (uint32_t m = 0; m < mip; m++) off += uint64_t(SlicePitch(m)) * MipDepth(m);
  return off;
}

// ---- creating resources ---------------------------------------------------------------

namespace {

uint32_t MipCount(uint32_t w, uint32_t h, uint32_t d) {
  uint32_t n = 1;
  while ((w | h | d) > 1) {
    w >>= 1, h >>= 1, d >>= 1;
    n++;
  }
  return n;
}

// D3D11_SUBRESOURCE_DATA {pSysMem, SysMemPitch, SysMemSlicePitch}: the
// initial contents of each subresource, copied in with the guest's pitches.
bool Fill(Resource& r, gaddr init) {
  bool block = false;
  if (r.dim != Dim::Buffer && !FormatBytes(r.format, &block))
    FatalF("d3d11: texture format %u not supported", r.format);
  r.size = r.dim == Dim::Buffer ? r.byte_width : r.SubOffset(r.mips * r.array);
  r.SetContents(std::make_shared<Block>(r.size));
  if (!init) return true;
  r.version = 1;  // contents the GPU's copy starts from
  if (r.dim == Dim::Buffer) {
    std::memcpy(r.Data(), GPtr(Rd64(init)), r.byte_width);
    return true;
  }
  for (uint32_t sub = 0; sub < r.mips * r.array; sub++) {
    const gaddr d = init + 16 * sub;
    const uint32_t mip = sub % r.mips;
    const gaddr src = Rd64(d);
    const uint32_t pitch = r.dim == Dim::Texture1D ? r.RowPitch(mip) : Rd32(d + 8);
    const uint32_t slice_pitch = r.dim == Dim::Texture3D ? Rd32(d + 12) : 0;
    const uint32_t rows = block ? (r.MipHeight(mip) + 3) / 4 : r.MipHeight(mip);
    uint8_t* dst = r.Data() + r.SubOffset(sub);
    for (uint32_t z = 0; z < r.MipDepth(mip); z++)
      for (uint32_t y = 0; y < rows; y++)
        std::memcpy(dst + uint64_t(z) * r.SlicePitch(mip) + uint64_t(y) * r.RowPitch(mip),
                    GPtr(src + uint64_t(z) * slice_pitch + uint64_t(y) * pitch), r.RowPitch(mip));
  }
  return true;
}

uint64_t Publish(std::shared_ptr<Resource> r, gaddr out) {
  if (!out) return kS_FALSE;  // validation only
  Wr64(out, com::Publish(r, ResourceVtable(r->dim)));
  return kS_OK;
}

}  // namespace

#define D3D11_METHOD(iface, name) \
  HLE_NAMED_X(kDll, #iface "::" #name, iface##_##name, kCdecl, true)

// CreateBuffer(desc, initial data, &buffer): D3D11_BUFFER_DESC {ByteWidth,
// Usage, BindFlags, CPUAccessFlags, MiscFlags, StructureByteStride}.
D3D11_METHOD(ID3D11Device, CreateBuffer) {
  const gaddr d = Arg(c, 1);
  auto r = std::make_shared<Resource>();
  r->dim = Dim::Buffer;
  r->byte_width = Rd32(d);
  r->usage = Usage(Rd32(d + 4));
  r->bind = Rd32(d + 8);
  r->cpu_access = Rd32(d + 12);
  r->misc = Rd32(d + 16);
  r->stride = Rd32(d + 20);
  if (!r->byte_width) return kE_INVALIDARG;
  if (!Fill(*r, Arg(c, 2))) return kE_OUTOFMEMORY;
  return Publish(r, Arg(c, 3));
}

// D3D11_TEXTURE1D_DESC {Width, MipLevels, ArraySize, Format, Usage,
// BindFlags, CPUAccessFlags, MiscFlags}.
D3D11_METHOD(ID3D11Device, CreateTexture1D) {
  const gaddr d = Arg(c, 1);
  auto r = std::make_shared<Resource>();
  r->dim = Dim::Texture1D;
  r->width = Rd32(d);
  r->mips = Rd32(d + 4);
  r->array = Rd32(d + 8);
  r->format = Rd32(d + 12);
  r->usage = Usage(Rd32(d + 16));
  r->bind = Rd32(d + 20);
  r->cpu_access = Rd32(d + 24);
  r->misc = Rd32(d + 28);
  if (!r->mips) r->mips = MipCount(r->width, 1, 1);
  if (!Fill(*r, Arg(c, 2))) return kE_OUTOFMEMORY;
  return Publish(r, Arg(c, 3));
}

// D3D11_TEXTURE2D_DESC {Width, Height, MipLevels, ArraySize, Format,
// SampleDesc {Count, Quality}, Usage, BindFlags, CPUAccessFlags, MiscFlags}.
D3D11_METHOD(ID3D11Device, CreateTexture2D) {
  const gaddr d = Arg(c, 1);
  auto r = std::make_shared<Resource>();
  r->dim = Dim::Texture2D;
  r->width = Rd32(d);
  r->height = Rd32(d + 4);
  r->mips = Rd32(d + 8);
  r->array = Rd32(d + 12);
  r->format = Rd32(d + 16);
  r->samples = Rd32(d + 20);
  r->quality = Rd32(d + 24);
  r->usage = Usage(Rd32(d + 28));
  r->bind = Rd32(d + 32);
  r->cpu_access = Rd32(d + 36);
  r->misc = Rd32(d + 40);
  if (!r->mips) r->mips = MipCount(r->width, r->height, 1);
  if (!Fill(*r, Arg(c, 2))) return kE_OUTOFMEMORY;
  return Publish(r, Arg(c, 3));
}

std::shared_ptr<Resource> NewBackBuffer(uint32_t w, uint32_t h, uint32_t format, uint32_t bind) {
  auto r = std::make_shared<Resource>();
  r->dim = Dim::Texture2D;
  r->width = w;
  r->height = h;
  r->format = format;
  r->bind = bind;
  if (!Fill(*r, 0)) FatalF("d3d11: no memory for a %ux%u back buffer", w, h);
  com::Publish(r, ResourceVtable(r->dim));
  return r;
}

// D3D11_TEXTURE3D_DESC {Width, Height, Depth, MipLevels, Format, Usage,
// BindFlags, CPUAccessFlags, MiscFlags}.
D3D11_METHOD(ID3D11Device, CreateTexture3D) {
  const gaddr d = Arg(c, 1);
  auto r = std::make_shared<Resource>();
  r->dim = Dim::Texture3D;
  r->width = Rd32(d);
  r->height = Rd32(d + 4);
  r->depth = Rd32(d + 8);
  r->mips = Rd32(d + 12);
  r->format = Rd32(d + 16);
  r->usage = Usage(Rd32(d + 20));
  r->bind = Rd32(d + 24);
  r->cpu_access = Rd32(d + 28);
  r->misc = Rd32(d + 32);
  if (!r->mips) r->mips = MipCount(r->width, r->height, r->depth);
  if (!Fill(*r, Arg(c, 2))) return kE_OUTOFMEMORY;
  return Publish(r, Arg(c, 3));
}

// ---- views ----------------------------------------------------------------------------

namespace {

// A view of the whole resource, as a null description means.
void WholeResource(View& v) {
  const Resource& r = *v.res;
  v.format = r.format;
  v.first_mip = 0;
  v.mips = r.mips;
  v.first_slice = 0;
  v.slices = r.dim == Dim::Texture3D ? r.depth : r.array;
  if (r.dim == Dim::Buffer) {
    v.first_element = 0;
    v.elements = r.stride ? r.byte_width / r.stride : r.byte_width / 4;
  }
}

// Reads a view description: {Format, ViewDimension, [Flags (DSV)], union}.
// The union's fields by dimension: (first mip | mip slice), mip count,
// first array slice, array size; buffers {first element, count, flags}.
void ReadViewDesc(View& v, gaddr d) {
  v.format = Rd32(d);
  v.view_dim = Rd32(d + 4);
  gaddr u = d + (v.kind == ViewKind::DepthStencil ? 12 : 8);
  if (v.kind == ViewKind::DepthStencil) v.flags = Rd32(d + 8);
  const bool srv = v.kind == ViewKind::ShaderResource;
  const bool buffer = (srv && (v.view_dim == 1 || v.view_dim == 11)) ||
                      (!srv && v.kind != ViewKind::DepthStencil && v.view_dim == 1);
  if (buffer) {
    v.first_element = Rd32(u);
    v.elements = Rd32(u + 4);
    if (v.view_dim == 11 || v.kind == ViewKind::UnorderedAccess) v.flags = Rd32(u + 8);
    return;
  }
  if (srv) {  // {MostDetailedMip, MipLevels, [FirstArraySlice, ArraySize]}
    v.first_mip = Rd32(u);
    v.mips = Rd32(u + 4);
    if (v.mips == 0xFFFFFFFF) v.mips = v.res->mips - v.first_mip;
    const bool arrayed = v.view_dim == 3 || v.view_dim == 5 || v.view_dim == 10;
    v.first_slice = arrayed ? Rd32(u + 8) : 0;
    v.slices = arrayed ? Rd32(u + 12) : (v.view_dim == 9 ? 6 : 1);
    if (v.view_dim == 10) v.slices *= 6;  // cube arrays count cubes
    return;
  }
  // RTV, DSV, UAV: {MipSlice, [FirstArraySlice, ArraySize]}
  v.first_mip = Rd32(u);
  v.mips = 1;
  const bool arrayed = v.kind == ViewKind::DepthStencil
                           ? (v.view_dim == 2 || v.view_dim == 4)
                           : (v.view_dim == 3 || v.view_dim == 5 || v.view_dim == 8);
  v.first_slice = arrayed ? Rd32(u + 4) : 0;
  v.slices = arrayed ? Rd32(u + 8) : 1;
}

// The description bytes of a view (RTV and UAV 20, SRV and DSV 24).
uint32_t ViewDescSize(ViewKind k) {
  return k == ViewKind::ShaderResource || k == ViewKind::DepthStencil ? 24 : 20;
}

// The description of a view of the whole resource, as Windows fills it in
// for a null one: the dimension from the resource (arrays, cubes,
// multisampling), every mip for a shader resource view, mip 0 otherwise,
// and every slice.
void WholeDesc(View& v) {
  const Resource& r = *v.res;
  const bool srv = v.kind == ViewKind::ShaderResource, dsv = v.kind == ViewKind::DepthStencil;
  const bool ms = r.samples > 1, cube = srv && (r.misc & kMiscTextureCube);
  switch (r.dim) {
    case Dim::Buffer: v.view_dim = 1; break;
    case Dim::Texture1D: v.view_dim = dsv ? (r.array > 1 ? 2 : 1) : (r.array > 1 ? 3 : 2); break;
    case Dim::Texture3D: v.view_dim = 8; break;
    default:
      if (cube) v.view_dim = r.array > 6 ? 10 : 9;
      else if (dsv) v.view_dim = ms ? (r.array > 1 ? 6 : 5) : (r.array > 1 ? 4 : 3);
      else v.view_dim = ms ? (r.array > 1 ? 7 : 6) : (r.array > 1 ? 5 : 4);
  }
  v.desc.assign(ViewDescSize(v.kind), 0);
  auto put = [&](uint32_t at, uint32_t x) { std::memcpy(v.desc.data() + at, &x, 4); };
  put(0, v.format);
  put(4, v.view_dim);
  const uint32_t u = dsv ? 12 : 8;
  if (r.dim == Dim::Buffer) {
    put(u, 0);
    put(u + 4, v.elements);
  } else if (srv) {
    put(u, 0);
    put(u + 4, r.mips);
    if (cube && v.view_dim == 10) put(u + 12, r.array / 6);  // {.., First2DArrayFace, NumCubes}
    else if (v.view_dim == 3 || v.view_dim == 5 || v.view_dim == 7) put(u + 12, r.array);
  } else {
    const bool arrayed = v.view_dim == (dsv ? 2u : 3u) || v.view_dim == (dsv ? 4u : 5u) ||
                         v.view_dim == (dsv ? 6u : 7u) || v.view_dim == 8;
    if (arrayed) put(u + 8, v.view_dim == 8 ? r.depth : r.array);
  }
}

uint64_t CreateView(Ctx& c, ViewKind kind) {
  auto v = std::make_shared<View>();
  v->kind = kind;
  v->res = ResourceAt(Arg(c, 1));
  WholeResource(*v);
  if (gaddr d = Arg(c, 2)) {
    ReadViewDesc(*v, d);
    v->desc.assign(GPtr(d), GPtr(d) + ViewDescSize(kind));
    // A shader resource view's MipLevels -1 ("all from the first on")
    // comes back from GetDesc as the count it stands for.
    uint32_t mips;
    std::memcpy(&mips, v->desc.data() + 12, 4);
    const bool buffer = v->view_dim == 1 || v->view_dim == 11;
    if (kind == ViewKind::ShaderResource && !buffer && mips == 0xFFFFFFFF)
      std::memcpy(v->desc.data() + 12, &v->mips, 4);
  } else {
    WholeDesc(*v);
  }
  // A typeless resource is made on the GPU in its views' format.
  Resource& r = *v->res;
  if (kind == ViewKind::RenderTarget && !r.rtv_format) r.rtv_format = v->format;
  if (!r.view_format) r.view_format = v->format;
  if (kind == ViewKind::ShaderResource && !r.srv_dim) r.srv_dim = v->view_dim;
  if (gaddr out = Arg(c, 3)) {
    Wr64(out, com::Publish(v, VtableFor(ViewIface(kind), {"GetResource", "GetDesc"})));
    return kS_OK;
  }
  return kS_FALSE;
}

}  // namespace

D3D11_METHOD(ID3D11Device, CreateShaderResourceView) { return CreateView(c, ViewKind::ShaderResource); }
D3D11_METHOD(ID3D11Device, CreateRenderTargetView) { return CreateView(c, ViewKind::RenderTarget); }
D3D11_METHOD(ID3D11Device, CreateDepthStencilView) { return CreateView(c, ViewKind::DepthStencil); }
D3D11_METHOD(ID3D11Device, CreateUnorderedAccessView) { return CreateView(c, ViewKind::UnorderedAccess); }

// ---- shaders and input layouts ------------------------------------------------------------

namespace {

// (bytecode, length, class linkage, &shader): the DXBC kept as is; its
// shader version token says the stage.
uint64_t CreateShader(Ctx& c, uint32_t stage, const char* iface) {
  const gaddr code = Arg(c, 1);
  const uint64_t len = Arg(c, 2);
  if (Rd32(code) != 0x43425844) return kE_INVALIDARG;  // 'DXBC'
  auto s = std::make_shared<Shader>();
  s->stage = stage;
  s->dxbc.assign(GPtr(code), GPtr(code) + len);
  s->hash = Fnv64(s->dxbc.data(), s->dxbc.size());
  if (gaddr out = Arg(c, 4)) {
    Wr64(out, com::Publish(s, com::Vtable(kDll, iface, kChildMethods)));
    return kS_OK;
  }
  return kS_FALSE;
}

}  // namespace

D3D11_METHOD(ID3D11Device, CreateVertexShader) { return CreateShader(c, 0, "ID3D11VertexShader"); }
D3D11_METHOD(ID3D11Device, CreatePixelShader) { return CreateShader(c, 1, "ID3D11PixelShader"); }
D3D11_METHOD(ID3D11Device, CreateComputeShader) { return CreateShader(c, 2, "ID3D11ComputeShader"); }
// The tessellation and geometry stages: their shaders are made and kept;
// binding one for a draw is not supported yet (the context's setters).
D3D11_METHOD(ID3D11Device, CreateHullShader) { return CreateShader(c, 3, "ID3D11HullShader"); }
D3D11_METHOD(ID3D11Device, CreateDomainShader) { return CreateShader(c, 4, "ID3D11DomainShader"); }
D3D11_METHOD(ID3D11Device, CreateGeometryShader) { return CreateShader(c, 5, "ID3D11GeometryShader"); }

// CreateInputLayout(elements, count, VS bytecode, length, &layout):
// D3D11_INPUT_ELEMENT_DESC {SemanticName, SemanticIndex, Format, InputSlot,
// AlignedByteOffset, InputSlotClass, InstanceDataStepRate}, 32 bytes.
// D3D11_APPEND_ALIGNED_ELEMENT offsets follow the slot's previous element.
D3D11_METHOD(ID3D11Device, CreateInputLayout) {
  const gaddr descs = Arg(c, 1);
  const uint32_t n = uint32_t(Arg(c, 2));
  auto l = std::make_shared<InputLayout>();
  uint32_t next[32] = {};
  for (uint32_t i = 0; i < n; i++) {
    const gaddr d = descs + 32 * i;
    InputLayout::Element e;
    e.semantic = GStr(Rd64(d));
    e.index = Rd32(d + 8);
    e.format = Rd32(d + 12);
    e.slot = Rd32(d + 16);
    e.offset = Rd32(d + 20);
    e.per_instance = Rd32(d + 24);
    e.step_rate = Rd32(d + 28);
    if (e.slot >= 32) return kE_INVALIDARG;
    if (e.offset == 0xFFFFFFFF) e.offset = next[e.slot];
    bool block;
    next[e.slot] = e.offset + FormatBytes(e.format, &block);
    l->elements.push_back(e);
  }
  if (gaddr out = Arg(c, 5)) {
    Wr64(out, com::Publish(l, com::Vtable(kDll, "ID3D11InputLayout", kChildMethods)));
    return kS_OK;
  }
  return kS_FALSE;
}

// ---- states ---------------------------------------------------------------------------

namespace {

// (desc, &state): the description's bytes; a null description is the
// state's defaults, which the reader of the bytes supplies.
uint64_t CreateState(Ctx& c, StateKind kind, uint32_t size) {
  auto s = std::make_shared<State>();
  s->kind = kind;
  if (gaddr d = Arg(c, 1)) s->desc.assign(GPtr(d), GPtr(d) + size);
  if (gaddr out = Arg(c, 2)) {
    Wr64(out, com::Publish(s, VtableFor(StateIface(kind), {"GetDesc"})));
    return kS_OK;
  }
  return kS_FALSE;
}

}  // namespace

D3D11_METHOD(ID3D11Device, CreateBlendState) { return CreateState(c, StateKind::Blend, 264); }
D3D11_METHOD(ID3D11Device, CreateDepthStencilState) {
  return CreateState(c, StateKind::DepthStencil, 52);
}
D3D11_METHOD(ID3D11Device, CreateRasterizerState) { return CreateState(c, StateKind::Rasterizer, 40); }
D3D11_METHOD(ID3D11Device, CreateSamplerState) { return CreateState(c, StateKind::Sampler, 52); }

// CreateQuery(D3D11_QUERY_DESC {Query, MiscFlags}, &query).
// CreateQuery/CreatePredicate(D3D11_QUERY_DESC {Query, MiscFlags}, &out):
// a predicate is an occlusion query of its own interface.
uint64_t CreateQuery(Ctx& c, const char* iface) {
  auto q = std::make_shared<Query>();
  q->type = Rd32(Arg(c, 1));
  q->misc = Rd32(Arg(c, 1) + 4);
  if (gaddr out = Arg(c, 2)) {
    Wr64(out, com::Publish(q, VtableFor(iface, {"GetDataSize", "GetDesc"})));
    return kS_OK;
  }
  return kS_FALSE;
}
D3D11_METHOD(ID3D11Device, CreateQuery) { return CreateQuery(c, "ID3D11Query"); }
D3D11_METHOD(ID3D11Device, CreatePredicate) { return CreateQuery(c, "ID3D11Predicate"); }

// ---- capabilities ---------------------------------------------------------------------

// CheckFormatSupport(format, &support): every format this layer can store
// supports everything a feature level 11_0 device must.
D3D11_METHOD(ID3D11Device, CheckFormatSupport) {
  bool block;
  RECOMP_TRACE("  d3d11: CheckFormatSupport(%u)\n", uint32_t(Arg(c, 1)));
  if (!FormatBytes(uint32_t(Arg(c, 1)), &block)) {
    Wr32(Arg(c, 2), 0);
    return kE_FAIL;
  }
  Wr32(Arg(c, 2), 0x3FFFFFFF);
  return kS_OK;
}

// CheckMultisampleQualityLevels(format, count, &levels): one quality level
// for 1, 2, 4 and 8 samples.
D3D11_METHOD(ID3D11Device, CheckMultisampleQualityLevels) {
  const uint32_t n = uint32_t(Arg(c, 2));
  Wr32(Arg(c, 3), n == 1 || n == 2 || n == 4 || n == 8 ? 1 : 0);
  return kS_OK;
}

// CheckFeatureSupport(feature, data, size): threading (no concurrent
// creates or command lists: the runtime emulates them), no doubles, the
// hardware options of a feature level 11_0 GPU.
D3D11_METHOD(ID3D11Device, CheckFeatureSupport) {
  const uint32_t feature = uint32_t(Arg(c, 1)), size = uint32_t(Arg(c, 3));
  const gaddr data = Arg(c, 2);
  std::memset(GPtr(data), 0, size);
  switch (feature) {
    case 0:  // D3D11_FEATURE_THREADING {DriverConcurrentCreates, DriverCommandLists}
    case 1:  // D3D11_FEATURE_DOUBLES
    case 3:  // D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS
    case 4:  // D3D11_FEATURE_D3D11_OPTIONS
    case 5:  // D3D11_FEATURE_ARCHITECTURE_INFO
    case 6:  // D3D11_FEATURE_D3D9_OPTIONS
      return kS_OK;
    case 2:  // D3D11_FEATURE_FORMAT_SUPPORT {InFormat, OutFormatSupport}
      Wr32(data + 4, 0x3FFFFFFF);
      return kS_OK;
    default:
      WarnF("d3d11: CheckFeatureSupport(%u): unsupported\n", feature);
      return kE_INVALIDARG;
  }
}

// ---- what every object has: private data, its device ---------------------------------------

namespace {

std::string GuidKey(gaddr g) { return std::string(reinterpret_cast<const char*>(GPtr(g)), 16); }

Child* ChildAt(gaddr p) {
  Child* o = com::As<Child>(p);
  if (!o) FatalF("d3d11: %08llX is not a device child", (unsigned long long)p);
  return o;
}

// QueryInterface for a child: its own interface, ID3D11DeviceChild,
// IUnknown and, for resources, ID3D11Resource.
uint64_t ChildQuery(Ctx& c, const char* iface) {
  const gaddr iid = Arg(c, 1), out = Arg(c, 2);
  Child* o = ChildAt(Arg(c, 0));
  bool ok = com::GuidEq(iid, kIID_IUnknown) || com::GuidEq(iid, kIID_ID3D11DeviceChild);
  if (auto* r = dynamic_cast<Resource*>(o)) {
    ok = ok || com::GuidEq(iid, kIID_ID3D11Resource) ||
         (r->dim == Dim::Buffer && com::GuidEq(iid, kIID_ID3D11Buffer)) ||
         (r->dim == Dim::Texture1D && com::GuidEq(iid, kIID_ID3D11Texture1D)) ||
         (r->dim == Dim::Texture2D && com::GuidEq(iid, kIID_ID3D11Texture2D)) ||
         (r->dim == Dim::Texture3D && com::GuidEq(iid, kIID_ID3D11Texture3D));
  }
  if (ok) {
    com::AddRef(Arg(c, 0));
    Wr64(out, Arg(c, 0));
    return kS_OK;
  }
  guest::GUID g = com::ReadGuid(iid);
  WarnF("%s::QueryInterface({%08X-%04X-%04X-...}): not supported\n", iface, g.Data1, g.Data2,
        g.Data3);
  Wr64(out, 0);
  return com::kE_NOINTERFACE;
}

// GetPrivateData(guid, &size, data): the stored bytes, or the size when
// data is null.
uint64_t GetPrivate(Ctx& c) {
  Child* o = ChildAt(Arg(c, 0));
  auto it = o->private_data.find(GuidKey(Arg(c, 1)));
  if (it == o->private_data.end()) {
    Wr32(Arg(c, 2), 0);
    return kDXGI_ERROR_NOT_FOUND;
  }
  const uint32_t have = uint32_t(it->second.size());
  if (gaddr data = Arg(c, 3)) {
    if (Rd32(Arg(c, 2)) < have) {
      Wr32(Arg(c, 2), have);
      return 0x887A0003;  // DXGI_ERROR_MORE_DATA
    }
    std::memcpy(GPtr(data), it->second.data(), have);
  }
  Wr32(Arg(c, 2), have);
  return kS_OK;
}

// SetPrivateData(guid, size, data): a copy kept (a null data removes it).
uint64_t SetPrivate(Ctx& c) {
  Child* o = ChildAt(Arg(c, 0));
  const uint32_t size = uint32_t(Arg(c, 2));
  if (!Arg(c, 3)) {
    o->private_data.erase(GuidKey(Arg(c, 1)));
    return kS_OK;
  }
  o->private_data[GuidKey(Arg(c, 1))].assign(GPtr(Arg(c, 3)), GPtr(Arg(c, 3)) + size);
  return kS_OK;
}

uint64_t GetDevice(Ctx& c) {
  gaddr d = TheDevice()->guest;
  com::AddRef(d);
  Wr64(Arg(c, 1), d);
  return 0;
}

}  // namespace

// The methods every child interface has, for each child interface.
#define D3D11_CHILD(iface)                                                                    \
  D3D11_METHOD(iface, QueryInterface) { return ChildQuery(c, #iface); }                       \
  D3D11_METHOD(iface, AddRef) { return com::AddRef(Arg(c, 0)); }                              \
  D3D11_METHOD(iface, Release) { return com::Release(Arg(c, 0)); }                            \
  D3D11_METHOD(iface, GetDevice) { return GetDevice(c); }                                     \
  D3D11_METHOD(iface, GetPrivateData) { return GetPrivate(c); }                               \
  D3D11_METHOD(iface, SetPrivateData) { return SetPrivate(c); }                               \
  D3D11_METHOD(iface, SetPrivateDataInterface) {                                              \
    WarnF(#iface "::SetPrivateDataInterface: ignored\n");                                     \
    return kS_OK;                                                                             \
  }
D3D11_CHILD(ID3D11Buffer)
D3D11_CHILD(ID3D11Texture1D)
D3D11_CHILD(ID3D11Texture2D)
D3D11_CHILD(ID3D11Texture3D)
D3D11_CHILD(ID3D11ShaderResourceView)
D3D11_CHILD(ID3D11RenderTargetView)
D3D11_CHILD(ID3D11DepthStencilView)
D3D11_CHILD(ID3D11UnorderedAccessView)
D3D11_CHILD(ID3D11VertexShader)
D3D11_CHILD(ID3D11PixelShader)
D3D11_CHILD(ID3D11ComputeShader)
D3D11_CHILD(ID3D11HullShader)
D3D11_CHILD(ID3D11DomainShader)
D3D11_CHILD(ID3D11GeometryShader)
D3D11_CHILD(ID3D11InputLayout)
D3D11_CHILD(ID3D11BlendState)
D3D11_CHILD(ID3D11DepthStencilState)
D3D11_CHILD(ID3D11RasterizerState)
D3D11_CHILD(ID3D11SamplerState)
D3D11_CHILD(ID3D11Query)
D3D11_CHILD(ID3D11Predicate)
D3D11_CHILD(ID3D11CommandList)
#undef D3D11_CHILD

D3D11_METHOD(ID3D11CommandList, GetContextFlags) { return 0; }

// ---- resources: GetType, eviction, GetDesc ------------------------------------------------

#define D3D11_RESOURCE(iface)                                                                 \
  D3D11_METHOD(iface, GetType) {                                                              \
    Wr32(Arg(c, 1), uint32_t(ResourceAt(Arg(c, 0))->dim));                                    \
    return 0;                                                                                 \
  }                                                                                           \
  D3D11_METHOD(iface, SetEvictionPriority) { return 0; }                                      \
  D3D11_METHOD(iface, GetEvictionPriority) { return 0; }
D3D11_RESOURCE(ID3D11Buffer)
D3D11_RESOURCE(ID3D11Texture1D)
D3D11_RESOURCE(ID3D11Texture2D)
D3D11_RESOURCE(ID3D11Texture3D)
#undef D3D11_RESOURCE

D3D11_METHOD(ID3D11Buffer, GetDesc) {
  auto r = ResourceAt(Arg(c, 0));
  const gaddr d = Arg(c, 1);
  Wr32(d, r->byte_width);
  Wr32(d + 4, uint32_t(r->usage));
  Wr32(d + 8, r->bind);
  Wr32(d + 12, r->cpu_access);
  Wr32(d + 16, r->misc);
  Wr32(d + 20, r->stride);
  return 0;
}
// D3D11_TEXTURE1D_DESC {Width, MipLevels, ArraySize, Format, Usage,
// BindFlags, CPUAccessFlags, MiscFlags}.
D3D11_METHOD(ID3D11Texture1D, GetDesc) {
  auto r = ResourceAt(Arg(c, 0));
  const uint32_t f[8] = {r->width, r->mips, r->array, r->format, uint32_t(r->usage), r->bind,
                         r->cpu_access, r->misc};
  for (int i = 0; i < 8; i++) Wr32(Arg(c, 1) + 4 * i, f[i]);
  return 0;
}
// D3D11_TEXTURE3D_DESC {Width, Height, Depth, MipLevels, Format, Usage,
// BindFlags, CPUAccessFlags, MiscFlags}.
D3D11_METHOD(ID3D11Texture3D, GetDesc) {
  auto r = ResourceAt(Arg(c, 0));
  const uint32_t f[9] = {r->width, r->height, r->depth, r->mips, r->format, uint32_t(r->usage),
                         r->bind, r->cpu_access, r->misc};
  for (int i = 0; i < 9; i++) Wr32(Arg(c, 1) + 4 * i, f[i]);
  return 0;
}
D3D11_METHOD(ID3D11Texture2D, GetDesc) {
  auto r = ResourceAt(Arg(c, 0));
  const gaddr d = Arg(c, 1);
  const uint32_t f[11] = {r->width, r->height,         r->mips, r->array,      r->format, r->samples,
                          r->quality, uint32_t(r->usage), r->bind, r->cpu_access, r->misc};
  for (int i = 0; i < 11; i++) Wr32(d + 4 * i, f[i]);
  return 0;
}

// ---- views: GetResource -------------------------------------------------------------------

#define D3D11_VIEW(iface)                                                                     \
  D3D11_METHOD(iface, GetResource) {                                                          \
    View* v = com::As<View>(Arg(c, 0));                                                       \
    com::AddRef(v->res->guest);                                                               \
    Wr64(Arg(c, 1), v->res->guest);                                                           \
    return 0;                                                                                 \
  }
D3D11_VIEW(ID3D11ShaderResourceView)
D3D11_VIEW(ID3D11RenderTargetView)
D3D11_VIEW(ID3D11DepthStencilView)
D3D11_VIEW(ID3D11UnorderedAccessView)
#undef D3D11_VIEW

#define D3D11_VIEW_DESC(iface)                                                \
  D3D11_METHOD(iface, GetDesc) {                                              \
    const View* v = com::As<View>(Arg(c, 0));                                 \
    std::memcpy(GPtr(Arg(c, 1)), v->desc.data(), v->desc.size());            \
    return 0;                                                                 \
  }
D3D11_VIEW_DESC(ID3D11ShaderResourceView)
D3D11_VIEW_DESC(ID3D11RenderTargetView)
D3D11_VIEW_DESC(ID3D11DepthStencilView)
D3D11_VIEW_DESC(ID3D11UnorderedAccessView)
#undef D3D11_VIEW_DESC

// ---- states: GetDesc gives back the description they were made with ----------------------

#define D3D11_STATE(iface)                                                     \
  D3D11_METHOD(iface, GetDesc) {                                               \
    const State* st = com::As<State>(Arg(c, 0));                               \
    std::memcpy(GPtr(Arg(c, 1)), st->desc.data(), st->desc.size());            \
    return 0;                                                                  \
  }
D3D11_STATE(ID3D11BlendState)
D3D11_STATE(ID3D11DepthStencilState)
D3D11_STATE(ID3D11RasterizerState)
D3D11_STATE(ID3D11SamplerState)
#undef D3D11_STATE

// ID3D11Query::GetDataSize: what GetData writes for the query's type.
uint64_t QueryDataSize(Ctx& c) {
  switch (com::As<Query>(Arg(c, 0))->type) {
    case 0: return 4;    // EVENT: BOOL
    case 1: return 8;    // OCCLUSION: UINT64
    case 2: return 8;    // TIMESTAMP: UINT64
    case 3: return 16;   // TIMESTAMP_DISJOINT: {Frequency, Disjoint}
    case 4: return 88;   // PIPELINE_STATISTICS
    case 5: return 4;    // OCCLUSION_PREDICATE: BOOL
    default: return 0;
  }
}
uint64_t QueryDesc(Ctx& c) {
  const Query* q = com::As<Query>(Arg(c, 0));
  Wr32(Arg(c, 1), q->type);
  Wr32(Arg(c, 1) + 4, q->misc);
  return 0;
}
D3D11_METHOD(ID3D11Query, GetDataSize) { return QueryDataSize(c); }
D3D11_METHOD(ID3D11Query, GetDesc) { return QueryDesc(c); }
D3D11_METHOD(ID3D11Predicate, GetDataSize) { return QueryDataSize(c); }
D3D11_METHOD(ID3D11Predicate, GetDesc) { return QueryDesc(c); }

// SetPredication(predicate, value): occlusion queries here say everything
// was visible, so drawing never depends on one; the predicate is accepted.
D3D11_METHOD(ID3D11DeviceContext, SetPredication) { return 0; }

}  // namespace recomp::hle::d3d11
