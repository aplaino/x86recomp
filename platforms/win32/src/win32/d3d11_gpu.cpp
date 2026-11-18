// Direct3D 11 on gfx::Device's programs: resources made on the GPU from
// their records (d3d11_impl.h) and kept up to date with the guest's CPU
// writes, the pipeline state translated per draw, draws and dispatches
// through the program interface, clears, copies and Present.
//
// Everything here runs on the thread driving the immediate context (or
// presenting), under Device::gpu_mu.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

#include "runtime/log.h"
#include "win32/d3d11_impl.h"
#include "win32/hle.h"
#include "x86/present.h"
#include "x86/runtime.h"

namespace recomp::hle::d3d11 {

namespace {

using gfx::Format;

// Warns once per call site.
#define WARN_ONCE(...)              \
  do {                              \
    static bool warned_ = false;    \
    if (!warned_) {                 \
      warned_ = true;               \
      WarnF(__VA_ARGS__);           \
    }                               \
  } while (0)

// ---- formats ---------------------------------------------------------------------------

bool Typeless(uint32_t f) {
  switch (f) {
    case 1: case 5: case 9: case 15: case 19: case 23: case 27: case 33: case 39: case 44:
    case 48: case 53: case 60: case 70: case 73: case 76: case 79: case 82: case 90: case 92:
    case 94: case 97:
      return true;
    default:
      return false;
  }
}

// A typeless family's usual typed member.
uint32_t DefaultTyped(uint32_t f) {
  switch (f) {
    case 1: return 2;    case 5: return 6;    case 9: return 10;   case 15: return 16;
    case 23: return 24;  case 27: return 28;  case 33: return 34;  case 39: return 41;
    case 48: return 49;  case 53: return 54;  case 60: return 61;  case 70: return 71;
    case 73: return 74;  case 76: return 77;  case 79: return 80;  case 82: return 83;
    case 90: return 87;  case 92: return 88;  case 94: return 95;  case 97: return 98;
    default: return f;
  }
}

// The format a resource is drawn with: its render target views' (the
// sRGB twin of a swap chain's buffer, or a typeless resource's), its
// other views' when typeless, else its own.
uint32_t DrawnFormat(const Resource& r) {
  uint32_t f = r.rtv_format ? r.rtv_format : Typeless(r.format) && r.view_format ? r.view_format
                                                                                  : r.format;
  return Typeless(f) ? DefaultTyped(f) : f;
}

// How guest texels become the GPU format's: formats gfx lacks are
// widened on upload.
enum class Conv : uint8_t { None, A8, X8, B5G6R5, B5G5R5A1, B4G4R4A4, Rgb32F, Rgb32I };
struct ColorFormat {
  Format fmt = Format::Unknown;
  Conv conv = Conv::None;
};

ColorFormat ColorFormatOf(uint32_t f) {
  switch (f) {
    case 2: return {Format::A32B32G32R32F};
    case 3: return {Format::R32G32B32A32Uint};
    case 4: return {Format::R32G32B32A32Sint};
    case 6: return {Format::A32B32G32R32F, Conv::Rgb32F};
    case 7: return {Format::R32G32B32A32Uint, Conv::Rgb32I};
    case 8: return {Format::R32G32B32A32Sint, Conv::Rgb32I};
    case 10: return {Format::A16B16G16R16F};
    case 11: return {Format::R16G16B16A16};
    case 12: return {Format::R16G16B16A16Uint};
    case 13: return {Format::R16G16B16A16Snorm};
    case 16: return {Format::G32R32F};
    case 17: return {Format::R32G32Uint};
    case 18: return {Format::R32G32Sint};
    case 24: return {Format::R10G10B10A2};
    case 26: return {Format::R11G11B10F};
    case 28: return {Format::R8G8B8A8};
    case 29: return {Format::R8G8B8A8Srgb};
    case 30: return {Format::R8G8B8A8Uint};
    case 31: return {Format::R8G8B8A8Snorm};
    case 34: return {Format::G16R16F};
    case 35: return {Format::R16G16};
    case 36: return {Format::R16G16Uint};
    case 37: return {Format::R16G16Snorm};
    case 41: return {Format::R32F};
    case 42: return {Format::R32Uint};
    case 43: return {Format::R32Sint};
    case 49: return {Format::R8G8};
    case 50: return {Format::R8G8Uint};
    case 54: return {Format::R16F};
    case 56: return {Format::R16};
    case 57: return {Format::R16Uint};
    case 61: return {Format::R8};
    case 62: return {Format::R8Uint};
    case 65: return {Format::R8G8B8A8, Conv::A8};
    case 71: return {Format::Bc1};
    case 72: return {Format::Bc1Srgb};
    case 74: return {Format::Bc2};
    case 75: return {Format::Bc2Srgb};
    case 77: return {Format::Bc3};
    case 78: return {Format::Bc3Srgb};
    case 80: return {Format::Bc4};
    case 83: return {Format::Bc5};
    case 85: return {Format::A8R8G8B8, Conv::B5G6R5};
    case 86: return {Format::A8R8G8B8, Conv::B5G5R5A1};
    case 87: return {Format::A8R8G8B8};  // bytes B, G, R, A
    case 88: return {Format::A8R8G8B8, Conv::X8};
    case 91: return {Format::B8G8R8A8Srgb};
    case 93: return {Format::B8G8R8A8Srgb, Conv::X8};
    case 95: return {Format::Bc6hUfloat};
    case 96: return {Format::Bc6hSfloat};
    case 98: return {Format::Bc7};
    case 99: return {Format::Bc7Srgb};
    case 115: return {Format::A8R8G8B8, Conv::B4G4R4A4};
    default: return {};
  }
}

// A depth-stencil resource's depth target format, by its family.
Format DepthFormatOf(uint32_t f) {
  switch (f) {
    case 19: case 20: case 21: case 22: return Format::D32FS8;
    case 39: case 40: case 41: return Format::D32;
    case 44: case 45: case 46: case 47: return Format::D24S8;
    case 53: case 55: case 56: return Format::D16;
    default: FatalF("d3d11: depth-stencil format %u not supported", f);
  }
}

// Widens rows of `n` guest texels in `conv` to the GPU format.
void ConvertRow(Conv conv, const uint8_t* s, uint8_t* d, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    switch (conv) {
      case Conv::A8:
        d[4 * i] = d[4 * i + 1] = d[4 * i + 2] = 0;
        d[4 * i + 3] = s[i];
        break;
      case Conv::X8:
        std::memcpy(d + 4 * i, s + 4 * i, 3);
        d[4 * i + 3] = 0xFF;
        break;
      case Conv::B5G6R5: case Conv::B5G5R5A1: case Conv::B4G4R4A4: {
        const uint32_t v = uint32_t(s[2 * i]) | uint32_t(s[2 * i + 1]) << 8;
        auto x = [](uint32_t c, int bits) { return uint8_t((c * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); };
        uint8_t* p = d + 4 * i;  // B, G, R, A
        if (conv == Conv::B5G6R5) {
          p[0] = x(v & 31, 5), p[1] = x((v >> 5) & 63, 6), p[2] = x(v >> 11, 5), p[3] = 0xFF;
        } else if (conv == Conv::B5G5R5A1) {
          p[0] = x(v & 31, 5), p[1] = x((v >> 5) & 31, 5), p[2] = x((v >> 10) & 31, 5);
          p[3] = v >> 15 ? 0xFF : 0;
        } else {
          p[0] = x(v & 15, 4), p[1] = x((v >> 4) & 15, 4), p[2] = x((v >> 8) & 15, 4);
          p[3] = x(v >> 12, 4);
        }
        break;
      }
      case Conv::Rgb32F: case Conv::Rgb32I: {
        std::memcpy(d + 16 * i, s + 12 * i, 12);
        const uint32_t one = conv == Conv::Rgb32F ? 0x3F800000u : 1u;
        std::memcpy(d + 16 * i + 12, &one, 4);
        break;
      }
      case Conv::None:
        break;
    }
  }
}
uint32_t ConvBytes(Conv conv) { return conv == Conv::Rgb32F || conv == Conv::Rgb32I ? 16 : 4; }

// ---- images --------------------------------------------------------------------------------

bool IsDepth(const Resource& r) { return r.dim != Dim::Buffer && (r.bind & kBindDepthStencil); }

// What the first shader resource view says the texture is sampled as
// (an array with one layer, a cube) decides its kind where its
// description does not.
gfx::TextureKind KindOf(const Resource& r) {
  switch (r.dim) {
    case Dim::Texture3D:
      return gfx::TextureKind::Tex3D;
    case Dim::Texture1D:  // a 2D texture of height 1, as the translator reads it
      return r.array > 1 || r.srv_dim == 3 ? gfx::TextureKind::Tex2DArray
                                           : gfx::TextureKind::Tex2D;
    default:
      if ((r.misc & kMiscTextureCube) && r.array == 6 && r.srv_dim != 5)
        return gfx::TextureKind::Cube;
      if (r.misc & kMiscTextureCube && r.array > 6)
        WARN_ONCE("d3d11: cube map arrays are made as 2D arrays\n");
      return r.array > 1 || r.srv_dim == 5 || r.srv_dim == 7 ? gfx::TextureKind::Tex2DArray
                                                             : gfx::TextureKind::Tex2D;
  }
}

uint32_t Rows(const Resource& r, uint32_t mip) {
  bool block;
  FormatBytes(r.format, &block);
  const uint32_t h = r.dim == Dim::Texture1D ? 1 : r.MipHeight(mip);
  return block ? (h + 3) / 4 : h;
}

// Uploads `rc` of a level and layer from guest-layout bytes (rows
// `pitch` apart), widened where the GPU format needs it.
void UploadRect(Device& dev, const Resource& r, uint32_t mip, uint32_t layer, const gfx::Rect& rc,
                const uint8_t* src, uint32_t pitch) {
  const Conv conv = ColorFormatOf(DrawnFormat(r)).conv;
  if (conv == Conv::None) {
    dev.gfx->UpdateImage(r.image, int(mip), int(layer), rc, src, pitch);
    return;
  }
  const uint32_t w = uint32_t(rc.right - rc.left), h = uint32_t(rc.bottom - rc.top);
  std::vector<uint8_t> wide(size_t(w) * h * ConvBytes(conv));
  for (uint32_t y = 0; y < h; y++)
    ConvertRow(conv, src + size_t(y) * pitch, wide.data() + size_t(y) * w * ConvBytes(conv), w);
  dev.gfx->UpdateImage(r.image, int(mip), int(layer), rc, wide.data(), w * ConvBytes(conv));
}

// A subresource from guest memory to the image.
void UploadSub(Device& dev, Resource& r, uint32_t sub) {
  const uint32_t mip = sub % r.mips, slice = sub / r.mips;
  const uint8_t* data = r.Data() + r.SubOffset(sub);
  const gfx::Rect rc{0, 0, int32_t(r.MipWidth(mip)),
                     int32_t(r.dim == Dim::Texture1D ? 1 : r.MipHeight(mip))};
  if (r.dim == Dim::Texture3D) {
    for (uint32_t z = 0; z < r.MipDepth(mip); z++)
      UploadRect(dev, r, mip, z, rc, data + uint64_t(z) * r.SlicePitch(mip), r.RowPitch(mip));
    return;
  }
  UploadRect(dev, r, mip, slice, rc, data, r.RowPitch(mip));
}

// The resource's image or depth target, made if it has none, with the
// guest's writes it lacks uploaded.
void EnsureImage(Device& dev, Resource& r) {
  if (r.dim == Dim::Buffer) FatalF("d3d11: a buffer used as a texture");
  if (!r.image && !r.depth_target) {
    const uint32_t h = r.dim == Dim::Texture1D ? 1 : r.height;
    if (IsDepth(r)) {
      const Format f = DepthFormatOf(r.format);
      const bool layered = r.array > 1 || r.srv_dim == 5 || r.srv_dim == 3 || r.srv_dim == 7;
      r.depth_target = layered ? dev.gfx->CreateDepthArray(int(r.width), int(h), int(r.array), f)
                               : dev.gfx->CreateDepth(int(r.width), int(h), f);
      if (!r.depth_target)
        FatalF("d3d11: no depth target %ux%u x%u (format %u)", r.width, h, r.array, r.format);
      if (r.mips > 1) WARN_ONCE("d3d11: depth targets have one level on the GPU\n");
      for (uint32_t l = 0; l < r.array; l++) dev.gfx->ClearDepth(r.depth_target, 1.0f, 0, int(l));
      r.dirty.clear();
      return;
    }
    const ColorFormat cf = ColorFormatOf(DrawnFormat(r));
    if (cf.fmt == Format::Unknown)
      FatalF("d3d11: texture format %u (drawn as %u) not supported on the GPU", r.format,
             DrawnFormat(r));
    gfx::ImageDesc d;
    d.w = int(r.width);
    d.h = int(h);
    d.layers = int(r.dim == Dim::Texture3D ? r.depth : r.array);
    d.levels = int(r.mips);
    d.fmt = cf.fmt;
    d.kind = KindOf(r);
    d.color_target = (r.bind & (kBindRenderTarget | kBindUnorderedAccess)) ||
                     (r.misc & kMiscGenerateMips);
    d.storage = (r.bind & kBindUnorderedAccess) != 0;
    if (r.samples > 1) WARN_ONCE("d3d11: multisampled textures are made with one sample\n");
    r.image = dev.gfx->CreateImage(d);
    if (!r.image)
      FatalF("d3d11: no image %ux%ux%u x%u (format %u)", r.width, h, d.layers, r.mips, r.format);
    r.gfx_format = uint32_t(cf.fmt);
    // A render target the CPU never wrote starts cleared; anything else
    // starts as its guest memory.
    if (d.color_target && !r.version) {
      for (uint32_t m = 0; m < r.mips; m++) dev.gfx->ClearImage(r.image, int(m), {0, 0, 0, 0});
      r.dirty.assign(r.Subresources(), 0);
    } else {
      r.dirty.assign(r.Subresources(), 1);
    }
  }
  if (r.image)
    for (uint32_t s = 0; s < r.dirty.size(); s++)
      if (r.dirty[s]) {
        UploadSub(dev, r, s);
        r.dirty[s] = 0;
      }
}

gfx::TextureId TextureOf(Device& dev, Resource& r) {
  EnsureImage(dev, r);
  return r.image ? r.image : r.depth_target;
}

// ---- buffers ---------------------------------------------------------------------------------

// Whether draws read a buffer in place on the GPU: static vertex, index
// and shader resource buffers. Constant buffers and dynamic buffers,
// rewritten between draws, are copied by each draw instead, as is a
// buffer refilled more than twice in a frame (UpdateSubresource between
// draws: a GPU buffer would have to wait for the draws before each fill).
bool InPlace(const Resource& r) {
  if (r.bind & kBindUnorderedAccess) return true;
  return !r.host_reads && (r.usage == Usage::Default || r.usage == Usage::Immutable) &&
         !(r.bind & kBindConstantBuffer);
}

// The buffer's GPU copy, made or brought up to date; false if draws are
// to copy it from now on.
bool EnsureGpuBuffer(Device& dev, Resource& r) {
  if (!r.gpu_buffer) {
    r.gpu_buffer = dev.gfx->CreateBuffer(r.byte_width);
    if (!r.gpu_buffer) FatalF("d3d11: no GPU buffer of %u bytes", r.byte_width);
    dev.gfx->UpdateBuffer(r.gpu_buffer, 0, r.Data(), r.byte_width);
    r.dirty_lo = r.dirty_hi = 0;
    return true;
  }
  if (r.dirty_hi <= r.dirty_lo) return true;
  if (r.upload_frame != dev.frames) {
    r.upload_frame = dev.frames;
    r.uploads = 0;
  }
  if (++r.uploads > 2 && !(r.bind & kBindUnorderedAccess)) {
    r.host_reads = true;
    dev.gfx->DestroyBuffer(r.gpu_buffer);
    r.gpu_buffer = 0;
    return false;
  }
  const uint64_t lo = r.dirty_lo & ~3ull, hi = std::min<uint64_t>(r.dirty_hi, r.byte_width);
  dev.gfx->UpdateBuffer(r.gpu_buffer, uint32_t(lo), r.Data() + lo, uint32_t(hi - lo));
  r.dirty_lo = r.dirty_hi = 0;
  return true;
}

// Zeros for buffers a draw reads that are unbound or too short.
struct Scratch {
  std::vector<std::vector<uint8_t>> keep;
  const uint8_t* Zeros(size_t n) {
    keep.emplace_back(std::max<size_t>(n, 16), 0);
    return keep.back().data();
  }
};

// `bd` reads `size` bytes of buffer r from byte `at` (fewer if it has
// fewer: zeros for an empty range).
void BindBuffer(Device& dev, Resource& r, uint64_t at, uint64_t size, gfx::BufferData* bd,
                Scratch& s) {
  if (at >= r.byte_width || !size) {
    bd->data = s.Zeros(16);
    bd->size = 16;
    return;
  }
  size = std::min<uint64_t>(size, r.byte_width - at);
  if (InPlace(r) && at % 4 == 0 && EnsureGpuBuffer(dev, r)) {
    bd->buffer = r.gpu_buffer;
    bd->offset = uint32_t(at);
    bd->size = uint32_t(size);
    return;
  }
  bd->data = r.Data() + at;
  bd->size = uint32_t(size);
}

// ---- states ------------------------------------------------------------------------------------

template <typename T>
T Field(const std::vector<uint8_t>& d, size_t at, T dflt) {
  if (d.size() < at + sizeof(T)) return dflt;
  T v;
  std::memcpy(&v, d.data() + at, sizeof v);
  return v;
}

gfx::CompareOp Compare(uint32_t f) {  // D3D11_COMPARISON_FUNC
  return f >= 1 && f <= 8 ? gfx::CompareOp(f - 1) : gfx::CompareOp::Always;
}

gfx::BlendFactor Factor(uint32_t b) {  // D3D11_BLEND
  using B = gfx::BlendFactor;
  switch (b) {
    case 1: return B::Zero;
    case 2: return B::One;
    case 3: return B::SrcColor;
    case 4: return B::OneMinusSrcColor;
    case 5: return B::SrcAlpha;
    case 6: return B::OneMinusSrcAlpha;
    case 7: return B::DstAlpha;
    case 8: return B::OneMinusDstAlpha;
    case 9: return B::DstColor;
    case 10: return B::OneMinusDstColor;
    case 11: return B::SrcAlphaSaturate;
    case 14: return B::ConstantColor;
    case 15: return B::OneMinusConstantColor;
    case 16: case 17: case 18: case 19:
      WARN_ONCE("d3d11: dual-source blending is drawn with the first source\n");
      return b == 16 ? B::SrcColor : b == 17 ? B::OneMinusSrcColor : b == 18 ? B::SrcAlpha
                                                                          : B::OneMinusSrcAlpha;
    default: return B::One;
  }
}

gfx::BlendOp BlendOpOf(uint32_t op) {  // D3D11_BLEND_OP
  return op >= 1 && op <= 5 ? gfx::BlendOp(op - 1) : gfx::BlendOp::Add;
}

gfx::StencilOp StencilOpOf(uint32_t op) {  // D3D11_STENCIL_OP
  return op >= 1 && op <= 8 ? gfx::StencilOp(op - 1) : gfx::StencilOp::Keep;
}

gfx::Topology TopologyOf(uint32_t t, bool* ok) {  // D3D11_PRIMITIVE_TOPOLOGY
  *ok = true;
  switch (t) {
    case 1: return gfx::Topology::PointList;
    case 2: case 10: return gfx::Topology::LineList;
    case 3: case 11: return gfx::Topology::LineStrip;
    case 4: case 12: return gfx::Topology::TriangleList;
    case 5: case 13: return gfx::Topology::TriangleStrip;
    default:
      *ok = false;
      return gfx::Topology::TriangleList;
  }
}

// The descriptions (bytes as CreateXState got them; empty: the state's
// defaults) as the draw's gfx state.
gfx::PipelineState PipelineOf(const ContextState& x, bool* ok) {
  gfx::PipelineState p;
  p.topology = TopologyOf(x.topology, ok);
  // D3D11_RASTERIZER_DESC: FillMode, CullMode, FrontCounterClockwise,
  // DepthBias, DepthBiasClamp, SlopeScaledDepthBias, DepthClipEnable,
  // ScissorEnable, MultisampleEnable, AntialiasedLineEnable.
  static const std::vector<uint8_t> none;
  const std::vector<uint8_t>& rs = x.rasterizer ? x.rasterizer->desc : none;
  const uint32_t cull = Field<uint32_t>(rs, 4, 3);
  p.cull = cull == 2 ? gfx::CullMode::Front : cull == 3 ? gfx::CullMode::Back : gfx::CullMode::None;
  p.front_ccw = Field<uint32_t>(rs, 8, 0) != 0;
  p.depth_bias = float(Field<int32_t>(rs, 12, 0));
  p.depth_bias_clamp = Field<float>(rs, 16, 0.0f);
  p.depth_bias_slope = Field<float>(rs, 20, 0.0f);
  p.depth_clip = Field<uint32_t>(rs, 24, 1) != 0;
  // D3D11_DEPTH_STENCIL_DESC: DepthEnable, DepthWriteMask, DepthFunc,
  // StencilEnable, StencilReadMask, StencilWriteMask (bytes), FrontFace,
  // BackFace {fail, depth fail, pass, func}.
  const std::vector<uint8_t>& ds = x.depth_stencil ? x.depth_stencil->desc : none;
  p.depth_test = Field<uint32_t>(ds, 0, 1) != 0;
  p.depth_write = p.depth_test && Field<uint32_t>(ds, 4, 1) != 0;
  p.depth_compare = p.depth_test ? Compare(Field<uint32_t>(ds, 8, 2)) : gfx::CompareOp::Always;
  p.stencil = Field<uint32_t>(ds, 12, 0) != 0;
  p.stencil_read_mask = Field<uint8_t>(ds, 16, 0xFF);
  p.stencil_write_mask = Field<uint8_t>(ds, 17, 0xFF);
  for (int f = 0; f < 2; f++) {
    gfx::StencilFace& s = f ? p.stencil_back : p.stencil_front;
    const size_t at = f ? 36 : 20;
    s.fail = StencilOpOf(Field<uint32_t>(ds, at, 1));
    s.depth_fail = StencilOpOf(Field<uint32_t>(ds, at + 4, 1));
    s.pass = StencilOpOf(Field<uint32_t>(ds, at + 8, 1));
    s.compare = Compare(Field<uint32_t>(ds, at + 12, 8));
  }
  p.stencil_ref = uint8_t(x.stencil_ref);
  // D3D11_BLEND_DESC: AlphaToCoverageEnable, IndependentBlendEnable,
  // RenderTarget[8] {BlendEnable, SrcBlend, DestBlend, BlendOp,
  // SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha, RenderTargetWriteMask}.
  const std::vector<uint8_t>& bs = x.blend ? x.blend->desc : none;
  const bool independent = Field<uint32_t>(bs, 4, 0) != 0;
  if (Field<uint32_t>(bs, 0, 0)) WARN_ONCE("d3d11: alpha to coverage is not drawn\n");
  for (int i = 0; i < gfx::kMaxTargets; i++) {
    const size_t at = 8 + 32 * size_t(independent ? i : 0);
    gfx::TargetBlend& b = p.blend[i];
    b.enable = Field<uint32_t>(bs, at, 0) != 0;
    b.src_color = Factor(Field<uint32_t>(bs, at + 4, 2));
    b.dst_color = Factor(Field<uint32_t>(bs, at + 8, 1));
    b.color_op = BlendOpOf(Field<uint32_t>(bs, at + 12, 1));
    b.src_alpha = Factor(Field<uint32_t>(bs, at + 16, 2));
    b.dst_alpha = Factor(Field<uint32_t>(bs, at + 20, 1));
    b.alpha_op = BlendOpOf(Field<uint32_t>(bs, at + 24, 1));
    b.write_mask = Field<uint8_t>(bs, at + 28, 0xF) & 0xF;
  }
  p.blend_constant = {x.blend_factor[0], x.blend_factor[1], x.blend_factor[2], x.blend_factor[3]};
  return p;
}

// D3D11_SAMPLER_DESC: Filter, AddressU, AddressV, AddressW, MipLODBias,
// MaxAnisotropy, ComparisonFunc, BorderColor[4], MinLOD, MaxLOD. Border
// colours are not drawn (gfx clamps to the edge).
gfx::SamplerDesc SamplerOf(const State* s) {
  static const std::vector<uint8_t> none;
  const std::vector<uint8_t>& d = s ? s->desc : none;
  const uint32_t f = Field<uint32_t>(d, 0, 0x15);
  gfx::SamplerDesc g;
  const bool aniso = (f & 0x40) != 0;
  g.min_linear = aniso || (f & 0x10);
  g.mag_linear = aniso || (f & 0x4);
  g.mip_linear = aniso || (f & 0x1);
  g.max_anisotropy = aniso ? float(std::clamp<uint32_t>(Field<uint32_t>(d, 20, 1), 1, 16)) : 1.0f;
  auto address = [](uint32_t a) {
    switch (a) {
      case 1: return gfx::AddressMode::Repeat;
      case 2: case 5: return gfx::AddressMode::MirroredRepeat;
      default: return gfx::AddressMode::ClampToEdge;
    }
  };
  g.u = address(Field<uint32_t>(d, 4, 3));
  g.v = address(Field<uint32_t>(d, 8, 3));
  g.w = address(Field<uint32_t>(d, 12, 3));
  g.lod_bias = Field<float>(d, 16, 0.0f);
  g.compare = (f & 0x180) == 0x80;
  g.compare_op = Compare(Field<uint32_t>(d, 24, 1));
  g.min_lod = std::max(0.0f, Field<float>(d, 44, -3.402823466e38f));
  g.max_lod = std::max(g.min_lod, std::min(1000.0f, Field<float>(d, 48, 3.402823466e38f)));
  return g;
}

// ---- draws -----------------------------------------------------------------------------------------

// The lowest and highest of `count` indices from `first` (guest memory).
struct MinMax {
  uint32_t lo = 0, hi = 0;
};
MinMax IndexRange(Device& dev, Resource& ib, uint32_t format, uint64_t at, uint32_t count) {
  struct Key {
    uint64_t serial, version, at;
    uint32_t count, format;
    bool operator<(const Key& o) const {
      return std::tie(serial, version, at, count, format) <
             std::tie(o.serial, o.version, o.at, o.count, o.format);
    }
  };
  static std::map<Key, MinMax> cache;
  const bool keep = ib.usage != Usage::Dynamic;
  const Key k{ib.serial, ib.version, at, count, format};
  if (keep)
    if (auto it = cache.find(k); it != cache.end()) return it->second;
  MinMax m{~0u, 0};
  const uint8_t* p = ib.Data() + at;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t v;
    if (format == 42) std::memcpy(&v, p + 4 * size_t(i), 4);
    else v = uint32_t(p[2 * i]) | uint32_t(p[2 * i + 1]) << 8;
    if ((format == 42 && v == 0xFFFFFFFFu) || (format != 42 && v == 0xFFFF)) continue;  // strip cut
    m.lo = std::min(m.lo, v);
    m.hi = std::max(m.hi, v);
  }
  if (m.lo > m.hi) m = {0, 0};
  if (keep) {
    if (cache.size() > 65536) cache.clear();
    cache[k] = m;
  }
  (void)dev;
  return m;
}

// What a draw binds for one stage, alive until DrawProgram has copied it.
struct StageData {
  std::vector<gfx::TextureId> textures;
  std::vector<gfx::SamplerDesc> samplers;
  std::vector<gfx::BufferData> buffers;
  StageUniforms uniforms{};
  std::vector<gfx::ColorTarget> rw_images;
  std::vector<gfx::RwBufferData> rw_buffers;
};

uint32_t ElementBytes(const View& v) {
  const Resource& r = *v.res;
  if (r.misc & kMiscBufferStructured) return r.stride;
  if (v.view_dim == 11 && (v.flags & 1)) return 4;  // raw
  bool block;
  const uint32_t b = FormatBytes(v.format, &block);
  return b ? b : 4;
}

// The read-only buffers and textures of a stage's bindings.
void BindStage(Device& dev, ContextState& x, int st, const StageBindings& b, const DrawCall* dc,
               StageData& sd, Scratch& s, gfx::ProgramResources* out) {
  ContextState::StageState& ss = x.stage[st];
  sd.buffers.resize(b.buffers.size());
  uint32_t vertex_buffers = 0;
  for (size_t i = 0; i < b.buffers.size(); i++) {
    const StageBindings::Buffer& e = b.buffers[i];
    gfx::BufferData& bd = sd.buffers[i];
    const ContextState::StageState& es = e.stage >= 0 ? x.stage[e.stage] : ss;
    switch (e.kind) {
      case StageBindings::BufferKind::Constant: {
        const ContextState::ConstantBuffer& cb = e.slot < 14 ? es.cbs[e.slot] : ContextState::ConstantBuffer{};
        if (!cb.res) {
          bd.data = s.Zeros(e.bytes);
          bd.size = std::max(e.bytes, 16u);
          break;
        }
        const uint64_t at = uint64_t(cb.first) * 16;
        const uint64_t have = at < cb.res->byte_width
                                  ? std::min<uint64_t>(cb.count ? cb.count * 16ull : ~0ull,
                                                       cb.res->byte_width - at)
                                  : 0;
        if (have >= e.bytes) {
          bd.data = cb.res->Data() + at;
          bd.size = uint32_t(have);
        } else {
          uint8_t* z = const_cast<uint8_t*>(s.Zeros(e.bytes));
          if (have) std::memcpy(z, cb.res->Data() + at, have);
          bd.data = z;
          bd.size = std::max(e.bytes, 16u);
        }
        break;
      }
      case StageBindings::BufferKind::View: {
        const View* v = e.slot < 128 ? es.srvs[e.slot].get() : nullptr;
        if (!v || v->res->dim != Dim::Buffer) {
          bd.data = s.Zeros(16);
          bd.size = 16;
          break;
        }
        const uint32_t eb = ElementBytes(*v);
        BindBuffer(dev, *v->res, uint64_t(v->first_element) * eb, uint64_t(v->elements) * eb, &bd,
                   s);
        break;
      }
      case StageBindings::BufferKind::Vertex: {
        const uint32_t k = vertex_buffers++;
        const ContextState::VertexBuffer& vb = e.slot < 32 ? x.vbs[e.slot] : ContextState::VertexBuffer{};
        if (k >= 32) FatalF("d3d11: a vertex stage reading more than 32 vertex buffers");
        sd.uniforms.ia[k][0] = vb.stride;
        sd.uniforms.ia[k][1] = 0;
        if (!vb.res || !dc) {
          bd.data = s.Zeros(16);
          bd.size = 16;
          break;
        }
        Resource& r = *vb.res;
        if (InPlace(r) && vb.offset % 4 == 0 && vb.offset < r.byte_width && EnsureGpuBuffer(dev, r)) {
          bd.buffer = r.gpu_buffer;
          bd.offset = vb.offset;
          bd.size = r.byte_width - vb.offset;
          break;
        }
        // Copied: only the vertices (instances) the draw reads.
        bool per_instance = false;
        uint32_t step = 1, extent = 0;
        if (x.layout)
          for (const InputLayout::Element& el : x.layout->elements)
            if (el.slot == e.slot) {
              per_instance = el.per_instance != 0;
              step = el.step_rate;
              bool block;
              extent = std::max(extent, el.offset + FormatBytes(el.format, &block));
            }
        int64_t lo, hi;
        if (dc->args) {  // arguments on the GPU: all of it
          lo = 0;
          hi = vb.stride ? int64_t(r.byte_width / vb.stride) + 1 : 1;
        } else if (per_instance) {
          lo = dc->first_instance;
          hi = lo + (step ? (dc->instances + step - 1) / step : 1);
        } else if (dc->indexed) {
          const uint32_t isize = x.ib_format == 42 ? 4 : 2;
          MinMax m{0, 0};
          if (x.ib)
            m = IndexRange(dev, *x.ib, x.ib_format, x.ib_offset + uint64_t(dc->first) * isize,
                           uint32_t(std::min<uint64_t>(
                               dc->count, x.ib->byte_width > x.ib_offset + uint64_t(dc->first) * isize
                                              ? (x.ib->byte_width - x.ib_offset - uint64_t(dc->first) * isize) / isize
                                              : 0)));
          lo = int64_t(m.lo) + dc->base_vertex;
          hi = int64_t(m.hi) + dc->base_vertex + 1;
        } else {
          lo = dc->first;
          hi = int64_t(dc->first) + dc->count;
        }
        lo = std::max<int64_t>(lo, 0);
        uint64_t at, size;
        if (!vb.stride) {
          lo = 0;
          at = vb.offset;
          size = std::max(extent, 4u);
        } else {
          at = vb.offset + uint64_t(lo) * vb.stride;
          size = uint64_t(std::max<int64_t>(hi - lo, 1)) * vb.stride;
        }
        sd.uniforms.ia[k][1] = uint32_t(lo);
        if (at >= r.byte_width) {
          bd.data = s.Zeros(size_t(std::min<uint64_t>(size, 1u << 20)));
          bd.size = uint32_t(std::min<uint64_t>(size, 1u << 20));
          break;
        }
        bd.data = r.Data() + at;
        bd.size = uint32_t(std::min<uint64_t>(size, r.byte_width - at));
        break;
      }
    }
  }
  sd.textures.resize(b.textures.size());
  // A sampler per texture, or the samplers apart.
  const bool apart = !b.samplers.empty();
  sd.samplers.resize(apart ? b.samplers.size() : b.textures.size());
  for (size_t k = 0; k < b.samplers.size(); k++) {
    const int slot = b.samplers[k];
    sd.samplers[k] = slot >= 0 && slot < 16 ? SamplerOf(ss.samplers[slot].get()) : gfx::SamplerDesc{};
  }
  for (size_t i = 0; i < b.textures.size(); i++) {
    const StageBindings::Texture& t = b.textures[i];
    const View* v = t.view < 128 ? ss.srvs[t.view].get() : nullptr;
    sd.textures[i] = v && v->res->dim != Dim::Buffer ? TextureOf(dev, *v->res) : 0;
    if (!apart)
      sd.samplers[i] = t.sampler >= 0 && t.sampler < 16 ? SamplerOf(ss.samplers[t.sampler].get())
                                                        : gfx::SamplerDesc{};
  }
  // The typed buffer views' formats (R_FMT_T#, R_FMT_U#): 0 for none,
  // which the translation reads as zeros.
  for (size_t i = 0; i < b.formats.size() && i < kMaxTypedViews; i++) {
    const uint32_t slot = b.formats[i].slot;
    const View* v = b.formats[i].uav ? (slot < 8 ? x.uavs[slot].get() : nullptr)
                                     : (slot < 128 ? ss.srvs[slot].get() : nullptr);
    sd.uniforms.formats[i] = v ? v->format : 0;
  }
  out->textures = sd.textures.data();
  out->samplers = sd.samplers.data();
  out->buffers = sd.buffers.data();
  out->uniforms = b.uniforms ? &sd.uniforms : nullptr;
}

}  // namespace

