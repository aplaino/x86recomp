// libSceVideoOut HLE: the main display port. Buffers are guest memory
// the game registers; a flip queues one, and at the vblank the flip rate
// allows, the host thread (orbis/host.h) shows it in the window through
// gfx::Device, clears its label and triggers the flip events. Behaviour
// from libSceVideoOut 11.00's exports: the handle, the error codes, the
// buffer attribute's layout and the flip event's ident and data;
// docs/hle.md has what is not measured.

#include "orbis/hle/videoout.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "orbis/gpu/cp.h"
#include "orbis/gpu/renderer.h"
#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "orbis/host.h"
#include "runtime/gfx/device.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/present.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::orbis::videoout {
namespace {

using Clock = std::chrono::steady_clock;

// Error codes.
constexpr int32_t kErrInvalidValue = int32_t(0x80290001);
constexpr int32_t kErrInvalidAddress = int32_t(0x80290002);
constexpr int32_t kErrResourceBusy = int32_t(0x80290009);
constexpr int32_t kErrInvalidHandle = int32_t(0x8029000B);
constexpr int32_t kErrInvalidEventQueue = int32_t(0x8029000C);

constexpr int kHandle = 1;  // the main port's
constexpr int kBuffers = 16;
constexpr int16_t kFilterVideoOut = -13;
constexpr uint64_t kFlipIdent = (uint64_t(6) << 48) | (uint64_t(kHandle) << 40);

// Pixel formats.
constexpr uint32_t kA8R8G8B8Srgb = 0x80000000;
constexpr uint32_t kA8B8G8R8Srgb = 0x80002200;
constexpr uint32_t kTilingLinear = 1;

struct Attr {  // SceVideoOutBufferAttribute
  uint32_t format, tiling, aspect, width, height, pitch;  // pitch in pixels
};

struct Flip {
  int index;  // -1: blank
  int64_t arg;
  uint64_t after = 0;  // graphics submissions done first (gpu/cp.h)
  gaddr addr = 0;  // the buffer and its attribute, when it runs
  Attr attr = {};
};

struct Port {
  bool open = false;
  uint32_t rate = 0;  // 0: 60 Hz, 1: 30, 2: 20
  gaddr buffers[kBuffers] = {};
  Attr attrs[kBuffers] = {};
  gaddr labels = 0;
  std::deque<Flip> queue;
  uint64_t flips = 0;
  // Host thread only.
  SDL_Window* window = nullptr;
  std::unique_ptr<gfx::Device> dev;
  int w = 0, h = 0;
};

std::mutex g_mu;
Port g_port;

Attr ReadAttr(gaddr a) {
  return {Rd32(a), Rd32(a + 4), Rd32(a + 8), Rd32(a + 12), Rd32(a + 16), Rd32(a + 20)};
}

// The window, sized to the frame or, on a smaller desktop, to fit it.
void OpenWindow(Port& p, int w, int h) {
  EnsureHostVideo();
  float scale = 1;
  if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
    const float d = m->pixel_density > 0 ? m->pixel_density : 1.0f;
    scale = std::min({1.0f, 0.9f * float(m->w) * d / float(w), 0.9f * float(m->h) * d / float(h)});
  }
  const float d = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  const float pts = scale / (d > 0 ? d : 1.0f);
  p.window = SDL_CreateWindow("", std::max(1, int(float(w) * pts)), std::max(1, int(float(h) * pts)),
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  hle::GfxWindowFlags());
  if (!p.window) FatalF("SDL_CreateWindow: %s", SDL_GetError());
  p.dev = hle::CreateGfxDevice(p.window);
  p.w = w;
  p.h = h;
  p.dev->ResetSwapChain(w, h, false);
}

