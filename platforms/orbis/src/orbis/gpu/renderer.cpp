// Register layouts (context, SH and uconfig offsets) as AMD documents them
// for Sea Islands; formats and descriptors as in shader.cpp.

#include "orbis/gpu/renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "orbis/gpu/shader.h"
#include "orbis/gpu/tiling.h"
#include "runtime/log.h"
#include "runtime/write_watch.h"
#include "x86/runtime.h"

namespace recomp::orbis::gpu {
namespace {

using gfx::Format;

// Context registers.
constexpr uint32_t kDbRenderControl = 0x000, kDbCountControl = 0x001, kDbDepthView = 0x002, kDbHtileDataBase = 0x005,
                   kDbZInfo = 0x010, kDbStencilInfo = 0x011, kDbZReadBase = 0x012, kDbStencilReadBase = 0x013,
                   kDbDepthSize = 0x016,
                   kDbDepthClear = 0x00B, kDbStencilClear = 0x00A, kScreenScissorTl = 0x00C,
                   kWindowScissorTl = 0x081, kGenericScissorTl = 0x090, kVportScissorTl = 0x094, kCbTargetMask = 0x08E,
                   kCbBlendRed = 0x105, kDbStencilControl = 0x10B, kDbStencilRefMask = 0x10C,
                   kVportXScale = 0x10F, kPsInputCntl = 0x191,
                   kPsInputEna = 0x1B3, kPsInputAddr = 0x1B4, kZFormat = 0x1C4, kColFormat = 0x1C5,
                   kBlendControl = 0x1E0, kDbDepthControl = 0x200, kCbColorControl = 0x202, kPaClClipCntl = 0x204,
                   kPaSuScModeCntl = 0x205, kVgtGsMode = 0x290, kPolyOffsetDbFmt = 0x2DE,
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
  FatalF("GPU: blend factor %u (dual source) is not drawn yet", f);
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
  FatalF("GPU: stencil operation %u is not drawn yet", op);
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

// A target's metadata in guest memory: a depth target's HTILE, a colour
// target's CMASK. A write of all of it, from its start, in which every
// tile reads as cleared (the bits `mask` picks out of each dword are 0) is
// the fast clear, applied at the target's next draw; other writes into it
// are not modelled. Writes the CPU sees are read at that draw (MetaCheck);
// a dispatch's write of all of it is kept in a GPU buffer and applied
// there instead (MetaOf, MetaTest), then checked at the next submission.
struct Meta {
  gaddr base = 0;      // 0: none
  uint32_t mask = 0;   // 0: its clears are not modelled
  bool clear = false;  // it says cleared, not applied yet
  gaddr written = 0;   // a write into it, checked at the target's next draw
  uint64_t written_size = 0;
  gfx::BufferId buf = 0;
  uint32_t buf_size = 0;
  bool gpu = false, verify = false;
};
// A colour target's CMASK (kept by its address: the target's image may
// be replaced) and the clear colour (CB_COLORn_CLEAR_WORD0-1) its last
// fast clear was applied with, while no draw has eliminated it.
struct Cmask {
  Meta meta;
  uint32_t words[2] = {};
  bool applied = false;
};
struct Target {
  gaddr addr;
  uint32_t w, h;
  Format fmt;
  gfx::TextureId tex;
  bool storage;  // also a compute stage's storage image
  uint32_t swap;  // CB_COLORn_INFO's COMP_SWAP (the image is in the shader's order)
};
// A copy of a target's top-left corner at the size a T# samples it with
// (targets are as large as their pitch and slice, or DB_DEPTH_SIZE, which
// the hardware pads): a colour image, or a depth one for a depth target.
struct TargetView {
  gaddr addr;
  uint32_t w, h;
  gfx::TextureId tex;
  gfx::DepthId depth;
  uint32_t levels = 1;  // a colour target's levels (MippedTarget)
  Format fmt = Format::Unknown;  // a stencil's: R8 or R8Uint (SampledStencil)
};
struct DepthTarget {
  gaddr addr;
  uint32_t w, h;
  Format fmt;
  gfx::DepthId depth;
  gaddr stencil;  // DB_STENCIL_READ_BASE (0: no stencil)
  uint32_t layers;  // 0: 2D; else a 2D array of them
  // DB_HTILE_DATA_BASE; its clears are modelled while it tracks depth
  // alone (ZMASK, bits 0-3).
  Meta htile;
};
// Guest memory a GPU copy is made from, compared once a submission
// (Changed).
struct Compared {
  uint64_t hash = 0;
  uint64_t checked = ~0ull;  // the submission it was last compared in
  uint32_t same = 0;         // comparisons in a row that found it unchanged
  watch::Watch watch;
};
struct Texture {
  gfx::TextureId tex = 0;
  Compared cmp;
};
// A run of guest memory that stages read, kept in a GPU buffer.
struct KeptRange {
  gfx::BufferId buffer = 0;
  uint32_t size = 0;
  Compared cmp;
  uint64_t used = 0;  // the submission it was last read in
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
  gfx::ProgramId stencil_program = 0, stencil_uint = 0;  // SampledStencil's
  std::map<uint32_t, gfx::ProgramId> meta_tests;  // MetaTest's, by mask
  gfx::BufferId meta_draw = 0;
  gfx::ProgramId htile_clear = 0, color_clear = 0;  // HtileClear's, ColorClear's
  std::map<gaddr, Cmask> cmasks;
  gfx::ProgramId strip_program = 0;    // StripProgram's
  // Geometry shaders' GPU buffers: the ESGS and GSVS rings, the GS's
  // emits, the copy shader's draw (DrawGeometry); their sizes.
  gfx::BufferId geometry[4] = {};
  uint32_t geometry_bytes[4] = {};
  std::vector<DepthTarget> depths;
  std::map<std::string, Texture> textures;
  std::map<gaddr, KeptRange> kept;  // by base; they do not overlap
  std::vector<gfx::BufferId> retired;  // merged ones' (draws may still bind them)
  std::unordered_map<std::string, Program> programs;
  std::unordered_map<std::string, Program> compute;
  uint64_t submission = 0;
  // Occlusion counts: draws DB_COUNT_CONTROL counts add 1 for each pixel
  // they write to a count target of their size (R32F, blended); CountsAdd
  // sums those on the GPU into `counted` as the sums land.
  struct CountTarget {
    gfx::TextureId tex = 0;
    bool dirty = false;
  };
  std::map<std::pair<uint32_t, uint32_t>, CountTarget> count_targets;
  gfx::ProgramId count_sum = 0;
  std::atomic<uint64_t> counted{0};
};
State& G() {
  static State* s = new State;  // the GPU thread uses it until exit
  return *s;
}

// What BeforeRead, Signal and Settle keep: the guest ranges of results
// not landed yet and the signals waiting, each with the device's count of
// queued results it waits for. Its own lock: reads under State::mu land
// results.
struct Landings {
  struct Range {
    gaddr base;
    uint64_t size, after;
  };
  struct Later {
    gaddr base;
    uint64_t size, after;
    std::function<void()> fn;
    std::vector<uint32_t> words;  // what fn writes, if known
  };
  std::mutex mu;
  std::atomic<size_t> pending{0};  // ranges and signals
  std::vector<Range> ranges;
  std::deque<Later> signals;
  uint64_t landed = 0;
};
Landings& L() {
  static Landings* l = new Landings;  // the GPU threads use it until exit
  return *l;
}

// Lands what the device has finished (all, with wait), then forgets the
// ranges and runs the signals that waited for it. Under L().mu.
void LandLocked(Landings& l, bool wait) {
  gfx::Device* dev = G().dev;
  if (dev) l.landed = dev->LandWrites(wait);
  std::erase_if(l.ranges, [&](const Landings::Range& r) { return r.after <= l.landed; });
  while (!l.signals.empty() && l.signals.front().after <= l.landed) {
    l.signals.front().fn();
    l.signals.pop_front();
  }
  l.pending = l.ranges.size() + l.signals.size();
}

// A hash of guest memory, to see whether texels changed: four lanes of
// xxHash64's round over 32-byte blocks, the tail a byte at a time (one
// FNV-1a lane over every byte was most of the GPU thread's time).
uint64_t ContentHash(const uint8_t* p, size_t n) {
  constexpr uint64_t k1 = 0x9E3779B185EBCA87ull, k2 = 0xC2B2AE3D27D4EB4Full;
  const auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
  uint64_t lane[4] = {k1, k2, ~k1, ~k2};
  size_t i = 0;
  for (; i + 32 <= n; i += 32)
    for (int l = 0; l < 4; l++) {
      uint64_t w;
      std::memcpy(&w, p + i + 8 * l, 8);
      lane[l] = rotl(lane[l] + w * k2, 31) * k1;
    }
  uint64_t h = n;
  for (int l = 0; l < 4; l++) h = (rotl(h, 27) ^ lane[l]) * k1;
  for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

// Whether n bytes at base changed since c's last comparison (true the
// first time); compared at most once a submission. Memory found unchanged
// twice in a row is watched from then on (write_watch.h), armed before it
// is hashed so that a write after the hash disarms it, and not hashed
// while no write has disarmed it.
bool Changed(State& s, Compared& c, gaddr base, uint64_t n) {
  if (c.checked == s.submission) return false;
  const bool first = c.checked == ~0ull && !c.same;
  c.checked = s.submission;
  if (c.watch.clean()) return false;
  if (c.same >= 2) c.watch.Arm(g_proc->vm.mem(), base, n);
  const uint64_t hash = ContentHash(GPtr(base), size_t(n));
  const bool changed = first || hash != c.hash;
  c.hash = hash;
  c.same = changed ? 0 : c.same + 1;
  return changed;
}

// The comparison of c is due again: something other than the guest's
// stores may have changed its memory.
void Recompare(Compared& c) {
  c.checked = ~0ull;
  c.watch.Disarm();
}

// A T#'s base address: 38 bits of 256-byte units (word 1's bits 6-7 are
// a cache policy, MTYPE_L2).
gaddr ImageBase(const uint32_t t[8]) {
  return (uint64_t(t[0]) | uint64_t(t[1] & 0x3F) << 32) << 8;
}

// Whether n bytes at a are guest memory; the dispatch results still to
// land there land first.
bool Readable(gaddr a, uint64_t n) {
  if (!n || !g_proc->vm.mem().IsCommitted(a) || !g_proc->vm.mem().IsCommitted(a + n - 1)) return false;
  BeforeRead(a, n);
  return true;
}

gfx::TextureId ColorTarget(State& s, gaddr addr, uint32_t w, uint32_t h, Format fmt,
                           uint32_t swap, bool storage = false) {
  for (Target& t : s.targets)
    if (t.addr == addr && t.w == w && t.h == h && t.fmt == fmt && t.swap == swap &&
        (t.storage || !storage))
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
  s.targets.push_back({addr, w, h, fmt, tex, storage, swap});
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
                                  if (d.htile.buf) s.dev->DestroyBuffer(d.htile.buf);
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

// Every target's metadata (Meta).
template <typename F>
void ForEachMeta(State& s, F f) {
  for (DepthTarget& d : s.depths) f(d.htile);
  for (auto& [base, c] : s.cmasks) f(c.meta);
}

// Guest memory a dispatch or a DMA wrote.
void MetaWritten(State& s, gaddr base, uint64_t size) {
  ForEachMeta(s, [&](Meta& m) {
    if (!m.base || m.base < base || m.base >= base + size) return;
    m.written = base;
    m.written_size = size;
    m.gpu = m.verify = false;
  });
}

// The same, written to the metadata's GPU buffer too (MetaOf).
void MetaGpuWritten(State& s, gaddr base, uint64_t size) {
  ForEachMeta(s, [&](Meta& m) {
    if (m.base != base) return;
    m.written = base;
    m.written_size = size;
    m.gpu = m.verify = true;
  });
}

bool MetaCleared(const Meta& m, gaddr base, uint64_t size) {
  BeforeRead(base, size);
  const uint8_t* p = GPtr(base);
  bool clear = m.base == base && m.mask;
  for (uint64_t i = 0; clear && i + 4 <= size; i += 4) {
    uint32_t w;
    std::memcpy(&w, p + i, 4);
    clear = !(w & m.mask);
  }
  if (!clear)
    FatalF("GPU: a write of an HTILE or CMASK (at %llX) other than a full clear is not modelled yet",
           (unsigned long long)m.base);
  return clear;
}

void MetaCheck(Meta& m) {
  if (!m.written_size || m.verify) return;
  m.clear = MetaCleared(m, m.written, m.written_size);
  m.written_size = 0;
}

// The metadata whose whole a dispatch writes at base, if its clears are
// modelled: kept in its GPU buffer too (MetaTest).
Meta* MetaOf(State& s, gaddr base, uint32_t size) {
  Meta* found = nullptr;
  ForEachMeta(s, [&](Meta& m) {
    if (!found && m.base == base && m.mask) found = &m;
  });
  if (!found) return nullptr;
  if (found->buf_size < size) {
    if (found->buf) s.dev->DestroyBuffer(found->buf);
    found->buf = s.dev->CreateBuffer(size);
    found->buf_size = found->buf ? size : 0;
  }
  return found->buf ? found : nullptr;
}

// Applies a dispatch's write of m at the target's next draw without
// waiting for it: one program turns the metadata into an indexed indirect
// draw (s.meta_draw) of a triangle if every tile reads as cleared
// (MetaCleared), of none otherwise; that triangle then writes the clear
// value (HtileClear, ColorClear). False if the device cannot run it.
bool MetaTest(State& s, const Meta& m) {
  gfx::ProgramId& test = s.meta_tests[m.mask];
  if (!test) {
    char mask[16];
    std::snprintf(mask, sizeof mask, "0x%Xu", m.mask);
    gfx::ProgramDesc pd;
    pd.cs.glsl = std::string(
        "shared uint masks[64];\n"
        "void main() {\n"
        "  uint lane = gl_LocalInvocationID.x, n = r_buf[0].y / 4u, m = 0u;\n"
        "  for (uint i = lane; i < n; i += 64u) m |= r_ld(0u, i) & ") + mask + ";\n"
        "  masks[lane] = m;\n"
        "  barrier();\n"
        "  if (lane != 0u) return;\n"
        "  for (uint i = 1u; i < 64u; i++) m |= masks[i];\n"
        "  uint w = r_rwbuf[0].x;\n"
        "  r_rw[w] = m == 0u ? 3u : 0u;\n"
        "  r_rw[w + 1u] = 1u;\n"
        "  r_rw[w + 2u] = 8u;\n"
        "  r_rw[w + 3u] = 0u;\n"
        "  r_rw[w + 4u] = 0u;\n"
        "  for (uint i = 0u; i < 3u; i++) r_rw[w + 8u + i] = i;\n"
        "}\n";
    pd.cs.buffers = 1;
    pd.cs.rw_buffers = 1;
    pd.local_size[0] = 64;
    test = s.dev->CreateProgram(pd);
  }
  if (!s.meta_draw) s.meta_draw = s.dev->CreateBuffer(64);
  if (!test || !s.meta_draw) return false;
  const gfx::BufferData in{nullptr, m.buf_size, m.buf};
  const gfx::RwBufferData out{nullptr, 64, s.meta_draw};
  gfx::ProgramDispatch t;
  t.program = test;
  t.cs.buffers = &in;
  t.rw_buffers = &out;
  s.dev->DispatchProgram(t);
  return true;
}

// A triangle over a whole target (with three vertices, or s.meta_draw's).
constexpr const char* kFullScreenVs =
    "void main() {\n"
    "  gl_Position = vec4(float((gl_VertexIndex & 1) * 4 - 1), float((gl_VertexIndex & 2) * 2 - 1), 0.0, 1.0);\n"
    "}\n";

// Writes the clear depth over each layer of d where MetaTest found its
// HTILE cleared.
void HtileClear(State& s, DepthTarget& d, float z) {
  if (!s.htile_clear) {
    gfx::ProgramDesc pd;
    pd.vs.glsl = kFullScreenVs;
    pd.fs.glsl =
        "layout(std140, R_UNIFORMS) uniform RClear { vec4 r_clear; };\n"
        "void main() { gl_FragDepth = r_clear.x; }\n";
    pd.fs.uniform_bytes = 16;
    s.htile_clear = s.dev->CreateProgram(pd);
  }
  if (!s.htile_clear || !MetaTest(s, d.htile)) return;
  const float u[4] = {z, 0, 0, 0};
  gfx::ProgramDraw c;
  c.program = s.htile_clear;
  c.depth = d.depth;
  c.viewport = {0, 0, d.w, d.h, 0.0f, 1.0f};
  c.scissor = {0, 0, int32_t(d.w), int32_t(d.h)};
  c.state.depth_test = c.state.depth_write = true;
  c.state.depth_compare = gfx::CompareOp::Always;
  c.indirect = s.meta_draw;
  c.fs.uniforms = u;
  for (uint32_t i = 0; i < std::max(d.layers, 1u); i++) {
    c.depth_layer = int(i);
    s.dev->DrawProgram(c);
  }
}

// Fills a colour target with a clear colour (CB_COLORn_CLEAR_WORD0-1: the
// target's format's bits), where MetaTest found its CMASK cleared
// (indirect) or at once. The program unpacks the words in the target's
// format: four halves, or four UNORM bytes.
void ColorClear(State& s, const Target& t, const uint32_t words[2], bool indirect) {
  uint32_t kind;
  if (t.fmt == Format::A16B16G16R16F) kind = 0;
  else if (t.fmt == Format::R8G8B8A8) kind = 1;
  else FatalF("GPU: a fast clear of a colour target of format %d is not drawn yet", int(t.fmt));
  if (t.swap) FatalF("GPU: a fast clear of a colour target with a component swap is not drawn yet");
  if (!s.color_clear) {
    gfx::ProgramDesc pd;
    pd.vs.glsl = kFullScreenVs;
    pd.fs.glsl =
        "layout(std140, R_UNIFORMS) uniform RClear { uvec4 r_clear; };\n"
        "layout(location = 0) out vec4 r_color0;\n"
        "void main() {\n"
        "  r_color0 = r_clear.z == 0u ? vec4(unpackHalf2x16(r_clear.x), unpackHalf2x16(r_clear.y))\n"
        "                             : unpackUnorm4x8(r_clear.x);\n"
        "}\n";
    pd.fs.uniform_bytes = 16;
    s.color_clear = s.dev->CreateProgram(pd);
  }
  if (!s.color_clear) return;
  const uint32_t u[4] = {words[0], words[1], kind, 0};
  gfx::ProgramDraw c;
  c.program = s.color_clear;
  c.targets[0] = {t.tex, 0};
  c.viewport = {0, 0, t.w, t.h, 0.0f, 1.0f};
  c.scissor = {0, 0, int32_t(t.w), int32_t(t.h)};
  if (indirect) c.indirect = s.meta_draw;
  else c.vertex_count = 3;
  c.fs.uniforms = u;
  s.dev->DrawProgram(c);
}

// A draw into colour target tex with CMASK fast clears (CB_COLORn_INFO's
// FAST_CLEAR; cb: its CB_COLORn_* registers): a fast clear of its CMASK
// since is applied with the clear colour the draw has. A CMASK seen the
// first time is read as it is (a clear before the target's first draw).
// A draw that eliminates fast clears (CB_COLOR_CONTROL's MODE 2) leaves
// the image as it is, since clears are applied at once: on the console it
// writes the clear colour its registers have then into the tiles still
// cleared, which must be the colour applied.
void CmaskDraw(State& s, gfx::TextureId tex, const uint32_t* cb, bool eliminate) {
  const auto t = std::find_if(s.targets.begin(), s.targets.end(),
                              [&](const Target& g) { return g.tex == tex; });
  if (t == s.targets.end()) return;
  const gaddr base = uint64_t(cb[7]) << 8;  // CB_COLORn_CMASK
  auto [it, first] = s.cmasks.try_emplace(base);
  Cmask& c = it->second;
  if (first) {
    c.meta.base = base;
    c.meta.mask = ~0u;  // a cleared tile is 0
    c.meta.written = base;
    // CB_COLORn_CMASK_SLICE's TILE_MAX: 128x128-pixel blocks, 128 bytes each.
    c.meta.written_size = uint64_t((cb[8] & 0x3FFF) + 1) * 128;
  }
  MetaCheck(c.meta);
  const uint32_t words[2] = {cb[11], cb[12]};
  if (c.meta.gpu || c.meta.clear) {
    ColorClear(s, *t, words, c.meta.gpu);
    c.meta.gpu = c.meta.clear = false;
    c.words[0] = words[0];
    c.words[1] = words[1];
    c.applied = true;
  }
  if (!eliminate) return;
  if (c.applied && (c.words[0] != words[0] || c.words[1] != words[1]))
    FatalF("GPU: a fast clear eliminated with another clear colour than it was drawn with is not "
           "modelled yet");
  c.applied = false;
}

// A colour target holds its components in the shader's order; in memory
// COMP_SWAP orders them, and a T#'s destination selects pick memory
// components. A stage's T#s of targets with a swap get selects of the
// components the image holds (and its key a note of them).
void TargetSelects(const State& s, Resolved& r) {
  // The shader component in each memory component: STD (RGBA), ALT
  // (BGRA), STD_REV (ABGR), ALT_REV (ARGB).
  static const uint32_t kOrder[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {3, 0, 1, 2}};
  for (size_t i = 0; i < r.images.size(); i++) {
    uint32_t* t = r.images[i].t;
    const auto tg = std::find_if(s.targets.begin(), s.targets.end(),
                                 [&](const Target& g) { return g.addr == ImageBase(t); });
    if (r.images[i].storage || tg == s.targets.end() || !tg->swap) continue;
    uint32_t sel = 0;
    for (int c = 0; c < 4; c++) {
      const uint32_t d = (t[3] >> (3 * c)) & 7;
      sel |= (d < 4 ? d : 4 + kOrder[tg->swap][d - 4]) << (3 * c);
    }
    t[3] = (t[3] & ~0xFFFu) | sel;
    r.key += "|t" + std::to_string(i) + ':' + std::to_string(tg->swap);
  }
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
    if (v.addr == tg.addr && v.tex && v.w == w && v.h == h && v.levels == 1) view = &v;
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

// A 2D T# of levels 0 to last at addr, colour targets among them (fmt
// theirs): an image of those levels, each the target at its address in
// the T#'s layout (the levels the GPU rendered; its top-left, as
// SampledTarget) or else read from memory, made each time.
gfx::TextureId MippedTarget(State& s, gaddr addr, Format fmt, const uint32_t t[8], uint32_t last) {
  const uint32_t w = (t[2] & 0x3FFF) + 1, h = ((t[2] >> 14) & 0x3FFF) + 1;
  if (gfx::IsCompressed(fmt)) FatalF("GPU: a compressed colour target");
  TextureLayout lay;
  lay.index = (t[3] >> 20) & 0x1F;
  lay.pow2 = (t[3] >> 25) & 1;
  lay.bpe = gfx::BitsPerPixel(fmt) / 8;
  lay.w = w;
  lay.h = h;
  lay.pitch = ((t[4] >> 13) & 0x3FFF) + 1;
  const uint32_t dfmt = (t[1] >> 20) & 0x3F;
  lay.depth_format = dfmt == 2 || dfmt == 4;
  TargetView* view = nullptr;
  for (TargetView& v : s.views)
    if (v.addr == addr && v.tex && v.w == w && v.h == h && v.levels == last + 1) view = &v;
  if (!view) {
    gfx::ImageDesc d;
    d.w = int(w);
    d.h = int(h);
    d.levels = int(last + 1);
    d.fmt = fmt;
    d.color_target = true;
    const gfx::TextureId tex = s.dev->CreateImage(d);
    if (!tex) return 0;
    s.views.push_back({addr, w, h, tex, 0, last + 1});
    view = &s.views.back();
  }
  std::vector<uint8_t> lin;
  for (uint32_t i = 0; i <= last; i++) {
    Level l;
    if (!LevelOf(lay, i, &l))
      FatalF("GPU: a colour target's tile mode %u is not read yet", lay.index);
    const gaddr at = addr + l.offset;
    const int32_t lw = int32_t(std::max(1u, w >> i)), lh = int32_t(std::max(1u, h >> i));
    const Target* level = nullptr;
    for (const Target& o : s.targets)
      if (o.addr == at) level = &o;
    if (level) {
      const gfx::Rect r{0, 0, std::min(lw, int32_t(level->w)), std::min(lh, int32_t(level->h))};
      s.dev->StretchRect({level->tex, 0}, r, {view->tex, int(i)}, r, false);
    } else if (Readable(at, l.slice_bytes)) {
      lin.resize(uint64_t(l.w) * l.h * lay.bpe);
      DetileSlice(l, lay.bpe, 0, GPtr(at), lin.data(), l.w * lay.bpe);
      s.dev->UpdateImage(view->tex, int(i), 0, {0, 0, lw, lh}, lin.data(), l.w * lay.bpe);
    }
  }
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

// A depth target's stencil as a T# of w x h in an 8-bit UNORM or UINT
// format samples it (the stencil plane in guest memory, which the host's
// depth-stencil image keeps instead): an R8 image of stencil / 255, made
// each time it is bound. Eight full-screen draws over the depth target,
// one a bit, pass where that stencil bit is set and add its weight (k /
// 255 sums exactly in UNORM). For UINT, one more draw writes those values
// as integers to an R8Uint image.
gfx::TextureId SampledStencil(State& s, const DepthTarget& dt, const uint32_t t[8], uint32_t w,
                              uint32_t h) {
  Format fmt;
  if (!TextureFormat(t, &fmt) || (fmt != Format::R8 && fmt != Format::R8Uint)) {
    FatalF("GPU: a stencil sampled in a format other than R8 is not drawn yet (data format %u, number "
           "format %u)", (t[1] >> 20) & 0x3F, (t[1] >> 26) & 0xF);
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
  auto view_of = [&](Format f) -> TargetView* {
    for (TargetView& v : s.views)
      if (v.addr == dt.stencil && v.tex && v.w == w && v.h == h && v.fmt == f) return &v;
    gfx::ImageDesc d;
    d.w = int(w);
    d.h = int(h);
    d.fmt = f;
    d.color_target = true;
    const gfx::TextureId tex = s.dev->CreateImage(d);
    if (!tex) return nullptr;
    s.views.push_back({dt.stencil, w, h, tex, 0, 1, f});
    return &s.views.back();
  };
  TargetView* view = view_of(Format::R8);
  if (!view) return 0;
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
  if (fmt == Format::R8) return view->tex;
  if (!s.stencil_uint) {
    gfx::ProgramDesc pd;
    pd.vs.glsl = kFullScreenVs;
    pd.fs.glsl =
        "layout(R_TEXTURE(0)) uniform sampler2D r_tex0;\n"
        "layout(location = 0) out uvec4 r_color0;\n"
        "void main() {\n"
        "  r_color0 = uvec4(uint(round(texelFetch(r_tex0, ivec2(gl_FragCoord.xy), 0).r * 255.0)));\n"
        "}\n";
    pd.fs.textures = 1;
    s.stencil_uint = s.dev->CreateProgram(pd);
    if (!s.stencil_uint) return 0;
  }
  const gfx::TextureId unorm = view->tex;
  TargetView* uview = view_of(Format::R8Uint);
  if (!uview) return 0;
  gfx::ProgramDraw u;
  u.program = s.stencil_uint;
  u.targets[0] = {uview->tex, 0};
  u.viewport = d.viewport;
  u.scissor = d.scissor;
  u.vertex_count = 3;
  const gfx::SamplerDesc nearest;
  u.fs.textures = &unorm;
  u.fs.samplers = &nearest;
  s.dev->DrawProgram(u);
  return uview->tex;
}

// A T#'s texture: the colour target at its address, else its texels from
// guest memory (compared once a submission and uploaded when they change):
// its levels from BASE_LEVEL to LAST_LEVEL and its slices (a 2D array's
// from slice 0 to LAST_ARRAY, a cube's faces, a 3D texture's depth), as
// tiling.h lays them out. Nothing (black) where memory is not mapped.
gfx::TextureId TextureFor(State& s, const ImageRef& ref) {
  const uint32_t* t = ref.t;
  const gaddr addr = ImageBase(t);
  const uint32_t w = (t[2] & 0x3FFF) + 1, h = ((t[2] >> 14) & 0x3FFF) + 1;
  const uint32_t type = t[3] >> 28, base = (t[3] >> 12) & 0xF, last = (t[3] >> 16) & 0xF;
  const uint32_t first_slice = t[5] & 0x1FFF, last_slice = (t[5] >> 13) & 0x1FFF;
  for (Target& tg : s.targets) {
    if (tg.addr != addr) continue;
    if (type != 9 || base)
      FatalF("GPU: a colour target sampled as a T# of type %u from level %u is not drawn yet", type,
             base);
    return last ? MippedTarget(s, addr, tg.fmt, t, last) : SampledTarget(s, tg, w, h);
  }
  for (DepthTarget& d : s.depths) {
    if (d.addr == addr) {
      // A 2D array (type 13) from BASE_ARRAY to LAST_ARRAY.
      if (type == 13 && !first_slice) EnsureLayers(s, d, last_slice + 1);
      if ((type == 13) != (d.layers != 0) || first_slice || base || last)
        FatalF("GPU: a depth target sampled as a T# of type %u, levels %u-%u, slices %u-%u is "
               "not drawn yet", type, base, last, first_slice, last_slice);
      return SampledDepth(s, d, w, h);
    }
    if (d.stencil && d.stencil == addr) return SampledStencil(s, d, t, w, h);
  }
  if (!Readable(addr, 1)) return 0;
  Format fmt;
  const uint32_t dfmt = (t[1] >> 20) & 0x3F;
  if (!TextureFormat(t, &fmt))
    FatalF("GPU: texture format %u, number format %u not drawn yet", dfmt, (t[1] >> 26) & 0xF);
  TextureLayout lay;
  gfx::ImageDesc desc;
  uint32_t layers = 1;  // the image's: slices, faces or depth
  lay.index = (t[3] >> 20) & 0x1F;
  lay.pow2 = (t[3] >> 25) & 1;
  lay.w = w;
  lay.h = h;
  lay.pitch = ((t[4] >> 13) & 0x3FFF) + 1;
  switch (type) {
    case 9: break;
    case 13:  // memory holds DEPTH + 1 slices
      if (first_slice)
        FatalF("GPU: a 2D array texture from slice %u is not drawn yet", first_slice);
      lay.slices = (t[4] & 0x1FFF) + 1;
      layers = last_slice + 1;
      desc.kind = gfx::TextureKind::Tex2DArray;
      break;
    case 11:
      if ((t[4] & 0x1FFF) || first_slice) FatalF("GPU: cube arrays are not drawn yet");
      lay.slices = layers = 6;
      desc.kind = gfx::TextureKind::Cube;
      break;
    case 10:
      lay.depth = (t[4] & 0x1FFF) + 1;
      lay.volume = true;
      desc.kind = gfx::TextureKind::Tex3D;
      break;
    default: FatalF("GPU: texture type %u (1D, multisampled) is not drawn yet", type);
  }
  if (last < base || base > 13) FatalF("GPU: a texture's levels %u-%u", base, last);
  if (layers > lay.slices && type != 10)
    FatalF("GPU: a texture shows slices 0-%u of %u", layers - 1, lay.slices);
  const bool bc = gfx::IsCompressed(fmt);
  lay.bpe = bc ? gfx::BitsPerPixel(fmt) * 2 : gfx::BitsPerPixel(fmt) / 8;
  lay.blocks = bc;
  lay.depth_format = (dfmt == 2 || dfmt == 4) && type != 10;
  Level top;
  if (!LevelOf(lay, last, &top))
    FatalF("GPU: a texture's tile mode %u (rotated, thick, PRT, or compressed and linear) is not "
           "read yet", lay.index);
  const uint64_t bytes = top.offset + top.slice_bytes * top.depth * lay.slices;
  // Levels past the first the GPU rendered into.
  for (const Target& o : s.targets) {
    if (o.addr <= addr || o.addr >= addr + bytes) continue;
    if (type != 9 || base || o.fmt != fmt)
      FatalF("GPU: a colour target inside a texture of type %u from level %u (format %u, the "
             "target's %u) is not drawn yet", type, base, unsigned(fmt), unsigned(o.fmt));
    return MippedTarget(s, addr, fmt, t, last);
  }
  if (!Readable(addr, bytes)) return 0;
  char k[128];
  std::snprintf(k, sizeof k, "%llx:%u:%u:%u:%u:%u:%u:%u:%u:%u", (unsigned long long)addr, w, h,
                unsigned(fmt), lay.pitch, type, base, last, layers, lay.depth);
  Texture& e = s.textures[k];
  if (!Changed(s, e.cmp, addr, bytes) && e.tex) return e.tex;
  const uint8_t* p = GPtr(addr);
  if (!e.tex) {
    desc.w = int(std::max(1u, w >> base));
    desc.h = int(std::max(1u, h >> base));
    desc.layers = type == 10 ? int(std::max(1u, lay.depth >> base)) : int(layers);
    desc.levels = int(last - base + 1);
    desc.fmt = fmt;
    e.tex = s.dev->CreateImage(desc);
  }
  if (!e.tex) return 0;
  std::vector<uint8_t> lin;
  for (uint32_t i = base; i <= last; i++) {
    Level l;
    LevelOf(lay, i, &l);
    lin.resize(uint64_t(l.w) * l.h * lay.bpe);
    const uint32_t n = type == 10 ? std::max(1u, lay.depth >> i) : layers;
    for (uint32_t sl = 0; sl < n; sl++) {
      DetileSlice(l, lay.bpe, sl, p + l.offset, lin.data(), l.w * lay.bpe);
      s.dev->UpdateImage(e.tex, int(i - base), int(sl),
                         {0, 0, int(std::max(1u, w >> i)), int(std::max(1u, h >> i))}, lin.data(),
                         l.w * lay.bpe);
    }
  }
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
    FatalF("GPU: a compute image store is not drawn yet (not 2D, past level 0, or its format)");
  }
  return ColorTarget(s, addr, w, h, fmt, 0, true);
}

// Read-only ranges of at least this many bytes (vertex buffers, mostly)
// are kept in GPU buffers, compared once a submission and uploaded when
// they change, instead of copied for each draw.
constexpr uint32_t kKeptMin = 64 << 10;
// Unread for this many submissions, they are let go.
constexpr uint64_t kKeptIdle = 600;

// The kept range holding [base, base + size), made (from it and any it
// overlaps) when there is none; its base in *at. Null when the range
// cannot be kept (more than 64 MiB with those it overlaps).
KeptRange* Kept(State& s, gaddr base, uint32_t size, gaddr* at) {
  gaddr lo = base, hi = base + size;
  auto it = s.kept.upper_bound(base);
  if (it != s.kept.begin() && std::prev(it)->first + std::prev(it)->second.size > base) --it;
  auto first = it;
  for (; it != s.kept.end() && it->first < hi; ++it) {
    lo = std::min(lo, it->first);
    hi = std::max<gaddr>(hi, it->first + it->second.size);
  }
  // Readable lands the dispatch results still to come there first, or
  // the comparison below would see the memory before them.
  if (hi - lo > (64u << 20) || !Readable(lo, hi - lo)) return nullptr;
  if (first == it || first->first != lo || first->first + first->second.size != hi) {
    for (auto e = first; e != it; ++e) s.retired.push_back(e->second.buffer);
    s.kept.erase(first, it);
    KeptRange& k = s.kept.try_emplace(lo).first->second;
    k.size = uint32_t(hi - lo);
    k.buffer = s.dev->CreateBuffer(k.size);
    first = s.kept.find(lo);
  }
  KeptRange& k = first->second;
  if (Changed(s, k.cmp, lo, k.size)) s.dev->UpdateBuffer(k.buffer, 0, GPtr(lo), k.size);
  k.used = s.submission;
  *at = lo;
  return &k;
}

// Kept ranges that [base, base + size) overlaps compare again.
void KeptWritten(State& s, gaddr base, uint64_t size) {
  auto it = s.kept.upper_bound(base);
  if (it != s.kept.begin()) --it;
  for (; it != s.kept.end() && it->first < base + size; ++it)
    if (it->first + it->second.size > base) Recompare(it->second.cmp);
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
  std::vector<gaddr> metas;  // metadata written to their GPU buffers (MetaOf)
};
// A guest range kept in a GPU buffer instead (a geometry shader's ring).
struct GpuRange {
  gaddr base;
  gfx::BufferId buffer;
};
void Bind(State& s, const StageState& st, const Resolved& r, Bound& b,
          const std::vector<GpuRange>* gpu = nullptr) {
  for (const BufferRange& br : r.buffers) {
    const bool rw = RunsAsCompute(st.stage) && br.written;
    gfx::BufferId kept = 0;
    if (gpu)
      for (const GpuRange& g : *gpu)
        if (g.base == br.base) kept = g.buffer;
    if (kept) {
      if (rw) b.rw.push_back({nullptr, br.size, kept});
      else b.ro.push_back({nullptr, br.size, kept});
      continue;
    }
    gaddr at;
    KeptRange* k;
    if (!rw && br.size >= kKeptMin && (k = Kept(s, br.base, br.size, &at)) && !((br.base - at) & 3)) {
      b.ro.push_back({nullptr, br.size, k->buffer, uint32_t(br.base - at)});
      continue;
    }
    const bool ok = Readable(br.base, br.size);
    Meta* m = rw && ok ? MetaOf(s, br.base, br.size) : nullptr;
    if (m) {
      b.rw.push_back({GPtr(br.base), br.size, m->buf, true});
      b.metas.push_back(br.base);
      continue;
    }
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
    // The T#'s MIN_LOD (4.8 fixed point) clamps as the sampler's does.
    gfx::SamplerDesc& sd = b.samplers.back();
    sd.min_lod = std::max(sd.min_lod, float((ref.t[1] >> 8) & 0xFFF) / 256.0f);
    sd.max_lod = std::max(sd.max_lod, sd.min_lod);
  }
  b.uniforms = Uniforms(st, r);
  b.res.textures = b.textures.data();
  b.res.samplers = b.samplers.data();
  b.res.buffers = b.ro.data();
  b.res.uniforms = b.uniforms.data();
}

gfx::ProgramStage PixelStub(int count) {
  gfx::ProgramStage st;
  st.glsl = count < 0 ? "void main() {}\n"
                      : "layout(location = " + std::to_string(count) +
                            ") out vec4 r_count;\nvoid main() { r_count = vec4(1.0); }\n";
  return st;
}

// ---- occlusion counts

// The count target of a w x h draw (zero when made).
State::CountTarget& CountTargetFor(State& s, uint32_t w, uint32_t h) {
  State::CountTarget& c = s.count_targets[{w, h}];
  if (!c.tex) {
    gfx::ImageDesc d;
    d.w = int(w);
    d.h = int(h);
    d.fmt = Format::R32F;
    d.color_target = true;
    c.tex = s.dev->CreateImage(d);
    if (!c.tex) FatalF("GPU: no %ux%u occlusion count target", w, h);
    s.dev->ClearImage(c.tex, 0, {0, 0, 0, 0});
  }
  return c;
}

// Sums the count targets drawn into since the last sum on the GPU (64
// bits, exact; a pixel's count is exact to 2^24 draws over it), adds the
// sums to s.counted as they land and zeroes the targets.
void CountsAdd(State& s) {
  if (!s.count_sum) {
    gfx::ProgramDesc pd;
    pd.cs.glsl =
        "layout(R_TEXTURE(0)) uniform sampler2D r_tex0;\n"
        "void main() {\n"
        "  ivec2 p = ivec2(gl_GlobalInvocationID.xy), n = textureSize(r_tex0, 0);\n"
        "  if (p.x >= n.x || p.y >= n.y) return;\n"
        "  uint v = uint(texelFetch(r_tex0, p, 0).r);\n"
        "  if (v == 0u) return;\n"
        "  uint w = r_rwbuf[0].x, old = atomicAdd(r_rw[w], v);\n"
        "  if (old + v < old) atomicAdd(r_rw[w + 1u], 1u);\n"
        "}\n";
    pd.cs.textures = 1;
    pd.cs.rw_buffers = 1;
    pd.local_size[0] = pd.local_size[1] = 8;
    s.count_sum = s.dev->CreateProgram(pd);
    if (!s.count_sum) FatalF("GPU: the occlusion count sum does not compile");
  }
  for (auto& [size, c] : s.count_targets) {
    if (!c.dirty) continue;
    c.dirty = false;
    uint32_t* sum = new uint32_t[2]{0, 0};
    const gfx::RwBufferData out{sum, 8};
    const gfx::SamplerDesc nearest;
    gfx::ProgramDispatch d;
    d.program = s.count_sum;
    d.cs.textures = &c.tex;
    d.cs.samplers = &nearest;
    d.rw_buffers = &out;
    d.land_later = true;
    d.groups[0] = (size.first + 7) / 8;
    d.groups[1] = (size.second + 7) / 8;
    s.dev->DispatchProgram(d);
    s.dev->ClearImage(c.tex, 0, {0, 0, 0, 0});
    Signal(0, 0, [sum, &s] {
      s.counted += sum[0] | uint64_t(sum[1]) << 32;
      delete[] sum;
    });
  }
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
  for (uint32_t i = 0; i < 8; i++) {
    const uint32_t info = r.context[kCbColor0Base + 0xF * i + 4], cf = (info >> 2) & 0x1F;
    const uint32_t n = cf == 1 || cf == 2 || cf == 4 ? 1 : cf == 3 || cf == 5 || cf == 11 ? 2 : 0;
    if (n && (info >> 11) & 3) ps.col_swap |= (n << 2 | ((info >> 11) & 3)) << (4 * i);
  }
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
// lists of the GS's output primitive. The rings stay on the GPU (the
// guest's, at their V#s' addresses, are not written); each part's waves
// start at the rings' starts.
struct Geometry {
  StageState es, gs;
  Resolved re, rg;
  gfx::ProgramId pe = 0, pg = 0;
  uint32_t gsvs_vsharp[4] = {};
  uint32_t part = 0;  // primitives per part
  std::vector<GpuRange> rings;
};

// One of State::geometry, at least `bytes` long.
gfx::BufferId GeometryBuffer(State& s, int i, uint32_t bytes) {
  if (s.geometry_bytes[i] < bytes) {
    if (s.geometry[i]) s.dev->DestroyBuffer(s.geometry[i]);
    s.geometry_bytes[i] = std::max(bytes, 2 * s.geometry_bytes[i]);
    s.geometry[i] = s.dev->CreateBuffer(s.geometry_bytes[i]);
  }
  return s.geometry[i];
}

// The program that turns a part's emits (the GS's, a word per vertex it
// may emit: bit 0 emitted, bit 1 a strip starts) into the copy shader's
// indexed indirect draw: its five words, then from word 8 the indices of
// the strips' points, lines or triangles in primitive order. 64 lanes
// each take a run of primitives: they count their indices, one lane
// turns the counts into offsets, then they write them.
gfx::ProgramId StripProgram(State& s) {
  if (s.strip_program) return s.strip_program;
  gfx::ProgramDesc pd;
  pd.cs.glsl =
      "layout(std140, R_UNIFORMS) uniform RStrip {\n"
      // primitives, emits per primitive, GSVS bytes per GS wave, the GSVS
      // V#'s element size, index stride and stride, output topology
      "  uint n, max_vert, wave_bytes, es, is, stride, topo;\n"
      "};\n"
      "shared uint counts[64];\n"
      "uint Walk(uint p, bool write, uint at) {\n"
      "  uint t = p % 64u, m = 0u, a = 0u, b = 0u, k = 0u, out_at = r_rwbuf[0].x + 8u + at;\n"
      "  for (uint j = 0u; j < max_vert; j++) {\n"
      "    uint e = r_ld(0u, p * max_vert + j);\n"
      "    if ((e & 1u) == 0u) break;\n"
      "    if ((e & 2u) != 0u) m = 0u;\n"
      "    uint o = 4u * j;\n"
      "    uint v = ((p / 64u) * wave_bytes + ((t / is) * stride + (o / es) * es) * is +\n"
      "              (t % is) * es + o % es) / 4u;\n"
      "    m++;\n"
      "    if (topo == 0u) {\n"
      "      if (write) r_rw[out_at + k] = v;\n"
      "      k += 1u;\n"
      "    } else if (topo == 1u && m >= 2u) {\n"
      "      if (write) { r_rw[out_at + k] = b; r_rw[out_at + k + 1u] = v; }\n"
      "      k += 2u;\n"
      "    } else if (topo == 2u && m >= 3u) {\n"
      // A strip's odd triangles swap their first two vertices (winding).
      "      if (write) {\n"
      "        r_rw[out_at + k] = (m % 2u) == 1u ? a : b;\n"
      "        r_rw[out_at + k + 1u] = (m % 2u) == 1u ? b : a;\n"
      "        r_rw[out_at + k + 2u] = v;\n"
      "      }\n"
      "      k += 3u;\n"
      "    }\n"
      "    a = b;\n"
      "    b = v;\n"
      "  }\n"
      "  return k;\n"
      "}\n"
      "void main() {\n"
      "  uint lane = gl_LocalInvocationID.x, per = (n + 63u) / 64u;\n"
      "  uint first = min(n, lane * per), last = min(n, first + per), c = 0u;\n"
      "  for (uint p = first; p < last; p++) c += Walk(p, false, 0u);\n"
      "  counts[lane] = c;\n"
      "  barrier();\n"
      "  if (lane == 0u) {\n"
      "    uint at = 0u;\n"
      "    for (uint i = 0u; i < 64u; i++) { uint ci = counts[i]; counts[i] = at; at += ci; }\n"
      "    uint w = r_rwbuf[0].x;\n"
      "    r_rw[w] = at;\n"
      "    r_rw[w + 1u] = 1u;\n"
      "    r_rw[w + 2u] = 8u;\n"
      "    r_rw[w + 3u] = 0u;\n"
      "    r_rw[w + 4u] = 0u;\n"
      "  }\n"
      "  barrier();\n"
      "  uint at = counts[lane];\n"
      "  for (uint p = first; p < last; p++) at += Walk(p, true, at);\n"
      "}\n";
  pd.cs.buffers = 1;
  pd.cs.rw_buffers = 1;
  pd.cs.uniform_bytes = 32;
  pd.local_size[0] = 64;
  s.strip_program = s.dev->CreateProgram(pd);
  return s.strip_program;
}

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
      FatalF("GPU: a geometry shader stage is not translated yet: %s", pr.error.c_str());
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
    FatalF("GPU: draws with tessellation, on-chip or instanced geometry shaders are not drawn yet");
  }
  uint32_t k;
  switch (r.uconfig[kPrimitiveType]) {
    case 1: k = 1; break;
    case 2: k = 2; break;
    case 4: k = 3; break;
    default:
      FatalF("GPU: a geometry shader's input primitive type %02X is not drawn yet",
             r.uconfig[kPrimitiveType]);
  }
  switch (cx[kVgtGsOutPrimType] & 0x3F) {
    case 0: *topology = gfx::Topology::PointList; break;
    case 1: *topology = gfx::Topology::LineList; break;
    case 2: *topology = gfx::Topology::TriangleList; break;
    default:
      FatalF("GPU: a geometry shader's output primitive type %u is not drawn yet",
             cx[kVgtGsOutPrimType] & 0x3F);
  }
  g->es = ShaderState(r, Stage::Export, kPgmLoEs);
  g->gs = ShaderState(r, Stage::Geometry, kPgmLoGs);
  g->re = Resolve(g->es);
  g->rg = Resolve(g->gs);
  TargetSelects(s, g->re);
  TargetSelects(s, g->rg);
  const std::string& err = !g->re.error.empty() ? g->re.error : g->rg.error;
  if (!err.empty()) {
    FatalF("GPU: a geometry shader stage cannot run yet: %s", err.c_str());
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
    FatalF("GPU: a geometry shader's rings are not drawn yet (not one each, or GSVS not swizzled)");
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
  if (!es_wave || !gs_wave || !max_vert)
    FatalF("GPU: a geometry shader draw with ring item sizes %u, %u and %u vertices out", es_wave,
           gs_wave, max_vert);
  uint32_t waves =
      std::min(capacity(g->rg, esgs.base) / (es_wave * k), capacity(rv, gsvs.base) / gs_wave);
  if (!waves) {
    FatalF("GPU: a geometry shader's rings are not drawn yet (smaller than a wave)");
  }
  // No more than the draw's primitives fill: each pass copies its rings
  // into its arenas and back, whole rings for a draw of a few waves.
  waves = std::min(waves, (call.count / k + 63) / 64);
  if (!waves) return false;
  g->part = 64 * waves;
  for (Resolved* res : {&g->re, &g->rg, &rv})
    for (BufferRange& b : res->buffers) {
      if (b.base == esgs.base) b.size = waves * k * es_wave;
      if (b.base == gsvs.base) b.size = waves * gs_wave;
    }
  std::memcpy(g->gsvs_vsharp, gsvs.vsharp, sizeof g->gsvs_vsharp);
  g->rings = {{esgs.base, GeometryBuffer(s, 0, waves * k * es_wave)},
              {gsvs.base, GeometryBuffer(s, 1, waves * gs_wave)}};
  for (StageState* st : {&g->es, &g->gs}) {
    st->es_wave_bytes = es_wave;
    st->gs_wave_bytes = gs_wave;
    st->prim_verts = k;
    st->max_vert = max_vert;
  }
  g->pe = PassProgram(s, g->es, g->re);
  g->pg = PassProgram(s, g->gs, g->rg);
  return g->pe && g->pg && StripProgram(s);
}

// Runs a compute pass of n invocations with one more range (Uniforms()):
// the ES's vertex indices, or the GS's emits (into a GPU buffer).
void RunPass(State& s, gfx::ProgramId program, const StageState& st, const Resolved& r,
             const Geometry& g, void* extra, uint32_t extra_bytes, gfx::BufferId extra_buffer) {
  Bound b;
  Bind(s, st, r, b, &g.rings);
  if (st.stage == Stage::Export) b.ro.push_back({extra, extra_bytes});
  else b.rw.push_back({extra, extra_bytes, extra_buffer});
  b.res.buffers = b.ro.data();
  gfx::ProgramDispatch d;
  d.program = program;
  d.cs = b.res;
  d.rw_buffers = b.rw.data();
  d.rw_images = b.images.data();
  d.groups[0] = (st.invocations + 63) / 64;
  s.dev->DispatchProgram(d);
}

// The draw's parts: the passes, the strips' indices (StripProgram), then
// the copy shader over the emitted vertices, each as the copy shader's
// vertex index (its dword in the GSVS ring: the GS wave's place, then the
// lane's and the vertex's under the ring V#'s swizzle, vertex j at dword
// j).
void DrawGeometry(State& s, const DrawCall& call, gfx::ProgramDraw& d, Geometry& g) {
  const uint32_t k = g.es.prim_verts, max_vert = g.gs.max_vert, prims = call.count / k;
  if (call.indices && !Readable(call.indices, uint64_t(call.count) * call.index_size)) return;
  const uint32_t* v = g.gsvs_vsharp;
  uint32_t params[8] = {0, max_vert, g.gs.gs_wave_bytes, 2u << ((v[3] >> 19) & 3),
                        8u << ((v[3] >> 21) & 3), (v[1] >> 16) & 0x3FFF};
  params[6] = d.state.topology == gfx::Topology::PointList  ? 0
              : d.state.topology == gfx::Topology::LineList ? 1
                                                            : 2;
  const gfx::BufferId emits_buf = GeometryBuffer(s, 2, g.part * max_vert * 4);
  const gfx::BufferId draw_buf = GeometryBuffer(s, 3, 32 + 3 * g.part * max_vert * 4);
  std::vector<uint32_t> ids, emits;
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
    RunPass(s, g.pe, g.es, g.re, g, ids.data(), n * k * 4, 0);
    g.gs.invocations = n;
    g.gs.first_prim = first;
    emits.assign(size_t(n) * max_vert, 0);
    RunPass(s, g.pg, g.gs, g.rg, g, emits.data(), n * max_vert * 4, emits_buf);
    params[0] = n;
    const gfx::BufferData in{nullptr, n * max_vert * 4, emits_buf};
    const gfx::RwBufferData out{nullptr, 32 + 3 * n * max_vert * 4, draw_buf};
    gfx::ProgramDispatch sd;
    sd.program = s.strip_program;
    sd.cs.buffers = &in;
    sd.cs.uniforms = params;
    sd.rw_buffers = &out;
    s.dev->DispatchProgram(sd);
    d.indirect = draw_buf;
    s.dev->DrawProgram(d);
  }
}

}  // namespace

void BeforeRead(gaddr a, uint64_t n) {
  Landings& l = L();
  if (!l.pending.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(l.mu);
  bool hit = false;
  for (const Landings::Range& r : l.ranges) hit |= r.base < a + n && a < r.base + r.size;
  for (const Landings::Later& r : l.signals) hit |= r.base < a + n && a < r.base + r.size;
  if (hit) LandLocked(l, true);
}

void Signal(gaddr a, uint64_t n, std::function<void()> fn, std::vector<uint32_t> words) {
  Landings& l = L();
  std::lock_guard<std::mutex> lk(l.mu);
  gfx::Device* dev = G().dev;
  const uint64_t after = dev ? dev->WritesQueued() : 0;
  if (l.signals.empty() && after > l.landed) LandLocked(l, false);
  if (l.signals.empty() && after <= l.landed) {
    fn();
    return;
  }
  l.signals.push_back({a, n, after, std::move(fn), std::move(words)});
  l.pending = l.ranges.size() + l.signals.size();
}

bool SignalValue(gaddr a, uint32_t* v) {
  Landings& l = L();
  if (!l.pending.load(std::memory_order_relaxed)) return false;
  std::lock_guard<std::mutex> lk(l.mu);
  for (auto it = l.signals.rbegin(); it != l.signals.rend(); ++it) {
    if (a + 4 <= it->base || a >= it->base + it->size) continue;
    if ((a - it->base) % 4 || 4 * it->words.size() != it->size) return false;
    *v = it->words[(a - it->base) / 4];
    return true;
  }
  return false;
}

void Settle(bool wait) {
  Landings& l = L();
  if (!l.pending.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(l.mu);
  LandLocked(l, wait);
}

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

uint64_t OcclusionCount() {
  {
    std::lock_guard<std::mutex> l(G().mu);
    if (G().dev) CountsAdd(G());
  }
  Settle(true);
  return G().counted;
}

void Exclusive(const std::function<void()>& fn) {
  std::lock_guard<std::mutex> l(G().mu);
  fn();
}

namespace {

// A primitive type the device has no topology for as a list of the
// device's, by vertex: fans and polygons (0 i i+1), quads (0 1 2, 0 2 3)
// and quad strips (2i 2i+1 2i+3, 2i 2i+3 2i+2) as triangles, line loops as
// lines.
std::vector<uint32_t> Expand(uint32_t prim, uint32_t n) {
  std::vector<uint32_t> v;
  const auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { v.insert(v.end(), {a, b, c}); };
  switch (prim) {
    case 5: case 0x15:
      for (uint32_t i = 1; i + 1 < n; i++) tri(0, i, i + 1);
      break;
    case 0x13:
      for (uint32_t q = 0; q + 4 <= n; q += 4) tri(q, q + 1, q + 2), tri(q, q + 2, q + 3);
      break;
    case 0x14:
      for (uint32_t q = 0; q + 4 <= n; q += 2) tri(q, q + 1, q + 3), tri(q, q + 3, q + 2);
      break;
    case 0x12:
      for (uint32_t i = 0; i + 1 < n; i++) v.insert(v.end(), {i, i + 1});
      if (n >= 2) v.insert(v.end(), {n - 1, 0});
      break;
  }
  return v;
}

}  // namespace

void Draw(const Regs& r, const DrawCall& call) {
  State& s = G();
  std::lock_guard<std::mutex> l(s.mu);
  if (!s.dev || !call.count) return;
  const uint32_t* cx = r.context;

  // Targets: colour targets 0 on while they have a format (none while
  // CB_COLOR_CONTROL's MODE is CB_DISABLE), the depth.
  gfx::ProgramDraw d;
  int ntargets = 0;
  uint32_t fw = 0, fh = 0;  // the largest target's size
  // CB_COLOR_CONTROL's MODE: 0 disabled, 1 normal, 2 eliminating fast
  // clears (CmaskDraw: nothing else is drawn); the others (resolving,
  // decompressing) and targets with FMASK compression are not drawn.
  const uint32_t cb_mode = (cx[kCbColorControl] >> 4) & 7;
  if (cb_mode > 2) FatalF("GPU: CB_COLOR_CONTROL mode %u is not drawn yet", cb_mode);
  for (int i = 0; i < 4 && cb_mode; i++) {
    const uint32_t* cb = cx + kCbColor0Base + 0xF * i;
    if (((cb[4] >> 2) & 0x1F) == 0) break;
    if (cb[4] & (1u << 14))
      FatalF("GPU: colour target %d with compression (CB_COLOR_INFO %08X) is not drawn yet", i,
             cb[4]);
    Format fmt;
    if (!ColorFormat(cb[4], &fmt)) {
      FatalF("GPU: colour target format %u, number type %u is not drawn yet", (cb[4] >> 2) & 0x1F,
             (cb[4] >> 8) & 7);
    }
    const uint32_t pitch = ((cb[1] & 0x7FF) + 1) * 8;
    const uint32_t height = uint32_t((uint64_t(cb[2] & 0x3FFFFF) + 1) * 64 / pitch);
    // COMP_SWAP: a target of four components keeps the shader's order
    // (TargetSelects), one of fewer memory order (the pixel shader's
    // export, ExportOrder).
    const uint32_t swap = (cb[4] >> 11) & 3, cf = (cb[4] >> 2) & 0x1F;
    const bool four = cf == 9 || cf == 10 || cf == 12 || cf == 14;
    if (swap && cf == 7)
      FatalF("GPU: a colour target's component swap is not drawn yet (three components)");
    d.targets[i] = {ColorTarget(s, uint64_t(cb[0]) << 8, pitch, height, fmt, four ? swap : 0), 0};
    fw = std::max(fw, pitch);
    fh = std::max(fh, height);
    if (cb[4] & (1u << 13)) CmaskDraw(s, d.targets[i].tex, cb, cb_mode == 2);
    else if (cb_mode == 2) FatalF("GPU: a fast clear eliminated without FAST_CLEAR is not drawn yet");
    ntargets++;
  }
  if (cb_mode == 2) return;
  const uint32_t zfmt = cx[kDbZInfo] & 3;
  const bool stencil = cx[kDbStencilInfo] & 1;
  if (zfmt) {
    const uint32_t size = cx[kDbDepthSize];
    const uint32_t dw = ((size & 0x7FF) + 1) * 8, dh = (((size >> 11) & 0x7FF) + 1) * 8;
    const Format f = zfmt == 1 ? Format::D16 : stencil ? Format::D32FS8 : Format::D32;
    // DB_DEPTH_VIEW: the array slices drawn, SLICE_START to SLICE_MAX.
    const uint32_t first = cx[kDbDepthView] & 0x7FF;
    const uint32_t last = std::max((cx[kDbDepthView] >> 13) & 0x7FF, first);
    DepthTarget* dt = DepthTargetAt(s, uint64_t(cx[kDbZReadBase]) << 8, dw, dh, f,
                                    stencil ? uint64_t(cx[kDbStencilReadBase]) << 8 : 0,
                                    last ? last + 1 : 0);
    d.depth = dt->depth;
    fw = std::max(fw, dw);
    fh = std::max(fh, dh);
    MetaCheck(dt->htile);
    if (dt->htile.gpu) {
      dt->htile.gpu = false;
      HtileClear(s, *dt, AsFloat(cx[kDbDepthClear]));
    }
    dt->htile.base = (cx[kDbZInfo] & (1u << 29)) ? uint64_t(cx[kDbHtileDataBase]) << 8 : 0;
    // TILE_STENCIL_DISABLE off: it tracks stencil too.
    dt->htile.mask = stencil && !(cx[kDbStencilInfo] & (1u << 29)) ? 0 : 0xF;
    if (dt->htile.clear) {
      dt->htile.clear = false;
      for (uint32_t i = 0; i < std::max(dt->layers, 1u); i++)
        s.dev->ClearDepth(d.depth, AsFloat(cx[kDbDepthClear]), -1, int(i));
    }
    // A depth (and stencil) clear: the draw writes the clear values.
    if (cx[kDbRenderControl] & 3) {
      for (uint32_t i = first; i <= last; i++)
        s.dev->ClearDepth(d.depth, AsFloat(cx[kDbDepthClear]),
                          (cx[kDbRenderControl] & 2) ? int(cx[kDbStencilClear] & 0xFF) : -1, int(i));
      return;
    }
    if (last != first) FatalF("GPU: a draw into several depth slices is not drawn yet");
    d.depth_layer = dt->layers ? int(first) : 0;
  }
  if (!ntargets && !d.depth) return;
  // Occlusion counts (DB_COUNT_CONTROL: ZPASS_INCREMENT_DISABLE clear,
  // ZPASS_ENABLE set): the pixels the draw writes, one more target after
  // the colour targets. Counts are exact whatever PERFECT_ZPASS_COUNTS
  // asks (a count of at least one when any pixel passes is what it
  // promises without it).
  const uint32_t cc = cx[kDbCountControl];
  const bool counted = !(cc & 1) && ((cc >> 8) & 0xF);
  if (counted) {
    if (ntargets == 4) FatalF("GPU: an occlusion-counted draw into four colour targets is not drawn yet");
    State::CountTarget& ct = CountTargetFor(s, fw, fh);
    d.targets[ntargets] = {ct.tex, 0};
    ct.dirty = true;
  }

  // Shaders.
  const bool gs = cx[kVgtShaderStagesEn] & 0x3F;
  const uint32_t prim = r.uconfig[kPrimitiveType];
  StageState vs = ShaderState(r, Stage::Vertex, kPgmLoVs), ps = PixelState(r);
  ps.count = counted ? ntargets : -1;
  vs.rects = !gs && prim == 0x11;
  Resolved rv = Resolve(vs);
  TargetSelects(s, rv);
  if (!rv.error.empty()) {
    FatalF("GPU: a vertex shader cannot run yet: %s", rv.error.c_str());
  }
  Geometry geo;
  gfx::Topology gs_topology = gfx::Topology::TriangleList;
  if (gs && !SetUpGeometry(s, r, call, rv, &geo, &gs_topology)) return;
  Resolved rp;
  if (ps.code) {
    rp = Resolve(ps);
    TargetSelects(s, rp);
    if (!rp.error.empty()) {
      FatalF("GPU: a pixel shader cannot run yet: %s", rp.error.c_str());
    }
  }
  const std::string key = rv.key + '|' + rp.key + (counted ? "|c" + std::to_string(ntargets) : "");
  Program& pr = s.programs[key];
  if (!pr.id && pr.error.empty()) {
    pr.vs = Translate(vs, rv);
    if (ps.code) pr.ps = Translate(ps, rp);
    else pr.ps.stage = PixelStub(ps.count);
    pr.error = !pr.vs.error.empty() ? pr.vs.error : pr.ps.error;
    if (pr.error.empty()) {
      gfx::ProgramDesc pd;
      pd.vs = pr.vs.stage;
      pd.fs = pr.ps.stage;
      pr.id = s.dev->CreateProgram(pd);
    } else {
      FatalF("GPU: a draw's shader is not translated yet: %s", pr.error.c_str());
    }
  }
  if (!pr.id) return;

  // Fixed state.
  gfx::PipelineState& ps_ = d.state;
  switch (gs ? ~0u : prim) {
    case ~0u: ps_.topology = gs_topology; break;
    case 1: ps_.topology = gfx::Topology::PointList; break;
    case 2: case 0x12: ps_.topology = gfx::Topology::LineList; break;
    case 3: ps_.topology = gfx::Topology::LineStrip; break;
    case 4: case 5: case 0x11: case 0x13: case 0x14: case 0x15:
      ps_.topology = gfx::Topology::TriangleList;
      break;
    case 6: ps_.topology = gfx::Topology::TriangleStrip; break;
    default: FatalF("GPU: primitive type %02X is not drawn yet", prim);
  }
  const uint32_t mode = cx[kPaSuScModeCntl];
  ps_.cull = (mode & 3) == 3 ? gfx::CullMode::None
             : (mode & 1)    ? gfx::CullMode::Front
             : (mode & 2)    ? gfx::CullMode::Back
                             : gfx::CullMode::None;
  if ((mode & 3) == 3) return;  // both faces culled
  ps_.front_ccw = !(mode & 4);
  // Polygon offset (PA_SU_POLY_OFFSET_*: DB_FMT_CNTL, CLAMP, then scale
  // and offset of front and back faces): front's, or back's when only back
  // faces have it. The scale is in 1/16ths of the slope; the offset in
  // units of 2^NEG_NUM_DB_BITS (of the exponent's for float depth), the
  // device's of the depth format's own bits (16; 23 of mantissa).
  if (mode & 0x1800) {
    const uint32_t* po = cx + kPolyOffsetDbFmt;
    const uint32_t face = (mode & 0x800) ? 0 : 2;
    const int bits = int(int8_t(po[0] & 0xFF)) + (zfmt == 1 ? 16 : 23);
    ps_.depth_bias_slope = AsFloat(po[2 + face]) / 16;
    ps_.depth_bias = std::ldexp(AsFloat(po[3 + face]), bits);
    ps_.depth_bias_clamp = AsFloat(po[1]);
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
    int from[4];
    ExportOrder(ps.col_swap, uint32_t(i), from);
    b.write_mask = 0;
    for (int c = 0; c < 4; c++) b.write_mask |= ((mask >> (4 * i + from[c])) & 1) << c;
    if (!(pr.ps.mrt_mask & (1u << i))) b.write_mask = 0;
    if (b.enable && from[0] == 3 && (bc & (1u << 29)))
      FatalF("GPU: blending into an alpha-only target is not drawn yet");
  }
  if (counted) {
    gfx::TargetBlend& b = ps_.blend[ntargets];
    b = {};
    b.enable = true;
    b.dst_color = b.dst_alpha = gfx::BlendFactor::One;
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
  Bind(s, vs, rv, bv, gs ? &geo.rings : nullptr);
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
  std::vector<uint32_t> expanded;
  if (prim == 0x11) {
    if (call.indices) FatalF("GPU: indexed rectangle lists are not drawn yet");
    d.vertex_count = call.count / 3 * 6;
  } else if (prim == 5 || prim >= 0x12) {
    expanded = Expand(prim, call.count);
    if (call.indices) {
      if (!Readable(call.indices, uint64_t(call.count) * call.index_size)) return;
      for (uint32_t& i : expanded)
        i = call.index_size == 4 ? Rd32(call.indices + 4 * uint64_t(i))
                                 : Rd16(call.indices + 2 * uint64_t(i));
    }
    d.vertex_count = uint32_t(expanded.size());
    d.indices = expanded.data();
    d.index_size = 4;
  } else if (call.indices) {
    // Kept like large ranges: a scene's draws use a few hundred index
    // ranges, each over and over.
    const uint32_t bytes = call.count * call.index_size;
    gaddr at;
    KeptRange* k;
    if ((k = Kept(s, call.indices, bytes, &at)) && !((call.indices - at) % call.index_size)) {
      d.index_buffer = k->buffer;
      d.first_index = uint32_t((call.indices - at) / call.index_size);
    } else {
      if (!Readable(call.indices, bytes)) return;
      d.indices = GPtr(call.indices);
    }
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
  TargetSelects(s, rc);
  if (!rc.error.empty()) {
    FatalF("GPU: a compute shader cannot run yet: %s", rc.error.c_str());
  }
  Program& pr = s.compute[rc.key];
  if (!pr.id && pr.error.empty()) {
    pr.vs = Translate(cs, rc);
    pr.error = pr.vs.error;
    if (pr.error.empty()) {
      gfx::ProgramDesc pd;
      pd.cs = pr.vs.stage;
      for (int i = 0; i < 3; i++) pd.local_size[i] = pr.vs.local_size[i];
      pr.id = s.dev->CreateProgram(pd);
    } else {
      FatalF("GPU: a compute shader is not translated yet: %s", pr.error.c_str());
    }
  }
  if (!pr.id) return;
  Bound b;
  Bind(s, cs, rc, b);
  for (const gfx::ColorTarget& img : b.images)
    if (!img.tex) return;
  // The fault word (Resolved::faults): the address of an instruction that
  // loaded outside its range or found a descriptor not resolved, or the
  // stage's for host subgroups unlike its waves,
  // checked once the dispatch has landed.
  uint32_t* fault = rc.faults ? new uint32_t(0) : nullptr;
  if (fault) b.rw.push_back({fault, 4});
  gfx::ProgramDispatch d;
  d.program = pr.id;
  d.cs = b.res;
  d.rw_buffers = b.rw.data();
  d.rw_images = b.images.data();
  d.land_later = true;
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
  bool host = false;
  for (const gfx::RwBufferData& x : b.rw) host |= x.data != nullptr;
  if (host) {
    Landings& l = L();
    std::lock_guard<std::mutex> lk(l.mu);
    for (const BufferRange& br : rc.buffers)
      if (br.written) l.ranges.push_back({br.base, br.size, s.dev->WritesQueued()});
    l.pending = l.ranges.size() + l.signals.size();
  }
  if (fault)
    Signal(0, 0, [fault] {
      const uint32_t at = *fault;
      delete fault;
      if (at)
        FatalF("GPU: a compute shader loaded outside the range resolved for a run-time offset, found "
               "a descriptor not resolved there at %08X (address's low half), "
               "or ran in host subgroups unlike its waves (the stage's address)", at);
    });
  for (const BufferRange& br : rc.buffers)
    if (br.written) {
      if (std::find(b.metas.begin(), b.metas.end(), br.base) == b.metas.end())
        MetaWritten(s, br.base, br.size);
      else
        MetaGpuWritten(s, br.base, br.size);
      KeptWritten(s, br.base, br.size);
    }
}

void MemoryWritten(gaddr base, uint64_t size) {
  std::lock_guard<std::mutex> l(G().mu);
  MetaWritten(G(), base, size);
  KeptWritten(G(), base, size);
}

void NextSubmission() {
  std::lock_guard<std::mutex> l(G().mu);
  State& s = G();
  s.submission++;
  if (!s.dev) return;
  // The metadata writes applied on the GPU, landed by now: only to stop
  // at those that were no full clears.
  ForEachMeta(s, [](Meta& m) {
    if (!m.verify || m.gpu) return;
    m.verify = false;
    MetaCleared(m, m.written, m.written_size);
    m.written_size = 0;
  });
  for (gfx::BufferId b : s.retired) s.dev->DestroyBuffer(b);
  s.retired.clear();
  for (auto it = s.kept.begin(); it != s.kept.end();) {
    if (s.submission - it->second.used < kKeptIdle) {
      ++it;
      continue;
    }
    s.dev->DestroyBuffer(it->second.buffer);
    it = s.kept.erase(it);
  }
}

}  // namespace recomp::orbis::gpu
