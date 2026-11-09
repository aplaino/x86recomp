// gfx::Device's explicit fixed-function draws (DrawFf) on the SDL_GPU
// backend: a draw from separate streams (RGBA colours, 32-bit indices,
// a first vertex past 0) with its own state gives the same pixels as the
// same vertices drawn interleaved after the same state set statefully,
// and leaves the stateful state as it was. Needs a Vulkan device
// (lavapipe will do); exits 77 (skipped) without one.

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/gfx/device.h"

namespace {

using namespace recomp::gfx;

constexpr int kSize = 32;
int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

// The scene's vertices: position, normal, colour (R, G, B, A bytes) and
// one texture coordinate set, kept in separate arrays as a renderer
// keeps them, with two unused vertices first.
struct Scene {
  std::vector<float> pos, normal, uv;
  std::vector<uint8_t> rgba;
  std::vector<uint32_t> indices;  // of the arrays' vertices
  static constexpr uint32_t kFirst = 2;
};

Scene MakeScene() {
  Scene s;
  auto vertex = [&](float x, float y, float z, uint32_t rgba, float u, float v) {
    s.pos.insert(s.pos.end(), {x, y, z, 1});  // 16 bytes apart; w unused
    s.normal.insert(s.normal.end(), {0, 0, -1});
    for (int i = 0; i < 4; i++) s.rgba.push_back(uint8_t(rgba >> (24 - 8 * i)));
    s.uv.insert(s.uv.end(), {u, v});
  };
  vertex(9, 9, 9, 0, 0, 0);  // unused
  vertex(9, 9, 9, 0, 0, 0);
  vertex(-0.9f, -0.9f, 0.5f, 0xFF4020FF, 0, 0);
  vertex(0.9f, -0.8f, 0.3f, 0x20FF40C0, 1, 0);
  vertex(-0.7f, 0.9f, 0.7f, 0x4020FF80, 0, 1);
  vertex(0.8f, 0.85f, 0.4f, 0xFFFFFF40, 1, 1);
  s.indices = {2, 3, 4, 3, 5, 4};
  return s;
}

// The state both draws have: blending, alpha test, a modulating texture
// stage, fog, lighting with two lights, a projection and world matrix.
struct StateValues {
  std::vector<std::pair<uint32_t, uint32_t>> rs;
  std::vector<std::pair<uint32_t, uint32_t>> tss;  // stage 0
  float world[16], proj[16];
  Material mat;
  Light lights[2];
  Viewport vp;
};

StateValues MakeState() {
  StateValues v{};
  v.rs = {{27, 1}, {19, 5}, {20, 6},                    // blend SRCALPHA, INVSRCALPHA
          {15, 1}, {25, 5}, {24, 0x30},                 // alpha test GREATER 0x30
          {22, 1},                                      // no culling
          {28, 1}, {35, 0}, {140, 3}, {34, 0x00306090},  // linear vertex fog
          {36, Bits(0.2f)}, {37, Bits(0.9f)},
          {137, 1}, {139, 0xFF202020}, {141, 1}, {145, 1}};
  v.tss = {{1, 4}, {2, 2}, {3, 0}, {4, 4}, {5, 2}, {6, 0}, {16, 2}, {17, 2}};
  const float world[16] = {0.9f, 0.1f, 0, 0, -0.1f, 0.9f, 0, 0, 0, 0, 1, 0, 0.05f, 0, 0, 1};
  const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::memcpy(v.world, world, sizeof world);
  std::memcpy(v.proj, proj, sizeof proj);
  v.mat = {{0.8f, 0.7f, 0.6f, 1}, {0.3f, 0.3f, 0.3f, 1}, {0, 0, 0, 0}, {0.1f, 0, 0, 0}, 0};
  v.lights[0] = {};
  v.lights[0].type = 3;
  v.lights[0].diffuse = {1, 0.9f, 0.8f, 1};
  v.lights[0].direction[2] = 1;
  v.lights[1] = {};
  v.lights[1].type = 1;
  v.lights[1].diffuse = {0.2f, 0.4f, 1, 1};
  v.lights[1].position[2] = -2;
  v.lights[1].range = 100;
  v.lights[1].attenuation0 = 1;
  v.vp = {2, 3, kSize - 4, kSize - 5, 0, 1};
  return v;
}

std::vector<uint32_t> Read(Device& dev) {
  std::vector<uint32_t> px(kSize * kSize);
  dev.ReadBack({0, 0, kSize, kSize}, px.data(), kSize * 4, Format::A8R8G8B8);
  return px;
}

void Clear(Device& dev) {
  dev.SetViewport({0, 0, kSize, kSize, 0, 1});
  dev.Clear(0, nullptr, 1, 0xFF102030, 1, 0);
}

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
  dev->ResetSwapChain(kSize, kSize, false);
  dev->SetRenderTarget(0, 0);

  // A 2x2 texture.
  const TextureId tex = dev->CreateTexture(2, 2, 1, Format::A8R8G8B8, false);
  const uint32_t texels[4] = {0xFFFF0000, 0x80FFFFFF, 0xFF00FF00, 0xC00000FF};
  dev->UpdateTexture(tex, 0, {0, 0, 2, 2}, texels, 8);

