// orbis/gpu's dispatches on the SDL_GPU backend, from hand-assembled GFX7
// code in guest memory: a wave's stores through a swizzled V# with
// ADD_TID_ENABLE (a geometry shader's ring) land where the ISA's swizzled
// addressing puts them; a buffer kept on the GPU is read with what an
// earlier dispatch stored there; texture levels, array slices and 3D
// slices load what their layout (tiling.h) puts there; cube maps sample
// the face and texel the v_cube* instructions select; explicit derivatives
// select the level; an indirect dispatch runs the groups its arguments
// give; rectangle lists (their fourth corner's outputs derived from the
// other three), triangle fans and quads cover what the hardware covers; a
// T# of several levels over colour targets reads each level's target.
// Needs a Vulkan device (lavapipe will do); exits 77 (skipped) without
// one.

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "orbis/gpu/cp.h"
#include "orbis/gpu/renderer.h"
#include "orbis/gpu/tiling.h"
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

constexpr gaddr kCode = 0x100000, kRing = 0x110000, kKept = 0x120000, kCopy = 0x130000,
                kOut = 0x138000, kTex = 0x140000;

// A T# of R32_UINT texels at addr: type 9 (2D), 10 (3D), 11 (cube) or 13
// (2D array), levels 0 to last, depth (3D) or slices (array), the identity
// destination selects.
void TSharp(uint32_t t[8], gaddr addr, uint32_t w, uint32_t h, uint32_t type, uint32_t tile,
            uint32_t last, uint32_t depth) {
  t[0] = uint32_t(addr >> 8);
  t[1] = 4u << 20 | 4u << 26;
  t[2] = (w - 1) | (h - 1) << 14;
  t[3] = 0xFACu | last << 16 | tile << 20 | type << 28;
  t[4] = (depth - 1) | (w - 1) << 13;
  t[5] = type == 13 ? (depth - 1) << 13 : 0;
  t[6] = t[7] = 0;
}

// Runs code with the user data on one group of `threads` (x) and waits
// for it; rsrc1 and rsrc2 (beside USER_SGPR) as given.
void Run(gaddr code, const uint32_t* user, uint32_t n, uint32_t threads = 1, uint32_t rsrc1 = 0,
         uint32_t rsrc2 = 0) {
  Regs r;
  r.sh[kThreadX] = threads;
  r.sh[kThreadX + 1] = r.sh[kThreadX + 2] = 1;
  r.sh[kPgmLo] = uint32_t(code >> 8);
  r.sh[kRsrc1] = rsrc1;
  r.sh[kRsrc1 + 1] = n << 1 | rsrc2;
  std::memcpy(r.sh + kUser, user, 4 * n);
  const uint32_t groups[3] = {1, 1, 1};
  Dispatch(r, groups);
  Settle(true);
}

// A V# of 32-bit dwords over n bytes at addr.
void VSharp(uint32_t v[4], gaddr addr, uint32_t n) {
  v[0] = uint32_t(addr);
  v[1] = 0;
  v[2] = n;
  v[3] = 0xFACu | 4u << 15;
}

// The registers of a draw of primitive type `prim` (positions: float4s at
// kVerts, from the vertex shader at vs) into an 8x8
// target of four floats at `target`: the vertex shader exports the
// position as parameter 0 too, the pixel shader its interpolated x and y,
// 0 and 1.
constexpr gaddr kVerts = 0x1A0000, kVsAt = kCode + 0x700, kPsAt = kCode + 0x800,
                kVsOffsetAt = kCode + 0x900;
void DrawRegs(Regs& r, uint32_t prim, gaddr target, gaddr vs) {
  uint32_t* cx = r.context;
  cx[0x00D] = 0x40004000;  // the scissors open
  cx[0x081] = 0x80000000;
  cx[0x082] = 0x40004000;
  cx[0x090] = 0x80000000;
  cx[0x091] = 0x40004000;
  cx[0x094] = 0x80000000;
  cx[0x095] = 0x40004000;
  cx[0x08E] = cx[0x08F] = 0xF;  // target and shader masks
  cx[0x202] = 1u << 4;          // CB_COLOR_CONTROL: normal
  cx[0x204] = 1u << 19;         // PA_CL_CLIP_CNTL: z 0 to 1
  const float vport[6] = {4.0f, 4.0f, -4.0f, 4.0f, 1.0f, 0.0f};
  std::memcpy(cx + 0x10F, vport, sizeof vport);
  cx[0x1B3] = cx[0x1B4] = 2;  // SPI_PS_INPUT_ENA/ADDR: PERSP_CENTER
  cx[0x1C3] = 4;              // SPI_SHADER_POS_FORMAT: 4 components
  cx[0x1C5] = 9;              // SPI_SHADER_COL_FORMAT: 32_ABGR
  uint32_t* cb = cx + 0x318;  // CB_COLOR0: 8x8, 32_32_32_32 float
  cb[0] = uint32_t(target >> 8);
  cb[4] = 14u << 2 | 7u << 8;
  r.sh[0x48] = uint32_t(vs >> 8);
  r.sh[0x4B] = 5u << 1;
  VSharp(r.sh + 0x4C, kVerts, 16 * 8);
  r.sh[0x4C + 1] = 16u << 16;  // stride 16
  r.sh[0x4C + 2] = 8;
  r.sh[0x08] = uint32_t(kPsAt >> 8);
  r.uconfig[0x242] = prim;
}

// True if every pixel of the target at `target` has its centre's x in clip
// space as its red (so drawn, and its parameter where the vertices put it).
bool Covered(uint32_t prim, gaddr target, gfx::Device* dev) {
  const gfx::TextureId tex = TargetAt(target, 8, 8);
  if (!tex) return false;
  float px[8 * 8 * 4] = {};
  dev->ReadImage(tex, 0, 0, {0, 0, 8, 8}, px, 8 * 16);
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      const float* p = px + 4 * (y * 8 + x);
      if (p[3] != 1.0f || std::fabs(p[0] - (-1.0f + (2 * x + 1) / 8.0f)) > 1e-4f) {
        std::printf("  primitive type %02X: pixel (%d,%d) is %g %g %g %g\n", prim, x, y, p[0], p[1],
                    p[2], p[3]);
        return false;
      }
    }
  return true;
}

void DrawInto(uint32_t prim, uint32_t count, gaddr target) {
  Regs r;
  DrawRegs(r, prim, target, kVsAt);
  Draw(r, {count, 1, 0, 2});
  Settle(true);
}
bool DrawCovers(uint32_t prim, uint32_t count, gaddr target, gfx::Device* dev) {
  DrawInto(prim, count, target);
  return Covered(prim, target, dev);
}

}  // namespace

