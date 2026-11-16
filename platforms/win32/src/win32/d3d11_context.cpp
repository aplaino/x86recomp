// Direct3D 11's immediate context: the pipeline state the guest sets
// (d3d11_impl.h, Context), resource access (Map, UpdateSubresource,
// copies), clears, draws and queries.

#include <algorithm>
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

Context& Ctx0(Ctx& c) {
  auto* x = com::As<Context>(Arg(c, 0));
  if (!x) FatalF("d3d11: %08llX is not a device context", (unsigned long long)Arg(c, 0));
  return *x;
}

// The host object behind a guest pointer of type T (null for 0).
template <typename T>
std::shared_ptr<T> Obj(gaddr p) {
  if (!p) return nullptr;
  auto o = std::dynamic_pointer_cast<T>(com::Lookup(p));
  if (!o) FatalF("d3d11: %08llX is not the object expected", (unsigned long long)p);
  return o;
}

// The guest pointer of a host object (0 for null), with a reference added
// as the Get* methods give.
template <typename T>
gaddr Out(const std::shared_ptr<T>& o) {
  if (!o) return 0;
  com::AddRef(o->guest);
  return o->guest;
}

}  // namespace

#define D3D11_METHOD(iface, name) \
  HLE_NAMED_X(kDll, #iface "::" #name, iface##_##name, kCdecl, true)
#define CTX_METHOD(name) D3D11_METHOD(ID3D11DeviceContext, name)

CTX_METHOD(QueryInterface) {
  com::AddRef(Arg(c, 0));
  Wr64(Arg(c, 2), Arg(c, 0));
  return kS_OK;
}

// ---- shaders, constant buffers, resources, samplers by stage ----------------------------

namespace {

// (start, count, objects): the stage's slots from start.
template <typename T, size_t N>
void SetSlots(std::shared_ptr<T> (&slots)[N], Ctx& c, int at) {
  const uint32_t start = uint32_t(Arg(c, at)), n = uint32_t(Arg(c, at + 1));
  const gaddr arr = Arg(c, at + 2);
  for (uint32_t i = 0; i < n && start + i < N; i++)
    slots[start + i] = arr ? Obj<T>(Rd64(arr + 8 * i)) : nullptr;
}
template <typename T, size_t N>
void GetSlots(std::shared_ptr<T> (&slots)[N], Ctx& c) {
  const uint32_t start = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 2));
  const gaddr arr = Arg(c, 3);
  for (uint32_t i = 0; i < n; i++) Wr64(arr + 8 * i, start + i < N ? Out(slots[start + i]) : 0);
}

void SetConstantBuffers(Context& x, int st, Ctx& c) {
  const uint32_t start = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 2));
  const gaddr arr = Arg(c, 3);
  for (uint32_t i = 0; i < n && start + i < 14; i++)
    x.stage[st].cbs[start + i] = {arr ? Obj<Resource>(Rd64(arr + 8 * i)) : nullptr, 0, 0};
}

// (shader, class instances, count)
void SetShader(Context& x, int st, Ctx& c) {
  if (Arg(c, 3)) FatalF("d3d11: class instances (dynamic linkage) are not supported");
  x.stage[st].shader = Obj<Shader>(Arg(c, 1));
}

}  // namespace

#define CTX_STAGE(P, st)                                                                    \
  CTX_METHOD(P##SetShader) {                                                                \
    SetShader(Ctx0(c), st, c);                                                              \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##SetConstantBuffers) {                                                       \
    SetConstantBuffers(Ctx0(c), st, c);                                                     \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##SetShaderResources) {                                                       \
    SetSlots(Ctx0(c).stage[st].srvs, c, 1);                                                 \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##SetSamplers) {                                                              \
    SetSlots(Ctx0(c).stage[st].samplers, c, 1);                                             \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##GetShader) {                                                                \
    Wr64(Arg(c, 1), Out(Ctx0(c).stage[st].shader));                                        \
    if (Arg(c, 3)) Wr32(Arg(c, 3), 0);                                                      \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##GetShaderResources) {                                                       \
    GetSlots(Ctx0(c).stage[st].srvs, c);                                                    \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##GetSamplers) {                                                              \
    GetSlots(Ctx0(c).stage[st].samplers, c);                                                \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##GetConstantBuffers) {                                                       \
    Context& x = Ctx0(c);                                                                   \
    const uint32_t start = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 2));                    \
    for (uint32_t i = 0; i < n; i++)                                                        \
      Wr64(Arg(c, 3) + 8 * i, start + i < 14 ? Out(x.stage[st].cbs[start + i].res) : 0);    \
    return 0;                                                                               \
  }
