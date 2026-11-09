// The EDRAM and resolves (edram.h). The layout, the resolve's rectangle,
// sample and format rules are Xenia's render target cache and resolve
// shaders' (BSD-3; docs/xenos.md, where the console checks are listed).

#include "xbox360/edram.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ppc/fatal.h"
#include "runtime/gfx/device.h"
#include "xbox360/display.h"
#include "xbox360/gpu.h"
#include "xbox360/hle.h"
#include "xbox360/memory.h"

namespace recomp::xbox360 {
namespace {

using ppc::Fatal;

// Registers the resolve reads.
constexpr uint32_t kRegRbSurfaceInfo = 0x2000, kRegRbColorInfo = 0x2001, kRegRbDepthInfo = 0x2002,
                   kRegRbColor1Info = 0x2003, kRegScreenScissorTl = 0x200E,
                   kRegScreenScissorBr = 0x200F, kRegWindowOffset = 0x2080,
                   kRegWindowScissorTl = 0x2081, kRegWindowScissorBr = 0x2082,
                   kRegPaSuScModeCntl = 0x2205, kRegPaSuVtxCntl = 0x2302,
                   kRegRbCopyControl = 0x2318, kRegRbCopyDestBase = 0x2319,
                   kRegRbCopyDestPitch = 0x231A, kRegRbCopyDestInfo = 0x231B,
                   kRegRbDepthClear = 0x231D, kRegRbColorClear = 0x231E,
                   kRegRbColorClearLo = 0x231F;

// Colour target formats (RB_COLOR_INFO) and the texture formats a resolve
// writes (RB_COPY_DEST_INFO), as far as resolves handle them.
enum : uint32_t {
  kRt8888 = 0, kRt8888Gamma = 1, kRt2101010 = 2, kRt2101010Float = 3, kRt16Fixed2 = 4, kRt16Fixed4 = 5,
  kRt16Float2 = 6, kRt16Float4 = 7,
  kRt2101010As10101010 = 10, kRt2101010FloatAs16 = 12, kRt32Float = 14, kRt32Float2 = 15,
};
enum : uint32_t {
  kTex8888 = 6, kTex2101010 = 7, kTex8888A = 14, kTex24_8 = 22, kTex16Unorm2 = 25, kTex16Unorm4 = 26,
  kTex16Float2 = 31,
  kTex16Float4 = 32, kTex32Float = 36, kTex32Float2 = 37, kTex8888As16 = 50,
  kTex2101010As16 = 54,
};

// The swaps of red and blue a resolve's copy_dest_swap makes, by layout.
enum : uint32_t { kSwapNone = 0, kSwap8888 = 1, kSwap2101010 = 2, kSwap16x4 = 3 };

const char kCommon[] = R"(
layout(local_size_x = 8, local_size_y = 8) in;
// The EDRAM word of sample s of pixel p of a target: base tile, pitch in
// 32bpp tiles, sample shifts (x, y), 64bpp, depth.
uint EdramWord(ivec2 p, uint s, uint base, uint pitch, uvec2 shift, bool wide, bool depth) {
  uint sx = (uint(p.x) << shift.x) + ((s >> 1u) & shift.x);
  uint sy = (uint(p.y) << shift.y) + (s & shift.y);
  uint tile_w = wide ? 40u : 80u;
  uint tile = (base + (sy / 16u) * (wide ? pitch * 2u : pitch) + sx / tile_w) & 2047u;
  uint ix = sx % tile_w;
  if (depth) ix = ix < 40u ? ix + 40u : ix - 40u;
  return tile * 1280u + ((sy % 16u) * tile_w + ix) * (wide ? 2u : 1u);
}
)";

// Clear: every sample of the rectangle's pixels.
const char kClear[] = R"(
layout(R_UNIFORMS) uniform U {
  ivec4 rect;   // x0, y0, w, h
  uvec4 src;    // base, pitch, samples, flags (1: 64bpp, 2: depth)
  uvec4 shift;  // sample shifts x, y
  uvec4 value;  // the words, low first
};
void main() {
  ivec2 i = ivec2(gl_GlobalInvocationID.xy);
  if (i.x >= rect.z || i.y >= rect.w) return;
  bool wide = (src.w & 1u) != 0u, depth = (src.w & 2u) != 0u;
  for (uint s = 0u; s < src.z; s++) {
    uint w = EdramWord(rect.xy + i, s, src.x, src.y, shift.xy, wide, depth);
    r_rw[r_rwbuf[0].x + w] = value.x;
    if (wide) r_rw[r_rwbuf[0].x + w + 1u] = value.y;
  }
}
)";

