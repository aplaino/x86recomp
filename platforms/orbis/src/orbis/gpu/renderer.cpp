// Register layouts (context, SH and uconfig offsets) as AMD documents them
// for Sea Islands; formats and descriptors as in shader.cpp.

#include "orbis/gpu/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "orbis/gpu/shader.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::orbis::gpu {
namespace {

using gfx::Format;

// Context registers.
constexpr uint32_t kDbRenderControl = 0x000, kDbZInfo = 0x010, kDbStencilInfo = 0x011,
                   kDbZReadBase = 0x012, kDbDepthSize = 0x016, kDbDepthClear = 0x00B,
                   kDbStencilClear = 0x00A, kScreenScissorTl = 0x00C, kWindowScissorTl = 0x081,
                   kGenericScissorTl = 0x090, kVportScissorTl = 0x094, kCbTargetMask = 0x08E,
                   kCbBlendRed = 0x105, kDbStencilControl = 0x10B, kDbStencilRefMask = 0x10C,
                   kVportXScale = 0x10F, kPsInputCntl = 0x191,
                   kPsInputEna = 0x1B3, kPsInputAddr = 0x1B4, kZFormat = 0x1C4, kColFormat = 0x1C5,
                   kBlendControl = 0x1E0, kDbDepthControl = 0x200, kPaClClipCntl = 0x204,
                   kPaSuScModeCntl = 0x205, kPolyOffsetScale = 0x20B, kStepRate0 = 0x2A8,
                   kVgtShaderStagesEn = 0x2D5, kCbColor0Base = 0x318;
// SH registers.
constexpr uint32_t kPgmLoPs = 0x008, kPgmLoVs = 0x048, kComputeThreads = 0x207,
                   kComputePgmLo = 0x20C, kComputeRsrc1 = 0x212, kComputeUser = 0x240;
// Uconfig.
constexpr uint32_t kPrimitiveType = 0x242;

float AsFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

std::once_flag g_warn_flags[32];
// One warning per kind for what is not drawn yet.
void WarnOnce(int kind, const char* what) {
  std::call_once(g_warn_flags[kind], [&] { WarnF("GPU: %s\n", what); });
}

// ---- formats

// A colour target's format from CB_COLORn_INFO (FORMAT, NUMBER_TYPE,
// COMP_SWAP); false if not handled yet.
bool ColorFormat(uint32_t info, Format* out) {
  const uint32_t fmt = (info >> 2) & 0x1F, num = (info >> 8) & 7, swap = (info >> 11) & 3;
  const bool fl = num == 7, srgb = num == 6, un = num == 0, ui = num == 4;
  switch (fmt) {
    case 1: if (un) return *out = Format::R8, true; if (ui) return *out = Format::R8Uint, true; break;
    case 2: if (fl) return *out = Format::R16F, true; if (un) return *out = Format::R16, true; break;
    case 3: if (un) return *out = Format::R8G8, true; break;
    case 4: if (fl) return *out = Format::R32F, true; if (ui) return *out = Format::R32Uint, true; break;
    case 5: if (fl) return *out = Format::G16R16F, true; if (un) return *out = Format::R16G16, true; break;
    case 7: if (fl) return *out = Format::R11G11B10F, true; break;
    case 9: if (un) return *out = Format::R10G10B10A2, true; break;
    case 10:
      if (un) return *out = swap == 1 ? Format::A8R8G8B8 : Format::R8G8B8A8, true;
      if (srgb) return *out = swap == 1 ? Format::B8G8R8A8Srgb : Format::R8G8B8A8Srgb, true;
      break;
    case 11: if (fl) return *out = Format::G32R32F, true; break;
    case 12: if (fl) return *out = Format::A16B16G16R16F, true; if (un) return *out = Format::R16G16B16A16, true; break;
    case 14: if (fl) return *out = Format::A32B32G32R32F, true; if (ui) return *out = Format::R32G32B32A32Uint, true; break;
  }
  return false;
}

// A texture's format from a T# (IMG_DATA_FORMAT, IMG_NUM_FORMAT); BGRA
// (destination selects Z, Y, X, W) as the B8G8R8A8 formats.
bool TextureFormat(const uint32_t t[8], Format* out) {
  const uint32_t dfmt = (t[1] >> 20) & 0x3F, nfmt = (t[1] >> 26) & 0xF;
  const bool bgra = (t[3] & 0xFFF) == (6 | 5 << 3 | 4 << 6 | 7 << 9);
  const bool un = nfmt == 0, fl = nfmt == 7, srgb = nfmt == 9, ui = nfmt == 4, si = nfmt == 5;
  switch (dfmt) {
    case 1: if (un) return *out = Format::R8, true; if (ui) return *out = Format::R8Uint, true; break;
    case 2: if (fl) return *out = Format::R16F, true; if (un) return *out = Format::R16, true;
            if (ui) return *out = Format::R16Uint, true; break;
    case 3: if (un) return *out = Format::R8G8, true; break;
    case 4: if (fl) return *out = Format::R32F, true; if (ui) return *out = Format::R32Uint, true;
            if (si) return *out = Format::R32Sint, true; break;
    case 5: if (fl) return *out = Format::G16R16F, true; if (un) return *out = Format::R16G16, true;
            if (nfmt == 1) return *out = Format::R16G16Snorm, true; break;
    case 7: if (fl) return *out = Format::R11G11B10F, true; break;
    case 9: if (un) return *out = Format::R10G10B10A2, true; break;
    case 10:
      if (un) return *out = bgra ? Format::A8R8G8B8 : Format::R8G8B8A8, true;
      if (srgb) return *out = bgra ? Format::B8G8R8A8Srgb : Format::R8G8B8A8Srgb, true;
      if (nfmt == 1) return *out = Format::R8G8B8A8Snorm, true;
      if (ui) return *out = Format::R8G8B8A8Uint, true;
      break;
    case 11: if (fl) return *out = Format::G32R32F, true; break;
    case 12: if (fl) return *out = Format::A16B16G16R16F, true; if (un) return *out = Format::R16G16B16A16, true; break;
    case 14: if (fl) return *out = Format::A32B32G32R32F, true; if (ui) return *out = Format::R32G32B32A32Uint, true; break;
    case 35: return *out = srgb ? Format::Bc1Srgb : Format::Bc1, true;
    case 36: return *out = srgb ? Format::Bc2Srgb : Format::Bc2, true;
    case 37: return *out = srgb ? Format::Bc3Srgb : Format::Bc3, true;
    case 38: return *out = Format::Bc4, true;
    case 39: return *out = Format::Bc5, true;
    case 40: return *out = nfmt == 1 || si ? Format::Bc6hSfloat : Format::Bc6hUfloat, true;
    case 41: return *out = srgb ? Format::Bc7Srgb : Format::Bc7, true;
  }
  return false;
}

gfx::BlendFactor Factor(uint32_t f) {
  using B = gfx::BlendFactor;
  switch (f) {
    case 0: return B::Zero;
    case 1: return B::One;
    case 2: return B::SrcColor;
    case 3: return B::OneMinusSrcColor;
    case 4: return B::SrcAlpha;
    case 5: return B::OneMinusSrcAlpha;
    case 6: return B::DstAlpha;
    case 7: return B::OneMinusDstAlpha;
    case 8: return B::DstColor;
    case 9: return B::OneMinusDstColor;
    case 10: return B::SrcAlphaSaturate;
    case 13: case 19: return B::ConstantColor;
    case 14: case 20: return B::OneMinusConstantColor;
  }
  WarnOnce(0, "a blend factor (dual source) is not drawn as the console does");
  return B::One;
}
gfx::BlendOp Comb(uint32_t c) {
  using O = gfx::BlendOp;
  switch (c) {
    case 1: return O::Subtract;
    case 2: return O::Min;
    case 3: return O::Max;
    case 4: return O::ReverseSubtract;
  }
  return O::Add;
}
gfx::StencilOp StencilOp(uint32_t op) {
  using S = gfx::StencilOp;
  switch (op) {
    case 0: return S::Keep;
    case 1: return S::Zero;
    case 3: case 4: return S::Replace;
    case 5: return S::IncrementClamp;
    case 6: return S::DecrementClamp;
    case 7: return S::Invert;
    case 8: return S::IncrementWrap;
    case 9: return S::DecrementWrap;
  }
  WarnOnce(1, "a stencil operation is not drawn as the console does");
  return S::Keep;
}
gfx::AddressMode Address(uint32_t clamp) {
  return clamp == 0 ? gfx::AddressMode::Repeat
         : clamp == 1 ? gfx::AddressMode::MirroredRepeat
                      : gfx::AddressMode::ClampToEdge;
}

gfx::SamplerDesc Sampler(const uint32_t s[4]) {
  gfx::SamplerDesc d;
  d.u = Address(s[0] & 7);
  d.v = Address((s[0] >> 3) & 7);
  d.w = Address((s[0] >> 6) & 7);
  d.max_anisotropy = float(1u << ((s[0] >> 9) & 7));
  const uint32_t cmp = (s[0] >> 12) & 7;
  d.compare = cmp != 0;
  d.compare_op = gfx::CompareOp(cmp);
  d.min_lod = float(s[1] & 0xFFF) / 256.0f;
  d.max_lod = float((s[1] >> 12) & 0xFFF) / 256.0f;
  d.lod_bias = float(int32_t(s[2] << 18) >> 18) / 256.0f;
  d.mag_linear = ((s[2] >> 20) & 3) != 0;
  d.min_linear = ((s[2] >> 22) & 3) != 0;
  const uint32_t mip = (s[2] >> 26) & 3;
  d.mip_linear = mip == 2;
  if (mip == 0) d.max_lod = d.min_lod + 0.25f;
  return d;
}

// ---- state shared by draws and dispatches

struct Target {
  gaddr addr;
  uint32_t w, h;
  Format fmt;
  gfx::TextureId tex;
};
struct DepthTarget {
  gaddr addr;
  uint32_t w, h;
  Format fmt;
  gfx::DepthId depth;
};
struct Texture {
  gfx::TextureId tex = 0;
  uint64_t hash = 0;
  uint64_t checked = ~0ull;  // the submission it was last compared in
};
struct Program {
  gfx::ProgramId id = 0;
  Translated vs, ps;
  std::string error;
};

struct State {
  std::mutex mu;
  gfx::Device* dev = nullptr;
  std::vector<Target> targets;
  std::vector<DepthTarget> depths;
  std::map<std::string, Texture> textures;
  std::unordered_map<std::string, Program> programs;
  std::unordered_map<std::string, Program> compute;
  uint64_t submission = 0;
};
State& G() {
  static State* s = new State;  // the GPU thread uses it until exit
  return *s;
}

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

// Micro tiling (Sea Islands, as AMD's addressing library describes it):
// 8x8 elements (pixels, or 4x4 blocks of a compressed format) a tile, in
// the "thin" (non-displayable) order inside it: index bits x0 y0 x1 y1 x2
// y2. 1D-tiled surfaces lay their tiles out row by row.
void Detile1dThin(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h, uint32_t pitch,
                  uint32_t bpe, uint32_t dst_row) {
  const uint32_t tiles_per_row = pitch / 8, tile_bytes = 64 * bpe;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      const uint32_t i = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
      const uint64_t at = uint64_t(tile_bytes) * ((y / 8) * tiles_per_row + x / 8) + uint64_t(i) * bpe;
      std::memcpy(dst + uint64_t(y) * dst_row + uint64_t(x) * bpe, src + at, bpe);
    }
}

