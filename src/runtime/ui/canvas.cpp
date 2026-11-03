#include "runtime/ui/canvas.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <mutex>

#include "runtime/log.h"

namespace recomp::ui {
namespace {

// Source over destination, straight alpha; `cover` (0-255) scales the
// source's alpha.
uint32_t Over(uint32_t dst, uint32_t src, uint32_t cover = 255) {
  const uint32_t sa = (src >> 24) * cover / 255;
  if (!sa) return dst;
  const uint32_t da = dst >> 24;
  const uint32_t oa = sa + da * (255 - sa) / 255;
  uint32_t out = oa << 24;
  for (int s = 0; s < 24; s += 8) {
    const uint32_t sc = (src >> s) & 0xFF, dc = (dst >> s) & 0xFF;
    out |= ((sc * sa + dc * da * (255 - sa) / 255) / oa) << s;
  }
  return out;
}

// The fonts fontconfig gives for sans-serif, best first; each face is
// opened when a character first needs it.
struct Fonts {
  std::mutex mu;
  FT_Library ft = nullptr;
  std::vector<std::string> files;
  std::vector<FT_Face> faces;  // null: not opened yet, or no such file

  Fonts() {
    if (FT_Init_FreeType(&ft)) FatalF("ui: FreeType does not start");
    if (!FcInit()) FatalF("ui: fontconfig does not start");
    FcPattern* p = FcNameParse(reinterpret_cast<const FcChar8*>("sans-serif"));
    FcConfigSubstitute(nullptr, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult r;
    if (FcFontSet* set = FcFontSort(nullptr, p, FcTrue, nullptr, &r)) {
      for (int i = 0; i < set->nfont; i++) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) == FcResultMatch)
          files.emplace_back(reinterpret_cast<const char*>(file));
      }
      FcFontSetDestroy(set);
    }
    FcPatternDestroy(p);
    if (files.empty()) FatalF("ui: fontconfig has no sans-serif font");
    faces.assign(files.size(), nullptr);
  }

  // The first face with a glyph for `ch` (else the first face), at `size`.
  FT_Face For(uint32_t ch, int size) {
    FT_Face first = nullptr;
    for (size_t i = 0; i < files.size(); i++) {
      if (!faces[i] && FT_New_Face(ft, files[i].c_str(), 0, &faces[i])) faces[i] = nullptr;
      if (!faces[i]) continue;
      if (!first) first = faces[i];
      if (FT_Get_Char_Index(faces[i], ch)) {
        first = faces[i];
        break;
      }
    }
    if (!first) FatalF("ui: no font can be opened (%s first)", files[0].c_str());
    FT_Set_Pixel_Sizes(first, 0, FT_UInt(size));
    return first;
  }
};

Fonts& TheFonts() {
  static Fonts* f = new Fonts;  // used until exit
  return *f;
}

// The next code point of UTF-8 text (U+FFFD for a bad sequence).
uint32_t Next(const std::string& s, size_t& i) {
  const uint8_t b = uint8_t(s[i++]);
  if (b < 0x80) return b;
  const int n = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
  if (!n) return 0xFFFD;
  uint32_t ch = b & (0x3F >> n);
  for (int k = 0; k < n; k++) {
    if (i >= s.size() || (uint8_t(s[i]) & 0xC0) != 0x80) return 0xFFFD;
    ch = ch << 6 | (uint8_t(s[i++]) & 0x3F);
  }
  return ch;
}

// Lays out the text; draws it too if `c`.
int Run(Canvas* c, int x, int y, int size, const std::string& s, uint32_t argb, int max_w) {
  Fonts& f = TheFonts();
  std::lock_guard<std::mutex> l(f.mu);
  int pen = 0;
  for (size_t i = 0; i < s.size();) {
    const uint32_t ch = Next(s, i);
    FT_Face face = f.For(ch, size);
    if (FT_Load_Char(face, ch, c ? FT_LOAD_RENDER : FT_LOAD_DEFAULT)) continue;
    const FT_GlyphSlot g = face->glyph;
    const int adv = int(g->advance.x >> 6);
    if (pen + adv > max_w) break;
    if (c) {
      const FT_Bitmap& b = g->bitmap;
      for (unsigned r = 0; r < b.rows; r++) {
        const int py = y - g->bitmap_top + int(r);
        if (py < 0 || py >= c->h) continue;
        for (unsigned col = 0; col < b.width; col++) {
          const int px = x + pen + g->bitmap_left + int(col);
          if (px < 0 || px >= c->w) continue;
          const uint8_t cover = b.buffer[r * unsigned(b.pitch) + col];
          uint32_t& d = c->px[size_t(py) * c->w + px];
          d = Over(d, argb, cover);
        }
      }
    }
    pen += adv;
  }
  return pen;
}

}  // namespace

void Canvas::Fill(int x, int y, int fw, int fh, uint32_t argb) {
  for (int py = std::max(y, 0); py < std::min(y + fh, h); py++)
    for (int px_ = std::max(x, 0); px_ < std::min(x + fw, w); px_++) {
      uint32_t& d = px[size_t(py) * w + px_];
      d = Over(d, argb);
    }
}

void Canvas::Draw(int x, int y, int dw, int dh, const uint32_t* src, int sw, int sh) {
  for (int py = std::max(y, 0); py < std::min(y + dh, h); py++)
    for (int px_ = std::max(x, 0); px_ < std::min(x + dw, w); px_++) {
      const uint32_t s = src[size_t((py - y) * sh / dh) * sw + (px_ - x) * sw / dw];
      uint32_t& d = px[size_t(py) * w + px_];
      d = Over(d, s);
    }
}

int Canvas::Text(int x, int y, int size, const std::string& utf8, uint32_t argb, int max_w) {
  return Run(this, x, y, size, utf8, argb, max_w);
}

int TextWidth(int size, const std::string& utf8) { return Run(nullptr, 0, 0, size, utf8, 0, 1 << 30); }

}  // namespace recomp::ui
