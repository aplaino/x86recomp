// gfx::Device's fixed-function draws (DrawFf) on the SDL_GPU backend: a
// draw from separate streams (RGBA colours, 32-bit indices, a first
// vertex past 0), which the device reads as they lie, gives the same
// pixels as the same vertices in records with an attribute off a word
// boundary, which it interleaves first; so do interleaved vertices
// (D3DCOLORs, 16-bit indices) and a stream off a word boundary; an absent
// stream reads as zeros; a locked stream is taken once per lock. Needs a
// Vulkan device (lavapipe will do); exits 77 (skipped) without one.

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

// The state the draws have: blending, alpha test, a modulating texture
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

void Clear(Device& dev) { dev.Clear(0, nullptr, 1, 0xFF102030, 1, 0); }

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

  // ---- 1. The reference: records with the colour 2 bytes past a word
  // boundary (position, normal, 2 bytes, colour, texture coordinates),
  // which the device interleaves before it draws.
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
  const uint32_t fvf = 0x002 | 0x010 | 0x040 | 0x100;  // XYZ NORMAL DIFFUSE TEX1
  const uint32_t nall = uint32_t(sc.pos.size() / 4), nv = nall - Scene::kFirst;
  constexpr uint32_t kRec = 40;
  std::vector<uint8_t> recs(size_t(nall) * kRec);
  for (uint32_t v = 0; v < nall; v++) {
    std::memcpy(&recs[v * kRec], &sc.pos[v * 4], 12);
    std::memcpy(&recs[v * kRec + 12], &sc.normal[v * 3], 12);
    std::memcpy(&recs[v * kRec + 26], &sc.rgba[v * 4], 4);
    std::memcpy(&recs[v * kRec + 32], &sc.uv[v * 2], 8);
  }
  FfDraw ref;
  ref.prim = 4;
  ref.fvf = fvf;
  ref.position = {recs.data(), kRec};
  ref.normal = {recs.data() + 12, kRec};
  ref.diffuse = {recs.data() + 26, kRec};
  ref.rgba_colors = true;
  ref.tex[0] = {recs.data() + 32, kRec};
  ref.first_vertex = Scene::kFirst;
  ref.vertex_count = nv;
  ref.indices = sc.indices.data();
  ref.index_count = uint32_t(sc.indices.size());
  ref.index_size = 4;
  Clear(*dev);
  dev->DrawFf(st, ref);
  const std::vector<uint32_t> want = Read(*dev);
  int drawn = 0;
  for (uint32_t p : want) drawn += p != 0xFF102030;
  Check(drawn > kSize * kSize / 4, "the reference draw covers part of the target");

  // ---- 2. The same vertices from separate streams.
  FfDraw fd = ref;
  fd.position = {sc.pos.data(), 16};
  fd.normal = {sc.normal.data(), 12};
  fd.diffuse = {sc.rgba.data(), 4};
  fd.tex[0] = {sc.uv.data(), 8};
  Clear(*dev);
  dev->DrawFf(st, fd);
  Check(Read(*dev) == want, "separate streams");
  FfDraw one_uv = fd;  // every vertex with vertex 0's coordinates (0, 0)
  one_uv.tex[0].stride = 0;
  Clear(*dev);
  dev->DrawFf(st, one_uv);
  Check(Read(*dev) != want, "texture coordinates per vertex");

  // ---- 3. Interleaved vertices with D3DCOLORs and 16-bit indices.
  const uint32_t stride = FvfStride(fvf);
  std::vector<uint8_t> inter;
  std::vector<uint16_t> idx16;
  for (uint32_t v = Scene::kFirst; v < nall; v++) {
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
  FfDraw inter_d = FfInterleaved(4, fvf, inter.data(), stride);
  inter_d.vertex_count = nv;
  inter_d.indices = idx16.data();
  inter_d.index_count = uint32_t(idx16.size());
  Clear(*dev);
  dev->DrawFf(st, inter_d);
  Check(Read(*dev) == want, "interleaved vertices");

  // ---- 4. Without indices: a fan of the same vertices.
  FfDraw fan_ref = ref, fan = fd;
  fan_ref.prim = fan.prim = 6;
  fan_ref.indices = fan.indices = nullptr;
  Clear(*dev);
  dev->DrawFf(st, fan_ref);
  const std::vector<uint32_t> want_fan = Read(*dev);
  Clear(*dev);
  dev->DrawFf(st, fan);
  Check(want_fan != want && Read(*dev) == want_fan, "a non-indexed fan");

  // ---- 5. A stream off a word boundary (copied to one).
  std::vector<uint8_t> shifted(sc.pos.size() * 4 + 2);
  std::memcpy(shifted.data() + 2, sc.pos.data(), sc.pos.size() * 4);
  FfDraw odd = fd;
  odd.position = {shifted.data() + 2, 16};
  Clear(*dev);
  dev->DrawFf(st, odd);
  Check(Read(*dev) == want, "a stream off a word boundary");

  // ---- 6. A draw with another texture after one with the first (the
  // device keeps each stage's binding from the draw before).
  const TextureId tex2 = dev->CreateTexture(2, 2, 1, Format::A8R8G8B8, false);
  const uint32_t texels2[4] = {0xFF0000FF, 0xFF00FFFF, 0x80FF00FF, 0xFFFFFF00};
  dev->UpdateTexture(tex2, 0, {0, 0, 2, 2}, texels2, 8);
  FfState other_tex = st;
  other_tex.textures[0] = tex2;
  auto forget = [&] { dev->DestroyTexture(dev->CreateTexture(1, 1, 1, Format::A8R8G8B8, false)); };
  forget();
  Clear(*dev);
  dev->DrawFf(other_tex, fd);
  const std::vector<uint32_t> want_tex2 = Read(*dev);
  forget();
  Clear(*dev);
  dev->DrawFf(st, fd);
  Clear(*dev);
  dev->DrawFf(other_tex, fd);
  Check(want_tex2 != want && Read(*dev) == want_tex2, "a draw with another texture after one");

  // ---- 7. An absent stream reads as zeros.
  const std::vector<uint8_t> zeros(sc.rgba.size(), 0);
  FfDraw black = fd;
  black.diffuse = {zeros.data(), 4};
  Clear(*dev);
  dev->DrawFf(st, black);
  const std::vector<uint32_t> want_black = Read(*dev);
  black.diffuse = {};
  Clear(*dev);
  dev->DrawFf(st, black);
  Check(Read(*dev) == want_black, "an absent stream reads as zeros");

  // ---- 8. A locked stream, opaque, drawn twice between flushes (a
  // readback flushes): with the same lock the second draw keeps the
  // first one's positions, though they changed in between (which a
  // lock's promise rules out, to see it); with another lock it takes them
  // again.
  FfState opaque = st;
  opaque.rs[27] = 0;
  std::vector<float> pos = sc.pos;
  auto twice = [&](uint32_t lock1, uint32_t lock2) {
    pos = sc.pos;
    FfDraw ld = fd;
    ld.position = {pos.data(), 16, lock1};
    Clear(*dev);
    dev->DrawFf(opaque, ld);
    for (size_t i = 0; i < pos.size(); i += 4) pos[i] = pos[i] * 0.5f - 0.4f;
    ld.position.lock = lock2;
    dev->DrawFf(opaque, ld);
    return Read(*dev);
  };
  const std::vector<uint32_t> both = twice(0, 0);
  FfDraw once = fd;
  once.position = {sc.pos.data(), 16};
  Clear(*dev);
  dev->DrawFf(opaque, once);
  const std::vector<uint32_t> first_only = Read(*dev);
  Check(both != first_only, "the moved positions draw elsewhere");
  Check(twice(7, 7) == first_only, "the same lock keeps its stream");
  Check(twice(8, 9) == both, "another lock takes the stream again");

  dev->DestroyTexture(tex2);
  dev->DestroyTexture(tex);
  if (failures) return 1;
  std::printf("ok\n");
  return 0;
}