bool Readable(gaddr a, uint64_t n) {
  return n && g_proc->vm.mem().IsCommitted(a) && g_proc->vm.mem().IsCommitted(a + n - 1);
}

gfx::TextureId ColorTarget(State& s, gaddr addr, uint32_t w, uint32_t h, Format fmt) {
  for (Target& t : s.targets)
    if (t.addr == addr && t.w == w && t.h == h && t.fmt == fmt) return t.tex;
  // A different target at that address replaces the old one.
  s.targets.erase(std::remove_if(s.targets.begin(), s.targets.end(),
                                 [&](const Target& t) {
                                   if (t.addr != addr) return false;
                                   s.dev->DestroyTexture(t.tex);
                                   return true;
                                 }),
                  s.targets.end());
  gfx::ImageDesc d;
  d.w = int(w);
  d.h = int(h);
  d.fmt = fmt;
  d.color_target = true;
  const gfx::TextureId tex = s.dev->CreateImage(d);
  if (tex) s.dev->ClearImage(tex, 0, {0, 0, 0, 0});
  s.targets.push_back({addr, w, h, fmt, tex});
  return tex;
}

gfx::DepthId DepthTargetAt(State& s, gaddr addr, uint32_t w, uint32_t h, Format fmt) {
  for (DepthTarget& d : s.depths)
    if (d.addr == addr && d.w == w && d.h == h && d.fmt == fmt) return d.depth;
  s.depths.erase(std::remove_if(s.depths.begin(), s.depths.end(),
                                [&](const DepthTarget& d) {
                                  if (d.addr != addr) return false;
                                  s.dev->DestroyDepth(d.depth);
                                  return true;
                                }),
                 s.depths.end());
  const gfx::DepthId id = s.dev->CreateDepth(int(w), int(h), fmt);
  if (id) s.dev->ClearDepth(id, 1.0f, fmt == Format::D32FS8 ? 0 : -1);
  s.depths.push_back({addr, w, h, fmt, id});
  return id;
}

