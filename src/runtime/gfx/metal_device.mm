// gfx::Device on Metal (Apple GPUs: macOS and iPadOS), for Direct3D 9.
//
// Only the programmable pipeline: shaders translated from HLSL
// (runtime/hlsl), then SPIR-V and MSL as for SDL_GPU, with the same
// prelude and Metal binding layout as gpu_device.cpp. Fixed-function draws
// are not provided (HasFixedFunction false: the d3d9 front end draws them
// with its stand-in shaders), nor are DirectDraw blits.
//
// The game's thread (its render thread) records; an encoding thread of the
// device's own does every Metal command-buffer and encoder call, from
// closures recorded in order (Rec), so encoding, pipeline compiles and
// waiting for a drawable leave the game's thread. The frame is one command
// buffer, from Present to Present (or until a readback needs the GPU's
// result). A render target change ends one render
// encoder and the next draw starts another; a clear of a whole target
// before anything is drawn is the encoder's load action. Per-draw data
// (vertices, indices, constants) is written once, from guest memory, into
// a bump arena of shared buffers per command buffer, reused when that
// command buffer has finished (the buffers made ahead, off the game's
// thread); vertex and index data in the memory the front end shares with
// the GPU (MapSharedMemory) is not copied at all. Texture uploads go to a blit encoder in a
// command buffer queued before the frame's, so they never split a render
// pass; only an upload to a texture the frame has already drawn with or
// into is encoded in order in the frame itself.
//
// Formats as in gpu_device.cpp: colour textures and targets B8G8R8A8 (other
// D3D colour formats converted on upload), DXT as BC1-3, float formats as
// is. Depth: D16 as Depth16Unorm, 24-bit depth as 32-bit float (Apple GPUs
// have no 24-bit depth), with stencil as Depth32Float_Stencil8; float depth
// stored reversed (1 - d, gfx/reversed_z.h) unless RECOMP_REVERSED_Z=0.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>
#import <MetalFX/MetalFX.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <TargetConditionals.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <thread>
#include <deque>
#include <condition_variable>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "runtime/gfx/device.h"
// kSmaaHlsl: SMAA.hlsl (smaa/, MIT), and its lookup textures.
#include "runtime/gfx/smaa/AreaTex.h"
#include "runtime/gfx/smaa/SearchTex.h"
#include "smaa_hlsl.h"
#include "runtime/gfx/metal_prelude.h"
#include "runtime/gfx/reversed_z.h"
#include "runtime/gfx/shader_compile.h"
#include "runtime/log.h"

// RECOMP_DISPLAY_LINK=1 (macOS 14+): the display hands out each drawable
// in time for its next refresh (CAMetalDisplayLink, Apple's frame pacing
// for games, variable refresh included); Show takes the latest one instead
// of asking the layer (nextDrawable) whenever a frame is done. A frame is
// then shown on the refresh it was made for: no uneven gaps without vsync,
// no queue of finished frames waiting (input lag) with it.
struct RecompLinkSlot {
  std::mutex mu;
  std::condition_variable cv;
  id<CAMetalDrawable> drawable = nil;
};

API_AVAILABLE(macos(14.0))
@interface RecompDisplayLinkTarget : NSObject <CAMetalDisplayLinkDelegate>
@property(nonatomic, assign) RecompLinkSlot* slot;
@end

@implementation RecompDisplayLinkTarget
- (void)metalDisplayLink:(CAMetalDisplayLink*)link needsUpdate:(CAMetalDisplayLinkUpdate*)update {
  (void)link;
  {
    std::lock_guard<std::mutex> l(_slot->mu);
    _slot->drawable = update.drawable;  // the one before, unused, goes back
  }
  _slot->cv.notify_one();
}
@end

namespace recomp::gfx {

namespace {

float AsFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void Argb(float* out, uint32_t c) {
  out[0] = float((c >> 16) & 0xFF) / 255.0f;
  out[1] = float((c >> 8) & 0xFF) / 255.0f;
  out[2] = float(c & 0xFF) / 255.0f;
  out[3] = float(c >> 24) / 255.0f;
}

// ---- D3D enums -> Metal

MTLBlendFactor BlendFactor(uint32_t d3d, bool target_alpha) {
  switch (d3d) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    // A target without alpha (X8R8G8B8, R5G6B5) reads alpha as 1.
    case 7: return target_alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorOne;
    case 8: return target_alpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorZero;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
  }
}

MTLBlendOperation BlendOp(uint32_t d3d) {
  switch (d3d) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
  }
}

// D3DCMPFUNC (NEVER 1 .. ALWAYS 8) and D3DSTENCILOP (KEEP 1 .. DECR 8) are
// Metal's numbering plus one.
MTLCompareFunction CompareOp(uint32_t d3d) {
  return d3d >= 1 && d3d <= 8 ? MTLCompareFunction(d3d - 1) : MTLCompareFunctionAlways;
}
MTLStencilOperation StencilOp(uint32_t d3d) {
  return d3d >= 1 && d3d <= 8 ? MTLStencilOperation(d3d - 1) : MTLStencilOperationKeep;
}

MTLSamplerAddressMode AddressMode(uint32_t d3d) {
  switch (d3d) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;  // MIRRORONCE
    default: return MTLSamplerAddressModeRepeat;
  }
}

// COLORWRITEENABLE (red 1, green 2, blue 4, alpha 8) in Metal's bit order
// (red 8, green 4, blue 2, alpha 1).
uint8_t WriteMask(uint32_t d3d) {
  return uint8_t(((d3d & 1) << 3) | ((d3d & 2) << 1) | ((d3d & 4) >> 1) | ((d3d & 8) >> 3));
}

bool HasAlpha(Format f) {
  switch (f) {
    case Format::A8R8G8B8: case Format::A1R5G5B5: case Format::A4R4G4B4:
    case Format::A8: case Format::A8L8: case Format::DXT1: case Format::DXT2:
    case Format::DXT3: case Format::DXT4: case Format::DXT5:
    case Format::A16B16G16R16F: case Format::A32B32G32R32F:
      return true;
    default:
      return false;
  }
}

MTLPixelFormat PixelFormat(Format f) {
  switch (f) {
    case Format::DXT1: return MTLPixelFormatBC1_RGBA;
    case Format::DXT2: case Format::DXT3: return MTLPixelFormatBC2_RGBA;
    case Format::DXT4: case Format::DXT5: return MTLPixelFormatBC3_RGBA;
    case Format::R16F: return MTLPixelFormatR16Float;
    case Format::G16R16F: return MTLPixelFormatRG16Float;
    case Format::A16B16G16R16F: return MTLPixelFormatRGBA16Float;
    case Format::R32F: return MTLPixelFormatR32Float;
    case Format::G32R32F: return MTLPixelFormatRG32Float;
    case Format::A32B32G32R32F: return MTLPixelFormatRGBA32Float;
    default: return MTLPixelFormatBGRA8Unorm;
  }
}

void ConvertToArgb(const uint8_t* src, uint32_t pitch, Format f, int w, int h, uint32_t* out) {
  uint32_t bpp = BitsPerPixel(f) / 8;
  for (int y = 0; y < h; y++) {
    const uint8_t* s = src + size_t(y) * pitch;
    uint32_t* d = out + size_t(y) * uint32_t(w);
    if (f == Format::A8R8G8B8) {
      std::memcpy(d, s, size_t(w) * 4);
    } else if (f == Format::X8R8G8B8) {
      const auto* p = reinterpret_cast<const uint32_t*>(s);
      for (int x = 0; x < w; x++) d[x] = p[x] | 0xFF000000u;
    } else {
      for (int x = 0; x < w; x++) d[x] = DecodePixel(s + size_t(x) * bpp, f);
    }
  }
}

Rect Intersect(const Rect& a, const Rect& b) {
  return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
          std::min(a.bottom, b.bottom)};
}
Rect Clip(const Rect& r, int w, int h) { return Intersect(r, {0, 0, w, h}); }
bool Empty(const Rect& r) { return r.right <= r.left || r.bottom <= r.top; }

// ---- cache keys (compared and hashed as bytes: no padding)

// A render pipeline: shaders, the pass's attachment formats, blending and
// write masks (Metal enums).
struct PipeKey {
  uint32_t vs, ps;
  uint16_t color[4];  // MTLPixelFormat per colour target (0: none)
  uint16_t depth, stencil;
  uint8_t blend, src, dst, op, src_a, dst_a, op_a;
  uint8_t mask[4];
  uint8_t lean;  // kLean*: the shaders' lean variants
  bool operator==(const PipeKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(PipeKey) == 32, "no padding");
// PipeKey::lean: a shader made without what the draw does not use.
constexpr uint8_t kLeanPs = 1;  // the pixel shader without the alpha test

struct DepthKey {
  uint8_t test, write, func, stencil;
  uint8_t fail, zfail, pass, sfunc;     // front faces (D3D's clockwise ones)
  uint8_t bfail, bzfail, bpass, bfunc;  // back faces (TWOSIDEDSTENCILMODE's CCW ops)
  uint8_t read, wmask, pad[2];
  bool operator==(const DepthKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(DepthKey) == 16, "no padding");

struct SamplerKey {
  uint8_t min, mag, mip, u, v, w, aniso, border;  // mip 0xFF: MIPFILTER NONE
  float min_lod;
  bool operator==(const SamplerKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(SamplerKey) == 12, "no padding");

template <typename K>
struct BytesHash {
  size_t operator()(const K& k) const {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto* b = reinterpret_cast<const uint8_t*>(&k);
    for (size_t i = 0; i < sizeof k; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return size_t(h);
  }
};

// The device's own shaders: clears and textured quads (StretchRect, the
// window).
constexpr const char* kInternalMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct ClearOut { float4 pos [[position]]; };
// A triangle over the whole target at depth p.x (the scissor bounds it).
vertex ClearOut clear_vs(uint vid [[vertex_id]], constant float4& p [[buffer(0)]]) {
  float2 t = float2((vid << 1) & 2, vid & 2);
  ClearOut o;
  o.pos = float4(t * 2.0 - 1.0, p.x, 1.0);
  return o;
}
fragment float4 clear_fs(constant float4& c [[buffer(0)]]) { return c; }
struct QuadOut { float4 pos [[position]]; float2 uv; };
// A strip of four: r[0] the rectangle in clip space (x0, y0, x1, y1),
// r[1] the source in texture coordinates, r[2].x the source level.
vertex QuadOut quad_vs(uint vid [[vertex_id]], constant float4* r [[buffer(0)]]) {
  float2 t = float2(vid & 1, vid >> 1);
  QuadOut o;
  o.pos = float4(mix(r[0].xy, r[0].zw, t), 0.0, 1.0);
  o.uv = mix(r[1].xy, r[1].zw, t);
  return o;
}
fragment float4 quad_fs(QuadOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                        sampler s [[sampler(0)]], constant float4* r [[buffer(0)]]) {
  return tex.sample(s, in.uv, level(r[2].x));
}
// A triangle over the whole target; uv 0..1 from the top left.
struct TriOut { float4 pos [[position]]; float2 uv; };
vertex TriOut tri_vs(uint vid [[vertex_id]]) {
  float2 t = float2((vid << 1) & 2, vid & 2);
  TriOut o;
  o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
  o.uv = float2(t.x, 1.0 - t.y);
  return o;
}
// A depth buffer copied to one of another size (nearest).
struct DepthOut { float d [[depth(any)]]; };
fragment DepthOut depth_copy_fs(TriOut in [[stage_in]], depth2d<float> src [[texture(0)]],
                                sampler s [[sampler(0)]]) {
  DepthOut o;
  o.d = src.sample(s, in.uv);
  return o;
}
// Motion from the camera alone: each pixel's point (from its depth) where
// the last frame's camera saw it, in texture coordinates (previous - now).
// m: this frame's clip space (the depth as stored) to the last frame's.
// The depth is copied in the same pass (MetalFX's depth input: the
// scene's own may have stencil).
struct MotionOut { float2 mv [[color(0)]]; float d [[depth(any)]]; };
fragment MotionOut motion_fs(TriOut in [[stage_in]], depth2d<float> depth [[texture(0)]],
                             constant float4x4& m [[buffer(0)]]) {
  float d = depth.read(uint2(in.pos.xy));
  float4 prev = m * float4(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0, d, 1.0);
  float2 p = prev.xy / prev.w;
  MotionOut o;
  o.mv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5) - in.uv;
  o.d = d;
  return o;
}

// Temporal antialiasing (RECOMP_UPSCALER=taa), one pass at the back
// buffer's size. The history is reprojected by the camera (motion from the
// nearest depth around the pixel, so edges move with their foreground),
// read through a Catmull-Rom filter (five bilinear taps, it stays sharp
// when it moves), and clipped toward the mean of this frame's 3x3
// neighbourhood in YCoCg (variance clipping: what moves on its own, cars,
// people, rain, swaying trees, has no motion here and must not ghost).
// The more the content moves, the more of this frame goes in. This
// frame's colour is reconstructed at the pixel's centre from its 3x3
// neighbourhood (each sample where the jitter put it, Blackman-Harris
// weights as Karis's UE4 TAA), not taken where the jitter left it: that
// offset, accumulated, is what blurs a plain TAA.
struct TaaParams {
  float4x4 m;      // this frame's clip space to the last frame's
  float2 inv_size;
  float2 jitter;   // the scene's shift this frame, pixels (x right, y down)
  float w_min, w_max;  // this frame's weight, still and moving
  float w_per_px;      // the weight's rise per pixel of motion
  float gamma;         // the clip box's half size, in standard deviations
  uint reset;
  uint direct;     // write the result to `out` too (no sharpening after)
  uint reversed;   // depth stored as 1 - d (m takes it so): the nearest is the largest
};
half3 rgb_to_ycocg(half3 c) {
  return half3(0.25h * (c.r + 2.0h * c.g + c.b), 0.5h * (c.r - c.b), 0.25h * (2.0h * c.g - c.r - c.b));
}
half3 ycocg_to_rgb(half3 c) { return half3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
float3 taa_history(texture2d<half> h, sampler s, float2 uv, float2 size, float2 inv_size) {
  const float2 t = floor(uv * size - 0.5) + 0.5, f = uv * size - t;
  const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
  const float2 w12 = w1 + w2;
  const float2 p0 = (t - 1.0) * inv_size, p3 = (t + 2.0) * inv_size, p12 = (t + w2 / w12) * inv_size;
  const float a = w12.x * w0.y, b = w0.x * w12.y, c = w12.x * w12.y, d = w3.x * w12.y, e = w12.x * w3.y;
  const float3 r = float3(h.sample(s, float2(p12.x, p0.y)).rgb) * a + float3(h.sample(s, float2(p0.x, p12.y)).rgb) * b +
                   float3(h.sample(s, p12).rgb) * c + float3(h.sample(s, float2(p3.x, p12.y)).rgb) * d +
                   float3(h.sample(s, float2(p12.x, p3.y)).rgb) * e;
  return max(r / (a + b + c + d + e), 0.0);
}
kernel void taa_cs(texture2d<half, access::read> color [[texture(0)]],
                   depth2d<float, access::read> depth [[texture(1)]],
                   texture2d<half> history [[texture(2)]],
                   texture2d<half, access::write> history_out [[texture(3)]],
                   texture2d<half, access::write> out [[texture(4)]],
                   constant TaaParams& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
  const uint2 size = uint2(color.get_width(), color.get_height());
  if (gid.x >= size.x || gid.y >= size.y) return;
  const int2 last = int2(size) - 1;
  half3 mean = 0.0h, sq = 0.0h;
  float3 filtered = 0.0;
  float wsum = 0.0;
  for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++) {
      const half3 c = rgb_to_ycocg(color.read(uint2(clamp(int2(gid) + int2(x, y), 0, last))).rgb);
      // The neighbour's sample lies at its centre less the jitter.
      const float2 d = float2(x, y) - p.jitter;
      const float w = exp(-2.29 * dot(d, d));
      filtered += float3(c) * w;
      wsum += w;
      mean += c;
      sq += c * c;
    }
  const half3 cur = half3(filtered / wsum);
  half3 res = cur;
  if (p.reset == 0) {
    // The nearest of the pixel and its diagonal neighbours.
    int2 q = int2(gid);
    float d = depth.read(gid);
    for (int k = 0; k < 4; k++) {
      const int2 n = clamp(int2(gid) + int2(k & 1 ? 1 : -1, k & 2 ? 1 : -1), 0, last);
      const float dn = depth.read(uint2(n));
      if (p.reversed ? dn > d : dn < d) { d = dn; q = n; }
    }
    const float2 uq = (float2(q) + 0.5) * p.inv_size;
    const float4 prev = p.m * float4(uq.x * 2.0 - 1.0, 1.0 - uq.y * 2.0, d, 1.0);
    const float2 mv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5) - uq;
    const float2 uv = (float2(gid) + 0.5) * p.inv_size + mv;
    if (prev.w > 0.0 && all(uv >= 0.0) && all(uv <= 1.0)) {
      constexpr sampler s(filter::linear, address::clamp_to_edge);
      mean /= 9.0h;
      const half3 sigma = sqrt(max(sq / 9.0h - mean * mean, 0.0h)) * half(p.gamma) + 1.0e-3h;
      half3 h = rgb_to_ycocg(half3(taa_history(history, s, uv, float2(size), p.inv_size)));
      const half3 off = h - mean, ext = abs(off) / sigma;
      h = mean + off / max(1.0h, max(ext.x, max(ext.y, ext.z)));
      const float moved = length(mv * float2(size));
      res = mix(h, cur, half(mix(p.w_min, p.w_max, saturate(moved * p.w_per_px))));
    }
  }
  const half4 o = half4(max(ycocg_to_rgb(res), 0.0h), 1.0h);
  history_out.write(o, gid);
  if (p.direct != 0) out.write(o, gid);
}

// Sharpening after TAA, contrast-adaptive (after AMD's CAS): the cross of
// neighbours pulled away from the centre, less where the local contrast
// is already high (no halos on edges). sharpness 0..1.
kernel void taa_sharpen_cs(texture2d<half, access::read> src [[texture(0)]],
                           texture2d<half, access::write> out [[texture(1)]],
                           constant float& sharpness [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
  const uint2 size = uint2(src.get_width(), src.get_height());
  if (gid.x >= size.x || gid.y >= size.y) return;
  const int2 last = int2(size) - 1, g = int2(gid);
  const half3 e = src.read(gid).rgb;
  const half3 b = src.read(uint2(clamp(g + int2(0, -1), 0, last))).rgb;
  const half3 d = src.read(uint2(clamp(g + int2(-1, 0), 0, last))).rgb;
  const half3 f = src.read(uint2(clamp(g + int2(1, 0), 0, last))).rgb;
  const half3 h = src.read(uint2(clamp(g + int2(0, 1), 0, last))).rgb;
  const half3 mn = min(e, min(min(b, d), min(f, h))), mx = max(e, max(max(b, d), max(f, h)));
  const half3 amp = sqrt(saturate(min(mn, 2.0h - mx) / max(mx, 1.0e-4h)));
  const half3 w = amp * half(-1.0f / mix(8.0f, 5.0f, sharpness));
  out.write(half4(saturate((e + (b + d + f + h) * w) / (1.0h + 4.0h * w)), 1.0h), gid);
}
)";

// SDL's view calls are AppKit calls and belong on the main thread; a device
// made on another thread has them run there (the runtime keeps the main
// thread pumping events while such a thread drives a window).
template <class F>
void OnMainThread(F&& f) {
  if (SDL_IsMainThread()) return f();
  auto call = [](void* p) { (*static_cast<std::remove_reference_t<F>*>(p))(); };
  if (!SDL_RunOnMainThread(call, &f, true)) WarnF("gfx: SDL_RunOnMainThread: %s\n", SDL_GetError());
}

constexpr uint32_t kClearVs = 0x80000001, kClearPs = 0x80000002;
constexpr uint32_t kQuadVs = 0x80000003, kQuadPs = 0x80000004;

class MetalDevice final : public Device {
 public:
  explicit MetalDevice(SDL_Window* w) : window_(w) {}

  bool Init() {
    @autoreleasepool {
      dev_ = MTLCreateSystemDefaultDevice();
      if (!dev_) {
        WarnF("gfx: no Metal device\n");
        return false;
      }
      queue_ = [dev_ newCommandQueue];
      OnMainThread([&] {
        view_ = SDL_Metal_CreateView(window_);
        if (view_) layer_ = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view_);
      });
      if (!view_) {
        WarnF("gfx: SDL_Metal_CreateView: %s\n", SDL_GetError());
        return false;
      }
      layer_.device = dev_;
      layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
      layer_.framebufferOnly = YES;
      // RECOMP_DRAWABLES=2|3: the layer's queue of drawables (3 by
      // default). With vsync, every queued drawable is a refresh of input
      // lag; 2 keeps it short.
      if (const char* e = std::getenv("RECOMP_DRAWABLES"))
        layer_.maximumDrawableCount = std::clamp(std::atoi(e), 2, 3);
      else
        layer_.maximumDrawableCount = 3;
      // Constant buffers' offset alignment: 256 bytes on Mac GPUs, less on
      // Apple's.
      uniform_align_ = [dev_ supportsFamily:MTLGPUFamilyApple1] ? 16 : 256;
#if TARGET_OS_IOS || TARGET_OS_TV
      if (@available(iOS 16.4, tvOS 16.4, *)) bc_ = dev_.supportsBCTextureCompression;
      if (!bc_) WarnF("gfx: no BC sampling on this GPU: DXT decoded on upload\n");
#endif
      // RECOMP_DISPLAY_LINK=1 (macOS 14+); off by default: with variable
      // refresh, frames presented as soon as they are done (nextDrawable)
      // played smoother than snapped to the link's refreshes, which showed
      // as constant spikes at 70-100 fps. Not for hidden windows.
      {
        const char* e = std::getenv("RECOMP_DISPLAY_LINK");
        const char* hidden = std::getenv("GTAIV_HIDDEN");
        if (e && *e == '1' && !(hidden && *hidden == '1')) StartDisplayLink();
      }
      // RECOMP_FRAMES_IN_FLIGHT=1..kSlots: how many frames the CPU may
      // record ahead of the GPU (kSlots by default). Fewer: less latency
      // between input and picture, more: more overlap.
      const char* pp = std::getenv("RECOMP_PACE_PRESENT");
      pace_present_ = pp && *pp == '1';
      int in_flight = kSlots;
      if (const char* e = std::getenv("RECOMP_FRAMES_IN_FLIGHT"))
        in_flight = std::clamp(std::atoi(e), 1, int(kSlots));
      frames_free_ = dispatch_semaphore_create(in_flight);
      // One arena chunk per frame slot, made ahead: a slot's first frame
      // takes one, and EndFrame makes another for a slot that fills past 3/4
      // (a chunk a device makes is resident for its lifetime).
      MakeSpareChunks(kSlots);

      NSError* err = nil;
      id<MTLLibrary> lib = [dev_ newLibraryWithSource:@(kInternalMsl) options:nil error:&err];
      if (!lib) FatalF("gfx: the device's own shaders: %s", err.localizedDescription.UTF8String);
      internal_lib_ = lib;
      LoadRecipes();
      LoadMslCache();
      MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
      dd.depthCompareFunction = MTLCompareFunctionAlways;
      dd.depthWriteEnabled = YES;
      depth_copy_ds_ = [dev_ newDepthStencilStateWithDescriptor:dd];
      // RECOMP_UPSCALER=metalfx: MetalFX's temporal antialiasing of the
      // scene (before the HUD); metalfx-spatial: SMAA at the internal
      // resolution, then MetalFX's spatial upscale (no history, a fraction
      // of the temporal one's cost); taa: this device's own (taa_cs), at the
      // back buffer's size only. RECOMP_RENDER_SCALE (0.25-1, default 1)
      // draws the scene smaller and upscales it (metalfx, metalfx-spatial
      // or bilinear).
      if (const char* u = std::getenv("RECOMP_UPSCALER")) {
        if (!std::strcmp(u, "metalfx")) upscaler_ = Upscaler::MetalFX;
        else if (!std::strcmp(u, "taa")) upscaler_ = Upscaler::Taa;
        else if (!std::strcmp(u, "smaa")) upscaler_ = Upscaler::Smaa;
        else if (!std::strcmp(u, "bilinear")) upscaler_ = Upscaler::Bilinear;
        else if (!std::strcmp(u, "metalfx-spatial")) upscaler_ = Upscaler::MetalFXSpatial;
      }
      // RECOMP_TAA_SHARPNESS=0..1 (default 0.5): sharpening after TAA (0:
      // none).
      if (const char* sh = std::getenv("RECOMP_TAA_SHARPNESS"))
        taa_sharpness_ = std::clamp(float(std::atof(sh)), 0.0f, 1.0f);
      // RECOMP_MFX_SIGNS=jx,jy,mv: the jitter's and motion's signs as given
      // to MetalFX (for checking conventions).
      if (const char* g = std::getenv("RECOMP_MFX_SIGNS"))
        std::sscanf(g, "%f,%f,%f", &jitter_sign_[0], &jitter_sign_[1], &mv_sign_);
      // RECOMP_RT_BOOST=WxH:k,...: render targets (and depth buffers) of
      // those sizes made k times larger in each direction.
      if (const char* b = std::getenv("RECOMP_RT_BOOST")) {
        for (const char* q = b; *q;) {
          RtBoost rb{};
          if (std::sscanf(q, "%dx%d:%d", &rb.w, &rb.h, &rb.k) == 3 && rb.w > 0 && rb.h > 0 && rb.k > 1)
            rt_boost_.push_back(rb);
          const char* comma = std::strchr(q, ',');
          if (!comma) break;
          q = comma + 1;
        }
        for (const RtBoost& rb : rt_boost_)
          std::fprintf(stderr, "gfx: %dx%d render targets made %dx%d\n", rb.w, rb.h, rb.w * rb.k, rb.h * rb.k);
      }
      if (const char* r = std::getenv("RECOMP_RENDER_SCALE"))
        render_scale_ = std::clamp(float(std::atof(r)), 0.25f, 1.0f);
      if (upscaler_ == Upscaler::Smaa || upscaler_ == Upscaler::Taa) render_scale_ = 1.0f;  // antialiasing only
      if (upscaler_ != Upscaler::Off)
        std::fprintf(stderr, "gfx: scene at %.2f of the back buffer, then %s\n", render_scale_,
                     upscaler_ == Upscaler::MetalFX ? "MetalFX (temporal)"
                     : upscaler_ == Upscaler::MetalFXSpatial ? "SMAA and MetalFX (spatial)"
                     : upscaler_ == Upscaler::Taa   ? "TAA"
                     : upscaler_ == Upscaler::Smaa  ? "SMAA 1x"
                                                    : "bilinear filtering");
      for (auto [sid, name] : {std::pair{kClearVs, "clear_vs"}, {kClearPs, "clear_fs"},
                               {kQuadVs, "quad_vs"}, {kQuadPs, "quad_fs"}}) {
        Shader s;
        s.fn = [lib newFunctionWithName:@(name)];
        shaders_[sid] = s;
        enc_shaders_[sid] = s.fn;
      }
      for (int linear = 0; linear < 2; linear++) {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter =
            linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        quad_sampler_[linear] = [dev_ newSamplerStateWithDescriptor:sd];
      }

      // Shaders' samplers without a texture read (0, 0, 0, 1), as in D3D9.
      auto dummy = [&](MTLTextureType type) {
        const uint32_t black = 0xFF000000u;
        MTLTextureDescriptor* td = [MTLTextureDescriptor new];
        td.textureType = type;
        td.pixelFormat = MTLPixelFormatBGRA8Unorm;
        td.width = td.height = td.depth = 1;
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [dev_ newTextureWithDescriptor:td];
        for (int s = 0; s < (type == MTLTextureTypeCube ? 6 : 1); s++)
          [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1)
               mipmapLevel:0
                     slice:NSUInteger(s)
                 withBytes:&black
               bytesPerRow:4
             bytesPerImage:4];
        return t;
      };
      dummy_2d_ = dummy(MTLTextureType2D);
      dummy_cube_ = dummy(MTLTextureTypeCube);
      dummy_3d_ = dummy(MTLTextureType3D);
      // D3D9's defaults (the front ends send only what changes).
      rs_.fill(0);
      rs_[8] = 3;
      rs_[14] = 1;
      rs_[19] = 2;
      rs_[20] = 1;
      rs_[22] = 3;
      rs_[23] = 4;
      rs_[25] = 8;
      rs_[53] = rs_[54] = rs_[55] = 1;
      rs_[56] = 8;
      rs_[58] = rs_[59] = 0xFFFFFFFFu;
      rs_[168] = 0xF;
      rs_[171] = 1;
      // RECOMP_METAL_SYNC=1: no encoding thread (for comparisons).
      // RECOMP_METAL_LABELS=1: render passes named in GPU traces.
      const char* labels = std::getenv("RECOMP_METAL_LABELS");
      labels_ = labels && *labels == '1';
      // RECOMP_GPU_PASSES=1: GPU time of every render pass, from Metal's
      // stage-boundary timestamps, reported every 120 frames.
      const char* gp = std::getenv("RECOMP_GPU_PASSES");
      if (gp && *gp == '1') SetUpPassTimes();
      const char* sync = std::getenv("RECOMP_METAL_SYNC");
      sync_ = sync && *sync && *sync != '0';
      // RECOMP_REVERSED_Z=0: float depth buffers hold D3D9's values, not
      // 1 - d (reversed_z.h).
      const char* rz = std::getenv("RECOMP_REVERSED_Z");
      reversed_z_ = !(rz && *rz == '0');
      if (!reversed_z_) std::fprintf(stderr, "gfx: reversed depth off\n");
      // RECOMP_DEPTH_FROM_W=0: perspective draws keep z (reversed_z.h).
      const char* dw = std::getenv("RECOMP_DEPTH_FROM_W");
      depth_from_w_ = reversed_z_ && !(dw && *dw == '0');
      if (reversed_z_ && !depth_from_w_) std::fprintf(stderr, "gfx: depth from w off\n");
      if (!sync_) encoder_ = std::thread([this] { EncoderMain(); });
      // A game: no App Nap, timers not coalesced. Without it, in a window
      // not in front, the benchmark street had twice the hitches.
      activity_ = [NSProcessInfo.processInfo
          beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                            reason:@"game"];
      std::fprintf(stderr, "gfx: Metal (%s)\n", dev_.name.UTF8String);
    }
    return true;
  }