// Copy: one sample of each pixel to the destination's tiled texel at the
// same coordinates, as its words (raw) or converted from the target's
// format to the texture's (unpacked, scaled by 2^exp_bias, red and blue
// swapped, packed).
const char kCopy[] = R"(
layout(R_UNIFORMS) uniform U {
  ivec4 rect;   // x0, y0, w, h
  uvec4 src;    // base, pitch, sample (4-6: samples 0-1, 2-3, 0-3 averaged), flags
                // (1: 64bpp, 2: depth, 4: 64bpp texels, 8: convert)
  uvec4 shift;  // sample shifts x, y; red/blue swap kind (raw) or 1 (convert); endian
  ivec4 dst;    // pitch (aligned to 32), bpb log2, base - range start
  uvec4 conv;   // target format, texture format, scale (float bits)
};
uint Endian(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
  if (e == 2u || e == 3u) v = (v << 16) | (v >> 16);
  return v;
}
int Tiled2D(int x, int y, int pitch, int bpb) {
  int outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
  int inner = (((y >> 1) & 7) << 3) | (x & 7);
  int oi = (outer | inner) << bpb;
  int bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
  return (oi & 15) | ((y & 1) << 4) | (((oi >> 4) & 1) << 5) | (pipe << 6) |
         (((oi >> 5) & 7) << 8) | (bank << 11) | ((oi >> 8) << 12);
}
// The Xenos's 7e3 float (bits 0-9): 3 exponent bits biased by 3, no
// infinity or NaN, [0, 32).
float F7e3(uint f) {
  f &= 0x3FFu;
  if (f == 0u) return 0.0;
  uint m = f & 0x7Fu;
  int e = int(f >> 7);
  if (e == 0) {
    int lz = 7 - findMSB(m);
    e = 1 - lz;
    m = (m << uint(lz)) & 0x7Fu;
  }
  // The 7 mantissa bits are the float's top ones (as Xenia's SPIR-V
  // translator places them; its C++ Float7e3To32 shifts them by 3).
  return uintBitsToFloat(uint(e + 124) << 23 | (m << 16));
}
float Un(uint v, uint shift, uint bits) {
  return float((v >> shift) & ((1u << bits) - 1u)) / float((1u << bits) - 1u);
}
// 16_16(_16_16) targets: signed 16-bit fixed point over [-32, 32].
vec2 Fixed16(uint v) { return max(vec2(-32.0), vec2(ivec2(int(v << 16), int(v)) >> 16) * (32.0 / 32767.0)); }
uint Pk(float f, uint bits) { return uint(clamp(f, 0.0, 1.0) * float((1u << bits) - 1u) + 0.5); }
vec4 Unpack(uint fmt, uint v0, uint v1) {
  switch (fmt) {
    case 0u: return vec4(Un(v0, 0u, 8u), Un(v0, 8u, 8u), Un(v0, 16u, 8u), Un(v0, 24u, 8u));
    case 2u: case 10u: return vec4(Un(v0, 0u, 10u), Un(v0, 10u, 10u), Un(v0, 20u, 10u), Un(v0, 30u, 2u));
    case 3u: case 12u: return vec4(F7e3(v0), F7e3(v0 >> 10), F7e3(v0 >> 20), Un(v0, 30u, 2u));
    case 4u: return vec4(Fixed16(v0), 0.0, 0.0);
    case 5u: return vec4(Fixed16(v0), Fixed16(v1));
    case 6u: return vec4(unpackHalf2x16(v0), 0.0, 0.0);
    case 7u: return vec4(unpackHalf2x16(v0), unpackHalf2x16(v1));
    case 14u: return vec4(uintBitsToFloat(v0), 0.0, 0.0, 0.0);
    default: return vec4(uintBitsToFloat(v0), uintBitsToFloat(v1), 0.0, 0.0);  // 15
  }
}
uvec2 Pack(uint fmt, vec4 c) {
  switch (fmt) {
    case 6u: case 14u: case 50u:
      return uvec2(Pk(c.r, 8u) | Pk(c.g, 8u) << 8 | Pk(c.b, 8u) << 16 | Pk(c.a, 8u) << 24, 0u);
    case 7u: case 54u:
      return uvec2(Pk(c.r, 10u) | Pk(c.g, 10u) << 10 | Pk(c.b, 10u) << 20 | Pk(c.a, 2u) << 30, 0u);
    case 25u: return uvec2(Pk(c.r, 16u) | Pk(c.g, 16u) << 16, 0u);
    case 26u: return uvec2(Pk(c.r, 16u) | Pk(c.g, 16u) << 16, Pk(c.b, 16u) | Pk(c.a, 16u) << 16);
    case 31u: return uvec2(packHalf2x16(c.rg), 0u);
    case 32u: return uvec2(packHalf2x16(c.rg), packHalf2x16(c.ba));
    case 36u: return uvec2(floatBitsToUint(c.r), 0u);
    default: return uvec2(floatBitsToUint(c.r), floatBitsToUint(c.g));  // 37
  }
}
void main() {
  ivec2 i = ivec2(gl_GlobalInvocationID.xy);
  if (i.x >= rect.z || i.y >= rect.w) return;
  ivec2 p = rect.xy + i;
  bool wide = (src.w & 1u) != 0u, depth = (src.w & 2u) != 0u, wide_out = (src.w & 4u) != 0u;
  uint first = src.z < 4u ? src.z : src.z == 5u ? 2u : 0u, n = src.z < 4u ? 1u : src.z == 6u ? 4u : 2u;
  uint w = EdramWord(p, first, src.x, src.y, shift.xy, wide, depth);
  uint v0 = r_ld(0u, w), v1 = wide ? r_ld(0u, w + 1u) : 0u;
  if ((src.w & 8u) != 0u) {
    vec4 c = Unpack(conv.x, v0, v1);
    for (uint k = 1u; k < n; k++) {
      uint wk = EdramWord(p, first + k, src.x, src.y, shift.xy, wide, depth);
      c += Unpack(conv.x, r_ld(0u, wk), wide ? r_ld(0u, wk + 1u) : 0u);
    }
    c *= uintBitsToFloat(conv.z) / float(n);
    if (shift.z != 0u) c = c.bgra;
    uvec2 o = Pack(conv.y, c);
    v0 = o.x;
    v1 = o.y;
  } else if (shift.z == 1u) {
    v0 = (v0 & 0xFF00FF00u) | ((v0 & 0xFFu) << 16) | ((v0 >> 16) & 0xFFu);
  } else if (shift.z == 2u) {
    v0 = (v0 & 0xC00FFC00u) | ((v0 & 0x3FFu) << 20) | ((v0 >> 20) & 0x3FFu);
  } else if (shift.z == 3u) {
    uint t = v0 & 0xFFFFu;
    v0 = (v0 & 0xFFFF0000u) | (v1 & 0xFFFFu);
    v1 = (v1 & 0xFFFF0000u) | t;
  }
  uint e = shift.w;
  if (e == 4u) {  // 8 in 64: the words trade places, then 8 in 32
    uint t = v0;
    v0 = v1;
    v1 = t;
    e = 2u;
  }
  uint at = uint(dst.z + Tiled2D(p.x, p.y, dst.x, dst.y)) >> 2;
  r_rw[r_rwbuf[0].x + at] = Endian(v0, e);
  if (wide_out) r_rw[r_rwbuf[0].x + at + 1u] = Endian(v1, e);
}
)";

