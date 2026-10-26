// libSceFont and libSceFontFt HLE on the host's FreeType. What the game
// sees is libSceFont 11.00's: the memory, surface and output structures,
// the argument checks and codes, and the objects it allocates through the
// game's memory interface (library and font 0x100 bytes, renderer 0x168,
// and the 0x4000-byte caches of the first two). Glyphs go the way
// libSceFontFt takes them through FreeType: outlines loaded unscaled and
// unhinted, scaled in its fixed point, rasterised by FreeType's
// anti-aliasing rasteriser at 1/64-pixel positions; kerning from the
// 'kern' table or GPOS. Its FreeType allocations through the game's
// memory are not made (docs/hle.md).

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "orbis/hle.h"
#include "orbis/process.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace {

using recomp::gaddr;

constexpr uint32_t kErrFatal = 0x80460001, kErrInvalidParameter = 0x80460002,
                   kErrInvalidMemory = 0x80460003, kErrInvalidLibrary = 0x80460004,
                   kErrInvalidFont = 0x80460005, kErrInvalidRenderer = 0x80460007,
                   kErrAllocationFailed = 0x80460010, kErrNoExternalFonts = 0x80460020,
                   kErrAlreadySpecified = 0x80460021, kErrNoSupportCode = 0x80460041,
                   kErrNoSupportGlyph = 0x80460042, kErrAlreadyBound = 0x80460060,
                   kErrNotBound = 0x80460061, kErrRenderFailed = 0x80460065;
constexpr uint16_t kTagMemory = 0xF00, kTagLibrary = 0xF01, kTagFont = 0xF02,
                   kTagRenderer = 0xF07;

// SceFontMemory: tag, region size, region, the interface's object, the
// interface {malloc(obj, size), free(obj, p), ...}, a destroy callback
// and its object.
gaddr MemObject(gaddr mem) { return recomp::Rd64(mem + 0x10); }
gaddr MemInterface(gaddr mem) { return recomp::Rd64(mem + 0x18); }

bool ValidMemory(gaddr mem) {
  if (!mem || recomp::Rd16(mem) != kTagMemory) return false;
  const gaddr mi = MemInterface(mem);
  return mi && recomp::Rd64(mi) && recomp::Rd64(mi + 8);
}

gaddr Alloc(recomp::Ctx& c, gaddr mem, uint64_t size) {
  return recomp::CallGuest(c, recomp::Rd64(MemInterface(mem)), {MemObject(mem), size});
}
void Free(recomp::Ctx& c, gaddr mem, gaddr p) {
  recomp::CallGuest(c, recomp::Rd64(MemInterface(mem) + 8), {MemObject(mem), p});
}

struct Library {
  gaddr mem, cache;
  bool system_fonts = false, external_fonts = false;
};
struct Renderer {
  gaddr mem, buffer;
  int32_t origin_row;  // the raster's row 0 on the surface, below
};
struct Font {
  gaddr library;
  FT_Face face = nullptr;
  float scale_w = 0, scale_h = 0, render_w = 0, render_h = 0;
  gaddr renderer = 0;
  std::vector<uint8_t> gpos;
  uint32_t kern = 0;  // GPOS's 'kern' feature (0: none), below
};

std::mutex g_mu;
FT_Library g_ft = nullptr;
std::map<gaddr, Library> g_libraries;
std::map<gaddr, Renderer> g_renderers;
std::map<gaddr, Font> g_fonts;

Font* FindFont(gaddr f) {
  if (!f || recomp::Rd16(f) != kTagFont) return nullptr;
  auto it = g_fonts.find(f);
  return it == g_fonts.end() ? nullptr : &it->second;
}

// libSceFontFt's glyph size: 26.6 truncated from the float, at 72 dpi.
bool SetSize(FT_Face face, float w, float h) {
  return !FT_Set_Char_Size(face, FT_F26Dot6(uint32_t(int64_t(w * 64.0f))),
                           FT_F26Dot6(uint32_t(int64_t(h * 64.0f))), 72, 72);
}

// The glyph of code into the face's slot, as libSceFontFt loads it: in
// font units, unhinted, its outline (never a bitmap) and metrics.
uint32_t LoadGlyph(FT_Face face, uint32_t code) {
  const FT_UInt g = FT_Get_Char_Index(face, code);
  if (!g) return kErrNoSupportGlyph;
  if (const FT_Error e = FT_Load_Glyph(face, g, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING |
                                                     FT_LOAD_NO_BITMAP | FT_LOAD_VERTICAL_LAYOUT))
    return e == FT_Err_Out_Of_Memory ? kErrAllocationFailed : kErrNoSupportGlyph;
  return 0;
}