int main() {
  Process p;
  if (!p.mem.Reserve(36)) {
    std::printf("cannot set up guest memory\n");
    return 1;
  }
  p.vm.Init(&p.mem, 0x10000, uint64_t(1) << 36);
  if (!p.vm.Reserve(kCode, 0x100000) || !p.vm.Commit(kCode, 0x100000, Prot::RW) ||
      !p.vm.Reserve(0x100000000ull, 0x10000) || !p.vm.Commit(0x100000000ull, 0x10000, Prot::RW)) {
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

  // Loads (image_load_mip) from x, y, the slice or z and the level in
  // s12-s15 (a 2D texture's level in s14), stored at kOut.
  static const uint32_t kLoad[] = {
      0x7E02020C,              // v_mov_b32 v1, s12
      0x7E04020D,              // v_mov_b32 v2, s13
      0x7E06020E,              // v_mov_b32 v3, s14
      0x7E08020F,              // v_mov_b32 v4, s15
      0xF0041F00, 0x00000501,  // image_load_mip v[5:8], v[1:4], s[0:7] dmask:0xf unorm
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xE0780000, 0x80020500,  // buffer_store_dwordx4 v[5:8], off, s[8:11], 0
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kLoadAt = kCode + 0x300;
  std::memcpy(GPtr(kLoadAt), kLoad, sizeof kLoad);
  // Textures whose every dword holds its own index, so a load returns
  // where it read: tile mode 14 (2D thin; levels below a macro tile in 1D)
  // and 13 (1D thin).
  for (uint32_t i = 0; i < 0x40000 / 4; i++) Wr32(kTex + 4 * i, i);
  struct LoadCase {
    const char* what;
    uint32_t w, h, type, tile, last, depth;
    uint32_t x, y, slice, level;
  } loads[] = {
      {"a 2D texture's level 0", 64, 64, 9, 14, 6, 1, 5, 7, 0, 0},
      {"a 2D texture's level 1", 64, 64, 9, 14, 6, 1, 3, 2, 0, 1},
      {"a 2D texture's level 3 (1D)", 64, 64, 9, 14, 6, 1, 1, 6, 0, 3},
      {"a 2D texture's last level", 64, 64, 9, 14, 6, 1, 0, 0, 0, 6},
      {"a 2D array's slice 2 at level 1", 32, 32, 13, 14, 1, 4, 2, 3, 2, 1},
      {"a 3D texture's slice 3", 8, 8, 10, 13, 1, 4, 1, 2, 3, 0},
      {"a 3D texture's slice 1 at level 1", 8, 8, 10, 13, 1, 4, 1, 3, 1, 1},
  };
  for (const LoadCase& lc : loads) {
    uint32_t user[16] = {};
    TSharp(user, kTex, lc.w, lc.h, lc.type, lc.tile, lc.last, lc.depth);
    VSharp(user + 8, kOut, 16);
    const bool two_d = lc.type == 9;
    user[12] = lc.x;
    user[13] = lc.y;
    user[14] = two_d ? lc.level : lc.slice;
    user[15] = two_d ? 0 : lc.level;
    Wr32(kOut, ~0u);
    Run(kLoadAt, user, 16);
    TextureLayout lay;
    lay.index = lc.tile;
    lay.w = lay.pitch = lc.w;
    lay.h = lc.h;
    lay.depth = lc.type == 10 ? lc.depth : 1;
    lay.slices = lc.type == 13 ? lc.depth : 1;
    Level l;
    LevelOf(lay, lc.level, &l);
    std::vector<uint32_t> texels(uint64_t(l.w) * l.h);
    DetileSlice(l, 4, lc.slice, GPtr(kTex + l.offset), reinterpret_cast<uint8_t*>(texels.data()),
                l.w * 4);
    const uint32_t want = texels[lc.y * l.w + lc.x];
    if (Rd32(kOut) != want) std::printf("  %s: %08X, want %08X\n", lc.what, Rd32(kOut), want);
    Check(Rd32(kOut) == want, lc.what);
  }

  // A cube map of 4x4 texels, face f's texel (x, y) (f + 1) << 8 | y * 4 + x,
  // linear (tile mode 8), sampled (image_sample_lz) where the v_cube*
  // instructions put a direction read from kOut + 16; the texel at kOut.
  static const uint32_t kCube[] = {
      0xE03C0010, 0x80030100,  // buffer_load_dwordx3 v[1:3], off, s[12:15], 0 offset:16
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xD28E0004, 0x040E0501,  // v_cubema_f32 v4, v1, v2, v3
      0xD3540104, 0x00000104,  // v_rcp_f32_e64 v4, |v4|
      0xD28A0005, 0x040E0501,  // v_cubesc_f32 v5, v1, v2, v3
      0xD28C0006, 0x040E0501,  // v_cubetc_f32 v6, v1, v2, v3
      0xD2880007, 0x040E0501,  // v_cubeid_f32 v7, v1, v2, v3
      0x420A0905, 0x3FC00000,  // v_madak_f32 v5, v5, v4, 1.5
      0x420C0906, 0x3FC00000,  // v_madak_f32 v6, v6, v4, 1.5
      0xF09C0F00, 0x00400805,  // image_sample_lz v[8:11], v[5:7], s[0:7], s[8:11] dmask:0xf
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xE0780000, 0x80030800,  // buffer_store_dwordx4 v[8:11], off, s[12:15], 0
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kCubeAt = kCode + 0x400, kCubeTex = kTex + 0x40000;
  std::memcpy(GPtr(kCubeAt), kCube, sizeof kCube);
  {
    TextureLayout lay;
    lay.index = 8;
    lay.w = lay.h = lay.pitch = 4;
    lay.slices = 6;
    Level l;
    LevelOf(lay, 0, &l);
    for (uint32_t f = 0; f < 6; f++)
      for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++)
          Wr32(kCubeTex + f * l.slice_bytes + (y * l.pitch + x) * 4, (f + 1) << 8 | (y * 4 + x));
  }
  struct CubeCase {
    float dir[3];
    uint32_t want;
  } cubes[] = {
      {{1.0f, 0.1f, 0.1f}, 0x100},  {{-1.0f, 0.1f, 0.1f}, 0x200}, {{0.1f, 1.0f, 0.1f}, 0x300},
      {{0.1f, -1.0f, 0.1f}, 0x400}, {{0.1f, 0.1f, 1.0f}, 0x500},  {{0.1f, 0.1f, -1.0f}, 0x600},
      // Inside a face: +X's s runs along -z, t along -y; -Z's s along -x.
      {{1.0f, -0.5f, -0.5f}, 0x10F}, {{0.5f, 0.5f, -1.0f}, 0x605},
  };
  for (const CubeCase& cc : cubes) {
    uint32_t user[16] = {};
    TSharp(user, kCubeTex, 4, 4, 11, 8, 0, 1);
    VSharp(user + 12, kOut, 32);
    std::memcpy(GPtr(kOut + 16), cc.dir, 12);
    Wr32(kOut, ~0u);
    Run(kCubeAt, user, 16);
    // Faces' middles: only which face; elsewhere the texel too.
    const bool middle = !(cc.want & 0xFF);
    const uint32_t got = middle ? Rd32(kOut) & ~0xFFu : Rd32(kOut);
    if (got != cc.want)
      std::printf("  cube (%g, %g, %g): %08X, want %08X\n", cc.dir[0], cc.dir[1], cc.dir[2],
                  Rd32(kOut), cc.want);
    Check(got == cc.want, "a cube map samples the face and texel of a direction");
  }

  // v_sad_u32 on three dwords from kOut + 16, stored at kOut: the
  // difference of unsigned values plus the third, mod 2^32.
  static const uint32_t kSad[] = {
      0xE03C0010, 0x80030100,  // buffer_load_dwordx3 v[1:3], off, s[12:15], 0 offset:16
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xD2BA0004, 0x040E0501,  // v_sad_u32 v4, v1, v2, v3
      0xE0700000, 0x80030400,  // buffer_store_dword v4, off, s[12:15], 0
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kSadAt = kCode + 0xA00;
  std::memcpy(GPtr(kSadAt), kSad, sizeof kSad);
  struct SadCase {
    uint32_t a, b, c, want;
  } sads[] = {
      {5, 0xFFFFFFF0u, 7, 0xFFFFFFF2u},  // unsigned: not |5 - (-16)| + 7
      {3, 10, 0xFFFFFFFEu, 5},           // the sum wraps
  };
  for (const SadCase& sc : sads) {
    uint32_t user[16] = {};
    VSharp(user + 12, kOut, 32);
    Wr32(kOut + 16, sc.a);
    Wr32(kOut + 20, sc.b);
    Wr32(kOut + 24, sc.c);
    Wr32(kOut, 0);
    Run(kSadAt, user, 16);
    if (Rd32(kOut) != sc.want)
      std::printf("  v_sad_u32(%08X, %08X, %08X): %08X, want %08X\n", sc.a, sc.b, sc.c,
                  Rd32(kOut), sc.want);
    Check(Rd32(kOut) == sc.want, "v_sad_u32 adds the unsigned difference");
  }

  // buffer_store_format_xy through a V# of format 32_32 uint: two dwords
  // from kOut + 16 stored at kOut as they are.
  static const uint32_t kStoreXy[] = {
      0xE0340010, 0x80030100,  // buffer_load_dwordx2 v[1:2], off, s[12:15], 0 offset:16
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xE0140000, 0x80030100,  // buffer_store_format_xy v[1:2], off, s[12:15], 0
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kStoreXyAt = kCode + 0xB00;
  std::memcpy(GPtr(kStoreXyAt), kStoreXy, sizeof kStoreXy);
  {
    uint32_t user[16] = {};
    VSharp(user + 12, kOut, 32);
    user[15] = 0xFACu | 4u << 12 | 11u << 15;
    Wr32(kOut + 16, 0x89ABCDEFu);
    Wr32(kOut + 20, 0x7FC00001u);
    Wr64(kOut, 0);
    Run(kStoreXyAt, user, 16);
    Check(Rd32(kOut) == 0x89ABCDEFu && Rd32(kOut + 4) == 0x7FC00001u,
          "buffer_store_format_xy stores 32-bit components as they are");
  }

  // image_gather4_lz of component 0 from an 8x8 texture whose texel (x, y)
  // is 8y + x (linear), at (3/8, 5/8): the 2x2 texels around it, (2, 5),
  // (3, 5), (3, 4), (2, 4) in that order. The coordinates come from
  // kOut + 16.
  static const uint32_t kGather[] = {
      0xE0340010, 0x80030100,  // buffer_load_dwordx2 v[1:2], off, s[12:15], 0 offset:16
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xF11C0100, 0x00400801,  // image_gather4_lz v[8:11], v[1:2], s[0:7], s[8:11] dmask:0x1
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xE0780000, 0x80030800,  // buffer_store_dwordx4 v[8:11], off, s[12:15], 0
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kGatherAt = kCode + 0xC00, kGatherTex = kTex + 0x60000;
  std::memcpy(GPtr(kGatherAt), kGather, sizeof kGather);
  {
    TextureLayout lay;
    lay.index = 8;
    lay.w = lay.h = lay.pitch = 8;
    Level l;
    LevelOf(lay, 0, &l);
    for (uint32_t y = 0; y < 8; y++)
      for (uint32_t x = 0; x < 8; x++) Wr32(kGatherTex + l.offset + 4 * (y * l.pitch + x), 8 * y + x);
    uint32_t user[16] = {};
    TSharp(user, kGatherTex, 8, 8, 9, 8, 0, 1);
    VSharp(user + 12, kOut, 32);
    const float at[2] = {3.0f / 8, 5.0f / 8};
    std::memcpy(GPtr(kOut + 16), at, sizeof at);
    for (uint32_t i = 0; i < 4; i++) Wr32(kOut + 4 * i, ~0u);
    Run(kGatherAt, user, 16);
    const uint32_t want[4] = {42, 43, 35, 34};
    bool ok = true;
    for (uint32_t i = 0; i < 4; i++) ok &= Rd32(kOut + 4 * i) == want[i];
    if (!ok)
      std::printf("  gather: %u %u %u %u\n", Rd32(kOut), Rd32(kOut + 4), Rd32(kOut + 8), Rd32(kOut + 12));
    Check(ok, "image_gather4 returns the 2x2 texels in order");
  }

  // A scalar load from an address summed in two halves (s_add_u32, then
  // s_addc_u32 with its carry): s[0:1] + 0x100 crosses 4 GiB when s0 is
  // 0xFFFFFF00. The load reads the dword stored there by the test.
  static const uint32_t kCarry[] = {
      0x8002FF00, 0x00000100,  // s_add_u32 s2, s0, 0x100
      0x82038001,              // s_addc_u32 s3, s1, 0
      0xC0020300,              // s_load_dword s4, s[2:3], 0x0
      0xBF8C007F,              // s_waitcnt lgkmcnt(0)
      0x7E020204,              // v_mov_b32 v1, s4
      0xE0700000, 0x80030100,  // buffer_store_dword v1, off, s[12:15], 0
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kCarryAt = kCode + 0xD00, kCarryData = 0x100000000ull;
  std::memcpy(GPtr(kCarryAt), kCarry, sizeof kCarry);
  {
    uint32_t user[16] = {0xFFFFFF00u, 0};
    VSharp(user + 12, kOut, 16);
    Wr32(kCarryData, 0x13579BDFu);
    Wr32(kOut, 0);
    Run(kCarryAt, user, 16);
    Check(Rd32(kOut) == 0x13579BDFu, "s_addc_u32 adds s_add_u32's carry");
  }

  // A T# from a table at a run-time offset: entry i (32 bytes) of the
  // table at s[0:1], i read from kOut + 16 by each lane and made scalar
  // (v_readfirstlane), the entry's address summed in halves. The stage
  // also reads the table's word 0x40 at a known offset, which makes the
  // table's range. Entries 0 and 1 are 1x1 textures of 111 and 222, 2
  // and 3 zero (no image).
  static const uint32_t kPick[] = {
      0xE0300010, 0x80030100,  // buffer_load_dword v1, off, s[12:15], 0 offset:16
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0x7E280501,              // v_readfirstlane_b32 s20, v1
      0xC00A8140,              // s_load_dword s21, s[0:1], 0x40
      0x9314A014,              // s_mul_i32 s20, s20, 32
      0x80161400,              // s_add_u32 s22, s0, s20
      0x82178001,              // s_addc_u32 s23, s1, 0
      0xC0CC1700,              // s_load_dwordx8 s[24:31], s[22:23], 0x0
      0xBF8C007F,              // s_waitcnt lgkmcnt(0)
      0x7E040280,              // v_mov_b32 v2, 0
      0x7E060280,              // v_mov_b32 v3, 0
      0xF0000100, 0x00060402,  // image_load v4, v[2:3], s[24:31] dmask:0x1
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xE0700000, 0x80030400,  // buffer_store_dword v4, off, s[12:15], 0
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kPickAt = kCode + 0xE00, kTable = kOut + 0x400, kPickTex = kTex + 0x70000;
  std::memcpy(GPtr(kPickAt), kPick, sizeof kPick);
  {
    std::memset(GPtr(kTable), 0, 0x104);
    for (uint32_t i = 0; i < 2; i++) {
      uint32_t t[8];
      TSharp(t, kPickTex + 0x1000 * i, 1, 1, 9, 8, 0, 1);
      std::memcpy(GPtr(kTable + 32 * i), t, sizeof t);
      Wr32(kPickTex + 0x1000 * i, 111 * (i + 1));
    }
    for (uint32_t i = 0; i < 2; i++) {
      uint32_t user[16] = {uint32_t(kTable), uint32_t(kTable >> 32)};
      VSharp(user + 12, kOut, 32);
      Wr32(kOut + 16, i);
      Wr32(kOut, 0);
      Run(kPickAt, user, 16);
      if (Rd32(kOut) != 111 * (i + 1)) std::printf("  entry %u: %u\n", i, Rd32(kOut));
      Check(Rd32(kOut) == 111 * (i + 1), "a T# loaded at a run-time offset picks its texture");
    }
  }

  // ds_swizzle_b32 in bit-mask mode across quads (lane ^ 4) over a wave of
  // 64: lane i stores lane i ^ 4's id (v0) at kOut + 0x200 + 4i.
  static const uint32_t kSwz[] = {
      0xD8D4101F, 0x01000000,  // ds_swizzle_b32 v1, v0 offset:swizzle(BITMASK_PERM xor 4)
      0xBF8C007F,              // s_waitcnt lgkmcnt(0)
      0x34040082,              // v_lshlrev_b32 v2, 2, v0
      0xE0701000, 0x80000102,  // buffer_store_dword v1, v2, s[0:3], 0 offen
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kSwzAt = kCode + 0xF00;
  std::memcpy(GPtr(kSwzAt), kSwz, sizeof kSwz);
  {
    uint32_t user[16] = {};
    VSharp(user, kOut + 0x200, 256);
    std::memset(GPtr(kOut + 0x200), 0xFF, 256);
    Run(kSwzAt, user, 4, 64);
    bool ok = true;
    for (uint32_t i = 0; i < 64; i++) ok &= Rd32(kOut + 0x200 + 4 * i) == (i ^ 4);
    if (!ok) std::printf("  lane 0..7: %d %d %d %d %d %d %d %d\n", int(Rd32(kOut + 0x200)), int(Rd32(kOut + 0x204)),
                         int(Rd32(kOut + 0x208)), int(Rd32(kOut + 0x20C)), int(Rd32(kOut + 0x210)),
                         int(Rd32(kOut + 0x214)), int(Rd32(kOut + 0x218)), int(Rd32(kOut + 0x21C)));
    Check(ok, "ds_swizzle_b32's bit-mask mode reads the lane across quads");
  }

  // s_not_b64 on a pair whose result is zero only in its low half and on
  // all ones; SCC picks 1 or 2 after each. Stores s[6:7], s8, s9 and s[12:13] at kOut + 0x300.
  static const uint32_t kNot[] = {
      0xBE8403C1,              // s_mov_b32 s4, -1
      0xBE8503FF, 0x0F0F00FF,  // s_mov_b32 s5, 0x0f0f00ff
      0xBE860804,              // s_not_b64 s[6:7], s[4:5]
      0x85088281,              // s_cselect_b32 s8, 1, 2
      0xBE8A04C1,              // s_mov_b64 s[10:11], -1
      0xBE8C080A,              // s_not_b64 s[12:13], s[10:11]
      0x85098281,              // s_cselect_b32 s9, 1, 2
      0x7E020206,              // v_mov_b32 v1, s6
      0x7E040207,              // v_mov_b32 v2, s7
      0x7E060208,              // v_mov_b32 v3, s8
      0x7E080209,              // v_mov_b32 v4, s9
      0x7E0A020C,              // v_mov_b32 v5, s12
      0x7E0C020D,              // v_mov_b32 v6, s13
      0xE0781000, 0x80000100,  // buffer_store_dwordx4 v[1:4], v0, s[0:3], 0 offen
      0xE0741010, 0x80000500,  // buffer_store_dwordx2 v[5:6], v0, s[0:3], 0 offen offset:16
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kNotAt = kCode + 0x1400;
  std::memcpy(GPtr(kNotAt), kNot, sizeof kNot);
  {
    uint32_t user[16] = {};
    VSharp(user, kOut + 0x300, 24);
    std::memset(GPtr(kOut + 0x300), 0xEE, 24);
    Run(kNotAt, user, 4, 1, 1);  // 8 VGPRs
    const uint32_t want[6] = {0, 0xF0F0FF00u, 1, 2, 0, 0};
    bool ok = true;
    for (uint32_t k = 0; k < 6; k++) {
      if (Rd32(kOut + 0x300 + 4 * k) != want[k]) std::printf("  word %u: %08X\n", k, Rd32(kOut + 0x300 + 4 * k));
      ok &= Rd32(kOut + 0x300 + 4 * k) == want[k];
    }
    Check(ok, "s_not_b64 inverts both halves and sets SCC from both");
  }

  // s_nand, s_nor, s_xnor in 32 and 64 bits on s[4:5] = {0x0F0F00FF, -1}
  // and s[6:7] = {0x00FF00FF, -1}, then SCC after a 64-bit result of zero
  // (s17) and one zero only in its low half (s22). Stores s8 to s22 and
  // s22 again at kOut + 0x5200.
  static const uint32_t kNand[] = {
      0xBE8403FF, 0x0F0F00FF,  // s_mov_b32 s4, 0x0f0f00ff
      0xBE8503C1,              // s_mov_b32 s5, -1
      0xBE8603FF, 0x00FF00FF,  // s_mov_b32 s6, 0x00ff00ff
      0xBE8703C1,              // s_mov_b32 s7, -1
      0x8C880604,              // s_nand_b64 s[8:9], s[4:5], s[6:7]
      0x8D8A0604,              // s_nor_b64 s[10:11], s[4:5], s[6:7]
      0x8E8C0604,              // s_xnor_b64 s[12:13], s[4:5], s[6:7]
      0x8C0E0604,              // s_nand_b32 s14, s4, s6
      0x8D0F0604,              // s_nor_b32 s15, s4, s6
      0x8E100604,              // s_xnor_b32 s16, s4, s6
      0x8D92C104,              // s_nor_b64 s[18:19], s[4:5], -1
      0x85118281,              // s_cselect_b32 s17, 1, 2
      0xBE9803C1,              // s_mov_b32 s24, -1
      0xBE990380,              // s_mov_b32 s25, 0
      0x8C941818,              // s_nand_b64 s[20:21], s[24:25], s[24:25]
      0x85168281,              // s_cselect_b32 s22, 1, 2
      0x7E020208, 0x7E040209, 0x7E06020A, 0x7E08020B,  // v_mov_b32 v1-v4, s8-s11
      0xE0781000, 0x80000100,                          // buffer_store_dwordx4 v[1:4], v0, s[0:3], 0 offen
      0xBF8C0000,                                      // s_waitcnt 0
      0x7E02020C, 0x7E04020D, 0x7E06020E, 0x7E08020F,  // v_mov_b32 v1-v4, s12-s15
      0xE0781010, 0x80000100,                          // ... offset:16
      0xBF8C0000,                                      // s_waitcnt 0
      0x7E020210, 0x7E040211, 0x7E060212, 0x7E080213,  // v_mov_b32 v1-v4, s16-s19
      0xE0781020, 0x80000100,                          // ... offset:32
      0xBF8C0000,                                      // s_waitcnt 0
      0x7E020214, 0x7E040215, 0x7E060216, 0x7E080216,  // v_mov_b32 v1-v4, s20-s22, s22
      0xE0781030, 0x80000100,                          // ... offset:48
      0xBF810000,                                      // s_endpgm
  };
  constexpr gaddr kNandAt = kCode + 0x1600;
  std::memcpy(GPtr(kNandAt), kNand, sizeof kNand);
  {
    uint32_t user[16] = {};
    VSharp(user, kOut + 0x5200, 64);
    std::memset(GPtr(kOut + 0x5200), 0xEE, 64);
    Run(kNandAt, user, 4, 1, 1);
    const uint32_t want[16] = {0xFFF0FF00u, 0, 0xF000FF00u, 0, 0xF00FFFFFu, 0xFFFFFFFFu, 0xFFF0FF00u, 0xF000FF00u,
                               0xF00FFFFFu, 2, 0, 0, 0, 0xFFFFFFFFu, 1, 1};
    bool ok = true;
    for (uint32_t k = 0; k < 16; k++) {
      if (Rd32(kOut + 0x5200 + 4 * k) != want[k]) std::printf("  word %u: %08X\n", k, Rd32(kOut + 0x5200 + 4 * k));
      ok &= Rd32(kOut + 0x5200 + 4 * k) == want[k];
    }
    Check(ok, "s_nand, s_nor and s_xnor invert their op in 32 and 64 bits and set SCC from both halves");
  }

  // v_mbcnt_lo/hi_u32_b32 over a wave: lane i stores mbcnt(-1) (i),
  // mbcnt of s[4:5] = 0x0000FFFF:AAAAAAAA plus 5 and mbcnt(EXEC) at
  // kOut + 0x5400 + 16i; run on 64 and 40 invocations.
  {
    static const uint32_t kMbcnt[] = {
        0xBE8403FF, 0xAAAAAAAA,  // s_mov_b32 s4, 0xaaaaaaaa
        0xBE8503FF, 0x0000FFFF,  // s_mov_b32 s5, 0xffff
        0xD2460001, 0x000100C1,  // v_mbcnt_lo_u32_b32 v1, -1, 0
        0xD2480001, 0x000202C1,  // v_mbcnt_hi_u32_b32 v1, -1, v1
        0xD2460002, 0x00010A04,  // v_mbcnt_lo_u32_b32 v2, s4, 5
        0xD2480002, 0x00020405,  // v_mbcnt_hi_u32_b32 v2, s5, v2
        0xD2460005, 0x0001007E,  // v_mbcnt_lo_u32_b32 v5, exec_lo, 0
        0xD2480005, 0x00020A7F,  // v_mbcnt_hi_u32_b32 v5, exec_hi, v5
        0x34060084,              // v_lshlrev_b32 v3, 4, v0
        0xE0741000, 0x80000103,  // buffer_store_dwordx2 v[1:2], v3, s[0:3], 0 offen
        0xE0701008, 0x80000503,  // buffer_store_dword v5, v3, s[0:3], 0 offen offset:8
        0xBF810000,              // s_endpgm
    };
    constexpr gaddr kAt = kCode + 0x1700, kTo = kOut + 0x5400;
    std::memcpy(GPtr(kAt), kMbcnt, sizeof kMbcnt);
    for (uint32_t threads : {64u, 40u}) {
      uint32_t user[16] = {};
      VSharp(user, kTo, 64 * 16);
      std::memset(GPtr(kTo), 0xEE, 64 * 16);
      Run(kAt, user, 4, threads, 1);
      bool ok = true;
      for (uint32_t i = 0; i < 64; i++) {
        const uint32_t want[3] = {i, (i < 32 ? i / 2 : 16 + std::min(i - 32, 16u)) + 5, i};
        for (uint32_t k = 0; k < 3; k++) {
          const uint32_t got = Rd32(kTo + 16 * i + 4 * k), w = i < threads ? want[k] : 0xEEEEEEEEu;
          if (got != w && ok) std::printf("  %u invocations: lane %u word %u: %08X, not %08X\n", threads, i, k, got, w);
          ok &= got == w;
        }
      }
      Check(ok, "v_mbcnt_lo/hi count a mask's bits below each lane of a wave");
    }
  }

  // ds_append on the GDS: M0 = size 0x10 at base 0x100, the counter at
  // GDS 0x104 from 1000; each lane stores the counter's value before its
  // wave's append plus its mbcnt(EXEC) at kOut + 0x5800 + 4i. Two waves
  // (128 invocations) take 1000-1127 between them, then 40 lanes
  // 1128-1167; the GDS holds 1168.
  {
    static const uint32_t kAppend[] = {
        0xBEFC03FF, 0x00100100,  // s_mov_b32 m0, 0x00100100
        0xD2460001, 0x0001007E,  // v_mbcnt_lo_u32_b32 v1, exec_lo, 0
        0xD2480001, 0x0002027F,  // v_mbcnt_hi_u32_b32 v1, exec_hi, v1
        0xD8FA0004, 0x02000000,  // ds_append v2 offset:4 gds
        0xBF8C007F,              // s_waitcnt lgkmcnt(0)
        0x4A040302,              // v_add_i32 v2, vcc, v2, v1
        0x34060082,              // v_lshlrev_b32 v3, 2, v0
        0xE0701000, 0x80000203,  // buffer_store_dword v2, v3, s[0:3], 0 offen
        0xBF810000,              // s_endpgm
    };
    constexpr gaddr kAt = kCode + 0x1800, kTo = kOut + 0x5800;
    std::memcpy(GPtr(kAt), kAppend, sizeof kAppend);
    const uint32_t start = 1000;
    std::memcpy(Gds().bytes + 0x104, &start, 4);
    bool ok = true;
    uint32_t first = start;
    for (uint32_t threads : {128u, 40u}) {
      uint32_t user[16] = {};
      VSharp(user, kTo, 128 * 4);
      std::memset(GPtr(kTo), 0xEE, 128 * 4);
      Run(kAt, user, 4, threads, 1);
      std::vector<bool> seen(threads);
      for (uint32_t i = 0; i < threads; i++) {
        const uint32_t v = Rd32(kTo + 4 * i) - first;
        if ((v >= threads || seen[v]) && ok) std::printf("  %u invocations: lane %u: %u\n", threads, i, v + first);
        ok &= v < threads && !seen[v];
        if (v < threads) seen[v] = true;
      }
      first += threads;
    }
    uint32_t end;
    std::memcpy(&end, Gds().bytes + 0x104, 4);
    if (end != first) std::printf("  GDS counter %u, not %u\n", end, first);
    Check(ok && end == first, "ds_append hands each wave its own run of a GDS counter");
  }

  // ds_write_b64 and ds_read_b64: lane i writes (i, i + 100) at LDS
  // 16 + 8i, then reads lane i ^ 1's pair and stores it at
  // kOut + 0x5A00 + 8i (64 invocations).
  {
    static const uint32_t kLds64[] = {
        0x7E020300,              // v_mov_b32 v1, v0
        0x4A0400FF, 0x00000064,  // v_add_i32 v2, vcc, 0x64, v0
        0x34060083,              // v_lshlrev_b32 v3, 3, v0
        0xD9340010, 0x00000103,  // ds_write_b64 v3, v[1:2] offset:16
        0xBF8C007F,              // s_waitcnt lgkmcnt(0)
        0xBF8A0000,              // s_barrier
        0x3A080081,              // v_xor_b32 v4, 1, v0
        0x34080883,              // v_lshlrev_b32 v4, 3, v4
        0xD9D80010, 0x05000004,  // ds_read_b64 v[5:6], v4 offset:16
        0xBF8C007F,              // s_waitcnt lgkmcnt(0)
        0xE0741000, 0x80000503,  // buffer_store_dwordx2 v[5:6], v3, s[0:3], 0 offen
        0xBF810000,              // s_endpgm
    };
    constexpr gaddr kAt = kCode + 0x1900, kTo = kOut + 0x5A00;
    std::memcpy(GPtr(kAt), kLds64, sizeof kLds64);
    uint32_t user[16] = {};
    VSharp(user, kTo, 64 * 8);
    std::memset(GPtr(kTo), 0xEE, 64 * 8);
    // 8 VGPRs (rsrc1), 256 dwords of LDS (rsrc2, in 128-dword units).
    Run(kAt, user, 4, 64, 1, 2u << 15);
    bool ok = true;
    for (uint32_t i = 0; i < 64; i++) {
      const uint32_t want[2] = {i ^ 1, (i ^ 1) + 100};
      for (uint32_t k = 0; k < 2; k++) {
        const uint32_t got = Rd32(kTo + 8 * i + 4 * k);
        if (got != want[k] && ok) std::printf("  lane %u word %u: %08X, not %08X\n", i, k, got, want[k]);
        ok &= got == want[k];
      }
    }
    Check(ok, "ds_write_b64 and ds_read_b64 move a lane's pair through the LDS");
  }

  // A stage in waves: readlane, readfirstlane under a partial EXEC,
  // writelane, comparisons and carries as ballots, a branch on the wave's
  // VCC, v_movrels (past the VGPRs too), ds_or_b32 and ds_read2_b64. Lane i stores 20 words at
  // kOut + 0x2000 + 128i (kWaveWant); run on 64 and 40 invocations.
  static const uint32_t kWave[] = {
      0xBE94047E,  // s_mov_b64 s[20:21], exec
      0x02094B00,  // v_readlane_b32 s4, v0, 37
      0x020B0B00,  // v_readlane_b32 s5, v0, 5
      0x7E0202A8,  // v_mov_b32 v1, 40
      0x7D820300,  // v_cmp_lt_u32 vcc, v0, v1
      0xBE86046A,  // s_mov_b64 s[6:7], vcc
      0x7DA600A3,  // v_cmpx_le_u32 vcc, 35, v0
      0xBE88047E,  // s_mov_b64 s[8:9], exec
      0x4A0400FF, 0x00000064,  // v_add_i32 v2, vcc, 0x64, v0
      0x7E140502,  // v_readfirstlane_b32 s10, v2
      0xBEFE0414,  // s_mov_b64 exec, s[20:21]
      0x7E060280,  // v_mov_b32 v3, 0
      0x04076404,  // v_writelane_b32 v3, s4, 50
      0xBEFC0381,  // s_mov_b32 m0, 1
      0x7E140300,  // v_mov_b32 v10, v0
      0x34160081,  // v_lshlrev_b32 v11, 1, v0
      0x7E18870A,  // v_movrels_b32 v12, v10
      0x7E1C02FF, 0xFFFFFFE0,  // v_mov_b32 v14, 0xffffffe0
      0x4A1A1D00,  // v_add_i32 v13, vcc, v0, v14
      0xBE8C046A,  // s_mov_b64 s[12:13], vcc
      0x7E220281,  // v_mov_b32 v17, 1
      0x00202280,  // v_cndmask_b32 v16, 0, v17, vcc
      0x7E1E0281,  // v_mov_b32 v15, 1
      0xBF860001,  // s_cbranch_vccz 1 (past the next)
      0x7E1E0287,  // v_mov_b32 v15, 7
      0x7E240280,  // v_mov_b32 v18, 0
      0x342C0082,  // v_lshlrev_b32 v22, 2, v0
      0xD8340000, 0x00000016,  // ds_write_b32 v22, v0
      0xD8340100, 0x00001212,  // ds_write_b32 v18, v18 offset:256
      0xBF8C007F,  // s_waitcnt lgkmcnt(0)
      0xBF8A0000,  // s_barrier
      0x3626009F,  // v_and_b32 v19, 31, v0
      0x7E280281,  // v_mov_b32 v20, 1
      0x34262913,  // v_lshlrev_b32 v19, v19, v20
      0xD8280100, 0x00001312,  // ds_or_b32 v18, v19 offset:256
      0xBF8C007F,  // s_waitcnt lgkmcnt(0)
      0xBF8A0000,  // s_barrier
      0xD8D80100, 0x15000012,  // ds_read_b32 v21, v18 offset:256
      0xD9DC0301, 0x16000012,  // ds_read2_b64 v[22:25], v18 offset0:1 offset1:3
      0xBF8C007F,  // s_waitcnt lgkmcnt(0)
      0x343C0087,  // v_lshlrev_b32 v30, 7, v0
      0x7E500204,  // v_mov_b32 v40, s4
      0x7E520205,  // v_mov_b32 v41, s5
      0x7E540206,  // v_mov_b32 v42, s6
      0x7E560207,  // v_mov_b32 v43, s7
      0x7E580208,  // v_mov_b32 v44, s8
      0x7E5A0209,  // v_mov_b32 v45, s9
      0x7E5C020A,  // v_mov_b32 v46, s10
      0x7E5E0303,  // v_mov_b32 v47, v3
      0x7E60030C,  // v_mov_b32 v48, v12
      0x7E62020C,  // v_mov_b32 v49, s12
      0x7E64020D,  // v_mov_b32 v50, s13
      0x7E66030F,  // v_mov_b32 v51, v15
      0x7E680310,  // v_mov_b32 v52, v16
      0x7E6A0315,  // v_mov_b32 v53, v21
      0x7E6C0316,  // v_mov_b32 v54, v22
      0x7E6E0319,  // v_mov_b32 v55, v25
      0x7E700214,  // v_mov_b32 v56, s20
      0x7E720215,  // v_mov_b32 v57, s21
      0x7E740300,  // v_mov_b32 v58, v0
      0xBEFC03FF, 0x00000064,  // s_mov_b32 m0, 100
      0x7E3E870A,  // v_movrels_b32 v31, v10 (past the VGPRs: v0)
      0x7E400285,  // v_mov_b32 v32, 5
      0x7E288520,  // v_movreld_b32 v20, v32 (past the VGPRs: not written)
      0x7E76031F,  // v_mov_b32 v59, v31
      0xE0781000, 0x8000281E,  // buffer_store_dwordx4 v[40:43], v30, s[0:3], 0 offen
      0xE0781010, 0x80002C1E,  // buffer_store_dwordx4 v[44:47], v30, s[0:3], 0 offen offset:16
      0xE0781020, 0x8000301E,  // buffer_store_dwordx4 v[48:51], v30, s[0:3], 0 offen offset:32
      0xE0781030, 0x8000341E,  // buffer_store_dwordx4 v[52:55], v30, s[0:3], 0 offen offset:48
      0xE0781040, 0x8000381E,  // buffer_store_dwordx4 v[56:59], v30, s[0:3], 0 offen offset:64
      0xBF810000,  // s_endpgm
  };
  constexpr gaddr kWaveAt = kCode + 0x1000, kWaveOut = kOut + 0x2000;
  std::memcpy(GPtr(kWaveAt), kWave, sizeof kWave);
  for (uint32_t threads : {64u, 40u}) {
    const uint32_t hi = threads == 64 ? ~0u : 0xFFu;  // the upper half's lanes
    uint32_t user[16] = {};
    VSharp(user, kWaveOut, 64 * 128);
    std::memset(GPtr(kWaveOut), 0xEE, 64 * 128);
    // 64 VGPRs (rsrc1), 128 dwords of LDS (rsrc2).
    Run(kWaveAt, user, 4, threads, 15, 1u << 15);
    bool ok = true;
    for (uint32_t i = 0; i < 64; i++) {
      const uint32_t want[20] = {37, 5, ~0u, 0xFF, 0, hi & 0xFFFFFFF8u, 135, i == 50 ? 37u : 0u,
                                 2 * i, 0, hi, 7, i >= 32 ? 1u : 0u, ~0u, 2, 7, ~0u, hi, i, i};
      for (uint32_t k = 0; k < 20; k++) {
        const uint32_t got = Rd32(kWaveOut + 128 * i + 4 * k), w = i < threads ? want[k] : 0xEEEEEEEEu;
        if (got != w && ok) std::printf("  %u invocations: lane %u word %u: %08X, not %08X\n", threads, i, k, got, w);
        ok &= got == w;
      }
    }
    Check(ok, "a stage in waves reads, writes and ballots across its lanes");
  }

  // Buffer loads at a 64-bit address (ADDR64): lane i reads word i + 1 of
  // the table at s[0:1] through VGPRs that add i * 4 to the pointer; the
  // stage reads word 4 at a known offset, which makes the range.
  static const uint32_t kWide[] = {
      0xC0010104,              // s_load_dword s2, s[0:1], 0x4
      0x34020082,              // v_lshlrev_b32 v1, 2, v0
      0x4A040200,              // v_add_i32 v2, vcc, s0, v1
      0x7E060201,              // v_mov_b32 v3, s1
      0x50060680,              // v_addc_u32 v3, vcc, 0, v3, vcc
      0xBE840380,              // s_mov_b32 s4, 0
      0xBE8503FF, 0x00040000,  // s_mov_b32 s5, 0x40000
      0xBE8603C1,              // s_mov_b32 s6, -1
      0xBE8703FF, 0x20024004,  // s_mov_b32 s7, 0x20024004
      0xE0308004, 0x80010402,  // buffer_load_dword v4, v[2:3], s[4:7], 0 addr64 offset:4
      0xBF8C0070,              // s_waitcnt vmcnt(0) lgkmcnt(0)
      0x4A080802,              // v_add_i32 v4, vcc, s2, v4
      0xE0701000, 0x80020401,  // buffer_store_dword v4, v1, s[8:11], 0 offen
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kWideAt = kCode + 0x1200, kWideTable = kOut + 0x600;
  std::memcpy(GPtr(kWideAt), kWide, sizeof kWide);
  {
    for (uint32_t i = 0; i < 5; i++) Wr32(kWideTable + 4 * i, 100 * i + 1);
    uint32_t user[16] = {uint32_t(kWideTable), uint32_t(kWideTable >> 32)};
    VSharp(user + 8, kOut, 16);
    std::memset(GPtr(kOut), 0, 16);
    Run(kWideAt, user, 12, 4);
    bool ok = true;
    for (uint32_t i = 0; i < 4; i++) ok &= Rd32(kOut + 4 * i) == 401 + 100 * (i + 1) + 1;
    if (!ok) std::printf("  lanes: %u %u %u %u\n", Rd32(kOut), Rd32(kOut + 4), Rd32(kOut + 8), Rd32(kOut + 12));
    Check(ok, "buffer loads at a 64-bit address read the pointer's range");
  }

  // An 8x8 texture of four levels, level n's texels n + 1 (linear), sampled
  // at (0.5, 0.5) with derivatives (image_sample_d) from kOut + 16, nearest
  // level (MIP_FILTER point, MAX_LOD 4): derivatives of 4 texels a pixel
  // select level 2, of one level 0.
  static const uint32_t kGrad[] = {
      0xE0380010, 0x80030100,  // buffer_load_dwordx4 v[1:4], off, s[12:15], 0 offset:16
      0xE0340020, 0x80030500,  // buffer_load_dwordx2 v[5:6], off, s[12:15], 0 offset:32
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xF0880F00, 0x00400801,  // image_sample_d v[8:11], v[1:6], s[0:7], s[8:11] dmask:0xf
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xE0780000, 0x80030800,  // buffer_store_dwordx4 v[8:11], off, s[12:15], 0
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kGradAt = kCode + 0x500, kGradTex = kTex + 0x50000;
  std::memcpy(GPtr(kGradAt), kGrad, sizeof kGrad);
  {
    TextureLayout lay;
    lay.index = 8;
    lay.w = lay.h = lay.pitch = 8;
    for (uint32_t i = 0; i < 4; i++) {
      Level l;
      LevelOf(lay, i, &l);
      for (uint64_t b = 0; b < l.slice_bytes; b += 4) Wr32(kGradTex + l.offset + b, i + 1);
    }
  }
  for (const auto& [d, want] : {std::pair{0.5f, 3u}, std::pair{0.125f, 1u}}) {
    uint32_t user[16] = {};
    TSharp(user, kGradTex, 8, 8, 9, 8, 3, 1);
    user[9] = 4u << 20;   // S# MAX_LOD 4.0
    user[10] = 1u << 26;  // MIP_FILTER point
    VSharp(user + 12, kOut, 48);
    const float args[6] = {d, 0.0f, 0.0f, d, 0.5f, 0.5f};
    std::memcpy(GPtr(kOut + 16), args, sizeof args);
    Wr32(kOut, ~0u);
    Run(kGradAt, user, 16);
    if (Rd32(kOut) != want) std::printf("  derivatives %g: level %d\n", d, int(Rd32(kOut)) - 1);
    Check(Rd32(kOut) == want, "derivatives select the level");
  }

  // An indirect dispatch through the command processor: SET_BASE, the
  // compute registers, DISPATCH_INDIRECT of the groups at the base. Each
  // group stores its id at its place in kOut.
  static const uint32_t kGroup[] = {
      0x7E020204,              // v_mov_b32 v1, s4
      0x34040282,              // v_lshlrev_b32 v2, 2, v1
      0xE0701000, 0x80000102,  // buffer_store_dword v1, v2, s[0:3], 0 offen
      0xBF810000,              // s_endpgm
  };
  constexpr gaddr kGroupAt = kCode + 0x600, kArgs = kOut + 0x100, kDcb = kOut + 0x200;
  std::memcpy(GPtr(kGroupAt), kGroup, sizeof kGroup);
  {
    const uint32_t groups[3] = {3, 1, 1};
    std::memcpy(GPtr(kArgs), groups, sizeof groups);
    uint32_t v[4];
    VSharp(v, kOut, 16);
    const uint32_t dcb[] = {
        0xC0021100, 1, uint32_t(kArgs), 0,                        // SET_BASE 1
        0xC0027602, 0x20C, uint32_t(kGroupAt >> 8), 0,            // COMPUTE_PGM_LO
        0xC0027602, 0x212, 0, 4u << 1 | 1u << 7,                  // RSRC1, RSRC2 (TGID_X)
        0xC0037602, 0x207, 1, 1, 1,                               // NUM_THREAD_X-Z
        0xC0047602, 0x240, v[0], v[1], v[2], v[3],                // USER_DATA_0-3
        0xC0011602, 0, 1,                                         // DISPATCH_INDIRECT
    };
    std::memcpy(GPtr(kDcb), dcb, sizeof dcb);
    for (int i = 0; i < 4; i++) Wr32(kOut + 4 * i, ~0u);
    std::atomic<bool> done{false};
    SubmitGraphics({{kDcb, uint32_t(sizeof dcb / 4)}}, {{0, 0}}, [&] { done = true; });
    for (int i = 0; i < 5000 && !done; i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Settle(true);
    Check(done && Rd32(kOut) == 0 && Rd32(kOut + 4) == 1 && Rd32(kOut + 8) == 2 &&
              Rd32(kOut + 12) == ~0u,
          "an indirect dispatch runs the groups at its base");
  }

  // Draws.
  static const uint32_t kVs[] = {
      0xE0382000, 0x80000100,  // buffer_load_dwordx4 v[1:4], v0, s[0:3], 0 idxen
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xF80008CF, 0x04030201,  // exp pos0 v1, v2, v3, v4 done
      0xF800020F, 0x04030201,  // exp param0 v1, v2, v3, v4
      0xBF810000,              // s_endpgm
  };
  static const uint32_t kPs[] = {
      0xC8080000,              // v_interp_p1_f32 v2, v0, attr0.x
      0xC8090001,              // v_interp_p2_f32 v2, v1, attr0.x
      0xC80C0100,              // v_interp_p1_f32 v3, v0, attr0.y
      0xC80D0101,              // v_interp_p2_f32 v3, v1, attr0.y
      0x7E080280,              // v_mov_b32 v4, 0
      0x7E0A02F2,              // v_mov_b32 v5, 1.0
      0xF800180F, 0x05040302,  // exp mrt0 v2, v3, v4, v5 done vm
      0xBF810000,              // s_endpgm
  };
  std::memcpy(GPtr(kVsAt), kVs, sizeof kVs);
  std::memcpy(GPtr(kPsAt), kPs, sizeof kPs);
  auto verts = [](std::initializer_list<std::pair<float, float>> xy) {
    gaddr a = kVerts;
    for (auto [x, y] : xy) {
      const float v[4] = {x, y, 0.5f, 1.0f};
      std::memcpy(GPtr(a), v, sizeof v);
      a += 16;
    }
  };
  // A rectangle from three corners: the fourth (and its parameter) derived.
  verts({{-1, -1}, {1, -1}, {-1, 1}});
  Check(DrawCovers(0x11, 3, 0x1B0000, dev.get()), "a rectangle list covers its rectangles");
  // A fan round the centre, a quad.
  verts({{0, 0}, {-1, -1}, {1, -1}, {1, 1}, {-1, 1}, {-1, -1}});
  Check(DrawCovers(5, 6, 0x1B1000, dev.get()), "a triangle fan covers its triangles");
  verts({{-1, -1}, {1, -1}, {1, 1}, {-1, 1}});
  Check(DrawCovers(0x13, 4, 0x1B2000, dev.get()), "a quad list covers its quads");
  // s_wqm_b64 exec, exec in a pixel shader after v_cmpx turns off column
  // 0 (clip x -0.875): its quads' column 1 turns it back on, and each
  // invocation's EXEC is its own lane, so 1 (lo) and 0 (hi) everywhere,
  // written as red and green.
  {
    static const uint32_t kWqmPs[] = {
        0xC8080000,              // v_interp_p1_f32 v2, v0, attr0.x
        0xC8090001,              // v_interp_p2_f32 v2, v1, attr0.x
        0x7C2204FF, 0xBF400000,  // v_cmpx_lt_f32 vcc, -0.75, v2
        0xBEFE0A7E,              // s_wqm_b64 exec, exec
        0x7E04027E,              // v_mov_b32 v2, exec_lo
        0x7E040D02,              // v_cvt_f32_u32 v2, v2
        0x7E06027F,              // v_mov_b32 v3, exec_hi
        0x7E060D03,              // v_cvt_f32_u32 v3, v3
        0x7E080280,              // v_mov_b32 v4, 0
        0x7E0A02F2,              // v_mov_b32 v5, 1.0
        0xF800180F, 0x05040302,  // exp mrt0 v2, v3, v4, v5 done vm
        0xBF810000,              // s_endpgm
    };
    constexpr gaddr kWqmPsAt = kCode + 0x1500, kWqmTarget = 0x1BE000;
    std::memcpy(GPtr(kWqmPsAt), kWqmPs, sizeof kWqmPs);
    verts({{-1, -1}, {1, -1}, {-1, 1}});
    Regs r;
    DrawRegs(r, 0x11, kWqmTarget, kVsAt);
    r.sh[0x08] = uint32_t(kWqmPsAt >> 8);
    Draw(r, {3, 1, 0, 2});
    Settle(true);
    float px[8 * 8 * 4] = {};
    const gfx::TextureId tex = TargetAt(kWqmTarget, 8, 8);
    if (tex) dev->ReadImage(tex, 0, 0, {0, 0, 8, 8}, px, 8 * 16);
    bool ok = tex;
    for (int i = 0; i < 64 && ok; i++)
      if (px[4 * i] != 1.0f || px[4 * i + 1] != 0.0f) {
        std::printf("  pixel %d: EXEC %g %g\n", i, px[4 * i], px[4 * i + 1]);
        ok = false;
      }
    Check(ok, "s_wqm_b64 in a pixel shader sets a lane of a quad with any lane set, and only it");
  }
  // A buffer load at a 64-bit address in a pixel shader, its lane offset
  // 0 or 16 (x > 0: the right half), so its reach known: words 1-4 or 5-8
  // of the table at s[0:1] (word i holding i), loaded over the address.
  {
    static const uint32_t kWidePs[] = {
        0xC8080000,              // v_interp_p1_f32 v2, v0, attr0.x
        0xC8090001,              // v_interp_p2_f32 v2, v1, attr0.x
        0x7C020480,              // v_cmp_lt_f32 vcc, 0, v2
        0xD2000006, 0x01A90280,  // v_cndmask_b32 v6, 0, 1, vcc
        0xD2D20006, 0x00020C90,  // v_mul_lo_u32 v6, 16, v6
        0x4A0E0C00,              // v_add_i32 v7, vcc, s0, v6
        0x7E100201,              // v_mov_b32 v8, s1
        0x50101080,              // v_addc_u32 v8, vcc, 0, v8, vcc
        0xBE840380,              // s_mov_b32 s4, 0
        0xBE8503FF, 0x00040000,  // s_mov_b32 s5, 0x40000
        0xBE8603C1,              // s_mov_b32 s6, -1
        0xBE8703FF, 0x20024004,  // s_mov_b32 s7, 0x20024004
        0xE0388004, 0x80010707,  // buffer_load_dwordx4 v[7:10], v[7:8], s[4:7], 0 addr64 offset:4
        0xBF8C0F70,              // s_waitcnt vmcnt(0)
        0xF800180F, 0x0A090807,  // exp mrt0 v7, v8, v9, v10 done vm
        0xBF810000,              // s_endpgm
    };
    constexpr gaddr kWidePsAt = kCode + 0x1A00, kWideTarget = 0x1C0000, kTable = kOut + 0x7000;
    std::memcpy(GPtr(kWidePsAt), kWidePs, sizeof kWidePs);
    for (uint32_t i = 0; i < 9; i++) {
      const float f = float(i);
      std::memcpy(GPtr(kTable + 4 * i), &f, 4);
    }
    verts({{-1, -1}, {1, -1}, {-1, 1}});
    Regs r;
    DrawRegs(r, 0x11, kWideTarget, kVsAt);
    r.sh[0x08] = uint32_t(kWidePsAt >> 8);
    r.sh[0x0B] = 2u << 1;  // SPI_SHADER_PGM_RSRC2_PS: 2 user SGPRs
    r.sh[0x0C] = uint32_t(kTable);
    r.sh[0x0D] = uint32_t(kTable >> 32);
    Draw(r, {3, 1, 0, 2});
    Settle(true);
    float px[8 * 8 * 4] = {};
    const gfx::TextureId tex = TargetAt(kWideTarget, 8, 8);
    if (tex) dev->ReadImage(tex, 0, 0, {0, 0, 8, 8}, px, 8 * 16);
    bool ok = tex;
    for (int i = 0; i < 64 && ok; i++) {
      const float first = i % 8 < 4 ? 1.0f : 5.0f;
      for (int c = 0; c < 4; c++) ok &= px[4 * i + c] == first + float(c);
      if (!ok) std::printf("  pixel %d: %g %g %g %g\n", i, px[4 * i], px[4 * i + 1], px[4 * i + 2], px[4 * i + 3]);
    }
    Check(ok, "a pixel shader's buffer load at a 64-bit address reads what its lane offset reaches");
  }
  // Levels 0 and 1 of an 8x8 texture of four floats (1D thin, 16 bytes an
  // element: level 1 at 1024) drawn as targets, level 1 with w = 2 (its
  // red twice the clip-space x); a load of level 1 through the T# reads
  // level 1's target.
  {
    constexpr gaddr kMipped = 0x1B4000;
    verts({{-1, -1}, {1, -1}, {-1, 1}});
    DrawInto(0x11, 3, kMipped);
    const float far[3][4] = {{-2, -2, 1, 2}, {2, -2, 1, 2}, {-2, 2, 1, 2}};
    std::memcpy(GPtr(kVerts), far, sizeof far);
    DrawInto(0x11, 3, kMipped + 1024);
    uint32_t user[16] = {};
    TSharp(user, kMipped, 8, 8, 9, 13, 1, 1);
    user[1] = 14u << 20 | 7u << 26;  // 32_32_32_32 float
    VSharp(user + 8, kOut, 16);
    user[12] = 1;  // x
    user[14] = 1;  // level
    Run(kLoadAt, user, 16);
    float red;
    std::memcpy(&red, GPtr(kOut), 4);
    if (std::fabs(red + 1.25f) > 1e-4f) std::printf("  level 1 texel (1,0): %g\n", red);
    Check(std::fabs(red + 1.25f) <= 1e-4f,
          "a T# of several levels reads each level's colour target");
  }
  // A draw over the left half of an 8x8 depth target with a stencil that
  // replaces it with 0xA5, then loads of the stencil through T#s in R8
  // UNORM and R8 UINT at x 1 (drawn) and 6 (not: the clear's 0).
  {
    constexpr gaddr kStTarget = 0x1B6000, kDepthAt = 0x1B8000, kStencilAt = 0x1BC000;
    verts({{-1, -1}, {0, -1}, {-1, 1}});
    Regs r;
    DrawRegs(r, 0x11, kStTarget, kVsAt);
    uint32_t* cx = r.context;
    cx[0x010] = 3;                                  // DB_Z_INFO: 32-bit float
    cx[0x011] = 1;                                  // DB_STENCIL_INFO: 8 bits
    cx[0x012] = uint32_t(kDepthAt >> 8);            // DB_Z_READ_BASE
    cx[0x013] = uint32_t(kStencilAt >> 8);          // DB_STENCIL_READ_BASE
    cx[0x016] = 0;                                  // DB_DEPTH_SIZE: 8x8
    cx[0x200] = 1u | 7u << 8;                       // DB_DEPTH_CONTROL: stencil, always
    cx[0x10B] = 4u | 4u << 4 | 4u << 8;             // DB_STENCIL_CONTROL: replace
    cx[0x10C] = 0xA5u | 0xFFu << 8 | 0xFFu << 16;   // ref, read and write masks
    Draw(r, {3, 1, 0, 2});
    Settle(true);
    for (const uint32_t nfmt : {0u, 4u}) {
      for (const uint32_t x : {1u, 6u}) {
        uint32_t user[16] = {};
        TSharp(user, kStencilAt, 8, 8, 9, 14, 0, 1);
        user[1] = 1u << 20 | nfmt << 26;  // 8, UNORM or UINT
        VSharp(user + 8, kOut, 16);
        user[12] = x;
        user[13] = 3;
        Wr32(kOut, 0xEEEEEEEEu);
        Run(kLoadAt, user, 16);
        const uint32_t want = x == 1 ? 0xA5 : 0, got = Rd32(kOut);
        float f;
        std::memcpy(&f, &got, 4);
        const bool ok = nfmt ? got == want : std::fabs(f - float(want) / 255.0f) < 1e-6f;
        if (!ok) std::printf("  number format %u, x %u: %08X\n", nfmt, x, got);
        Check(ok, "a stencil samples as R8 UNORM and UINT");
      }
    }
  }
  // Occlusion counts over that depth target (stencil 0xA5 on the left
  // half, 0 on the right): draws DB_COUNT_CONTROL counts add the pixels
  // they write (twice for two instances), those its stencil test fails or that it does not count
  // add nothing.
  {
    constexpr gaddr kStTarget = 0x1B6000, kDepthAt = 0x1B8000, kStencilAt = 0x1BC000;
    auto draw_counted = [&](bool left, uint32_t func, uint32_t count_control, uint32_t instances = 1) {
      if (left) verts({{-1, -1}, {0, -1}, {-1, 1}});
      else verts({{-1, -1}, {1, -1}, {-1, 1}});
      Regs r;
      DrawRegs(r, 0x11, kStTarget, kVsAt);
      uint32_t* cx = r.context;
      cx[0x001] = count_control;                    // DB_COUNT_CONTROL
      cx[0x010] = 3;
      cx[0x011] = 1;
      cx[0x012] = uint32_t(kDepthAt >> 8);
      cx[0x013] = uint32_t(kStencilAt >> 8);
      cx[0x200] = 1u | func << 8;                   // stencil test, no depth test
      cx[0x10C] = 0xA5u | 0xFFu << 8;               // ref 0xA5, read mask, no writes
      Draw(r, {3, instances, 0, 2});
    };
    auto counted = [&](bool left, uint32_t func, uint32_t count_control, uint32_t instances = 1) {
      const uint64_t before = OcclusionCount();
      draw_counted(left, func, count_control, instances);
      Settle(true);
      return OcclusionCount() - before;
    };
    const uint64_t all = counted(true, 7, 1u << 8, 2), failed = counted(true, 5, 1u << 8),
                   right = counted(false, 5, 1u << 8), off = counted(false, 7, 1u << 8 | 1);
    if (all != 64 || failed != 0 || right != 32 || off != 0)
      std::printf("  counts: %llu %llu %llu %llu\n", (unsigned long long)all, (unsigned long long)failed,
                  (unsigned long long)right, (unsigned long long)off);
    Check(all == 64 && failed == 0 && right == 32 && off == 0, "occlusion counts count the pixels drawn");
    // A query through the command processor: PIXEL_PIPE_STAT_CONTROL as
    // the console's Gnm writes it, a dump (begin) at kQuery, a counted
    // draw of 64 pixels, a dump (end) at kQuery + 8. Each of the 8 render
    // backends' 16 bytes holds begin and end with bit 63 set; their
    // differences add up to the draw's pixels.
    constexpr gaddr kQuery = kOut + 0x4000;
    auto dump = [&](gaddr at) {
      const uint32_t dcb[] = {0xC0024600, 0x138, 0x7FC00, 0, 0xC0024600, 0x139, uint32_t(at), uint32_t(at >> 32)};
      std::memcpy(GPtr(kDcb), dcb, sizeof dcb);
      std::atomic<bool> done{false};
      SubmitGraphics({{kDcb, uint32_t(sizeof dcb / 4)}}, {{0, 0}}, [&] { done = true; });
      for (int i = 0; i < 5000 && !done; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return bool(done);
    };
    std::memset(GPtr(kQuery), 0xEE, 128);
    bool ok = dump(kQuery);
    draw_counted(true, 7, 1u << 8, 2);
    ok &= dump(kQuery + 8);
    Settle(true);
    uint64_t sum = 0;
    for (int rb = 0; rb < 8; rb++) {
      const uint64_t begin = Rd64(kQuery + 16 * rb), end = Rd64(kQuery + 16 * rb + 8);
      ok &= (begin >> 63) && (end >> 63) && end >= begin;
      sum += end - begin;
    }
    if (!ok || sum != 64) std::printf("  dumped sum %llu\n", (unsigned long long)sum);
    Check(ok && sum == 64, "an occlusion query's dumps hold the pixels drawn between them");
  }
  // DMA_DATA into and out of GDS: a value written twice (8 bytes) at GDS
  // offset 0x10, a dword from memory at 0x18, then 16 bytes from 0x10
  // back into memory (0x1C still holds GDS's zero).
  {
    constexpr gaddr kFrom = kOut + 0x5000, kTo = kOut + 0x5100;
    Wr32(kFrom, 0xCAFEF00D);
    std::memset(GPtr(kTo), 0xEE, 32);
    const uint32_t dcb[] = {
        0xC0055000, 0x40100000, 0x12345678, 0, 0x10, 0, 8,                      // data -> GDS
        0xC0055000, 0x00100000, uint32_t(kFrom), uint32_t(kFrom >> 32), 0x18, 0, 4,  // memory -> GDS
        0xC0055000, 0x20000000, 0x10, 0, uint32_t(kTo), uint32_t(kTo >> 32), 16,     // GDS -> memory
    };
    std::memcpy(GPtr(kDcb), dcb, sizeof dcb);
    std::atomic<bool> done{false};
    SubmitGraphics({{kDcb, uint32_t(sizeof dcb / 4)}}, {{0, 0}}, [&] { done = true; });
    for (int i = 0; i < 5000 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Settle(true);
    const uint32_t want[] = {0x12345678, 0x12345678, 0xCAFEF00D, 0, 0xEEEEEEEE};
    bool ok = done;
    for (int i = 0; i < 5; i++) ok &= Rd32(kTo + 4 * i) == want[i];
    if (!ok)
      std::printf("  GDS copy: %08X %08X %08X %08X %08X\n", Rd32(kTo), Rd32(kTo + 4), Rd32(kTo + 8),
                  Rd32(kTo + 12), Rd32(kTo + 16));
    Check(ok, "DMA_DATA writes GDS from data and memory and copies it back");
  }
  // An indirect draw through the command processor: its first vertex (3)
  // goes to the vertex shader's user SGPR 4, which it adds to the vertex
  // index; vertices 0-2 are off the target.
  static const uint32_t kVsOffset[] = {
      0x4A000004,              // v_add_i32 v0, vcc, s4, v0
      0xE0382000, 0x80000100,  // buffer_load_dwordx4 v[1:4], v0, s[0:3], 0 idxen
      0xBF8C0F70,              // s_waitcnt vmcnt(0)
      0xF80008CF, 0x04030201,  // exp pos0 v1, v2, v3, v4 done
      0xF800020F, 0x04030201,  // exp param0 v1, v2, v3, v4
      0xBF810000,              // s_endpgm
  };
  std::memcpy(GPtr(kVsOffsetAt), kVsOffset, sizeof kVsOffset);
  verts({{2, 2}, {3, 2}, {2, 3}, {-1, -1}, {1, -1}, {-1, 1}});
  {
    Regs r;
    DrawRegs(r, 0x11, 0x1B3000, kVsOffsetAt);
    std::vector<uint32_t> dcb;
    for (uint32_t i = 0; i < kRegSpace; i++) {
      if (r.context[i]) dcb.insert(dcb.end(), {0xC0016900, i, r.context[i]});
      if (r.sh[i]) dcb.insert(dcb.end(), {0xC0017600, i, r.sh[i]});
      if (r.uconfig[i]) dcb.insert(dcb.end(), {0xC0017900, i, r.uconfig[i]});
    }
    const uint32_t args[4] = {3, 1, 3, 0};  // vertices, instances, first vertex, first instance
    std::memcpy(GPtr(kArgs), args, sizeof args);
    dcb.insert(dcb.end(), {0xC0021100, 1, uint32_t(kArgs), 0});    // SET_BASE 1
    dcb.insert(dcb.end(), {0xC0032400, 0, 0x4C + 4, 0, 2});        // DRAW_INDIRECT
    std::memcpy(GPtr(kDcb), dcb.data(), 4 * dcb.size());
    std::atomic<bool> done{false};
    SubmitGraphics({{kDcb, uint32_t(dcb.size())}}, {{0, 0}}, [&] { done = true; });
    for (int i = 0; i < 5000 && !done; i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Settle(true);
    Check(done && Covered(0x11, 0x1B3000, dev.get()),
          "an indirect draw starts at its first vertex through the shader's register");
  }

  SetDevice(nullptr);
  dev.reset();
  SDL_DestroyWindow(w);
  SDL_Quit();
  if (failures) return 1;
  std::printf("orbis gpu: ok\n");
  return 0;
}