// Shows a flip's buffer (host thread).
void Show(Port& p, const Flip& f) {
  std::vector<uint8_t> px;
  int w, h;
  if (f.index < 0) {
    w = p.w ? p.w : 1920;
    h = p.h ? p.h : 1080;
    px.assign(size_t(w) * h * 4, 0);
  } else {
    const Attr& a = f.attr;
    w = int(a.width);
    h = int(a.height);
    px.resize(size_t(w) * h * 4);
    if (a.tiling != kTilingLinear) {
      // Only the GPU writes tiled buffers: its colour target at the
      // buffer's address is what to show (black if it has drawn none).
      const gfx::TextureId t = gpu::TargetAt(f.addr, uint32_t(w), uint32_t(h));
      if (t && p.window && w == p.w && h == p.h) {
        gpu::Exclusive([&] {
          p.dev->SetRenderTarget(0, 0);
          p.dev->DrawImage(t, {0, 0, w, h});
          hle::PresentFrame(*p.dev, w, h);
        });
        return;
      }
      std::fill(px.begin(), px.end(), uint8_t(0));
      goto show;
    }
    for (int y = 0; y < h; y++)
      std::memcpy(&px[size_t(y) * w * 4], GPtr(f.addr + uint64_t(y) * a.pitch * 4),
                  size_t(w) * 4);
    if (a.format == kA8B8G8R8Srgb)
      for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    else if (a.format != kA8R8G8B8Srgb)
      FatalF("sceVideoOutSubmitFlip: pixel format %08X not implemented yet", a.format);
  }
show:
  if (!p.window) OpenWindow(p, w, h);
  if (w != p.w || h != p.h) {
    p.w = w;
    p.h = h;
    p.dev->ResetSwapChain(w, h, false);
  }
  p.dev->SetRenderTarget(0, 0);
  const gfx::Rect r{0, 0, w, h};
  p.dev->Blit(px.data(), uint32_t(w * 4), gfx::Format::X8R8G8B8, r, r, false, 0);
  hle::PresentFrame(*p.dev, w, h);
}

// Vblanks at 60 Hz; a flip queued before one runs at it if the flip rate
// allows, on the host thread.
void VblankThread() {
  auto next = Clock::now();
  for (uint64_t vblank = 1;; vblank++) {
    next += std::chrono::nanoseconds(1000000000 / 60);
    std::this_thread::sleep_until(next);
    std::lock_guard<std::mutex> l(g_mu);
    if (g_port.queue.empty() || vblank % (g_port.rate + 1)) continue;
    // The GPU first finishes what was submitted before the flip, as it
    // would by the vblank on the console.
    if (g_port.queue.front().after > gpu::GraphicsCompleted()) continue;
    Flip f = g_port.queue.front();
    g_port.queue.pop_front();
    if (f.index >= 0) {
      f.addr = g_port.buffers[f.index];
      f.attr = g_port.attrs[f.index];
    }
    PostToHost([f] {
      Show(g_port, f);
      GuestSyncPoint sync;
      std::lock_guard<std::mutex> l(g_mu);
      if (f.index >= 0) Wr64(g_port.labels + 8 * uint64_t(f.index), 0);
      g_port.flips++;
      // data: the flip's argument from bit 16, the number of flips the
      // event reports (1: each is reported) in bits 12-15.
      kernel::TriggerEvent(kFlipIdent, kFilterVideoOut, (f.arg << 16) | (1 << 12));
    });
  }
}

}  // namespace

gaddr LabelAddress(int handle) {
  std::lock_guard<std::mutex> l(g_mu);
  return handle == kHandle && g_port.open ? g_port.labels : 0;
}

int32_t SubmitFlip(int handle, int index, uint32_t mode, int64_t arg, uint64_t after) {
  GuestSyncPoint sync;
  std::lock_guard<std::mutex> l(g_mu);
  if (handle != kHandle || !g_port.open) return kErrInvalidHandle;
  if (index < -1 || index >= kBuffers || (index >= 0 && !g_port.buffers[index]))
    FatalF("sceVideoOutSubmitFlip: buffer %d is not registered", index);
  if (mode != 1) FatalF("sceVideoOutSubmitFlip: flip mode %u not implemented yet", mode);
  if (index >= 0) Wr64(g_port.labels + 8 * uint64_t(index), 1);
  g_port.queue.push_back({index, arg, after});
  return 0;
}

}  // namespace recomp::orbis::videoout

using namespace recomp::orbis::videoout;
using recomp::hle::Arg;

// int sceVideoOutOpen(SceUserServiceUserId user, int bus, int index, const
// void* param): bus 0 (the main display) only.
ORBIS_HLE("libSceVideoOut", sceVideoOutOpen, 4) {
  const uint32_t bus = uint32_t(Arg(c, 1));
  if (bus > 8) return uint32_t(kErrInvalidValue);
  if (bus != 0 || Arg(c, 2) != 0 || Arg(c, 3))
    recomp::FatalF("sceVideoOutOpen: bus %u index %d param %llX not implemented yet", bus,
                   int(Arg(c, 2)), (unsigned long long)Arg(c, 3));
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_port.open) return uint32_t(kErrResourceBusy);
    g_port.open = true;
    if (!g_port.labels) {
      g_port.labels = recomp::g_proc->vm.Reserve(0, recomp::GuestMemory::kPageSize);
      if (!g_port.labels ||
          !recomp::g_proc->vm.Commit(g_port.labels, recomp::GuestMemory::kPageSize, recomp::Prot::RW))
        recomp::FatalF("sceVideoOutOpen: out of guest memory");
      std::thread(VblankThread).detach();
    }
  }
  // The window and its device now: the GPU draws with it before the
  // first flip.
  recomp::orbis::RunOnHost([] {
    if (!g_port.window) OpenWindow(g_port, 1920, 1080);
  });
  recomp::orbis::gpu::SetDevice(g_port.dev.get());
  return kHandle;
}