// The loaded glyph's metrics at sx, sy (16.16 26.6-pixels per unit), each
// edge rounded on its own: width, height, then horizontal bearing x, y
// and advance, then vertical ones (SceFontGlyphMetrics). Without vertical
// metrics in the font the vertical bearings centre the glyph.
void ScaleMetrics(FT_Face face, FT_Fixed sx, FT_Fixed sy, float out[8]) {
  const FT_Glyph_Metrics& m = face->glyph->metrics;
  const float k = 1.0f / 64.0f;
  std::fill(out, out + 8, 0.0f);
  const FT_Pos hadv = FT_MulFix(m.horiAdvance, sx), vadv = FT_MulFix(m.vertAdvance, sy);
  out[4] = float(hadv) * k;
  out[7] = float(vadv) * k;
  if (!m.width && !m.height) return;
  const FT_Pos left = FT_MulFix(m.horiBearingX, sx), top = FT_MulFix(m.horiBearingY, sy);
  const FT_Pos height = top - FT_MulFix(m.horiBearingY - m.height, sy);
  out[0] = float(FT_MulFix(m.horiBearingX + m.width, sx) - left) * k;
  out[1] = float(height) * k;
  out[2] = float(left) * k;
  out[3] = float(top) * k;
  if (FT_HAS_VERTICAL(face)) {
    out[5] = float(FT_MulFix(m.vertBearingX, sx)) * k;
    out[6] = float(FT_MulFix(m.vertBearingY, sy)) * k;
  } else {
    out[5] = float(left - hadv / 2) * k;
    out[6] = float((vadv - height) / 2) * k;
  }
}

// What the rasteriser drew: each span keeps the larger coverage per pixel,
// raster row y on surface row origin_row - 1 - y; the box of the spans
// with coverage, in raster coordinates.
struct Spans {
  gaddr origin;
  int32_t pitch;
  FT_Pos x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
};
void DrawSpans(int y, int count, const FT_Span* spans, void* user) {
  Spans& u = *static_cast<Spans*>(user);
  const FT_Span *first = nullptr, *last = nullptr;
  for (const FT_Span* sp = spans; sp < spans + count; sp++) {
    if (!sp->coverage) continue;
    if (!first) first = sp;
    if (!sp->len) continue;
    last = sp;
    uint8_t* p = recomp::GPtr(u.origin + int64_t(sp->x) - int64_t(y) * u.pitch);
    for (int i = 0; i < sp->len; i++) p[i] = std::max(p[i], sp->coverage);
  }
  if (!first) return;
  u.y0 = std::min<FT_Pos>(u.y0, y);
  u.y1 = std::max<FT_Pos>(u.y1, y);
  u.x0 = std::min<FT_Pos>(u.x0, first->x);
  if (last && u.x1 + 1 < last->x + FT_Pos(last->len)) u.x1 = last->x + FT_Pos(last->len) - 1;
}

// libSceFontFt's renderer on the loaded glyph: its outline at sx, sy
// (16.16) moved to the pen (x, y) truncated to 26.6, y up, rasterised with
// anti-aliasing into the surface's scissor. m gets the glyph's metrics at
// that scale (its width and left bearing from the outline's points), rect
// the pixels drawn (x, y, w, h on the surface; zeros if none).
uint32_t Render(FT_Face face, FT_Fixed sx, FT_Fixed sy, float x, float y, gaddr s,
                int32_t origin_row, float m[8], int32_t rect[4]) {
  ScaleMetrics(face, sx, sy, m);
  std::fill(rect, rect + 4, 0);
  const FT_Outline& o = face->glyph->outline;
  if (!o.n_contours || !o.n_points) return 0;
  std::vector<FT_Vector> points(size_t(o.n_points));
  const FT_Pos ox = FT_Pos(x * 64.0f), oy = FT_Pos(y * -64.0f);
  FT_Pos min = INT64_MAX, max = INT64_MIN;
  for (size_t i = 0; i < points.size(); i++) {
    const FT_Pos px = FT_MulFix(o.points[i].x, sx);
    min = std::min(min, px);
    max = std::max(max, px);
    points[i] = {px + ox, FT_MulFix(o.points[i].y, sy) + oy};
  }
  m[0] = float(max - min) * (1.0f / 64.0f);
  m[2] = float(min) * (1.0f / 64.0f);
  FT_Outline copy = o;
  copy.points = points.data();
  copy.flags &= ~FT_OUTLINE_OWNER;
  const int32_t pitch = int32_t(recomp::Rd32(s + 8));
  Spans u{recomp::Rd64(s) + (pitch >= 0 ? int64_t(origin_row - 1) * pitch : 0), pitch};
  FT_Raster_Params p{};
  p.source = &copy;
  p.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
  p.gray_spans = DrawSpans;
  p.user = &u;
  p.clip_box = {FT_Pos(recomp::Rd32(s + 0x18)), origin_row - FT_Pos(recomp::Rd32(s + 0x24)),
                FT_Pos(recomp::Rd32(s + 0x20)), origin_row - FT_Pos(recomp::Rd32(s + 0x1C))};
  if (FT_Outline_Render(g_ft, &copy, &p)) return kErrRenderFailed;
  if (u.x0 > u.x1) return 0;
  rect[0] = int32_t(u.x0);
  rect[1] = int32_t(origin_row - 1 - u.y1);
  rect[2] = int32_t(u.x1 - u.x0 + 1);
  rect[3] = int32_t(u.y1 - u.y0 + 1);
  return 0;
}

