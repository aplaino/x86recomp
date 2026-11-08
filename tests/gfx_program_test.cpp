// gfx::Device's programs (console GPUs' path) on the SDL_GPU backend: a
// draw reading a buffer, a uniform block and a texture into a colour
// target, the scissor, clears and depth testing, a compute dispatch
// writing a buffer (written back) and a storage image, GPU buffers read
// by draws and dispatches and drawn from as indices, and reading images
// back, on a device without a window. Needs a Vulkan device (lavapipe
// will do); exits 77 (skipped) without one.

#include <SDL3/SDL.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

#include "runtime/gfx/device.h"

namespace {

using namespace recomp::gfx;

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

const char* kVs = R"(
void main() {
  uint b = uint(gl_VertexIndex) * 3u;
  gl_Position = vec4(uintBitsToFloat(r_ld(0u, b)), uintBitsToFloat(r_ld(0u, b + 1u)),
                     uintBitsToFloat(r_ld(0u, b + 2u)), 1.0);
}
)";
const char* kFs = R"(
layout(R_UNIFORMS) uniform U { vec4 color; };
layout(R_TEXTURE(0)) uniform sampler2D tex;
layout(location = 0) out vec4 o;
void main() { o = color * texelFetch(tex, ivec2(0), 0); }
)";
const char* kCs = R"(
layout(R_IMAGE(0), r32ui) uniform writeonly uimage2D img;
void main() {
  uint i = gl_GlobalInvocationID.x;
  r_rw[r_rwbuf[0].x + i] = r_ld(0u, i) * 2u + 1u;
  if (i < 16u) imageStore(img, ivec2(i & 3u, i >> 2), uvec4(i * 10u));
}
)";

// A triangle covering the target at depth z.
std::vector<float> Cover(float z) { return {-1, -1, z, 3, -1, z, -1, 3, z}; }

}  // namespace