// ---- CPU writes and reads ---------------------------------------------------------------------

void CpuWrote(Resource& r, uint32_t sub, uint64_t lo, uint64_t hi) {
  r.version++;
  if (r.dim == Dim::Buffer) {
    hi = std::min<uint64_t>(hi, r.byte_width);
    if (lo >= hi) return;
    if (r.dirty_hi <= r.dirty_lo) {
      r.dirty_lo = lo;
      r.dirty_hi = hi;
    } else {
      r.dirty_lo = std::min(r.dirty_lo, lo);
      r.dirty_hi = std::max(r.dirty_hi, hi);
    }
    return;
  }
  if (!r.image) return;  // made from the guest memory when first used
  if (sub == ~0u) std::fill(r.dirty.begin(), r.dirty.end(), 1);
  else if (sub < r.dirty.size()) r.dirty[sub] = 1;
}

void GpuSyncToCpu(Device& dev, Resource& r) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (!r.gpu_newer || !dev.gfx) return;
  r.gpu_newer = false;
  if (r.dim == Dim::Buffer) {
    WARN_ONCE("d3d11: buffers the GPU wrote are not read back\n");
    return;
  }
  if (!r.image) {
    WARN_ONCE("d3d11: depth targets are not read back\n");
    return;
  }
  bool block;
  FormatBytes(r.format, &block);
  if (block || ColorFormatOf(DrawnFormat(r)).conv != Conv::None) {
    WARN_ONCE("d3d11: format %u is not read back\n", r.format);
    return;
  }
  for (uint32_t sub = 0; sub < r.Subresources(); sub++) {
    if (r.dirty.size() > sub && r.dirty[sub]) continue;  // the guest's own writes are newer
    const uint32_t mip = sub % r.mips, slice = sub / r.mips;
    uint8_t* data = r.Data() + r.SubOffset(sub);
    const gfx::Rect rc{0, 0, int32_t(r.MipWidth(mip)), int32_t(r.dim == Dim::Texture1D ? 1 : r.MipHeight(mip))};
    if (r.dim == Dim::Texture3D) {
      for (uint32_t z = 0; z < r.MipDepth(mip); z++)
        dev.gfx->ReadImage(r.image, int(mip), int(z), rc, data + uint64_t(z) * r.SlicePitch(mip),
                           r.RowPitch(mip));
    } else {
      dev.gfx->ReadImage(r.image, int(mip), int(slice), rc, data, r.RowPitch(mip));
    }
  }
}