// A big-endian table (GPOS) read with bounds: zeros past its end.
struct Table {
  const std::vector<uint8_t>& t;
  uint32_t U16(uint32_t o) const { return o + 2 <= t.size() ? uint32_t(t[o]) << 8 | t[o + 1] : 0; }
  uint32_t U32(uint32_t o) const { return U16(o) << 16 | U16(o + 2); }
};
constexpr uint32_t kTagKern = 0x6B65726E, kTagLatn = 0x6C61746E;

// The 'kern' feature of the 'latn' script's default language system (or
// its first): the language's required feature if that is 'kern', else its
// first 'kern'. 0 if none.
uint32_t KernFeature(const Table& t) {
  const uint32_t scripts = t.U16(4), features = t.U16(6), nf = t.U16(features);
  for (uint32_t i = 0, n = t.U16(scripts); i < n; i++) {
    const uint32_t r = scripts + 2 + 6 * i;
    if (t.U32(r) != kTagLatn) continue;
    const uint32_t script = scripts + t.U16(r + 4);
    const uint32_t lang = t.U16(script)       ? script + t.U16(script)
                          : t.U16(script + 2) ? script + t.U16(script + 8)
                                              : 0;
    if (!lang || !nf) return 0;
    const auto tag = [&](uint32_t k) { return t.U32(features + 2 + 6 * k); };
    const auto at = [&](uint32_t k) { return features + t.U16(features + 6 + 6 * k); };
    const uint32_t req = t.U16(lang + 2);
    if (req != 0xFFFF && req < nf && tag(req) == kTagKern) return at(req);
    for (uint32_t j = 0, m = t.U16(lang + 4); j < m; j++) {
      const uint32_t k = t.U16(lang + 6 + 2 * j);
      if (k < nf && tag(k) == kTagKern) return at(k);
    }
    return 0;
  }
  return 0;
}

// Coverage index of g (-1: not covered) and class of g.
int32_t Coverage(const Table& t, uint32_t c, uint32_t g) {
  if (!c) return -1;
  const uint32_t n = t.U16(c + 2);
  int32_t lo = 0, hi = int32_t(n) - 1;
  if (t.U16(c) == 1) {
    while (lo <= hi) {
      const int32_t mid = (lo + hi) / 2, v = int32_t(t.U16(c + 4 + 2 * mid));
      if (v == int32_t(g)) return mid;
      if (v < int32_t(g)) lo = mid + 1; else hi = mid - 1;
    }
  } else if (t.U16(c) == 2) {
    while (lo <= hi) {
      const int32_t mid = (lo + hi) / 2;
      const uint32_t r = c + 4 + 6 * uint32_t(mid);
      if (g < t.U16(r)) hi = mid - 1;
      else if (g > t.U16(r + 2)) lo = mid + 1;
      else return int32_t(g - t.U16(r) + t.U16(r + 4));
    }
  }
  return -1;
}
uint32_t Class(const Table& t, uint32_t c, uint32_t g) {
  if (!c) return 0;
  if (t.U16(c) == 1) {
    const uint32_t start = t.U16(c + 2);
    return g >= start && g - start < t.U16(c + 4) ? t.U16(c + 6 + 2 * (g - start)) : 0;
  }
  if (t.U16(c) != 2) return 0;
  int32_t lo = 0, hi = int32_t(t.U16(c + 2)) - 1;
  while (lo <= hi) {
    const int32_t mid = (lo + hi) / 2;
    const uint32_t r = c + 4 + 6 * uint32_t(mid);
    if (g < t.U16(r)) hi = mid - 1;
    else if (g > t.U16(r + 2)) lo = mid + 1;
    else return t.U16(r + 4);
  }
  return 0;
}

// A pair's value records (format vf: the first's in its low 16 bits) at
// o: x and y placement and advance of each; device tables skipped.
void Values(const Table& t, uint32_t o, uint32_t vf, int16_t v1[4], int16_t v2[4]) {
  for (int r = 0; r < 2; r++, vf >>= 16) {
    int16_t* v = r ? v2 : v1;
    for (int i = 0; i < 4; i++) {
      v[i] = vf & (1u << i) ? int16_t(t.U16(o)) : 0;
      if (vf & (1u << i)) o += 2;
    }
    o += 2 * uint32_t(std::popcount(vf & 0xF0));
  }
}