int main() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::printf("skipped: SDL video: %s\n", SDL_GetError());
    return 77;
  }
  std::unique_ptr<Device> dev = CreateGpuDevice(nullptr);
  if (!dev) {
    std::printf("skipped: no SDL_GPU device\n");
    return 77;
  }
  dev->ResetSwapChain(8, 8, false);

  ProgramDesc pd;
  pd.vs.glsl = kVs;
  pd.vs.buffers = 1;
  pd.fs.glsl = kFs;
  pd.fs.textures = 1;
  pd.fs.uniform_bytes = 16;
  const ProgramId prog = dev->CreateProgram(pd);
  Check(prog != 0, "CreateProgram");

  ImageDesc td;
  td.fmt = Format::R8G8B8A8;
  const TextureId tex = dev->CreateImage(td);
  const uint8_t texel[4] = {0xFF, 0x80, 0x00, 0xFF};
  dev->UpdateImage(tex, 0, 0, {0, 0, 1, 1}, texel, 4);
  ImageDesc rd;
  rd.w = rd.h = 4;
  rd.fmt = Format::R8G8B8A8;
  rd.color_target = true;
  const TextureId rt = dev->CreateImage(rd);
  const DepthId depth = dev->CreateDepth(4, 4, Format::D32);

  auto draw_from = [&](const BufferData& vb, const float color[4], const Rect& scissor,
                       bool depth_test, BufferId index_buffer = 0, uint32_t first_index = 0) {
    ProgramDraw d;
    d.program = prog;
    d.targets[0] = {rt, 0};
    d.depth = depth_test ? depth : 0;
    d.state.depth_test = d.state.depth_write = depth_test;
    d.state.depth_compare = CompareOp::Less;
    d.viewport = {0, 0, 4, 4, 0, 1};
    d.scissor = scissor;
    d.vertex_count = 3;
    d.index_buffer = index_buffer;
    d.first_index = first_index;
    d.vs.buffers = &vb;
    d.fs.textures = &tex;
    SamplerDesc smp;
    d.fs.samplers = &smp;
    d.fs.uniforms = color;
    dev->DrawProgram(d);
  };
  auto draw = [&](const std::vector<float>& tri, const float color[4], const Rect& scissor,
                  bool depth_test) {
    draw_from({tri.data(), uint32_t(tri.size() * 4)}, color, scissor, depth_test);
  };
  auto pixels = [&] {
    std::vector<uint32_t> px(16);
    dev->ReadImage(rt, 0, 0, {0, 0, 4, 4}, px.data(), 16);
    return px;
  };

  dev->ClearImage(rt, 0, {0, 0, 1, 1});
  std::vector<uint32_t> px = pixels();
  Check(px[0] == 0xFFFF0000u && px[15] == 0xFFFF0000u, "a clear fills the image");
  const float white[4] = {1, 1, 1, 1};
  draw(Cover(0), white, {0, 0, 4, 4}, false);
  px = pixels();
  bool all = true;
  for (uint32_t p : px) all &= p == 0xFF0080FFu;
  Check(all, "a draw writes the texel times the uniform colour everywhere");

  const float half[4] = {0, 1, 1, 1};
  draw(Cover(0), half, {0, 0, 2, 4}, false);
  px = pixels();
  Check(px[0] == 0xFF008000u && px[1] == 0xFF008000u && px[2] == 0xFF0080FFu &&
            px[15] == 0xFF0080FFu,
        "the scissor keeps the draw in its columns");

  // Depth: the nearer triangle wins whichever is drawn last.
  dev->ClearDepth(depth, 1.0f, -1, 0);
  const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
  draw(Cover(0.25f), red, {0, 0, 4, 4}, true);
  draw(Cover(0.5f), blue, {0, 0, 4, 4}, true);
  px = pixels();
  Check(px[5] == 0xFF0000FFu, "the farther triangle fails the depth test");

  // Compute: a read-write buffer from a read-only one, and an image.
  ProgramDesc cd;
  cd.cs.glsl = kCs;
  cd.cs.buffers = 1;
  cd.cs.rw_buffers = 1;
  cd.cs.rw_images = 1;
  cd.local_size[0] = 64;
  const ProgramId cprog = dev->CreateProgram(cd);
  ImageDesc id;
  id.w = id.h = 4;
  id.fmt = Format::R32Uint;
  id.storage = true;
  const TextureId img = dev->CreateImage(id);
  std::vector<uint32_t> in(64), out(64, 0xDEADBEEF);
  for (uint32_t i = 0; i < 64; i++) in[i] = i;
  ProgramDispatch pdp;
  pdp.program = cprog;
  BufferData ib{in.data(), 256};
  pdp.cs.buffers = &ib;
  RwBufferData ob{out.data(), 256};
  pdp.rw_buffers = &ob;
  ColorTarget ic{img, 0};
  pdp.rw_images = &ic;
  dev->DispatchProgram(pdp);
  bool ok = true;
  for (uint32_t i = 0; i < 64; i++) ok &= out[i] == i * 2 + 1;
  Check(ok, "a dispatch's read-write buffer comes back written");
  std::fill(out.begin(), out.end(), 0xDEADBEEF);
  pdp.land_later = true;
  dev->DispatchProgram(pdp);
  Check(dev->WritesQueued() == 1, "a land_later dispatch queues its write-back");
  Check(dev->LandWrites(true) == 1, "LandWrites lands it");
  ok = true;
  for (uint32_t i = 0; i < 64; i++) ok &= out[i] == i * 2 + 1;
  Check(ok, "a land_later dispatch's read-write buffer is written once landed");
  pdp.land_later = false;
  std::vector<uint32_t> iv(16);
  dev->ReadImage(img, 0, 0, {0, 0, 4, 4}, iv.data(), 16);
  ok = true;
  for (uint32_t i = 0; i < 16; i++) ok &= iv[i] == i * 10;
  Check(ok, "a dispatch's storage image reads back");

  // GPU buffers: a draw's vertices from one, at an offset, as they were
  // when it was recorded (the update after it is for the next draw).
  const std::vector<float> cover = Cover(0), away = {5, 5, 0, 6, 5, 0, 5, 6, 0};
  const BufferId vbuf = dev->CreateBuffer(64);
  dev->UpdateBuffer(vbuf, 16, cover.data(), 36);
  dev->ClearImage(rt, 0, {0, 0, 0, 1});
  draw_from({nullptr, 36, vbuf, 16}, red, {0, 0, 4, 4}, false);
  dev->UpdateBuffer(vbuf, 16, away.data(), 36);
  draw_from({nullptr, 36, vbuf, 16}, blue, {0, 0, 4, 4}, false);
  px = pixels();
  Check(px[0] == 0xFF0000FFu && px[15] == 0xFF0000FFu,
        "a draw reads a GPU buffer as it was when recorded");
  // 16-bit indices from a GPU buffer, from its third.
  const uint16_t idx[6] = {7, 7, 0, 1, 2, 0};
  const BufferId ibuf = dev->CreateBuffer(sizeof idx);
  dev->UpdateBuffer(ibuf, 0, idx, sizeof idx);
  const float green[4] = {0, 1, 0, 1};
  draw_from({cover.data(), 36}, green, {0, 0, 4, 4}, false, ibuf, 2);
  px = pixels();
  Check(px[0] == 0xFF008000u && px[15] == 0xFF008000u, "a draw's indices from a GPU buffer");
  // A dispatch reading a GPU buffer.
  std::vector<uint32_t> gin(64);
  for (uint32_t i = 0; i < 64; i++) gin[i] = i + 100;
  const BufferId gbuf = dev->CreateBuffer(256);
  dev->UpdateBuffer(gbuf, 0, gin.data(), 256);
  BufferData gb{nullptr, 256, gbuf};
  pdp.cs.buffers = &gb;
  std::fill(out.begin(), out.end(), 0xDEADBEEF);
  dev->DispatchProgram(pdp);
  ok = true;
  for (uint32_t i = 0; i < 64; i++) ok &= out[i] == (i + 100) * 2 + 1;
  Check(ok, "a dispatch reads a GPU buffer");
  // A buffer past the GPU buffers' room grows it; the others keep theirs.
  const BufferId big = dev->CreateBuffer(96u << 20);
  std::fill(out.begin(), out.end(), 0xDEADBEEF);
  dev->DispatchProgram(pdp);
  ok = true;
  for (uint32_t i = 0; i < 64; i++) ok &= out[i] == (i + 100) * 2 + 1;
  Check(big != 0 && ok, "GPU buffers keep their contents when their room grows");
  for (BufferId b : {vbuf, ibuf, gbuf, big}) dev->DestroyBuffer(b);

  dev->DrawImage(rt, {0, 0, 4, 4});
  dev->Present();
  dev.reset();
  SDL_Quit();
  if (failures) return 1;
  std::printf("gfx programs: ok\n");
  return 0;
}