// A T#'s texture: the colour target at its address, else its texels from
// guest memory (compared once a submission and uploaded when they change).
// Only level 0 and linear layouts are read as they are yet.
gfx::TextureId TextureFor(State& s, const ImageRef& ref) {
  const uint32_t* t = ref.t;
  const gaddr addr = (uint64_t(t[0]) | uint64_t(t[1] & 0xFF) << 32) << 8;
  const uint32_t w = (t[2] & 0x3FFF) + 1, h = ((t[2] >> 14) & 0x3FFF) + 1;
  for (Target& tg : s.targets)
    if (tg.addr == addr) return tg.tex;
  for (DepthTarget& d : s.depths)
    if (d.addr == addr) return d.depth;
  Format fmt;
  if (!TextureFormat(t, &fmt)) {
    WarnOnce(2, "a texture format is not drawn yet (textures read as black)");
    return 0;
  }
  const uint32_t type = t[3] >> 28;
  if (type != 9) {
    WarnOnce(3, "texture types other than 2D are not drawn yet (read as black)");
    return 0;
  }
  // Tile modes (the console's table): 8 and 31 linear, 13 1D-tiled thin;
  // the others (2D, depth) are read as linear yet.
  const uint32_t tiling = (t[3] >> 20) & 0x1F;
  const bool tiled1d = tiling == 13;
  if (tiling != 8 && tiling != 31 && !tiled1d)
    WarnOnce(4, "2D-tiled and depth-tiled textures are read as linear yet (they look scrambled)");
  const uint32_t pitch = ((t[4] >> 13) & 0x3FFF) + 1;
  const uint32_t row = gfx::RowPitch(fmt, pitch);
  const uint32_t rows = tiled1d ? (gfx::RowCount(fmt, h) + 7) & ~7u : gfx::RowCount(fmt, h);
  const uint64_t bytes = uint64_t(row) * rows;
  if (!Readable(addr, bytes)) return 0;
  char k[96];
  std::snprintf(k, sizeof k, "%llx:%u:%u:%u:%u", (unsigned long long)addr, w, h, unsigned(fmt), pitch);
  Texture& e = s.textures[k];
  if (e.tex && e.checked == s.submission) return e.tex;
  const uint8_t* p = GPtr(addr);
  const uint64_t hash = Fnv(p, size_t(bytes));
  e.checked = s.submission;
  if (e.tex && e.hash == hash) return e.tex;
  if (!e.tex) {
    gfx::ImageDesc d;
    d.w = int(w);
    d.h = int(h);
    d.fmt = fmt;
    e.tex = s.dev->CreateImage(d);
  }
  e.hash = hash;
  if (!e.tex) return 0;
  if (tiled1d) {
    const uint32_t bpe = gfx::IsCompressed(fmt) ? gfx::BitsPerPixel(fmt) * 2 : gfx::BitsPerPixel(fmt) / 8;
    const uint32_t wel = gfx::IsCompressed(fmt) ? (w + 3) / 4 : w;
    const uint32_t pel = gfx::IsCompressed(fmt) ? (pitch + 3) / 4 : pitch;
    std::vector<uint8_t> lin(uint64_t(wel) * bpe * gfx::RowCount(fmt, h));
    Detile1dThin(p, lin.data(), wel, gfx::RowCount(fmt, h), pel, bpe, wel * bpe);
    s.dev->UpdateImage(e.tex, 0, 0, {0, 0, int(w), int(h)}, lin.data(), wel * bpe);
  } else {
    s.dev->UpdateImage(e.tex, 0, 0, {0, 0, int(w), int(h)}, p, row);
  }
  return e.tex;
}