  const Scene sc = MakeScene();
  const StateValues sv = MakeState();

  // ---- 1. statefully: the state set, the vertices interleaved.
  Clear(*dev);
  for (auto [s, v] : sv.rs) dev->SetRenderState(s, v);
  for (auto [t, v] : sv.tss) dev->SetTextureStageState(0, t, v);
  dev->SetTexture(0, tex);
  dev->SetTransform(1, sv.world);
  dev->SetTransform(3, sv.proj);
  dev->SetMaterial(sv.mat);
  dev->SetLight(5, sv.lights[0]);  // indices do not matter
  dev->SetLight(9, sv.lights[1]);
  dev->LightEnable(5, true);
  dev->LightEnable(9, true);
  dev->SetViewport(sv.vp);
  const uint32_t fvf = 0x002 | 0x010 | 0x040 | 0x100;  // XYZ NORMAL DIFFUSE TEX1
  const uint32_t stride = FvfStride(fvf);
  std::vector<uint8_t> inter;
  std::vector<uint16_t> idx16;
  for (uint32_t v = Scene::kFirst; v < sc.pos.size() / 4; v++) {
    auto put = [&](const void* p, size_t n) {
      inter.insert(inter.end(), (const uint8_t*)p, (const uint8_t*)p + n);
    };
    put(&sc.pos[v * 4], 12);
    put(&sc.normal[v * 3], 12);
    const uint8_t* c = &sc.rgba[v * 4];
    const uint8_t bgra[4] = {c[2], c[1], c[0], c[3]};
    put(bgra, 4);
    put(&sc.uv[v * 2], 8);
  }
  for (uint32_t i : sc.indices) idx16.push_back(uint16_t(i - Scene::kFirst));
  const uint32_t nv = uint32_t(sc.pos.size() / 4 - Scene::kFirst);
  dev->Draw(4, fvf, inter.data(), nv, stride, idx16.data(), uint32_t(idx16.size()));
  const std::vector<uint32_t> want = Read(*dev);
  int drawn = 0;
  for (uint32_t p : want) drawn += p != 0xFF102030;
  Check(drawn > kSize * kSize / 4, "the stateful draw covers part of the target");

  // ---- 2. DrawFf with the same state from separate streams.
  FfState st = DefaultFfState();
  for (auto [s, v] : sv.rs) st.rs[s] = v;
  for (auto [t, v] : sv.tss) st.tss[0][t] = v;
  st.textures[0] = tex;
  std::memcpy(st.world[0], sv.world, 64);
  std::memcpy(st.proj, sv.proj, 64);
  st.material = sv.mat;
  st.lights[0] = sv.lights[0];
  st.lights[1] = sv.lights[1];
  st.light_count = 2;
  st.viewport = sv.vp;
  FfDraw fd;
  fd.prim = 4;
  fd.fvf = fvf;
  fd.position = {sc.pos.data(), 16};
  fd.normal = {sc.normal.data(), 12};
  fd.diffuse = {sc.rgba.data(), 4};
  fd.rgba_colors = true;
  fd.tex[0] = {sc.uv.data(), 8};
  fd.first_vertex = Scene::kFirst;
  fd.vertex_count = nv;
  fd.indices = sc.indices.data();
  fd.index_count = uint32_t(sc.indices.size());
  fd.index_size = 4;

  // The stateful state changed to something else first: DrawFf ignores it.
  dev->SetRenderState(27, 0);
  dev->SetRenderState(28, 0);
  dev->SetTextureStageState(0, 1, 2);
  dev->LightEnable(5, false);
  Clear(*dev);
  dev->DrawFf(st, fd);
  Check(Read(*dev) == want, "DrawFf draws what the stateful draw drew");

  // ---- 3. The stateful state is as it was set, not DrawFf's.
  for (auto [s, v] : sv.rs) dev->SetRenderState(s, v);
  dev->SetTextureStageState(0, 1, 4);
  dev->LightEnable(5, true);
  dev->SetViewport(sv.vp);
  Clear(*dev);
  FfState other = st;
  other.rs[27] = 0;
  other.textures[0] = 0;
  dev->DrawFf(other, fd);
  Clear(*dev);
  dev->SetViewport(sv.vp);
  dev->Draw(4, fvf, inter.data(), nv, stride, idx16.data(), uint32_t(idx16.size()));
  Check(Read(*dev) == want, "a stateful draw after DrawFf has the stateful state");

  // ---- 4. Without indices: a fan of the same vertices.
  Clear(*dev);
  FfDraw fan = fd;
  fan.prim = 6;
  fan.indices = nullptr;
  dev->DrawFf(st, fan);
  Clear(*dev);
  dev->SetViewport(sv.vp);
  dev->Draw(6, fvf, inter.data(), nv, stride, nullptr, 0);
  const std::vector<uint32_t> want_fan = Read(*dev);
  Clear(*dev);
  dev->DrawFf(st, fan);
  Check(Read(*dev) == want_fan, "a non-indexed fan through DrawFf");

  dev->DestroyTexture(tex);
  if (failures) return 1;
  std::printf("ok\n");
  return 0;
}
