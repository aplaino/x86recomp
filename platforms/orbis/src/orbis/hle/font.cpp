// libSceFont and libSceFontFt HLE on the host's FreeType. What the game
// sees is libSceFont 11.00's: the memory, surface and output structures,
// the argument checks and codes, and the objects it allocates through the
// game's memory interface (library, renderer, font: 0x100 bytes each, and
// the 0x4000-byte caches of the first two). The glyphs are FreeType's
// (outlines unhinted, anti-aliased): libSceFontFt rasterises with a
// renderer of its own, so pixels differ; its FreeType allocations through
// the game's memory are not made either (docs/hle.md).

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

#include "orbis/hle.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace {

using recomp::gaddr;

constexpr uint32_t kErrFatal = 0x80460001, kErrInvalidParameter = 0x80460002,
                   kErrInvalidMemory = 0x80460003, kErrInvalidLibrary = 0x80460004,
                   kErrInvalidFont = 0x80460005, kErrInvalidRenderer = 0x80460007,
                   kErrAllocationFailed = 0x80460010, kErrNoExternalFonts = 0x80460020,
                   kErrAlreadySpecified = 0x80460021, kErrNoSupportCode = 0x80460041,
                   kErrAlreadyBound = 0x80460060, kErrNotBound = 0x80460061;
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
};
struct Font {
  gaddr library;
  FT_Face face = nullptr;
  float scale_w = 0, scale_h = 0, render_w = 0, render_h = 0;
  gaddr renderer = 0;
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

// Metrics of code at w x h pixels: width, height, then horizontal
// bearing x, y and advance, then vertical ones (SceFontGlyphMetrics).
bool Metrics(Font& f, uint32_t code, float w, float h, float out[8], bool render) {
  FT_Set_Char_Size(f.face, FT_F26Dot6(std::lround(w * 64)), FT_F26Dot6(std::lround(h * 64)), 72,
                   72);
  if (FT_Load_Char(f.face, code, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP |
                                     (render ? FT_LOAD_RENDER : 0)))
    return false;
  const FT_Glyph_Metrics& m = f.face->glyph->metrics;
  const float k = 1.0f / 64.0f;
  out[0] = float(m.width) * k;
  out[1] = float(m.height) * k;
  out[2] = float(m.horiBearingX) * k;
  out[3] = float(m.horiBearingY) * k;
  out[4] = float(m.horiAdvance) * k;
  out[5] = float(m.vertBearingX) * k;
  out[6] = float(m.vertBearingY) * k;
  out[7] = float(m.vertAdvance) * k;
  return true;
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

ORBIS_HLE("libSceFont", sceFontDestroyLibrary, 1) {
  if (!Arg(c, 0)) return kErrInvalidParameter;
  const gaddr p = recomp::Rd64(Arg(c, 0));
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
  recomp::Wr64(Arg(c, 0), 0);
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
  if (!mem) return kErrInvalidParameter;
  if (!ValidMemory(mem)) return kErrInvalidMemory;
  if (!sel || !out) return kErrInvalidParameter;
  if (sel != Selection(1)) recomp::FatalF("sceFontCreateRendererWithEdition: not the FreeType renderer");
  const gaddr r = Alloc(c, mem, 0x100), buffer = Alloc(c, mem, 0x4000);
  if (!r || !buffer) {
    if (buffer) Free(c, mem, buffer);
    if (r) Free(c, mem, r);
    return kErrAllocationFailed;
  }
  std::memset(recomp::GPtr(r), 0, 0x100);
  recomp::Wr16(r, kTagRenderer);
  std::lock_guard<std::mutex> l(g_mu);
  g_renderers[r] = {mem, buffer};
  recomp::Wr64(out, r);
  return 0;
}

ORBIS_HLE("libSceFont", sceFontDestroyRenderer, 1) {
  if (!Arg(c, 0)) return kErrInvalidParameter;
  const gaddr p = recomp::Rd64(Arg(c, 0));
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
  recomp::Wr64(Arg(c, 0), 0);
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
// SceFontGlyphMetrics* m): at the font's scale; zeros on an error.
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
  else if (!Metrics(*f, code, f->scale_w, f->scale_h, m, false)) r = kErrNoSupportCode;
  if (out) PutMetrics(out, m);
  return r;
}

// int sceFontGetKerning(SceFontHandle f, uint32_t pre, uint32_t code,
// SceFontKerning* k): {dx, dy, position x, y} in pixels at the font's
// scale.
ORBIS_HLE("libSceFont", sceFontGetKerning, 4) {
  const gaddr out = Arg(c, 3);
  std::lock_guard<std::mutex> l(g_mu);
  Font* f = FindFont(Arg(c, 0));
  if (!f) return kErrInvalidFont;
  if (!out) return kErrInvalidParameter;
  std::memset(recomp::GPtr(out), 0, 16);
  if (!FT_HAS_KERNING(f->face)) return 0;
  FT_Set_Char_Size(f->face, FT_F26Dot6(std::lround(f->scale_w * 64)),
                   FT_F26Dot6(std::lround(f->scale_h * 64)), 72, 72);
  FT_Vector k;
  if (FT_Get_Kerning(f->face, FT_Get_Char_Index(f->face, FT_ULong(Arg(c, 1))),
                     FT_Get_Char_Index(f->face, FT_ULong(Arg(c, 2))), FT_KERNING_UNFITTED, &k))
    return 0;
  PutF(out, float(k.x) / 64.0f);
  PutF(out + 4, float(k.y) / 64.0f);
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
// coverage), clipped to the scissor. out is {0, the image's first byte,
// width bytes, pixel bytes, the updated rectangle, the image's offset
// from (x, y) (x, y), the advance (twice), the image's size}, rounded as
// libSceFont does.
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
  float m[8] = {};
  if (!err && !Metrics(*f, code, f->render_w, f->render_h, m, true)) err = kErrNoSupportCode;
  if (err) {
    if (mo) PutMetrics(mo, m);
    if (out) std::memset(recomp::GPtr(out), 0, 64);
    return err;
  }
  if (recomp::GPtr(s)[0xC] != 1)
    recomp::FatalF("sceFontRenderCharGlyphImageHorizontal: %u-byte pixels not implemented yet",
                   recomp::GPtr(s)[0xC]);
  PutMetrics(mo, m);
  // The bitmap, its top-left at (x + left, y - top), clipped.
  const FT_GlyphSlot g = f->face->glyph;
  const gaddr buf = recomp::Rd64(s);
  const int32_t pitch = int32_t(recomp::Rd32(s + 8));
  const int32_t sw = int32_t(recomp::Rd32(s + 0x20)), sh = int32_t(recomp::Rd32(s + 0x24));
  const int32_t gx = int32_t(std::floor(x)) + g->bitmap_left, gy = int32_t(std::floor(y)) - g->bitmap_top;
  const int32_t x0 = std::max(gx, 0), y0 = std::max(gy, 0);
  const int32_t x1 = std::min(gx + int32_t(g->bitmap.width), sw);
  const int32_t y1 = std::min(gy + int32_t(g->bitmap.rows), sh);
  for (int32_t py = y0; py < y1; py++)
    for (int32_t px = x0; px < x1; px++)
      recomp::GPtr(buf + uint64_t(py) * uint32_t(pitch) + uint32_t(px))[0] =
          g->bitmap.buffer[(py - gy) * g->bitmap.pitch + (px - gx)];
  const int32_t rw = std::max(x1 - x0, 0), rh = std::max(y1 - y0, 0);
  recomp::Wr64(out, 0);
  recomp::Wr64(out + 8, buf + uint64_t(y0) * uint32_t(pitch) + uint32_t(x0));
  recomp::Wr32(out + 0x10, uint32_t(pitch));
  recomp::Wr32(out + 0x14, 1);
  recomp::Wr32(out + 0x18, uint32_t(x0));
  recomp::Wr32(out + 0x1C, uint32_t(y0));
  recomp::Wr32(out + 0x20, uint32_t(rw));
  recomp::Wr32(out + 0x24, uint32_t(rh));
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
