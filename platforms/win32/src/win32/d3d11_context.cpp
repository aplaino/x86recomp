// Direct3D 11's device contexts: the pipeline state the guest sets
// (d3d11_impl.h, ContextState), resource access (Map, UpdateSubresource,
// copies), clears, draws and queries, and command lists.
//
// The immediate context runs its work on the GPU side (d3d11_gpu.cpp) as
// it comes. A deferred context records it instead (Context::ops): state
// changes apply to the deferred context at once too (its Get* methods
// read them), the rest only when ExecuteCommandList replays the list on
// the immediate context. Windows' runtime does the same for drivers
// without command lists, which is what CheckFeatureSupport reports.

#include <algorithm>
#include <array>
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
constexpr uint32_t kMapWriteDiscard = 4, kMapWriteNoOverwrite = 5;

Context& Ctx0(Ctx& c) {
  auto* x = com::As<Context>(Arg(c, 0));
  if (!x) FatalF("d3d11: %08llX is not a device context", (unsigned long long)Arg(c, 0));
  return *x;
}

Device& Dev() { return *TheDevice(); }

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

// A state change: applied to the context now and, on a deferred one,
// recorded for the replay.
template <typename F>
void Set(Context& x, F f) {
  f(x);
  if (x.deferred) x.ops.emplace_back(std::move(f));
}

// Work for the GPU: run on the immediate context, recorded on a deferred
// one.
template <typename F>
void Run(Context& x, F f) {
  if (x.deferred) x.ops.emplace_back(std::move(f));
  else f(x);
}

ContextState& StateOf(Context& x) { return x; }

// A D3D11_BOX {left, top, front, right, bottom, back} from the guest,
// clamped to a subresource's extent (a buffer's bytes); false if it is
// empty or inverted, for which Direct3D does nothing.
bool ReadBox(const Resource& r, uint32_t sub, gaddr p, std::array<uint32_t, 6>* box) {
  for (int i = 0; i < 6; i++) (*box)[i] = Rd32(p + 4 * i);
  uint32_t ext[3] = {r.byte_width, 1, 1};
  if (r.dim != Dim::Buffer) {
    const uint32_t mip = sub % r.mips;
    ext[0] = r.MipWidth(mip);
    ext[1] = r.dim == Dim::Texture1D ? 1 : r.MipHeight(mip);
    ext[2] = r.MipDepth(mip);
  }
  for (int i = 0; i < 3; i++) {
    (*box)[3 + i] = std::min((*box)[3 + i], ext[i]);
    if ((*box)[i] >= (*box)[3 + i]) return false;
  }
  return true;
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

// (start, count, objects): slots from start, of the array `pick` names.
template <typename T, size_t N, typename Pick>
void SetSlots(Context& x, Ctx& c, Pick pick) {
  const uint32_t start = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 2));
  const gaddr arr = Arg(c, 3);
  std::vector<std::shared_ptr<T>> v;
  for (uint32_t i = 0; i < n && start + i < N; i++)
    v.push_back(arr ? Obj<T>(Rd64(arr + 8 * i)) : nullptr);
  Set(x, [start, v, pick](Context& y) {
    auto& slots = pick(y);
    for (size_t i = 0; i < v.size(); i++) slots[start + i] = v[i];
  });
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
  std::vector<std::shared_ptr<Resource>> v;
  for (uint32_t i = 0; i < n && start + i < 14; i++)
    v.push_back(arr ? Obj<Resource>(Rd64(arr + 8 * i)) : nullptr);
  Set(x, [st, start, v](Context& y) {
    for (size_t i = 0; i < v.size(); i++) y.stage[st].cbs[start + i] = {v[i], 0, 0};
  });
}