// ---- draws and dispatches -----------------------------------------------------------------------

namespace {

// A draw's arguments the GPU wrote (four words at `at`), its vertex count
// and first vertex times `per`, for a draw of that many vertices for each
// (point patches, geometry shaders): written by a dispatch into the
// device's buffer, which the draw then reads. One buffer does for every
// such draw, as a dispatch submits the draws recorded before it.
gfx::BufferId ScaledArgs(Device& dev, Resource& args, uint32_t at, uint32_t per) {
  if (!dev.scale_args) {
    gfx::ProgramDesc d;
    d.cs.glsl =
        "layout(R_UNIFORMS) uniform U { uvec4 k; };\n"
        "void main() {\n  uint w = r_rwbuf[0].x;\n  r_rw[w] = r_ld(0u, 0u) * k.x;\n"
        "  r_rw[w + 1u] = r_ld(0u, 1u);\n  r_rw[w + 2u] = r_ld(0u, 2u) * k.x;\n  r_rw[w + 3u] = r_ld(0u, 3u);\n}\n";
    d.cs.buffers = 1;
    d.cs.rw_buffers = 1;
    d.cs.uniform_bytes = 16;
    dev.scale_args = dev.gfx->CreateProgram(d);
    dev.scaled_args = dev.gfx->CreateBuffer(16);
    if (!dev.scale_args || !dev.scaled_args) FatalF("d3d11: no program to scale indirect arguments");
  }
  gfx::ProgramDispatch pd;
  pd.program = dev.scale_args;
  const gfx::BufferData in{nullptr, 16, args.gpu_buffer, at};
  pd.cs.buffers = &in;
  const uint32_t k[4] = {per, 0, 0, 0};
  pd.cs.uniforms = k;
  gfx::RwBufferData out;
  out.buffer = dev.scaled_args;
  out.size = 16;
  pd.rw_buffers = &out;
  dev.gfx->DispatchProgram(pd);
  return dev.scaled_args;
}

}  // namespace