// GPOS pair adjustment of (g1, g2) by the feature's lookups in order (pair
// adjustment lookups, also through extensions): the first subtable
// covering the pair gives it.
bool PairAdjust(const Table& t, uint32_t feature, uint32_t g1, uint32_t g2, int16_t v1[4],
                int16_t v2[4]) {
  const uint32_t lookups = t.U16(8);
  for (uint32_t i = 0, n = t.U16(feature + 2); i < n; i++) {
    const uint32_t index = t.U16(feature + 4 + 2 * i);
    if (index >= t.U16(lookups) || !t.U16(lookups + 2 + 2 * index)) return false;
    const uint32_t lookup = lookups + t.U16(lookups + 2 + 2 * index), type = t.U16(lookup);
    uint32_t ext_type = 0;  // an extension lookup's, from its first subtable
    for (uint32_t j = 0, ns = t.U16(lookup + 4); j < ns; j++) {
      if (!t.U16(lookup + 6 + 2 * j)) break;
      uint32_t sub = lookup + t.U16(lookup + 6 + 2 * j), st = type;
      if (type == 9) {
        bool ok = true;
        while (st == 9 && (ok = t.U16(sub) == 1)) {
          st = t.U16(sub + 2);
          sub += t.U32(sub + 4);
        }
        if (!ok) continue;
        if (!ext_type) ext_type = st;
        else if (ext_type != 2) continue;
      }
      if (st != 2) continue;
      const uint32_t format = t.U16(sub);
      const int32_t ci = format == 1 || format == 2 ? Coverage(t, t.U16(sub + 2) ? sub + t.U16(sub + 2) : 0, g1) : -1;
      if (ci < 0) continue;
      const uint32_t vf = t.U16(sub + 4) | t.U16(sub + 6) << 16;
      const uint32_t size = 2 * uint32_t(std::popcount(vf));
      if (format == 2) {
        const uint32_t c2n = t.U16(sub + 14);
        const uint32_t c2 = Class(t, t.U16(sub + 10) ? sub + t.U16(sub + 10) : 0, g2);
        if (c2 >= c2n) continue;
        const uint32_t c1 = Class(t, t.U16(sub + 8) ? sub + t.U16(sub + 8) : 0, g1);
        if (c1 >= t.U16(sub + 12)) continue;
        Values(t, sub + 16 + (c1 * c2n + c2) * size, vf, v1, v2);
        return true;
      }
      if (uint32_t(ci) >= t.U16(sub + 8)) continue;
      const uint32_t set = sub + t.U16(sub + 10 + 2 * uint32_t(ci));
      for (uint32_t k = 0, o = set + 4, np = t.U16(set); k < np; k++, o += size + 2)
        if (t.U16(o - 2) == g2) {
          Values(t, o, vf, v1, v2);
          return true;
        }
    }
  }
  return false;
}

void PutMetrics(gaddr p, const float m[8]) {
  for (int i = 0; i < 8; i++) {
    uint32_t v;
    std::memcpy(&v, &m[i], 4);
    recomp::Wr32(p + 4 * uint64_t(i), v);
  }
}
void PutF(gaddr p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  recomp::Wr32(p, v);
}

}  // namespace

using recomp::hle::Arg;
using recomp::hle::ArgF;

// int sceFontMemoryInit(SceFontMemory* mem, void* region, uint32_t size,
// const SceFontMemoryInterface* mi, void* object, destroy callback,
// destroy object): a region, or an interface.
ORBIS_HLE("libSceFont", sceFontMemoryInit, 7) {
  const gaddr mem = Arg(c, 0), region = Arg(c, 1), mi = Arg(c, 3);
  const uint32_t size = uint32_t(Arg(c, 2));
  if (!mem) return kErrInvalidParameter;
  if (!mi) {
    recomp::Wr16(mem, 0);
    if (!region || !size) return kErrInvalidParameter;
    recomp::FatalF("sceFontMemoryInit: a memory region without an interface not implemented yet");
  }
  recomp::Wr32(mem, kTagMemory);
  recomp::Wr32(mem + 4, size);
  recomp::Wr64(mem + 8, region);
  recomp::Wr64(mem + 0x10, Arg(c, 4));
  recomp::Wr64(mem + 0x18, mi);
  recomp::Wr64(mem + 0x20, Arg(c, 5));
  recomp::Wr64(mem + 0x28, Arg(c, 6));
  recomp::Wr64(mem + 0x30, 0);
  recomp::Wr64(mem + 0x38, 0);
  return 0;
}

// Runs the destroy callback (mem, object, destroy object) and clears the
// memory's first 32 bytes.
ORBIS_HLE("libSceFont", sceFontMemoryTerm, 1) {
  const gaddr mem = Arg(c, 0);
  if (!mem) return kErrInvalidParameter;
  if (recomp::Rd16(mem) != kTagMemory) return kErrInvalidMemory;
  if (int8_t(recomp::GPtr(mem)[2]) < 0)
    recomp::FatalF("sceFontMemoryTerm: an mspace memory not implemented yet");
  if (const gaddr cb = recomp::Rd64(mem + 0x20)) {
    recomp::Wr32(mem, 0);
    recomp::CallGuest(c, cb, {mem, recomp::Rd64(mem + 0x10), recomp::Rd64(mem + 0x28)});
  }
  std::memset(recomp::GPtr(mem), 0, 32);
  return 0;
}