  ~MetalDevice() override {
    if (!dev_) return;
    // Lean shader and pipeline prebuild blocks capture this device's state:
    // they are finished below, before its maps, mutexes and Metal objects
    // are destroyed. Those still queued see closing_ and do nothing, so only
    // the one running is waited for (quitting while loading, hundreds are
    // queued).
    closing_ = true;
    // Each step that takes long is told, to find what holds a quit up.
    uint64_t t = NowNs();
    auto step = [&t](const char* what) {
      const uint64_t now = NowNs();
      if (now - t > 500000000ull) std::fprintf(stderr, "gfx: closing: %s took %.1f s\n", what, double(now - t) / 1e9);
      t = now;
    };
    std::lock_guard<std::recursive_mutex> l(mu_);
    step("waiting for the device's lock");
    if (frame_open_) EndFrame();
    WaitIdle();
    step("the last frame");
    {
      std::lock_guard<std::mutex> q(q_mu_);
      stop_ = true;
    }
    q_cv_.notify_all();
    if (encoder_.joinable()) encoder_.join();
    step("the encoding thread");
    if (last_cb_) [last_cb_ waitUntilCompleted];
    step("the GPU");
    dispatch_sync(AheadQueue(), ^{});
    step("the build ahead");
    StopDisplayLink();
    if (view_) OnMainThread([&] { SDL_Metal_DestroyView(view_); });
  }

  bool HasFixedFunction() const override { return false; }

  // ---- memory shared with the GPU

  bool SharesMemory() const override { return true; }