void GpuDraw(Device& dev, Context& x, const DrawCall& call) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (!dev.gfx) return;
  dev.stats.draws++;
  // Indirect arguments the GPU wrote are read there, with all of the
  // vertex buffers bound; their first vertex and instance are taken as 0
  // (SV_VertexID and SV_InstanceID count from them).
  DrawCall dc = call;
  if (dc.args && !(dc.args->gpu_newer && dc.args->gpu_buffer)) dc.args = nullptr;
  if (dc.args) dc.first = dc.first_instance = 0, dc.base_vertex = 0;
  else if (!dc.count || !dc.instances) return;
  const Shader* vs = x.stage[0].shader.get();
  // A list of patches of one control point (D3D11_PRIMITIVE_TOPOLOGY_1_
  // CONTROL_POINT_PATCHLIST) with a domain shader: point patches, drawn as
  // their quads' triangles (dxbc.h), when the shaders are those. A point
  // list with a geometry shader: the triangles of its strips, likewise.
  // Both draw patch_vertices vertices for each of the draw's (expand).
  const Shader* ds = x.stage[kDomainStage].shader.get();
  const Shader* gs = x.stage[kGeometryStage].shader.get();
  const bool point_patch = x.topology == 33 && x.hull && ds;
  const bool expand = point_patch || gs;
  std::shared_ptr<const LinkedProgram> prog;
  if (vs && gs) {
    if (x.topology == 1) prog = GeometryProgramFor(dev, *vs, *gs, x.stage[1].shader.get(), x.layout.get());
    if (!prog || !prog->id) {
      static std::set<uint64_t> warned;
      if (warned.insert(gs->hash).second)
        WarnF("d3d11: a draw with geometry shader %016llX (topology %u) is skipped: not points made strips\n",
              (unsigned long long)gs->hash, x.topology);
      dev.stats.skipped++;
      return;
    }
  } else if (vs && point_patch) {
    prog = PointPatchProgramFor(dev, *vs, *x.hull, *ds, x.stage[1].shader.get(), x.layout.get());
  } else if (vs && x.topology != 33) {
    prog = ProgramFor(dev, *vs, x.stage[1].shader.get(), x.layout.get());
  }
  if (!prog || !prog->id) {
    if (vs && x.topology == 33) {
      static std::set<uint64_t> warned;
      if (warned.insert(x.hull ? x.hull->hash : 0).second)
        WarnF("d3d11: a draw of patches (hull shader %016llX, domain %016llX) is skipped: not point patches\n",
              x.hull ? (unsigned long long)x.hull->hash : 0ull, ds ? (unsigned long long)ds->hash : 0ull);
    }
    dev.stats.skipped++;
    return;
  }
  if (dc.args && expand && dc.indexed) {
    WARN_ONCE("d3d11: indexed indirect draws of point patches or geometry shaders, with arguments the GPU "
              "wrote, are skipped\n");
    return;
  }
  bool ok;
  gfx::ProgramDraw d;
  d.program = prog->id;
  d.state = PipelineOf(x, &ok);
  if (expand) {
    d.state.topology = prog->patch_lines ? gfx::Topology::LineList : gfx::Topology::TriangleList;
    ok = true;
  }
  if (!ok) {
    static std::set<uint64_t> warned;
    if (warned.insert(vs->hash).second)
      WarnF("d3d11: a draw with topology %u (vertex shader %016llX) is skipped\n", x.topology,
            (unsigned long long)vs->hash);
    return;
  }
  // Without a pixel shader only depth and stencil are written.
  if (!x.stage[1].shader)
    for (gfx::TargetBlend& b : d.state.blend) b.write_mask = 0;
  // Targets: the render target views from the first on.
  int w = 0, h = 0;
  for (int i = 0; i < gfx::kMaxTargets && x.rtvs[i]; i++) {
    const View& v = *x.rtvs[i];
    Resource& r = *v.res;
    EnsureImage(dev, r);
    d.targets[i] = {r.image, int(v.first_mip), int(v.first_slice)};
    r.gpu_newer = true;
    if (!i) w = int(r.MipWidth(v.first_mip)), h = int(r.MipHeight(v.first_mip));
  }
  if (x.dsv) {
    Resource& r = *x.dsv->res;
    EnsureImage(dev, r);
    d.depth = r.depth_target;
    d.depth_layer = int(x.dsv->first_slice);
    if (!w) w = int(r.width), h = int(r.height);
  }
  if (!d.targets[0].tex && !d.depth) return;
  const ContextState::Viewport& vp = x.viewports[0];
  d.viewport = {uint32_t(std::max(0.0f, vp.x)), uint32_t(std::max(0.0f, vp.y)),
                uint32_t(std::max(0.0f, vp.w)), uint32_t(std::max(0.0f, vp.h)), vp.min_z, vp.max_z};
  const bool scissor = x.rasterizer && Field<uint32_t>(x.rasterizer->desc, 28, 0);
  d.scissor = scissor && x.num_scissors
                  ? gfx::Rect{x.scissors[0][0], x.scissors[0][1], x.scissors[0][2], x.scissors[0][3]}
                  : gfx::Rect{0, 0, w, h};
  // Geometry.
  d.vertex_count = dc.count;
  d.instance_count = dc.instances;
  // An indexed point patch's vertices are indexed on the host: vertex j
  // of patch p is the patch's control point's vertex times the vertices a
  // patch has, plus j (the domain stage divides it back).
  std::vector<uint32_t> patch_indices;
  if (expand && dc.indexed) {
    if (!x.ib) return;
    const Resource& ib = *x.ib;
    const uint32_t isize = x.ib_format == 42 ? 4 : 2, per = prog->patch_vertices;
    const uint64_t at = x.ib_offset + uint64_t(dc.first) * isize;
    if (at >= ib.byte_width) return;
    const uint32_t n = uint32_t(std::min<uint64_t>(dc.count, (ib.byte_width - at) / isize));
    patch_indices.resize(size_t(n) * per);
    for (uint32_t p = 0; p < n; p++) {
      uint32_t v;
      if (isize == 4) std::memcpy(&v, ib.Data() + at + 4ull * p, 4);
      else v = uint32_t(ib.Data()[at + 2ull * p]) | uint32_t(ib.Data()[at + 2ull * p + 1]) << 8;
      v += uint32_t(dc.base_vertex);
      for (uint32_t j = 0; j < per; j++) patch_indices[size_t(p) * per + j] = v * per + j;
    }
    d.index_size = 4;
    d.indices = patch_indices.data();
    d.vertex_count = uint32_t(patch_indices.size());
  } else if (dc.indexed) {
    if (!x.ib) return;
    Resource& ib = *x.ib;
    const uint32_t isize = x.ib_format == 42 ? 4 : 2;
    d.index_size = isize;
    d.base_vertex = dc.base_vertex;
    const uint64_t at = x.ib_offset + uint64_t(dc.first) * isize;
    if (at >= ib.byte_width) return;
    d.vertex_count = uint32_t(std::min<uint64_t>(dc.count, (ib.byte_width - at) / isize));
    if (InPlace(ib) && x.ib_offset % isize == 0 && EnsureGpuBuffer(dev, ib)) {
      d.index_buffer = ib.gpu_buffer;
      d.first_index = uint32_t(at / isize);
    } else if (dc.args) {
      WARN_ONCE("d3d11: indirect draws with arguments the GPU wrote, of indices from the host, are skipped\n");
      return;
    } else {
      d.indices = ib.Data() + at;
    }
  } else if (expand) {
    d.vertex_count = dc.count * prog->patch_vertices;
    d.base_vertex = int32_t(dc.first * prog->patch_vertices);
  } else {
    d.base_vertex = int32_t(dc.first);
  }
  Scratch s;
  StageData vsd, psd;
  vsd.uniforms.draw[0] = dc.indexed ? dc.base_vertex : int32_t(dc.first);
  vsd.uniforms.draw[1] = int32_t(dc.first_instance);
  if (dc.args) {
    d.args = expand ? ScaledArgs(dev, *dc.args, dc.args_at, prog->patch_vertices) : dc.args->gpu_buffer;
    d.args_at = expand ? 0 : dc.args_at;
  }
  BindStage(dev, x, gs ? kGeometryStage : point_patch ? kDomainStage : 0, prog->vs, &dc, vsd, s, &d.vs);
  BindStage(dev, x, 1, prog->ps, &dc, psd, s, &d.fs);
  if (!dev.gfx->DrawProgram(d)) dev.stats.waiting++;
}