// (shader, class instances, count)
void SetShader(Context& x, int st, Ctx& c) {
  if (Arg(c, 3)) FatalF("d3d11: class instances (dynamic linkage) are not supported");
  auto s = Obj<Shader>(Arg(c, 1));
  Set(x, [st, s](Context& y) { y.stage[st].shader = s; });
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
    SetSlots<View, 128>(Ctx0(c), c, [](Context& y) -> auto& { return y.stage[st].srvs; }); \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##SetSamplers) {                                                              \
    SetSlots<State, 16>(Ctx0(c), c, [](Context& y) -> auto& { return y.stage[st].samplers; }); \
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

// The geometry, hull and domain stages are not drawn. Geometry shaders
// can only be unbound; the tessellation stages can be bound, as games
// keep them bound for their tessellated draws: those draw patch lists,
// which are skipped (TopologyOf, d3d11_gpu.cpp).
#define CTX_UNUSED_STAGE(P, bindable)                                                       \
  CTX_METHOD(P##SetShader) {                                                                \
    if (Arg(c, 1) && !(bindable)) FatalF("d3d11: " #P " shaders are not supported");        \
    return 0;                                                                               \
  }                                                                                         \
  CTX_METHOD(P##SetConstantBuffers) { return 0; }                                           \
  CTX_METHOD(P##SetShaderResources) { return 0; }                                           \
  CTX_METHOD(P##SetSamplers) { return 0; }
CTX_UNUSED_STAGE(GS, false)
CTX_UNUSED_STAGE(HS, true)
CTX_UNUSED_STAGE(DS, true)
#undef CTX_UNUSED_STAGE

// Stream output, likewise: SOSetTargets(count, buffers, offsets) only
// unbinding, and SOGetTargets finds nothing bound.
CTX_METHOD(SOSetTargets) {
  const uint32_t n = uint32_t(Arg(c, 1));
  for (uint32_t i = 0; i < n && Arg(c, 2); i++)
    if (Rd64(Arg(c, 2) + 8 * i)) FatalF("d3d11: stream output is not supported");
  return 0;
}
CTX_METHOD(SOGetTargets) {
  for (uint32_t i = 0; i < uint32_t(Arg(c, 1)); i++) Wr64(Arg(c, 2) + 8 * i, 0);
  return 0;
}

// CSSetUnorderedAccessViews(start, count, views, initial counts)
CTX_METHOD(CSSetUnorderedAccessViews) {
  SetSlots<View, 8>(Ctx0(c), c, [](Context& y) -> auto& { return y.uavs; });
  return 0;
}
CTX_METHOD(CSGetUnorderedAccessViews) {
  GetSlots(Ctx0(c).uavs, c);
  return 0;
}

// ---- input assembler --------------------------------------------------------------------

CTX_METHOD(IASetInputLayout) {
  auto l = Obj<InputLayout>(Arg(c, 1));
  Set(Ctx0(c), [l](Context& y) { y.layout = l; });
  return 0;
}
CTX_METHOD(IAGetInputLayout) {
  Wr64(Arg(c, 1), Out(Ctx0(c).layout));
  return 0;
}
CTX_METHOD(IASetPrimitiveTopology) {
  const uint32_t t = uint32_t(Arg(c, 1));
  Set(Ctx0(c), [t](Context& y) { y.topology = t; });
  return 0;
}
CTX_METHOD(IAGetPrimitiveTopology) {
  Wr32(Arg(c, 1), Ctx0(c).topology);
  return 0;
}
// (start, count, buffers, strides, offsets)
CTX_METHOD(IASetVertexBuffers) {
  const uint32_t start = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 2));
  const gaddr bufs = Arg(c, 3), strides = Arg(c, 4), offsets = Arg(c, 5);
  std::vector<ContextState::VertexBuffer> v;
  for (uint32_t i = 0; i < n && start + i < 32; i++)
    v.push_back({bufs ? Obj<Resource>(Rd64(bufs + 8 * i)) : nullptr, strides ? Rd32(strides + 4 * i) : 0,
                 offsets ? Rd32(offsets + 4 * i) : 0});
  Set(Ctx0(c), [start, v](Context& y) {
    for (size_t i = 0; i < v.size(); i++) y.vbs[start + i] = v[i];
  });
  return 0;
}
// (start, count, &buffers, &strides, &offsets)
CTX_METHOD(IAGetVertexBuffers) {
  Context& x = Ctx0(c);
  const uint32_t start = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 2));
  for (uint32_t i = 0; i < n; i++) {
    const ContextState::VertexBuffer vb = start + i < 32 ? x.vbs[start + i] : ContextState::VertexBuffer{};
    if (Arg(c, 3)) Wr64(Arg(c, 3) + 8 * i, Out(vb.res));
    if (Arg(c, 4)) Wr32(Arg(c, 4) + 4 * i, vb.stride);
    if (Arg(c, 5)) Wr32(Arg(c, 5) + 4 * i, vb.offset);
  }
  return 0;
}
// (buffer, format, offset)
CTX_METHOD(IASetIndexBuffer) {
  auto b = Obj<Resource>(Arg(c, 1));
  const uint32_t format = uint32_t(Arg(c, 2)), offset = uint32_t(Arg(c, 3));
  Set(Ctx0(c), [b, format, offset](Context& y) {
    y.ib = b;
    y.ib_format = format;
    y.ib_offset = offset;
  });
  return 0;
}
CTX_METHOD(IAGetIndexBuffer) {
  Context& x = Ctx0(c);
  if (Arg(c, 1)) Wr64(Arg(c, 1), Out(x.ib));
  if (Arg(c, 2)) Wr32(Arg(c, 2), x.ib_format);
  if (Arg(c, 3)) Wr32(Arg(c, 3), x.ib_offset);
  return 0;
}

// ---- rasterizer and output merger ---------------------------------------------------------

CTX_METHOD(RSSetState) {
  auto s = Obj<State>(Arg(c, 1));
  Set(Ctx0(c), [s](Context& y) { y.rasterizer = s; });
  return 0;
}
CTX_METHOD(RSGetState) {
  Wr64(Arg(c, 1), Out(Ctx0(c).rasterizer));
  return 0;
}
// (count, D3D11_VIEWPORT {TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth}*)
CTX_METHOD(RSSetViewports) {
  const uint32_t n = std::min<uint32_t>(uint32_t(Arg(c, 1)), 16);
  std::vector<ContextState::Viewport> v(n);
  for (uint32_t i = 0; i < n; i++) std::memcpy(&v[i], GPtr(Arg(c, 2) + 24 * i), 24);
  Set(Ctx0(c), [v](Context& y) {
    y.num_viewports = uint32_t(v.size());
    std::copy(v.begin(), v.end(), y.viewports);
  });
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
  const uint32_t n = std::min<uint32_t>(uint32_t(Arg(c, 1)), 16);
  std::vector<std::array<int32_t, 4>> v(n);
  for (uint32_t i = 0; i < n; i++) std::memcpy(v[i].data(), GPtr(Arg(c, 2) + 16 * i), 16);
  Set(Ctx0(c), [v](Context& y) {
    y.num_scissors = uint32_t(v.size());
    for (size_t i = 0; i < v.size(); i++) std::copy(v[i].begin(), v[i].end(), y.scissors[i]);
  });
  return 0;
}
CTX_METHOD(RSGetScissorRects) {
  Context& x = Ctx0(c);
  if (!Arg(c, 2)) {
    Wr32(Arg(c, 1), x.num_scissors);
    return 0;
  }
  const uint32_t n = std::min(Rd32(Arg(c, 1)), x.num_scissors);
  for (uint32_t i = 0; i < n; i++) std::memcpy(GPtr(Arg(c, 2) + 16 * i), x.scissors[i], 16);
  Wr32(Arg(c, 1), n);
  return 0;
}
// (count, render target views, depth stencil view)
CTX_METHOD(OMSetRenderTargets) {
  const uint32_t n = uint32_t(Arg(c, 1));
  std::array<std::shared_ptr<View>, 8> rtvs;
  for (uint32_t i = 0; i < 8; i++)
    rtvs[i] = i < n && Arg(c, 2) ? Obj<View>(Rd64(Arg(c, 2) + 8 * i)) : nullptr;
  auto dsv = Obj<View>(Arg(c, 3));
  Set(Ctx0(c), [rtvs, dsv](Context& y) {
    std::copy(rtvs.begin(), rtvs.end(), y.rtvs);
    y.dsv = dsv;
  });
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
  auto s = Obj<State>(Arg(c, 1));
  std::array<float, 4> f = {1, 1, 1, 1};
  if (Arg(c, 2)) std::memcpy(f.data(), GPtr(Arg(c, 2)), 16);
  const uint32_t mask = uint32_t(Arg(c, 3));
  Set(Ctx0(c), [s, f, mask](Context& y) {
    y.blend = s;
    std::copy(f.begin(), f.end(), y.blend_factor);
    y.sample_mask = mask;
  });
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
  auto s = Obj<State>(Arg(c, 1));
  const uint32_t ref = uint32_t(Arg(c, 2));
  Set(Ctx0(c), [s, ref](Context& y) {
    y.depth_stencil = s;
    y.stencil_ref = ref;
  });
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
// RowPitch, DepthPitch}): the subresource's bytes in guest memory, in the
// resource's current block. WRITE_DISCARD renames: a fresh block when
// anything else (a command list) still holds the current one. On a
// deferred context every first map in a recording, and every DISCARD,
// gets a block of its own, which the command list installs as the
// resource's contents where the map was recorded; NO_OVERWRITE maps
// after it append to the same block.
CTX_METHOD(Map) {
  Context& x = Ctx0(c);
  auto r = Obj<Resource>(Arg(c, 1));
  const uint32_t sub = uint32_t(Arg(c, 2)), type = uint32_t(Arg(c, 3));
  const gaddr out = Arg(c, 5);
  if (r->dim != Dim::Buffer && sub >= r->Subresources()) return kE_INVALIDARG;
  std::shared_ptr<Block> b;
  if (x.deferred) {
    auto it = x.mapped.find(r.get());
    if (it != x.mapped.end() && type == kMapWriteNoOverwrite) {
      b = it->second;
    } else {
      b = std::make_shared<Block>(r->size);
      if (type != kMapWriteDiscard || r->dim != Dim::Buffer)
        std::memcpy(b->Data(), r->Data(), r->size);  // what the map does not rewrite stays
      x.mapped[r.get()] = b;
      x.ops.emplace_back([r, b](Context&) {
        r->SetContents(b);
        CpuWrote(*r, ~0u);
      });
    }
  } else {
    Device& dev = Dev();
    std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
    b = r->Contents();
    if (type == kMapWriteDiscard) {
      if (b.use_count() > 2) {  // the resource's and ours: something else holds it
        auto nb = std::make_shared<Block>(r->size);
        if (r->dim != Dim::Buffer) std::memcpy(nb->Data(), b->Data(), r->size);
        r->SetContents(nb);
        b = nb;
      }
      if (r->dim == Dim::Buffer) r->gpu_newer = false;
    } else if (type != kMapWriteNoOverwrite && r->gpu_newer) {
      GpuSyncToCpu(dev, *r);
    }
  }
  const uint32_t mip = r->dim == Dim::Buffer ? 0 : sub % r->mips;
  Wr64(out, b->mem + r->SubOffset(sub));
  Wr32(out + 8, r->dim == Dim::Buffer ? r->byte_width : r->RowPitch(mip));
  Wr32(out + 12, r->dim == Dim::Buffer ? r->byte_width : r->SlicePitch(mip));
  return kS_OK;
}
CTX_METHOD(Unmap) {
  auto r = Obj<Resource>(Arg(c, 1));
  const uint32_t sub = r->dim == Dim::Buffer ? 0 : uint32_t(Arg(c, 2));
  Context& x = Ctx0(c);
  if (x.deferred) {
    x.ops.emplace_back([r, sub](Context&) { CpuWrote(*r, sub); });
    return 0;
  }
  std::lock_guard<std::recursive_mutex> l(Dev().gpu_mu);
  CpuWrote(*r, sub);
  return 0;
}

// UpdateSubresource(resource, subresource, D3D11_BOX* {left, top, front,
// right, bottom, back}, data, row pitch, depth pitch). A deferred context
// keeps a copy of the bytes. With a box it reads them from data plus the
// box's corner (front * depth pitch + top * row pitch + left texels): the
// offset Windows' runtime applies when it records for a driver without
// command lists, which the documentation tells games to take back from
// the pointer they pass.
CTX_METHOD(UpdateSubresource) {
  Context& x = Ctx0(c);
  auto r = Obj<Resource>(Arg(c, 1));
  const uint32_t sub = uint32_t(Arg(c, 2));
  const gaddr bp = Arg(c, 3);
  gaddr src = Arg(c, 4);
  const uint32_t pitch = uint32_t(Arg(c, 5)), slice_pitch = uint32_t(Arg(c, 6));
  if (sub >= r->Subresources()) return 0;
  std::array<uint32_t, 6> box{};
  if (bp && !ReadBox(*r, sub, bp, &box)) return 0;
  if (!x.deferred) {
    GpuUpdate(Dev(), *r, sub, bp ? box.data() : nullptr, GPtr(src), pitch, slice_pitch);
    return 0;
  }
  // The bytes it reads.
  uint64_t size;
  if (r->dim == Dim::Buffer) {
    size = bp ? box[3] - box[0] : r->byte_width;
    if (bp) src += box[0];
  } else {
    const uint32_t mip = sub % r->mips;
    bool block;
    const uint32_t bpp = FormatBytes(r->format, &block), div = block ? 4 : 1;
    const uint32_t w = bp ? box[3] - box[0] : r->MipWidth(mip),
                   h = bp ? box[4] - box[1] : r->dim == Dim::Texture1D ? 1 : r->MipHeight(mip),
                   d = bp ? box[5] - box[2] : r->MipDepth(mip);
    size = uint64_t(d - 1) * slice_pitch + uint64_t((h + div - 1) / div - 1) * pitch +
           uint64_t((w + div - 1) / div) * bpp;
    if (bp) src += uint64_t(box[2]) * slice_pitch + uint64_t(box[1] / div) * pitch + box[0] / div * bpp;
  }
  std::vector<uint8_t> bytes(GPtr(src), GPtr(src) + size);
  const bool has_box = bp != 0;
  x.ops.emplace_back([r, sub, box, has_box, bytes, pitch, slice_pitch](Context&) {
    GpuUpdate(Dev(), *r, sub, has_box ? box.data() : nullptr, bytes.data(), pitch, slice_pitch);
  });
  return 0;
}

// CopyResource(dst, src): every subresource, of the same size.
CTX_METHOD(CopyResource) {
  auto d = Obj<Resource>(Arg(c, 1)), s = Obj<Resource>(Arg(c, 2));
  Run(Ctx0(c), [d, s](Context&) { GpuCopyResource(Dev(), *d, *s); });
  return 0;
}

// CopySubresourceRegion(dst, dst sub, x, y, z, src, src sub, box)
CTX_METHOD(CopySubresourceRegion) {
  auto d = Obj<Resource>(Arg(c, 1)), s = Obj<Resource>(Arg(c, 6));
  const uint32_t dsub = uint32_t(Arg(c, 2)), ssub = uint32_t(Arg(c, 7));
  const uint32_t dx = uint32_t(Arg(c, 3)), dy = uint32_t(Arg(c, 4)), dz = uint32_t(Arg(c, 5));
  const gaddr bp = Arg(c, 8);
  if (dsub >= d->Subresources() || ssub >= s->Subresources()) return 0;
  std::array<uint32_t, 6> box{};
  if (bp && !ReadBox(*s, ssub, bp, &box)) return 0;
  const bool has_box = bp != 0;
  Run(Ctx0(c), [=](Context&) {
    GpuCopy(Dev(), *d, dsub, dx, dy, dz, *s, ssub, has_box ? box.data() : nullptr);
  });
  return 0;
}

// ResolveSubresource(dst, dst sub, src, src sub, format): multisampled
// textures have one sample here, so it is a copy.
CTX_METHOD(ResolveSubresource) {
  auto d = Obj<Resource>(Arg(c, 1)), s = Obj<Resource>(Arg(c, 3));
  const uint32_t dsub = uint32_t(Arg(c, 2)), ssub = uint32_t(Arg(c, 4));
  if (dsub >= d->Subresources() || ssub >= s->Subresources()) return 0;
  Run(Ctx0(c), [=](Context&) { GpuCopy(Dev(), *d, dsub, 0, 0, 0, *s, ssub, nullptr); });
  return 0;
}

// ---- clears, draws and dispatches ------------------------------------------------------------

// (view, FLOAT color[4])
CTX_METHOD(ClearRenderTargetView) {
  auto v = Obj<View>(Arg(c, 1));
  std::array<float, 4> col;
  std::memcpy(col.data(), GPtr(Arg(c, 2)), 16);
  Run(Ctx0(c), [v, col](Context&) { GpuClearTarget(Dev(), *v, col.data()); });
  return 0;
}
// (view, flags, FLOAT depth, UINT8 stencil)
CTX_METHOD(ClearDepthStencilView) {
  auto v = Obj<View>(Arg(c, 1));
  const uint32_t flags = uint32_t(Arg(c, 2));
  const float z = ArgF(c, 3);
  const uint8_t stencil = uint8_t(Arg(c, 4));
  Run(Ctx0(c), [=](Context&) { GpuClearDepth(Dev(), *v, flags, z, stencil); });
  return 0;
}
// (view, values[4]): UINT or FLOAT values
CTX_METHOD(ClearUnorderedAccessViewUint) {
  auto v = Obj<View>(Arg(c, 1));
  std::array<uint32_t, 4> val;
  std::memcpy(val.data(), GPtr(Arg(c, 2)), 16);
  Run(Ctx0(c), [v, val](Context&) { GpuClearUav(Dev(), *v, val.data(), false); });
  return 0;
}
CTX_METHOD(ClearUnorderedAccessViewFloat) {
  auto v = Obj<View>(Arg(c, 1));
  std::array<uint32_t, 4> val;
  std::memcpy(val.data(), GPtr(Arg(c, 2)), 16);
  Run(Ctx0(c), [v, val](Context&) { GpuClearUav(Dev(), *v, val.data(), true); });
  return 0;
}
CTX_METHOD(GenerateMips) {
  auto v = Obj<View>(Arg(c, 1));
  Run(Ctx0(c), [v](Context&) { GpuGenerateMips(Dev(), *v); });
  return 0;
}

namespace {
void Draw(Context& x, const DrawCall& d) {
  Run(x, [d](Context& y) { GpuDraw(Dev(), y, d); });
}
}  // namespace

// Draw(count, first vertex)
CTX_METHOD(Draw) {
  DrawCall d;
  d.count = uint32_t(Arg(c, 1));
  d.first = uint32_t(Arg(c, 2));
  Draw(Ctx0(c), d);
  return 0;
}
// DrawIndexed(count, first index, base vertex)
CTX_METHOD(DrawIndexed) {
  DrawCall d;
  d.indexed = true;
  d.count = uint32_t(Arg(c, 1));
  d.first = uint32_t(Arg(c, 2));
  d.base_vertex = int32_t(Arg(c, 3));
  Draw(Ctx0(c), d);
  return 0;
}
// DrawInstanced(vertices per instance, instances, first vertex, first instance)
CTX_METHOD(DrawInstanced) {
  DrawCall d;
  d.count = uint32_t(Arg(c, 1));
  d.instances = uint32_t(Arg(c, 2));
  d.first = uint32_t(Arg(c, 3));
  d.first_instance = uint32_t(Arg(c, 4));
  Draw(Ctx0(c), d);
  return 0;
}
// DrawIndexedInstanced(indices per instance, instances, first index, base
// vertex, first instance)
CTX_METHOD(DrawIndexedInstanced) {
  DrawCall d;
  d.indexed = true;
  d.count = uint32_t(Arg(c, 1));
  d.instances = uint32_t(Arg(c, 2));
  d.first = uint32_t(Arg(c, 3));
  d.base_vertex = int32_t(Arg(c, 4));
  d.first_instance = uint32_t(Arg(c, 5));
  Draw(Ctx0(c), d);
  return 0;
}
CTX_METHOD(Dispatch) {
  const uint32_t gx = uint32_t(Arg(c, 1)), gy = uint32_t(Arg(c, 2)), gz = uint32_t(Arg(c, 3));
  Run(Ctx0(c), [=](Context& y) { GpuDispatch(Dev(), y, gx, gy, gz); });
  return 0;
}

// ---- queries --------------------------------------------------------------------------------

CTX_METHOD(Begin) { return 0; }
CTX_METHOD(End) {
  auto q = Obj<Query>(Arg(c, 1));
  Run(Ctx0(c), [q](Context&) { q->ended = true; });
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
  Set(Ctx0(c), [](Context& y) { StateOf(y) = ContextState{}; });
  return 0;
}
CTX_METHOD(Flush) { return 0; }
// D3D11_DEVICE_CONTEXT_IMMEDIATE 0, DEFERRED 1.
CTX_METHOD(GetType) { return Ctx0(c).deferred ? 1 : 0; }
CTX_METHOD(GetContextFlags) { return 0; }

// FinishCommandList(restore deferred context state, &command list): the
// recording so far as a command list. The context's state goes back to
// its defaults unless kept, and then the next recording starts by
// setting it, as a command list runs from the default state.
CTX_METHOD(FinishCommandList) {
  Context& x = Ctx0(c);
  if (!x.deferred) return kDXGI_ERROR_INVALID_CALL;
  const bool keep = Arg(c, 1) != 0;
  auto l = std::make_shared<CommandList>();
  l->ops = std::move(x.ops);
  x.ops.clear();
  x.mapped.clear();
  if (keep) x.ops.emplace_back([s = ContextState(x)](Context& y) { StateOf(y) = s; });
  else StateOf(x) = ContextState{};
  const gaddr out = Arg(c, 2);
  if (!out) return kS_OK;
  Wr64(out, com::Publish(l, com::Vtable(kDll, "ID3D11CommandList",
                                        {"QueryInterface", "AddRef", "Release", "GetDevice",
                                         "GetPrivateData", "SetPrivateData",
                                         "SetPrivateDataInterface", "GetContextFlags"})));
  return kS_OK;
}

// ExecuteCommandList(command list, restore context state): its operations
// from the default state; then the context's own state back, or the
// defaults.
CTX_METHOD(ExecuteCommandList) {
  auto l = Obj<CommandList>(Arg(c, 1));
  const bool restore = Arg(c, 2) != 0;
  Run(Ctx0(c), [l, restore](Context& y) {
    std::lock_guard<std::recursive_mutex> g(Dev().gpu_mu);
    ContextState saved;
    if (restore) saved = y;
    StateOf(y) = ContextState{};
    for (const Op& op : l->ops) op(y);
    StateOf(y) = restore ? saved : ContextState{};
    Dev().stats.executed_lists++;
  });
  return 0;
}

}  // namespace recomp::hle::d3d11