// The FreeType library and renderer selections: blocks only this HLE
// reads.
static gaddr Selection(int which) {
  static gaddr blocks[2] = {};
  if (!blocks[which]) {
    blocks[which] = recomp::g_proc->vm.Reserve(0, recomp::GuestMemory::kPageSize);
    if (!blocks[which] ||
        !recomp::g_proc->vm.Commit(blocks[which], recomp::GuestMemory::kPageSize, recomp::Prot::RW))
      recomp::FatalF("libSceFontFt: out of guest memory");
  }
  return blocks[which];
}
ORBIS_HLE("libSceFontFt", sceFontSelectLibraryFt, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return int(Arg(c, 0)) == 0 ? Selection(0) : 0;
}
ORBIS_HLE("libSceFontFt", sceFontSelectRendererFt, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return int(Arg(c, 0)) == 0 ? Selection(1) : 0;
}

// int sceFontCreateLibraryWithEdition(const SceFontMemory* mem, const
// SceFontLibrarySelection* sel, uint64_t edition, SceFontLibrary* out)
ORBIS_HLE("libSceFont", sceFontCreateLibraryWithEdition, 4) {
  const gaddr mem = Arg(c, 0), sel = Arg(c, 1), out = Arg(c, 3);
  if (out) recomp::Wr64(out, 0);
  if (!mem) return kErrInvalidParameter;
  if (!ValidMemory(mem)) return kErrInvalidMemory;
  if (!sel || !out) return kErrInvalidParameter;
  if (sel != Selection(0)) recomp::FatalF("sceFontCreateLibraryWithEdition: not the FreeType library");
  const gaddr lib = Alloc(c, mem, 0x100);
  if (!lib) return kErrAllocationFailed;
  const gaddr cache = Alloc(c, mem, 0x4000);
  if (!cache) {
    Free(c, mem, lib);
    return kErrAllocationFailed;
  }
  std::memset(recomp::GPtr(lib), 0, 0x100);
  recomp::Wr16(lib, kTagLibrary);
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_ft && FT_Init_FreeType(&g_ft)) recomp::FatalF("FreeType does not start");
  g_libraries[lib] = {mem, cache};
  recomp::Wr64(out, lib);
  return 0;
}

static Library* FindLibrary(gaddr lib) {
  if (!lib || recomp::Rd16(lib) != kTagLibrary) return nullptr;
  auto it = g_libraries.find(lib);
  return it == g_libraries.end() ? nullptr : &it->second;
}

// Arguments are read before calling the game's memory functions, which
// reuse the registers.
ORBIS_HLE("libSceFont", sceFontDestroyLibrary, 1) {
  const gaddr h = Arg(c, 0);
  if (!h) return kErrInvalidParameter;
  const gaddr p = recomp::Rd64(h);
  Library lib;
  {
    std::lock_guard<std::mutex> l(g_mu);
    Library* found = FindLibrary(p);
    if (!found) return kErrInvalidLibrary;
    lib = *found;
    g_libraries.erase(p);
  }
  recomp::Wr16(p, 0);
  Free(c, lib.mem, lib.cache);
  Free(c, lib.mem, p);
  recomp::Wr64(h, 0);
  return 0;
}

ORBIS_HLE("libSceFont", sceFontSupportSystemFonts, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  Library* lib = FindLibrary(Arg(c, 0));
  if (!lib) return kErrInvalidLibrary;
  if (lib->system_fonts) return kErrAlreadySpecified;
  lib->system_fonts = true;
  return 0;
}

// int sceFontSupportExternalFonts(SceFontLibrary lib, uint32_t max_fonts,
// uint32_t formats)
ORBIS_HLE("libSceFont", sceFontSupportExternalFonts, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  Library* lib = FindLibrary(Arg(c, 0));
  if (!lib) return kErrInvalidLibrary;
  if (lib->external_fonts) return kErrAlreadySpecified;
  lib->external_fonts = true;
  return 0;
}

// The device cache (glyph cache memory the game lends the library):
// FreeType here keeps its own.
ORBIS_HLE("libSceFont", sceFontAttachDeviceCacheBuffer, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!FindLibrary(Arg(c, 0))) return kErrInvalidLibrary;
  return 0;
}