namespace {

// A UAV's hidden counter, made (zero) when first used.
gfx::BufferId CounterOf(Device& dev, View& v) {
  if (!v.counter) {
    v.counter = dev.gfx->CreateBuffer(4);
    const uint32_t zero = 0;
    dev.gfx->UpdateBuffer(v.counter, 0, &zero, 4);
  }
  return v.counter;
}

}  // namespace

void GpuSetCounter(Device& dev, View& v, uint32_t count) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (!dev.gfx) return;
  dev.gfx->UpdateBuffer(CounterOf(dev, v), 0, &count, 4);
}

void GpuDispatch(Device& dev, Context& x, uint32_t gx, uint32_t gy, uint32_t gz, Resource* args,
                 uint32_t args_at) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (!dev.gfx) return;
  dev.stats.dispatches++;
  const Shader* cs = x.stage[2].shader.get();
  std::shared_ptr<const LinkedProgram> prog;
  if (cs) prog = ComputeProgramFor(dev, *cs);
  if (!prog || !prog->id) {
    dev.stats.skipped_dispatches++;
    return;
  }
  gfx::ProgramDispatch d;
  d.program = prog->id;
  d.groups[0] = gx, d.groups[1] = gy, d.groups[2] = gz;
  if (args && args->gpu_newer && args->gpu_buffer) {
    d.args = args->gpu_buffer;
    d.args_at = args_at;
  }
  Scratch s;
  StageData sd;
  BindStage(dev, x, 2, prog->cs, nullptr, sd, s, &d.cs);
  // UAVs: buffers stay on the GPU (written back there), as do their
  // hidden counters; textures are storage images.
  for (uint32_t entry : prog->cs.rw_buffers) {
    const uint32_t slot = entry & ~StageBindings::kCounter;
    View* v = slot < 8 ? x.uavs[slot].get() : nullptr;
    gfx::RwBufferData rb;
    if (v && (entry & StageBindings::kCounter)) {
      rb.buffer = CounterOf(dev, *v);
      rb.size = 4;
    } else if (v && v->res->dim == Dim::Buffer) {
      Resource& r = *v->res;
      EnsureGpuBuffer(dev, r);
      const uint32_t eb = ElementBytes(*v);
      rb.buffer = r.gpu_buffer;
      rb.size = std::min<uint32_t>(v->elements * eb, r.byte_width);
      if (v->first_element) WARN_ONCE("d3d11: UAVs of a buffer from an element past its first\n");
      r.gpu_newer = true;
    } else {
      // Nothing bound: zeros, and the writes go to a GPU buffer nothing
      // reads. Written back to the host, they would make the dispatch
      // wait for the GPU to finish all it was given.
      if (!dev.scratch_uav) dev.scratch_uav = dev.gfx->CreateBuffer(16);
      rb.data = const_cast<uint8_t*>(s.Zeros(16));
      rb.buffer = dev.scratch_uav;
      rb.size = 16;
    }
    sd.rw_buffers.push_back(rb);
  }
  for (uint32_t slot : prog->cs.rw_images) {
    const View* v = slot < 8 ? x.uavs[slot].get() : nullptr;
    if (!v || v->res->dim == Dim::Buffer) FatalF("d3d11: Dispatch without the texture UAV in slot %u", slot);
    Resource& r = *v->res;
    EnsureImage(dev, r);
    r.gpu_newer = true;
    sd.rw_images.push_back({r.image, int(v->first_mip), int(v->first_slice)});
  }
  d.rw_buffers = sd.rw_buffers.data();
  d.rw_images = sd.rw_images.data();
  dev.gfx->DispatchProgram(d);
}

