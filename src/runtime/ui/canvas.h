#pragma once
// Pictures the runtime draws itself, over a game's frames: a system
// dialog of a console whose system software drew it. Drawn on the CPU
// into an A8R8G8B8 image with straight alpha (gfx::Device::BlitBlend
// shows it); text in the host's user interface font, which fontconfig
// chooses (sans-serif, then its fallbacks for characters it lacks),
// through FreeType.

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::ui {

struct Canvas {
  Canvas(int w, int h) : w(w), h(h), px(size_t(w) * h) {}
  int w, h;
  std::vector<uint32_t> px;  // rows top to bottom, w pixels each

  // Each blends source over what is there, clipped to the canvas.
  void Fill(int x, int y, int fw, int fh, uint32_t argb);
  // A sw x sh A8R8G8B8 image scaled into the rectangle (nearest texel).
  void Draw(int x, int y, int dw, int dh, const uint32_t* src, int sw, int sh);
  // UTF-8 text `size` pixels high (em), its baseline at y, cut off at
  // max_w pixels; returns its width.
  int Text(int x, int y, int size, const std::string& utf8, uint32_t argb, int max_w = 1 << 30);
};

// The width Canvas::Text would draw.
int TextWidth(int size, const std::string& utf8);

}  // namespace recomp::ui