// The device's view of a stage's resolved resources: its buffers, its
// textures with their samplers, its uniforms.
struct Bound {
  std::vector<gfx::BufferData> ro;
  std::vector<gfx::RwBufferData> rw;
  std::vector<gfx::TextureId> textures;
  std::vector<gfx::SamplerDesc> samplers;
  std::vector<gfx::ColorTarget> images;
  std::vector<uint32_t> uniforms;
  gfx::ProgramResources res;
};
void Bind(State& s, const StageState& st, const Resolved& r, Bound& b) {
  for (const BufferRange& br : r.buffers) {
    const bool rw = st.stage == Stage::Compute && br.written;
    const bool ok = Readable(br.base, br.size);
    if (rw) b.rw.push_back({ok ? GPtr(br.base) : nullptr, ok ? br.size : 0});
    else b.ro.push_back({ok ? GPtr(br.base) : nullptr, ok ? br.size : 0});
  }
  for (const ImageRef& ref : r.images) {
    if (ref.storage) {
      WarnOnce(5, "compute image stores are not drawn yet");
      b.images.push_back({});
      continue;
    }
    b.textures.push_back(TextureFor(s, ref));
    b.samplers.push_back(Sampler(ref.s));
  }
  b.uniforms = Uniforms(st, r);
  b.res.textures = b.textures.data();
  b.res.samplers = b.samplers.data();
  b.res.buffers = b.ro.data();
  b.res.uniforms = b.uniforms.data();
}

