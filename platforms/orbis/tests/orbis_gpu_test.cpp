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

// Runs code with the user data on one 1x1x1 group and waits for it.
void Run(gaddr code, const uint32_t* user, uint32_t n) {
  Regs r;
  r.sh[kThreadX] = r.sh[kThreadX + 1] = r.sh[kThreadX + 2] = 1;
  r.sh[kPgmLo] = uint32_t(code >> 8);
  r.sh[kRsrc1 + 1] = n << 1;
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