// int sceFontCreateRendererWithEdition(const SceFontMemory* mem, const
// SceFontRendererSelection* sel, uint64_t edition, SceFontRenderer* out)
ORBIS_HLE("libSceFont", sceFontCreateRendererWithEdition, 4) {
  const gaddr mem = Arg(c, 0), sel = Arg(c, 1), out = Arg(c, 3);
  const uint32_t edition = uint32_t(Arg(c, 2) >> 32);
  if (!mem) return kErrInvalidParameter;
  if (!ValidMemory(mem)) return kErrInvalidMemory;
  if (!sel || !out) return kErrInvalidParameter;
  if (sel != Selection(1)) recomp::FatalF("sceFontCreateRendererWithEdition: not the FreeType renderer");
  const gaddr r = Alloc(c, mem, 0x168), buffer = Alloc(c, mem, 0x4000);
  if (!r || !buffer) {
    if (buffer) Free(c, mem, buffer);
    if (r) Free(c, mem, r);
    return kErrAllocationFailed;
  }
  std::memset(recomp::GPtr(r), 0, 0x168);
  recomp::Wr16(r, kTagRenderer);
  // For SDK versions 0.93 up to 2.50 (the edition's and the game's, the
  // older) the raster's row 0 is the surface's: glyphs land a row lower
  // than for later ones.
  const gaddr pp = recomp::orbis::ProcParam();
  const uint32_t compiled = pp ? recomp::Rd32(pp + 0x10) : 0;
  const uint32_t sdk = compiled && compiled < edition ? compiled : edition;
  std::lock_guard<std::mutex> l(g_mu);
  g_renderers[r] = {mem, buffer, sdk - 0x930000u < 0x1BD0000u ? 1 : 0};
  recomp::Wr64(out, r);
  return 0;
}

ORBIS_HLE("libSceFont", sceFontDestroyRenderer, 1) {
  const gaddr h = Arg(c, 0);
  if (!h) return kErrInvalidParameter;
  const gaddr p = recomp::Rd64(h);
  Renderer r;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_renderers.find(p);
    if (!p || it == g_renderers.end()) return kErrInvalidRenderer;
    r = it->second;
    g_renderers.erase(it);
  }
  recomp::Wr16(p, 0);
  Free(c, r.mem, r.buffer);
  Free(c, r.mem, p);
  recomp::Wr64(h, 0);
  return 0;
}

// int sceFontOpenFontMemory(SceFontLibrary lib, const void* data, uint32_t
// size, const SceFontOpenDetail* detail, SceFontHandle* out): the font
// stays in the game's memory.
ORBIS_HLE("libSceFont", sceFontOpenFontMemory, 5) {
  const gaddr lib_h = Arg(c, 0), data = Arg(c, 1), out = Arg(c, 4);
  const uint32_t size = uint32_t(Arg(c, 2));
  gaddr mem;
  {
    std::lock_guard<std::mutex> l(g_mu);
    Library* lib = FindLibrary(lib_h);
    if (!lib) return kErrInvalidLibrary;
    if (!lib->external_fonts) return kErrNoExternalFonts;
    mem = lib->mem;
  }
  if (!data || !size || !out) return kErrInvalidParameter;
  if (Arg(c, 3)) recomp::FatalF("sceFontOpenFontMemory: open details not implemented yet");
  if (recomp::Rd64(out)) recomp::FatalF("sceFontOpenFontMemory: reopening into a handle not implemented yet");
  const gaddr f = Alloc(c, mem, 0x100);
  if (!f) return kErrAllocationFailed;
  std::memset(recomp::GPtr(f), 0, 0x100);
  std::lock_guard<std::mutex> l(g_mu);
  Font font{lib_h};
  if (FT_New_Memory_Face(g_ft, recomp::GPtr(data), FT_Long(size), 0, &font.face)) {
    Free(c, mem, f);
    return kErrFatal;
  }
  // GPOS for kerning, which libSceFontFt reads itself (FreeType does not).
  FT_ULong gpos_size = 0;
  if (!FT_Load_Sfnt_Table(font.face, TTAG_GPOS, 0, nullptr, &gpos_size) && gpos_size) {
    font.gpos.resize(gpos_size);
    if (FT_Load_Sfnt_Table(font.face, TTAG_GPOS, 0, font.gpos.data(), &gpos_size)) font.gpos.clear();
    else font.kern = KernFeature(Table{font.gpos});
  }
  recomp::Wr16(f, kTagFont);
  g_fonts[f] = font;
  recomp::Wr64(out, f);
  return 0;
}

ORBIS_HLE("libSceFont", sceFontCloseFont, 1) {
  gaddr mem;
  {
    std::lock_guard<std::mutex> l(g_mu);
    Font* f = FindFont(Arg(c, 0));
    if (!f) return kErrInvalidFont;
    FT_Done_Face(f->face);
    mem = g_libraries[f->library].mem;
    g_fonts.erase(Arg(c, 0));
  }
  recomp::Wr16(Arg(c, 0), 0);
  Free(c, mem, Arg(c, 0));
  return 0;
}

// int sceFontSetScalePixel(SceFontHandle f, float w, float h)
ORBIS_HLE("libSceFont", sceFontSetScalePixel, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  Font* f = FindFont(Arg(c, 0));
  if (!f) return kErrInvalidFont;
  f->scale_w = ArgF(c, 0);
  f->scale_h = ArgF(c, 1);
  return 0;
}

ORBIS_HLE("libSceFont", sceFontGetScalePixel, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  Font* f = FindFont(Arg(c, 0));
  if (!f) return kErrInvalidFont;
  if (Arg(c, 1)) PutF(Arg(c, 1), f->scale_w);
  if (Arg(c, 2)) PutF(Arg(c, 2), f->scale_h);
  return 0;
}