gfx::ProgramStage PixelStub() {
  gfx::ProgramStage st;
  st.glsl = "void main() {}\n";
  return st;
}

// ---- draws

StageState VertexState(const Regs& r) {
  StageState vs;
  vs.stage = Stage::Vertex;
  vs.code = uint64_t(r.sh[kPgmLoVs]) << 8 | uint64_t(r.sh[kPgmLoVs + 1] & 0xFF) << 40;
  vs.rsrc1 = r.sh[kPgmLoVs + 2];
  vs.rsrc2 = r.sh[kPgmLoVs + 3];
  std::memcpy(vs.user, r.sh + kPgmLoVs + 4, sizeof vs.user);
  return vs;
}
StageState PixelState(const Regs& r) {
  StageState ps;
  ps.stage = Stage::Pixel;
  ps.code = uint64_t(r.sh[kPgmLoPs]) << 8 | uint64_t(r.sh[kPgmLoPs + 1] & 0xFF) << 40;
  ps.rsrc1 = r.sh[kPgmLoPs + 2];
  ps.rsrc2 = r.sh[kPgmLoPs + 3];
  std::memcpy(ps.user, r.sh + kPgmLoPs + 4, sizeof ps.user);
  ps.ps_input_ena = r.context[kPsInputEna];
  ps.ps_input_addr = r.context[kPsInputAddr];
  std::memcpy(ps.ps_input_cntl, r.context + kPsInputCntl, sizeof ps.ps_input_cntl);
  ps.z_format = r.context[kZFormat];
  ps.col_format = r.context[kColFormat];
  return ps;
}

// The scissor of a draw: the screen, window, generic and viewport 0
// scissors (top-left and bottom-right registers) intersected.
gfx::Rect Scissor(const Regs& r) {
  gfx::Rect s{0, 0, 16384, 16384};
  for (uint32_t tl : {kScreenScissorTl, kWindowScissorTl, kGenericScissorTl, kVportScissorTl}) {
    const uint32_t a = r.context[tl], b = r.context[tl + 1];
    s.left = std::max(s.left, int32_t(a & 0x7FFF));
    s.top = std::max(s.top, int32_t((a >> 16) & 0x7FFF));
    s.right = std::min(s.right, int32_t(b & 0x7FFF));
    s.bottom = std::min(s.bottom, int32_t((b >> 16) & 0x7FFF));
  }
  return s;
}

}  // namespace

