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
#include "orbis/gpu/tiling.h"
#include "runtime/log.h"
#include "x86/runtime.h"

namespace recomp::orbis::gpu {
namespace {

using gfx::Format;

// Context registers.
constexpr uint32_t kDbRenderControl = 0x000, kDbDepthView = 0x002, kDbZInfo = 0x010,
                   kDbStencilInfo = 0x011, kDbZReadBase = 0x012, kDbStencilReadBase = 0x013,
                   kDbDepthSize = 0x016,
                   kDbDepthClear = 0x00B, kDbStencilClear = 0x00A, kScreenScissorTl = 0x00C,
                   kWindowScissorTl = 0x081, kGenericScissorTl = 0x090, kVportScissorTl = 0x094, kCbTargetMask = 0x08E,
                   kCbBlendRed = 0x105, kDbStencilControl = 0x10B, kDbStencilRefMask = 0x10C,
                   kVportXScale = 0x10F, kPsInputCntl = 0x191,
                   kPsInputEna = 0x1B3, kPsInputAddr = 0x1B4, kZFormat = 0x1C4, kColFormat = 0x1C5,
                   kBlendControl = 0x1E0, kDbDepthControl = 0x200, kPaClClipCntl = 0x204,
                   kPaSuScModeCntl = 0x205, kPolyOffsetScale = 0x20B, kVgtGsMode = 0x290,
                   kVgtGsOutPrimType = 0x29B, kStepRate0 = 0x2A8, kVgtEsgsRingItemsize = 0x2AB,
                   kVgtGsvsRingItemsize = 0x2AC, kVgtGsMaxVertOut = 0x2CE, kVgtShaderStagesEn = 0x2D5,
                   kVgtGsInstanceCnt = 0x2E4, kCbColor0Base = 0x318;
// SH registers.
constexpr uint32_t kPgmLoPs = 0x008, kPgmLoVs = 0x048, kPgmLoGs = 0x088, kPgmLoEs = 0x0C8,
                   kComputeThreads = 0x207,
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
  bool storage;  // also a compute stage's storage image
};
// A copy of a target's top-left corner at the size a T# samples it with
// (targets are as large as their pitch and slice, or DB_DEPTH_SIZE, which
// the hardware pads): a colour image, or a depth one for a depth target.
struct TargetView {
  gaddr addr;
  uint32_t w, h;
  gfx::TextureId tex;
  gfx::DepthId depth;
};
struct DepthTarget {
  gaddr addr;
  uint32_t w, h;
  Format fmt;
  gfx::DepthId depth;
  gaddr stencil;  // DB_STENCIL_READ_BASE (0: no stencil)
  uint32_t layers;  // 0: 2D; else a 2D array of them
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
  std::vector<TargetView> views;
  gfx::ProgramId stencil_program = 0;  // SampledStencil's
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

// A T#'s base address: 38 bits of 256-byte units (word 1's bits 6-7 are
// a cache policy, MTYPE_L2).
gaddr ImageBase(const uint32_t t[8]) {
  return (uint64_t(t[0]) | uint64_t(t[1] & 0x3F) << 32) << 8;
}

bool Readable(gaddr a, uint64_t n) {
  return n && g_proc->vm.mem().IsCommitted(a) && g_proc->vm.mem().IsCommitted(a + n - 1);
}

gfx::TextureId ColorTarget(State& s, gaddr addr, uint32_t w, uint32_t h, Format fmt,
                           bool storage = false) {
  for (Target& t : s.targets)
    if (t.addr == addr && t.w == w && t.h == h && t.fmt == fmt && (t.storage || !storage))
      return t.tex;
  // A different target at that address (or one compute cannot write)
  // replaces the old one.
  s.targets.erase(std::remove_if(s.targets.begin(), s.targets.end(),
                                 [&](const Target& t) {
                                   if (t.addr != addr) return false;
                                   s.dev->DestroyTexture(t.tex);
                                   return true;
                                 }),
                  s.targets.end());
  s.views.erase(std::remove_if(s.views.begin(), s.views.end(),
                               [&](const TargetView& v) {
                                 if (v.addr != addr || !v.tex) return false;
                                 s.dev->DestroyTexture(v.tex);
                                 return true;
                               }),
                s.views.end());
  gfx::ImageDesc d;
  d.w = int(w);
  d.h = int(h);
  d.fmt = fmt;
  d.color_target = true;
  d.storage = storage;
  const gfx::TextureId tex = s.dev->CreateImage(d);
  if (tex) s.dev->ClearImage(tex, 0, {0, 0, 0, 0});
  s.targets.push_back({addr, w, h, fmt, tex, storage});
  return tex;
}

void DropDepthViews(State& s, gaddr addr) {
  s.views.erase(std::remove_if(s.views.begin(), s.views.end(),
                               [&](const TargetView& v) {
                                 if (v.addr != addr || !v.depth) return false;
                                 s.dev->DestroyDepth(v.depth);
                                 return true;
                               }),
                s.views.end());
}

gfx::DepthId NewDepth(State& s, uint32_t w, uint32_t h, uint32_t layers, Format fmt) {
  return layers ? s.dev->CreateDepthArray(int(w), int(h), int(layers), fmt)
                : s.dev->CreateDepth(int(w), int(h), fmt);
}

// A depth target with at least `layers` array layers (0: a 2D one will
// do). Slices are drawn (DB_DEPTH_VIEW) and sampled (an array T#) one at
// a time, so a target grows into an array of as many as the game has
// used, its layers so far kept.
void EnsureLayers(State& s, DepthTarget& d, uint32_t layers) {
  if (!layers || (d.layers && d.layers >= layers)) return;
  const gfx::DepthId id = NewDepth(s, d.w, d.h, layers, d.fmt);
  if (!id) return;
  for (uint32_t i = 0; i < layers; i++)
    s.dev->ClearDepth(id, 1.0f, d.fmt == Format::D32FS8 ? 0 : -1, int(i));
  s.dev->CopyDepth(d.depth, id, {0, 0, int32_t(d.w), int32_t(d.h)});
  s.dev->DestroyDepth(d.depth);
  DropDepthViews(s, d.addr);
  d.depth = id;
  d.layers = layers;
}

DepthTarget* DepthTargetAt(State& s, gaddr addr, uint32_t w, uint32_t h, Format fmt, gaddr stencil,
                           uint32_t layers) {
  for (DepthTarget& d : s.depths)
    if (d.addr == addr && d.w == w && d.h == h && d.fmt == fmt) {
      d.stencil = stencil;
      EnsureLayers(s, d, layers);
      return &d;
    }
  s.depths.erase(std::remove_if(s.depths.begin(), s.depths.end(),
                                [&](const DepthTarget& d) {
                                  if (d.addr != addr) return false;
                                  s.dev->DestroyDepth(d.depth);
                                  return true;
                                }),
                 s.depths.end());
  DropDepthViews(s, addr);
  const gfx::DepthId id = NewDepth(s, w, h, layers, fmt);
  for (uint32_t i = 0; id && i < std::max(layers, 1u); i++)
    s.dev->ClearDepth(id, 1.0f, fmt == Format::D32FS8 ? 0 : -1, int(i));
  s.depths.push_back({addr, w, h, fmt, id, stencil, layers});
  return &s.depths.back();
}

// A colour target as a T# of w x h samples it: the target itself if the
// T# covers it, else a copy of its top-left w x h, made each time (draws
// in between may have changed the target). Normalized coordinates then
// span what the T# describes, not the target's padding.
gfx::TextureId SampledTarget(State& s, const Target& tg, uint32_t w, uint32_t h) {
  w = std::min(w, tg.w);
  h = std::min(h, tg.h);
  if (w == tg.w && h == tg.h) return tg.tex;
  TargetView* view = nullptr;
  for (TargetView& v : s.views)
    if (v.addr == tg.addr && v.tex && v.w == w && v.h == h) view = &v;
  if (!view) {
    gfx::ImageDesc d;
    d.w = int(w);
    d.h = int(h);
    d.fmt = tg.fmt;
    d.color_target = true;
    const gfx::TextureId tex = s.dev->CreateImage(d);
    if (!tex) return 0;
    s.views.push_back({tg.addr, w, h, tex, 0});
    view = &s.views.back();
  }
  const gfx::Rect r{0, 0, int32_t(w), int32_t(h)};
  s.dev->StretchRect({tg.tex, 0}, r, {view->tex, 0}, r, false);
  return view->tex;
}

// The same for a depth target.
gfx::TextureId SampledDepth(State& s, const DepthTarget& dt, uint32_t w, uint32_t h) {
  w = std::min(w, dt.w);
  h = std::min(h, dt.h);
  if (w == dt.w && h == dt.h) return dt.depth;
  TargetView* view = nullptr;
  for (TargetView& v : s.views)
    if (v.addr == dt.addr && v.depth && v.w == w && v.h == h) view = &v;
  if (!view) {
    const gfx::DepthId depth = NewDepth(s, w, h, dt.layers, dt.fmt);
    if (!depth) return 0;
    s.views.push_back({dt.addr, w, h, 0, depth});
    view = &s.views.back();
  }
  s.dev->CopyDepth(dt.depth, view->depth, {0, 0, int32_t(w), int32_t(h)});
  return view->depth;
}

// A depth target's stencil as a T# of w x h in an 8-bit UNORM format
// samples it (the stencil plane in guest memory, which the host's
// depth-stencil image keeps instead): an R8 image of stencil / 255, made
// each time it is bound. Eight full-screen draws over the depth target,
// one a bit, pass where that stencil bit is set and add its weight.
gfx::TextureId SampledStencil(State& s, const DepthTarget& dt, const uint32_t t[8], uint32_t w,
                              uint32_t h) {
  Format fmt;
  if (!TextureFormat(t, &fmt) || fmt != Format::R8) {
    WarnOnce(14, "a stencil sampled in a format other than R8 is not drawn yet (read as black)");
    return 0;
  }
  w = std::min(w, dt.w);
  h = std::min(h, dt.h);
  if (!s.stencil_program) {
    gfx::ProgramDesc pd;
    pd.vs.glsl =
        "void main() {\n"
        "  gl_Position = vec4(float((gl_VertexIndex & 1) * 4 - 1), float((gl_VertexIndex & 2) * 2 - 1), 0.0, 1.0);\n"
        "}\n";
    pd.fs.glsl =
        "layout(std140, R_UNIFORMS) uniform RWeight { vec4 r_weight; };\n"
        "layout(location = 0) out vec4 r_color0;\n"
        "void main() { r_color0 = r_weight; }\n";
    pd.fs.uniform_bytes = 16;
    s.stencil_program = s.dev->CreateProgram(pd);
    if (!s.stencil_program) return 0;
  }
  TargetView* view = nullptr;
  for (TargetView& v : s.views)
    if (v.addr == dt.stencil && v.tex && v.w == w && v.h == h) view = &v;
  if (!view) {
    gfx::ImageDesc d;
    d.w = int(w);
    d.h = int(h);
    d.fmt = Format::R8;
    d.color_target = true;
    const gfx::TextureId tex = s.dev->CreateImage(d);
    if (!tex) return 0;
    s.views.push_back({dt.stencil, w, h, tex, 0});
    view = &s.views.back();
  }
  s.dev->ClearImage(view->tex, 0, {0, 0, 0, 0});
  gfx::ProgramDraw d;
  d.program = s.stencil_program;
  d.targets[0] = {view->tex, 0};
  d.depth = dt.depth;
  d.viewport = {0, 0, w, h, 0.0f, 1.0f};
  d.scissor = {0, 0, int32_t(w), int32_t(h)};
  d.vertex_count = 3;
  gfx::PipelineState& ps = d.state;
  ps.stencil = true;
  ps.stencil_front = {gfx::StencilOp::Keep, gfx::StencilOp::Keep, gfx::StencilOp::Keep,
                      gfx::CompareOp::Equal};
  ps.stencil_back = ps.stencil_front;
  ps.stencil_write_mask = 0;
  ps.blend[0].enable = true;
  ps.blend[0].src_color = ps.blend[0].dst_color = gfx::BlendFactor::One;
  ps.blend[0].src_alpha = ps.blend[0].dst_alpha = gfx::BlendFactor::One;
  for (int bit = 0; bit < 8; bit++) {
    const float weight = float(1 << bit) / 255.0f;
    const float u[4] = {weight, weight, weight, weight};
    ps.stencil_read_mask = ps.stencil_ref = uint8_t(1 << bit);
    d.fs.uniforms = u;
    s.dev->DrawProgram(d);
  }
  return view->tex;
}

// A T#'s texture: the colour target at its address, else its texels from
// guest memory (compared once a submission and uploaded when they change).
// Level 0 only, in the tile modes tiling.h reads.
gfx::TextureId TextureFor(State& s, const ImageRef& ref) {
  const uint32_t* t = ref.t;
  const gaddr addr = ImageBase(t);
  const uint32_t w = (t[2] & 0x3FFF) + 1, h = ((t[2] >> 14) & 0x3FFF) + 1;
  for (Target& tg : s.targets)
    if (tg.addr == addr) return SampledTarget(s, tg, w, h);
  const uint32_t type = t[3] >> 28;
  for (DepthTarget& d : s.depths) {
    if (d.addr == addr) {
      // A 2D array (type 13) from BASE_ARRAY to LAST_ARRAY.
      const uint32_t base = t[5] & 0x1FFF, last = (t[5] >> 13) & 0x1FFF;
      if (type == 13 && !base) EnsureLayers(s, d, last + 1);
      if ((type == 13) != (d.layers != 0) || base) {
        WarnOnce(16, "a depth target sampled other than as all its slices is not drawn yet "
                     "(read as black)");
        return 0;
      }
      return SampledDepth(s, d, w, h);
    }
    if (d.stencil && d.stencil == addr) return SampledStencil(s, d, t, w, h);
  }
  Format fmt;
  if (!TextureFormat(t, &fmt)) {
    WarnOnce(2, "a texture format is not drawn yet (textures read as black)");
    return 0;
  }
  if (type != 9) {
    WarnOnce(3, "texture types other than 2D are not drawn yet (read as black)");
    return 0;
  }
  const uint32_t tiling = (t[3] >> 20) & 0x1F;
  const uint32_t pitch = ((t[4] >> 13) & 0x3FFF) + 1;
  const bool bc = gfx::IsCompressed(fmt);
  const uint32_t bpe = bc ? gfx::BitsPerPixel(fmt) * 2 : gfx::BitsPerPixel(fmt) / 8;
  const uint32_t wel = bc ? (w + 3) / 4 : w, pel = bc ? (pitch + 3) / 4 : pitch;
  const uint32_t hel = gfx::RowCount(fmt, h);
  const uint64_t bytes = TiledBytes(tiling, pel, hel, bpe);
  if (!bytes) {
    WarnOnce(4, "a texture tile mode (rotated, thick, PRT) is not read yet (read as black)");
    return 0;
  }
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
  std::vector<uint8_t> lin(uint64_t(wel) * bpe * hel);
  Detile(tiling, p, lin.data(), wel, hel, pel, bpe, wel * bpe);
  s.dev->UpdateImage(e.tex, 0, 0, {0, 0, int(w), int(h)}, lin.data(), wel * bpe);
  return e.tex;
}

// A compute stage's storage image: a colour target at the T#'s address
// that compute can write, so what it stores the next draws sample. 2D,
// level 0 only.
gfx::TextureId StorageImage(State& s, const ImageRef& ref) {
  const uint32_t* t = ref.t;
  const gaddr addr = ImageBase(t);
  const uint32_t w = (t[2] & 0x3FFF) + 1, h = ((t[2] >> 14) & 0x3FFF) + 1;
  Format fmt;
  if (t[3] >> 28 != 9 || ((t[3] >> 12) & 0xF) != 0 || !TextureFormat(t, &fmt) ||
      !StorageQualifier(fmt)) {
    WarnOnce(5, "a compute image store is not drawn yet (not 2D, past level 0, or its format)");
    return 0;
  }
  return ColorTarget(s, addr, w, h, fmt, true);
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
    const bool rw = RunsAsCompute(st.stage) && br.written;
    const bool ok = Readable(br.base, br.size);
    if (rw) b.rw.push_back({ok ? GPtr(br.base) : nullptr, ok ? br.size : 0});
    else b.ro.push_back({ok ? GPtr(br.base) : nullptr, ok ? br.size : 0});
  }
  for (const ImageRef& ref : r.images) {
    if (ref.storage) {
      b.images.push_back({StorageImage(s, ref), 0});
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

// A stage from its SPI_SHADER_PGM_LO_xS register on.
StageState ShaderState(const Regs& r, Stage stage, uint32_t lo) {
  StageState st;
  st.stage = stage;
  st.code = uint64_t(r.sh[lo]) << 8 | uint64_t(r.sh[lo + 1] & 0xFF) << 40;
  st.rsrc1 = r.sh[lo + 2];
  st.rsrc2 = r.sh[lo + 3];
  std::memcpy(st.user, r.sh + lo + 4, sizeof st.user);
  return st;
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

// ---- geometry shaders

// A draw with an ES, a GS and a copy shader runs as two compute passes and
// a draw, in parts that fit the rings: the ES once per vertex of the
// draw's primitives (in index order, a vertex shared by primitives once
// for each), writing its outputs to the ESGS ring; the GS once per
// primitive, reading them there and writing the vertices it emits to the
// GSVS ring; then the copy shader draws them, read from that ring, as
// lists of the GS's output primitive. The rings are the guest's (at their
// V#s' addresses); each part's waves start at the rings' starts.
struct Geometry {
  StageState es, gs;
  Resolved re, rg;
  gfx::ProgramId pe = 0, pg = 0;
  gaddr gsvs = 0;
  uint32_t gsvs_vsharp[4] = {};
  uint32_t part = 0;  // primitives per part
};

// The compute program of an Export or Geometry stage.
gfx::ProgramId PassProgram(State& s, const StageState& st, const Resolved& r) {
  Program& pr = s.compute[r.key];
  if (!pr.id && pr.error.empty()) {
    pr.vs = Translate(st, r);
    pr.error = pr.vs.error;
    if (pr.error.empty()) {
      gfx::ProgramDesc pd;
      pd.cs = pr.vs.stage;
      pd.local_size[0] = 64;
      pr.id = s.dev->CreateProgram(pd);
    } else {
      WarnF("GPU: a geometry shader stage is not translated yet (such draws are skipped): %s\n",
            pr.error.c_str());
    }
  }
  return pr.id;
}

// The ES, GS and rings of a draw (VGT_SHADER_STAGES_EN: real ES, GS, copy
// shader, VGT_GS_MODE: SCENARIO_G, off chip; one GS instance), the copy
// shader's resolution sized to the rings' parts; false if it is not drawn.
bool SetUpGeometry(State& s, const Regs& r, const DrawCall& call, Resolved& rv, Geometry* g,
                   gfx::Topology* topology) {
  const uint32_t* cx = r.context;
  if (cx[kVgtShaderStagesEn] != 0xB0 || (cx[kVgtGsMode] & 0x600003) != 3 || (cx[kVgtGsInstanceCnt] & 1) ||
      call.instances > 1) {
    WarnOnce(6, "draws with tessellation, on-chip or instanced geometry shaders are skipped");
    return false;
  }
  uint32_t k;
  switch (r.uconfig[kPrimitiveType]) {
    case 1: k = 1; break;
    case 2: k = 2; break;
    case 4: k = 3; break;
    default: WarnOnce(12, "a geometry shader's input primitive type is not drawn yet"); return false;
  }
  switch (cx[kVgtGsOutPrimType] & 0x3F) {
    case 0: *topology = gfx::Topology::PointList; break;
    case 1: *topology = gfx::Topology::LineList; break;
    case 2: *topology = gfx::Topology::TriangleList; break;
    default: WarnOnce(12, "a geometry shader's output primitive type is not drawn yet"); return false;
  }
  g->es = ShaderState(r, Stage::Export, kPgmLoEs);
  g->gs = ShaderState(r, Stage::Geometry, kPgmLoGs);
  g->re = Resolve(g->es);
  g->rg = Resolve(g->gs);
  const std::string& err = !g->re.error.empty() ? g->re.error : g->rg.error;
  if (!err.empty()) {
    WarnOnce(13, ("a geometry shader stage cannot run yet (draws skipped): " + err).c_str());
    return false;
  }
  // The rings: what the ES writes, what the GS writes (stream 0 alone).
  auto written = [](const Resolved& res, BufferRange* out) {
    int n = 0;
    for (const BufferRange& b : res.buffers)
      if (b.written) *out = b, n++;
    return n == 1;
  };
  BufferRange esgs, gsvs;
  if (!written(g->re, &esgs) || !written(g->rg, &gsvs) || !(gsvs.vsharp[1] >> 31)) {
    WarnOnce(13, "a geometry shader's rings are not drawn yet (not one each, or GSVS not swizzled)");
    return false;
  }
  // Their sizes: the V#s the GS and the copy shader read them with.
  auto capacity = [](const Resolved& res, gaddr base) {
    uint32_t c = 0;
    for (const BufferRange& b : res.buffers)
      if (!b.written && b.base == base) c = std::max(c, b.size);
    return c;
  };
  const uint32_t es_wave = cx[kVgtEsgsRingItemsize] * 256, gs_wave = cx[kVgtGsvsRingItemsize] * 256;
  const uint32_t max_vert = cx[kVgtGsMaxVertOut] & 0x7FF;
  if (!es_wave || !gs_wave || !max_vert) return false;
  const uint32_t waves =
      std::min(capacity(g->rg, esgs.base) / (es_wave * k), capacity(rv, gsvs.base) / gs_wave);
  if (!waves) {
    WarnOnce(13, "a geometry shader's rings are not drawn yet (smaller than a wave)");
    return false;
  }
  g->part = 64 * waves;
  for (Resolved* res : {&g->re, &g->rg, &rv})
    for (BufferRange& b : res->buffers) {
      if (b.base == esgs.base) b.size = waves * k * es_wave;
      if (b.base == gsvs.base) b.size = waves * gs_wave;
    }
  g->gsvs = gsvs.base;
  std::memcpy(g->gsvs_vsharp, gsvs.vsharp, sizeof g->gsvs_vsharp);
  for (StageState* st : {&g->es, &g->gs}) {
    st->es_wave_bytes = es_wave;
    st->gs_wave_bytes = gs_wave;
    st->prim_verts = k;
    st->max_vert = max_vert;
  }
  g->pe = PassProgram(s, g->es, g->re);
  g->pg = PassProgram(s, g->gs, g->rg);
  return g->pe && g->pg;
}

// Runs a compute pass of n invocations with one more range (Uniforms()).
void RunPass(State& s, gfx::ProgramId program, const StageState& st, const Resolved& r, void* extra,
             uint32_t extra_bytes) {
  Bound b;
  Bind(s, st, r, b);
  if (st.stage == Stage::Export) b.ro.push_back({extra, extra_bytes});
  else b.rw.push_back({extra, extra_bytes});
  b.res.buffers = b.ro.data();
  gfx::ProgramDispatch d;
  d.program = program;
  d.cs = b.res;
  d.rw_buffers = b.rw.data();
  d.rw_images = b.images.data();
  d.groups[0] = (st.invocations + 63) / 64;
  s.dev->DispatchProgram(d);
}

// The draw's parts: the passes, then the copy shader over the emitted
// vertices, each as the copy shader's vertex index (its dword in the GSVS
// ring: the GS wave's place, then the lane's and the vertex's under the
// ring V#'s swizzle, vertex j at dword j).
void DrawGeometry(State& s, const DrawCall& call, gfx::ProgramDraw& d, Geometry& g) {
  const uint32_t k = g.es.prim_verts, max_vert = g.gs.max_vert, prims = call.count / k;
  if (call.indices && !Readable(call.indices, uint64_t(call.count) * call.index_size)) return;
  const uint32_t* v = g.gsvs_vsharp;
  const uint32_t es = 2u << ((v[3] >> 19) & 3), is = 8u << ((v[3] >> 21) & 3), stride = (v[1] >> 16) & 0x3FFF;
  std::vector<uint32_t> ids, emits, indices, strip;
  for (uint32_t first = 0; first < prims; first += g.part) {
    const uint32_t n = std::min(prims - first, g.part);
    ids.resize(size_t(n) * k);
    for (uint32_t i = 0; i < n * k; i++) {
      const uint64_t at = uint64_t(first) * k + i;
      ids[i] = uint32_t(at);
      if (call.indices) {
        ids[i] = 0;
        std::memcpy(&ids[i], GPtr(call.indices + at * call.index_size), call.index_size);
      }
    }
    g.es.invocations = n * k;
    RunPass(s, g.pe, g.es, g.re, ids.data(), n * k * 4);
    g.gs.invocations = n;
    g.gs.first_prim = first;
    emits.assign(size_t(n) * max_vert, 0);
    RunPass(s, g.pg, g.gs, g.rg, emits.data(), n * max_vert * 4);
    indices.clear();
    for (uint32_t p = 0; p < n; p++) {
      const uint32_t t = p % 64;
      strip.clear();
      for (uint32_t j = 0; j < max_vert; j++) {
        const uint32_t e = emits[size_t(p) * max_vert + j];
        if (!(e & 1)) break;
        if (e & 2) strip.clear();
        const uint32_t o = 4 * j;
        strip.push_back(((p / 64) * g.gs.gs_wave_bytes + ((t / is) * stride + o / es) * es * is +
                         (t % is) * es + o % es) / 4);
        const size_t m = strip.size();
        if (d.state.topology == gfx::Topology::PointList) {
          indices.push_back(strip[m - 1]);
        } else if (d.state.topology == gfx::Topology::LineList && m >= 2) {
          indices.insert(indices.end(), {strip[m - 2], strip[m - 1]});
        } else if (d.state.topology == gfx::Topology::TriangleList && m >= 3) {
          // A strip's odd triangles swap their first two vertices (winding).
          if (m % 2) indices.insert(indices.end(), {strip[m - 3], strip[m - 2], strip[m - 1]});
          else indices.insert(indices.end(), {strip[m - 2], strip[m - 3], strip[m - 1]});
        }
      }
    }
    if (indices.empty()) continue;
    d.indices = indices.data();
    d.index_size = 4;
    d.vertex_count = uint32_t(indices.size());
    d.instance_count = 1;
    s.dev->DrawProgram(d);
  }
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
    // DB_DEPTH_VIEW: the array slices drawn, SLICE_START to SLICE_MAX.
    const uint32_t first = cx[kDbDepthView] & 0x7FF;
    const uint32_t last = std::max((cx[kDbDepthView] >> 13) & 0x7FF, first);
    const DepthTarget* dt = DepthTargetAt(s, uint64_t(cx[kDbZReadBase]) << 8, dw, dh, f,
                                          stencil ? uint64_t(cx[kDbStencilReadBase]) << 8 : 0,
                                          last ? last + 1 : 0);
    d.depth = dt->depth;
    // A depth (and stencil) clear: the draw writes the clear values.
    if (cx[kDbRenderControl] & 3) {
      for (uint32_t i = first; i <= last; i++)
        s.dev->ClearDepth(d.depth, AsFloat(cx[kDbDepthClear]),
                          (cx[kDbRenderControl] & 2) ? int(cx[kDbStencilClear] & 0xFF) : -1, int(i));
      return;
    }
    if (last != first) WarnOnce(15, "a draw into several depth slices is drawn into the first only");
    d.depth_layer = dt->layers ? int(first) : 0;
  }
  if (!ntargets && !d.depth) return;

  // Shaders.
  StageState vs = ShaderState(r, Stage::Vertex, kPgmLoVs), ps = PixelState(r);
  Resolved rv = Resolve(vs);
  if (!rv.error.empty()) {
    WarnOnce(8, ("a vertex shader cannot run yet (draws skipped): " + rv.error).c_str());
    return;
  }
  const bool gs = cx[kVgtShaderStagesEn] & 0x3F;
  Geometry geo;
  gfx::Topology gs_topology = gfx::Topology::TriangleList;
  if (gs && !SetUpGeometry(s, r, call, rv, &geo, &gs_topology)) return;
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
  switch (gs ? ~0u : r.uconfig[kPrimitiveType]) {
    case ~0u: ps_.topology = gs_topology; break;
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
  if (gs) return DrawGeometry(s, call, d, geo);
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
    if (!img.tex) return;
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