// ---- clears ----------------------------------------------------------------------------------

namespace {

// The layers of a view's level: one of them, or all (-1).
std::vector<int> ViewLayers(const View& v) {
  const Resource& r = *v.res;
  const uint32_t layers = r.dim == Dim::Texture3D ? r.MipDepth(v.first_mip) : r.array;
  const uint32_t n = v.slices == 0xFFFFFFFFu ? layers - std::min(layers, v.first_slice) : v.slices;
  if (v.first_slice == 0 && n >= layers) return {-1};
  std::vector<int> out;
  for (uint32_t i = 0; i < n && v.first_slice + i < layers; i++) out.push_back(int(v.first_slice + i));
  return out;
}

}  // namespace

void GpuClearTarget(Device& dev, const View& v, const float color[4]) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (!dev.gfx) return;
  dev.stats.clears++;
  Resource& r = *v.res;
  EnsureImage(dev, r);
  for (int layer : ViewLayers(v))
    dev.gfx->ClearImage(r.image, int(v.first_mip), {color[0], color[1], color[2], color[3]}, layer);
  r.gpu_newer = true;
}

// flags: D3D11_CLEAR_DEPTH 1, D3D11_CLEAR_STENCIL 2.
void GpuClearDepth(Device& dev, const View& v, uint32_t flags, float z, uint8_t stencil) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (!dev.gfx || !(flags & 3)) return;
  dev.stats.clears++;
  Resource& r = *v.res;
  EnsureImage(dev, r);
  const float depth = flags & 1 ? z : std::nanf("");
  const int st = flags & 2 ? int(stencil) : -1;
  const uint32_t layers = r.array > 1 || r.srv_dim == 5 ? r.array : 1;
  const uint32_t n = std::min(v.slices, layers - std::min(layers, v.first_slice));
  for (uint32_t i = 0; i < std::max(n, 1u) && v.first_slice + i < layers; i++)
    dev.gfx->ClearDepth(r.depth_target, depth, st, int(v.first_slice + i));
  r.gpu_newer = true;
}