void SetDevice(gfx::Device* dev) {
  std::lock_guard<std::mutex> l(G().mu);
  G().dev = dev;
}

gfx::TextureId TargetAt(gaddr addr, uint32_t w, uint32_t h) {
  std::lock_guard<std::mutex> l(G().mu);
  for (const Target& t : G().targets)
    if (t.addr == addr && t.w >= w && t.h >= h) return t.tex;
  return 0;
}

void Exclusive(const std::function<void()>& fn) {
  std::lock_guard<std::mutex> l(G().mu);
  fn();
}

void Draw(const Regs& r, const DrawCall& call) {
  State& s = G();
  std::lock_guard<std::mutex> l(s.mu);
  if (!s.dev || !call.count) return;
  const uint32_t* cx = r.context;
  if (cx[kVgtShaderStagesEn] & 0x3F) {
    WarnOnce(6, "draws with geometry or tessellation shaders are skipped");
    return;
  }

  // Targets: colour targets 0 on while they have a format, the depth.
  gfx::ProgramDraw d;
  int ntargets = 0;
  for (int i = 0; i < 4; i++) {
    const uint32_t* cb = cx + kCbColor0Base + 0xF * i;
    if (((cb[4] >> 2) & 0x1F) == 0) break;
    Format fmt;
    if (!ColorFormat(cb[4], &fmt)) {
      WarnOnce(7, "a colour target format is not drawn yet (the draw is skipped)");
      return;
    }
    const uint32_t pitch = ((cb[1] & 0x7FF) + 1) * 8;
    const uint32_t height = uint32_t((uint64_t(cb[2] & 0x3FFFFF) + 1) * 64 / pitch);
    d.targets[i] = {ColorTarget(s, uint64_t(cb[0]) << 8, pitch, height, fmt), 0};
    ntargets++;
  }
  const uint32_t zfmt = cx[kDbZInfo] & 3;
  const bool stencil = cx[kDbStencilInfo] & 1;
  if (zfmt) {
    const uint32_t size = cx[kDbDepthSize];
    const uint32_t dw = ((size & 0x7FF) + 1) * 8, dh = (((size >> 11) & 0x7FF) + 1) * 8;
    const Format f = zfmt == 1 ? Format::D16 : stencil ? Format::D32FS8 : Format::D32;
    d.depth = DepthTargetAt(s, uint64_t(cx[kDbZReadBase]) << 8, dw, dh, f);
    // A depth (and stencil) clear: the draw writes the clear values.
    if (cx[kDbRenderControl] & 3) {
      s.dev->ClearDepth(d.depth, AsFloat(cx[kDbDepthClear]),
                        (cx[kDbRenderControl] & 2) ? int(cx[kDbStencilClear] & 0xFF) : -1);
      return;
    }
  }
  if (!ntargets && !d.depth) return;

  // Shaders.
  StageState vs = VertexState(r), ps = PixelState(r);
  Resolved rv = Resolve(vs);
  if (!rv.error.empty()) {
    WarnOnce(8, ("a vertex shader cannot run yet (draws skipped): " + rv.error).c_str());
    return;
  }
  Resolved rp;
  if (ps.code) {
    rp = Resolve(ps);
    if (!rp.error.empty()) {
      WarnOnce(9, ("a pixel shader cannot run yet (draws skipped): " + rp.error).c_str());
      return;
    }
  }
  const std::string key = rv.key + '|' + rp.key;
  Program& pr = s.programs[key];
  if (!pr.id && pr.error.empty()) {
    pr.vs = Translate(vs, rv);
    if (ps.code) pr.ps = Translate(ps, rp);
    else pr.ps.stage = PixelStub();
    pr.error = !pr.vs.error.empty() ? pr.vs.error : pr.ps.error;
    if (pr.error.empty()) {
      gfx::ProgramDesc pd;
      pd.vs = pr.vs.stage;
      pd.fs = pr.ps.stage;
      pr.id = s.dev->CreateProgram(pd);
    } else {
      WarnF("GPU: a draw's shader is not translated yet (such draws are skipped): %s\n",
            pr.error.c_str());
    }
  }
  if (!pr.id) return;

  // Fixed state.
  gfx::PipelineState& ps_ = d.state;
  switch (r.uconfig[kPrimitiveType]) {
    case 1: ps_.topology = gfx::Topology::PointList; break;
    case 2: ps_.topology = gfx::Topology::LineList; break;
    case 3: ps_.topology = gfx::Topology::LineStrip; break;
    case 4: ps_.topology = gfx::Topology::TriangleList; break;
    case 6: ps_.topology = gfx::Topology::TriangleStrip; break;
    default:
      WarnOnce(10, "a primitive type is not drawn yet (fans, rectangles, quads: skipped)");
      return;
  }
  const uint32_t mode = cx[kPaSuScModeCntl];
  ps_.cull = (mode & 3) == 3 ? gfx::CullMode::None
             : (mode & 1)    ? gfx::CullMode::Front
             : (mode & 2)    ? gfx::CullMode::Back
                             : gfx::CullMode::None;
  if ((mode & 3) == 3) return;  // both faces culled
  ps_.front_ccw = !(mode & 4);
  if (mode & 0x1800) {
    ps_.depth_bias = AsFloat(cx[kPolyOffsetScale + 1]);
    ps_.depth_bias_slope = AsFloat(cx[kPolyOffsetScale]);
  }
  const uint32_t dc = cx[kDbDepthControl];
  ps_.depth_test = dc & 2;
  ps_.depth_write = (dc & 4) && (dc & 2);
  ps_.depth_compare = gfx::CompareOp((dc >> 4) & 7);
  if ((dc & 1) && stencil) {
    ps_.stencil = true;
    const uint32_t sc = cx[kDbStencilControl], ref = cx[kDbStencilRefMask];
    ps_.stencil_front = {StencilOp(sc & 0xF), StencilOp((sc >> 8) & 0xF), StencilOp((sc >> 4) & 0xF),
                         gfx::CompareOp((dc >> 8) & 7)};
    ps_.stencil_back = ps_.stencil_front;
    if (dc & 0x80) {
      ps_.stencil_back = {StencilOp((sc >> 12) & 0xF), StencilOp((sc >> 20) & 0xF),
                          StencilOp((sc >> 16) & 0xF), gfx::CompareOp((dc >> 20) & 7)};
    }
    ps_.stencil_ref = uint8_t(ref & 0xFF);
    ps_.stencil_read_mask = uint8_t(ref >> 8);
    ps_.stencil_write_mask = uint8_t(ref >> 16);
  }
  const uint32_t mask = cx[kCbTargetMask];
  for (int i = 0; i < ntargets; i++) {
    const uint32_t bc = cx[kBlendControl + i];
    gfx::TargetBlend& b = ps_.blend[i];
    b.enable = bc & (1u << 30);
    b.src_color = Factor(bc & 0x1F);
    b.color_op = Comb((bc >> 5) & 7);
    b.dst_color = Factor((bc >> 8) & 0x1F);
    if (bc & (1u << 29)) {
      b.src_alpha = Factor((bc >> 16) & 0x1F);
      b.alpha_op = Comb((bc >> 21) & 7);
      b.dst_alpha = Factor((bc >> 24) & 0x1F);
    } else {
      b.src_alpha = b.src_color;
      b.alpha_op = b.color_op;
      b.dst_alpha = b.dst_color;
    }
    b.write_mask = uint8_t((mask >> (4 * i)) & 0xF);
    if (!(pr.ps.mrt_mask & (1u << i))) b.write_mask = 0;
  }
  ps_.blend_constant = {AsFloat(cx[kCbBlendRed]), AsFloat(cx[kCbBlendRed + 1]),
                        AsFloat(cx[kCbBlendRed + 2]), AsFloat(cx[kCbBlendRed + 3])};

  // Viewport 0 from its scale and offset; a y scale that is positive
  // flips the image (the vertex shader negates y, the winding swaps).
  const float xs = AsFloat(cx[kVportXScale]), xo = AsFloat(cx[kVportXScale + 1]);
  const float ys = AsFloat(cx[kVportXScale + 2]), yo = AsFloat(cx[kVportXScale + 3]);
  const float zs = AsFloat(cx[kVportXScale + 4]), zo = AsFloat(cx[kVportXScale + 5]);
  d.viewport.x = uint32_t(std::max(0.0f, xo - std::fabs(xs)));
  d.viewport.y = uint32_t(std::max(0.0f, yo - std::fabs(ys)));
  d.viewport.width = uint32_t(std::max(1.0f, 2 * std::fabs(xs)));
  d.viewport.height = uint32_t(std::max(1.0f, 2 * std::fabs(ys)));
  // PA_CL_CLIP_CNTL.DX_CLIP_SPACE_DEF clear: z in -1..1 maps through the
  // scale and offset; the shader halves it to 0..1 (Uniforms below).
  const bool gl_clip = !(cx[kPaClClipCntl] & (1u << 19));
  d.viewport.min_z = gl_clip ? zo - zs : zo;
  d.viewport.max_z = zo + zs;
  if (d.viewport.min_z > d.viewport.max_z) std::swap(d.viewport.min_z, d.viewport.max_z);
  const bool flip = ys > 0;
  if (flip) ps_.front_ccw = !ps_.front_ccw;
  d.scissor = Scissor(r);

  // Resources and the call.
  Bound bv, bp;
  Bind(s, vs, rv, bv);
  if (ps.code) Bind(s, ps, rp, bp);
  bv.uniforms[20] = cx[kStepRate0];
  bv.uniforms[21] = cx[kStepRate0 + 1];
  bv.uniforms[22] = (flip ? 1 : 0) | (gl_clip ? 2 : 0);
  d.program = pr.id;
  d.vs = bv.res;
  d.fs = bp.res;
  d.vertex_count = call.count;
  d.instance_count = std::max(call.instances, 1u);
  if (call.indices) {
    if (!Readable(call.indices, uint64_t(call.count) * call.index_size)) return;
    d.indices = GPtr(call.indices);
    d.index_size = call.index_size;
  }
  s.dev->DrawProgram(d);
}