// The scale glyphs are rendered at (with the bound renderer).
ORBIS_HLE("libSceFont", sceFontSetupRenderScalePixel, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  Font* f = FindFont(Arg(c, 0));
  if (!f) return kErrInvalidFont;
  if (!f->renderer) return kErrNotBound;
  f->render_w = ArgF(c, 0);
  f->render_h = ArgF(c, 1);
  return 0;
}

ORBIS_HLE("libSceFont", sceFontBindRenderer, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  Font* f = FindFont(Arg(c, 0));
  if (!f) return kErrInvalidFont;
  if (f->renderer) return kErrAlreadyBound;
  const gaddr r = Arg(c, 1);
  if (!r || recomp::Rd16(r) != kTagRenderer || !g_renderers.count(r)) return kErrInvalidRenderer;
  f->renderer = r;
  return 0;
}

ORBIS_HLE("libSceFont", sceFontUnbindRenderer, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  Font* f = FindFont(Arg(c, 0));
  if (!f) return kErrInvalidFont;
  if (!f->renderer) return kErrNotBound;
  f->renderer = 0;
  return 0;
}

// int sceFontGetCharGlyphMetrics(SceFontHandle f, uint32_t code,
// SceFontGlyphMetrics* m): at the font's scale (FreeType's for it); zeros
// on an error.
ORBIS_HLE("libSceFont", sceFontGetCharGlyphMetrics, 3) {
  const gaddr out = Arg(c, 2);
  std::lock_guard<std::mutex> l(g_mu);
  Font* f = FindFont(Arg(c, 0));
  const uint32_t code = uint32_t(Arg(c, 1));
  uint64_t r = 0;
  float m[8] = {};
  if (!f) r = kErrInvalidFont;
  else if (!code) r = kErrNoSupportCode;
  else if (!out) return kErrInvalidParameter;
  else if (!SetSize(f->face, f->scale_w, f->scale_h)) r = kErrFatal;
  else if (!(r = LoadGlyph(f->face, code)))
    ScaleMetrics(f->face, f->face->size->metrics.x_scale, f->face->size->metrics.y_scale, m);
  if (out) PutMetrics(out, m);
  return r;
}

// int sceFontGetKerning(SceFontHandle f, uint32_t pre, uint32_t code,
// SceFontKerning* k): {dx, dy, position x, y} in pixels at the font's
// scale, zeros for no pre code. Codes of right-to-left scripts pair in
// the other order. Fonts with a 'kern' table kern by it (dx, dy), others
// by GPOS (the first's advance, the second's placement). A pre code
// without a glyph pairs glyph 0.
static bool RightToLeft(uint32_t code) {
  return (code >= 0x590 && code < 0x900) || (code >= 0xFB1D && code < 0xFE00) ||
         (code >= 0xFE70 && code < 0xFEFF) || (code & 0xFFFFF800) == 0x10800;
}
ORBIS_HLE("libSceFont", sceFontGetKerning, 4) {
  const gaddr out = Arg(c, 3);
  uint32_t pre = uint32_t(Arg(c, 1)), code = uint32_t(Arg(c, 2));
  std::lock_guard<std::mutex> l(g_mu);
  Font* f = FindFont(Arg(c, 0));
  if (!f || !code) {
    if (out) std::memset(recomp::GPtr(out), 0, 16);
    return f ? kErrNoSupportCode : kErrInvalidFont;
  }
  if (!out) return kErrInvalidParameter;
  std::memset(recomp::GPtr(out), 0, 16);
  if (!pre) return 0;
  if (RightToLeft(pre) || RightToLeft(code)) std::swap(pre, code);
  const FT_UInt g1 = FT_Get_Char_Index(f->face, pre), g2 = FT_Get_Char_Index(f->face, code);
  if (!g2) return kErrNoSupportGlyph;
  if (!SetSize(f->face, f->scale_w, f->scale_h)) return kErrFatal;
  const FT_Fixed sx = f->face->size->metrics.x_scale, sy = f->face->size->metrics.y_scale;
  if (FT_HAS_KERNING(f->face)) {
    FT_Vector k;
    if (FT_Get_Kerning(f->face, g1, g2, FT_KERNING_UNFITTED, &k)) return 0;
    PutF(out, float(k.x) / 64.0f);
    PutF(out + 4, float(k.y) / 64.0f);
    return 0;
  }
  int16_t v1[4], v2[4];
  if (!f->kern || !PairAdjust(Table{f->gpos}, f->kern, g1, g2, v1, v2)) return 0;
  PutF(out, float(FT_MulFix(v1[2], sx)) / 64.0f);
  PutF(out + 4, float(FT_MulFix(v1[3], sy)) / 64.0f);
  PutF(out + 8, float(FT_MulFix(v2[0], sx)) / 64.0f);
  PutF(out + 12, float(FT_MulFix(v2[1], sy)) / 64.0f);
  return 0;
}