// The EDRAM and the resolves' programs on the display's device
// (HostDevice(), its lock held).
struct State {
  gfx::BufferId edram = 0;
  gfx::ProgramId clear = 0, copy = 0;
};
State* g_state;

State& Device() {
  if (g_state) return *g_state;
  auto s = std::make_unique<State>();
  gfx::Device& dev = HostDevice();
  s->edram = dev.CreateBuffer(kEdramBytes);
  std::vector<uint8_t> zero(kEdramBytes);
  dev.UpdateBuffer(s->edram, 0, zero.data(), kEdramBytes);
  gfx::ProgramDesc d;
  d.cs.glsl = std::string(kCommon) + kClear;
  d.cs.rw_buffers = 1;
  d.cs.uniform_bytes = 64;
  d.local_size[0] = d.local_size[1] = 8;
  s->clear = dev.CreateProgram(d);
  d.cs.glsl = std::string(kCommon) + kCopy;
  d.cs.buffers = 1;
  d.cs.uniform_bytes = 80;
  s->copy = dev.CreateProgram(d);
  g_state = s.release();
  return *g_state;
}

int32_t Signed(uint32_t v, uint32_t bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }

// The formats a converting copy reads and writes (the copy shader's
// Unpack and Pack), and whether a texture format's texels are 64-bit.
bool Unpacks(uint32_t rt) {
  return rt == kRt8888 || rt == kRt2101010 || rt == kRt2101010Float || rt == kRt16Fixed2 ||
         rt == kRt16Fixed4 || rt == kRt16Float2 ||
         rt == kRt16Float4 || rt == kRt2101010As10101010 || rt == kRt2101010FloatAs16 || rt == kRt32Float ||
         rt == kRt32Float2;
}
bool Packs(uint32_t tex) {
  return tex == kTex8888 || tex == kTex8888A || tex == kTex8888As16 || tex == kTex2101010 ||
         tex == kTex2101010As16 || tex == kTex16Unorm2 || tex == kTex16Unorm4 || tex == kTex16Float2 || tex == kTex16Float4 || tex == kTex32Float ||
         tex == kTex32Float2;
}
bool WideTexels(uint32_t tex) { return tex == kTex16Unorm4 || tex == kTex16Float4 || tex == kTex32Float2; }

