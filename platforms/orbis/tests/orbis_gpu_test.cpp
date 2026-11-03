// orbis/gpu's dispatches on the SDL_GPU backend, from hand-assembled GFX7
// code in guest memory: a wave's stores through a swizzled V# with
// ADD_TID_ENABLE (a geometry shader's ring) land where the ISA's swizzled
// addressing puts them; a buffer kept on the GPU is read with what an
// earlier dispatch stored there. Needs a Vulkan device (lavapipe will do); exits 77
// (skipped) without one.

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include "orbis/gpu/renderer.h"
#include "runtime/gfx/device.h"
#include "x86/runtime.h"

namespace {

using namespace recomp;
using namespace recomp::orbis::gpu;

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

// SH registers (dwords from 0xB000): COMPUTE_NUM_THREAD_X, PGM_LO, PGM_HI,
// PGM_RSRC1, PGM_RSRC2, USER_DATA_0.
constexpr uint32_t kThreadX = 0x207, kPgmLo = 0x20C, kRsrc1 = 0x212, kUser = 0x240;

constexpr gaddr kCode = 0x100000, kRing = 0x110000, kKept = 0x120000, kCopy = 0x130000;

}  // namespace

int main() {
  Process p;
  if (!p.mem.Reserve(36)) {
    std::printf("cannot set up guest memory\n");
    return 1;
  }
  p.vm.Init(&p.mem, 0x10000, uint64_t(1) << 36);
  if (!p.vm.Reserve(kCode, 0x40000) || !p.vm.Commit(kCode, 0x40000, Prot::RW)) {
    std::printf("cannot commit guest memory\n");
    return 1;
  }
  g_proc = &p;

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::printf("skipped: SDL video: %s\n", SDL_GetError());
    return 77;
  }
  SDL_Window* w = SDL_CreateWindow("orbis_gpu_test", 64, 64, SDL_WINDOW_HIDDEN);
  std::unique_ptr<gfx::Device> dev = w ? gfx::CreateGpuDevice(w) : nullptr;
  if (!dev) {
    std::printf("skipped: no SDL_GPU device\n");
    return 77;
  }
  dev->ResetSwapChain(8, 8, false);
  SetDevice(dev.get());

  // Each lane stores its lane at element offset 0 and the lane + 64 at 4,
  // through s[0:3]: no index or offset VGPR, so the index is the lane.
  static const uint32_t kStores[] = {
      0x4A0200C0,              // v_add_i32 v1, vcc, 64, v0
      0xE0700000, 0x80000000,  // buffer_store_dword v0, s[0:3], 0
      0xE0700004, 0x80000100,  // buffer_store_dword v1, s[0:3], 0 offset:4
      0xBF810000,              // s_endpgm
  };
  std::memcpy(GPtr(kCode), kStores, sizeof kStores);
  std::memset(GPtr(kRing), 0xFF, 0x1000);

  // The V#: a stride of 8 bytes, 64 records, swizzled in elements of 4
  // bytes by 16 indices, ADD_TID_ENABLE, 32-bit UINT.
  Regs r;
  r.sh[kThreadX] = 64;
  r.sh[kPgmLo] = uint32_t(kCode >> 8);
  r.sh[kRsrc1] = 0;
  r.sh[kRsrc1 + 1] = 4 << 1;  // USER_SGPR
  const uint32_t v[4] = {uint32_t(kRing), 8u << 16 | 1u << 31, 64,
                         0xFACu | 4u << 12 | 4u << 15 | 1u << 19 | 1u << 21 | 1u << 23};
  std::memcpy(r.sh + kUser, v, sizeof v);
  const uint32_t groups[3] = {1, 1, 1};
  Dispatch(r, groups);
  Settle(true);

  // Lane t's element at offset o: ((t / 16) * 8 + (o / 4) * 4) * 16 +
  // (t % 16) * 4; the 64 lanes' two elements fill 512 bytes.
  bool ok = true;
  for (uint32_t t = 0; t < 64; t++)
    for (uint32_t o = 0; o < 8; o += 4) {
      const uint32_t at = ((t / 16) * 8 + (o / 4) * 4) * 16 + (t % 16) * 4;
      ok &= Rd32(kRing + at) == t + 16 * o;
    }
  Check(ok, "swizzled stores land at the ISA's addresses");
  bool rest = true;
  for (uint32_t at = 512; at < 0x1000; at += 4) rest &= Rd32(kRing + at) == ~0u;
  Check(rest, "nothing is stored past the 64 lanes' elements");

  // A copy of a 64 KiB buffer's first dwords (large enough to be kept in
  // a GPU buffer), then a store into it, then the copy again: the second
  // copy reads the store.
  static const uint32_t kCopyCode[] = {
      0xE0300000, 0x80000100,  // buffer_load_dword v1, s[0:3], 0
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xE0700000, 0x80010100,  // buffer_store_dword v1, s[4:7], 0
      0xBF810000,              // s_endpgm
  };
  static const uint32_t kStore[] = {
      0xE0700000, 0x80000000,  // buffer_store_dword v0, s[0:3], 0
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kCopyAt = kCode + 0x100, kStoreAt = kCode + 0x200;
  std::memcpy(GPtr(kCopyAt), kCopyCode, sizeof kCopyCode);
  std::memcpy(GPtr(kStoreAt), kStore, sizeof kStore);
  std::memset(GPtr(kKept), 0xFF, 0x10000);
  // V#s of 4-byte records with ADD_TID_ENABLE: 16384 of them over the
  // kept buffer, 64 over the copy.
  const uint32_t w3 = 0xFACu | 4u << 15 | 1u << 23;
  const uint32_t kept[4] = {uint32_t(kKept), 4u << 16, 16384, w3};
  const uint32_t copy[4] = {uint32_t(kCopy), 4u << 16, 64, w3};
  Regs c;
  c.sh[kThreadX] = 64;
  c.sh[kPgmLo] = uint32_t(kCopyAt >> 8);
  c.sh[kRsrc1 + 1] = 8 << 1;
  std::memcpy(c.sh + kUser, kept, sizeof kept);
  std::memcpy(c.sh + kUser + 4, copy, sizeof copy);
  Regs st = r;
  st.sh[kPgmLo] = uint32_t(kStoreAt >> 8);
  std::memcpy(st.sh + kUser, kept, sizeof kept);
  Dispatch(c, groups);
  Dispatch(st, groups);
  Dispatch(c, groups);
  Settle(true);
  bool copied = true;
  for (uint32_t t = 0; t < 64; t++) copied &= Rd32(kCopy + 4 * t) == t;
  Check(copied, "a kept buffer is read after a dispatch's stores into it");

  SetDevice(nullptr);
  dev.reset();
  SDL_DestroyWindow(w);
  SDL_Quit();
  if (failures) return 1;
  std::printf("orbis gpu: ok\n");
  return 0;
}