CTX_STAGE(VS, 0)
CTX_STAGE(PS, 1)
CTX_STAGE(CS, 2)
#undef CTX_STAGE

// The geometry, hull and domain stages are never used: only unbinding
// them is accepted.
#define CTX_UNUSED_STAGE(P)                                                                 \
  CTX_METHOD(P##SetShader) {                                                                \
    if (Arg(c, 1)) FatalF("d3d11: " #P " shaders are not supported");                       \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##SetConstantBuffers) { return 0; }                                           \
  CTX_METHOD(P##SetShaderResources) { return 0; }                                           \
  CTX_METHOD(P##SetSamplers) { return 0; }
CTX_UNUSED_STAGE(GS)
CTX_UNUSED_STAGE(HS)
CTX_UNUSED_STAGE(DS)
#undef CTX_UNUSED_STAGE

// CSSetUnorderedAccessViews(start, count, views, initial counts)
CTX_METHOD(CSSetUnorderedAccessViews) {
  SetSlots(Ctx0(c).uavs, c, 1);
  return 0;
}

// ---- input assembler --------------------------------------------------------------------

CTX_METHOD(IASetInputLayout) {
  Ctx0(c).layout = Obj<InputLayout>(Arg(c, 1));
  return 0;
}
CTX_METHOD(IAGetInputLayout) {
  Wr64(Arg(c, 1), Out(Ctx0(c).layout));
  return 0;
}
CTX_METHOD(IASetPrimitiveTopology) {
  Ctx0(c).topology = uint32_t(Arg(c, 1));
  return 0;
}
CTX_METHOD(IAGetPrimitiveTopology) {
  Wr32(Arg(c, 1), Ctx0(c).topology);
  return 0;
}
// (start, count, buffers, strides, offsets)
CTX_METHOD(IASetVertexBuffers) {
  Context& x = Ctx0(c);
  const uint32_t start = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 2));
  const gaddr bufs = Arg(c, 3), strides = Arg(c, 4), offsets = Arg(c, 5);
  for (uint32_t i = 0; i < n && start + i < 32; i++)
    x.vbs[start + i] = {Obj<Resource>(Rd64(bufs + 8 * i)), Rd32(strides + 4 * i),
                        Rd32(offsets + 4 * i)};
  return 0;
}
// (buffer, format, offset)
CTX_METHOD(IASetIndexBuffer) {
  Context& x = Ctx0(c);
  x.ib = Obj<Resource>(Arg(c, 1));
  x.ib_format = uint32_t(Arg(c, 2));
  x.ib_offset = uint32_t(Arg(c, 3));
  return 0;
}

// ---- rasterizer and output merger ---------------------------------------------------------

CTX_METHOD(RSSetState) {
  Ctx0(c).rasterizer = Obj<State>(Arg(c, 1));
  return 0;
}
CTX_METHOD(RSGetState) {
  Wr64(Arg(c, 1), Out(Ctx0(c).rasterizer));
  return 0;
}
// (count, D3D11_VIEWPORT {TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth}*)
CTX_METHOD(RSSetViewports) {
  Context& x = Ctx0(c);
  x.num_viewports = std::min<uint32_t>(uint32_t(Arg(c, 1)), 16);
  for (uint32_t i = 0; i < x.num_viewports; i++)
    std::memcpy(&x.viewports[i], GPtr(Arg(c, 2) + 24 * i), 24);
  return 0;
}
// (&count, viewports): the count when viewports is null.
CTX_METHOD(RSGetViewports) {
  Context& x = Ctx0(c);
  if (!Arg(c, 2)) {
    Wr32(Arg(c, 1), x.num_viewports);
    return 0;
  }
  const uint32_t n = std::min(Rd32(Arg(c, 1)), x.num_viewports);
  for (uint32_t i = 0; i < n; i++) std::memcpy(GPtr(Arg(c, 2) + 24 * i), &x.viewports[i], 24);
  Wr32(Arg(c, 1), n);
  return 0;
}
// (count, D3D11_RECT {left, top, right, bottom}*)
CTX_METHOD(RSSetScissorRects) {
  Context& x = Ctx0(c);
  x.num_scissors = std::min<uint32_t>(uint32_t(Arg(c, 1)), 16);
  for (uint32_t i = 0; i < x.num_scissors; i++)
    std::memcpy(x.scissors[i], GPtr(Arg(c, 2) + 16 * i), 16);
  return 0;
}
// (count, render target views, depth stencil view)
CTX_METHOD(OMSetRenderTargets) {
  Context& x = Ctx0(c);
  const uint32_t n = uint32_t(Arg(c, 1));
  for (uint32_t i = 0; i < 8; i++)
    x.rtvs[i] = i < n && Arg(c, 2) ? Obj<View>(Rd64(Arg(c, 2) + 8 * i)) : nullptr;
  x.dsv = Obj<View>(Arg(c, 3));
  return 0;
}
CTX_METHOD(OMGetRenderTargets) {
  Context& x = Ctx0(c);
  const uint32_t n = uint32_t(Arg(c, 1));
  for (uint32_t i = 0; i < n && Arg(c, 2); i++) Wr64(Arg(c, 2) + 8 * i, i < 8 ? Out(x.rtvs[i]) : 0);
  if (Arg(c, 3)) Wr64(Arg(c, 3), Out(x.dsv));
  return 0;
}
// (state, blend factor[4], sample mask)
CTX_METHOD(OMSetBlendState) {
  Context& x = Ctx0(c);
  x.blend = Obj<State>(Arg(c, 1));
  if (Arg(c, 2)) std::memcpy(x.blend_factor, GPtr(Arg(c, 2)), 16);
  else std::fill(x.blend_factor, x.blend_factor + 4, 1.0f);
  x.sample_mask = uint32_t(Arg(c, 3));
  return 0;
}
CTX_METHOD(OMGetBlendState) {
  Context& x = Ctx0(c);
  if (Arg(c, 1)) Wr64(Arg(c, 1), Out(x.blend));
  if (Arg(c, 2)) std::memcpy(GPtr(Arg(c, 2)), x.blend_factor, 16);
  if (Arg(c, 3)) Wr32(Arg(c, 3), x.sample_mask);
  return 0;
}
// (state, stencil ref)
CTX_METHOD(OMSetDepthStencilState) {
  Context& x = Ctx0(c);
  x.depth_stencil = Obj<State>(Arg(c, 1));
  x.stencil_ref = uint32_t(Arg(c, 2));
  return 0;
}
CTX_METHOD(OMGetDepthStencilState) {
  Context& x = Ctx0(c);
  if (Arg(c, 1)) Wr64(Arg(c, 1), Out(x.depth_stencil));
  if (Arg(c, 2)) Wr32(Arg(c, 2), x.stencil_ref);
  return 0;
}

// ---- resource access ------------------------------------------------------------------------

// Map(resource, subresource, type, flags, D3D11_MAPPED_SUBRESOURCE {pData,
// RowPitch, DepthPitch}): the subresource's bytes in guest memory, where the
// resource keeps them; nothing is copied until Unmap says they changed.
CTX_METHOD(Map) {
  auto r = Obj<Resource>(Arg(c, 1));
  const uint32_t sub = uint32_t(Arg(c, 2));
  const gaddr out = Arg(c, 5);
  const uint32_t mip = r->dim == Dim::Buffer ? 0 : sub % r->mips;
  Wr64(out, r->mem + r->SubOffset(sub));
  Wr32(out + 8, r->dim == Dim::Buffer ? r->byte_width : r->RowPitch(mip));
  Wr32(out + 12, r->dim == Dim::Buffer ? r->byte_width : r->SlicePitch(mip));
  return kS_OK;
}
CTX_METHOD(Unmap) {
  Obj<Resource>(Arg(c, 1))->version++;
  return 0;
}

// UpdateSubresource(resource, subresource, D3D11_BOX* {left, top, front,
// right, bottom, back}, data, row pitch, depth pitch)
CTX_METHOD(UpdateSubresource) {
  auto r = Obj<Resource>(Arg(c, 1));
  const uint32_t sub = uint32_t(Arg(c, 2));
  const gaddr box = Arg(c, 3), src = Arg(c, 4);
  const uint32_t pitch = uint32_t(Arg(c, 5)), slice_pitch = uint32_t(Arg(c, 6));
  uint8_t* dst = r->Data() + r->SubOffset(sub);
  if (r->dim == Dim::Buffer) {
    const uint32_t lo = box ? Rd32(box) : 0, hi = box ? Rd32(box + 12) : r->byte_width;
    std::memcpy(dst + lo, GPtr(src), hi - lo);
    r->version++;
    return 0;
  }
  const uint32_t mip = sub % r->mips;
  bool block;
  const uint32_t bpp = FormatBytes(r->format, &block);
  uint32_t x0 = 0, y0 = 0, z0 = 0, x1 = r->MipWidth(mip), y1 = r->MipHeight(mip),
           z1 = r->MipDepth(mip);
  if (box) {
    x0 = Rd32(box), y0 = Rd32(box + 4), z0 = Rd32(box + 8);
    x1 = Rd32(box + 12), y1 = Rd32(box + 16), z1 = Rd32(box + 20);
  }
  const uint32_t div = block ? 4 : 1;
  const uint32_t rows = (y1 - y0 + div - 1) / div, row_bytes = (x1 - x0 + div - 1) / div * bpp;
  for (uint32_t z = z0; z < z1; z++)
    for (uint32_t y = 0; y < rows; y++)
      std::memcpy(dst + uint64_t(z) * r->SlicePitch(mip) + uint64_t(y0 / div + y) * r->RowPitch(mip) +
                      x0 / div * bpp,
                  GPtr(src + uint64_t(z - z0) * slice_pitch + uint64_t(y) * pitch), row_bytes);
  r->version++;
  return 0;
}

// CopyResource(dst, src): every subresource, of the same size.
CTX_METHOD(CopyResource) {
  auto d = Obj<Resource>(Arg(c, 1)), s = Obj<Resource>(Arg(c, 2));
  std::memcpy(d->Data(), s->Data(), std::min(d->size, s->size));
  d->version++;
  return 0;
}

// CopySubresourceRegion(dst, dst sub, x, y, z, src, src sub, box)
CTX_METHOD(CopySubresourceRegion) {
  auto d = Obj<Resource>(Arg(c, 1)), s = Obj<Resource>(Arg(c, 6));
  const uint32_t dsub = uint32_t(Arg(c, 2)), ssub = uint32_t(Arg(c, 7));
  const uint32_t dx = uint32_t(Arg(c, 3)), dy = uint32_t(Arg(c, 4)), dz = uint32_t(Arg(c, 5));
  const gaddr box = Arg(c, 8);
  if (d->dim == Dim::Buffer) {
    const uint32_t lo = box ? Rd32(box) : 0, hi = box ? Rd32(box + 12) : s->byte_width;
    std::memcpy(d->Data() + dx, s->Data() + lo, hi - lo);
    d->version++;
    return 0;
  }
  const uint32_t smip = ssub % s->mips, dmip = dsub % d->mips;
  bool block;
  const uint32_t bpp = FormatBytes(s->format, &block), div = block ? 4 : 1;
  uint32_t x0 = 0, y0 = 0, z0 = 0, x1 = s->MipWidth(smip), y1 = s->MipHeight(smip),
           z1 = s->MipDepth(smip);
  if (box) {
    x0 = Rd32(box), y0 = Rd32(box + 4), z0 = Rd32(box + 8);
    x1 = Rd32(box + 12), y1 = Rd32(box + 16), z1 = Rd32(box + 20);
  }
  const uint32_t rows = (y1 - y0 + div - 1) / div, row_bytes = (x1 - x0 + div - 1) / div * bpp;
  uint8_t* dst = d->Data() + d->SubOffset(dsub);
  const uint8_t* src = s->Data() + s->SubOffset(ssub);
  for (uint32_t z = 0; z < z1 - z0; z++)
    for (uint32_t y = 0; y < rows; y++)
      std::memcpy(dst + uint64_t(dz + z) * d->SlicePitch(dmip) +
                      uint64_t(dy / div + y) * d->RowPitch(dmip) + dx / div * bpp,
                  src + uint64_t(z0 + z) * s->SlicePitch(smip) +
                      uint64_t(y0 / div + y) * s->RowPitch(smip) + x0 / div * bpp,
                  row_bytes);
  d->version++;
  return 0;
}

// ---- clears and draws: recorded for the GPU (not yet drawn) ----------------------------------

CTX_METHOD(ClearRenderTargetView) { return 0; }
CTX_METHOD(ClearDepthStencilView) { return 0; }
CTX_METHOD(ClearUnorderedAccessViewUint) { return 0; }
CTX_METHOD(ClearUnorderedAccessViewFloat) { return 0; }
CTX_METHOD(Draw) { return 0; }
CTX_METHOD(DrawIndexed) { return 0; }
CTX_METHOD(DrawInstanced) { return 0; }
CTX_METHOD(DrawIndexedInstanced) { return 0; }
CTX_METHOD(Dispatch) { return 0; }
CTX_METHOD(GenerateMips) { return 0; }

// ---- queries --------------------------------------------------------------------------------

CTX_METHOD(Begin) { return 0; }
CTX_METHOD(End) {
  Obj<Query>(Arg(c, 1))->ended = true;
  return 0;
}
// GetData(query, data, size, flags): every query is done once ended; an
// occlusion query says everything was visible, an event TRUE.
CTX_METHOD(GetData) {
  auto q = Obj<Query>(Arg(c, 1));
  const gaddr data = Arg(c, 2);
  const uint32_t size = uint32_t(Arg(c, 3));
  if (!q->ended) return kS_FALSE;
  if (data && size) {
    std::memset(GPtr(data), 0, size);
    switch (q->type) {
      case 0: case 5: Wr32(data, 1); break;              // EVENT, OCCLUSION_PREDICATE
      case 1: Wr64(data, 0xFFFFFFFF); break;              // OCCLUSION
      case 3: Wr64(data, 1000000000); break;              // TIMESTAMP_DISJOINT: 1 GHz
      default: break;
    }
  }
  return kS_OK;
}

// ---- context ---------------------------------------------------------------------------------

// Every binding and state back to its default.
CTX_METHOD(ClearState) {
  Context& x = Ctx0(c);
  for (auto& st : x.stage) st = Context::StageState{};
  for (auto& u : x.uavs) u = nullptr;
  x.layout = nullptr;
  x.topology = 0;
  for (auto& vb : x.vbs) vb = {};
  x.ib = nullptr;
  x.ib_format = x.ib_offset = 0;
  for (auto& r : x.rtvs) r = nullptr;
  x.dsv = nullptr;
  x.blend = x.depth_stencil = x.rasterizer = nullptr;
  std::fill(x.blend_factor, x.blend_factor + 4, 1.0f);
  x.sample_mask = 0xFFFFFFFF;
  x.stencil_ref = 0;
  x.num_viewports = x.num_scissors = 0;
  return 0;
}
CTX_METHOD(Flush) { return 0; }
CTX_METHOD(GetType) { return 0; }  // D3D11_DEVICE_CONTEXT_IMMEDIATE
CTX_METHOD(GetContextFlags) { return 0; }

}  // namespace recomp::hle::d3d11