// void sceVideoOutSetBufferAttribute(SceVideoOutBufferAttribute* a, format,
// tiling, aspect, width, height, pitch): fills the attribute.
ORBIS_HLE("libSceVideoOut", sceVideoOutSetBufferAttribute, 7) {
  const recomp::gaddr a = Arg(c, 0);
  recomp::Wr32(a, uint32_t(Arg(c, 1)));
  recomp::Wr32(a + 4, uint32_t(Arg(c, 2)));
  recomp::Wr32(a + 8, uint32_t(Arg(c, 3)));
  recomp::Wr32(a + 12, uint32_t(Arg(c, 4)));
  recomp::Wr32(a + 16, uint32_t(Arg(c, 5)));
  recomp::Wr32(a + 20, uint32_t(Arg(c, 6)));
  for (int i = 24; i < 40; i += 8) recomp::Wr64(a + i, 0);
  return 0;
}

// int sceVideoOutSetFlipRate(int handle, int rate): 0 60 Hz, 1 30 Hz, 2 20 Hz.
ORBIS_HLE("libSceVideoOut", sceVideoOutSetFlipRate, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (int(Arg(c, 0)) != kHandle || !g_port.open) return uint32_t(kErrInvalidHandle);
  const uint32_t rate = uint32_t(Arg(c, 1));
  if (rate > 2) recomp::FatalF("sceVideoOutSetFlipRate: rate %u", rate);
  g_port.rate = rate;
  return 0;
}

// int sceVideoOutAddFlipEvent(SceKernelEqueue eq, int handle, void* udata)
ORBIS_HLE("libSceVideoOut", sceVideoOutAddFlipEvent, 3) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (int(Arg(c, 1)) != kHandle || !g_port.open) return uint32_t(kErrInvalidHandle);
  }
  if (!recomp::orbis::kernel::AddEvent(uint32_t(Arg(c, 0)), kFlipIdent, kFilterVideoOut, Arg(c, 2)))
    return uint32_t(kErrInvalidEventQueue);
  return 0;
}

// int sceVideoOutRegisterBuffers(int handle, int start, void* const* addrs,
// int count, const SceVideoOutBufferAttribute* attr): buffers start ..
// start + count - 1, one attribute for all; returns the group's index.
ORBIS_HLE("libSceVideoOut", sceVideoOutRegisterBuffers, 5) {
  if (!Arg(c, 2) || !Arg(c, 4)) return uint32_t(kErrInvalidAddress);
  std::lock_guard<std::mutex> l(g_mu);
  if (int(Arg(c, 0)) != kHandle || !g_port.open) return uint32_t(kErrInvalidHandle);
  const int start = int(Arg(c, 1)), count = int(Arg(c, 3));
  if (start < 0 || count < 1 || start + count > kBuffers)
    recomp::FatalF("sceVideoOutRegisterBuffers: buffers %d+%d", start, count);
  const Attr a = ReadAttr(Arg(c, 4));
  recomp::WarnF("videoout: buffers %d-%d: %ux%u pitch %u, format %08X, tiling %u, aspect %u\n",
                start, start + count - 1, a.width, a.height, a.pitch, a.format, a.tiling, a.aspect);
  for (int i = 0; i < count; i++) {
    if (g_port.buffers[start + i])
      recomp::FatalF("sceVideoOutRegisterBuffers: buffer %d is registered already", start + i);
    g_port.buffers[start + i] = recomp::Rd64(Arg(c, 2) + 8 * uint64_t(i));
    g_port.attrs[start + i] = a;
  }
  static int groups = 0;
  return uint32_t(groups++);
}

// int sceVideoOutSubmitFlip(int handle, int index, int mode, int64_t arg):
// index -1 blanks the display. After what the game has submitted to the
// GPU so far.
ORBIS_HLE("libSceVideoOut", sceVideoOutSubmitFlip, 4) {
  return uint32_t(SubmitFlip(int(Arg(c, 0)), int(Arg(c, 1)), uint32_t(Arg(c, 2)),
                             int64_t(Arg(c, 3)), recomp::orbis::gpu::GraphicsSubmitted()));
}
