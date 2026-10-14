// Backend selection and presentation geometry for gfx::Device.

#include "runtime/gfx/device.h"

#include <mutex>

namespace recomp::gfx {

namespace {
std::mutex g_present_mu;
Presentation g_present{};
}  // namespace

Rect FitRect(int src_w, int src_h, int dst_w, int dst_h) {
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return {0, 0, dst_w, dst_h};
  int64_t w = dst_w, h = dst_h;
  if (int64_t(dst_w) * src_h > int64_t(dst_h) * src_w)
    w = (int64_t(dst_h) * src_w + src_h / 2) / src_h;  // narrower: pillarbox
  else
    h = (int64_t(dst_w) * src_h + src_w / 2) / src_w;  // wider: letterbox
  const int x = int(dst_w - w) / 2, y = int(dst_h - h) / 2;
  return {x, y, x + int(w), y + int(h)};
}

void NotePresentation(const Presentation& p) {
  std::lock_guard<std::mutex> l(g_present_mu);
  g_present = p;
}

Presentation LastPresentation() {
  std::lock_guard<std::mutex> l(g_present_mu);
  return g_present;
}

std::unique_ptr<Device> CreateDevice(Backend b, SDL_Window* window) {
  switch (b) {
    case Backend::D3D9:
#ifdef RECOMP_HAVE_D3D9
      return CreateD3D9Device(window);
#else
      return nullptr;
#endif
    case Backend::Gpu:
      return CreateGpuDevice(window);
  }
  return nullptr;
}

}  // namespace recomp::gfx
