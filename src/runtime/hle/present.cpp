// Frame presentation shared by the graphics APIs (ddraw, opengl32).

#include "runtime/hle/present.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/hle/user32_window.h"
#include "runtime/log.h"
#include "runtime/perf.h"
#include "runtime/platform.h"
#include "runtime/runtime.h"
#include "runtime/watchdog.h"

namespace recomp::hle {

namespace {

// <ID>_HITCH's mark in a CPU profile: 1.5 ms spent here, at the end of each
// long frame, so the frame's window can be found (what the threads did in
// the hitch-length span before it).
[[gnu::noinline]] void HitchMark() {
  const Uint64 end = SDL_GetTicksNS() + 1500000;
  while (SDL_GetTicksNS() < end) {
  }
}

}  // namespace

namespace {

std::mutex g_shot_mu;
std::string g_shot_path;  // RequestShot's, until the next frame saves it

void SaveFrame(gfx::Device& dev, int w, int h, const std::string& path) {
  std::vector<uint8_t> px(size_t(w) * h * 4);
  dev.ReadBackFront({0, 0, w, h}, px.data(), uint32_t(w * 4), gfx::Format::X8R8G8B8);
  // Relative to the working directory (SDL would take a relative path from
  // the app bundle on iOS).
  std::string file = path;
  if (!file.empty() && file[0] != '/') {
    char cwd[1024];
    if (getcwd(cwd, sizeof cwd)) file = std::string(cwd) + "/" + file;
  }
  if (SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, px.data(), w * 4)) {
    SDL_SaveBMP(surf, file.c_str());
    SDL_DestroySurface(surf);
  }
}

}  // namespace

std::atomic<uint64_t> g_peek_messages{0};

void RequestShot(const std::string& path) {
  std::lock_guard<std::mutex> l(g_shot_mu);
  g_shot_path = path;
}

// Dev aid: <ID>_SHOT_EVERY=N saves every Nth presented frame (from frame
// <ID>_SHOT_FROM on) as $<ID>_SHOT_DIR/frame_NNNNN.bmp (default dir: current
// directory).
void PresentFrame(gfx::Device& dev, int w, int h) {
  static const int every = [] {
    const char* e = Env("SHOT_EVERY");
    return e ? std::atoi(e) : 0;
  }();
  static const int from = [] {
    const char* e = Env("SHOT_FROM");
    return e ? std::atoi(e) : 0;
  }();
  dev.Present();
  // <ID>_FPS_LIMIT=n: at most n frames a second (the next frame starts no
  // sooner than 1/n s after the last one did).
  static const double limit_ns = [] {
    const char* e = Env("FPS_LIMIT");
    const double n = e ? std::atof(e) : 0.0;
    return n > 0 ? 1e9 / n : 0.0;
  }();
  if (limit_ns > 0) {
    static Uint64 next = 0;
    Uint64 now = SDL_GetTicksNS();
    if (next > now) {
      SDL_DelayPrecise(next - now);
      now = SDL_GetTicksNS();
    }
    next = (next && now - next < Uint64(limit_ns)) ? next + Uint64(limit_ns) : now + Uint64(limit_ns);
  }
  // Shots after presenting: the front buffer holds the frame as shown.
  static int frame = 0;
  frame++;
  if (every > 0 && frame >= from && frame % every == 0) {
    const char* dir = Env("SHOT_DIR");
    char path[512];
    std::snprintf(path, sizeof path, "%s/frame_%05d.bmp", dir ? dir : ".", frame);
    SaveFrame(dev, w, h, path);
  }
  std::string requested;
  {
    std::lock_guard<std::mutex> l(g_shot_mu);
    requested.swap(g_shot_path);
  }
  if (!requested.empty()) SaveFrame(dev, w, h, requested);
  // Loading screens present without pumping messages; keep answering the
  // window system (compositor pings) so the window does not look hung.
  // Events are only queued for the game, as Windows would.
  PumpHostEvents();
  WatchdogFrame();
  // <ID>_HITCH=ms: report frames that took longer than that.
  static const double hitch_ms = [] {
    const char* e = Env("HITCH");
    return e ? std::atof(e) : 0.0;
  }();
  if (hitch_ms > 0) {
    static Uint64 last = 0, start = 0;
    Uint64 now = SDL_GetTicksNS();
    if (!start) start = now;
    double dt = last ? (now - last) / 1e6 : 0;
    if (dt > hitch_ms) {
      WarnF("hitch: frame %d took %.1f ms (t=%.2f s)\n", frame, dt,
            (now - start) / 1e9);
      HitchMark();
    }
    last = now;
  }
  // <ID>_FPS_REPORT=s: every s seconds, the frame rate and the spread of
  // frame intervals.
  static const double report_s = [] {
    const char* e = Env("FPS_REPORT");
    return e ? std::atof(e) : 0.0;
  }();
  if (perf::On()) {
    static uint64_t last = 0;
    const uint64_t now = platform::MonotonicNs();
    if (!last) last = now;
    if (now - last >= 2000000000u) {
      const std::string st = dev.TakePerfStats();
      if (!st.empty()) WarnF("gfx: perf: %s\n", st.c_str());
      last = now;
    }
  }
  if (report_s > 0) {
    static Uint64 last = 0, first = 0, t0 = 0;
    static std::vector<double> dts;
    Uint64 now = SDL_GetTicksNS();
    if (!t0) t0 = now;
    if (last) dts.push_back((now - last) / 1e6);
    if (!first) first = now;
    last = now;
    if ((now - first) / 1e9 >= report_s && !dts.empty()) {
      std::sort(dts.begin(), dts.end());
      auto q = [&](double p) { return dts[size_t(p * double(dts.size() - 1) + 0.5)]; };
      static uint64_t peeks_before = 0;
      const uint64_t peeks = g_peek_messages.load(std::memory_order_relaxed);
      const double gpu = dev.TakeGpuFrameMs();
      char gpu_text[32] = "";
      if (gpu >= 0) std::snprintf(gpu_text, sizeof gpu_text, "; GPU %.2f ms a frame", gpu);
      double sm, s99, smax;
      char shown_text[80] = "";
      if (dev.TakeShownIntervals(&sm, &s99, &smax))
        std::snprintf(shown_text, sizeof shown_text, "; shown ms: median %.2f p99 %.2f max %.2f",
                      sm, s99, smax);
      double lm, l99;
      char lat_text[64] = "";
      if (dev.TakePresentLatency(&lm, &l99))
        std::snprintf(lat_text, sizeof lat_text, "; present to display ms: median %.1f p99 %.1f", lm, l99);
      WarnF("fps: %.2f at t=%.1f s (interval ms: min %.3f p1 %.3f median %.3f p99 %.3f max %.3f; "
            "%.1f PeekMessage a frame%s%s%s)\n",
            dts.size() / ((now - first) / 1e9), (now - t0) / 1e9, dts.front(), q(0.01),
            q(0.5), q(0.99), dts.back(), double(peeks - peeks_before) / double(dts.size()),
            gpu_text, shown_text, lat_text);
      peeks_before = peeks;
      dts.clear();
      first = now;
    }
  }
}

