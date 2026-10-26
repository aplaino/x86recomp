// gfx::Device's programs (console GPUs' path) on the SDL_GPU backend: a
// draw reading a buffer, a uniform block and a texture into a colour
// target, the scissor, clears and depth testing, a compute dispatch
// writing a buffer (written back) and a storage image, and reading images
// back. Needs a
// Vulkan device (lavapipe will do); exits 77 (skipped) without one.

#include <SDL3/SDL.h>

#include <cstdio>
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
  uint b = r_buf[0].x + uint(gl_VertexIndex) * 3u;
  gl_Position = vec4(uintBitsToFloat(r_mem[b]), uintBitsToFloat(r_mem[b + 1u]),
                     uintBitsToFloat(r_mem[b + 2u]), 1.0);
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
  r_rw[r_rwbuf[0].x + i] = r_mem[r_buf[0].x + i] * 2u + 1u;
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
  SDL_Window* w = SDL_CreateWindow("gfx_program_test", 64, 64, SDL_WINDOW_HIDDEN);
  std::unique_ptr<Device> dev = w ? CreateGpuDevice(w) : nullptr;
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

  auto draw = [&](const std::vector<float>& tri, const float color[4], const Rect& scissor,
                  bool depth_test) {
    ProgramDraw d;
    d.program = prog;
    d.targets[0] = {rt, 0};
    d.depth = depth_test ? depth : 0;
    d.state.depth_test = d.state.depth_write = depth_test;
    d.state.depth_compare = CompareOp::Less;
    d.viewport = {0, 0, 4, 4, 0, 1};
    d.scissor = scissor;
    d.vertex_count = 3;
    BufferData vb{tri.data(), uint32_t(tri.size() * 4)};
    d.vs.buffers = &vb;
    d.fs.textures = &tex;
    SamplerDesc smp;
    d.fs.samplers = &smp;
    d.fs.uniforms = color;
    dev->DrawProgram(d);
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
  dev->ClearDepth(depth, 1.0f, -1);
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
  std::vector<uint32_t> iv(16);
  dev->ReadImage(img, 0, 0, {0, 0, 4, 4}, iv.data(), 16);
  ok = true;
  for (uint32_t i = 0; i < 16; i++) ok &= iv[i] == i * 10;
  Check(ok, "a dispatch's storage image reads back");

  dev->PresentImage(rt, {0, 0, 4, 4});
  dev.reset();
  SDL_DestroyWindow(w);
  SDL_Quit();
  if (failures) return 1;
  std::printf("gfx programs: ok\n");
  return 0;
}