// void sceFontRenderSurfaceInit(SceFontRenderSurface* s, void* buffer, int
// width_bytes, int pixel_bytes, int width, int height): the surface and
// a scissor of all of it.
ORBIS_HLE("libSceFont", sceFontRenderSurfaceInit, 6) {
  const gaddr s = Arg(c, 0);
  if (!s) return 0;
  const int32_t w = std::max(int32_t(Arg(c, 4)), 0), h = std::max(int32_t(Arg(c, 5)), 0);
  recomp::Wr64(s, Arg(c, 1));
  recomp::Wr32(s + 8, uint32_t(Arg(c, 2)));
  recomp::GPtr(s)[0xC] = uint8_t(Arg(c, 3));
  recomp::GPtr(s)[0xD] = 0;
  recomp::Wr16(s + 0xE, 0);
  recomp::Wr32(s + 0x10, uint32_t(w));
  recomp::Wr32(s + 0x14, uint32_t(h));
  recomp::Wr64(s + 0x18, 0);
  recomp::Wr32(s + 0x20, uint32_t(w));
  recomp::Wr32(s + 0x24, uint32_t(h));
  return 0;
}

// int sceFontRenderCharGlyphImageHorizontal(SceFontHandle f, uint32_t
// code, SceFontRenderSurface* s, float x, float y, SceFontGlyphMetrics* m,
// SceFontRenderOutput* out): the glyph at the render scale with its
// origin on the baseline at (x, y) of the surface (rows downward, 8-bit
// coverage), clipped to the scissor (Render above); m its metrics at that
// scale. out is {0, the updated rectangle's first byte, width bytes,
// pixel bytes, the updated rectangle, the image's offset from (x, y) (x,
// y), the advance (twice), the image's size}, rounded as libSceFont
// does.
ORBIS_HLE("libSceFont", sceFontRenderCharGlyphImageHorizontal, 5) {
  const gaddr fh = Arg(c, 0), s = Arg(c, 2), mo = Arg(c, 3), out = Arg(c, 4);
  const uint32_t code = uint32_t(Arg(c, 1));
  const float x = ArgF(c, 0), y = ArgF(c, 1);
  std::lock_guard<std::mutex> l(g_mu);
  Font* f = FindFont(fh);
  uint64_t err = 0;
  if (!f) err = kErrInvalidFont;
  else if (!code) err = kErrNoSupportCode;
  else if (!s || !mo || !out) err = kErrInvalidParameter;
  else if (!f->renderer) err = kErrNotBound;
  if (!err && recomp::GPtr(s)[0xC] != 1)
    recomp::FatalF("sceFontRenderCharGlyphImageHorizontal: %u-byte pixels not implemented yet",
                   recomp::GPtr(s)[0xC]);
  float m[8] = {};
  int32_t rect[4];
  if (!err && !SetSize(f->face, f->render_w, f->render_h)) err = kErrFatal;
  if (!err) err = LoadGlyph(f->face, code);
  if (!err) {
    const FT_Long upem = f->face->units_per_EM;
    err = Render(f->face, FT_DivFix(FT_Long(f->render_w * 64.0f), upem),
                 FT_DivFix(FT_Long(f->render_h * 64.0f), upem), x, y, s,
                 g_renderers[f->renderer].origin_row, m, rect);
  }
  if (err) {
    if (mo) std::memset(recomp::GPtr(mo), 0, 32);
    if (out) std::memset(recomp::GPtr(out), 0, 64);
    return err;
  }
  PutMetrics(mo, m);
  const gaddr buf = recomp::Rd64(s);
  const int32_t pitch = int32_t(recomp::Rd32(s + 8));
  recomp::Wr64(out, 0);
  recomp::Wr64(out + 8, buf + uint32_t(rect[0]) + uint32_t(rect[1] * pitch));
  recomp::Wr32(out + 0x10, uint32_t(pitch));
  recomp::Wr32(out + 0x14, 1);
  for (int i = 0; i < 4; i++) recomp::Wr32(out + 0x18 + 4 * uint64_t(i), uint32_t(rect[i]));
  // The image's box from the metrics, rounded outwards.
  const float left = x + m[2], top = y + m[3], right = left + m[0], bottom = top - m[1];
  const float fl = std::floor(left), ct = std::ceil(top);
  const int32_t il = int32_t(fl), ir = int32_t(std::ceil(right)), ib = int32_t(std::floor(bottom));
  recomp::Wr32(out + 0x38, uint32_t(ir - il));
  recomp::Wr32(out + 0x3C, uint32_t(int32_t(ct) - ib));
  PutF(out + 0x28, fl - x);
  PutF(out + 0x2C, ct - y);
  const float pen = x + m[4], cp = std::ceil(pen);
  PutF(out + 0x30, cp - x);
  float adv = cp - x;
  if (pen != cp) {
    const int32_t t = int32_t((float(ir) - right) + pen);
    int32_t v = pen <= right ? t : ir + 1;
    if (ir < t) v = t;
    adv = float(v) - x;
  }
  PutF(out + 0x34, adv);
  return 0;
}