  bool MapSharedMemory(void* host, size_t size) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (shared_buf_) return false;  // one region
    shared_buf_ = [dev_ newBufferWithBytesNoCopy:host
                                          length:size
                                         options:MTLResourceStorageModeShared
                                     deallocator:nil];
    if (!shared_buf_) {
      WarnF("gfx: cannot map %zu bytes of shared memory\n", size);
      return false;
    }
    shared_lo_ = static_cast<const uint8_t*>(host);
    shared_hi_ = shared_lo_ + size;
    return true;
  }

  // Without mu_: front ends ask for every buffer a draw reads.
  uint64_t FrameSerial() const override { return draw_serial_.load(std::memory_order_acquire); }
  uint64_t CompletedSerial() const override { return completed_.load(std::memory_order_acquire); }

  bool TakePresentLatency(double* median, double* p99) override {
    std::vector<double> d;
    {
      std::lock_guard<std::mutex> l(shown_mu_);
      d.swap(latency_);
    }
    if (d.empty()) return false;
    std::sort(d.begin(), d.end());
    *median = d[d.size() / 2];
    *p99 = d[size_t(0.99 * double(d.size() - 1) + 0.5)];
    return true;
  }

  bool TakeShownIntervals(double* median, double* p99, double* max) override {
    std::vector<double> d;
    {
      std::lock_guard<std::mutex> l(shown_mu_);
      for (size_t i = 1; i < shown_.size(); i++)
        if (shown_[i] > shown_[i - 1]) d.push_back((shown_[i] - shown_[i - 1]) * 1e3);
      if (!shown_.empty()) shown_.erase(shown_.begin(), shown_.end() - 1);
    }
    if (d.empty()) return false;
    std::sort(d.begin(), d.end());
    *median = d[d.size() / 2];
    *p99 = d[size_t(0.99 * double(d.size() - 1) + 0.5)];
    *max = d.back();
    return true;
  }

  void EnablePerfStats() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    perf_t0_ = NowNs();
    perf_ = true;
  }

  std::string TakePerfStats() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!perf_ || !pc_.frames) return {};
    const PerfCounts c = pc_;
    pc_ = {};
    const uint64_t now = NowNs();
    const double wall = double(now - perf_t0_);
    perf_t0_ = now;
    const double f = double(c.frames);
    const double busy = double(enc_busy_ns_.exchange(0));
    const uint64_t pipes = pipe_compiles_.exchange(0), pipe_ns = pipe_ns_.exchange(0),
                   pipe_max = pipe_max_ns_.exchange(0), prebuilt = pipe_prebuilt_.exchange(0);
    char b[768];
    std::snprintf(b, sizeof b,
                  "per frame %.0f draws (%.0f of them depth from w, %.0f not for another projection; %.2f ms "
                  "recording them, %.2f us each), "
                  "%.0f recorded ops, "
                  "%.1f render passes, %.1f uploads ahead + %.1f in order (%.2f MB, %.2f MB in "
                  "order); encoder busy %.0f%% (%.2f ms a frame); in %.0f frames: %llu textures + "
                  "%llu depth buffers created, %llu arena buffers (%.0f MB) + %llu made ahead, %llu shaders (%.1f ms, "
                  "%llu MSL compiles), %llu pipelines compiled while drawing (%.1f ms, max %.1f), "
                  "%llu built ahead used",
                  double(c.draws) / f, double(c.w_draws) / f, double(c.w_other) / f, double(c.draw_ns) / f / 1e6,
                  c.draws ? double(c.draw_ns) / double(c.draws) / 1e3 : 0.0, double(c.ops) / f,
                  double(c.passes) / f, double(c.uploads_ahead) / f, double(c.uploads_in_order) / f,
                  double(c.upload_bytes) / f / 1048576.0, double(c.in_order_bytes) / f / 1048576.0,
                  wall > 0 ? 100.0 * busy / wall : 0.0, busy / f / 1e6, f,
                  (unsigned long long)c.textures, (unsigned long long)c.depths,
                  (unsigned long long)c.arena_buffers, double(c.arena_bytes) / 1048576.0,
                  (unsigned long long)c.arena_ahead,
                  (unsigned long long)c.shaders, double(c.shader_ns) / 1e6,
                  (unsigned long long)c.msl_compiles, (unsigned long long)pipes, double(pipe_ns) / 1e6,
                  double(pipe_max) / 1e6, (unsigned long long)prebuilt);
    return b;
  }

  double TakeGpuFrameMs() override {
    const uint64_t ns = gpu_ns_.exchange(0), frames = gpu_frames_.exchange(0);
    return frames ? double(ns) / 1e6 / double(frames) : -1;
  }

  // ---- swap chain

  void ResetSwapChain(int w, int h, bool vsync) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    @autoreleasepool {
      Settle();
      pass_ = {};
      Keep(backbuffer_);
      Keep(front_);
      // The temporal antialiasing and MetalFX write their output straight
      // into the back buffer; the two trade places at Present, so alike.
      const MTLTextureUsage extra =
          Temporal() || upscaler_ == Upscaler::MetalFXSpatial ? MTLTextureUsageShaderWrite : 0;
      backbuffer_ = NewTexture(MTLTextureType2D, MTLPixelFormatBGRA8Unorm, w, h, 1, true, false, extra);
      front_ = NewTexture(MTLTextureType2D, MTLPixelFormatBGRA8Unorm, w, h, 1, true, false, extra);
      bb_w_ = w;
      bb_h_ = h;
#if TARGET_OS_OSX
      Rec([this, vsync] { layer_.displaySyncEnabled = vsync; });
#endif
      vsync_ = vsync;
      // Both start black, as a fresh D3D swap chain does.
      for (id<MTLTexture> t : {backbuffer_, front_}) {
        Pass p{};
        p.color[0] = t;
        p.fmt[0] = MTLPixelFormatBGRA8Unorm;
        p.targets = 1;
        p.w = w;
        p.h = h;
        SetPass(p);
        clear_flags_ = 1;
        clear_color_ = MTLClearColorMake(0, 0, 0, 1);
        Settle();
      }
      BindTarget();
      std::fprintf(stderr, "gfx: back buffer %dx%d%s\n", w, h, vsync ? " (vsync)" : "");
    }
  }

  // ---- resources

  TextureId CreateTexture(int w, int h, int levels, Format fmt, bool rt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (w <= 0 || h <= 0) return 0;
    if (levels <= 0) {
      levels = 1;
      for (int s = std::max(w, h); s > 1; s >>= 1) levels++;
    }
    Tex t;
    t.w = w;
    t.h = h;
    t.levels = levels;
    t.fmt = fmt;
    t.rt = rt;
    // X8R8G8B8 is stored as is, alpha read as 1 (uploads copy it untouched).
    t.opaque = !rt && fmt == Format::X8R8G8B8;
    int rw = w, rh = h;
    t.scaled = rt && ScaledSize(w, h, &rw, &rh, &t.scene);
    t.tex = NewTexture(MTLTextureType2D, TexFormat(fmt), rw, rh, levels, rt, t.opaque);
    if (perf_) pc_.textures++;
    if (!t.tex) {
      WarnF("gfx: CreateTexture %dx%d x%d fmt %u failed\n", w, h, levels, unsigned(fmt));
      return 0;
    }
    if (fmt == Format::P8) WarnF("gfx: P8 texture (no palette support)\n");
    TextureId tid = NewTexId();
    AddTex(tid, t);
    return tid;
  }

  void DestroyTexture(TextureId tid) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(tid);
    if (it == textures_.end()) return;
    bool bound = current_color_ == tid;
    if (current_color_ == tid) current_color_ = 0, current_level_ = 0;
    for (ColorTarget& e : current_extra_)
      if (e.tex == tid) e = {}, bound = true;
    if (const DepthId z = it->second.depth) {  // a depth texture: its depth entry too
      if (current_depth_ == z) current_depth_ = 0, bound = true;
      depths_.erase(z);
    }
    if (bound) BindTarget();
    // Command buffers keep what they use alive; until recorded work has
    // been encoded, Keep does.
    Keep(it->second.tex);
    tex_at_[tid] = nullptr;
    textures_.erase(it);
    free_tex_ids_.push_back(tid);
  }

  void FillTexture(TextureId tid, const Rect& r, uint32_t argb) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(tid);
    if (it == textures_.end()) return;
    Rect c = Clip(r, it->second.w, it->second.h);
    int w = c.right - c.left, h = c.bottom - c.top;
    if (w <= 0 || h <= 0) return;
    std::vector<uint32_t> px(size_t(w) * h, argb);
    Upload(it->second, 0, 0, c, px.data(), uint32_t(w) * 4, Format::A8R8G8B8);
  }

  void UpdateTexture(TextureId tid, int level, const Rect& r, const void* src,
                     uint32_t pitch) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Update(tid, 0, level, r, src, pitch);
  }

  TextureId CreateCubeTexture(int size, int levels, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (size <= 0) return 0;
    if (levels <= 0) {
      levels = 1;
      for (int s = size; s > 1; s >>= 1) levels++;
    }
    Tex t;
    t.w = t.h = size;
    t.levels = levels;
    t.fmt = fmt;
    t.cube = true;
    t.opaque = fmt == Format::X8R8G8B8;
    t.tex = NewTexture(MTLTextureTypeCube, TexFormat(fmt), size, size, levels, false, t.opaque);
    if (perf_) pc_.textures++;
    if (!t.tex) {
      WarnF("gfx: CreateCubeTexture %d x%d fmt %u failed\n", size, levels, unsigned(fmt));
      return 0;
    }
    TextureId tid = NewTexId();
    AddTex(tid, t);
    return tid;
  }

  void UpdateCubeTexture(TextureId tid, int face, int level, const Rect& r, const void* src,
                         uint32_t pitch) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (face < 0 || face > 5) return;
    Update(tid, face, level, r, src, pitch);
  }

  DepthId CreateDepth(int w, int h, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    int rw = w, rh = h;
    bool scene = false;
    const bool scaled = ScaledSize(w, h, &rw, &rh, &scene);
    // Sampled when copied to a full-size depth buffer (DepthProxy), or read
    // by TAA.
    Depth d = NewDepth(rw, rh, fmt, scaled || scene);
    d.scaled = scaled;
    if (!d.tex) return 0;
    if (perf_) pc_.depths++;
    DepthId did = next_id_++;
    depths_[did] = d;
    return did;
  }

  void SetDepthCompare(TextureId id, bool on) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (Tex* t = FindTex(id)) t->compare = on;
  }

  TextureId CreateDepthTexture(int w, int h, DepthId* depth) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    *depth = 0;
    int rw = w, rh = h;
    bool scene = false;
    const bool scaled = ScaledSize(w, h, &rw, &rh, &scene);
    Depth d = NewDepth(rw, rh, Format::D24S8, true);
    if (!d.tex) return 0;
    if (perf_) pc_.depths++;
    d.scaled = scaled;
    Tex t;
    t.scaled = scaled;
    t.scene = scene;
    t.tex = d.tex;
    t.w = w;
    t.h = h;
    t.fmt = Format::D24S8;
    t.depth = next_id_++;
    depths_[t.depth] = d;
    TextureId tid = NewTexId();
    AddTex(tid, t);
    *depth = t.depth;
    return tid;
  }

  void DestroyDepth(DepthId did) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = depths_.find(did);
    if (it == depths_.end()) return;
    if (current_depth_ == did) {
      current_depth_ = 0;
      BindTarget();
    }
    Keep(it->second.tex);
    depths_.erase(it);
  }

  void SetRenderTarget(TextureId color, DepthId depth) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    current_color_ = color;
    current_level_ = 0;
    current_extra_.fill({});
    current_depth_ = depth;
    BindTarget();
  }

  void SetRenderTargets(const ColorTarget color[4], DepthId depth) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    current_color_ = color[0].tex;
    current_level_ = color[0].tex ? color[0].level : 0;
    for (int i = 0; i < 3; i++) current_extra_[i] = color[i + 1];
    current_depth_ = depth;
    BindTarget();
  }

  void StretchRect(ColorTarget src, const Rect& sr, ColorTarget dst, const Rect& dr,
                   bool linear) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    @autoreleasepool {
      int sw, sh, dw, dh;
      Tex* st = nullptr;
      Tex* dt = nullptr;
      id<MTLTexture> s = Target(src, &sw, &sh, &st);
      id<MTLTexture> d = Target(dst, &dw, &dh, &dt);
      if (!s || !d) return;
      Rect a = Clip(sr, sw, sh), b = Clip(dr, dw, dh);
      if (Empty(a) || Empty(b)) return;
      // Targets at the internal resolution: rectangles in their pixels.
      auto scale = [](Rect r, id<MTLTexture> t, int level, int gw, int gh) {
        const float fx = float(std::max<NSUInteger>(1, t.width >> level)) / float(gw);
        const float fy = float(std::max<NSUInteger>(1, t.height >> level)) / float(gh);
        if (fx == 1.0f && fy == 1.0f) return r;
        return Rect{int32_t(std::lround(r.left * fx)), int32_t(std::lround(r.top * fy)),
                    int32_t(std::lround(r.right * fx)), int32_t(std::lround(r.bottom * fy))};
      };
      a = scale(a, s, src.tex ? src.level : 0, sw, sh);
      b = scale(b, d, dst.tex ? dst.level : 0, dw, dh);
      if (Empty(a) || Empty(b)) return;
      for (Tex* t : {st, dt})
        if (t) t->used = serial_;
      Settle();
      EnsureFrame();
      const int sl = src.tex ? src.level : 0, dl = dst.tex ? dst.level : 0;
      const bool copy = a.right - a.left == b.right - b.left &&
                        a.bottom - a.top == b.bottom - b.top && s.pixelFormat == d.pixelFormat;
      __unsafe_unretained id<MTLTexture> su = s, du = d;
      Rec([=, this] {
        if (copy) {
          id<MTLBlitCommandEncoder> bl = [cmd_ blitCommandEncoder];
          [bl copyFromTexture:su
                   sourceSlice:0
                   sourceLevel:NSUInteger(sl)
                  sourceOrigin:MTLOriginMake(NSUInteger(a.left), NSUInteger(a.top), 0)
                    sourceSize:MTLSizeMake(NSUInteger(a.right - a.left),
                                           NSUInteger(a.bottom - a.top), 1)
                     toTexture:du
              destinationSlice:0
              destinationLevel:NSUInteger(dl)
             destinationOrigin:MTLOriginMake(NSUInteger(b.left), NSUInteger(b.top), 0)];
          [bl endEncoding];
          return;
        }
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = du;
        rp.colorAttachments[0].level = NSUInteger(dl);
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        DrawQuad(e, du.pixelFormat, su, sl, float(sw), float(sh), a, float(dw), float(dh), b, linear);
        [e endEncoding];
      });
    }
  }

  // ---- 2D: DirectDraw's, not provided

  void Blit(const void*, uint32_t, Format, const Rect&, const Rect&, bool, uint32_t) override {
    WarnOnce(1, "Blit");
  }
  void BlitBlend(const void*, uint32_t, const Rect&, const Rect&) override {
    WarnOnce(2, "BlitBlend");
  }
  void BlitFront(const void*, uint32_t, Format, const Rect&, const Rect&) override {
    WarnOnce(4, "BlitFront");
  }

  void ReadBack(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    ReadTexture(pass_.color[0], pass_.w, pass_.h, pass_.level[0], r, dst, pitch, fmt);
  }

  uint32_t StartReadBack(const Rect& r) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    uint32_t t = next_read_++;
    if (!t) t = next_read_++;
    Read& rd = reads_[t];
    rd.r = r;
    rd.px.resize(size_t(std::max(r.right - r.left, 0)) * size_t(std::max(r.bottom - r.top, 0)));
    ReadTexture(pass_.color[0], pass_.w, pass_.h, pass_.level[0], r, rd.px.data(),
                uint32_t(r.right - r.left) * 4, Format::A8R8G8B8);
    return t;
  }

  void FinishReadBack(uint32_t ticket, void* dst, uint32_t pitch, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = reads_.find(ticket);
    if (it == reads_.end()) return;
    if (dst) {
      const Read& rd = it->second;
      int w = rd.r.right - rd.r.left, h = rd.r.bottom - rd.r.top;
      uint32_t bpp = BitsPerPixel(fmt) / 8;
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          EncodePixel(static_cast<uint8_t*>(dst) + size_t(y) * pitch + size_t(x) * bpp, fmt,
                      rd.px[size_t(y) * w + x]);
    }
    reads_.erase(it);
  }

  void ReadBackFront(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    ReadTexture(front_, bb_w_, bb_h_, 0, r, dst, pitch, fmt);
  }

  void PresentFront() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Settle();
    EnsureFrame();
    __unsafe_unretained id<MTLTexture> front = front_;
    const int bw = bb_w_, bh = bb_h_;
    Rec([=, this] { Show(front, bw, bh); });
    EndFrame();
  }

  // ---- state. Only what the shader path uses: the fixed-function state
  // (texture stages, transforms, material, lights) has no effect here.

  void SetRenderState(uint32_t s, uint32_t v) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (s < rs_.size()) rs_[s] = v;
  }
  void SetTextureStageState(uint32_t, uint32_t, uint32_t) override {}
  void SetTexture(uint32_t, TextureId) override {}
  void SetTransform(uint32_t, const float*) override {}
  void SetViewport(const Viewport& v) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    viewport_ = v;
  }
  void SetScissor(const Rect& r) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    scissor_ = r;
  }
  void SetMaterial(const Material&) override {}
  void SetLight(uint32_t, const Light&) override {}
  void LightEnable(uint32_t, bool) override {}
  void SetClipPlane(uint32_t i, const float* p) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (i < 6) std::memcpy(clip_planes_[i], p, sizeof clip_planes_[i]);
  }

  // ---- drawing

  void Clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t color, float z,
             uint32_t stencil) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    EnsureTarget();
    if (!pass_.color[0]) return;
    if (!pass_.depth) flags &= ~6u;
    if (!pass_.stencil) flags &= ~4u;
    if (!(flags & 7)) return;
    Rect area = ViewportRect();
    if (rs_[174]) area = Intersect(area, scissor_);
    std::vector<Rect> todo;
    if (!count || !rects) {
      todo.push_back(area);
    } else {
      for (uint32_t i = 0; i < count; i++) {
        Rect c = Intersect(rects[i], area);
        if (!Empty(c)) todo.push_back(c);
      }
    }
    if (todo.empty()) return;
    if (pass_.reversed) z = 1.0f - z;
    // A new content of the whole buffer: the next draw from w sets its
    // projection.
    if ((flags & 2) && todo.size() == 1 && todo[0].left == 0 && todo[0].top == 0 &&
        todo[0].right == pass_.w && todo[0].bottom == pass_.h)
      if (auto dit = depths_.find(pass_.depth_id); dit != depths_.end()) dit->second.wd.fresh = true;
    // A clear of the whole target before anything was drawn is the render
    // pass's load action.
    if (!enc_open_ && todo.size() == 1 && todo[0].left == 0 && todo[0].top == 0 &&
        todo[0].right == pass_.w && todo[0].bottom == pass_.h) {
      if (flags & 1) Argb(&clear_rgba_[0], color);
      clear_color_ = MTLClearColorMake(clear_rgba_[0], clear_rgba_[1], clear_rgba_[2],
                                       clear_rgba_[3]);
      if (flags & 2) clear_depth_ = z;
      if (flags & 4) clear_stencil_ = uint8_t(stencil);
      clear_flags_ |= flags & 7;
      return;
    }
    // Otherwise a triangle per rectangle, scissored to it, writing exactly
    // what the flags say.
    if ((flags & 1) && pass_.targets > 1)
      FatalF("gfx: partial Clear of %d render targets is not supported", pass_.targets);
    if (!BeginDraw()) return;
    PipeKey pk = PassKey();
    pk.vs = kClearVs;
    pk.ps = kClearPs;
    pk.mask[0] = (flags & 1) ? MTLColorWriteMaskAll : 0;
    for (int i = 1; i < 4; i++) pk.mask[i] = 0;
    BindPipeline(pk);
    DepthKey dk{};
    dk.func = uint8_t(MTLCompareFunctionAlways);
    if (flags & 2) dk.test = dk.write = 1;
    dk.sfunc = dk.bfunc = uint8_t(MTLCompareFunctionAlways);
    if (flags & 4) {
      dk.stencil = 1;
      dk.fail = dk.zfail = dk.pass = uint8_t(MTLStencilOperationReplace);
      dk.bfail = dk.bzfail = dk.bpass = uint8_t(MTLStencilOperationReplace);
      dk.read = dk.wmask = 0xFF;
    }
    BindDepth(dk);
    Raster(MTLCullModeNone, MTLTriangleFillModeFill, 0, 0);
    ViewportOf({0, 0, double(pass_.w), double(pass_.h), 0, 1});
    StencilRef(uint8_t(stencil));
    struct Vec {
      float v[4];
    } c, zp{{z, 0, 0, 0}};
    Argb(c.v, color);
    Rec([this, c, zp] {
      [enc_ setVertexBytes:zp.v length:sizeof zp.v atIndex:0];
      [enc_ setFragmentBytes:c.v length:sizeof c.v atIndex:0];
    });
    bound_.vbuf[0] = bound_.fbuf[0] = nullptr;
    for (const Rect& r : todo) {
      ScissorOf({NSUInteger(r.left), NSUInteger(r.top), NSUInteger(r.right - r.left),
                 NSUInteger(r.bottom - r.top)});
      Rec([this] { [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; });
    }
  }

  void BeginScene() override {}
  void EndScene() override {}

  void Draw(uint32_t, uint32_t, const void*, uint32_t, uint32_t, const uint16_t*,
            uint32_t) override {
    FatalF("gfx: fixed-function draw on the Metal backend");
  }
  Mapped MapDraw(uint32_t, uint32_t, uint32_t) override {
    FatalF("gfx: fixed-function draw on the Metal backend");
  }
  void DrawMapped(uint32_t, uint32_t) override {
    FatalF("gfx: fixed-function draw on the Metal backend");
  }

  void Present() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    {
      EndUpscalerFrame();
      Settle();
      EnsureFrame();
      const bool smaa = upscaler_ == Upscaler::Smaa;
      if (smaa) SmaaFrame();
      __unsafe_unretained id<MTLTexture> back = backbuffer_, front = front_;
      const int bw = bb_w_, bh = bb_h_;
      const double t_present = CACurrentMediaTime();  // the game's Present call
      // SMAA filtered the back buffer into the front one; else the back
      // buffer becomes the front one and the old front the next back
      // buffer, D3D9's flip (a discarded back buffer's contents after
      // Present are undefined) instead of a full-size copy.
      __unsafe_unretained id<MTLTexture> shown = smaa ? front : back;
      Rec([=, this] { Show(shown, bw, bh, t_present); });
      if (!smaa) std::swap(backbuffer_, front_);
      presented_ = true;
      if (perf_) {
        pc_.frames++;
        pc_.upload_bytes += frame_upload_bytes_;
      }
      if (frame_upload_bytes_ > (8u << 20))
        std::fprintf(stderr, "gfx: frame uploaded %.1f MB of textures\n", frame_upload_bytes_ / 1048576.0);
      frame_upload_bytes_ = 0;
      EndFrame();
    }
  }

  void SetDepthFromW(int reg, DepthPlanes planes) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    wvp_reg_ = depth_from_w_ && reg >= 0 && reg <= 252 ? reg : -1;
    depth_planes_ = planes;
  }

  void SetMinAnisotropy(int level) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    min_aniso_ = std::clamp(level, 0, 16);
    for (auto& [k, smp] : samplers_) Keep(smp);
    samplers_.clear();
    for (SlotSampler& c : slot_sampler_) c = {};
    if (min_aniso_ > 1) std::fprintf(stderr, "gfx: anisotropic filtering %dx\n", min_aniso_);
  }

  void SetPostAntialiasing(bool on) override {
    if (on) WarnF("gfx: post-process anti-aliasing is not in the Metal backend yet\n");
  }

  // ---- translated shaders (runtime/hlsl). The prelude completes the
  // translation (hlsl.h) as gpu_device.cpp's Metal build does: buffers
  // 0 float registers, 1 int and bool registers, 2 the prelude's state,
  // vertex shaders' 3 the vertices; textures and samplers at their
  // register.

  struct VsState {
    int32_t in[16][4];  // per input slot: byte offset of vertex 0, stride, D3DDECLTYPE (-1: none), 0
    float pixel[4];     // 1 / viewport width, 1 / viewport height; jitter (clip space)
    float clip[6][4];   // SetClipPlane's, clip space
    int32_t clip_mask[4];  // CLIPPLANEENABLE; depth (r_position: 0 D3D9's, 1 reversed, 2 from w)
    float depth[4];        // depth from w: k0, k1 (reversed_z.h)
  };
  struct PsState {
    int32_t alpha[4];  // D3DCMPFUNC (ALWAYS when the test is off), ALPHAREF,
                       // bit n: sampler n reads a reversed depth texture (R_DEPTH_TEX),
                       // 1 if the depth buffer is reversed (R_DEPTH_OUT)
    float bias[16];    // per sampler register: MIPMAPLODBIAS (R_BIAS)
    float vscale[4];   // game pixels per pixel of the target (r_vpos)
  };

  static std::string Prelude(hlsl::Stage stage) { return MetalShaderPrelude(stage); }

  ShaderId CreateShader(const hlsl::Program& prog) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    const uint64_t perf_t0 = perf_ ? NowNs() : 0;
    @autoreleasepool {
      Shader sh;
      sh.stage = prog.stage;
      sh.inputs = prog.inputs.size();
      sh.float_regs = prog.float_regs;
      sh.position_matrix = prog.position_matrix;
      sh.position_note = prog.position_note;
      sh.float_reads = prog.float_reads;
      sh.sampler_type.fill(hlsl::ParamType::Void);
      for (const hlsl::Sampler& smp : prog.samplers) {
        if (smp.reg >= 16) FatalF("gfx: shader sampler register %u", smp.reg);
        sh.sampler_type[smp.reg] = smp.type;
        sh.samplers = std::max(sh.samplers, smp.reg + 1);
      }
      bool vertex = prog.stage == hlsl::Stage::Vertex;
      std::string glsl = "#version 450\n" + Prelude(prog.stage) + prog.glsl;
      const uint64_t hash = Fnv64(glsl.data(), glsl.size()) ^ uint64_t(prog.stage);
      // RECOMP_SHADER_DUMP=dir: each translated shader's GLSL, by hash.
      if (const char* dir = std::getenv("RECOMP_SHADER_DUMP")) {
        char path[1024];
        std::snprintf(path, sizeof path, "%s/%016llx.%s.glsl", dir, (unsigned long long)hash, vertex ? "vs" : "ps");
        if (FILE* f = std::fopen(path, "wx")) {
          std::fwrite(glsl.data(), 1, glsl.size(), f);
          std::fclose(f);
        }
      }
      // GLSL to SPIR-V to MSL takes most of the time (Metal's own compile
      // of the MSL is cached by the system): the MSL comes from earlier
      // runs when it can.
      std::string msl = CachedMsl(hash);
      NSError* err = nil;
      bool has_lean = MetalLeanMacros(&msl, vertex);
      id<MTLLibrary> lib = msl.empty() ? nil
                                       : [dev_ newLibraryWithSource:@(msl.c_str())
                                                            options:has_lean ? MslOptions(true) : nil
                                                              error:&err];
      if (!lib) {
        std::vector<uint32_t> spirv;
        std::string log;
        if (!GlslToSpirv(glsl, vertex ? ShaderStage::Vertex : ShaderStage::Fragment, &spirv, &log)) {
          std::fprintf(stderr, "%s\n", glsl.c_str());
          FatalF("gfx: compiling a translated shader (source above): %s", log.c_str());
        }
        msl.clear();
        if (!SpirvToMsl(spirv, &msl, &log)) FatalF("gfx: SPIR-V to MSL: %s", log.c_str());
        if (perf_) pc_.msl_compiles++;
        has_lean = MetalLeanMacros(&msl, vertex);
        lib = [dev_ newLibraryWithSource:@(msl.c_str())
                                 options:has_lean ? MslOptions(true) : nil
                                   error:&err];
        if (lib) CacheMsl(hash, msl);
      }
      if (!lib) {
        std::fprintf(stderr, "%s\n", msl.c_str());
        FatalF("gfx: compiling MSL (above): %s", err.localizedDescription.UTF8String);
      }
      sh.fn = [lib newFunctionWithName:@"main0"];
      sh.hash = hash;
      sh.varyings = hlsl::VaryingMask(prog);
      if (!vertex) sh.program = std::make_shared<const hlsl::Program>(prog);
      sh.has_lean = has_lean;
      if (has_lean) {
        NSString* src = @(msl.c_str());
        std::atomic<bool>* closing = &closing_;
        dispatch_async(AheadQueue(), ^{
          if (closing->load()) return;
          Lean(hash, src);
        });
      }
      ShaderId sid = next_id_++;
      shaders_[sid] = sh;
      sid_by_hash_[sh.hash] = sid;
      Keep(sh.fn);
      __unsafe_unretained id<MTLFunction> fn = sh.fn;
      Rec([this, sid, fn, hash] {
        enc_shaders_[sid] = fn;
        enc_shader_hash_[sid] = hash;
      });
      PrebuildFor(sh.hash);
      if (perf_t0) {
        pc_.shaders++;
        pc_.shader_ns += NowNs() - perf_t0;
      }
      return sid;
    }
  }

  void DestroyShader(ShaderId sid) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = shaders_.find(sid);
    if (it == shaders_.end() || sid >= kClearVs) return;
    const auto variants = std::move(it->second.variants);
    for (const auto& [mask, vid] : variants) DestroyShader(vid);  // `it` stays valid
    Keep(it->second.fn);
    auto h = sid_by_hash_.find(it->second.hash);
    if (h != sid_by_hash_.end() && h->second == sid) sid_by_hash_.erase(h);
    shaders_.erase(it);
    bound_.pipe_set = false;
    {
      std::lock_guard<std::mutex> pl(prebuilt_mu_);
      for (auto p = prebuilt_.begin(); p != prebuilt_.end();)
        p = p->first.vs == sid || p->first.ps == sid ? prebuilt_.erase(p) : std::next(p);
    }
    Rec([this, sid] {
      last_pipe_ = nil;
      for (auto p = pipelines_.begin(); p != pipelines_.end();) {
        // Work encoded so far holds what it uses: erased at once here.
        if (p->first.vs == sid || p->first.ps == sid)
          p = pipelines_.erase(p);
        else
          ++p;
      }
      enc_shaders_.erase(sid);
      enc_shader_hash_.erase(sid);
    });
  }

  void DrawShader(const ShaderDraw& d) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    for (uint32_t i = 0; i < d.state_count; i++)
      if (d.states[i][0] < rs_.size()) rs_[d.states[i][0]] = d.states[i][1];
    if (d.viewport) viewport_ = *d.viewport;
    if (d.scissor) scissor_ = *d.scissor;
    if (!perf_) return RecordDraw(d);
    const uint64_t t0 = NowNs();
    RecordDraw(d);
    pc_.draws++;
    pc_.draw_ns += NowNs() - t0;
  }

  void RecordDraw(const ShaderDraw& d) {
    EnsureTarget();
    auto vit = shaders_.find(d.vs), pit = shaders_.find(d.ps);
    if (vit == shaders_.end() || pit == shaders_.end()) return;
    const Shader& vs = vit->second;
    const Shader& ps = pit->second;
    ShaderId ps_id = (ps.varyings & ~vs.varyings) ? PixelShaderFor(d.ps, pit->second, vs.varyings) : d.ps;
    if (!pass_.depth) ps_id = PixelShaderWithoutDepth(ps_id);
    const uint32_t count = d.primitives;
    MTLPrimitiveType type;
    uint32_t n;  // vertices or indices used
    bool tri = true;
    switch (d.prim) {
      case 1: type = MTLPrimitiveTypePoint; n = count; tri = false; break;
      case 2: type = MTLPrimitiveTypeLine; n = count * 2; tri = false; break;
      case 3: type = MTLPrimitiveTypeLineStrip; n = count + 1; tri = false; break;
      case 4: type = MTLPrimitiveTypeTriangle; n = count * 3; break;
      case 5: type = MTLPrimitiveTypeTriangleStrip; n = count + 2; break;
      case 6: type = MTLPrimitiveTypeTriangle; n = count + 2; break;  // fan, as a list
      default: return;
    }
    if (!count) return;
    WarnUnsupportedShaderState();
    MTLScissorRect sc;
    if (!DrawScissor(&sc)) return;
    if (!BeginDraw()) return;
    if (pass_.scene && !pass_.low) frame_scene_ = true;
    // Jittered for the temporal upscaler: the scene's geometry (depth
    // tested and written), not full-target quads.
    jitter_draw_ = Temporal() && pass_.scene && !pass_.low && rs_[7] && rs_[14] &&
                   count > 4;
    if (jitter_draw_ && !have_vp_ && d.vs_float) {
      CaptureCamera(d.vs_float);
      scene_depth_ = pass_.depth;
    }

    // Pipeline: shaders, blending, write masks.
    PipeKey pk = PassKey();
    pk.vs = d.vs;
    pk.ps = ps_id;
    if (rs_[27]) {  // ALPHABLENDENABLE
      uint32_t s = rs_[19], dst = rs_[20];
      if (s == 12) { s = 5; dst = 6; }        // BOTHSRCALPHA
      else if (s == 13) { s = 6; dst = 5; }   // BOTHINVSRCALPHA
      pk.blend = 1;
      pk.src = uint8_t(BlendFactor(s, pass_.alpha));
      pk.dst = uint8_t(BlendFactor(dst, pass_.alpha));
      pk.op = uint8_t(BlendOp(rs_[171]));
      if (rs_[206]) {  // SEPARATEALPHABLENDENABLE
        pk.src_a = uint8_t(BlendFactor(rs_[207], pass_.alpha));
        pk.dst_a = uint8_t(BlendFactor(rs_[208], pass_.alpha));
        pk.op_a = uint8_t(BlendOp(rs_[209]));
      } else {
        pk.src_a = pk.src;
        pk.dst_a = pk.dst;
        pk.op_a = pk.op;
      }
    }
    pk.mask[0] = WriteMask(rs_[168]);
    for (int i = 1; i < 4; i++) pk.mask[i] = i < pass_.targets ? WriteMask(rs_[189 + i]) : 0;
    // ALPHATESTENABLE off, or ALPHAFUNC ALWAYS: the shader without it.
    // (Of the pixel shader drawn: a variant for unwritten varyings has its own.)
    const bool ps_lean = (ps_id == d.ps ? ps : shaders_.at(ps_id)).has_lean;
    if (ps_lean && (!rs_[15] || rs_[25] == 8)) pk.lean |= kLeanPs;
    BindPipeline(pk);

    // Depth and stencil.
    DepthKey dk{};
    dk.func = dk.sfunc = dk.bfunc = uint8_t(MTLCompareFunctionAlways);
    float bias = 0, slope = 0;
    clip_bias_ = 0;
    if (pass_.depth) {
      dk.test = rs_[7] ? 1 : 0;
      dk.write = (rs_[7] && rs_[14]) ? 1 : 0;
      dk.func = uint8_t(CompareOp(pass_.reversed ? MirrorDepthCompare(rs_[23]) : rs_[23]));
      if (pass_.stencil && rs_[52]) {
        dk.stencil = 1;
        dk.fail = uint8_t(StencilOp(rs_[53]));
        dk.zfail = uint8_t(StencilOp(rs_[54]));
        dk.pass = uint8_t(StencilOp(rs_[55]));
        dk.sfunc = uint8_t(CompareOp(rs_[56]));
        const bool two = rs_[185] != 0;  // TWOSIDEDSTENCILMODE: CCW_STENCIL* for back faces
        dk.bfail = two ? uint8_t(StencilOp(rs_[186])) : dk.fail;
        dk.bzfail = two ? uint8_t(StencilOp(rs_[187])) : dk.zfail;
        dk.bpass = two ? uint8_t(StencilOp(rs_[188])) : dk.pass;
        dk.bfunc = two ? uint8_t(CompareOp(rs_[189])) : dk.sfunc;
        dk.read = uint8_t(rs_[58]);
        dk.wmask = uint8_t(rs_[59]);
      }
      // DEPTHBIAS (less the legacy ZBIAS) and SLOPESCALEDEPTHBIAS; on a
      // reversed buffer the constant part from the vertex shader
      // (reversed_z.h).
      const DepthBias db = DepthBiasOf(AsFloat(rs_[195]) - float(rs_[47]) * (1.0f / 65535.0f), AsFloat(rs_[175]),
                                       pass_.reversed, viewport_.min_z, viewport_.max_z, pass_.bias_scale);
      bias = db.constant;
      slope = db.slope;
      clip_bias_ = db.clip;
    }
    depth_used_ = dk.test || dk.write;
    BindDepth(dk);

    // Rasterizer: D3D culls by screen winding, CW (2) or CCW (3); front
    // faces are D3D's (clockwise) whatever is culled, for VFACE and
    // two-sided stencil.
    MTLCullMode cull = MTLCullModeNone;
    if (tri && rs_[22] == 2) cull = MTLCullModeFront;
    else if (tri && rs_[22] == 3) cull = MTLCullModeBack;
    Raster(cull, rs_[8] != 3 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill, bias, slope);
    const DepthRange zr = pass_.reversed ? ReversedViewportDepth(viewport_.min_z, viewport_.max_z)
                                         : DepthRange{viewport_.min_z, viewport_.max_z};
    ViewportOf({double(viewport_.x), double(viewport_.y), double(std::max(viewport_.width, 1u)),
                double(std::max(viewport_.height, 1u)), double(zr.min_z), double(zr.max_z)});
    ScissorOf(sc);
    StencilRef(uint8_t(rs_[57]));
    float blend[4];
    Argb(blend, rs_[193]);
    BlendColor(blend);

    // Vertices: read in place if every stream used is in shared memory
    // (the draw's base vertex then Metal's), else each used stream's range
    // copied once, all into one chunk.
    const bool indexed = d.indices != nullptr;
    bool in_place = shared_buf_ != nil;
    for (size_t i = 0; i < vs.inputs && in_place; i++) {
      VertexInput in = i < d.input_count ? d.inputs[i] : VertexInput{};
      if (in.type >= 17 || in.stream >= 16 || !d.streams[in.stream].data) continue;
      const VertexStream& vsm = d.streams[in.stream];
      in_place = vsm.data >= shared_lo_ && vsm.data + vsm.size <= shared_hi_;
    }
    if (in_place) {
      DrawInPlace(d, vs, type, n, count);
      AfterDraw();
      return;
    }
    const int64_t first = int64_t(d.base_vertex) + (indexed ? d.min_index : 0);
    const uint32_t nverts = indexed ? d.vertex_count : n;
    VsState st{};
    struct Copy {
      int64_t start, end;
      size_t pos;
    };
    Copy copies[16];
    bool used[16] = {};
    size_t total = 0;
    for (size_t i = 0; i < vs.inputs; i++) {
      VertexInput in = i < d.input_count ? d.inputs[i] : VertexInput{};
      if (in.type >= 17 || in.stream >= 16 || !d.streams[in.stream].data || used[in.stream])
        continue;
      const VertexStream& vsm = d.streams[in.stream];
      Copy& c = copies[in.stream];
      c.start = first * vsm.stride;
      // (A stride of 0, an instance's data: one element, of unknown size.)
      const size_t span = vsm.stride ? size_t(nverts) * vsm.stride : std::min<size_t>(vsm.size, 256);
      c.end = std::min<int64_t>(c.start + int64_t(span), vsm.size);
      c.pos = total;
      // Room for a whole last vertex and the shader's unaligned reads.
      total += (span + 8 + 3) & ~size_t(3);
      used[in.stream] = true;
    }
    Slice vb = Alloc(std::max<size_t>(total, 16), 4);
    for (int s = 0; s < 16; s++) {
      if (!used[s]) continue;
      const Copy& c = copies[s];
      if (c.start >= 0 && c.end > c.start)
        std::memcpy(vb.cpu + c.pos, d.streams[s].data + c.start, size_t(c.end - c.start));
    }
    for (size_t i = 0; i < vs.inputs; i++) {
      VertexInput in = i < d.input_count ? d.inputs[i] : VertexInput{};
      if (in.type >= 17 || in.stream >= 16 || !used[in.stream]) {
        st.in[i][2] = -1;
        continue;
      }
      const VertexStream& vsm = d.streams[in.stream];
      st.in[i][0] = int32_t(vb.off + copies[in.stream].pos) -
                    int32_t(indexed ? d.min_index : 0) * int32_t(vsm.stride) + int32_t(in.offset);
      st.in[i][1] = int32_t(vsm.stride);
      st.in[i][2] = int32_t(in.type);
    }
    PixelState(st.pixel);
    ClipState(st, d, vs);
    VertexBuffer(3, vb.buf, 0);

    ShaderResources(d, vs, ps, st);

    // The draw.
    if (indexed && d.prim != 6) {
      const size_t bytes = size_t(n) * d.index_size;
      Slice ib = Alloc(bytes, 4);
      std::memcpy(ib.cpu, d.indices, bytes);
      const MTLIndexType it = d.index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
      Rec([this, type, n, it, ib] {
        [enc_ drawIndexedPrimitives:type
                         indexCount:n
                          indexType:it
                        indexBuffer:ib.buf
                  indexBufferOffset:ib.off];
      });
    } else if (d.prim == 6) {
      Slice ib = FanIndices(d, count);
      Rec([this, type, count, ib] {
        [enc_ drawIndexedPrimitives:type
                         indexCount:count * 3
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:ib.buf
                  indexBufferOffset:ib.off];
      });
    } else {
      Rec([this, type, n] { [enc_ drawPrimitives:type vertexStart:0 vertexCount:n]; });
    }
    AfterDraw();
  }

 private:
  struct Shader;  // (below)

  // ---- the scene at an internal resolution, upscaled (RECOMP_UPSCALER)

  enum class Upscaler { Off, Bilinear, MetalFX, MetalFXSpatial, Taa, Smaa };
  // Temporal: the scene is jittered and resolved with its history.
  bool Temporal() const { return upscaler_ == Upscaler::MetalFX || upscaler_ == Upscaler::Taa; }

  // A depth buffer of format `pf` stores 1 - d (reversed_z.h): float ones,
  // unless RECOMP_REVERSED_Z=0.
  bool ReversedDepth(MTLPixelFormat pf) const {
    return reversed_z_ && (pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth32Float_Stencil8);
  }

  // The size a game's render target or depth buffer of w x h is made at:
  // the back buffer's and its halves and quarters (the scene's buffers and
  // their downsamples) at the internal resolution. False for the others.
  bool ScaledSize(int w, int h, int* rw, int* rh, bool* scene = nullptr) const {
    if (scene) *scene = false;
    // SMAA works on the finished frame (Present): the scene is drawn as
    // with no upscaler.
    if (upscaler_ != Upscaler::Off && upscaler_ != Upscaler::Smaa && bb_w_ > 0)
      for (int k = 0; k < 3; k++)
        if (w == bb_w_ >> k && h == bb_h_ >> k) {
          // TAA: the scene's buffers (jittered) at their own size; the scene
          // goes to the back buffer itself, as with no upscaler.
          if (upscaler_ == Upscaler::Taa) {
            if (scene) *scene = true;
            return false;
          }
          *rw = std::max(1, int(std::lround(w * render_scale_)));
          *rh = std::max(1, int(std::lround(h * render_scale_)));
          if (scene) *scene = true;
          return true;
        }
    // RECOMP_RT_BOOST: targets of these sizes made larger (shadow maps,
    // reflections), the game none the wiser.
    for (const RtBoost& b : rt_boost_)
      if (w == b.w && h == b.h && (bb_w_ <= 0 || w != bb_w_ || h != bb_h_)) {
        *rw = std::min(16384, w * b.k);
        *rh = std::min(16384, h * b.k);
        return true;
      }
    return false;
  }

  // The composite's target: the back buffer at the internal resolution.
  void EnsureLowColor() {
    const NSUInteger w = NSUInteger(std::max(1L, std::lround(bb_w_ * render_scale_)));
    const NSUInteger h = NSUInteger(std::max(1L, std::lround(bb_h_ * render_scale_)));
    if (low_color_ && low_color_.width == w && low_color_.height == h) return;
    if (low_color_) Keep(low_color_);
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    low_color_ = [dev_ newTextureWithDescriptor:td];
    fx_reset_ = true;
  }

  // Pixel size and jitter for the vertex shaders' r_pixel.
  void PixelState(float* px) const {
    const float w = float(std::max(viewport_.width, 1u)) * pass_.sx;
    const float h = float(std::max(viewport_.height, 1u)) * pass_.sy;
    px[0] = 1.0f / w;
    px[1] = 1.0f / h;
    px[2] = jitter_draw_ ? 2.0f * jitter_[0] / w : 0.0f;
    px[3] = jitter_draw_ ? 2.0f * jitter_[1] / h : 0.0f;
  }

  // The vertex state's user clip planes (D3D9 with shaders: clip space),
  // how depth is made (r_position): reversed, its bias, and from w where
  // the draw qualifies (reversed_z.h); and the vertex samplers that read a
  // reversed depth texture (R_DEPTH_TEX).
  void ClipState(VsState& st, const ShaderDraw& d, const Shader& vs) {
    st.clip_mask[0] = int32_t(rs_[152] & 0x3F);
    if (st.clip_mask[0]) std::memcpy(st.clip, clip_planes_, sizeof st.clip);
    for (uint32_t r = 0; r < vs.samplers && r < 4; r++)
      if (const Tex* t = FindTex(d.textures[16 + r]); t && t->depth && ReversedDepth(t->tex.pixelFormat))
        st.clip_mask[2] |= int32_t(1u << r);
    st.clip_mask[1] = pass_.reversed ? 1 : 0;
    st.depth[2] = clip_bias_;
    if (!pass_.reversed || wvp_reg_ < 0 || !d.vs_float) return;
    if (vs.position_matrix != wvp_reg_) {
      NoteNotFromW(vs);
      return;
    }
    WProjection p;
    if (!PerspectiveOf(d.vs_float + wvp_reg_, &p)) return;
    auto dit = depths_.find(pass_.depth_id);
    if (dit == depths_.end()) return;
    WDepth& wd = dit->second.wd;
    // The first after a clear: the game's planes, where it knows them.
    WProjection exact;
    float n, f;
    const bool fresh = wd.fresh && depth_used_;
    const bool planes = fresh && depth_planes_ && depth_planes_(&n, &f) && n > 0 && f > n;
    if (planes) exact = PerspectiveOfPlanes(n, f);
    if (!wd.Use(p, planes ? &exact : nullptr, depth_used_)) {
      // Tests or writes depth, with another projection than the content's.
      if (perf_ && depth_used_) pc_.w_other++;
      return;
    }
    // The first few contents' projections, for the log.
    if (fresh && w_logged_ < 4) {
      w_logged_++;
      std::fprintf(stderr, "gfx: depth from w: a %.9g, n %.6g (%s)\n", wd.a, wd.n,
                   !planes ? "the matrix's" : wd.n == exact.n ? "the game's" : "the matrix's: the game's disagree");
    }
    st.clip_mask[1] = 2;
    st.depth[0] = wd.K0();
    st.depth[1] = wd.K1();
    if (perf_) pc_.w_draws++;
  }

  // A vertex shader drawn into a reversed buffer, testing or writing
  // depth, that reads the world-view-projection's registers but does not
  // qualify for depth from w: once per shader, why (the first 256).
  void NoteNotFromW(const Shader& vs) {
    if (!depth_used_ || !vs.hash || w_noted_.size() > 256) return;
    bool reads = false;
    for (int k = 0; k < 4; k++) reads = reads || vs.float_reads.test(size_t(wvp_reg_ + k));
    if (!reads || !w_noted_.insert(vs.hash).second) return;
    if (w_noted_.size() > 256) {
      std::fprintf(stderr, "gfx: depth from w: more vertex shaders kept w - z, not logged\n");
      return;
    }
    char other[64] = "";
    if (vs.position_matrix >= 0)
      std::snprintf(other, sizeof other, "its position is from c%d-c%d", vs.position_matrix, vs.position_matrix + 3);
    std::fprintf(stderr, "gfx: depth from w: vertex shader %016llx reads c%d-c%d, keeps w - z: %s\n",
                 (unsigned long long)vs.hash, wvp_reg_, wvp_reg_ + 3,
                 *other ? other : vs.position_note.empty() ? "(no analysis)" : vs.position_note.c_str());
  }

  // The camera (world to clip space, D3D's row vectors) from a scene
  // draw's constants, RAGE's layout: c0-c3 world, c8-c11 world-view-
  // projection.
  void CaptureCamera(const float (*c)[4]) {
    double w[16], wvp[16], iw[16];
    for (int r = 0; r < 4; r++)
      for (int k = 0; k < 4; k++) {
        w[r * 4 + k] = c[r][k];
        wvp[r * 4 + k] = c[8 + r][k];
      }
    if (!Invert4(w, iw)) return;
    Mul4(iw, wvp, vp_cur_);
    have_vp_ = true;
  }

  static void Mul4(const double* a, const double* b, double* o) {
    for (int r = 0; r < 4; r++)
      for (int k = 0; k < 4; k++) {
        double v = 0;
        for (int i = 0; i < 4; i++) v += a[r * 4 + i] * b[i * 4 + k];
        o[r * 4 + k] = v;
      }
  }

  static bool Invert4(const double* m, double* o) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
      for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r ? 1.0 : 0.0);
    for (int col = 0; col < 4; col++) {
      int piv = col;
      for (int r = col + 1; r < 4; r++)
        if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
      if (std::fabs(a[piv][col]) < 1e-12) return false;
      if (piv != col)
        for (int k = 0; k < 8; k++) std::swap(a[piv][k], a[col][k]);
      const double inv = 1.0 / a[col][col];
      for (int k = 0; k < 8; k++) a[col][k] *= inv;
      for (int r = 0; r < 4; r++)
        if (r != col) {
          const double f = a[r][col];
          for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k];
        }
    }
    for (int r = 0; r < 4; r++)
      for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r][k + 4];
    return true;
  }

  // Near and far planes of a perspective world-to-clip matrix (row vectors,
  // depth 0..1 at near..far, w = view depth). Column 2 (z) is a * column 3
  // (w) + b * (0, 0, 0, 1) with a = f / (f - n), b = -n a. False if it is
  // not one.
  static bool NearFar(const double* vp, double* near_plane, double* far_plane) {
    auto at = [&](int r, int c) { return vp[r * 4 + c]; };
    int r = 0;
    for (int i = 1; i < 3; i++)
      if (std::fabs(at(i, 3)) > std::fabs(at(r, 3))) r = i;
    if (!(std::fabs(at(r, 3)) > 1e-9)) return false;
    const double a = at(r, 2) / at(r, 3), b = at(3, 2) - a * at(3, 3);
    if (!(std::fabs(a) > 1e-9) || !(std::fabs(1.0 - a) > 1e-12)) return false;
    *near_plane = -b / a;
    *far_plane = b / (1.0 - a);
    return *near_plane > 0 && *far_plane > *near_plane;
  }

  // A cut: m (this frame's clip space to the last frame's, row vectors)
  // moves points of the screen at depth value z by more than a third of
  // it, or behind the last camera.
  static bool CameraCut(const double* m, double z) {
    static constexpr double kPts[5][2] = {{0, 0}, {-0.8, -0.8}, {0.8, -0.8}, {-0.8, 0.8}, {0.8, 0.8}};
    for (const auto& q : kPts) {
      const double v[4] = {q[0], q[1], z, 1.0};
      double o[4] = {};
      for (int k = 0; k < 4; k++)
        for (int i = 0; i < 4; i++) o[k] += v[i] * m[i * 4 + k];
      if (!(o[3] > 1e-9)) return true;
      const double dx = o[0] / o[3] - q[0], dy = o[1] / o[3] - q[1];
      if (dx * dx + dy * dy > (2.0 / 3.0) * (2.0 / 3.0)) return true;
    }
    return false;
  }

  static float Halton(int i, int b) {
    float f = 1, r = 0;
    for (; i > 0; i /= b) {
      f /= float(b);
      r += f * float(i % b);
    }
    return r;
  }

  // After each draw: the composite's done once it has covered the whole
  // back buffer.
  void AfterDraw() {
    if (pass_.low && viewport_.x == 0 && viewport_.y == 0 && int(viewport_.width) >= bb_w_ &&
        int(viewport_.height) >= bb_h_)
      Composite();
  }

  // A pipeline copying a depth buffer into one of `pf` (DepthProxy, the
  // upscaler's depth).
  id<MTLRenderPipelineState> DepthCopyPipe(MTLPixelFormat pf) {
    auto it = depth_copy_pipes_.find(pf);
    if (it != depth_copy_pipes_.end()) return it->second;
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [internal_lib_ newFunctionWithName:@"tri_vs"];
    pd.fragmentFunction = [internal_lib_ newFunctionWithName:@"depth_copy_fs"];
    pd.depthAttachmentPixelFormat = pf;
    if (pf == MTLPixelFormatDepth32Float_Stencil8) pd.stencilAttachmentPixelFormat = pf;
    NSError* err = nil;
    id<MTLRenderPipelineState> ps = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps) FatalF("gfx: depth copy pipeline: %s", err.localizedDescription.UTF8String);
    depth_copy_pipes_[pf] = ps;
    return ps;
  }

  // Encodes a copy of depth buffer `src` into `dst` (any size; stencil
  // cleared).
  void CopyDepth(__unsafe_unretained id<MTLTexture> src, __unsafe_unretained id<MTLTexture> dst) {
    __unsafe_unretained id<MTLRenderPipelineState> pipe = DepthCopyPipe(dst.pixelFormat);
    __unsafe_unretained id<MTLDepthStencilState> ds = depth_copy_ds_;
    __unsafe_unretained id<MTLSamplerState> smp = quad_sampler_[0];
    const bool stencil = dst.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    Rec([=, this] {
      MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
      rp.depthAttachment.texture = dst;
      rp.depthAttachment.loadAction = MTLLoadActionDontCare;
      rp.depthAttachment.storeAction = MTLStoreActionStore;
      if (stencil) {
        rp.stencilAttachment.texture = dst;
        rp.stencilAttachment.loadAction = MTLLoadActionClear;
        rp.stencilAttachment.clearStencil = 0;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
      }
      id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
      [e setRenderPipelineState:pipe];
      [e setDepthStencilState:ds];
      [e setFragmentTexture:src atIndex:0];
      [e setFragmentSamplerState:smp atIndex:0];
      [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [e endEncoding];
    });
  }

  // A full-size copy (w x h) of depth buffer `src` at the internal
  // resolution, kept per size, copied once a frame (when first wanted:
  // after the scene). From BeginPassEncoder: no encoder is open.
  id<MTLTexture> DepthProxy(id<MTLTexture> src, NSUInteger w, NSUInteger h) {
    std::vector<DepthCopy>& sizes = depth_proxies_[(__bridge void*)src];
    auto px = std::find_if(sizes.begin(), sizes.end(),
                           [&](const DepthCopy& c) { return c.tex.width == w && c.tex.height == h; });
    if (px == sizes.end()) {
      MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];
      td.storageMode = MTLStorageModePrivate;
      td.usage = MTLTextureUsageRenderTarget;
      px = sizes.insert(sizes.end(), DepthCopy{[dev_ newTextureWithDescriptor:td], 0});
    }
    if (px->frame != serial_) {
      px->frame = serial_;
      CopyDepth(src, px->tex);
    }
    return px->tex;
  }

  // mf: this frame's clip space to the last frame's, from the camera alone
  // (identity without a last frame). True if the history starts over: no
  // last frame, or a cut (cutscenes, respawns, camera switches): a point
  // 20 m into the scene (its depth value from the camera's planes) moved by
  // more than a third of the screen.
  // With `reversed`, mf takes the depth as stored (1 - d, reversed_z.h):
  // R * m, R taking (x, y, 1 - d, 1) to (x, y, d, 1).
  bool CameraMotion(float mf[16], bool reversed) {
    double inv[16], m[16];
    const bool moved = have_prev_ && Invert4(vp_cur_, inv);
    if (moved) Mul4(inv, vp_prev_, m);
    for (int i = 0; i < 16; i++) mf[i] = moved ? float(m[i]) : (i % 5 == 0 ? 1.0f : 0.0f);
    if (reversed)
      for (int k = 0; k < 4; k++) {
        mf[12 + k] += mf[8 + k];
        mf[8 + k] = -mf[8 + k];
      }
    double cut_z = 0.99, n, f;
    if (NearFar(vp_cur_, &n, &f)) {
      const double d = std::clamp(20.0, n * 2.0, f * 0.5);
      cut_z = f / (f - n) * (1.0 - n / d);
    }
    const bool reset = fx_reset_ || !have_prev_ || (moved && CameraCut(m, cut_z));
    fx_reset_ = false;
    return reset;
  }

  // TAA's pipeline, the composite's copy it reads and its history (two),
  // at the back buffer's size. False if the back buffer cannot take its
  // output.
  bool EnsureTaa() {
    if (!(backbuffer_.usage & MTLTextureUsageShaderWrite)) return false;
    const NSUInteger w = NSUInteger(bb_w_), h = NSUInteger(bb_h_);
    if (taa_pipe_ && taa_src_.width == w && taa_src_.height == h) return true;
    if (!taa_pipe_) {
      NSError* err = nil;
      taa_pipe_ = [dev_ newComputePipelineStateWithFunction:[internal_lib_ newFunctionWithName:@"taa_cs"] error:&err];
      if (!taa_pipe_) FatalF("gfx: TAA pipeline: %s", err.localizedDescription.UTF8String);
      taa_sharpen_pipe_ = [dev_ newComputePipelineStateWithFunction:[internal_lib_ newFunctionWithName:@"taa_sharpen_cs"]
                                                              error:&err];
      if (!taa_sharpen_pipe_) FatalF("gfx: TAA sharpening pipeline: %s", err.localizedDescription.UTF8String);
    }
    for (int i = 0; i < 2; i++) {
      if (taa_history_[i]) Keep(taa_history_[i]);
      MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];
      td.storageMode = MTLStorageModePrivate;
      td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
      taa_history_[i] = [dev_ newTextureWithDescriptor:td];
    }
    if (taa_src_) Keep(taa_src_);
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:backbuffer_.pixelFormat
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead;
    taa_src_ = [dev_ newTextureWithDescriptor:td];
    fx_reset_ = true;
    std::fprintf(stderr, "gfx: TAA %lux%lu, sharpening %.2f, texture LOD bias %.1f\n", (unsigned long)w,
                 (unsigned long)h, taa_sharpness_, kTaaLodBias);
    return true;
  }

  // The upscaler for low_color_'s size to the back buffer's, and its
  // inputs' textures. False if MetalFX cannot (bilinear then). It writes
  // into the back buffer itself where that allows (made so for MetalFX),
  // else into fx_out_, copied there.
  bool EnsureScaler() {
    if (scaler_failed_) return false;
    const NSUInteger iw = low_color_.width, ih = low_color_.height;
    if (scaler_ && scaler_.inputWidth == iw && scaler_.inputHeight == ih &&
        scaler_.outputWidth == NSUInteger(bb_w_) && scaler_.outputHeight == NSUInteger(bb_h_))
      return true;
    if (@available(macOS 13.0, iOS 16.0, *)) {
      MTLFXTemporalScalerDescriptor* sd = [MTLFXTemporalScalerDescriptor new];
      sd.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
      sd.depthTextureFormat = MTLPixelFormatDepth32Float;
      sd.motionTextureFormat = MTLPixelFormatRG16Float;
      sd.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
      sd.inputWidth = iw;
      sd.inputHeight = ih;
      sd.outputWidth = NSUInteger(bb_w_);
      sd.outputHeight = NSUInteger(bb_h_);
      id<MTLFXTemporalScaler> sc = [sd newTemporalScalerWithDevice:dev_];
      if (!sc) {
        WarnF("gfx: MetalFX temporal scaler %lux%lu -> %dx%d unavailable: bilinear\n", (unsigned long)iw,
              (unsigned long)ih, bb_w_, bb_h_);
        scaler_failed_ = true;
        return false;
      }
      sc.motionVectorScaleX = float(iw);
      sc.motionVectorScaleY = float(ih);
      sc.depthReversed = NO;
      for (id o : {(id)scaler_, (id)motion_tex_, (id)fx_depth_, (id)fx_out_})
        if (o) Keep(o);
      scaler_ = sc;
      fx_out_ = nil;
      auto tex = [&](MTLPixelFormat pf, NSUInteger w, NSUInteger h, MTLTextureUsage usage) {
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = usage;
        return [dev_ newTextureWithDescriptor:td];
      };
      motion_tex_ = tex(MTLPixelFormatRG16Float, iw, ih,
                        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | sc.motionTextureUsage);
      fx_depth_ = tex(MTLPixelFormatDepth32Float, iw, ih,
                      MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | sc.depthTextureUsage);
      if ((backbuffer_.usage & sc.outputTextureUsage) != sc.outputTextureUsage)
        fx_out_ = tex(MTLPixelFormatBGRA8Unorm, NSUInteger(bb_w_), NSUInteger(bb_h_),
                      MTLTextureUsageShaderRead | sc.outputTextureUsage);
      if (!motion_pipe_) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [internal_lib_ newFunctionWithName:@"tri_vs"];
        pd.fragmentFunction = [internal_lib_ newFunctionWithName:@"motion_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Float;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError* err = nil;
        motion_pipe_ = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!motion_pipe_) FatalF("gfx: motion pipeline: %s", err.localizedDescription.UTF8String);
      }
      fx_reset_ = true;
      std::fprintf(stderr, "gfx: MetalFX temporal scaler %lux%lu -> %dx%d%s\n", (unsigned long)iw,
                   (unsigned long)ih, bb_w_, bb_h_, fx_out_ ? " (then copied)" : "");
      return true;
    }
    scaler_failed_ = true;
    return false;
  }

  // MetalFX's spatial scaler for low_color_'s size to the back buffer's,
  // and the SMAA'd composite it reads (spatial_in_), its output into the
  // back buffer. False if MetalFX cannot (bilinear then).
  bool EnsureSpatial() {
    if (spatial_failed_) return false;
    const NSUInteger iw = low_color_.width, ih = low_color_.height;
    if (spatial_ && spatial_.inputWidth == iw && spatial_.inputHeight == ih &&
        spatial_.outputWidth == NSUInteger(bb_w_) && spatial_.outputHeight == NSUInteger(bb_h_))
      return true;
    if (@available(macOS 13.0, iOS 16.0, *)) {
      MTLFXSpatialScalerDescriptor* sd = [MTLFXSpatialScalerDescriptor new];
      sd.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
      sd.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
      sd.inputWidth = iw;
      sd.inputHeight = ih;
      sd.outputWidth = NSUInteger(bb_w_);
      sd.outputHeight = NSUInteger(bb_h_);
      sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;  // the game's 8-bit sRGB-ish colours
      id<MTLFXSpatialScaler> sc = [sd newSpatialScalerWithDevice:dev_];
      if (!sc) {
        WarnF("gfx: MetalFX spatial scaler %lux%lu -> %dx%d unavailable: bilinear\n", (unsigned long)iw,
              (unsigned long)ih, bb_w_, bb_h_);
        spatial_failed_ = true;
        return false;
      }
      for (id o : {(id)spatial_, (id)spatial_in_, (id)spatial_out_})
        if (o) Keep(o);
      spatial_ = sc;
      MTLTextureDescriptor* td =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:iw height:ih mipmapped:NO];
      td.storageMode = MTLStorageModePrivate;
      td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | sc.colorTextureUsage;
      spatial_in_ = [dev_ newTextureWithDescriptor:td];
      spatial_out_ = nil;
      if ((backbuffer_.usage & sc.outputTextureUsage) != sc.outputTextureUsage) {
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                width:NSUInteger(bb_w_)
                                                               height:NSUInteger(bb_h_)
                                                            mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | sc.outputTextureUsage;
        spatial_out_ = [dev_ newTextureWithDescriptor:td];
      }
      std::fprintf(stderr, "gfx: MetalFX spatial scaler %lux%lu -> %dx%d%s\n", (unsigned long)iw,
                   (unsigned long)ih, bb_w_, bb_h_, spatial_out_ ? " (then copied)" : "");
      return true;
    }
    spatial_failed_ = true;
    return false;
  }

  // ---- SMAA 1x (Jimenez et al., SMAA.hlsl), the scene's antialiasing
  // when RECOMP_UPSCALER=smaa: from the composite to the back buffer
  // through luma edges, blending weights (from the area and search lookup
  // textures), then neighbourhood blending; the HUD draws on after.

  id<MTLRenderPipelineState> SmaaPipeline(int pass, MTLPixelFormat target) {
    static const int kSamplers[3] = {1, 3, 2};  // per pass
    id<MTLFunction> fn[2];
    for (int stage = 0; stage < 2; stage++) {
      const bool vertex = stage == 0;
      std::string glsl = "#version 450\n#define SMAA_GLSL_4 1\n#define SMAA_PRESET_HIGH 1\n"
                         "#define SMAA_RT_METRICS smaa_metrics\n#define PASS " +
                         std::to_string(pass) + "\n";
      glsl += vertex ? "#define SMAA_INCLUDE_PS 0\n" : "#define SMAA_INCLUDE_VS 0\n";
      glsl += "layout(std140, set = 0, binding = 0) uniform SmaaMetrics { vec4 smaa_metrics; };\n";
      for (int i = 0; !vertex && i < kSamplers[pass]; i++)
        glsl += "layout(set = 0, binding = " + std::to_string(i) + ") uniform sampler2D t" +
                std::to_string(i) + ";\n";
      glsl += kSmaaHlsl;
      glsl += vertex ? R"(
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_pix;
layout(location = 2) out vec4 v_off[3];
void main() {
  // One triangle over the target; texture y runs down.
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  v_uv = vec2(p.x, 1.0 - p.y);
  v_pix = vec2(0.0);
  v_off[0] = v_off[1] = v_off[2] = vec4(0.0);
#if PASS == 0
  SMAAEdgeDetectionVS(v_uv, v_off);
#elif PASS == 1
  SMAABlendingWeightCalculationVS(v_uv, v_pix, v_off);
#else
  SMAANeighborhoodBlendingVS(v_uv, v_off[0]);
#endif
}
)" : R"(
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pix;
layout(location = 2) in vec4 v_off[3];
layout(location = 0) out vec4 o_color;
void main() {
#if PASS == 0
  o_color = vec4(SMAALumaEdgeDetectionPS(v_uv, v_off, t0), 0.0, 0.0);
#elif PASS == 1
  o_color = SMAABlendingWeightCalculationPS(v_uv, v_pix, v_off, t0, t1, t2, vec4(0.0));
#else
  o_color = SMAANeighborhoodBlendingPS(v_uv, v_off[0], t0, t1);
#endif
}
)";
      std::vector<uint32_t> spirv;
      std::string log, msl;
      if (!GlslToSpirv(glsl, vertex ? ShaderStage::Vertex : ShaderStage::Fragment, &spirv, &log))
        FatalF("gfx: compiling SMAA pass %d: %s", pass, log.c_str());
      if (!SpirvToMsl(spirv, &msl, &log)) FatalF("gfx: SMAA pass %d to MSL: %s", pass, log.c_str());
      NSError* err = nil;
      id<MTLLibrary> lib = [dev_ newLibraryWithSource:@(msl.c_str()) options:nil error:&err];
      if (!lib) FatalF("gfx: SMAA pass %d MSL: %s", pass, err.localizedDescription.UTF8String);
      fn[stage] = [lib newFunctionWithName:@"main0"];
    }
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = fn[0];
    pd.fragmentFunction = fn[1];
    pd.colorAttachments[0].pixelFormat = target;
    if (pass < 2) pd.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;  // smaa_stencil_
    NSError* err = nil;
    id<MTLRenderPipelineState> ps = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps) FatalF("gfx: SMAA pass %d pipeline: %s", pass, err.localizedDescription.UTF8String);
    return ps;
  }

  // Pipelines and lookup textures once; the edge and weight targets at
  // the back buffer's size.
  void SmaaPrepare(NSUInteger w, NSUInteger h) {
    auto tex = [&](MTLPixelFormat pf, NSUInteger w, NSUInteger h, bool target) {
      MTLTextureDescriptor* td =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
      td.storageMode = target ? MTLStorageModePrivate : MTLStorageModeShared;
      td.usage = MTLTextureUsageShaderRead | (target ? MTLTextureUsageRenderTarget : 0);
      return [dev_ newTextureWithDescriptor:td];
    };
    if (!smaa_pipe_[0]) {
      smaa_pipe_[0] = SmaaPipeline(0, MTLPixelFormatRG8Unorm);
      smaa_pipe_[1] = SmaaPipeline(1, MTLPixelFormatRGBA8Unorm);
      smaa_pipe_[2] = SmaaPipeline(2, MTLPixelFormatBGRA8Unorm);
      // The edge pass sets 1 where it found edges (it discards elsewhere);
      // the weight pass runs only there.
      for (int i = 0; i < 2; i++) {
        MTLStencilDescriptor* sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = i ? MTLCompareFunctionEqual : MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = i ? MTLStencilOperationKeep : MTLStencilOperationReplace;
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.frontFaceStencil = dd.backFaceStencil = sd;
        smaa_ds_[i] = [dev_ newDepthStencilStateWithDescriptor:dd];
      }
      smaa_area_ = tex(MTLPixelFormatRG8Unorm, AREATEX_WIDTH, AREATEX_HEIGHT, false);
      [smaa_area_ replaceRegion:MTLRegionMake2D(0, 0, AREATEX_WIDTH, AREATEX_HEIGHT)
                    mipmapLevel:0
                      withBytes:areaTexBytes
                    bytesPerRow:AREATEX_PITCH];
      smaa_search_ = tex(MTLPixelFormatR8Unorm, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, false);
      [smaa_search_ replaceRegion:MTLRegionMake2D(0, 0, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT)
                      mipmapLevel:0
                        withBytes:searchTexBytes
                      bytesPerRow:SEARCHTEX_PITCH];
    }
    if (!smaa_edges_ || smaa_edges_.width != w || smaa_edges_.height != h) {
      for (id t : {(id)smaa_edges_, (id)smaa_blend_, (id)smaa_stencil_})
        if (t) Keep(t);
      smaa_edges_ = tex(MTLPixelFormatRG8Unorm, w, h, true);
      smaa_blend_ = tex(MTLPixelFormatRGBA8Unorm, w, h, true);
      MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];
      td.storageMode = MTLStorageModePrivate;
      td.usage = MTLTextureUsageRenderTarget;
      smaa_stencil_ = [dev_ newTextureWithDescriptor:td];
    }
  }

  // Encodes SMAA from `src` to `dst` (the frame: back to front buffer; a
  // composite: low_color_ to the back buffer).
  void SmaaEncode(__unsafe_unretained id<MTLTexture> src, __unsafe_unretained id<MTLTexture> dst) {
    const NSUInteger sw = src.width, sh = src.height;
    SmaaPrepare(sw, sh);
    struct Metrics {
      float v[4];
    } m{{1.0f / float(sw), 1.0f / float(sh), float(sw), float(sh)}};
    __unsafe_unretained id<MTLTexture> edges = smaa_edges_, blend = smaa_blend_, area = smaa_area_,
                                       search = smaa_search_;
    __unsafe_unretained id<MTLRenderPipelineState> p0 = smaa_pipe_[0], p1 = smaa_pipe_[1], p2 = smaa_pipe_[2];
    __unsafe_unretained id<MTLTexture> stencil = smaa_stencil_;
    __unsafe_unretained id<MTLDepthStencilState> ds0 = smaa_ds_[0], ds1 = smaa_ds_[1];
    __unsafe_unretained id<MTLSamplerState> lin = quad_sampler_[1], point = quad_sampler_[0];
    Rec([=, this] {
      struct Pass {
        __unsafe_unretained id<MTLTexture> target;
        __unsafe_unretained id<MTLRenderPipelineState> pipe;
        __unsafe_unretained id<MTLTexture> in[3];
        __unsafe_unretained id<MTLSamplerState> smp[3];
        int inputs;
      };
      const Pass passes[3] = {
          {edges, p0, {src, nil, nil}, {lin, nil, nil}, 1},
          {blend, p1, {edges, area, search}, {lin, lin, point}, 3},
          {dst, p2, {src, blend, nil}, {lin, lin, nil}, 2},
      };
      for (int i = 0; i < 3; i++) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = passes[i].target;
        // Edges and weights start at 0 (the edge pass discards where there
        // is none); the last pass writes every pixel.
        rp.colorAttachments[0].loadAction = i < 2 ? MTLLoadActionClear : MTLLoadActionDontCare;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (i < 2) {
          rp.stencilAttachment.texture = stencil;
          rp.stencilAttachment.loadAction = i ? MTLLoadActionLoad : MTLLoadActionClear;
          rp.stencilAttachment.clearStencil = 0;
          rp.stencilAttachment.storeAction = i ? MTLStoreActionDontCare : MTLStoreActionStore;
        }
        id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:passes[i].pipe];
        if (i < 2) {
          [e setDepthStencilState:i ? ds1 : ds0];
          [e setStencilReferenceValue:1];
        }
        [e setVertexBytes:m.v length:sizeof m.v atIndex:0];
        [e setFragmentBytes:m.v length:sizeof m.v atIndex:0];
        for (int t = 0; t < passes[i].inputs; t++) {
          [e setFragmentTexture:passes[i].in[t] atIndex:NSUInteger(t)];
          [e setFragmentSamplerState:passes[i].smp[t] atIndex:NSUInteger(t)];
        }
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
      }
    });
  }

  // SMAA over the finished frame, from the back buffer straight into the
  // front one (what Present shows; the back buffer's contents after
  // Present are undefined in D3D9). On the frame as a whole (its HUD too):
  // drawing the scene into another target for the filter (the upscalers'
  // composite redirect, every full-size target marked) cost GTA IV as much
  // as 20 ms a frame at 3440x1440.
  void SmaaFrame() { SmaaEncode(backbuffer_, front_); }

  // The composite is done (at the internal resolution): upscaled into the
  // back buffer, which the HUD then draws on at full size.
  void Composite() {
    composited_ = true;
    Settle();
    EnsureFrame();
    // TAA's composite is the back buffer itself.
    __unsafe_unretained id<MTLTexture> low = upscaler_ == Upscaler::Taa ? backbuffer_ : low_color_,
                                       back = backbuffer_;
    const int bw = bb_w_, bh = bb_h_;
    const int lw = int(low.width), lh = int(low.height);
    if (upscaler_ == Upscaler::Smaa) {
      SmaaEncode(low, back);
    } else if (upscaler_ == Upscaler::Taa && scene_depth_ && have_vp_ && EnsureTaa()) {
      struct {  // taa_cs's TaaParams
        float m[16];
        float inv_size[2];
        float jitter[2];
        float w_min, w_max, w_per_px, gamma;
        uint32_t reset, direct, reversed, pad;
      } tp = {};
      static_assert(sizeof tp == 112);
      const bool reversed = ReversedDepth(scene_depth_.pixelFormat);
      tp.reversed = reversed ? 1 : 0;
      tp.reset = CameraMotion(tp.m, reversed) ? 1 : 0;
      tp.inv_size[0] = 1.0f / float(lw);
      tp.inv_size[1] = 1.0f / float(lh);
      tp.jitter[0] = jitter_[0];  // r_position: +x right, +y down, in pixels
      tp.jitter[1] = jitter_[1];
      // 5% of this frame when still, up to 20% at 10 pixels of motion (as
      // FusionFix's TAA for this game); the clip box one deviation (Salvi's
      // variance clipping: tighter keeps less blurred history).
      tp.w_min = 0.05f;
      tp.w_max = 0.2f;
      tp.w_per_px = 0.1f;
      tp.gamma = 1.0f;
      const float sharp = taa_sharpness_;
      tp.direct = sharp > 0.0f ? 0 : 1;
      __unsafe_unretained id<MTLTexture> depth = scene_depth_, h_in = taa_history_[taa_cur_],
                                         h_out = taa_history_[taa_cur_ ^ 1], src = taa_src_;
      __unsafe_unretained id<MTLComputePipelineState> pipe = taa_pipe_, sharpen = taa_sharpen_pipe_;
      taa_cur_ ^= 1;
      Rec([=, this] {
        // The composite, then resolved from that copy into the back buffer.
        id<MTLBlitCommandEncoder> bl = [cmd_ blitCommandEncoder];
        [bl copyFromTexture:back toTexture:src];
        [bl endEncoding];
        id<MTLComputeCommandEncoder> e = [cmd_ computeCommandEncoder];
        [e setComputePipelineState:pipe];
        [e setTexture:src atIndex:0];
        [e setTexture:depth atIndex:1];
        [e setTexture:h_in atIndex:2];
        [e setTexture:h_out atIndex:3];
        [e setTexture:back atIndex:4];
        [e setBytes:&tp length:sizeof tp atIndex:0];
        [e dispatchThreads:MTLSizeMake(NSUInteger(lw), NSUInteger(lh), 1) threadsPerThreadgroup:MTLSizeMake(16, 8, 1)];
        if (sharp > 0.0f) {  // the same encoder: Metal orders the two dispatches' texture use
          [e setComputePipelineState:sharpen];
          [e setTexture:h_out atIndex:0];
          [e setTexture:back atIndex:1];
          [e setBytes:&sharp length:sizeof sharp atIndex:0];
          [e dispatchThreads:MTLSizeMake(NSUInteger(lw), NSUInteger(lh), 1)
              threadsPerThreadgroup:MTLSizeMake(16, 8, 1)];
        }
        [e endEncoding];
      });
    } else if (upscaler_ == Upscaler::MetalFX && scene_depth_ && have_vp_ && EnsureScaler()) {
      struct M16 {
        float v[16];
      } mm;
      // The depth MetalFX takes is the scene's as stored.
      const bool reversed = ReversedDepth(scene_depth_.pixelFormat);
      const bool reset = CameraMotion(mm.v, reversed);
      __unsafe_unretained id<MTLTexture> depth = scene_depth_, motion = motion_tex_, fxd = fx_depth_,
                                         out = fx_out_ ? fx_out_ : back;
      __unsafe_unretained id<MTLRenderPipelineState> mp = motion_pipe_;
      __unsafe_unretained id<MTLDepthStencilState> ds = depth_copy_ds_;
      __unsafe_unretained id<MTLFXTemporalScaler> sc = scaler_;
      const float jx = jitter_[0] * jitter_sign_[0], jy = jitter_[1] * jitter_sign_[1];
      const float mvs = mv_sign_;
      Rec([=, this] {
        // Motion, and the depth MetalFX takes, in one pass.
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = motion;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = fxd;
        rp.depthAttachment.loadAction = MTLLoadActionDontCare;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:mp];
        [e setDepthStencilState:ds];
        [e setFragmentTexture:depth atIndex:0];
        [e setFragmentBytes:mm.v length:sizeof mm.v atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        sc.colorTexture = low;
        sc.depthTexture = fxd;
        sc.motionTexture = motion;
        sc.outputTexture = out;
        sc.jitterOffsetX = jx;
        sc.jitterOffsetY = jy;
        sc.motionVectorScaleX = mvs * float(lw);
        sc.motionVectorScaleY = mvs * float(lh);
        sc.depthReversed = reversed;
        sc.reset = reset;
        [sc encodeToCommandBuffer:cmd_];
        if (out == back) return;
        id<MTLBlitCommandEncoder> bl = [cmd_ blitCommandEncoder];
        [bl copyFromTexture:out toTexture:back];
        [bl endEncoding];
      });
    } else if (upscaler_ == Upscaler::MetalFXSpatial && EnsureSpatial()) {
      // SMAA at the internal resolution, then the spatial upscale.
      SmaaEncode(low, spatial_in_);
      __unsafe_unretained id<MTLTexture> in = spatial_in_, out = spatial_out_ ? spatial_out_ : back;
      __unsafe_unretained id<MTLFXSpatialScaler> sc = spatial_;
      Rec([=, this] {
        sc.colorTexture = in;
        sc.outputTexture = out;
        [sc encodeToCommandBuffer:cmd_];
        if (out == back) return;
        id<MTLBlitCommandEncoder> bl = [cmd_ blitCommandEncoder];
        [bl copyFromTexture:out toTexture:back];
        [bl endEncoding];
      });
    } else if (low != back) {
      Rec([=, this] {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = back;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
        DrawQuad(e, MTLPixelFormatBGRA8Unorm, low, 0, float(lw), float(lh), {0, 0, lw, lh}, float(bw),
                 float(bh), {0, 0, bw, bh}, true);
        [e endEncoding];
      });
    }
    BindTarget();  // the back buffer itself now
  }

  // At the end of a frame: the next one's jitter and camera history.
  void EndUpscalerFrame() {
    if (upscaler_ == Upscaler::Off) return;
    if (frame_scene_ && !composited_ && pass_.low) Composite();  // never covered: as it is
    if (have_vp_) std::memcpy(vp_prev_, vp_cur_, sizeof vp_cur_);
    have_prev_ = have_vp_ && composited_;
    have_vp_ = false;
    frame_scene_ = composited_ = false;
    scene_depth_ = nil;
    // 8 phases for TAA (a short cycle shimmers less when the history is
    // clipped), 16 for MetalFX.
    jitter_phase_ = (jitter_phase_ + 1) % (upscaler_ == Upscaler::Taa ? 8 : 16);
    jitter_[0] = Halton(jitter_phase_ + 1, 2) - 0.5f;
    jitter_[1] = Halton(jitter_phase_ + 1, 3) - 0.5f;
  }
  struct Tex {
    id<MTLTexture> tex = nil;
    int w = 0, h = 0, levels = 1;
    Format fmt = Format::Unknown;
    bool rt = false;
    bool cube = false;
    DepthId depth = 0;  // CreateDepthTexture: the depth buffer sharing `tex`
    bool compare = false;  // SetDepthCompare: read as D3D9 compares (R_SHADOW)
    uint64_t used = 0;  // the last frame (serial_) that drew with or into it
    bool opaque = false;  // alpha read as 1: X8R8G8B8 data uploaded as is
    bool scaled = false;  // made at another size than w x h (ScaledSize)
    bool scene = false;   // one of the scene's buffers (the back buffer's size or a fraction)
  };
  struct Depth {
    id<MTLTexture> tex = nil;
    int w = 0, h = 0;
    bool stencil = false;
    float bias_scale = 8388608.0f;  // float depth: 2^23 per unit of depth range
    bool scaled = false;  // at the internal resolution (ScaledSize)
    WDepth wd;            // the projection draws from w use (reversed_z.h)
  };
  // A texture by id (null if none): through tex_at_, not the map.
  Tex* FindTex(TextureId tid) const { return tid < tex_at_.size() ? tex_at_[tid] : nullptr; }
  // The id of a new texture: a destroyed one's, whose row in tex_at_ is there,
  // else the next (a stream of textures would otherwise grow tex_at_ for
  // ever). Textures, depth buffers and shaders share next_id_'s numbering, so
  // only a texture's id comes back.
  TextureId NewTexId() {
    if (free_tex_ids_.empty()) return next_id_++;
    const TextureId tid = free_tex_ids_.back();
    free_tex_ids_.pop_back();
    return tid;
  }
  void AddTex(TextureId tid, const Tex& t) {
    Tex& e = textures_[tid] = t;
    if (tex_at_.size() <= tid) tex_at_.resize(size_t(tid) + 1);
    tex_at_[tid] = &e;
  }
  struct Shader {
    id<MTLFunction> fn = nil;
    hlsl::Stage stage = hlsl::Stage::Vertex;
    size_t inputs = 0;
    std::array<hlsl::ParamType, 16> sampler_type{};  // per register; Void if unused
    uint32_t samplers = 0;  // highest register used + 1
    ShaderId without_depth = 0;  // (PixelShaderWithoutDepth)
    uint32_t float_regs = 0;
    int position_matrix = -1;  // hlsl::Program's, vertex shaders
    std::string position_note;
    std::bitset<256> float_reads;
    uint64_t hash = 0;  // of its translation: the same from run to run (0: the device's own)
    uint32_t varyings = 0;  // locations written (vertex) or read (pixel): hlsl::VaryingMask
    // Pixel shaders: the translation, and its variants for vertex shaders
    // that leave some of its inputs unwritten, by the inputs they do write
    // (PixelShaderFor).
    std::shared_ptr<const hlsl::Program> program;
    std::unordered_map<uint32_t, ShaderId> variants;
    bool has_lean = false;  // also made lean (MslOptions(false)): kLeanPs
  };

  // The pixel shader `sid` (ps) as drawn after a vertex shader writing
  // the varying locations `written`: itself if that covers its inputs,
  // else a variant made once with the others as constants (Metal rejects
  // a pipeline whose fragment inputs the vertex function lacks; D3D9 does
  // not).
  ShaderId PixelShaderFor(ShaderId sid, Shader& ps, uint32_t written) {
    written &= ps.varyings;
    if (written == ps.varyings || !ps.program) return sid;
    auto it = ps.variants.find(written);
    if (it != ps.variants.end()) return it->second;
    hlsl::Program v = *ps.program;
    v.glsl = hlsl::GlslWithDefaultInputs(v, written);
    std::erase_if(v.varyings, [&](hlsl::Semantic s) {
      int l = hlsl::VaryingLocation(s);
      return l >= 0 && !(written & (1u << l));
    });
    ShaderId vid = CreateShader(v);  // ps stays valid: shaders_ is node-based
    ps.variants[written] = vid;
    return vid;
  }

  // A pixel shader writing depth for a pass without a depth buffer: D3D9
  // ignores what it writes, a Metal pipeline cannot be made with it.
  ShaderId PixelShaderWithoutDepth(ShaderId sid) {
    Shader& ps = shaders_.find(sid)->second;
    if (!ps.program || !ps.program->writes_depth) return sid;
    if (ps.without_depth) return ps.without_depth;
    hlsl::Program v = *ps.program;
    v.writes_depth = false;
    std::string glsl;
    size_t at = 0;
    while (at < v.glsl.size()) {
      size_t end = v.glsl.find('\n', at);
      end = end == std::string::npos ? v.glsl.size() : end + 1;
      if (v.glsl.find("gl_FragDepth", at) >= end) glsl.append(v.glsl, at, end - at);
      at = end;
    }
    v.glsl = std::move(glsl);
    const ShaderId vid = CreateShader(v);  // ps stays valid: shaders_ is node-based
    ps.without_depth = vid;
    return vid;
  }

  // The render targets drawn to.
  struct Pass {
    id<MTLTexture> color[4] = {};
    int level[4] = {};
    MTLPixelFormat fmt[4] = {};
    TextureId ids[4] = {};
    int targets = 0;
    id<MTLTexture> depth = nil;
    MTLPixelFormat depth_fmt = MTLPixelFormatInvalid;
    bool stencil = false, alpha = false;
    float bias_scale = 1.0f;
    int w = 0, h = 0;  // the game's size of the target
    // A target at the internal resolution: its pixels per game pixel.
    float sx = 1.0f, sy = 1.0f;
    bool scaled = false;
    bool low = false;    // the back buffer, drawn to low_color_ for the upscaler
    bool scene = false;  // a scene buffer (not a shadow map or reflection)
    bool proxy = false;  // depth drawn as its full-size copy (DepthProxy)
    bool reversed = false;  // depth stored as 1 - d (ReversedDepth)
    DepthId depth_id = 0;
    bool Same(const Pass& o) const {
      for (int i = 0; i < 4; i++)
        if (color[i] != o.color[i] || level[i] != o.level[i]) return false;
      return depth == o.depth && proxy == o.proxy;
    }
  };
  struct Slice {
    __unsafe_unretained id<MTLBuffer> buf;  // owned by the arena
    size_t off;
    uint8_t* cpu;
  };
  // A command buffer's per-draw memory: chunks used in turn.
  struct Arena {
    std::vector<id<MTLBuffer>> chunks;
    size_t chunk = 0, used = 0;
  };
  struct Read {
    Rect r{};
    std::vector<uint32_t> px;  // A8R8G8B8
  };
  // What the render encoder has bound, so unchanged state is not sent again.
  struct Bound {
    PipeKey pipe{};
    bool pipe_set = false;
    void* ds = nullptr;
    int cull = -1, fill = -1;
    float bias = NAN, slope = NAN;
    MTLViewport vp{};
    bool has_vp = false;
    MTLScissorRect sc{};
    bool has_sc = false;
    int stencil_ref = -1;
    float blend[4] = {NAN, NAN, NAN, NAN};
    void* vbuf[4] = {};
    size_t voff[4] = {};
    void* fbuf[3] = {};
    size_t foff[3] = {};
    void* vtex[4] = {};
    void* vsmp[4] = {};
    void* ftex[16] = {};
    void* fsmp[16] = {};
  };

  // DrawShader's second half when its vertices are in shared memory: byte
  // offsets from the shared buffer's start, base vertex and indices given
  // to Metal (indices in shared memory read in place too).
  void DrawInPlace(const ShaderDraw& d, const Shader& vs, MTLPrimitiveType type, uint32_t n,
                   uint32_t count) {
    const Shader& ps = shaders_.find(d.ps)->second;
    VsState st{};
    for (size_t i = 0; i < vs.inputs; i++) {
      VertexInput in = i < d.input_count ? d.inputs[i] : VertexInput{};
      if (in.type >= 17 || in.stream >= 16 || !d.streams[in.stream].data) {
        st.in[i][2] = -1;
        continue;
      }
      const VertexStream& vsm = d.streams[in.stream];
      st.in[i][0] = int32_t(vsm.data - shared_lo_) + int32_t(in.offset);
      st.in[i][1] = int32_t(vsm.stride);
      st.in[i][2] = int32_t(in.type);
    }
    PixelState(st.pixel);
    ClipState(st, d, vs);
    VertexBuffer(3, shared_buf_, 0);
    ShaderResources(d, vs, ps, st);

    const auto* idx = static_cast<const uint8_t*>(d.indices);
    if (d.prim == 6) {
      Slice ib = FanIndices(d, count);
      const int32_t base = d.base_vertex;
      Rec([this, type, count, ib, base] {
        [enc_ drawIndexedPrimitives:type
                         indexCount:count * 3
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:ib.buf
                  indexBufferOffset:ib.off
                      instanceCount:1
                         baseVertex:base
                       baseInstance:0];
      });
    } else if (idx) {
      const MTLIndexType it = d.index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
      const size_t bytes = size_t(n) * d.index_size;
      __unsafe_unretained id<MTLBuffer> buf = shared_buf_;
      size_t off = size_t(idx - shared_lo_);
      // In place when there and 4-byte aligned (Metal's offset rule).
      if (idx < shared_lo_ || idx + bytes > shared_hi_ || (off & 3)) {
        Slice s = Alloc(bytes, 4);
        std::memcpy(s.cpu, idx, bytes);
        buf = s.buf;
        off = s.off;
      }
      const int32_t base = d.base_vertex;
      Rec([this, type, n, it, buf, off, base] {
        [enc_ drawIndexedPrimitives:type
                         indexCount:n
                          indexType:it
                        indexBuffer:buf
                  indexBufferOffset:off
                      instanceCount:1
                         baseVertex:base
                       baseInstance:0];
      });
    } else {
      const NSUInteger start = NSUInteger(std::max(d.base_vertex, 0));
      Rec([this, type, n, start] { [enc_ drawPrimitives:type vertexStart:start vertexCount:n]; });
    }
  }

  // A fan's triangles as a 32-bit index list (vertex numbers before the
  // base vertex).
  Slice FanIndices(const ShaderDraw& d, uint32_t count) {
    Slice ib = Alloc(size_t(count) * 12, 4);
    auto* out = reinterpret_cast<uint32_t*>(ib.cpu);
    auto at = [&](uint32_t i) -> uint32_t {
      return !d.indices ? i
             : d.index_size == 4 ? static_cast<const uint32_t*>(d.indices)[i]
                                 : static_cast<const uint16_t*>(d.indices)[i];
    };
    for (uint32_t t = 0; t < count; t++) {
      out[t * 3] = at(0);
      out[t * 3 + 1] = at(t + 1);
      out[t * 3 + 2] = at(t + 2);
    }
    return ib;
  }

  // A shader draw's uniform blocks, textures and samplers.
  // `tex` is one of the pass's targets or its depth buffer.
  bool Attached(__unsafe_unretained id<MTLTexture> tex) const {
    for (int i = 0; i < pass_.targets; i++)
      if (tex == pass_.color[i]) return true;
    return tex == pass_.depth;
  }

  void ShaderResources(const ShaderDraw& d, const Shader& vs, const Shader& ps, const VsState& st) {
    auto ints = [](const int32_t (*i)[4], const uint32_t* b) {
      std::array<int32_t, 80> u{};
      if (i) std::memcpy(u.data(), i, 16 * 16);
      for (int k = 0; b && k < 16; k++) u[64 + k] = b[k] ? 1 : 0;
      return u;
    };
    Uniform(0, d.vs_float, vs.float_regs * 16, d.vs_float_version);
    // Int and bool registers: rebuilt only when they changed.
    if (!d.vs_ib_version || d.vs_ib_version != ib_version_[0] || d.vs_int != ib_src_[0]) {
      ib_[0] = ints(d.vs_int, d.vs_bool);
      ib_version_[0] = d.vs_ib_version;
      ib_src_[0] = d.vs_int;
      ib_serial_[0]++;
    }
    Uniform(1, ib_[0].data(), sizeof ib_[0], ib_serial_[0]);
    Uniform(2, &st, sizeof st);
    Uniform(3, d.ps_float, ps.float_regs * 16, d.ps_float_version);
    if (!d.ps_ib_version || d.ps_ib_version != ib_version_[1] || d.ps_int != ib_src_[1]) {
      ib_[1] = ints(d.ps_int, d.ps_bool);
      ib_version_[1] = d.ps_ib_version;
      ib_src_[1] = d.ps_int;
      ib_serial_[1]++;
    }
    Uniform(4, ib_[1].data(), sizeof ib_[1], ib_serial_[1]);
    PsState pst{};
    pst.alpha[0] = rs_[15] ? int32_t(rs_[25]) : 8;
    pst.alpha[1] = int32_t(rs_[24] & 0xFF);
    pst.alpha[3] = pass_.reversed ? 1 : 0;
    // Depth textures sampled: read back as D3D9 stores depth (R_DEPTH_TEX).
    for (uint32_t r = 0; r < ps.samplers; r++)
      if (const Tex* t = FindTex(d.textures[r]); t && t->depth) {
        if (ReversedDepth(t->tex.pixelFormat)) pst.alpha[2] |= int32_t(1u << r);
        if (t->compare) pst.alpha[2] |= int32_t(1u << (16 + r));
      }
    const float lod_bias = jitter_draw_ && upscaler_ == Upscaler::Taa ? kTaaLodBias : 0.0f;
    for (uint32_t r = 0; r < 16 && d.sampler_states; r++) pst.bias[r] = AsFloat(d.sampler_states[r][8]) + lod_bias;
    pst.vscale[0] = 1.0f / pass_.sx;
    pst.vscale[1] = 1.0f / pass_.sy;
    Uniform(5, &pst, sizeof pst);

    // Samplers: pixel 0-15, then vertex 0-3.
    for (uint32_t r = 0; r < 20; r++) {
      const bool vertex = r >= 16;
      const Shader& sh = vertex ? vs : ps;
      const uint32_t reg = vertex ? r - 16 : r;
      if (reg >= sh.samplers) continue;
      const hlsl::ParamType want = sh.sampler_type[reg];
      // Unretained: textures and samplers outlive the draw (their tables
      // hold them; command buffers retain what they use).
      __unsafe_unretained id<MTLTexture> tex = want == hlsl::ParamType::SamplerCube ? dummy_cube_
                           : want == hlsl::ParamType::Sampler3D ? dummy_3d_
                                                                : dummy_2d_;
      Tex* t = FindTex(d.textures[r]);
      bool reversed = false;  // a reversed depth texture: its border colour mirrored
      if (t && t->cube == (want == hlsl::ParamType::SamplerCube) &&
          want != hlsl::ParamType::Sampler3D) {
        tex = t->tex;
        t->used = serial_;
        reversed = t->depth && ReversedDepth(tex.pixelFormat);
      }
      // The sampler made from the row last time, while the row's change
      // counter has not moved.
      const uint32_t* row = d.sampler_states ? d.sampler_states[r] : nullptr;
      const uint64_t version = d.sampler_versions ? d.sampler_versions[r] : 0;
      SlotSampler& cached = slot_sampler_[r];
      if (!version || cached.version != version || cached.row != row)
        cached = {row, version, Sampler(D3D9Sampler(row))};
      const bool nearest = pass_.scaled && Attached(tex);
      if (nearest || reversed) {
        SamplerKey k = D3D9Sampler(row);
        // A target the pass draws to, read at the pixel being drawn (the
        // game's lighting reads its accumulation and depth so): at the
        // internal resolution the game's coordinates fall off the texel
        // centres, and linear filtering then reads the neighbours, which
        // across a GPU tile's edge another tile may be writing (NaN rows
        // and columns on a 32-pixel grid). Nearest, as at full size.
        if (nearest) k.min = k.mag = uint8_t(MTLSamplerMinMagFilterNearest);
        // Read as 1 - rgb (R_DEPTH_TEX): the border's black and white
        // swapped, so it reads as D3D9's.
        if (reversed) k.border = ReversedDepthBorder(k.border);
        TextureAndSampler(vertex, reg, tex, Sampler(k));
        continue;
      }
      TextureAndSampler(vertex, reg, tex, cached.smp);
    }
  }

  // ---- the encoding thread
  //
  // Recorded work: closures (trivially copyable, so the bytes can move)
  // in a batch, run in order on the encoding thread. They capture what
  // they need by value, Metal objects unretained: their owners keep them,
  // and an owner letting one go while recorded work may still use it hands
  // it to Keep, which holds it until that batch has run.
  struct Op {
    void (*run)(void*);
    uint32_t at;
  };
  struct Batch {
    std::vector<uint8_t> bytes;
    std::vector<Op> ops;
    std::vector<id> keep;
  };

  template <typename F>
  void Rec(F f) {
    static_assert(std::is_trivially_copyable_v<F> && std::is_trivially_destructible_v<F>,
                  "recorded closures capture plain values and unretained objects");
    if (sync_) {  // RECOMP_METAL_SYNC: encoded on the spot, no encoding thread
      @autoreleasepool {
        f();
      }
      return;
    }
    static_assert(alignof(F) <= 16);
    const size_t at = (rec_.bytes.size() + alignof(F) - 1) & ~(alignof(F) - 1);
    rec_.bytes.resize(at + sizeof(F));
    new (rec_.bytes.data() + at) F(f);
    rec_.ops.push_back({[](void* p) { (*static_cast<F*>(p))(); }, uint32_t(at)});
    // A long frame is handed over in parts, so encoding overlaps it.
    if (rec_.ops.size() >= 2048) Flush();
  }

  void Keep(id o) {
    if (o && !sync_) rec_.keep.push_back(o);
  }

  // Hands the recorded batch to the encoding thread.
  void Flush() {
    if (rec_.ops.empty() && rec_.keep.empty()) return;
    if (perf_) pc_.ops += rec_.ops.size();
    // The next batch: one the encoding thread is done with, its memory
    // kept (DXVK's pool of CS chunks), else a new one.
    Batch next;
    bool reused = false;
    {
      std::lock_guard<std::mutex> l(q_mu_);
      batches_.push_back(std::move(rec_));
      submitted_++;
      if (!free_batches_.empty()) {
        next = std::move(free_batches_.back());
        free_batches_.pop_back();
        reused = true;
      }
    }
    if (!reused) next.bytes.reserve(1 << 20);
    rec_ = std::move(next);
    q_cv_.notify_one();
  }

  // Everything recorded so far encoded.
  void WaitIdle() {
    Flush();
    std::unique_lock<std::mutex> l(q_mu_);
    done_cv_.wait(l, [&] { return done_ == submitted_; });
  }

  void EncoderMain() {
    // Its frames are the game's: scheduled like a time-critical thread.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (;;) {
      Batch b;
      {
        std::unique_lock<std::mutex> l(q_mu_);
        q_cv_.wait(l, [&] { return !batches_.empty() || stop_; });
        if (batches_.empty()) return;
        b = std::move(batches_.front());
        batches_.pop_front();
      }
      const uint64_t t0 = perf_.load(std::memory_order_relaxed) ? NowNs() : 0;
      @autoreleasepool {
        for (const Op& o : b.ops) o.run(b.bytes.data() + o.at);
        b.keep.clear();
      }
      if (t0) enc_busy_ns_.fetch_add(NowNs() - t0, std::memory_order_relaxed);
      b.bytes.clear();
      b.ops.clear();
      {
        std::lock_guard<std::mutex> l(q_mu_);
        done_++;
        if (free_batches_.size() < 4) free_batches_.push_back(std::move(b));
      }
      done_cv_.notify_all();
    }
  }

  // Frames recorded but not finished on the GPU: recording, encoding and
  // the GPU each hold one, and one more absorbs their jitter.
  static constexpr int kSlots = 4;
  static constexpr size_t kChunk = 32u << 20;
  static constexpr size_t kKeptChunks = 2;  // per slot, between frames

  void WarnOnce(uint32_t bit, const char* what) {
    if (warned_ & bit) return;
    warned_ |= bit;
    WarnF("gfx: %s is not in the Metal backend\n", what);
  }

  void WarnUnsupportedShaderState() {
    if (rs_[194] && !(warned_ & 0x200)) {
      warned_ |= 0x200;
      WarnF("gfx: SRGBWRITEENABLE is not supported yet\n");
    }
  }

  // `opaque`: alpha read as 1 (X8 formats uploaded as they are).
  int64_t GpuAllocatedBytes() const override { return int64_t(dev_.currentAllocatedSize); }

  void SetBackBufferAlpha(bool on) override { back_buffer_alpha_ = on; }

  std::string GpuMemoryText() const override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    uint64_t tex = 0, rt = 0, depth = 0, draw = 0;
    for (const auto& [id, t] : textures_)
      if (t.tex) (t.rt ? rt : tex) += t.tex.allocatedSize;
    for (const auto& [id, d] : depths_)
      if (d.tex) depth += d.tex.allocatedSize;
    for (const Arena& a : arenas_)
      for (id<MTLBuffer> b : a.chunks) draw += b.length;
    char buf[160];
    std::snprintf(buf, sizeof buf, "textures %llu MB (%zu), targets %llu MB, depth %llu MB, draw data %llu MB",
                  (unsigned long long)(tex >> 20), textures_.size(), (unsigned long long)(rt >> 20),
                  (unsigned long long)(depth >> 20), (unsigned long long)(draw >> 20));
    return buf;
  }

  // DXT is stored as B8G8R8A8 (decoded on upload) where BC cannot be sampled.
  MTLPixelFormat TexFormat(Format f) const {
    return IsCompressed(f) && !bc_ ? MTLPixelFormatBGRA8Unorm : PixelFormat(f);
  }

  void DecodeDxtOnUpload() override {
    if (!textures_.empty()) return;  // those already made keep their format
    bc_ = false;
    WarnF("gfx: DXT decoded on upload\n");
  }

  id<MTLTexture> NewTexture(MTLTextureType type, MTLPixelFormat pf, int w, int h, int levels,
                            bool rt, bool opaque = false, MTLTextureUsage extra_usage = 0) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType = type;
    td.pixelFormat = pf;
    td.width = NSUInteger(w);
    td.height = NSUInteger(h);
    td.mipmapLevelCount = NSUInteger(levels);
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | (rt ? MTLTextureUsageRenderTarget : 0) | extra_usage;
    if (opaque)
      td.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                                 MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
    return [dev_ newTextureWithDescriptor:td];
  }

  Depth NewDepth(int w, int h, Format fmt, bool sampled) {
    Depth d;
    d.w = w;
    d.h = h;
    MTLPixelFormat pf;
    switch (fmt) {
      case Format::D16: pf = MTLPixelFormatDepth16Unorm; d.bias_scale = 65535.0f; break;
      case Format::D24X8: case Format::D32: pf = MTLPixelFormatDepth32Float; break;
      default: pf = MTLPixelFormatDepth32Float_Stencil8; d.stencil = true; break;  // D24S8, D15S1
    }
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2D;
    td.pixelFormat = pf;
    td.width = NSUInteger(w);
    td.height = NSUInteger(h);
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | (sampled ? MTLTextureUsageShaderRead : 0);
    d.tex = [dev_ newTextureWithDescriptor:td];
    if (!d.tex) WarnF("gfx: depth buffer %dx%d fmt %u failed\n", w, h, unsigned(fmt));
    return d;
  }

  // ---- frames: command buffers and their arenas

  // A frame being recorded, its command buffers made on the encoding
  // thread.
  void EnsureFrame() {
    if (!frame_open_) BeginFrame();
  }

  void BeginFrame() {
    dispatch_semaphore_wait(frames_free_, DISPATCH_TIME_FOREVER);
    slot_ = (slot_ + 1) % kSlots;
    // The slot's last frame is done: a frame that took more (a level's
    // texture uploads: hundreds of MB, several times that with DXT decoded)
    // does not keep it. Two standard chunks stay; bigger ones go.
    Arena& arena = arenas_[slot_];
    std::erase_if(arena.chunks, [](id<MTLBuffer> b) { return b.length > kChunk; });
    if (arena.chunks.size() > kKeptChunks) arena.chunks.resize(kKeptChunks);
    arena.chunk = arena.used = 0;
    serial_++;
    frame_open_ = true;
    draw_serial_.store(serial_, std::memory_order_release);
    for (auto& u : last_uniform_) u = {};
    Rec([this] {
      // Uploads run before the frame's draws: their command buffer is
      // queued first.
      upload_cb_ = [queue_ commandBuffer];
      [upload_cb_ enqueue];
      cmd_ = [queue_ commandBuffer];
      [cmd_ enqueue];
    });
  }

  void EndFrame() {
    if (!frame_open_) return;
    // A frame slot filled past 3/4: one more chunk made ahead, for the
    // frame that outgrows it.
    const Arena& a = arenas_[slot_];
    size_t used = a.used, capacity = 0;
    for (size_t i = 0; i < a.chunks.size(); i++) {
      capacity += a.chunks[i].length;
      if (i < a.chunk) used += a.chunks[i].length;
    }
    if (used * 4 > capacity * 3) {
      bool none;
      {
        std::lock_guard<std::mutex> l(spares_->mu);
        none = spares_->ready.empty() && !spares_->pending;
      }
      if (none) MakeSpareChunks(1);
    }
    Settle();
    const uint64_t serial = serial_;
    const bool presented = presented_;
    presented_ = false;
    frame_open_ = false;
    draw_serial_.store(serial_ + 1, std::memory_order_release);
    Rec([this, serial, presented] { EncodeEndFrame(serial, presented); });
    Flush();
  }

  // On the encoding thread: commits the frame's command buffers.
  void EncodeEndFrame(uint64_t serial, bool presented) {
    if (upload_blit_) [upload_blit_ endEncoding];
    upload_blit_ = nil;
    [upload_cb_ commit];
    dispatch_semaphore_t sem = frames_free_;
    // RECOMP_PACE_PRESENT: a frame that showed a drawable frees its slot
    // when the display shows it (Show), not when the GPU is done, so the
    // CPU never runs more frames ahead of the screen than are in flight
    // (DXVK's frame latency limit): no growing queue to the display.
    const bool freed_by_display = pace_present_ && frame_drawable_;
    frame_drawable_ = false;
    std::atomic<uint64_t>* completed = &completed_;
    std::atomic<uint64_t>* gpu_ns = &gpu_ns_;
    std::atomic<uint64_t>* gpu_frames = &gpu_frames_;
    id<MTLCommandBuffer> uploads = upload_cb_;
    PassTimes* pt = gp_on_ ? &gp_[gp_slot_] : nullptr;
    gp_slot_ = (gp_slot_ + 1) % gp_.size();
    [cmd_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
      if (pt) ReportPassTimes(pt, cb.GPUEndTime - cb.GPUStartTime);
      // GPU time of the frame's work (its uploads included).
      const double t = (cb.GPUEndTime - cb.GPUStartTime) + (uploads.GPUEndTime - uploads.GPUStartTime);
      if (t > 0) gpu_ns->fetch_add(uint64_t(t * 1e9));
      if (presented) gpu_frames->fetch_add(1);
      if (cb.status == MTLCommandBufferStatusError)
        std::fprintf(stderr, "gfx: command buffer failed: %s\n",
                     cb.error.localizedDescription.UTF8String);
      uint64_t c = completed->load();
      while (c < serial && !completed->compare_exchange_weak(c, serial)) {
      }
      if (!freed_by_display) dispatch_semaphore_signal(sem);
    }];
    [cmd_ commit];
    pass_no_ = 0;
    last_cb_ = cmd_;
    cmd_ = nil;
    upload_cb_ = nil;
  }

  // `n` arena chunks made on a background queue, their pages touched, so
  // that neither the allocation nor first-use page faults fall on a frame.
  void MakeSpareChunks(int n) {
    std::shared_ptr<SparePool> pool = spares_;
    id<MTLDevice> dev = dev_;
    {
      std::lock_guard<std::mutex> l(pool->mu);
      pool->pending += n;
    }
    for (int i = 0; i < n; i++)
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        id<MTLBuffer> b = [dev newBufferWithLength:kChunk
                                           options:MTLResourceStorageModeShared |
                                                   MTLResourceCPUCacheModeWriteCombined];
        if (b) {
          volatile uint8_t* p = static_cast<uint8_t*>(b.contents);
          for (size_t o = 0; o < kChunk; o += 4096) p[o] = 0;
        }
        std::lock_guard<std::mutex> l(pool->mu);
        pool->pending--;
        if (b) pool->ready.push_back(b);
      });
  }

  id<MTLBuffer> TakeSpareChunk() {
    std::lock_guard<std::mutex> l(spares_->mu);
    if (spares_->ready.empty()) return nil;
    id<MTLBuffer> b = spares_->ready.back();
    spares_->ready.pop_back();
    return b;
  }

  // `bytes` of this frame's arena, aligned to `align` (a power of two).
  Slice Alloc(size_t bytes, size_t align) {
    EnsureFrame();
    Arena& a = arenas_[slot_];
    for (;;) {
      if (a.chunk < a.chunks.size()) {
        id<MTLBuffer> b = a.chunks[a.chunk];
        size_t off = (a.used + align - 1) & ~(align - 1);
        if (off + bytes <= b.length) {
          a.used = off + bytes;
          return {b, off, static_cast<uint8_t*>(b.contents) + off};
        }
        if (a.used == 0 && bytes > b.length) {  // too small for this one: replace it
          if (perf_) {
            pc_.arena_buffers++;
            pc_.arena_bytes += std::max(bytes, kChunk);
          }
          Keep(a.chunks[a.chunk]);
          a.chunks[a.chunk] = [dev_ newBufferWithLength:std::max(bytes, kChunk)
                                                options:MTLResourceStorageModeShared |
                                                        MTLResourceCPUCacheModeWriteCombined];
          continue;
        }
        a.chunk++;
        a.used = 0;
        continue;
      }
      if (id<MTLBuffer> ready = bytes <= kChunk ? TakeSpareChunk() : nil) {
        if (perf_) pc_.arena_ahead++;
        a.chunks.push_back(ready);
        continue;
      }
      if (perf_) {
        pc_.arena_buffers++;
        pc_.arena_bytes += std::max(bytes, kChunk);
      }
      a.chunks.push_back([dev_ newBufferWithLength:std::max(bytes, kChunk)
                                           options:MTLResourceStorageModeShared |
                                                   MTLResourceCPUCacheModeWriteCombined]);
      if (!a.chunks.back()) FatalF("gfx: cannot allocate %zu bytes for draw data", bytes);
    }
  }

  // ---- render targets and encoders

  id<MTLTexture> Target(ColorTarget t, int* w, int* h, Tex** tex) {
    *tex = nullptr;
    if (!t.tex) {
      *w = bb_w_;
      *h = bb_h_;
      return backbuffer_;
    }
    Tex* e = FindTex(t.tex);
    if (!e || !e->rt || t.level < 0 || t.level >= e->levels) return nil;
    *w = std::max(1, e->w >> t.level);
    *h = std::max(1, e->h >> t.level);
    *tex = e;
    return e->tex;
  }

  // The pass for the D3D render targets.
  void BindTarget() {
    Pass p{};
    Tex* t0 = nullptr;
    int w = 0, h = 0;
    id<MTLTexture> c0 = Target({current_color_, current_level_}, &w, &h, &t0);
    if (!c0) c0 = backbuffer_, w = bb_w_, h = bb_h_, t0 = nullptr;
    if (!t0 && c0 == backbuffer_ && upscaler_ != Upscaler::Off && upscaler_ != Upscaler::Smaa &&
        frame_scene_ && !composited_) {
      // The frame's first drawing to the back buffer after its scene: the
      // composite, at the internal resolution, upscaled when done. TAA's
      // is the back buffer itself, resolved in place when done.
      p.low = p.scene = true;
      if (upscaler_ != Upscaler::Taa) {
        EnsureLowColor();
        c0 = low_color_;
        p.scaled = true;
      }
    } else if (t0 && (t0->scaled || t0->scene)) {
      p.scaled = t0->scaled;
      p.scene = t0->scene;
    }
    if (p.scaled) {
      p.sx = float(std::max<NSUInteger>(1, c0.width >> (t0 ? current_level_ : 0))) / float(w);
      p.sy = float(std::max<NSUInteger>(1, c0.height >> (t0 ? current_level_ : 0))) / float(h);
    }
    p.color[0] = c0;
    p.level[0] = t0 ? current_level_ : 0;
    p.fmt[0] = c0 ? c0.pixelFormat : MTLPixelFormatInvalid;
    p.ids[0] = t0 ? current_color_ : 0;
    p.alpha = t0 ? HasAlpha(t0->fmt) : back_buffer_alpha_;
    p.w = w;
    p.h = h;
    p.targets = c0 ? 1 : 0;
    for (int i = 0; i < 3 && p.color[i]; i++) {
      int ew, eh;
      Tex* te = nullptr;
      const ColorTarget& e = current_extra_[i];
      if (!e.tex) break;
      id<MTLTexture> c = Target(e, &ew, &eh, &te);
      if (!c) break;
      p.color[i + 1] = c;
      p.level[i + 1] = e.level;
      p.fmt[i + 1] = c.pixelFormat;
      p.ids[i + 1] = e.tex;
      p.targets = i + 2;
    }
    auto dit = depths_.find(current_depth_);
    if (dit != depths_.end()) {
      p.depth = dit->second.tex;
      p.depth_id = current_depth_;
      p.depth_fmt = p.depth.pixelFormat;
      p.reversed = ReversedDepth(p.depth_fmt);
      p.stencil = dit->second.stencil;
      p.bias_scale = dit->second.bias_scale;
      // A depth buffer at the internal resolution with a full-size target
      // (the HUD's markers on the back buffer): a full-size copy of it, made
      // when the pass opens (the game also binds its targets one at a
      // time, such pairs then never drawn to).
      const NSUInteger cw = std::max<NSUInteger>(1, c0.width >> p.level[0]);
      const NSUInteger ch = std::max<NSUInteger>(1, c0.height >> p.level[0]);
      p.proxy = dit->second.scaled && p.depth.width < cw && p.depth.height < ch;
    }
    SetPass(p);
    bound_serial_ = frame_open_ ? serial_ : 0;
    bound_scene_ = frame_scene_;
    bound_composited_ = composited_;
  }

  // Before a draw or a clear: BindTarget again if what it chose by,
  // besides the bound targets, has moved since (front ends bind only when
  // their bindings change): the frame (DepthProxy copies once a frame;
  // none open yet means the draw opens the next one) and the upscaler's
  // stage (the back buffer stands for low_color_ until the composite).
  void EnsureTarget() {
    if (!frame_open_ || bound_serial_ != serial_ || bound_scene_ != frame_scene_ ||
        bound_composited_ != composited_)
      BindTarget();
  }

  void SetPass(const Pass& p) {
    if (p.Same(pass_) && p.targets == pass_.targets) return;
    Settle();
    pass_ = p;
  }

  // Ends the encoders, first running a pending whole-target clear.
  void Settle() {
    if (!enc_open_ && clear_flags_ && pass_.color[0]) BeginPassEncoder();
    if (enc_open_) Rec([this] {
      [enc_ endEncoding];
      enc_ = nil;
    });
    enc_open_ = false;
    if (blit_open_) Rec([this] {
      [blit_ endEncoding];
      blit_ = nil;
    });
    blit_open_ = false;
  }

  // A render pass's targets and load actions, as recorded for the
  // encoding thread.
  struct PassDesc {
    __unsafe_unretained id<MTLTexture> color[4];
    int level[4];
    int targets;
    __unsafe_unretained id<MTLTexture> depth;
    bool stencil;
    uint32_t clear;
    MTLClearColor clear_color;
    double clear_depth;
    uint8_t clear_stencil;
  };

  void BeginPassEncoder() {
    if (perf_) pc_.passes++;
    if (blit_open_) Rec([this] {
      [blit_ endEncoding];
      blit_ = nil;
    });
    blit_open_ = false;
    EnsureFrame();
    PassDesc pd{};
    for (int i = 0; i < pass_.targets; i++) {
      pd.color[i] = pass_.color[i];
      pd.level[i] = pass_.level[i];
      if (pass_.ids[i]) {
        if (Tex* t = FindTex(pass_.ids[i])) t->used = serial_;
      }
    }
    pd.targets = pass_.targets;
    pd.depth = pass_.depth;
    if (pass_.proxy) {
      id<MTLTexture> c0 = pass_.color[0];
      pd.depth = DepthProxy(pass_.depth, std::max<NSUInteger>(1, c0.width >> pass_.level[0]),
                            std::max<NSUInteger>(1, c0.height >> pass_.level[0]));
    }
    pd.stencil = pass_.stencil;
    pd.clear = clear_flags_;
    pd.clear_color = clear_color_;
    pd.clear_depth = clear_depth_;
    pd.clear_stencil = clear_stencil_;
    clear_flags_ = 0;
    Rec([this, pd] {
      MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
      for (int i = 0; i < pd.targets; i++) {
        MTLRenderPassColorAttachmentDescriptor* a = rp.colorAttachments[NSUInteger(i)];
        a.texture = pd.color[i];
        a.level = NSUInteger(pd.level[i]);
        // A clear clears every target, as D3D9's does.
        a.loadAction = (pd.clear & 1) ? MTLLoadActionClear : MTLLoadActionLoad;
        a.clearColor = pd.clear_color;
        a.storeAction = MTLStoreActionStore;
      }
      if (pd.depth) {
        rp.depthAttachment.texture = pd.depth;
        rp.depthAttachment.loadAction = (pd.clear & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = pd.clear_depth;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (pd.stencil) {
          rp.stencilAttachment.texture = pd.depth;
          rp.stencilAttachment.loadAction = (pd.clear & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
          rp.stencilAttachment.clearStencil = pd.clear_stencil;
          rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
      }
      if (gp_on_) TimePass(rp, pd);
      enc_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
      [enc_ setFrontFacingWinding:MTLWindingClockwise];
      if (labels_) {
        // For GPU traces: the pass's place in the frame, its size and what
        // it loads (L), clears (C) or neither (-): colour, depth.
        id<MTLTexture> t = pd.color[0];
        enc_.label = [NSString
            stringWithFormat:@"pass %d %lux%lu x%d fmt %lu %c%c", pass_no_++,
                             (unsigned long)(t.width >> pd.level[0]),
                             (unsigned long)(t.height >> pd.level[0]), pd.targets,
                             (unsigned long)t.pixelFormat, (pd.clear & 1) ? 'C' : 'L',
                             pd.depth ? ((pd.clear & 2) ? 'C' : 'L') : '-'];
      }
    });
    enc_open_ = true;
    bound_ = {};
  }

  // An encoder for the D3D targets; false if there is no colour target.
  bool BeginDraw() {
    if (!pass_.color[0]) return false;
    if (!enc_open_) BeginPassEncoder();
    return true;
  }

  PipeKey PassKey() const {
    PipeKey k;
    std::memset(&k, 0, sizeof k);
    for (int i = 0; i < 4; i++) k.color[i] = i < pass_.targets ? uint16_t(pass_.fmt[i]) : 0;
    k.depth = uint16_t(pass_.depth_fmt);
    k.stencil = pass_.stencil ? uint16_t(pass_.depth_fmt) : 0;
    k.src = k.src_a = uint8_t(MTLBlendFactorOne);
    k.dst = k.dst_a = uint8_t(MTLBlendFactorZero);
    k.op = k.op_a = uint8_t(MTLBlendOperationAdd);
    return k;
  }

  id<MTLRenderPipelineState> Pipeline(const PipeKey& k) {
    auto it = pipelines_.find(k);
    if (it != pipelines_.end()) return it->second;
    {
      // Built ahead from an earlier run's recipe (PrebuildFor)?
      std::lock_guard<std::mutex> l(prebuilt_mu_);
      auto pb = prebuilt_.find(k);
      if (pb != prebuilt_.end()) {
        id<MTLRenderPipelineState> p = pb->second;
        prebuilt_.erase(pb);
        pipelines_[k] = p;
        prebuilt_used_++;
        if (perf_.load(std::memory_order_relaxed)) pipe_prebuilt_.fetch_add(1, std::memory_order_relaxed);
        return p;
      }
    }
    id<MTLFunction> ps = enc_shaders_.at(k.ps);
    if (k.lean & kLeanPs) {
      id<MTLFunction> lean = FoundLean(enc_shader_hash_.at(k.ps));
      if (!lean) {
        // Not made yet: the full shader (its alpha test then ALWAYS
        // passes) meanwhile, not kept under this key.
        PipeKey full = k;
        full.lean &= uint8_t(~kLeanPs);
        return Pipeline(full);
      }
      ps = lean;
    }
    @autoreleasepool {
      MTLRenderPipelineDescriptor* pd = Describe(k, enc_shaders_.at(k.vs), ps);
      NSError* err = nil;
      const auto t0 = std::chrono::steady_clock::now();
      id<MTLRenderPipelineState> p = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
      if (!p) FatalF("gfx: render pipeline: %s", err.localizedDescription.UTF8String);
      // Compiling while drawing stalls the frame: the slow ones are named.
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (ms > 2)
        std::fprintf(stderr, "gfx: pipeline %zu compiled in %.1f ms (%zu built ahead so far)\n",
                     pipelines_.size() + 1, ms, prebuilt_used_);
      if (perf_.load(std::memory_order_relaxed)) {
        const uint64_t ns = uint64_t(ms * 1e6);
        pipe_compiles_.fetch_add(1, std::memory_order_relaxed);
        pipe_ns_.fetch_add(ns, std::memory_order_relaxed);
        uint64_t m = pipe_max_ns_.load(std::memory_order_relaxed);
        while (ns > m && !pipe_max_ns_.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {
        }
      }
      pipelines_[k] = p;
      RecordRecipe(k);
      return p;
    }
  }

  // A pipeline's descriptor from its key and functions.
  static MTLRenderPipelineDescriptor* Describe(const PipeKey& k, id<MTLFunction> vs, id<MTLFunction> ps) {
    {
      MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
      pd.vertexFunction = vs;
      pd.fragmentFunction = ps;
      for (int i = 0; i < 4; i++) {
        if (!k.color[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor* a = pd.colorAttachments[NSUInteger(i)];
        a.pixelFormat = MTLPixelFormat(k.color[i]);
        a.blendingEnabled = k.blend != 0;
        a.sourceRGBBlendFactor = MTLBlendFactor(k.src);
        a.destinationRGBBlendFactor = MTLBlendFactor(k.dst);
        a.rgbBlendOperation = MTLBlendOperation(k.op);
        a.sourceAlphaBlendFactor = MTLBlendFactor(k.src_a);
        a.destinationAlphaBlendFactor = MTLBlendFactor(k.dst_a);
        a.alphaBlendOperation = MTLBlendOperation(k.op_a);
        a.writeMask = MTLColorWriteMask(k.mask[i]);
      }
      pd.depthAttachmentPixelFormat = MTLPixelFormat(k.depth);
      pd.stencilAttachmentPixelFormat = MTLPixelFormat(k.stencil);
      return pd;
    }
  }

  // ---- pipelines built ahead: each pipeline compiled in a run is kept as
  // a recipe (its shaders' hashes and the rest of its key) in the user's
  // caches; in later runs, once both shaders of a recipe exist, it is
  // compiled in the background and drawing finds it ready.

  struct Recipe {
    uint64_t vs, ps;  // Shader::hash
    PipeKey key;      // vs, ps 0
  };
  static_assert(sizeof(Recipe) == 48, "no padding");

  static uint64_t Fnv64(const void* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001B3ull;
    return h;
  }

  static std::string RecipePath() {
    NSString* dir = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject
        stringByAppendingPathComponent:@"x86recomp"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return [dir stringByAppendingPathComponent:@"metal-pipelines-v1.bin"].fileSystemRepresentation;
  }

  // ---- shaders' lean variants (MetalLeanMacros): pixel shaders without
  // the alpha test, from the same MSL with the macro R_ALPHA_TEST false (a
  // function constant left in it would need every function specialised).

  static MTLCompileOptions* MslOptions(bool full) {
    static MTLCompileOptions* options[2];
    static std::once_flag once;
    std::call_once(once, [] {
      for (int i = 0; i < 2; i++) {
        options[i] = [MTLCompileOptions new];
        options[i].preprocessorMacros = @{@"R_ALPHA_TEST" : i ? @"true" : @"false"};
      }
    });
    return options[full ? 1 : 0];
  }

  // The lean variant of the shader of `hash` (MSL `src`), made once; any
  // thread.
  id<MTLFunction> Lean(uint64_t hash, NSString* src) {
    if (id<MTLFunction> fn = FoundLean(hash)) return fn;
    NSError* err = nil;
    id<MTLLibrary> lib = [dev_ newLibraryWithSource:src options:MslOptions(false) error:&err];
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
    if (!fn) return nil;
    std::lock_guard<std::mutex> l(lean_mu_);
    return lean_.emplace(hash, fn).first->second;
  }

  id<MTLFunction> FoundLean(uint64_t hash) {
    std::lock_guard<std::mutex> l(lean_mu_);
    auto it = lean_.find(hash);
    return it == lean_.end() ? nil : it->second;
  }

  // ---- translated shaders' MSL from earlier runs, by Shader::hash (one
  // file of records: hash, length, text). The version changes with the
  // translation's settings (shader_compile.cpp).
  static constexpr uint32_t kMslCacheVersion = 1;

  void LoadMslCache() {
    NSString* dir = [NSString stringWithUTF8String:RecipePath().c_str()].stringByDeletingLastPathComponent;
    msl_path_ = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"metal-msl-v%u.bin",
                                                                                kMslCacheVersion]]
                    .fileSystemRepresentation;
    if (FILE* f = std::fopen(msl_path_.c_str(), "rb")) {
      uint64_t h;
      uint32_t n;
      while (std::fread(&h, 8, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1 && n < (16u << 20)) {
        std::string text(n, '\0');
        if (std::fread(text.data(), 1, n, f) != n) break;
        msl_cache_[h] = std::move(text);
      }
      std::fclose(f);
    }
    if (!msl_cache_.empty()) std::fprintf(stderr, "gfx: %zu translated shaders from earlier runs\n", msl_cache_.size());
    msl_file_ = std::fopen(msl_path_.c_str(), "ab");
  }

  std::string CachedMsl(uint64_t hash) {
    auto it = msl_cache_.find(hash);
    return it == msl_cache_.end() ? std::string() : it->second;
  }

  void CacheMsl(uint64_t hash, const std::string& msl) {
    msl_cache_[hash] = msl;
    if (!msl_file_) return;
    const uint32_t n = uint32_t(msl.size());
    std::fwrite(&hash, 8, 1, msl_file_);
    std::fwrite(&n, 4, 1, msl_file_);
    std::fwrite(msl.data(), 1, n, msl_file_);
    std::fflush(msl_file_);
  }

  void LoadRecipes() {
    recipe_path_ = RecipePath();
    if (FILE* f = std::fopen(recipe_path_.c_str(), "rb")) {
      Recipe r;
      while (std::fread(&r, sizeof r, 1, f) == 1) {
        if (!recipe_known_.insert(Fnv64(&r, sizeof r)).second) continue;
        recipes_.push_back(r);
        recipe_by_shader_.emplace(r.vs, recipes_.size() - 1);
        if (r.ps != r.vs) recipe_by_shader_.emplace(r.ps, recipes_.size() - 1);
      }
      std::fclose(f);
    }
    if (!recipes_.empty())
      std::fprintf(stderr, "gfx: %zu pipeline recipes from earlier runs\n", recipes_.size());
    recipe_file_ = std::fopen(recipe_path_.c_str(), "ab");
  }

  // On the encoding thread: a pipeline just compiled, kept for later runs.
  void RecordRecipe(const PipeKey& k) {
    auto vh = enc_shader_hash_.find(k.vs), ph = enc_shader_hash_.find(k.ps);
    if (vh == enc_shader_hash_.end() || ph == enc_shader_hash_.end() || !vh->second || !ph->second) return;
    Recipe r{vh->second, ph->second, k};
    r.key.vs = r.key.ps = 0;
    std::lock_guard<std::mutex> l(recipe_mu_);
    if (!recipe_file_ || !recipe_known_.insert(Fnv64(&r, sizeof r)).second) return;
    std::fwrite(&r, sizeof r, 1, recipe_file_);
    std::fflush(recipe_file_);
  }

  // A shader of `hash` was made: the recipes it completes, compiled in the
  // background.
  // Work done ahead (pipelines from earlier runs' recipes, lean shader
  // variants): one at a time, at background priority. On the global queues
  // GCD ran one per core: on an iPad the first run with a Mac's 1,700
  // recipes compiled on every core at once (and Metal's compiler service
  // at their priority) and the game crawled for minutes.
  static dispatch_queue_t AheadQueue() {
    static dispatch_queue_t q = dispatch_queue_create(
        "recomp.gfx.ahead",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_BACKGROUND, 0));
    return q;
  }

  void PrebuildFor(uint64_t hash) {
    auto [lo, hi] = recipe_by_shader_.equal_range(hash);
    for (auto it = lo; it != hi; ++it) {
      const Recipe& r = recipes_[it->second];
      auto vs = sid_by_hash_.find(r.vs), ps = sid_by_hash_.find(r.ps);
      if (vs == sid_by_hash_.end() || ps == sid_by_hash_.end()) continue;
      PipeKey k = r.key;
      k.vs = vs->second;
      k.ps = ps->second;
      NSString* src = nil;  // the pixel shader's MSL, for its lean variant
      if (k.lean & kLeanPs) {
        auto m = msl_cache_.find(r.ps);
        if (m == msl_cache_.end() || !shaders_.at(k.ps).has_lean) continue;
        src = @(m->second.c_str());
      }
      if (!prebuild_started_.insert(k).second) continue;
      MTLRenderPipelineDescriptor* pd = Describe(k, shaders_.at(k.vs).fn, shaders_.at(k.ps).fn);
      id<MTLDevice> dev = dev_;
      std::mutex* mu = &prebuilt_mu_;
      auto* done = &prebuilt_;
      const uint64_t ps_hash = r.ps;
      std::atomic<bool>* closing = &closing_;
      dispatch_async(AheadQueue(), ^{
        if (closing->load()) return;
        if (src) {
          id<MTLFunction> ps = Lean(ps_hash, src);
          if (!ps || closing->load()) return;
          pd.fragmentFunction = ps;
        }
        NSError* err = nil;
        id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p) return;
        std::lock_guard<std::mutex> l(*mu);
        (*done)[k] = p;
      });
    }
  }

  void BindPipeline(const PipeKey& k) {
    if (bound_.pipe_set && k == bound_.pipe) return;
    bound_.pipe = k;
    bound_.pipe_set = true;
    // Found (or compiled) on the encoding thread; consecutive draws mostly
    // share theirs, unhashed.
    Rec([this, k] {
      if (!last_pipe_ || !(k == last_pipe_key_)) {
        last_pipe_ = Pipeline(k);
        last_pipe_key_ = k;
      }
      [enc_ setRenderPipelineState:last_pipe_];
    });
  }

  void BindDepth(const DepthKey& k) {
    if (last_depth_ && k == last_depth_key_) {
      if ((__bridge void*)last_depth_ != bound_.ds) {
        __unsafe_unretained id<MTLDepthStencilState> ds = last_depth_;
        Rec([this, ds] { [enc_ setDepthStencilState:ds]; });
        bound_.ds = (__bridge void*)last_depth_;
      }
      return;
    }
    auto it = depth_states_.find(k);
    if (it == depth_states_.end()) {
      MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
      dd.depthCompareFunction = k.test ? MTLCompareFunction(k.func) : MTLCompareFunctionAlways;
      dd.depthWriteEnabled = k.write != 0;
      if (k.stencil) {
        MTLStencilDescriptor* f = [MTLStencilDescriptor new];
        f.stencilFailureOperation = MTLStencilOperation(k.fail);
        f.depthFailureOperation = MTLStencilOperation(k.zfail);
        f.depthStencilPassOperation = MTLStencilOperation(k.pass);
        f.stencilCompareFunction = MTLCompareFunction(k.sfunc);
        f.readMask = k.read;
        f.writeMask = k.wmask;
        MTLStencilDescriptor* b = [MTLStencilDescriptor new];
        b.stencilFailureOperation = MTLStencilOperation(k.bfail);
        b.depthFailureOperation = MTLStencilOperation(k.bzfail);
        b.depthStencilPassOperation = MTLStencilOperation(k.bpass);
        b.stencilCompareFunction = MTLCompareFunction(k.bfunc);
        b.readMask = k.read;
        b.writeMask = k.wmask;
        dd.frontFaceStencil = f;
        dd.backFaceStencil = b;
      }
      it = depth_states_.emplace(k, [dev_ newDepthStencilStateWithDescriptor:dd]).first;
    }
    last_depth_ = it->second;
    last_depth_key_ = k;
    if ((__bridge void*)it->second == bound_.ds) return;
    __unsafe_unretained id<MTLDepthStencilState> ds = it->second;
    Rec([this, ds] { [enc_ setDepthStencilState:ds]; });
    bound_.ds = (__bridge void*)it->second;
  }

  void Raster(MTLCullMode cull, MTLTriangleFillMode fill, float bias, float slope) {
    if (int(cull) != bound_.cull) {
      Rec([this, cull] { [enc_ setCullMode:cull]; });
      bound_.cull = int(cull);
    }
    if (int(fill) != bound_.fill) {
      Rec([this, fill] { [enc_ setTriangleFillMode:fill]; });
      bound_.fill = int(fill);
    }
    if (bias != bound_.bias || slope != bound_.slope) {
      Rec([this, bias, slope] { [enc_ setDepthBias:bias slopeScale:slope clamp:0]; });
      bound_.bias = bias;
      bound_.slope = slope;
    }
  }

  // Game coordinates; scaled to the target (pass_.sx, sy).
  void ViewportOf(MTLViewport v) {
    v.originX *= pass_.sx;
    v.width *= pass_.sx;
    v.originY *= pass_.sy;
    v.height *= pass_.sy;
    if (bound_.has_vp && std::memcmp(&v, &bound_.vp, sizeof v) == 0) return;
    Rec([this, v] { [enc_ setViewport:v]; });
    bound_.vp = v;
    bound_.has_vp = true;
  }

  // Game coordinates; scaled to the target, outwards.
  void ScissorOf(MTLScissorRect s) {
    if (pass_.sx != 1.0f || pass_.sy != 1.0f) {
      const NSUInteger w = pass_.color[0] ? std::max<NSUInteger>(1, pass_.color[0].width >> pass_.level[0]) : 1;
      const NSUInteger h = pass_.color[0] ? std::max<NSUInteger>(1, pass_.color[0].height >> pass_.level[0]) : 1;
      const NSUInteger x0 = std::min(w, NSUInteger(std::floor(s.x * pass_.sx)));
      const NSUInteger y0 = std::min(h, NSUInteger(std::floor(s.y * pass_.sy)));
      const NSUInteger x1 = std::min(w, NSUInteger(std::ceil((s.x + s.width) * pass_.sx)));
      const NSUInteger y1 = std::min(h, NSUInteger(std::ceil((s.y + s.height) * pass_.sy)));
      s = {x0, y0, std::max(x1, x0) - x0, std::max(y1, y0) - y0};
    }
    if (bound_.has_sc && std::memcmp(&s, &bound_.sc, sizeof s) == 0) return;
    Rec([this, s] { [enc_ setScissorRect:s]; });
    bound_.sc = s;
    bound_.has_sc = true;
  }

  void StencilRef(uint8_t r) {
    if (int(r) == bound_.stencil_ref) return;
    Rec([this, r] { [enc_ setStencilReferenceValue:r]; });
    bound_.stencil_ref = r;
  }

  void BlendColor(const float c[4]) {
    if (std::memcmp(c, bound_.blend, sizeof bound_.blend) == 0) return;
    const float r = c[0], g = c[1], b = c[2], a = c[3];
    Rec([this, r, g, b, a] { [enc_ setBlendColorRed:r green:g blue:b alpha:a]; });
    std::memcpy(bound_.blend, c, sizeof bound_.blend);
  }

  void VertexBuffer(int index, __unsafe_unretained id<MTLBuffer> b, size_t off) {
    void* p = (__bridge void*)b;
    const NSUInteger i = NSUInteger(index);
    if (bound_.vbuf[index] == p) {
      if (bound_.voff[index] != off) Rec([this, off, i] { [enc_ setVertexBufferOffset:off atIndex:i]; });
    } else {
      Rec([this, b, off, i] { [enc_ setVertexBuffer:b offset:off atIndex:i]; });
    }
    bound_.vbuf[index] = p;
    bound_.voff[index] = off;
  }

  void FragmentBuffer(int index, __unsafe_unretained id<MTLBuffer> b, size_t off) {
    void* p = (__bridge void*)b;
    const NSUInteger i = NSUInteger(index);
    if (bound_.fbuf[index] == p) {
      if (bound_.foff[index] != off) Rec([this, off, i] { [enc_ setFragmentBufferOffset:off atIndex:i]; });
    } else {
      Rec([this, b, off, i] { [enc_ setFragmentBuffer:b offset:off atIndex:i]; });
    }
    bound_.fbuf[index] = p;
    bound_.foff[index] = off;
  }

  // Uniform block `slot` of a shader draw (0-2 vertex, 3-5 pixel): the
  // previous draw's copy when equal (by `version`, the source's change
  // counter, when it has one; else compared with a copy in ordinary
  // memory, as the arena's write-combined memory is slow to read). At least
  // 16 bytes. The shadow copy is only of the buffer this cache holds, so an
  // upload that does not write it invalidates it.
  void Uniform(int slot, const void* data, uint32_t len, uint64_t version = 0) {
    static const uint8_t kZero[16] = {};
    if (!data || !len) {
      data = kZero;
      len = sizeof kZero;
      version = 0;
    }
    LastUniform& last = last_uniform_[slot];
    const bool shadowed = !version && len <= sizeof last.shadow;
    const bool same = last.len == len &&
                      (version    ? last.version == version && last.src == data
                       : shadowed ? last.shadow_valid && std::memcmp(last.shadow, data, len) == 0
                                  : std::memcmp(last.cpu, data, len) == 0);
    if (!same) {
      Slice s = Alloc(len, uniform_align_);
      std::memcpy(s.cpu, data, len);
      last.buf = s.buf;
      last.off = s.off;
      last.cpu = s.cpu;
      last.len = len;
      last.src = data;
      last.version = version;
      // The shadow copies the buffer, so only an upload that writes it keeps
      // it good: a versioned or larger one leaves a shadow of another buffer,
      // which an unversioned upload must not compare against.
      last.shadow_valid = shadowed;
      if (shadowed) std::memcpy(last.shadow, data, len);
    }
    if (slot < 3)
      VertexBuffer(slot, last.buf, last.off);
    else
      FragmentBuffer(slot - 3, last.buf, last.off);
  }

  void TextureAndSampler(bool vertex, uint32_t reg, __unsafe_unretained id<MTLTexture> t,
                         __unsafe_unretained id<MTLSamplerState> s) {
    void* tp = (__bridge void*)t;
    void* sp = (__bridge void*)s;
    void*& bt = vertex ? bound_.vtex[reg] : bound_.ftex[reg];
    void*& bs = vertex ? bound_.vsmp[reg] : bound_.fsmp[reg];
    if (bt != tp) {
      Rec([this, vertex, t, reg] {
        if (vertex) [enc_ setVertexTexture:t atIndex:reg];
        else [enc_ setFragmentTexture:t atIndex:reg];
      });
      bt = tp;
    }
    if (bs != sp) {
      Rec([this, vertex, s, reg] {
        if (vertex) [enc_ setVertexSamplerState:s atIndex:reg];
        else [enc_ setFragmentSamplerState:s atIndex:reg];
      });
      bs = sp;
    }
  }

  // D3DSAMPLERSTATETYPE values (index 1-13; null: D3D9's defaults).
  static SamplerKey D3D9Sampler(const uint32_t* ss) {
    static const uint32_t kDefaults[14] = {0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0};
    if (!ss) ss = kDefaults;
    SamplerKey k{};
    // D3DTEXF: NONE 0, POINT 1, LINEAR 2, ANISOTROPIC 3, ...
    k.mag = ss[5] <= 1 ? 0 : 1;
    k.min = ss[6] <= 1 ? 0 : 1;
    k.mip = ss[7] == 1 ? 0 : ss[7] >= 2 ? 1 : 0xFF;
    k.u = uint8_t(AddressMode(ss[1]));
    k.v = uint8_t(AddressMode(ss[2]));
    k.w = uint8_t(AddressMode(ss[3]));
    // BORDERCOLOR: Metal's nearest fixed one.
    const uint32_t b = ss[4];
    k.border = (b >> 24) < 0x80 ? 0 : ((b & 0xFFFFFF) == 0 || ((b >> 8) & 0xFF) < 0x80) ? 1 : 2;
    bool aniso = ss[5] == 3 || ss[6] == 3;
    k.aniso = uint8_t(aniso ? std::clamp<uint32_t>(ss[10], 1, 16) : 1);
    k.min_lod = float(ss[9]);  // MAXMIPLEVEL: the most detailed level used
    return k;
  }

  id<MTLSamplerState> Sampler(const SamplerKey& k) {
    auto it = samplers_.find(k);
    if (it != samplers_.end()) return it->second;
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = k.min ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.magFilter = k.mag ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    // MIPFILTER NONE samples only the top level (or MAXMIPLEVEL).
    sd.mipFilter = k.mip == 1 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    sd.lodMinClamp = k.min_lod;
    sd.lodMaxClamp = k.mip == 0xFF ? k.min_lod : FLT_MAX;
    sd.sAddressMode = MTLSamplerAddressMode(k.u);
    sd.tAddressMode = MTLSamplerAddressMode(k.v);
    sd.rAddressMode = MTLSamplerAddressMode(k.w);
    sd.borderColor = k.border == 0   ? MTLSamplerBorderColorTransparentBlack
                     : k.border == 1 ? MTLSamplerBorderColorOpaqueBlack
                                     : MTLSamplerBorderColorOpaqueWhite;
    int aniso = k.aniso;
    if (min_aniso_ > aniso && k.min && k.mip != 0xFF) aniso = min_aniso_;
    sd.maxAnisotropy = NSUInteger(std::max(aniso, 1));
    id<MTLSamplerState> s = [dev_ newSamplerStateWithDescriptor:sd];
    samplers_[k] = s;
    return s;
  }

  // ---- uploads

  void Update(TextureId tid, int slice, int level, const Rect& r, const void* src, uint32_t pitch) {
    auto it = textures_.find(tid);
    if (it == textures_.end() || level < 0 || level >= it->second.levels) return;
    Tex& t = it->second;
    Rect c = Clip(r, std::max(1, t.w >> level), std::max(1, t.h >> level));
    if (Empty(c)) return;
    Upload(t, slice, level, c, src, pitch, t.fmt);
  }

  // `c` (clipped) of `level` / `slice` from linear memory in `fmt`.
  void Upload(Tex& t, int slice, int level, const Rect& c, const void* src, uint32_t pitch,
              Format fmt) {
    @autoreleasepool {
      const int w = c.right - c.left, h = c.bottom - c.top;
      const auto* s = static_cast<const uint8_t*>(src);
      const bool in_order = t.used == serial_;
      if (perf_) (in_order ? pc_.uploads_in_order : pc_.uploads_ahead)++;
      // Data in shared memory, in the texture's own layout: copied by the
      // GPU from there (the front end keeps it until this frame is done).
      const bool decode = IsCompressed(fmt) && !bc_;
      const bool raw = (IsCompressed(fmt) && !decode) || IsFloat(fmt) || fmt == Format::A8R8G8B8 ||
                       (fmt == Format::X8R8G8B8 && t.opaque);
      const uint32_t unit = IsCompressed(fmt) ? RowPitch(fmt, 4) : BitsPerPixel(fmt) / 8;
      if (raw && shared_buf_ && s >= shared_lo_ && unit && size_t(s - shared_lo_) % unit == 0 &&
          pitch % 4 == 0) {
        const uint32_t n = IsCompressed(fmt) ? RowCount(fmt, uint32_t(h)) : uint32_t(h);
        if (s + size_t(pitch) * n <= shared_hi_) {
          frame_upload_bytes_ += size_t(pitch) * n;
          if (perf_ && in_order) pc_.in_order_bytes += size_t(pitch) * n;
          if (in_order) {
            if (enc_open_ || clear_flags_) Settle();
            if (!blit_open_) Rec([this] { blit_ = [cmd_ blitCommandEncoder]; });
            blit_open_ = true;
          }
          EnsureFrame();
          __unsafe_unretained id<MTLTexture> tex = t.tex;
          __unsafe_unretained id<MTLBuffer> buf = shared_buf_;
          const size_t off = size_t(s - shared_lo_);
          const MTLOrigin origin = MTLOriginMake(NSUInteger(c.left), NSUInteger(c.top), 0);
          Rec([=, this] {
            id<MTLBlitCommandEncoder> bl = blit_;
            if (!in_order) {
              if (!upload_blit_) upload_blit_ = [upload_cb_ blitCommandEncoder];
              bl = upload_blit_;
            }
            [bl copyFromBuffer:buf
                       sourceOffset:off
                  sourceBytesPerRow:pitch
                sourceBytesPerImage:size_t(pitch) * n
                         sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                          toTexture:tex
                   destinationSlice:NSUInteger(slice)
                   destinationLevel:NSUInteger(level)
                  destinationOrigin:origin];
          });
          return;
        }
      }
      uint32_t row, rows;
      if ((IsCompressed(fmt) && !decode) || IsFloat(fmt)) {  // as the GPU format stores it
        row = RowPitch(fmt, uint32_t(w));
        rows = RowCount(fmt, uint32_t(h));
      } else {
        row = uint32_t(w) * 4;
        rows = uint32_t(h);
      }
      Slice dst = Alloc(size_t(row) * rows, 16);
      frame_upload_bytes_ += size_t(row) * rows;
      if (perf_ && in_order) pc_.in_order_bytes += size_t(row) * rows;
      if (decode) {
        DecodeDxt(s, pitch, fmt, w, h, reinterpret_cast<uint32_t*>(dst.cpu));
      } else if (IsCompressed(fmt) || IsFloat(fmt)) {
        for (uint32_t y = 0; y < rows; y++)
          std::memcpy(dst.cpu + size_t(y) * row, s + size_t(y) * pitch, row);
      } else {
        ConvertToArgb(s, pitch, fmt, w, h, reinterpret_cast<uint32_t*>(dst.cpu));
      }
      // Draws already recorded with or into it must see the old contents:
      // then in order. Otherwise ahead of the frame.
      if (in_order) {
        if (enc_open_ || clear_flags_) Settle();
        if (!blit_open_) Rec([this] { blit_ = [cmd_ blitCommandEncoder]; });
        blit_open_ = true;
      }
      __unsafe_unretained id<MTLTexture> tex = t.tex;
      const MTLOrigin origin = MTLOriginMake(NSUInteger(c.left), NSUInteger(c.top), 0);
      Rec([=, this] {
        id<MTLBlitCommandEncoder> bl = blit_;
        if (!in_order) {
          if (!upload_blit_) upload_blit_ = [upload_cb_ blitCommandEncoder];
          bl = upload_blit_;
        }
        [bl copyFromBuffer:dst.buf
                   sourceOffset:dst.off
              sourceBytesPerRow:row
            sourceBytesPerImage:size_t(row) * rows
                     sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                      toTexture:tex
               destinationSlice:NSUInteger(slice)
               destinationLevel:NSUInteger(level)
              destinationOrigin:origin];
      });
    }
  }

  // ---- quads: StretchRect and the window

  // On the encoding thread: draws `sr` of level `level` of `src` (sw x
  // sh) over `dr` of a dw x dh target of format `pf`.
  void DrawQuad(id<MTLRenderCommandEncoder> e, MTLPixelFormat pf, id<MTLTexture> src, int level,
                float sw, float sh, const Rect& sr, float dw, float dh, const Rect& dr, bool linear) {
    PipeKey k;
    std::memset(&k, 0, sizeof k);
    k.vs = kQuadVs;
    k.ps = kQuadPs;
    k.color[0] = uint16_t(pf);
    k.src = k.src_a = uint8_t(MTLBlendFactorOne);
    k.dst = k.dst_a = uint8_t(MTLBlendFactorZero);
    k.mask[0] = MTLColorWriteMaskAll;
    [e setRenderPipelineState:Pipeline(k)];
    const float r[12] = {float(dr.left) / dw * 2 - 1,  1 - float(dr.top) / dh * 2,
                         float(dr.right) / dw * 2 - 1, 1 - float(dr.bottom) / dh * 2,
                         float(sr.left) / sw,          float(sr.top) / sh,
                         float(sr.right) / sw,         float(sr.bottom) / sh,
                         float(level),                 0,
                         0,                            0};
    [e setVertexBytes:r length:sizeof r atIndex:0];
    [e setFragmentBytes:r length:sizeof r atIndex:0];
    [e setFragmentTexture:src atIndex:0];
    [e setFragmentSamplerState:quad_sampler_[linear ? 1 : 0] atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  }

  // On the encoding thread: the front buffer (bw x bh) to the window,
  // scaled to fit with its aspect kept, black around it (FitRect). Not
  // while the window cannot be seen.
  // t_present: when the game called Present (CACurrentMediaTime), for the
  // latency from that call to the display (0: not measured).
  void Show(__unsafe_unretained id<MTLTexture> front, int bw, int bh, double t_present = 0) {
    if (SDL_GetWindowFlags(window_) &
        (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED))
      return;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    if (pw <= 0 || ph <= 0) return;
    if (layer_.drawableSize.width != pw || layer_.drawableSize.height != ph)
      layer_.drawableSize = CGSizeMake(pw, ph);
    id<CAMetalDrawable> drawable = nil;
    if (link_on_) {
      // The display's drawable for its next refresh: wait at most ~3
      // refreshes (none comes while the window cannot be seen).
      std::unique_lock<std::mutex> l(link_slot_.mu);
      link_slot_.cv.wait_for(l, std::chrono::milliseconds(50), [&] { return link_slot_.drawable != nil; });
      drawable = link_slot_.drawable;
      link_slot_.drawable = nil;
    } else {
      drawable = [layer_ nextDrawable];
    }
    if (!drawable) return;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
    const Rect r = FitRect(bw, bh, pw, ph);
    const bool same = r.right - r.left == bw && r.bottom - r.top == bh;
    DrawQuad(e, MTLPixelFormatBGRA8Unorm, front, 0, float(bw), float(bh), {0, 0, bw, bh},
             float(pw), float(ph), r, !same);
    [e endEncoding];
    // When the display really showed it (what Metal's HUD graphs).
    std::mutex* mu = &shown_mu_;
    std::vector<double>* shown = &shown_;
    std::vector<double>* latency = &latency_;
    dispatch_semaphore_t slot = pace_present_ ? frames_free_ : nullptr;
    frame_drawable_ = true;
    [drawable addPresentedHandler:^(id<MTLDrawable> shown_drawable) {
      if (slot) dispatch_semaphore_signal(slot);  // RECOMP_PACE_PRESENT
      const double t = shown_drawable.presentedTime;
      if (t <= 0) return;  // dropped
      std::lock_guard<std::mutex> l(*mu);
      shown->push_back(t);
      if (t_present > 0 && t > t_present) latency->push_back((t - t_present) * 1e3);
    }];
    // With vsync, shown no sooner than one refresh of the fastest rate
    // after the last: a variable-refresh display (ProMotion, Adaptive-Sync
    // set to Variable) then shows each frame when it is ready instead of
    // on a fixed 120 Hz grid (an 85 fps game shown 8.3 or 16.7 ms apart).
    if (vsync_ && !link_on_)
      [cmd_ presentDrawable:drawable afterMinimumDuration:MinFrameDuration()];
    else
      [cmd_ presentDrawable:drawable];
    NotePresentation({bw, bh, pw, ph, r});
  }

  // One refresh at the display's highest rate, in seconds.
  double MinFrameDuration() {
    NSInteger hz = 0;
#if TARGET_OS_OSX
    for (NSScreen* scr in NSScreen.screens)
      if (@available(macOS 12.0, *)) hz = std::max(hz, scr.maximumFramesPerSecond);
#else
    hz = UIScreen.mainScreen.maximumFramesPerSecond;
#endif
    return 1.0 / double(hz > 0 ? hz : 60);
  }

  // ---- readback

  // `r` of level `level` of `tex` (tw x th), as everything recorded so far
  // leaves it, into dst in `fmt`. Waits for the GPU.
  void ReadTexture(__unsafe_unretained id<MTLTexture> tex, int tw, int th, int level, const Rect& r, void* dst,
                   uint32_t pitch, Format fmt) {
    @autoreleasepool {
      if (!tex) return;
      Rect c = Clip(r, tw, th);
      const int w = c.right - c.left, h = c.bottom - c.top;
      if (w <= 0 || h <= 0) return;
      const size_t bytes = size_t(w) * h * 4;
      if (!readback_ || readback_.length < bytes) {
        Keep(readback_);
        readback_ = [dev_ newBufferWithLength:std::max<size_t>(bytes, 1u << 20)
                                      options:MTLResourceStorageModeShared];
      }
      Settle();
      EnsureFrame();
      __unsafe_unretained id<MTLTexture> src = tex;
      __unsafe_unretained id<MTLBuffer> out = readback_;
      Rec([=, this] {
        id<MTLBlitCommandEncoder> bl = [cmd_ blitCommandEncoder];
        [bl copyFromTexture:src
                         sourceSlice:0
                         sourceLevel:NSUInteger(level)
                        sourceOrigin:MTLOriginMake(NSUInteger(c.left), NSUInteger(c.top), 0)
                          sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                            toBuffer:out
                   destinationOffset:0
              destinationBytesPerRow:NSUInteger(w) * 4
            destinationBytesPerImage:bytes];
        [bl endEncoding];
      });
      EndFrame();
      WaitIdle();  // the encoding thread has committed it: last_cb_ is it
      [last_cb_ waitUntilCompleted];
      const auto* px = static_cast<const uint32_t*>(readback_.contents);
      const uint32_t bpp = BitsPerPixel(fmt) / 8;
      // Rows of the requested rectangle outside the target stay as they were.
      const int ox = c.left - r.left, oy = c.top - r.top;
      for (int y = 0; y < h; y++) {
        const uint32_t* s = px + size_t(y) * uint32_t(w);
        uint8_t* out = static_cast<uint8_t*>(dst) + size_t(y + oy) * pitch + size_t(ox) * bpp;
        if (fmt == Format::X8R8G8B8 || fmt == Format::A8R8G8B8) {
          auto* o = reinterpret_cast<uint32_t*>(out);
          for (int x = 0; x < w; x++) o[x] = s[x] | 0xFF000000u;
        } else {
          for (int x = 0; x < w; x++) EncodePixel(out + size_t(x) * bpp, fmt, s[x] | 0xFF000000u);
        }
      }
    }
  }

  // ---- rectangles

  Rect ViewportRect() const {
    Rect v{int32_t(viewport_.x), int32_t(viewport_.y), int32_t(viewport_.x + viewport_.width),
           int32_t(viewport_.y + viewport_.height)};
    return Clip(v, pass_.w, pass_.h);
  }

  // Scissor of a draw: the target and the scissor rect if enabled. False if
  // nothing is left.
  bool DrawScissor(MTLScissorRect* s) const {
    Rect r{0, 0, pass_.w, pass_.h};
    if (rs_[174]) r = Intersect(r, scissor_);
    if (Empty(r)) return false;
    *s = {NSUInteger(r.left), NSUInteger(r.top), NSUInteger(r.right - r.left),
          NSUInteger(r.bottom - r.top)};
    return true;
  }

  // ---- members

  mutable std::recursive_mutex mu_;
  SDL_Window* window_;
  SDL_MetalView view_ = nullptr;
  id<MTLDevice> dev_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  CAMetalLayer* layer_ = nil;
  size_t uniform_align_ = 256;
  bool back_buffer_alpha_ = false;  // SetBackBufferAlpha
  bool bc_ = true;  // the GPU samples BC1-3 (else DXT is decoded on upload)

  // The encoding thread, its queue of recorded batches and the batch being
  // recorded.
  std::thread encoder_;
  id<NSObject> activity_ = nil;  // beginActivityWithOptions:, held while alive
  std::mutex q_mu_;
  std::condition_variable q_cv_, done_cv_;
  std::deque<Batch> batches_;
  std::vector<Batch> free_batches_;  // run, their memory kept for reuse
  uint64_t submitted_ = 0, done_ = 0;
  bool stop_ = false;
  bool sync_ = false;  // RECOMP_METAL_SYNC: closures run as recorded
  bool reversed_z_ = true;  // RECOMP_REVERSED_Z: float depth stored as 1 - d
  bool depth_from_w_ = true;  // RECOMP_DEPTH_FROM_W
  int wvp_reg_ = -1;  // SetDepthFromW's; -1 also with depth from w off
  DepthPlanes depth_planes_ = nullptr;
  int w_logged_ = 0;
  std::unordered_set<uint64_t> w_noted_;  // NoteNotFromW's shaders
  // The draw being recorded: a reversed buffer's depth bias for
  // r_position (DepthBiasOf's clip), and whether it tests or writes depth.
  float clip_bias_ = 0;
  bool depth_used_ = false;
  // RECOMP_GPU_PASSES: a timestamp buffer per frame in flight (one more
  // than the slots), the passes it timed, and the totals by pass number.
  struct PassTimes {
    id<MTLCounterSampleBuffer> buf = nil;
    std::vector<std::string> names;  // by pass number in the frame
  };
  static constexpr size_t kTimedPasses = 256;
  bool gp_on_ = false;
  std::vector<PassTimes> gp_;
  size_t gp_slot_ = 0;  // encoding thread
  std::mutex gp_mu_;
  struct PassSum {
    double ms = 0;
    std::string name;
  };
  std::vector<PassSum> gp_sum_;
  double gp_frame_ms_ = 0;
  int gp_frames_ = 0;
  double gp_tick_ns_ = 1;  // GPU timestamp tick in ns

  void SetUpPassTimes() {
    if (![dev_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
      WarnF("gfx: RECOMP_GPU_PASSES: no stage-boundary counters on this GPU\n");
      return;
    }
    id<MTLCounterSet> ts = nil;
    for (id<MTLCounterSet> c in dev_.counterSets)
      if ([c.name isEqualToString:MTLCommonCounterSetTimestamp]) ts = c;
    if (!ts) return;
    gp_.resize(kSlots + 1);
    for (PassTimes& p : gp_) {
      MTLCounterSampleBufferDescriptor* d = [MTLCounterSampleBufferDescriptor new];
      d.counterSet = ts;
      d.storageMode = MTLStorageModeShared;
      d.sampleCount = kTimedPasses * 2;
      NSError* err = nil;
      p.buf = [dev_ newCounterSampleBufferWithDescriptor:d error:&err];
      if (!p.buf) {
        WarnF("gfx: RECOMP_GPU_PASSES: %s\n", err.localizedDescription.UTF8String);
        gp_.clear();
        return;
      }
    }
    // GPU ticks to ns: two CPU/GPU timestamp pairs 50 ms apart.
    MTLTimestamp c0, g0, c1, g1;
    [dev_ sampleTimestamps:&c0 gpuTimestamp:&g0];
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    [dev_ sampleTimestamps:&c1 gpuTimestamp:&g1];
    // The CPU side is in ns here (measured: with mach_timebase applied, the
    // passes of a 29 ms frame added up to 41.7 x that).
    if (g1 > g0) gp_tick_ns_ = double(c1 - c0) / double(g1 - g0);
    gp_on_ = true;
    WarnF("gfx: GPU pass times on (1 GPU tick = %.3f ns)\n", gp_tick_ns_);
  }

  // On the encoding thread: the pass about to be made samples the time its
  // vertex work starts and its fragment work ends.
  void TimePass(MTLRenderPassDescriptor* rp, const PassDesc& pd) {
    PassTimes& p = gp_[gp_slot_];
    const size_t n = p.names.size();
    if (n >= kTimedPasses) return;
    id<MTLTexture> t = pd.color[0];
    char name[96];
    std::snprintf(name, sizeof name, "%lux%lu x%d fmt %lu %c%c",
                  (unsigned long)(t.width >> pd.level[0]), (unsigned long)(t.height >> pd.level[0]),
                  pd.targets, (unsigned long)t.pixelFormat, (pd.clear & 1) ? 'C' : 'L',
                  pd.depth ? ((pd.clear & 2) ? 'C' : 'L') : '-');
    p.names.push_back(name);
    MTLRenderPassSampleBufferAttachmentDescriptor* a = rp.sampleBufferAttachments[0];
    a.sampleBuffer = p.buf;
    a.startOfVertexSampleIndex = n * 2;
    a.endOfVertexSampleIndex = MTLCounterDontSample;
    a.startOfFragmentSampleIndex = MTLCounterDontSample;
    a.endOfFragmentSampleIndex = n * 2 + 1;
  }

  // When the frame's GPU work is done: its passes' times added up; every
  // 120 frames the passes by GPU time, then the frame's total.
  void ReportPassTimes(PassTimes* p, double frame_s) {
    const size_t n = p->names.size();
    std::vector<double> ms(n, 0);
    if (n) {
      NSData* d = [p->buf resolveCounterRange:NSMakeRange(0, n * 2)];
      const auto* r = static_cast<const MTLCounterResultTimestamp*>(d.bytes);
      // A pass's cost: from the end of the one before (the first: its own
      // vertex start) to its fragment end. Starts alone mislead on a tiled
      // GPU, where the next pass's vertex work runs under this one's
      // fragments; ends are in order, so the costs add up to the frame.
      uint64_t prev = d && r[0].timestamp != MTLCounterErrorValue ? r[0].timestamp : 0;
      for (size_t i = 0; d && i < n; i++) {
        const uint64_t end = r[2 * i + 1].timestamp;
        if (end == MTLCounterErrorValue || !end || !prev) continue;
        if (end > prev) ms[i] = double(end - prev) * gp_tick_ns_ / 1e6;
        prev = std::max(prev, end);
      }
    }
    std::lock_guard<std::mutex> l(gp_mu_);
    if (gp_sum_.size() < n) gp_sum_.resize(n);
    for (size_t i = 0; i < n; i++) {
      gp_sum_[i].ms += ms[i];
      gp_sum_[i].name = p->names[i];
    }
    p->names.clear();
    gp_frame_ms_ += frame_s * 1e3;
    if (++gp_frames_ < 120) return;
    std::vector<size_t> order(gp_sum_.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return gp_sum_[a].ms > gp_sum_[b].ms; });
    double timed = 0;
    for (const PassSum& s : gp_sum_) timed += s.ms;
    std::string out;
    char line[160];
    std::snprintf(line, sizeof line, "gfx: GPU %.2f ms a frame, render passes %.2f (each from the previous pass's end to its own):\n",
                  gp_frame_ms_ / gp_frames_, timed / gp_frames_);
    out += line;
    for (size_t k = 0; k < order.size() && k < 16; k++) {
      const PassSum& s = gp_sum_[order[k]];
      std::snprintf(line, sizeof line, "  pass %2zu %6.2f ms  %s\n", order[k], s.ms / gp_frames_, s.name.c_str());
      out += line;
    }
    std::fputs(out.c_str(), stderr);
    gp_sum_.clear();
    gp_frame_ms_ = 0;
    gp_frames_ = 0;
  }

  bool labels_ = false;  // RECOMP_METAL_LABELS
  float clip_planes_[6][4] = {};  // SetClipPlane
  // The upscaler (RECOMP_UPSCALER): recording side, except as noted.
  Upscaler upscaler_ = Upscaler::Off;
  struct RtBoost {
    int w, h, k;
  };
  std::vector<RtBoost> rt_boost_;  // RECOMP_RT_BOOST
  float render_scale_ = 1.0f;
  id<MTLLibrary> internal_lib_ = nil;
  id<MTLDepthStencilState> depth_copy_ds_ = nil;
  std::unordered_map<NSUInteger, id<MTLRenderPipelineState>> depth_copy_pipes_;
  struct DepthCopy {
    id<MTLTexture> tex = nil;
    uint64_t frame = 0;  // serial_ of the last copy
  };
  std::unordered_map<void*, std::vector<DepthCopy>> depth_proxies_;  // by the scaled depth buffer
  id<MTLTexture> low_color_ = nil;  // the composite at the internal resolution
  bool frame_scene_ = false;        // a scene buffer drawn to this frame
  bool composited_ = false;         // and upscaled to the back buffer
  // What BindTarget chose by (EnsureTarget): serial_ (0: no frame open),
  // frame_scene_ and composited_ then.
  uint64_t bound_serial_ = 0;
  bool bound_scene_ = false, bound_composited_ = false;
  bool jitter_draw_ = false;        // the draw being recorded is jittered
  int jitter_phase_ = 0;
  float jitter_[2] = {};            // this frame's, in pixels of the internal resolution
  float jitter_sign_[2] = {1, 1}, mv_sign_ = 1;  // conventions (RECOMP_MFX_SIGNS)
  double vp_cur_[16] = {}, vp_prev_[16] = {};  // the camera, world to clip (row vectors)
  bool have_vp_ = false, have_prev_ = false;
  __unsafe_unretained id<MTLTexture> scene_depth_ = nil;  // the jittered scene's depth buffer
  id<MTLFXTemporalScaler> scaler_ = nil;
  id<MTLFXSpatialScaler> spatial_ = nil;                 // EnsureSpatial
  id<MTLTexture> spatial_in_ = nil, spatial_out_ = nil;  // its input (SMAA's output); its output if not the back buffer
  bool spatial_failed_ = false;
  bool scaler_failed_ = false, fx_reset_ = true;
  id<MTLTexture> motion_tex_ = nil, fx_depth_ = nil, fx_out_ = nil;
  id<MTLRenderPipelineState> motion_pipe_ = nil;
  id<MTLComputePipelineState> taa_pipe_ = nil, taa_sharpen_pipe_ = nil;
  float taa_sharpness_ = 0.5f;  // RECOMP_TAA_SHARPNESS
  // The scene's textures under TAA: a mip sharper, as FSR 2 and DLSS
  // advise for temporal antialiasing (log2(render / display) - 1; the
  // accumulated jittered samples keep it from aliasing).
  static constexpr float kTaaLodBias = 0.0f;  // -1 sharpened textures but cost ~2 ms a frame (bandwidth) on the M3
  id<MTLTexture> taa_history_[2] = {};  // read one, write the other
  id<MTLTexture> taa_src_ = nil;        // the composite, as TAA reads it
  int taa_cur_ = 0;
  id<MTLRenderPipelineState> smaa_pipe_[3] = {};  // SmaaPrepare
  id<MTLTexture> smaa_area_ = nil, smaa_search_ = nil, smaa_edges_ = nil, smaa_blend_ = nil;
  id<MTLTexture> smaa_stencil_ = nil;            // SmaaPrepare: the edge pass's pixels
  id<MTLDepthStencilState> smaa_ds_[2] = {};     // SmaaPrepare: edge, weight pass
  int pass_no_ = 0;      // labels_: render passes in this frame (encoding thread)
  Batch rec_;
  // Recording side: a frame is open (BeginFrame), a render or blit encoder
  // is (as the encoding thread will have it).
  bool frame_open_ = false, enc_open_ = false, blit_open_ = false;
  // Encoding thread: the shaders' functions (CreateShader hands them over)
  // and the device's own quad samplers.
  std::unordered_map<ShaderId, id<MTLFunction>> enc_shaders_;
  std::unordered_map<ShaderId, uint64_t> enc_shader_hash_;  // the encoding thread's Shader::hash
  // Pipelines built ahead (Recipe): the recording side's index, the
  // background compiles' results.
  std::unordered_map<uint64_t, ShaderId> sid_by_hash_;
  std::vector<Recipe> recipes_;
  std::unordered_multimap<uint64_t, size_t> recipe_by_shader_;
  std::unordered_set<PipeKey, BytesHash<PipeKey>> prebuild_started_;
  std::mutex prebuilt_mu_;
  std::unordered_map<PipeKey, id<MTLRenderPipelineState>, BytesHash<PipeKey>> prebuilt_;
  size_t prebuilt_used_ = 0;  // encoding thread
  std::mutex recipe_mu_;
  std::unordered_set<uint64_t> recipe_known_;
  std::mutex lean_mu_;
  std::unordered_map<uint64_t, id<MTLFunction>> lean_;  // by Shader::hash (Lean)
  std::string recipe_path_;
  FILE* recipe_file_ = nullptr;
  std::unordered_map<uint64_t, std::string> msl_cache_;  // recording side (CreateShader)
  std::string msl_path_;
  FILE* msl_file_ = nullptr;
  id<MTLSamplerState> quad_sampler_[2] = {};

  // Encoding thread: the frame's command buffers and encoders.
  id<MTLCommandBuffer> cmd_ = nil;
  id<MTLCommandBuffer> upload_cb_ = nil;  // queued before cmd_
  id<MTLCommandBuffer> last_cb_ = nil;    // the last one committed
  id<MTLRenderCommandEncoder> enc_ = nil;
  id<MTLBlitCommandEncoder> blit_ = nil;         // in cmd_, between render encoders
  id<MTLBlitCommandEncoder> upload_blit_ = nil;  // in upload_cb_
  uint64_t serial_ = 0;
  // FrameSerial: the frame the next draw goes to (serial_ while one is
  // open, else serial_ + 1); written under mu_.
  std::atomic<uint64_t> draw_serial_{1};
  std::atomic<uint64_t> completed_{0};  // the last frame the GPU finished
  // GPU time of finished command buffers and the presented frames among
  // them (TakeGpuFrameMs); whether the one being recorded is presented.
  std::atomic<uint64_t> gpu_ns_{0}, gpu_frames_{0};
  // Counters for tuning (EnablePerfStats): the recording side's under mu_,
  // the encoding thread's atomic.
  struct PerfCounts {
    uint64_t frames = 0, draws = 0, draw_ns = 0, ops = 0, passes = 0;
    uint64_t w_draws = 0;  // with depth from w
    uint64_t w_other = 0;  // that would be, but have another projection than the buffer content's
    uint64_t uploads_ahead = 0, uploads_in_order = 0, upload_bytes = 0, in_order_bytes = 0;
    // Arena buffers: made while drawing, and taken ready (made ahead).
    uint64_t textures = 0, depths = 0, arena_buffers = 0, arena_bytes = 0, arena_ahead = 0;
    uint64_t shaders = 0, shader_ns = 0, msl_compiles = 0;
  };
  std::atomic<bool> perf_{false};
  std::atomic<bool> closing_{false};  // the device is going: prebuild blocks do nothing
  PerfCounts pc_;
  uint64_t perf_t0_ = 0;
  std::atomic<uint64_t> enc_busy_ns_{0}, pipe_compiles_{0}, pipe_ns_{0}, pipe_max_ns_{0},
      pipe_prebuilt_{0};
  static uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
  }
  bool presented_ = false;
  bool vsync_ = false;  // ResetSwapChain's: frames paced by the display
  // RECOMP_DISPLAY_LINK: the link runs on a thread of its own with its own
  // run loop (the game's main thread pumps events only between frames).
  bool link_on_ = false;
  RecompLinkSlot link_slot_;
  std::thread link_thread_;
  CFRunLoopRef link_loop_ = nullptr;
  std::mutex link_mu_;
  std::condition_variable link_cv_;

  void StartDisplayLink() {
    if (@available(macOS 14.0, *)) {
      link_on_ = true;
      link_thread_ = std::thread([this] {
        @autoreleasepool {
          RecompDisplayLinkTarget* target = [RecompDisplayLinkTarget new];
          target.slot = &link_slot_;
          CAMetalDisplayLink* link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer_];
          link.delegate = target;
          // RECOMP_LINK_LATENCY: frames the display may hold (default 1).
          float latency = 1.0f;
          if (const char* e = std::getenv("RECOMP_LINK_LATENCY")) latency = std::clamp(float(std::atof(e)), 1.0f, 2.0f);
          link.preferredFrameLatency = latency;
          const float hz = float(1.0 / MinFrameDuration());
          link.preferredFrameRateRange = CAFrameRateRangeMake(30.0f, hz, hz);
          [link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSDefaultRunLoopMode];
          {
            std::lock_guard<std::mutex> l(link_mu_);
            link_loop_ = CFRunLoopGetCurrent();
          }
          link_cv_.notify_all();
          CFRunLoopRun();
          [link invalidate];
        }
      });
      std::unique_lock<std::mutex> l(link_mu_);
      link_cv_.wait(l, [&] { return link_loop_ != nullptr; });
      std::fprintf(stderr, "gfx: frames paced by CAMetalDisplayLink (up to %.0f Hz)\n", 1.0 / MinFrameDuration());
    } else {
      std::fprintf(stderr, "gfx: RECOMP_DISPLAY_LINK needs macOS 14\n");
    }
  }

  void StopDisplayLink() {
    if (!link_on_) return;
    {
      std::lock_guard<std::mutex> l(link_mu_);
      if (link_loop_) CFRunLoopStop(link_loop_);
    }
    if (link_thread_.joinable()) link_thread_.join();
    link_on_ = false;
  }
  bool pace_present_ = false;   // RECOMP_PACE_PRESENT=1
  bool frame_drawable_ = false; // encoding thread: this frame showed a drawable
  // Times the display showed frames (presented handlers), for
  // TakeShownIntervals.
  std::mutex shown_mu_;
  std::vector<double> shown_;
  std::vector<double> latency_;  // ms from the game's Present to the display
  size_t frame_upload_bytes_ = 0;  // texture data uploaded since the last Present
  dispatch_semaphore_t frames_free_ = nil;
  // Guest memory read in place (MapSharedMemory).
  id<MTLBuffer> shared_buf_ = nil;
  const uint8_t* shared_lo_ = nullptr;
  const uint8_t* shared_hi_ = nullptr;
  Arena arenas_[kSlots];
  int slot_ = 0;
  // Arena chunks made ahead (MakeSpareChunks), shared with the blocks
  // that make them.
  struct SparePool {
    std::mutex mu;
    std::vector<id<MTLBuffer>> ready;
    int pending = 0;
  };
  std::shared_ptr<SparePool> spares_ = std::make_shared<SparePool>();
  Bound bound_;
  struct LastUniform {
    __unsafe_unretained id<MTLBuffer> buf;  // owned by the arena
    size_t off;
    uint8_t* cpu;
    uint32_t len;
    const void* src;   // where it was copied from, and that source's
    uint64_t version;  // change counter (0: none)
    // Without a counter: the data, if it fits (VsState, PsState), and only
    // while it is `buf`'s (shadow_valid).
    alignas(16) uint8_t shadow[sizeof(VsState)];
    bool shadow_valid;
  };
  LastUniform last_uniform_[6] = {};
  // Per sampler register (pixel 0-15, vertex 0-3): the sampler last made
  // from its D3D9 states, the row and the row's change counter.
  struct SlotSampler {
    const uint32_t* row;
    uint64_t version;
    __unsafe_unretained id<MTLSamplerState> smp;  // samplers_ holds it
  };
  SlotSampler slot_sampler_[20] = {};
  // Int and bool register blocks of the last draws (vertex, pixel), the
  // front end's change counter and source they were built from, and a
  // serial of each rebuild (Uniform's version for them).
  std::array<int32_t, 80> ib_[2] = {};
  uint64_t ib_version_[2] = {}, ib_serial_[2] = {1, 1};
  const void* ib_src_[2] = {};

  // The pass and its pending whole-target clear (flags as Clear's).
  Pass pass_;
  uint32_t clear_flags_ = 0;
  float clear_rgba_[4] = {0, 0, 0, 1};
  MTLClearColor clear_color_ = {0, 0, 0, 1};
  double clear_depth_ = 1;
  uint8_t clear_stencil_ = 0;

  id<MTLTexture> backbuffer_ = nil;  // the game's back buffer
  id<MTLTexture> front_ = nil;       // the last presented frame
  int bb_w_ = 0, bb_h_ = 0;
  id<MTLTexture> dummy_2d_ = nil, dummy_cube_ = nil, dummy_3d_ = nil;
  id<MTLBuffer> readback_ = nil;
  std::unordered_map<uint32_t, Read> reads_;  // StartReadBack tickets
  uint32_t next_read_ = 1;
  int min_aniso_ = 0;
  uint32_t warned_ = 0;

  std::unordered_map<TextureId, Tex> textures_;
  // The same by id, for lookups on every draw: pointers into textures_,
  // whose elements do not move. Sized by the highest live id, and the ids of
  // destroyed textures are handed out again (NewTexId).
  std::vector<Tex*> tex_at_;
  std::vector<TextureId> free_tex_ids_;
  std::unordered_map<DepthId, Depth> depths_;
  std::unordered_map<ShaderId, Shader> shaders_;
  uint32_t next_id_ = 1;
  TextureId current_color_ = 0;
  int current_level_ = 0;
  std::array<ColorTarget, 3> current_extra_{};
  DepthId current_depth_ = 0;
  std::unordered_map<PipeKey, id<MTLRenderPipelineState>, BytesHash<PipeKey>> pipelines_;
  id<MTLRenderPipelineState> last_pipe_ = nil;  // Pipeline(last_pipe_key_)
  PipeKey last_pipe_key_{};
  id<MTLDepthStencilState> last_depth_ = nil;
  DepthKey last_depth_key_{};
  std::unordered_map<DepthKey, id<MTLDepthStencilState>, BytesHash<DepthKey>> depth_states_;
  std::unordered_map<SamplerKey, id<MTLSamplerState>, BytesHash<SamplerKey>> samplers_;

  // D3D state.
  std::array<uint32_t, 256> rs_{};
  Viewport viewport_{0, 0, 1, 1, 0, 1};
  Rect scissor_{};
};

}  // namespace

std::unique_ptr<Device> CreateMetalDevice(SDL_Window* window) {
  auto d = std::make_unique<MetalDevice>(window);
  if (!d->Init()) return nullptr;
  return d;
}

}  // namespace recomp::gfx