gfx::Backend GfxBackend() {
  static const gfx::Backend b = [] {
    const char* e = Env("GFX");
    if (e && !std::strcmp(e, "gpu")) return gfx::Backend::Gpu;
#if defined(__APPLE__)
    if (e && !std::strcmp(e, "metal")) return gfx::Backend::Metal;
#endif
    if (e && !std::strcmp(e, "d3d9")) {
#ifdef RECOMP_HAVE_D3D9
      return gfx::Backend::D3D9;
#else
      FatalF("graphics backend d3d9 is not in this build (no dxvk-native)");
#endif
    }
    if (e && *e) FatalF("unknown graphics backend '%s' (d3d9, gpu or metal)", e);
    return gfx::Backend::Gpu;
  }();
  return b;
}

uint64_t GfxWindowFlags() {
  switch (GfxBackend()) {
    case gfx::Backend::D3D9:
      return SDL_WINDOW_VULKAN;  // dxvk's SDL3 WSI makes a Vulkan surface
    case gfx::Backend::Metal:
      return SDL_WINDOW_METAL;
    case gfx::Backend::Gpu:
#if defined(__APPLE__)
      return SDL_WINDOW_METAL;
#else
      return SDL_WINDOW_VULKAN;
#endif
  }
  return 0;
}

std::unique_ptr<gfx::Device> CreateGfxDevice(SDL_Window* w) {
  std::unique_ptr<gfx::Device> d = gfx::CreateDevice(GfxBackend(), w);
  if (!d) FatalF("cannot create the graphics device");
  const char* aa = Env("AA");
  d->SetPostAntialiasing(aa ? *aa && *aa != '0' : Game().post_aa);
  if (perf::On()) d->EnablePerfStats();
  const char* aniso = Env("ANISO");
  d->SetMinAnisotropy(aniso ? std::atoi(aniso) : Game().anisotropy);
  return d;
}

}  // namespace recomp::hle
