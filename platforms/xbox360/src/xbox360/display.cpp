// xbox360/display.h.

#include "xbox360/display.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <vector>

#include "ppc/fatal.h"
#include "runtime/gfx/present.h"
#include "xbox360/config.h"
#include "xbox360/edram.h"
#include "xbox360/gpu.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/memory.h"

namespace recomp::xbox360 {
namespace {

using ppc::Fatal;

// Texture format k_8_8_8_8 (the fetch constant's dword 1, bits 0-5).
constexpr uint32_t kTex8888 = 6;

struct Display {
  SDL_Window* window = nullptr;
  std::unique_ptr<gfx::Device> dev;
  std::mutex dev_lock;
  int w = 0, h = 0;  // the swap chain's size
  // The frame a swap queued, X8R8G8B8, until the main thread has shown
  // it: the next swap waits for that, as a swap chain holds the GPU back
  // (so every swap is shown once, in order).
  std::mutex frame_lock;
  std::condition_variable frame_changed;
  std::vector<uint32_t> frame;
  int frame_w = 0, frame_h = 0;
  bool queued = false;
};
Display* g_display;

// The window, sized to the video mode or, on a smaller desktop, to fit it
// (as the PS4's video output does).
void OpenWindow(Display& d, int w, int h) {
  float scale = 1;
  if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
    const float density = m->pixel_density > 0 ? m->pixel_density : 1.0f;
    scale = std::min({1.0f, 0.9f * float(m->w) * density / float(w),
                      0.9f * float(m->h) * density / float(h)});
  }
  const float content = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  const float pts = scale / (content > 0 ? content : 1.0f);
  d.window = SDL_CreateWindow("", std::max(1, int(float(w) * pts)), std::max(1, int(float(h) * pts)),
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  gfx::ChosenWindowFlags());
  if (!d.window) Fatal(nullptr, "display: SDL_CreateWindow: %s", SDL_GetError());
  d.dev = gfx::CreateChosenDevice(d.window);
  d.w = w;
  d.h = h;
  d.dev->ResetSwapChain(w, h, false);
}

// One component of a k_8_8_8_8 texel after the fetch constant's swizzle:
// 3 bits per destination, 0-3 a source component, 4 zero, 5 one.
uint32_t Swizzled(uint32_t texel, uint32_t select) {
  if (select < 4) return texel >> 8 * select & 0xFF;
  if (select == 4) return 0;
  if (select == 5) return 0xFF;
  Fatal(nullptr, "display: front buffer swizzle %u not implemented yet", select);
}

}  // namespace

void StartDisplay() {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) Fatal(nullptr, "display: SDL video: %s", SDL_GetError());
  // The EDRAM's resolves are compute programs, which only SDL_GPU runs.
  if (gfx::ChosenBackend() != gfx::Backend::Gpu)
    Fatal(nullptr, "display: the Xbox 360 needs the SDL_GPU graphics backend");
  g_display = new Display;
  Display& d = *g_display;
  VideoMode m = CurrentVideoMode();
  // SDL's offscreen video driver (automated tests) has nowhere to show
  // frames, and host drivers may not present to its surfaces: a device
  // without a window, whose frames reach only the dev aids.
  if (!std::strcmp(SDL_GetCurrentVideoDriver(), "offscreen")) {
    d.dev = gfx::CreateGpuDevice(nullptr);
    if (!d.dev) Fatal(nullptr, "display: no host GPU device: %s", SDL_GetError());
    d.w = int(m.width);
    d.h = int(m.height);
    d.dev->ResetSwapChain(d.w, d.h, false);
    return;
  }
  OpenWindow(d, int(m.width), int(m.height));
}

gfx::Device& HostDevice() { return *g_display->dev; }
std::mutex& HostDeviceLock() { return g_display->dev_lock; }

void QueueFrame(const uint32_t fetch[6], uint32_t width, uint32_t height) {
  uint32_t format = fetch[1] & 0x3F, endian = fetch[1] >> 6 & 3, base = fetch[1] & 0xFFFFF000;
  uint32_t pitch = (fetch[0] >> 22 & 0x1FF) * 32;
  bool tiled = fetch[0] >> 31;
  uint32_t tex_w = (fetch[2] & 0x1FFF) + 1, tex_h = (fetch[2] >> 13 & 0x1FFF) + 1;
  uint32_t swizzle = fetch[3] >> 1 & 0xFFF;
  if (format != kTex8888) Fatal(nullptr, "display: front buffer format %u not implemented yet", format);
  int w = int(std::min(width, tex_w)), h = int(std::min(height, tex_h));
  if (w <= 0 || h <= 0) return;
  // The display's gamma ramp is not applied: the texels are shown as
  // stored (console check).
  std::vector<uint32_t> px(size_t(w) * h);
  const uint32_t va = PhysicalToVirtual(base);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint32_t at = tiled ? uint32_t(Tiled2D(x, y, pitch, 2)) : (uint32_t(y) * pitch + uint32_t(x)) * 4;
      uint32_t t = GpuSwap(__builtin_bswap32(Rd32(va + at)), endian);
      px[size_t(y) * w + x] = Swizzled(t, swizzle & 7) << 16 | Swizzled(t, swizzle >> 3 & 7) << 8 |
                              Swizzled(t, swizzle >> 6 & 7);
    }
  Display& d = *g_display;
  {
    std::unique_lock<std::mutex> l(d.frame_lock);
    d.frame_changed.wait(l, [&] { return !d.queued; });
    d.frame = std::move(px);
    d.frame_w = w;
    d.frame_h = h;
    d.queued = true;
  }
  d.frame_changed.notify_all();
}

void RunDisplay() {
  Display& d = *g_display;
  std::vector<uint32_t> px;
  for (;;) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
      if (e.type == SDL_EVENT_QUIT) ExitProcess(0);
    int w, h;
    {
      std::unique_lock<std::mutex> l(d.frame_lock);
      if (!d.frame_changed.wait_for(l, std::chrono::milliseconds(10), [&] { return d.queued; })) continue;
      px.swap(d.frame);
      w = d.frame_w;
      h = d.frame_h;
    }
    {
      std::lock_guard<std::mutex> l(d.dev_lock);
      if (w != d.w || h != d.h) {
        d.w = w;
        d.h = h;
        d.dev->ResetSwapChain(w, h, false);
      }
      d.dev->SetRenderTarget(0, 0);
      const gfx::Rect r{0, 0, w, h};
      d.dev->Blit(px.data(), uint32_t(w * 4), gfx::Format::X8R8G8B8, r, r, false, 0);
      gfx::PresentWithDevAids(*d.dev, w, h);
    }
    {
      std::lock_guard<std::mutex> l(d.frame_lock);
      d.queued = false;
    }
    d.frame_changed.notify_all();
  }
}

}  // namespace recomp::xbox360