void Dispatch(const Regs& r, const uint32_t groups[3]) {
  State& s = G();
  std::lock_guard<std::mutex> l(s.mu);
  if (!s.dev) return;
  StageState cs;
  cs.stage = Stage::Compute;
  cs.code = uint64_t(r.sh[kComputePgmLo]) << 8 | uint64_t(r.sh[kComputePgmLo + 1] & 0xFF) << 40;
  cs.rsrc1 = r.sh[kComputeRsrc1];
  cs.rsrc2 = r.sh[kComputeRsrc1 + 1];
  std::memcpy(cs.user, r.sh + kComputeUser, sizeof cs.user);
  for (int i = 0; i < 3; i++) cs.threads[i] = r.sh[kComputeThreads + i] & 0x3FF;
  Resolved rc = Resolve(cs);
  if (!rc.error.empty()) {
    WarnOnce(11, ("a compute shader cannot run yet (dispatches skipped): " + rc.error).c_str());
    return;
  }
  Program& pr = s.compute[rc.key];
  if (!pr.id && pr.error.empty()) {
    pr.vs = Translate(cs, rc);
    pr.error = pr.vs.error;
    if (pr.error.empty()) {
      gfx::ProgramDesc pd;
      pd.cs = pr.vs.stage;
      for (int i = 0; i < 3; i++) pd.local_size[i] = std::max(cs.threads[i], 1u);
      pr.id = s.dev->CreateProgram(pd);
    } else {
      WarnF("GPU: a compute shader is not translated yet (such dispatches are skipped): %s\n",
            pr.error.c_str());
    }
  }
  if (!pr.id) return;
  Bound b;
  Bind(s, cs, rc, b);
  for (const gfx::ColorTarget& img : b.images)
    if (!img.tex) return;  // image stores: not yet
  gfx::ProgramDispatch d;
  d.program = pr.id;
  d.cs = b.res;
  d.rw_buffers = b.rw.data();
  d.rw_images = b.images.data();
  d.groups[1] = groups[1];
  d.groups[2] = groups[2];
  // More groups in x than the host runs at once: in parts, each told
  // where it starts.
  constexpr uint32_t kMaxGroups = 65535;
  for (uint32_t first = 0; first < groups[0]; first += kMaxGroups) {
    d.groups[0] = std::min(groups[0] - first, kMaxGroups);
    b.uniforms[23] = first;
    s.dev->DispatchProgram(d);
  }
}

void NextSubmission() {
  std::lock_guard<std::mutex> l(G().mu);
  G().submission++;
}

}  // namespace recomp::orbis::gpu