// The colour target formats whose EDRAM words a destination format holds
// as they are, and the swap of red and blue in that layout.
bool Bitwise(uint32_t rt, uint32_t tex, uint32_t* swap) {
  switch (rt) {
    case kRt8888:
    case kRt8888Gamma:
      *swap = kSwap8888;
      return tex == kTex8888 || tex == kTex8888A || tex == kTex8888As16;
    case kRt2101010:
    case kRt2101010As10101010:
      *swap = kSwap2101010;
      return tex == kTex2101010 || tex == kTex2101010As16;
    case kRt16Float2: *swap = kSwapNone; return tex == kTex16Float2;
    case kRt16Float4: *swap = kSwap16x4; return tex == kTex16Float4;
    case kRt32Float: *swap = kSwapNone; return tex == kTex32Float;
    case kRt32Float2: *swap = kSwapNone; return tex == kTex32Float2;
    default: return false;
  }
}

struct Rect {
  int32_t x0, y0, x1, y1;
};

// The rectangle a resolve covers, in pixels of the target: the three
// vertices of vertex fetch 0, as the scissor and the alignment leave it.
Rect ResolveRect(uint32_t surface_pitch) {
  uint32_t f0 = GpuRegister(kRegShaderConstantFetch), f1 = GpuRegister(kRegShaderConstantFetch + 1);
  if ((f0 & 3) != 3 || (f1 >> 2 & 0xFFFFFF) != 6)
    Fatal(nullptr, "GPU: a resolve whose vertex fetch 0 is not three points (%08x %08x)", f0, f1);
  uint32_t pa = f0 & 0x1FFFFFFC;
  // D3D's pixel centres are at integers: half a pixel moves them to the
  // centres the rasterizer uses.
  float half = GpuRegister(kRegPaSuVtxCntl) & 1 ? 0.0f : 0.5f;
  int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
  for (uint32_t k = 0; k < 6; k++) {
    uint32_t bits = GpuSwap(__builtin_bswap32(Rd32(PhysicalToVirtual(pa + 4 * k))), f1 & 3);
    float v;
    std::memcpy(&v, &bits, 4);
    // 16.8 fixed point, rounded to nearest (D3D11's conversion, Xenia's).
    float f = std::clamp((v + half) * 256.0f, -8388608.0f, 8388607.0f);
    int32_t x = int32_t(std::nearbyint(f));
    lo[k & 1] = std::min(lo[k & 1], x);
    hi[k & 1] = std::max(hi[k & 1], x);
  }
  Rect r{(lo[0] + 127) >> 8, (lo[1] + 127) >> 8, (hi[0] + 127) >> 8, (hi[1] + 127) >> 8};
  uint32_t off = GpuRegister(kRegWindowOffset);
  int32_t ox = Signed(off, 15), oy = Signed(off >> 16, 15);
  if (GpuRegister(kRegPaSuScModeCntl) >> 16 & 1) {
    r.x0 += ox;
    r.x1 += ox;
    r.y0 += oy;
    r.y1 += oy;
  }
  // The window scissor (offset unless it says not to), within the screen
  // scissor.
  uint32_t tl = GpuRegister(kRegWindowScissorTl), br = GpuRegister(kRegWindowScissorBr);
  int32_t sx0 = tl & 0x3FFF, sy0 = tl >> 16 & 0x3FFF, sx1 = br & 0x3FFF, sy1 = br >> 16 & 0x3FFF;
  if (!(tl >> 31)) {
    sx0 += ox;
    sx1 += ox;
    sy0 += oy;
    sy1 += oy;
  }
  uint32_t stl = GpuRegister(kRegScreenScissorTl), sbr = GpuRegister(kRegScreenScissorBr);
  sx0 = std::max({sx0, Signed(stl, 15), 0});
  sy0 = std::max({sy0, Signed(stl >> 16, 15), 0});
  sx1 = std::max(std::min(sx1, Signed(sbr, 15)), sx0);
  sy1 = std::max(std::min(sy1, Signed(sbr >> 16, 15)), sy0);
  r.x0 = std::clamp(r.x0, sx0, sx1);
  r.x1 = std::clamp(r.x1, sx0, sx1);
  r.y0 = std::clamp(r.y0, sy0, sy1);
  r.y1 = std::clamp(r.y1, sy0, sy1);
  // Whole blocks of 8 pixels, within the surface's pitch.
  int32_t pitch = int32_t(surface_pitch & ~7u);
  r.x0 = std::min(r.x0 & ~7, pitch);
  r.y0 &= ~7;
  r.x1 = std::min((r.x1 + 7) & ~7, pitch);
  r.y1 = std::min((r.y1 + 7) & ~7, r.y0 + 16384 - 8);
  return r;
}