// A buffer UAV's elements set to values[0] (raw and structured views,
// and typed ones of 32-bit formats; others take the value per 32 bits);
// a texture UAV's level cleared to the values.
void GpuClearUav(Device& dev, const View& v, const uint32_t values[4], bool floats) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (!dev.gfx) return;
  dev.stats.clears++;
  Resource& r = *v.res;
  if (r.dim == Dim::Buffer) {
    const uint32_t eb = ElementBytes(v);
    const uint64_t lo = uint64_t(v.first_element) * eb;
    const uint64_t hi = std::min<uint64_t>(lo + uint64_t(v.elements) * eb, r.byte_width);
    uint8_t* p = r.Data();
    for (uint64_t a = lo; a + 4 <= hi; a += 4) std::memcpy(p + a, &values[0], 4);
    CpuWrote(r, 0, lo, hi);
    if (r.gpu_buffer) EnsureGpuBuffer(dev, r);
    return;
  }
  EnsureImage(dev, r);
  if (!r.image) return;
  gfx::Color c;
  float* f = &c.r;
  for (int i = 0; i < 4; i++) {
    if (floats) std::memcpy(&f[i], &values[i], 4);
    else f[i] = float(values[i]);
  }
  for (int layer : ViewLayers(v)) dev.gfx->ClearImage(r.image, int(v.first_mip), c, layer);
  r.gpu_newer = true;
}

void GpuGenerateMips(Device& dev, const View& v) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (!dev.gfx || v.res->dim == Dim::Buffer) return;
  Resource& r = *v.res;
  EnsureImage(dev, r);
  if (!r.image) return;
  dev.gfx->GenerateMips(r.image);
  r.gpu_newer = true;
}

// ---- copies and updates --------------------------------------------------------------------------

namespace {

// Whether a copy involving r must be made on the GPU: its newest
// contents are there.
bool OnGpu(const Resource& r) { return (r.image || r.depth_target) && r.gpu_newer; }

// A subresource region copy in guest memory (rows of whole texels or
// blocks).
void CpuCopyRegion(Resource& d, uint32_t dsub, uint32_t dx, uint32_t dy, uint32_t dz, Resource& s,
                   uint32_t ssub, const uint32_t* box) {
  if (dsub >= d.Subresources() || ssub >= s.Subresources()) return;
  const uint32_t smip = ssub % s.mips, dmip = dsub % d.mips;
  bool block, dblock;
  const uint32_t bpp = FormatBytes(s.format, &block), div = block ? 4 : 1;
  // Direct3D copies only between formats of the same size (one typeless
  // family); it drops anything else.
  if (FormatBytes(d.format, &dblock) != bpp || dblock != block) return;
  uint32_t x0 = 0, y0 = 0, z0 = 0, x1 = s.MipWidth(smip), y1 = s.MipHeight(smip),
           z1 = s.MipDepth(smip);
  if (box) x0 = box[0], y0 = box[1], z0 = box[2], x1 = box[3], y1 = box[4], z1 = box[5];
  // No further than the destination level reaches.
  const uint32_t dw = d.MipWidth(dmip), dh = d.MipHeight(dmip), dd = d.MipDepth(dmip);
  if (dx >= dw || dy >= dh || dz >= dd) return;
  x1 = std::min(x1, x0 + (dw - dx));
  y1 = std::min(y1, y0 + (dh - dy));
  z1 = std::min(z1, z0 + (dd - dz));
  if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
  const uint32_t rows = (y1 - y0 + div - 1) / div, row_bytes = (x1 - x0 + div - 1) / div * bpp;
  uint8_t* dst = d.Data() + d.SubOffset(dsub);
  const uint8_t* src = s.Data() + s.SubOffset(ssub);
  for (uint32_t z = 0; z < z1 - z0; z++)
    for (uint32_t y = 0; y < rows; y++)
      std::memcpy(dst + uint64_t(dz + z) * d.SlicePitch(dmip) +
                      uint64_t(dy / div + y) * d.RowPitch(dmip) + dx / div * bpp,
                  src + uint64_t(z0 + z) * s.SlicePitch(smip) +
                      uint64_t(y0 / div + y) * s.RowPitch(smip) + x0 / div * bpp,
                  row_bytes);
}

}  // namespace