void Dispatch(State& s, gfx::ProgramId prog, const Rect& r, const void* uniforms,
              const gfx::BufferData* in, gfx::RwBufferData* out) {
  gfx::ProgramDispatch d;
  d.program = prog;
  d.groups[0] = uint32_t(r.x1 - r.x0 + 7) / 8;
  d.groups[1] = uint32_t(r.y1 - r.y0 + 7) / 8;
  d.cs.uniforms = uniforms;
  d.cs.buffers = in;
  d.rw_buffers = out;
  HostDevice().DispatchProgram(d);
}

}  // namespace

void StartEdram() {
  std::lock_guard<std::mutex> l(HostDeviceLock());
  Device();
}

int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpb_log2) {
  int32_t outer = ((y >> 5) * int32_t(pitch >> 5) + (x >> 5)) << 6;
  int32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
  int32_t oi = (outer | inner) << bpb_log2;
  int32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
  return (oi & 15) | ((y & 1) << 4) | (((oi >> 4) & 1) << 5) | (pipe << 6) | (((oi >> 5) & 7) << 8) |
         (bank << 11) | ((oi >> 8) * (1 << 12));
}

void Resolve() {
  uint32_t control = GpuRegister(kRegRbCopyControl), surface = GpuRegister(kRegRbSurfaceInfo);
  uint32_t src_select = control & 7, sample = control >> 4 & 7, command = control >> 20 & 3;
  bool clear_color = control >> 8 & 1, clear_depth = control >> 9 & 1, depth = src_select >= 4;
  uint32_t msaa = surface >> 16 & 3;
  if (msaa > 2) Fatal(nullptr, "GPU: a resolve of a surface with MSAA mode 3");
  if (command > 1) Fatal(nullptr, "GPU: resolve copy command %u", command);
  Rect r = ResolveRect(surface & 0x3FFF);
  if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
  std::lock_guard<std::mutex> l(HostDeviceLock());
  State& s = Device();
  uint32_t shift_x = msaa == 2, shift_y = msaa >= 1, samples = 1u << msaa;
  // The pitch in 32bpp tiles: 80 samples each.
  uint32_t pitch_tiles = (((surface & 0x3FFF) << shift_x) + 79) / 80;
  static const uint32_t kColorInfo[4] = {kRegRbColorInfo, kRegRbColor1Info, kRegRbColor1Info + 1,
                                         kRegRbColor1Info + 2};
  uint32_t color_info = GpuRegister(kColorInfo[src_select & 3]), depth_info = GpuRegister(kRegRbDepthInfo);
  uint32_t color_format = color_info >> 16 & 0xF;
  bool wide = !depth && (color_format == 5 || color_format == 7 || color_format == 15);
  gfx::BufferData edram_in{nullptr, kEdramBytes, s.edram, 0};
  gfx::RwBufferData edram_out{nullptr, kEdramBytes, s.edram, false};

  // The copy: one sample into the tiled destination, as its words where
  // the formats agree (the target's swap of red and blue), else converted.
  uint32_t dest_info = GpuRegister(kRegRbCopyDestInfo), dest_pitch_reg = GpuRegister(kRegRbCopyDestPitch);
  uint32_t endian = dest_info & 7, format = dest_info >> 7 & 0x3F, swap = kSwapNone;
  int32_t exp_bias = Signed(dest_info >> 16, 6);
  if (dest_info >> 3 & 1) Fatal(nullptr, "GPU: a resolve to a 3D texture (not implemented yet)");
  if (depth) {
    // Depth goes as its words (D3D names a colour format for it).
    format = kTex24_8;
    exp_bias = 0;
  }
  // The sample copied: one the surface has, or an average (of a colour
  // target's), as Xenia narrows the choice.
  if (msaa == 0) sample = 0;
  else if (msaa == 1) sample = sample == 2 ? 0 : sample == 3 ? 1 : std::min(sample, 4u);
  else sample = std::min(sample, 6u);
  if (depth) sample = sample == 4 || sample == 6 ? 0 : sample == 5 ? 2 : sample;
  bool convert = !depth && (exp_bias || sample > 3 || !Bitwise(color_format, format, &swap));
  if (convert && (!Unpacks(color_format) || !Packs(format)))
    Fatal(nullptr, "GPU: a resolve of colour format %u to texture format %u (not implemented yet)", color_format,
          format);
  if (convert) swap = 1;
  if (!(dest_info >> 24 & 1)) swap = kSwapNone;
  bool wide_out = depth ? false : convert ? WideTexels(format) : wide;
  if (endian > (wide_out ? 4u : 3u)) Fatal(nullptr, "GPU: a resolve with endian swap %u", endian);
  uint32_t bpb_log2 = wide_out ? 3 : 2, dest_base = GpuRegister(kRegRbCopyDestBase);
  uint32_t dest_pitch = ((dest_pitch_reg & 0x3FFF) + 31) & ~31u;
  // The bytes the copy touches: from the first texel's 32x32 block to the
  // end of the last's.
  uint32_t lo = dest_base + uint32_t(Tiled2D(r.x0 & ~31, r.y0 & ~31, dest_pitch, bpb_log2));
  uint32_t hi = dest_base + uint32_t(Tiled2D((r.x1 - 1) & ~31, (r.y1 - 1) & ~31, dest_pitch, bpb_log2)) +
                (0x400u << bpb_log2);
  if (lo >= kPhysicalSize || hi > kPhysicalSize || hi <= lo)
    Fatal(nullptr, "GPU: a resolve to %x-%x, outside physical memory", lo, hi);
  uint32_t info = depth ? depth_info : color_info,
           flags = (wide ? 1 : 0) | (depth ? 2 : 0) | (wide_out ? 4 : 0) | (convert ? 8 : 0);
  float scale = std::ldexp(1.0f, exp_bias);
  uint32_t scale_bits;
  std::memcpy(&scale_bits, &scale, 4);
  struct {
    int32_t rect[4];
    uint32_t src[4], shift[4];
    int32_t dst[4];
    uint32_t conv[4];
  } cu = {{r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0},
          {info & 0xFFF, pitch_tiles, sample, flags},
          {shift_x, shift_y, swap, endian},
          {int32_t(dest_pitch), int32_t(bpb_log2), int32_t(dest_base - lo), 0},
          {color_format, format, scale_bits, 0}};
  gfx::RwBufferData dest{Mem() + PhysicalToVirtual(lo), hi - lo, 0, false};
  Dispatch(s, s.copy, r, &cu, &edram_in, &dest);

  // Then the clears, of every sample in the rectangle.
  struct {
    int32_t rect[4];
    uint32_t src[4], shift[4], value[4];
  } clear = {{r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0}, {}, {shift_x, shift_y, 0, 0}, {}};
  if (clear_depth) {
    clear.src[0] = depth_info & 0xFFF;
    clear.src[1] = pitch_tiles;
    clear.src[2] = samples;
    clear.src[3] = 2;
    clear.value[0] = GpuRegister(kRegRbDepthClear);
    Dispatch(s, s.clear, r, &clear, nullptr, &edram_out);
  }
  if (clear_color && !depth) {
    // A 64bpp target takes RB_COLOR_CLEAR as its low word (Xenia's
    // order; console check).
    clear.src[0] = color_info & 0xFFF;
    clear.src[1] = pitch_tiles;
    clear.src[2] = samples;
    clear.src[3] = wide ? 1 : 0;
    clear.value[0] = GpuRegister(kRegRbColorClear);
    clear.value[1] = GpuRegister(kRegRbColorClearLo);
    Dispatch(s, s.clear, r, &clear, nullptr, &edram_out);
  }
}

}  // namespace recomp::xbox360