void GpuCopy(Device& dev, Resource& d, uint32_t dsub, uint32_t dx, uint32_t dy, uint32_t dz,
             Resource& s, uint32_t ssub, const uint32_t* box) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  // Every caller's subresources within the resources (a buffer has one).
  if (dsub >= d.Subresources() || ssub >= s.Subresources()) return;
  dev.stats.copies++;
  if (d.dim == Dim::Buffer) {
    GpuSyncToCpu(dev, s);
    uint32_t lo = box ? box[0] : 0, hi = box ? box[3] : s.byte_width;
    hi = std::min(hi, s.byte_width);
    if (lo >= hi || dx >= d.byte_width) return;
    const uint32_t n = std::min(hi - lo, d.byte_width - dx);
    std::memmove(d.Data() + dx, s.Data() + lo, n);
    CpuWrote(d, 0, dx, dx + uint64_t(n));
    return;
  }
  if (dev.gfx && (OnGpu(s) || OnGpu(d)) && d.usage != Usage::Staging) {
    if (IsDepth(s) || IsDepth(d)) {
      if (!IsDepth(s) || !IsDepth(d)) FatalF("d3d11: a copy between depth and colour textures");
      EnsureImage(dev, s);
      EnsureImage(dev, d);
      const uint32_t x0 = box ? box[0] : 0, y0 = box ? box[1] : 0;
      if (x0 != dx || y0 != dy) WARN_ONCE("d3d11: depth copies to another place are made in place\n");
      dev.gfx->CopyDepth(s.depth_target, d.depth_target,
                         box ? gfx::Rect{int(box[0]), int(box[1]), int(box[3]), int(box[4])}
                             : gfx::Rect{0, 0, int(s.width), int(s.height)});
      d.gpu_newer = true;
      return;
    }
    EnsureImage(dev, s);
    EnsureImage(dev, d);
    const uint32_t smip = ssub % s.mips, dmip = dsub % d.mips;
    uint32_t x0 = 0, y0 = 0, z0 = 0, x1 = s.MipWidth(smip),
             y1 = s.dim == Dim::Texture1D ? 1 : s.MipHeight(smip), z1 = s.MipDepth(smip);
    if (box) x0 = box[0], y0 = box[1], z0 = box[2], x1 = box[3], y1 = box[4], z1 = box[5];
    const gfx::Rect rc{int(x0), int(y0), int(x1), int(y1)};
    for (uint32_t z = z0; z < z1; z++) {
      const uint32_t sl = s.dim == Dim::Texture3D ? z : ssub / s.mips;
      const uint32_t dl = d.dim == Dim::Texture3D ? dz + (z - z0) : dsub / d.mips;
      dev.gfx->CopyImage(s.image, int(smip), int(sl), rc, d.image, int(dmip), int(dl), int(dx),
                         int(dy));
    }
    d.gpu_newer = true;
    return;
  }
  GpuSyncToCpu(dev, s);
  CpuCopyRegion(d, dsub, dx, dy, dz, s, ssub, box);
  CpuWrote(d, dsub);
}

void GpuCopyResource(Device& dev, Resource& d, Resource& s) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (d.dim == Dim::Buffer) {
    GpuCopy(dev, d, 0, 0, 0, 0, s, 0, nullptr);
    return;
  }
  if (dev.gfx && (OnGpu(s) || OnGpu(d)) && d.usage != Usage::Staging) {
    if (IsDepth(s) || IsDepth(d)) {
      GpuCopy(dev, d, 0, 0, 0, 0, s, 0, nullptr);
      return;
    }
    for (uint32_t sub = 0; sub < std::min(s.Subresources(), d.Subresources()); sub++)
      GpuCopy(dev, d, sub, 0, 0, 0, s, sub, nullptr);
    return;
  }
  dev.stats.copies++;
  GpuSyncToCpu(dev, s);
  std::memcpy(d.Data(), s.Data(), std::min(d.size, s.size));
  CpuWrote(d, ~0u, 0, d.size);
}

void GpuUpdate(Device& dev, Resource& r, uint32_t sub, const uint32_t* box, const uint8_t* src,
               uint32_t pitch, uint32_t slice_pitch) {
  std::lock_guard<std::recursive_mutex> l(dev.gpu_mu);
  if (r.dim == Dim::Buffer) {
    const uint32_t lo = box ? box[0] : 0, hi = std::min(box ? box[3] : r.byte_width, r.byte_width);
    if (lo >= hi) return;
    std::memcpy(r.Data() + lo, src, hi - lo);
    CpuWrote(r, 0, lo, hi);
    return;
  }
  if (IsDepth(r)) {
    WARN_ONCE("d3d11: UpdateSubresource of a depth texture is not drawn\n");
    return;
  }
  const uint32_t mip = sub % r.mips;
  bool block;
  const uint32_t bpp = FormatBytes(r.format, &block), div = block ? 4 : 1;
  uint32_t x0 = 0, y0 = 0, z0 = 0, x1 = r.MipWidth(mip), y1 = r.MipHeight(mip), z1 = r.MipDepth(mip);
  if (box) x0 = box[0], y0 = box[1], z0 = box[2], x1 = box[3], y1 = box[4], z1 = box[5];
  if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
  const uint32_t rows = (y1 - y0 + div - 1) / div, row_bytes = (x1 - x0 + div - 1) / div * bpp;
  uint8_t* dst = r.Data() + r.SubOffset(sub);
  for (uint32_t z = z0; z < z1; z++)
    for (uint32_t y = 0; y < rows; y++)
      std::memcpy(dst + uint64_t(z) * r.SlicePitch(mip) + uint64_t(y0 / div + y) * r.RowPitch(mip) +
                      x0 / div * bpp,
                  src + uint64_t(z - z0) * slice_pitch + uint64_t(y) * pitch, row_bytes);
  // A texture on the GPU gets the box there (when the GPU's copy is the
  // newest, the rest of the guest memory stays behind it).
  if (r.image && dev.gfx) {
    const gfx::Rect rc{int(x0), int(y0), int(x1), int(r.dim == Dim::Texture1D ? 1 : y1)};
    for (uint32_t z = z0; z < z1; z++)
      UploadRect(dev, r, mip, r.dim == Dim::Texture3D ? z : sub / r.mips, rc,
                 src + uint64_t(z - z0) * slice_pitch, pitch);
    r.version++;
    return;
  }
  CpuWrote(r, sub);
}

// ---- Present ---------------------------------------------------------------------------------------

void GpuPresent(Device& dev, Resource& bb, int w, int h) {
  std::unique_lock<std::recursive_mutex> l(dev.gpu_mu);
  if (!dev.gfx) return;
  EnsureImage(dev, bb);
  dev.gfx->SetRenderTarget(0, 0);
  dev.gfx->DrawImage(bb.image, {0, 0, w, h});
  const Device::Stats s = dev.stats;
  const size_t linked = dev.linked.size();
  dev.stats = {};
  const uint64_t frame = dev.frames++;
  // The window system is serviced (guest window procedures may run)
  // without the device held.
  l.unlock();
  PresentFrame(*dev.gfx, w, h);
  if (g_trace || frame < 5 || frame % 60 == 0)
    std::fprintf(stderr,
                 "d3d11: frame %llu: %u draws (%u skipped: no program, %u waiting for shaders), "
                 "%u dispatches (%u skipped), %u clears, %u copies, %u command lists, %u programs "
                 "linked (%zu in all)\n",
                 (unsigned long long)frame, s.draws, s.skipped, s.waiting, s.dispatches,
                 s.skipped_dispatches, s.clears, s.copies, s.executed_lists, s.linked, linked);
}

}  // namespace recomp::hle::d3d11
