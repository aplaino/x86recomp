// gfx::Device on SDL_GPU: native Metal on macOS, Vulkan elsewhere.
//
// The D3D fixed-function pipeline is two uber-shaders (shaders/ff.vert
// and ff.frag; SPIR-V for Vulkan, MSL from SPIRV-Cross for Metal) driven
// by uniforms, so nothing is compiled per state. What a pipeline object
// must fix (blend, depth/stencil, rasterizer, topology, depth format) is
// cached by state.
//
// D3D draws arrive one at a time with their data in guest memory; SDL_GPU
// uploads only in copy passes, outside render passes. So draws are
// recorded: vertex/index data and uniforms go to CPU arenas, the draw to a
// command list for the current render target. A flush (render target
// change, texture update of a texture in use, readback, present) uploads
// everything in one copy pass and replays the list in one render pass.
// Texture uploads queue the same way and land before the next pass.
//
// Colour textures and render targets are all B8G8R8A8 (D3D's A8R8G8B8
// memory layout): other D3D formats are converted on upload and on
// readback, as the d3d9 backend does for its X8R8G8B8 back buffer. DXT
// stays compressed (BC1-3).

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/log.h"

// kFfVert and kFfFrag, built from shaders/ff.*: MSL source on macOS,
// SPIR-V words elsewhere.
#include "ff_frag.h"
#include "ff_vert.h"

namespace recomp::gfx {

namespace {

// ---- uniform blocks (layouts mirror the shaders')

struct GpuLight {
  float diffuse[4], specular[4], ambient[4], position[4], direction[4], atten[4], spot[4];
};

struct VsUniforms {
  float world_view[4][16];
  float normal[4][16];
  float proj[16];
  float tex[8][16];
  float viewport[4];
  float target[4];
  float ambient[4];
  float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4];
  float mat_power[4];
  float fog[4];
  float clip[8][4];
  GpuLight lights[8];
  int32_t layout[4];
  int32_t layout2[4];
  int32_t tex_offset[8];
  int32_t tex_size[8];
  int32_t mode[4], mode2[4], mode3[4], mode4[4], mode5[4];
  int32_t tci[8];
  int32_t ttf[8];
  float point[4];
};

struct FsUniforms {
  int32_t color[8][4];
  int32_t alpha[8][4];
  int32_t stage[8][4];
  float tfactor[4];
  float fog_color[4];
  float fog[4];
  int32_t mode[4];
  int32_t mode2[4];
};

// ---- small maths on D3D row-major matrices (row vectors: v * M)

using Mat = std::array<float, 16>;

Mat Identity() {
  Mat m{};
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}

Mat Mul(const Mat& a, const Mat& b) {
  Mat r{};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = s;
    }
  return r;
}

// General 4x4 inverse (cofactors); identity if singular.
Mat Inverse(const Mat& m) {
  Mat inv;
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (det == 0 || !std::isfinite(det)) return Identity();
  for (float& v : inv) v /= det;
  return inv;
}

// The normal matrix of `m` (row vectors): inverse transpose of its 3x3.
Mat NormalMatrix(const Mat& m) {
  Mat a = Identity();
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) a[r * 4 + c] = m[r * 4 + c];
  Mat inv = Inverse(a);
  Mat t = Identity();
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) t[r * 4 + c] = inv[c * 4 + r];
  return t;
}

void Vec4(float* out, const Color& c) {
  out[0] = c.r;
  out[1] = c.g;
  out[2] = c.b;
  out[3] = c.a;
}

void Argb(float* out, uint32_t c) {
  out[0] = float((c >> 16) & 0xFF) / 255.0f;
  out[1] = float((c >> 8) & 0xFF) / 255.0f;
  out[2] = float(c & 0xFF) / 255.0f;
  out[3] = float(c >> 24) / 255.0f;
}

float AsFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

uint32_t FloatBits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}

// ---- D3D enums -> SDL_GPU

SDL_GPUBlendFactor BlendFactor(uint32_t d3d, bool target_alpha) {
  switch (d3d) {
    case 1: return SDL_GPU_BLENDFACTOR_ZERO;
    case 2: return SDL_GPU_BLENDFACTOR_ONE;
    case 3: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 4: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 6: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    // A target without alpha (X8R8G8B8, R5G6B5) reads alpha as 1.
    case 7: return target_alpha ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
    case 8: return target_alpha ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ZERO;
    case 9: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 10: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 11: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 14: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case 15: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return SDL_GPU_BLENDFACTOR_ONE;
  }
}

SDL_GPUBlendOp BlendOp(uint32_t d3d) {
  switch (d3d) {
    case 2: return SDL_GPU_BLENDOP_SUBTRACT;
    case 3: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case 4: return SDL_GPU_BLENDOP_MIN;
    case 5: return SDL_GPU_BLENDOP_MAX;
    default: return SDL_GPU_BLENDOP_ADD;
  }
}

// D3DCMPFUNC and D3DSTENCILOP use SDL_GPU's numbering (1..8).
SDL_GPUCompareOp CompareOp(uint32_t d3d) {
  return d3d >= 1 && d3d <= 8 ? SDL_GPUCompareOp(d3d) : SDL_GPU_COMPAREOP_ALWAYS;
}
SDL_GPUStencilOp StencilOp(uint32_t d3d) {
  return d3d >= 1 && d3d <= 8 ? SDL_GPUStencilOp(d3d) : SDL_GPU_STENCILOP_KEEP;
}

SDL_GPUSamplerAddressMode AddressMode(uint32_t d3d) {
  switch (d3d) {
    case 2: case 5: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case 3: case 4: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;  // no border
    default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
  }
}

bool HasAlpha(Format f) {
  switch (f) {
    case Format::A8R8G8B8: case Format::A1R5G5B5: case Format::A4R4G4B4:
    case Format::A8: case Format::A8L8: case Format::DXT1: case Format::DXT2:
    case Format::DXT3: case Format::DXT4: case Format::DXT5:
      return true;
    default:
      return false;
  }
}

// D3D's FVF code, decoded to byte offsets for the vertex shader.
struct FvfLayout {
  int stride = 0;
  int pos_kind = 0;  // 0 xyz, 1 xyzrhw, 2 xyzw
  int weights = 0;
  int normal = -1, psize = -1, diffuse = -1, specular = -1;
  int tex_offset[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int tex_size[8] = {};
};

FvfLayout DecodeFvf(uint32_t fvf, uint32_t stride) {
  FvfLayout l;
  int off = 0;
  switch (fvf & 0x400E) {
    case 0x002: off = 12; break;
    case 0x004: l.pos_kind = 1; off = 16; break;
    case 0x006: case 0x008: case 0x00A: case 0x00C: case 0x00E:
      l.weights = int(((fvf & 0xE) - 4) / 2);
      off = 12 + 4 * l.weights;
      break;
    case 0x4002: l.pos_kind = 2; off = 16; break;
    default: off = 12; break;
  }
  if (fvf & 0x010) { l.normal = off; off += 12; }
  if (fvf & 0x020) { l.psize = off; off += 4; }
  if (fvf & 0x040) { l.diffuse = off; off += 4; }
  if (fvf & 0x080) { l.specular = off; off += 4; }
  int ntex = int((fvf >> 8) & 0xF);
  for (int i = 0; i < ntex && i < 8; i++) {
    static constexpr int kSizes[4] = {2, 3, 4, 1};
    int n = kSizes[(fvf >> (16 + 2 * i)) & 3];
    l.tex_offset[i] = off;
    l.tex_size[i] = n;
    off += 4 * n;
  }
  l.stride = int(stride);
  return l;
}

// ---- pipelines

struct PipelineKey {
  uint8_t prim, fill, cull, front;
  uint8_t blend, src, dst, op;
  uint8_t write_mask, depth_test, depth_write, depth_func;
  uint8_t stencil, s_fail, s_zfail, s_pass;
  uint8_t s_func, s_read, s_write, depth_format;
  float bias_const, bias_slope;

  bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(PipelineKey) == 28, "no padding: compared and hashed as bytes");

struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto* b = reinterpret_cast<const uint8_t*>(&k);
    for (size_t i = 0; i < sizeof k; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return size_t(h);
  }
};

struct SamplerKey {
  uint8_t min, mag, mip, u, v, aniso;
  uint8_t pad[2];
  float lod_bias, min_lod;
  bool operator<(const SamplerKey& o) const { return std::memcmp(this, &o, sizeof *this) < 0; }
};

class GpuDevice final : public Device {
 public:
  explicit GpuDevice(SDL_Window* w) : window_(w) {}

  bool Init() {
#if defined(__APPLE__)
    const SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_MSL;
    const char* entry = "main0";  // SPIRV-Cross's name for main
    size_t vs_size = sizeof kFfVert - 1, fs_size = sizeof kFfFrag - 1;
#else
    const SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_SPIRV;
    const char* entry = "main";
    size_t vs_size = sizeof kFfVert, fs_size = sizeof kFfFrag;
#endif
    dev_ = SDL_CreateGPUDevice(format, false, nullptr);
    if (!dev_) {
      WarnF("gfx: SDL_CreateGPUDevice: %s\n", SDL_GetError());
      return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(dev_, window_)) {
      WarnF("gfx: SDL_ClaimWindowForGPUDevice: %s\n", SDL_GetError());
      return false;
    }
    SDL_GPUShaderCreateInfo vs{};
    vs.code = reinterpret_cast<const Uint8*>(kFfVert);
    vs.code_size = vs_size;
    vs.entrypoint = entry;
    vs.format = format;
    vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs.num_uniform_buffers = 1;
    vs.num_storage_buffers = 1;
    vs_ = SDL_CreateGPUShader(dev_, &vs);
    SDL_GPUShaderCreateInfo fs = vs;
    fs.code = reinterpret_cast<const Uint8*>(kFfFrag);
    fs.code_size = fs_size;
    fs.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fs.num_storage_buffers = 0;
    fs.num_samplers = 8;
    fs_ = SDL_CreateGPUShader(dev_, &fs);
    if (!vs_ || !fs_) FatalF("gfx: compiling the fixed-function shaders: %s", SDL_GetError());

    for (auto [d3d, sdl] : {std::pair{Format::DXT1, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM},
                            {Format::DXT3, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM},
                            {Format::DXT5, SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM}})
      if (!SDL_GPUTextureSupportsFormat(dev_, sdl, SDL_GPU_TEXTURETYPE_2D,
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER))
        WarnF("gfx: this GPU cannot sample format %u (DXT)\n", unsigned(d3d));
    depth24_ = SupportsDepth(SDL_GPU_TEXTUREFORMAT_D24_UNORM)
                   ? SDL_GPU_TEXTUREFORMAT_D24_UNORM : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    depth24s8_ = SupportsDepth(SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT)
                     ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
                     : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

    // Stages without a texture still need something bound.
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    dummy_ = SDL_CreateGPUTexture(dev_, &ti);
    uint32_t white = 0xFFFFFFFFu;
    QueueUpload(dummy_, 0, 0, 0, 1, 1, &white, 4, 1, 4);

    InitDefaults();
    std::fprintf(stderr, "gfx: SDL_GPU (%s)\n", SDL_GetGPUDeviceDriver(dev_));
    return true;
  }

  ~GpuDevice() override {
    if (!dev_) return;
    SDL_WaitForGPUIdle(dev_);
    for (SDL_GPUFence* f : in_flight_) SDL_ReleaseGPUFence(dev_, f);
    for (auto& [k, p] : pipelines_) SDL_ReleaseGPUGraphicsPipeline(dev_, p);
    for (auto& [k, s] : samplers_) SDL_ReleaseGPUSampler(dev_, s);
    for (auto& [id, t] : textures_) SDL_ReleaseGPUTexture(dev_, t.tex);
    for (auto& [id, d] : depths_) SDL_ReleaseGPUTexture(dev_, d.tex);
    for (SDL_GPUTexture* t : {backbuffer_, front_, dummy_})
      if (t) SDL_ReleaseGPUTexture(dev_, t);
    for (auto& b : blit_pool_) SDL_ReleaseGPUTexture(dev_, b.tex);
    for (SDL_GPUBuffer* b : {vbuf_, ibuf_})
      if (b) SDL_ReleaseGPUBuffer(dev_, b);
    for (SDL_GPUTransferBuffer* b : {upload_tb_, download_tb_})
      if (b) SDL_ReleaseGPUTransferBuffer(dev_, b);
    if (vs_) SDL_ReleaseGPUShader(dev_, vs_);
    if (fs_) SDL_ReleaseGPUShader(dev_, fs_);
    SDL_ReleaseWindowFromGPUDevice(dev_, window_);
    SDL_DestroyGPUDevice(dev_);
  }

  // ---- swap chain

  void ResetSwapChain(int w, int h, bool vsync) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Flush();
    pass_color_ = pass_depth_ = nullptr;
    for (SDL_GPUTexture** t : {&backbuffer_, &front_}) {
      if (*t) SDL_ReleaseGPUTexture(dev_, *t);
      *t = CreateColor(w, h, 1);
    }
    bb_w_ = w;
    bb_h_ = h;
    // Without vsync, mailbox (no tearing, never blocks), as the d3d9
    // backend has DXVK do. Where there is none (Metal), it is emulated on
    // vsync: Show() presents at most one frame per refresh (immediate
    // presentation tears in fullscreen).
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    mailbox_emulated_ = false;
    if (!vsync) {
      if (SDL_WindowSupportsGPUPresentMode(dev_, window_, SDL_GPU_PRESENTMODE_MAILBOX)) {
        mode = SDL_GPU_PRESENTMODE_MAILBOX;
      } else {
        mailbox_emulated_ = true;
        const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
        const double hz = dm && dm->refresh_rate > 0 ? double(dm->refresh_rate) : 60.0;
        refresh_ns_ = uint64_t(1e9 / hz + 0.5);
        next_show_ns_ = 0;
      }
    }
    SDL_SetGPUSwapchainParameters(dev_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
    // Both start black, as a fresh D3D swap chain does.
    for (SDL_GPUTexture* t : {backbuffer_, front_}) {
      SetPass(t, nullptr);
      pass_clear_color_ = true;
      clear_color_ = {0, 0, 0, 1};
      Flush();
    }
    BindTarget();
    std::fprintf(stderr, "gfx: back buffer %dx%d%s\n", w, h,
                 vsync ? " (vsync)" : mailbox_emulated_ ? " (mailbox on vsync)" : "");
  }

  // ---- resources

  TextureId CreateTexture(int w, int h, int levels, Format fmt, bool rt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (w <= 0 || h <= 0) return 0;
    if (levels <= 0) {
      levels = 1;
      for (int s = std::max(w, h); s > 1; s >>= 1) levels++;
    }
    Tex t;
    t.w = w;
    t.h = h;
    t.levels = levels;
    t.fmt = fmt;
    t.rt = rt;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = GpuFormat(fmt);
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (rt ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    ti.width = Uint32(w);
    ti.height = Uint32(h);
    ti.layer_count_or_depth = 1;
    ti.num_levels = Uint32(levels);
    t.tex = SDL_CreateGPUTexture(dev_, &ti);
    if (!t.tex) {
      WarnF("gfx: CreateTexture %dx%d x%d fmt %u failed: %s\n", w, h, levels,
            unsigned(fmt), SDL_GetError());
      return 0;
    }
    if (fmt == Format::P8) WarnF("gfx: P8 texture (no palette support): drawn as magenta\n");
    TextureId id = next_id_++;
    textures_[id] = t;
    return id;
  }

  void DestroyTexture(TextureId id) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    if (Pending(it->second.tex)) Flush();
    if (current_color_ == id) {
      current_color_ = 0;
      BindTarget();
    }
    SDL_ReleaseGPUTexture(dev_, it->second.tex);
    textures_.erase(it);
    for (TextureId& s : stage_tex_)
      if (s == id) s = 0;
    fs_dirty_ = true;
  }

  void FillTexture(TextureId id, const Rect& r, uint32_t argb) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    Rect c = Clip(r, it->second.w, it->second.h);
    int w = c.right - c.left, h = c.bottom - c.top;
    if (w <= 0 || h <= 0) return;
    if (Pending(it->second.tex)) Flush();
    std::vector<uint32_t> px(size_t(w) * h, argb);
    QueueUpload(it->second.tex, 0, c.left, c.top, w, h, px.data(), uint32_t(w) * 4,
                uint32_t(h), uint32_t(w) * 4);
  }

  void UpdateTexture(TextureId id, int level, const Rect& r, const void* src,
                     uint32_t pitch) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end() || level < 0 || level >= it->second.levels) return;
    const Tex& t = it->second;
    int lw = std::max(1, t.w >> level), lh = std::max(1, t.h >> level);
    Rect c = Clip(r, lw, lh);
    int w = c.right - c.left, h = c.bottom - c.top;
    if (w <= 0 || h <= 0) return;
    // Draws recorded before the update must see the old contents.
    if (Pending(t.tex)) Flush();
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (IsCompressed(t.fmt)) {
      uint32_t row = RowPitch(t.fmt, uint32_t(w)), rows = RowCount(t.fmt, uint32_t(h));
      QueueUpload(t.tex, level, c.left, c.top, w, h, s, pitch, rows, row, false, true);
      return;
    }
    std::vector<uint32_t> px(size_t(w) * h);
    ConvertToArgb(s, pitch, t.fmt, w, h, px.data());
    QueueUpload(t.tex, level, c.left, c.top, w, h, px.data(), uint32_t(w) * 4, uint32_t(h),
                uint32_t(w) * 4);
  }

  DepthId CreateDepth(int w, int h, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Depth d;
    d.w = w;
    d.h = h;
    d.fmt = fmt;
    switch (fmt) {
      case Format::D16: d.gpu_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM; d.bias_scale = 65535.0f; break;
      case Format::D24X8: d.gpu_format = depth24_; break;
      case Format::D32: d.gpu_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT; break;
      default: d.gpu_format = depth24s8_; d.stencil = true; break;  // D24S8, D15S1
    }
    if (d.gpu_format == SDL_GPU_TEXTUREFORMAT_D24_UNORM ||
        d.gpu_format == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT)
      d.bias_scale = 16777215.0f;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = d.gpu_format;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    ti.width = Uint32(w);
    ti.height = Uint32(h);
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    d.tex = SDL_CreateGPUTexture(dev_, &ti);
    if (!d.tex) {
      WarnF("gfx: CreateDepth %dx%d fmt %u failed: %s\n", w, h, unsigned(fmt), SDL_GetError());
      return 0;
    }
    DepthId id = next_id_++;
    depths_[id] = d;
    return id;
  }

  void DestroyDepth(DepthId id) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = depths_.find(id);
    if (it == depths_.end()) return;
    if (pass_depth_ == it->second.tex) Flush();
    if (current_depth_ == id) {
      current_depth_ = 0;
      BindTarget();
    }
    SDL_ReleaseGPUTexture(dev_, it->second.tex);
    depths_.erase(it);
  }

  void SetRenderTarget(TextureId color, DepthId depth) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    current_color_ = color;
    current_depth_ = depth;
    BindTarget();
  }

  // ---- 2D

  void Blit(const void* src, uint32_t pitch, Format fmt, const Rect& sr, const Rect& dr,
            bool colorkey, uint32_t key) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    DrawImage(src, pitch, fmt, sr, dr, colorkey, key);
  }

  void ReadBack(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    ReadTexture(pass_color_, pass_w_, pass_h_, r, dst, pitch, fmt);
  }

  void BlitFront(const void* src, uint32_t pitch, Format fmt, const Rect& sr,
                 const Rect& dr) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    SetPass(front_, nullptr);
    DrawImage(src, pitch, fmt, sr, dr, false, 0);
    BindTarget();
  }

  void ReadBackFront(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    ReadTexture(front_, bb_w_, bb_h_, r, dst, pitch, fmt);
  }

  void PresentFront() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Flush();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    Show(cmd);
    Submit(cmd);
  }

  // ---- fixed-function state

  void SetRenderState(uint32_t s, uint32_t v) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (s >= rs_.size()) return;
    rs_[s] = v;
    vs_dirty_ = fs_dirty_ = true;
  }

  void SetTextureStageState(uint32_t stage, uint32_t type, uint32_t v) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (stage >= 8 || type >= 33) return;
    tss_[stage][type] = v;
    vs_dirty_ = fs_dirty_ = true;
  }

  void SetTexture(uint32_t stage, TextureId t) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (stage >= 8) return;
    stage_tex_[stage] = t;
    fs_dirty_ = true;
  }

  void SetTransform(uint32_t type, const float m[16]) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Mat x;
    std::memcpy(x.data(), m, sizeof(float) * 16);
    switch (type) {
      case 1: world_[0] = x; break;
      case 2: view_ = x; break;
      case 3: proj_ = x; break;
      case 4: case 5: case 6: world_[type - 3] = x; break;
      default:
        if (type >= 16 && type < 24) tex_[type - 16] = x;
        break;
    }
    vs_dirty_ = true;
  }

  void SetViewport(const Viewport& v) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    viewport_ = v;
    vs_dirty_ = true;
  }

  void SetScissor(const Rect& r) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    scissor_ = r;
  }

  void SetMaterial(const Material& m) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    material_ = m;
    vs_dirty_ = true;
  }

  void SetLight(uint32_t i, const Light& li) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    lights_[i] = li;
    vs_dirty_ = true;
  }

  void LightEnable(uint32_t i, bool e) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (e) {
      lights_on_.insert(i);
      if (!lights_.count(i)) lights_[i] = DefaultLight();
    } else {
      lights_on_.erase(i);
    }
    vs_dirty_ = true;
  }

  void SetClipPlane(uint32_t i, const float p[4]) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (i >= 8) return;
    std::memcpy(clip_[i].data(), p, sizeof(float) * 4);
    vs_dirty_ = true;
  }

  // ---- drawing

  void Clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t color, float z,
             uint32_t stencil) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!pass_depth_) flags &= ~6u;
    if (!pass_stencil_) flags &= ~4u;
    if (!(flags & 7)) return;
    Rect area = ViewportRect();
    if (rs_[174]) area = Intersect(area, scissor_);
    std::vector<Rect> todo;
    if (!count || !rects) {
      todo.push_back(area);
    } else {
      for (uint32_t i = 0; i < count; i++) {
        Rect c = Intersect(rects[i], area);
        if (c.right > c.left && c.bottom > c.top) todo.push_back(c);
      }
    }
    if (todo.empty()) return;
    // A clear of the whole target before anything was drawn is the
    // render pass's load operation.
    if (cmds_.empty() && todo.size() == 1 && todo[0].left == 0 && todo[0].top == 0 &&
        todo[0].right == pass_w_ && todo[0].bottom == pass_h_) {
      if (flags & 1) {
        pass_clear_color_ = true;
        Argb(&clear_color_.r, color);
      }
      if (flags & 2) {
        pass_clear_depth_ = true;
        clear_depth_ = z;
      }
      if (flags & 4) {
        pass_clear_stencil_ = true;
        clear_stencil_ = uint8_t(stencil);
      }
      return;
    }
    // Otherwise quads that write exactly what the flags say.
    PipelineKey k = BaseKey(SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    k.write_mask = (flags & 1) ? 0xF : 0;
    if (flags & 2) {
      k.depth_test = k.depth_write = 1;
      k.depth_func = SDL_GPU_COMPAREOP_ALWAYS;
    }
    if (flags & 4) {
      k.stencil = 1;
      k.s_fail = k.s_zfail = k.s_pass = SDL_GPU_STENCILOP_REPLACE;
      k.s_func = SDL_GPU_COMPAREOP_ALWAYS;
      k.s_read = k.s_write = 0xFF;
    }
    struct V {
      float x, y, z, rhw;
      uint32_t c;
    };
    for (const Rect& r : todo) {
      float x0 = float(r.left) - 0.5f, y0 = float(r.top) - 0.5f;
      float x1 = float(r.right) - 0.5f, y1 = float(r.bottom) - 0.5f;
      V q[6] = {{x0, y0, z, 1, color}, {x1, y0, z, 1, color}, {x0, y1, z, 1, color},
                {x1, y0, z, 1, color}, {x1, y1, z, 1, color}, {x0, y1, z, 1, color}};
      Cmd c = InternalCmd(k, 0x004 | 0x040, false);
      c.stencil_ref = uint8_t(stencil);
      c.scissor = {r.left, r.top, r.right - r.left, r.bottom - r.top};
      AddVertices(c, q, 6, sizeof(V));
      cmds_.push_back(c);
    }
  }

  void BeginScene() override {}
  void EndScene() override {}

  void Draw(uint32_t prim, uint32_t fvf, const void* vertices, uint32_t nv, uint32_t stride,
            const uint16_t* indices, uint32_t ni) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!vertices || !nv || !stride || (stride & 3)) return;
    uint32_t count = PrimitiveCountFromVertices(prim, indices ? ni : nv);
    if (!count) return;
    SDL_GPUPrimitiveType type;
    uint32_t n;  // vertices or indices used
    switch (prim) {
      case 1: type = SDL_GPU_PRIMITIVETYPE_POINTLIST; n = count; break;
      case 2: type = SDL_GPU_PRIMITIVETYPE_LINELIST; n = count * 2; break;
      case 3: type = SDL_GPU_PRIMITIVETYPE_LINESTRIP; n = count + 1; break;
      case 4: type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; n = count * 3; break;
      case 5: type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP; n = count + 2; break;
      case 6: type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; n = count + 2; break;  // fan
      default: return;
    }
    FvfLayout lay = DecodeFvf(fvf, stride);

    Cmd c{};
    c.key = DrawKey(type);
    if (!SetupScissor(c, lay.pos_kind == 1)) return;
    BuildUniforms();
    VsUniforms vu = vs_cur_;
    PatchLayout(vu, lay);
    c.vs = PushVs(vu);
    c.fs = PushFs(fs_cur_);
    BindStageTextures(c);
    c.stencil_ref = uint8_t(rs_[57]);
    c.viewport = lay.pos_kind == 1 ? FullViewport() : D3DViewport();

    uint32_t first_vertex = AppendVertices(vertices, nv, stride);
    // Indices as 32-bit, fans as lists, out-of-range ones clamped (D3D7
    // tolerates them; the shader would read other draws' vertices).
    if (indices || prim == 6) {
      c.indexed = true;
      c.first = uint32_t(indices_.size());
      auto at = [&](uint32_t i) -> uint32_t {
        uint32_t v = indices ? indices[i] : i;
        return v < nv ? v : nv - 1;
      };
      if (prim == 6) {
        for (uint32_t t = 0; t < count; t++) {
          indices_.push_back(at(0));
          indices_.push_back(at(t + 1));
          indices_.push_back(at(t + 2));
        }
        c.count = count * 3;
      } else {
        for (uint32_t i = 0; i < n; i++) indices_.push_back(at(i));
        c.count = n;
      }
      c.base = int32_t(first_vertex);
    } else {
      c.first = first_vertex;
      c.count = std::min(n, nv);
    }
    cmds_.push_back(c);
  }

  // Draw records into CPU arenas anyway (uploaded at the next flush), so
  // mapped draws are built in scratch memory and recorded like any other.
  Mapped MapDraw(uint32_t nv, uint32_t stride, uint32_t ni) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    mapped_ = {nv, stride, ni};
    mapped_vertices_.resize(size_t(nv) * stride);
    mapped_indices_.resize(ni);
    return {mapped_vertices_.data(), mapped_indices_.data()};
  }

  void DrawMapped(uint32_t prim, uint32_t fvf) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Draw(prim, fvf, mapped_vertices_.data(), mapped_.nv, mapped_.stride,
         mapped_indices_.data(), mapped_.ni);
  }

  void Present() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Flush();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureLocation s{}, d{};
    s.texture = backbuffer_;
    d.texture = front_;
    SDL_CopyGPUTextureToTexture(cp, &s, &d, Uint32(bb_w_), Uint32(bb_h_), 1, false);
    SDL_EndGPUCopyPass(cp);
    Show(cmd);
    Submit(cmd);
  }

 private:
  struct Tex {
    SDL_GPUTexture* tex = nullptr;
    int w = 0, h = 0, levels = 1;
    Format fmt = Format::Unknown;
    bool rt = false;
  };
  struct Depth {
    SDL_GPUTexture* tex = nullptr;
    int w = 0, h = 0;
    Format fmt = Format::Unknown;
    SDL_GPUTextureFormat gpu_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    bool stencil = false;
    float bias_scale = 8388608.0f;  // float depth: 2^23 per unit of depth range
  };
  struct Upload {
    SDL_GPUTexture* tex;
    int level;
    uint32_t x, y, w, h;
    uint32_t offset, pixels_per_row, rows;
    bool cycle;
  };
  struct Cmd {
    PipelineKey key;
    int vs = 0, fs = 0;
    SDL_GPUTexture* tex[8] = {};
    SDL_GPUSampler* smp[8] = {};
    SDL_GPUViewport viewport{};
    SDL_Rect scissor{};
    uint8_t stencil_ref = 0;
    bool indexed = false;
    uint32_t first = 0, count = 0;
    int32_t base = 0;
  };
  struct BlitTex {
    SDL_GPUTexture* tex;
    int w, h;
  };

  // ---- defaults (D3D9's, which the front ends rely on: they only send
  // what changes)

  void InitDefaults() {
    rs_.fill(0);
    rs_[8] = 3;            // FILLMODE solid
    rs_[9] = 2;            // SHADEMODE gouraud
    rs_[14] = 1;           // ZWRITEENABLE
    rs_[16] = 1;           // LASTPIXEL
    rs_[19] = 2;           // SRCBLEND one
    rs_[20] = 1;           // DESTBLEND zero
    rs_[22] = 3;           // CULLMODE ccw
    rs_[23] = 4;           // ZFUNC lessequal
    rs_[25] = 8;           // ALPHAFUNC always
    rs_[37] = FloatBits(1.0f);  // FOGEND
    rs_[38] = FloatBits(1.0f);  // FOGDENSITY
    rs_[53] = rs_[54] = rs_[55] = 1;  // stencil ops keep
    rs_[56] = 8;           // STENCILFUNC always
    rs_[58] = rs_[59] = 0xFFFFFFFFu;
    rs_[60] = 0xFFFFFFFFu;  // TEXTUREFACTOR
    rs_[136] = 1;          // CLIPPING
    rs_[137] = 1;          // LIGHTING
    rs_[141] = 1;          // COLORVERTEX
    rs_[142] = 1;          // LOCALVIEWER
    rs_[145] = 1;          // DIFFUSEMATERIALSOURCE color1
    rs_[146] = 2;          // SPECULARMATERIALSOURCE color2
    rs_[154] = FloatBits(1.0f);  // POINTSIZE
    rs_[168] = 0xF;        // COLORWRITEENABLE
    rs_[171] = 1;          // BLENDOP add
    for (uint32_t s = 0; s < 8; s++) {
      auto& t = tss_[s];
      t.fill(0);
      t[1] = s == 0 ? 4 : 1;   // COLOROP modulate / disable
      t[2] = 2;                // COLORARG1 texture
      t[3] = 1;                // COLORARG2 current
      t[4] = s == 0 ? 2 : 1;   // ALPHAOP selectarg1 / disable
      t[5] = 2;
      t[6] = 1;
      t[11] = s;               // TEXCOORDINDEX
      t[12] = t[13] = t[14] = 1;  // ADDRESS wrap
      t[16] = t[17] = 1;       // MAG/MINFILTER point
      t[18] = 1;               // MIPFILTER none
      t[21] = 1;               // MAXANISOTROPY
      t[26] = t[27] = t[28] = 1;  // COLORARG0, ALPHAARG0, RESULTARG current
    }
    for (Mat& m : world_) m = Identity();
    view_ = proj_ = Identity();
    for (Mat& m : tex_) m = Identity();
    for (auto& c : clip_) c.fill(0);
    material_ = Material{};
    viewport_ = {0, 0, 1, 1, 0, 1};
  }

  static Light DefaultLight() {
    Light l{};
    l.type = 3;
    l.diffuse = {1, 1, 1, 0};
    l.direction[2] = 1;
    return l;
  }

  // ---- render targets and passes

  SDL_GPUTexture* CreateColor(int w, int h, int levels) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = Uint32(w);
    ti.height = Uint32(h);
    ti.layer_count_or_depth = 1;
    ti.num_levels = Uint32(levels);
    SDL_GPUTexture* t = SDL_CreateGPUTexture(dev_, &ti);
    if (!t) FatalF("gfx: cannot create a %dx%d render target: %s", w, h, SDL_GetError());
    return t;
  }

  bool SupportsDepth(SDL_GPUTextureFormat f) {
    return SDL_GPUTextureSupportsFormat(dev_, f, SDL_GPU_TEXTURETYPE_2D,
                                        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
  }

  SDL_GPUTextureFormat GpuFormat(Format f) const {
    switch (f) {
      case Format::DXT1: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
      case Format::DXT2: case Format::DXT3: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
      case Format::DXT4: case Format::DXT5: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
      default: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    }
  }

  // The pass for the D3D render target (current_color_, current_depth_).
  void BindTarget() {
    SDL_GPUTexture* color = backbuffer_;
    int w = bb_w_, h = bb_h_;
    bool alpha = false;
    if (current_color_) {
      auto it = textures_.find(current_color_);
      if (it != textures_.end() && it->second.rt) {
        color = it->second.tex;
        w = it->second.w;
        h = it->second.h;
        alpha = HasAlpha(it->second.fmt);
      }
    }
    const Depth* d = nullptr;
    auto dit = depths_.find(current_depth_);
    if (dit != depths_.end()) d = &dit->second;
    SetPass(color, d, w, h, alpha);
  }

  void SetPass(SDL_GPUTexture* color, const Depth* d) {
    SetPass(color, d, bb_w_, bb_h_, false);
  }

  void SetPass(SDL_GPUTexture* color, const Depth* d, int w, int h, bool alpha) {
    SDL_GPUTexture* depth = d ? d->tex : nullptr;
    if (color == pass_color_ && depth == pass_depth_) return;
    Flush();
    pass_color_ = color;
    pass_depth_ = depth;
    pass_w_ = w;
    pass_h_ = h;
    pass_alpha_ = alpha;
    pass_stencil_ = d && d->stencil;
    pass_depth_format_ = d ? d->gpu_format : SDL_GPU_TEXTUREFORMAT_INVALID;
    pass_bias_scale_ = d ? d->bias_scale : 1.0f;
  }

  // Whether recorded draws or queued uploads use `t`.
  bool Pending(SDL_GPUTexture* t) const {
    if (t == pass_color_ && !cmds_.empty()) return true;
    for (const Upload& u : uploads_)
      if (u.tex == t) return true;
    for (const Cmd& c : cmds_)
      for (SDL_GPUTexture* x : c.tex)
        if (x == t) return true;
    return false;
  }

  // ---- uploads

  // Queues `rows` rows of `row_bytes` (pixel rows, or rows of 4x4 blocks
  // if `compressed`) for the texels at x, y, w, h of `level`.
  void QueueUpload(SDL_GPUTexture* tex, int level, int x, int y, int w, int h, const void* src,
                   uint32_t src_pitch, uint32_t rows, uint32_t row_bytes, bool cycle = false,
                   bool compressed = false) {
    size_t off = (upload_bytes_.size() + 15) & ~size_t(15);
    upload_bytes_.resize(off + size_t(rows) * row_bytes);
    for (uint32_t r = 0; r < rows; r++)
      std::memcpy(upload_bytes_.data() + off + size_t(r) * row_bytes,
                  static_cast<const uint8_t*>(src) + size_t(r) * src_pitch, row_bytes);
    Upload u{tex, level, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), uint32_t(off),
             uint32_t(w), uint32_t(h), cycle};
    if (compressed) {
      u.pixels_per_row = (uint32_t(w) + 3) & ~3u;
      u.rows = (uint32_t(h) + 3) & ~3u;
    }
    uploads_.push_back(u);
  }

  uint32_t AppendVertices(const void* v, uint32_t n, uint32_t stride) {
    size_t pos = (verts_.size() + stride - 1) / stride * stride;
    verts_.resize(pos + size_t(n) * stride);
    std::memcpy(verts_.data() + pos, v, size_t(n) * stride);
    return uint32_t(pos / stride);
  }

  SDL_GPUBuffer* EnsureBuffer(SDL_GPUBuffer*& b, uint32_t& cap, size_t need,
                              SDL_GPUBufferUsageFlags usage) {
    if (b && cap >= need) return b;
    if (b) SDL_ReleaseGPUBuffer(dev_, b);
    cap = std::max<uint32_t>(cap ? cap : 1u << 20, 1u << 20);
    while (cap < need) cap *= 2;
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = usage;
    bi.size = cap;
    b = SDL_CreateGPUBuffer(dev_, &bi);
    if (!b) FatalF("gfx: cannot create a %u-byte buffer: %s", cap, SDL_GetError());
    return b;
  }

  // ---- flush: one copy pass, then one render pass for the target

  // Submits cmd, keeping at most kMaxInFlight submissions unfinished.
  // Each flush maps the upload transfer buffer cycling: while earlier
  // submissions still use it, SDL gives the mapping a fresh buffer of the
  // same size and keeps it for reuse. Unbounded, a burst of flushes with
  // no present to pace it (every texture upload while a level loads) piled
  // up tens of GB of staging memory.
  void Submit(SDL_GPUCommandBuffer* cmd) {
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!f) FatalF("gfx: SDL_SubmitGPUCommandBuffer: %s", SDL_GetError());
    in_flight_.push_back(f);
    while (!in_flight_.empty() &&
           (in_flight_.size() > kMaxInFlight || SDL_QueryGPUFence(dev_, in_flight_.front()))) {
      SDL_WaitForGPUFences(dev_, true, &in_flight_.front(), 1);
      SDL_ReleaseGPUFence(dev_, in_flight_.front());
      in_flight_.pop_front();
    }
  }

  void Flush() {
    bool draw = !cmds_.empty() || pass_clear_color_ || pass_clear_depth_ || pass_clear_stencil_;
    if (!draw && uploads_.empty()) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    if (!cmd) FatalF("gfx: SDL_AcquireGPUCommandBuffer: %s", SDL_GetError());

    size_t vbytes = verts_.size(), ibytes = indices_.size() * 4;
    size_t ubytes = (upload_bytes_.size() + 15) & ~size_t(15);
    size_t total = ubytes + vbytes + ibytes;
    if (total) {
      if (!upload_tb_ || upload_tb_cap_ < total) {
        if (upload_tb_) SDL_ReleaseGPUTransferBuffer(dev_, upload_tb_);
        upload_tb_cap_ = std::max<uint32_t>(upload_tb_cap_ ? upload_tb_cap_ : 4u << 20, 4u << 20);
        while (upload_tb_cap_ < total) upload_tb_cap_ *= 2;
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = upload_tb_cap_;
        upload_tb_ = SDL_CreateGPUTransferBuffer(dev_, &tbi);
        if (!upload_tb_) FatalF("gfx: transfer buffer: %s", SDL_GetError());
      }
      auto* p = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(dev_, upload_tb_, true));
      if (!upload_bytes_.empty()) std::memcpy(p, upload_bytes_.data(), upload_bytes_.size());
      if (vbytes) std::memcpy(p + ubytes, verts_.data(), vbytes);
      if (ibytes) std::memcpy(p + ubytes + vbytes, indices_.data(), ibytes);
      SDL_UnmapGPUTransferBuffer(dev_, upload_tb_);

      SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
      for (const Upload& u : uploads_) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = upload_tb_;
        src.offset = u.offset;
        src.pixels_per_row = u.pixels_per_row;
        src.rows_per_layer = u.rows;
        SDL_GPUTextureRegion dst{};
        dst.texture = u.tex;
        dst.mip_level = Uint32(u.level);
        dst.x = u.x;
        dst.y = u.y;
        dst.w = u.w;
        dst.h = u.h;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, u.cycle);
      }
      if (vbytes) {
        EnsureBuffer(vbuf_, vbuf_cap_, vbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        SDL_GPUTransferBufferLocation src{upload_tb_, Uint32(ubytes)};
        SDL_GPUBufferRegion dst{vbuf_, 0, Uint32(vbytes)};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
      }
      if (ibytes) {
        EnsureBuffer(ibuf_, ibuf_cap_, ibytes, SDL_GPU_BUFFERUSAGE_INDEX);
        SDL_GPUTransferBufferLocation src{upload_tb_, Uint32(ubytes + vbytes)};
        SDL_GPUBufferRegion dst{ibuf_, 0, Uint32(ibytes)};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
      }
      SDL_EndGPUCopyPass(cp);
    }

    if (draw && pass_color_) {
      SDL_GPUColorTargetInfo ct{};
      ct.texture = pass_color_;
      ct.load_op = pass_clear_color_ ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
      ct.clear_color = clear_color_;
      ct.store_op = SDL_GPU_STOREOP_STORE;
      SDL_GPUDepthStencilTargetInfo dt{};
      if (pass_depth_) {
        dt.texture = pass_depth_;
        dt.load_op = pass_clear_depth_ ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.clear_depth = clear_depth_;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = pass_clear_stencil_ ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
        dt.clear_stencil = clear_stencil_;
      }
      SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, pass_depth_ ? &dt : nullptr);
      if (vbuf_) SDL_BindGPUVertexStorageBuffers(rp, 0, &vbuf_, 1);
      if (ibuf_ && ibytes) {
        SDL_GPUBufferBinding ib{ibuf_, 0};
        SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
      }
      SDL_GPUGraphicsPipeline* bound = nullptr;
      int vs = -1, fs = -1;
      for (const Cmd& c : cmds_) {
        SDL_GPUGraphicsPipeline* p = Pipeline(c.key);
        if (p != bound) {
          SDL_BindGPUGraphicsPipeline(rp, p);
          bound = p;
        }
        if (c.vs != vs) {
          SDL_PushGPUVertexUniformData(cmd, 0, &vs_list_[size_t(c.vs)], sizeof(VsUniforms));
          vs = c.vs;
        }
        if (c.fs != fs) {
          SDL_PushGPUFragmentUniformData(cmd, 0, &fs_list_[size_t(c.fs)], sizeof(FsUniforms));
          fs = c.fs;
        }
        SDL_GPUTextureSamplerBinding tb[8];
        for (int i = 0; i < 8; i++) tb[i] = {c.tex[i] ? c.tex[i] : dummy_, c.smp[i]};
        SDL_BindGPUFragmentSamplers(rp, 0, tb, 8);
        SDL_SetGPUViewport(rp, &c.viewport);
        SDL_SetGPUScissor(rp, &c.scissor);
        SDL_SetGPUStencilReference(rp, c.stencil_ref);
        if (c.indexed)
          SDL_DrawGPUIndexedPrimitives(rp, c.count, 1, c.first, c.base, 0);
        else
          SDL_DrawGPUPrimitives(rp, c.count, 1, c.first, 0);
      }
      SDL_EndGPURenderPass(rp);
    }
    Submit(cmd);

    cmds_.clear();
    uploads_.clear();
    upload_bytes_.clear();
    verts_.clear();
    indices_.clear();
    vs_list_.clear();
    fs_list_.clear();
    blit_used_ = 0;
    pass_clear_color_ = pass_clear_depth_ = pass_clear_stencil_ = false;
  }

  // Front buffer to the window: scaled to fit with its aspect kept,
  // black around it (FitRect).
  void Show(SDL_GPUCommandBuffer* cmd) {
    SDL_GPUTexture* sc = nullptr;
    Uint32 sw = 0, sh = 0;
    if (mailbox_emulated_) {
      // One frame per refresh, on a grid of refresh periods: a frame
      // before the next grid point stays in the front buffer unshown, so
      // the vsync queue never fills and acquiring does not wait. If it
      // does wait (the grid drifted ahead of the display), the grid
      // restarts from the moment a drawable came free.
      const uint64_t now = SDL_GetTicksNS();
      if (next_show_ns_ && now < next_show_ns_) return;
      next_show_ns_ = next_show_ns_ && now < next_show_ns_ + refresh_ns_
                          ? next_show_ns_ + refresh_ns_
                          : now + refresh_ns_;
      const bool ok = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &sc, &sw, &sh);
      const uint64_t after = SDL_GetTicksNS();
      if (after - now > refresh_ns_ / 4) next_show_ns_ = after + refresh_ns_;
      if (!ok || !sc) return;
    } else if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &sc, &sw, &sh) || !sc) {
      return;
    }
    const Rect r = FitRect(bb_w_, bb_h_, int(sw), int(sh));
    const int w = r.right - r.left, h = r.bottom - r.top;
    SDL_GPUBlitInfo b{};
    b.source.texture = front_;
    b.source.w = Uint32(bb_w_);
    b.source.h = Uint32(bb_h_);
    b.destination.texture = sc;
    b.destination.x = Uint32(r.left);
    b.destination.y = Uint32(r.top);
    b.destination.w = Uint32(w);
    b.destination.h = Uint32(h);
    b.load_op = SDL_GPU_LOADOP_DONT_CARE;
    if (w != int(sw) || h != int(sh)) {
      // The bars: clear the whole image (a blit's own clear is only
      // specified for its destination region), then blit over it.
      SDL_GPUColorTargetInfo t{};
      t.texture = sc;
      t.load_op = SDL_GPU_LOADOP_CLEAR;
      t.store_op = SDL_GPU_STOREOP_STORE;
      t.clear_color = {0, 0, 0, 1};
      SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &t, 1, nullptr));
      b.load_op = SDL_GPU_LOADOP_LOAD;
    }
    b.filter = (w == bb_w_ && h == bb_h_) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &b);
    NotePresentation({bb_w_, bb_h_, int(sw), int(sh), r});
  }

  void ReadTexture(SDL_GPUTexture* tex, int tw, int th, const Rect& r, void* dst, uint32_t pitch,
                   Format fmt) {
    Flush();
    Rect c = Clip(r, tw, th);
    int w = c.right - c.left, h = c.bottom - c.top;
    if (w <= 0 || h <= 0 || !tex) return;
    uint32_t bytes = uint32_t(w) * uint32_t(h) * 4;
    if (!download_tb_ || download_tb_cap_ < bytes) {
      if (download_tb_) SDL_ReleaseGPUTransferBuffer(dev_, download_tb_);
      download_tb_cap_ = std::max(bytes, 1u << 20);
      SDL_GPUTransferBufferCreateInfo tbi{};
      tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
      tbi.size = download_tb_cap_;
      download_tb_ = SDL_CreateGPUTransferBuffer(dev_, &tbi);
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = tex;
    src.x = Uint32(c.left);
    src.y = Uint32(c.top);
    src.w = Uint32(w);
    src.h = Uint32(h);
    src.d = 1;
    SDL_GPUTextureTransferInfo d{};
    d.transfer_buffer = download_tb_;
    d.pixels_per_row = Uint32(w);
    d.rows_per_layer = Uint32(h);
    SDL_DownloadFromGPUTexture(cp, &src, &d);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(dev_, true, &fence, 1);
    SDL_ReleaseGPUFence(dev_, fence);
    const auto* px = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(dev_, download_tb_, false));
    uint32_t bpp = BitsPerPixel(fmt) / 8;
    // Rows of the requested rectangle that fell outside the target stay
    // as they were.
    int ox = c.left - r.left, oy = c.top - r.top;
    for (int y = 0; y < h; y++) {
      const uint32_t* s = px + size_t(y) * uint32_t(w);
      uint8_t* out = static_cast<uint8_t*>(dst) + size_t(y + oy) * pitch + size_t(ox) * bpp;
      if (fmt == Format::X8R8G8B8 || fmt == Format::A8R8G8B8) {
        auto* o = reinterpret_cast<uint32_t*>(out);
        for (int x = 0; x < w; x++) o[x] = s[x] | 0xFF000000u;
      } else {
        for (int x = 0; x < w; x++) EncodePixel(out + size_t(x) * bpp, fmt, s[x] | 0xFF000000u);
      }
    }
    SDL_UnmapGPUTransferBuffer(dev_, download_tb_);
  }

  static void ConvertToArgb(const uint8_t* src, uint32_t pitch, Format f, int w, int h,
                            uint32_t* out) {
    uint32_t bpp = BitsPerPixel(f) / 8;
    for (int y = 0; y < h; y++) {
      const uint8_t* s = src + size_t(y) * pitch;
      uint32_t* d = out + size_t(y) * uint32_t(w);
      if (f == Format::A8R8G8B8) {
        std::memcpy(d, s, size_t(w) * 4);
      } else if (f == Format::X8R8G8B8) {
        const auto* p = reinterpret_cast<const uint32_t*>(s);
        for (int x = 0; x < w; x++) d[x] = p[x] | 0xFF000000u;
      } else {
        for (int x = 0; x < w; x++) d[x] = DecodePixel(s + size_t(x) * bpp, f);
      }
    }
  }

  // ---- DirectDraw blits: a textured screen quad with private state

  void DrawImage(const void* src, uint32_t pitch, Format fmt, const Rect& sr, const Rect& dr,
                 bool colorkey, uint32_t key) {
    int w = sr.right - sr.left, h = sr.bottom - sr.top;
    if (w <= 0 || h <= 0 || dr.right <= dr.left || dr.bottom <= dr.top) return;
    std::vector<uint32_t> px(size_t(w) * h);
    uint32_t bpp = BitsPerPixel(fmt) / 8;
    const uint8_t* base = static_cast<const uint8_t*>(src) + size_t(sr.top) * pitch +
                          size_t(sr.left) * bpp;
    if (!colorkey) {
      ConvertToArgb(base, pitch, fmt, w, h, px.data());
      for (uint32_t& p : px) p |= 0xFF000000u;
    } else {
      for (int y = 0; y < h; y++) {
        const uint8_t* s = base + size_t(y) * pitch;
        for (int x = 0; x < w; x++, s += bpp)
          px[size_t(y) * w + x] = RawPixel(s, fmt) == key ? 0 : (DecodePixel(s, fmt) | 0xFF000000u);
      }
    }
    // A pool texture per blit in this flush.
    if (blit_used_ == blit_pool_.size()) blit_pool_.push_back({nullptr, 0, 0});
    size_t slot = blit_used_++;
    if (blit_pool_[slot].w < w || blit_pool_[slot].h < h) {
      if (blit_pool_[slot].tex) {
        Flush();  // recorded blits may still use the old one
        SDL_ReleaseGPUTexture(dev_, blit_pool_[slot].tex);
        blit_used_ = slot + 1;
      }
      BlitTex& b = blit_pool_[slot];
      b.w = std::max({w, b.w, 256});
      b.h = std::max({h, b.h, 256});
      SDL_GPUTextureCreateInfo ti{};
      ti.type = SDL_GPU_TEXTURETYPE_2D;
      ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
      ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
      ti.width = Uint32(b.w);
      ti.height = Uint32(b.h);
      ti.layer_count_or_depth = 1;
      ti.num_levels = 1;
      b.tex = SDL_CreateGPUTexture(dev_, &ti);
      if (!b.tex) FatalF("gfx: blit texture %dx%d: %s", b.w, b.h, SDL_GetError());
    }
    const BlitTex& bt = blit_pool_[slot];
    QueueUpload(bt.tex, 0, 0, 0, w, h, px.data(), uint32_t(w) * 4, uint32_t(h),
                uint32_t(w) * 4, true);

    PipelineKey k = BaseKey(SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    Cmd c = InternalCmd(k, 0x004 | 0x100, true, colorkey);
    c.tex[0] = bt.tex;
    SamplerKey sk{};
    sk.min = sk.mag = SDL_GPU_FILTER_LINEAR;
    sk.mip = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sk.u = sk.v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sk.aniso = 1;
    c.smp[0] = Sampler(sk);
    Rect clip = Intersect(dr, {0, 0, pass_w_, pass_h_});
    if (clip.right <= clip.left || clip.bottom <= clip.top) return;
    c.scissor = {clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top};
    float u1 = float(w) / float(bt.w), v1 = float(h) / float(bt.h);
    struct V {
      float x, y, z, rhw, u, v;
    };
    // Pixel edges, in D3D9's integer-centred coordinates.
    float x0 = float(dr.left) - 0.5f, y0 = float(dr.top) - 0.5f;
    float x1 = float(dr.right) - 0.5f, y1 = float(dr.bottom) - 0.5f;
    V q[6] = {{x0, y0, 0, 1, 0, 0},   {x1, y0, 0, 1, u1, 0}, {x0, y1, 0, 1, 0, v1},
              {x1, y0, 0, 1, u1, 0},  {x1, y1, 0, 1, u1, v1}, {x0, y1, 0, 1, 0, v1}};
    AddVertices(c, q, 6, sizeof(V));
    cmds_.push_back(c);
  }

  // A draw of our own (clear quads, blits): pre-transformed vertices,
  // stage 0 selecting the texture or the diffuse colour, nothing else.
  Cmd InternalCmd(const PipelineKey& k, uint32_t fvf, bool textured, bool alpha_test = false) {
    Cmd c{};
    c.key = k;
    VsUniforms vu{};
    FvfLayout lay = DecodeFvf(fvf, fvf & 0x100 ? 24 : 20);
    PatchLayout(vu, lay);
    vu.target[0] = float(pass_w_);
    vu.target[1] = float(pass_h_);
    vu.mode[0] = 1;
    for (int s = 0; s < 8; s++) vu.tci[s] = s;
    c.vs = PushVs(vu);
    FsUniforms fu{};
    int arg = textured ? 2 : 0;
    fu.color[0][0] = 2;
    fu.color[0][1] = arg;
    fu.alpha[0][0] = 2;
    fu.alpha[0][1] = arg;
    fu.stage[0][1] = textured ? 1 : 0;
    for (int s = 0; s < 8; s++) fu.stage[s][2] = -1;
    for (int s = 1; s < 8; s++) fu.color[s][0] = fu.alpha[s][0] = 1;
    if (alpha_test) {
      fu.mode[0] = 5;  // GREATER 0: colour-keyed texels have alpha 0
      fu.mode[1] = 0;
    }
    c.fs = PushFs(fu);
    SamplerKey sk{};
    sk.aniso = 1;
    for (int i = 0; i < 8; i++) c.smp[i] = Sampler(sk);
    c.viewport = FullViewport();
    c.scissor = {0, 0, pass_w_, pass_h_};
    return c;
  }

  void AddVertices(Cmd& c, const void* v, uint32_t n, uint32_t stride) {
    c.indexed = false;
    c.first = AppendVertices(v, n, stride);
    c.count = n;
  }

  // ---- per-draw state

  PipelineKey BaseKey(SDL_GPUPrimitiveType prim) const {
    PipelineKey k;
    std::memset(&k, 0, sizeof k);
    k.prim = uint8_t(prim);
    k.fill = SDL_GPU_FILLMODE_FILL;
    k.cull = SDL_GPU_CULLMODE_NONE;
    k.front = SDL_GPU_FRONTFACE_CLOCKWISE;
    k.src = SDL_GPU_BLENDFACTOR_ONE;
    k.dst = SDL_GPU_BLENDFACTOR_ZERO;
    k.op = SDL_GPU_BLENDOP_ADD;
    k.write_mask = 0xF;
    k.depth_func = SDL_GPU_COMPAREOP_ALWAYS;
    k.s_fail = k.s_zfail = k.s_pass = SDL_GPU_STENCILOP_KEEP;
    k.s_func = SDL_GPU_COMPAREOP_ALWAYS;
    k.depth_format = uint8_t(pass_depth_format_);
    return k;
  }

  PipelineKey DrawKey(SDL_GPUPrimitiveType prim) const {
    PipelineKey k = BaseKey(prim);
    if (rs_[8] != 3) k.fill = SDL_GPU_FILLMODE_LINE;
    bool tri = prim == SDL_GPU_PRIMITIVETYPE_TRIANGLELIST ||
               prim == SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    // D3D culls by screen winding: CW (2) or CCW (3) triangles.
    if (tri && rs_[22] == 2) {
      k.cull = SDL_GPU_CULLMODE_BACK;
      k.front = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    } else if (tri && rs_[22] == 3) {
      k.cull = SDL_GPU_CULLMODE_BACK;
      k.front = SDL_GPU_FRONTFACE_CLOCKWISE;
    }
    if (rs_[27]) {
      k.blend = 1;
      uint32_t s = rs_[19], d = rs_[20];
      if (s == 12) { s = 5; d = 6; }        // BOTHSRCALPHA
      else if (s == 13) { s = 6; d = 5; }   // BOTHINVSRCALPHA
      k.src = uint8_t(BlendFactor(s, pass_alpha_));
      k.dst = uint8_t(BlendFactor(d, pass_alpha_));
      k.op = uint8_t(BlendOp(rs_[171]));
    }
    k.write_mask = uint8_t(rs_[168] & 0xF);
    if (pass_depth_) {
      k.depth_test = rs_[7] ? 1 : 0;
      k.depth_write = (rs_[7] && rs_[14]) ? 1 : 0;
      k.depth_func = uint8_t(CompareOp(rs_[23]));
      if (pass_stencil_ && rs_[52]) {
        k.stencil = 1;
        k.s_fail = uint8_t(StencilOp(rs_[53]));
        k.s_zfail = uint8_t(StencilOp(rs_[54]));
        k.s_pass = uint8_t(StencilOp(rs_[55]));
        k.s_func = uint8_t(CompareOp(rs_[56]));
        k.s_read = uint8_t(rs_[58]);
        k.s_write = uint8_t(rs_[59]);
      }
      // D3D9's DEPTHBIAS is in depth-range units; D3D7's ZBIAS 0..16
      // as the d3d9 backend maps it.
      float bias = AsFloat(rs_[195]) - float(rs_[47]) * 0.000005f;
      k.bias_const = bias * pass_bias_scale_;
      k.bias_slope = AsFloat(rs_[175]);
    }
    return k;
  }

  SDL_GPUGraphicsPipeline* Pipeline(const PipelineKey& k) {
    auto it = pipelines_.find(k);
    if (it != pipelines_.end()) return it->second;
    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ctd.blend_state.enable_blend = k.blend != 0;
    ctd.blend_state.src_color_blendfactor = SDL_GPUBlendFactor(k.src);
    ctd.blend_state.dst_color_blendfactor = SDL_GPUBlendFactor(k.dst);
    ctd.blend_state.color_blend_op = SDL_GPUBlendOp(k.op);
    ctd.blend_state.src_alpha_blendfactor = SDL_GPUBlendFactor(k.src);
    ctd.blend_state.dst_alpha_blendfactor = SDL_GPUBlendFactor(k.dst);
    ctd.blend_state.alpha_blend_op = SDL_GPUBlendOp(k.op);
    ctd.blend_state.color_write_mask = k.write_mask;
    ctd.blend_state.enable_color_write_mask = true;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs_;
    pi.fragment_shader = fs_;
    pi.primitive_type = SDL_GPUPrimitiveType(k.prim);
    pi.rasterizer_state.fill_mode = SDL_GPUFillMode(k.fill);
    pi.rasterizer_state.cull_mode = SDL_GPUCullMode(k.cull);
    pi.rasterizer_state.front_face = SDL_GPUFrontFace(k.front);
    pi.rasterizer_state.enable_depth_clip = true;
    if (k.bias_const != 0 || k.bias_slope != 0) {
      pi.rasterizer_state.enable_depth_bias = true;
      pi.rasterizer_state.depth_bias_constant_factor = k.bias_const;
      pi.rasterizer_state.depth_bias_slope_factor = k.bias_slope;
    }
    pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    auto& ds = pi.depth_stencil_state;
    ds.enable_depth_test = k.depth_test != 0;
    ds.enable_depth_write = k.depth_write != 0;
    ds.compare_op = SDL_GPUCompareOp(k.depth_func);
    ds.enable_stencil_test = k.stencil != 0;
    SDL_GPUStencilOpState so{SDL_GPUStencilOp(k.s_fail), SDL_GPUStencilOp(k.s_pass),
                             SDL_GPUStencilOp(k.s_zfail), SDL_GPUCompareOp(k.s_func)};
    ds.front_stencil_state = ds.back_stencil_state = so;
    ds.compare_mask = k.s_read;
    ds.write_mask = k.s_write;
    pi.target_info.color_target_descriptions = &ctd;
    pi.target_info.num_color_targets = 1;
    pi.target_info.has_depth_stencil_target = k.depth_format != SDL_GPU_TEXTUREFORMAT_INVALID;
    pi.target_info.depth_stencil_format = SDL_GPUTextureFormat(k.depth_format);
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
    if (!p) FatalF("gfx: SDL_CreateGPUGraphicsPipeline: %s", SDL_GetError());
    pipelines_[k] = p;
    return p;
  }

  SDL_GPUSampler* Sampler(const SamplerKey& k) {
    auto it = samplers_.find(k);
    if (it != samplers_.end()) return it->second;
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPUFilter(k.min);
    si.mag_filter = SDL_GPUFilter(k.mag);
    si.mipmap_mode = SDL_GPUSamplerMipmapMode(k.mip);
    si.address_mode_u = SDL_GPUSamplerAddressMode(k.u);
    si.address_mode_v = SDL_GPUSamplerAddressMode(k.v);
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.mip_lod_bias = k.lod_bias;
    si.min_lod = k.min_lod;
    // MIPFILTER NONE samples only the top level (or MAXMIPLEVEL).
    si.max_lod = k.mip == 0xFF ? k.min_lod : 1000.0f;
    if (k.mip == 0xFF) si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.enable_anisotropy = k.aniso > 1;
    si.max_anisotropy = float(k.aniso);
    SDL_GPUSampler* s = SDL_CreateGPUSampler(dev_, &si);
    if (!s) FatalF("gfx: SDL_CreateGPUSampler: %s", SDL_GetError());
    samplers_[k] = s;
    return s;
  }

  SamplerKey StageSampler(int s) const {
    const auto& t = tss_[s];
    SamplerKey k{};
    k.mag = (t[16] == 1) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    k.min = (t[17] == 1) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    k.mip = t[18] == 2 ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST
          : t[18] == 3 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : 0xFF;
    k.u = uint8_t(AddressMode(t[13]));
    k.v = uint8_t(AddressMode(t[14]));
    bool aniso = t[16] == 5 || t[17] == 3;
    k.aniso = uint8_t(aniso ? std::clamp<uint32_t>(t[21], 1, 16) : 1);
    k.lod_bias = AsFloat(t[19]);
    k.min_lod = float(t[20]);
    return k;
  }

  void BindStageTextures(Cmd& c) {
    for (int s = 0; s < 8; s++) {
      auto it = textures_.find(stage_tex_[s]);
      c.tex[s] = it != textures_.end() ? it->second.tex : nullptr;
      c.smp[s] = Sampler(StageSampler(s));
    }
  }

  // ---- uniforms from the D3D state

  void PatchLayout(VsUniforms& u, const FvfLayout& l) const {
    u.layout[0] = l.stride;
    u.layout[1] = 0;
    u.layout[2] = l.normal;
    u.layout[3] = l.diffuse;
    u.layout2[0] = l.specular;
    u.layout2[1] = l.psize;
    u.layout2[2] = l.weights;
    u.layout2[3] = 0;
    for (int i = 0; i < 8; i++) {
      u.tex_offset[i] = l.tex_offset[i];
      u.tex_size[i] = l.tex_size[i] ? l.tex_size[i] : 2;
    }
    u.mode[0] = l.pos_kind;
  }

  void BuildUniforms() {
    if (vs_dirty_) {
      VsUniforms& u = vs_cur_;
      std::memset(&u, 0, sizeof u);
      for (int i = 0; i < 4; i++) {
        Mat wv = Mul(world_[i], view_);
        std::memcpy(u.world_view[i], wv.data(), 64);
        Mat nm = NormalMatrix(wv);
        std::memcpy(u.normal[i], nm.data(), 64);
      }
      std::memcpy(u.proj, proj_.data(), 64);
      for (int i = 0; i < 8; i++) std::memcpy(u.tex[i], tex_[i].data(), 64);
      u.viewport[0] = float(viewport_.x);
      u.viewport[1] = float(viewport_.y);
      u.viewport[2] = float(std::max(viewport_.width, 1u));
      u.viewport[3] = float(std::max(viewport_.height, 1u));
      u.target[0] = float(pass_w_);
      u.target[1] = float(pass_h_);
      u.target[2] = proj_[11] != 0 ? 1.0f : 0.0f;  // _34: perspective
      Argb(u.ambient, rs_[139]);
      Vec4(u.mat_diffuse, material_.diffuse);
      Vec4(u.mat_ambient, material_.ambient);
      Vec4(u.mat_specular, material_.specular);
      Vec4(u.mat_emissive, material_.emissive);
      u.mat_power[0] = material_.power;
      u.fog[0] = AsFloat(rs_[36]);
      u.fog[1] = AsFloat(rs_[37]);
      u.fog[2] = AsFloat(rs_[38]);
      // Clip planes are in world space; the shader has view space.
      Mat vinv = Inverse(view_);
      for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) {
          float s = 0;
          for (int k = 0; k < 4; k++) s += vinv[j * 4 + k] * clip_[i][k];
          u.clip[i][j] = s;
        }
      int n = 0;
      for (uint32_t idx : lights_on_) {
        if (n == 8) break;
        const Light& l = lights_[idx];
        GpuLight& g = u.lights[n++];
        Vec4(g.diffuse, l.diffuse);
        Vec4(g.specular, l.specular);
        Vec4(g.ambient, l.ambient);
        float p[4] = {l.position[0], l.position[1], l.position[2], 1};
        float d[4] = {l.direction[0], l.direction[1], l.direction[2], 0};
        for (int c = 0; c < 3; c++) {
          g.position[c] = p[0] * view_[c] + p[1] * view_[4 + c] + p[2] * view_[8 + c] + view_[12 + c];
          g.direction[c] = -(d[0] * view_[c] + d[1] * view_[4 + c] + d[2] * view_[8 + c]);
        }
        float len = std::sqrt(g.direction[0] * g.direction[0] + g.direction[1] * g.direction[1] +
                              g.direction[2] * g.direction[2]);
        if (len > 0)
          for (int c = 0; c < 3; c++) g.direction[c] /= len;
        g.position[3] = l.range > 0 ? l.range : 1e30f;
        g.direction[3] = l.falloff;
        g.atten[0] = l.attenuation0;
        g.atten[1] = l.attenuation1;
        g.atten[2] = l.attenuation2;
        g.atten[3] = float(l.type);
        g.spot[0] = std::cos(l.theta / 2);
        g.spot[1] = std::cos(l.phi / 2);
      }
      u.mode[1] = rs_[137] ? 1 : 0;
      u.mode[2] = rs_[143] ? 1 : 0;
      u.mode[3] = rs_[142] ? 1 : 0;
      u.mode2[0] = rs_[141] ? 1 : 0;
      u.mode2[1] = int(rs_[145]);
      u.mode2[2] = int(rs_[146]);
      u.mode2[3] = int(rs_[147]);
      u.mode3[0] = int(rs_[148]);
      u.mode3[1] = int(rs_[140]);
      u.mode3[2] = int(rs_[35]);
      u.mode3[3] = rs_[48] ? 1 : 0;
      u.mode4[0] = rs_[29] ? 1 : 0;
      uint32_t vb = rs_[151];
      u.mode4[1] = (vb >= 1 && vb <= 3) ? int(vb) : 0;
      u.mode4[2] = int(rs_[152] & 0xFF);
      u.mode4[3] = n;
      u.mode5[0] = rs_[28] ? 1 : 0;
      for (int s = 0; s < 8; s++) {
        u.tci[s] = int(tss_[s][11]);
        u.ttf[s] = int(tss_[s][24]);
      }
      u.point[0] = AsFloat(rs_[154]);
      vs_dirty_ = false;
    }
    if (fs_dirty_) {
      FsUniforms& u = fs_cur_;
      std::memset(&u, 0, sizeof u);
      for (int s = 0; s < 8; s++) {
        const auto& t = tss_[s];
        u.color[s][0] = int(t[1]);
        u.color[s][1] = int(t[2]);
        u.color[s][2] = int(t[3]);
        u.color[s][3] = int(t[26]);
        u.alpha[s][0] = int(t[4]);
        u.alpha[s][1] = int(t[5]);
        u.alpha[s][2] = int(t[6]);
        u.alpha[s][3] = int(t[27]);
        u.stage[s][0] = int(t[28]);
        u.stage[s][1] = textures_.count(stage_tex_[s]) ? 1 : 0;
        uint32_t ttf = t[24], count = ttf & 0xFF;
        u.stage[s][2] = (ttf & 256) && count >= 2 ? int(count) - 1 : -1;
      }
      Argb(u.tfactor, rs_[60]);
      Argb(u.fog_color, rs_[34]);
      u.fog[0] = AsFloat(rs_[36]);
      u.fog[1] = AsFloat(rs_[37]);
      u.fog[2] = AsFloat(rs_[38]);
      u.mode[0] = rs_[15] ? int(rs_[25]) : 0;
      u.mode[1] = int(rs_[24] & 0xFF);
      u.mode[2] = rs_[28] ? (rs_[35] ? 2 : 1) : 0;
      u.mode[3] = int(rs_[35]);
      u.mode2[0] = rs_[9] == 1 ? 1 : 0;
      u.mode2[1] = rs_[29] ? 1 : 0;
      fs_dirty_ = false;
    }
    // The render target size feeds the vertex shader too.
    vs_cur_.target[0] = float(pass_w_);
    vs_cur_.target[1] = float(pass_h_);
  }

  int PushVs(const VsUniforms& u) {
    if (vs_list_.empty() || std::memcmp(&vs_list_.back(), &u, sizeof u) != 0)
      vs_list_.push_back(u);
    return int(vs_list_.size()) - 1;
  }

  int PushFs(const FsUniforms& u) {
    if (fs_list_.empty() || std::memcmp(&fs_list_.back(), &u, sizeof u) != 0)
      fs_list_.push_back(u);
    return int(fs_list_.size()) - 1;
  }

  // ---- rectangles

  static Rect Intersect(const Rect& a, const Rect& b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
            std::min(a.bottom, b.bottom)};
  }
  static Rect Clip(const Rect& r, int w, int h) { return Intersect(r, {0, 0, w, h}); }

  Rect ViewportRect() const {
    Rect v{int32_t(viewport_.x), int32_t(viewport_.y), int32_t(viewport_.x + viewport_.width),
           int32_t(viewport_.y + viewport_.height)};
    return Clip(v, pass_w_, pass_h_);
  }

  SDL_GPUViewport D3DViewport() const {
    return {float(viewport_.x), float(viewport_.y), float(std::max(viewport_.width, 1u)),
            float(std::max(viewport_.height, 1u)), viewport_.min_z, viewport_.max_z};
  }
  SDL_GPUViewport FullViewport() const {
    return {0, 0, float(pass_w_), float(pass_h_), 0, 1};
  }

  // Scissor of a draw: the target, the scissor rect if enabled, and for
  // pre-transformed vertices (drawn with a full-target viewport) the
  // viewport. False if nothing is left.
  bool SetupScissor(Cmd& c, bool rhw) const {
    Rect r{0, 0, pass_w_, pass_h_};
    if (rs_[174]) r = Intersect(r, scissor_);
    if (rhw) r = Intersect(r, ViewportRect());
    if (r.right <= r.left || r.bottom <= r.top) return false;
    c.scissor = {r.left, r.top, r.right - r.left, r.bottom - r.top};
    return true;
  }

  // ---- members

  std::recursive_mutex mu_;
  SDL_Window* window_;
  SDL_GPUDevice* dev_ = nullptr;
  SDL_GPUShader* vs_ = nullptr;
  SDL_GPUShader* fs_ = nullptr;
  SDL_GPUTextureFormat depth24_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  SDL_GPUTextureFormat depth24s8_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
  SDL_GPUTexture* backbuffer_ = nullptr;  // the game's back buffer
  SDL_GPUTexture* front_ = nullptr;       // the DirectDraw primary
  SDL_GPUTexture* dummy_ = nullptr;
  int bb_w_ = 0, bb_h_ = 0;
  // Mailbox emulated on vsync (ResetSwapChain, Show).
  bool mailbox_emulated_ = false;
  uint64_t refresh_ns_ = 0, next_show_ns_ = 0;
  std::map<TextureId, Tex> textures_;
  std::map<DepthId, Depth> depths_;
  uint32_t next_id_ = 1;
  TextureId current_color_ = 0;
  DepthId current_depth_ = 0;

  // The pass being recorded.
  SDL_GPUTexture* pass_color_ = nullptr;
  SDL_GPUTexture* pass_depth_ = nullptr;
  int pass_w_ = 0, pass_h_ = 0;
  bool pass_alpha_ = false, pass_stencil_ = false;
  SDL_GPUTextureFormat pass_depth_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  float pass_bias_scale_ = 1.0f;
  bool pass_clear_color_ = false, pass_clear_depth_ = false, pass_clear_stencil_ = false;
  SDL_FColor clear_color_{0, 0, 0, 1};
  float clear_depth_ = 1;
  uint8_t clear_stencil_ = 0;
  std::vector<Cmd> cmds_;
  std::vector<VsUniforms> vs_list_;
  std::vector<FsUniforms> fs_list_;
  std::vector<uint8_t> verts_;
  std::vector<uint32_t> indices_;
  std::vector<Upload> uploads_;
  std::vector<uint8_t> upload_bytes_;
  std::vector<BlitTex> blit_pool_;
  struct {
    uint32_t nv, stride, ni;
  } mapped_{};
  std::vector<uint8_t> mapped_vertices_;
  std::vector<uint16_t> mapped_indices_;
  size_t blit_used_ = 0;

  SDL_GPUBuffer* vbuf_ = nullptr;
  SDL_GPUBuffer* ibuf_ = nullptr;
  uint32_t vbuf_cap_ = 0, ibuf_cap_ = 0;
  SDL_GPUTransferBuffer* upload_tb_ = nullptr;
  static constexpr size_t kMaxInFlight = 4;
  std::deque<SDL_GPUFence*> in_flight_;  // submitted, not known finished
  SDL_GPUTransferBuffer* download_tb_ = nullptr;
  uint32_t upload_tb_cap_ = 0, download_tb_cap_ = 0;
  std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline*, PipelineKeyHash> pipelines_;
  std::map<SamplerKey, SDL_GPUSampler*> samplers_;

  // D3D state.
  std::array<uint32_t, 256> rs_{};
  std::array<std::array<uint32_t, 33>, 8> tss_{};
  std::array<TextureId, 8> stage_tex_{};
  std::array<Mat, 4> world_;
  Mat view_, proj_;
  std::array<Mat, 8> tex_;
  std::array<std::array<float, 4>, 8> clip_{};
  Viewport viewport_{};
  Rect scissor_{};
  Material material_{};
  std::map<uint32_t, Light> lights_;
  std::set<uint32_t> lights_on_;
  bool vs_dirty_ = true, fs_dirty_ = true;
  VsUniforms vs_cur_{};
  FsUniforms fs_cur_{};
};

}  // namespace

std::unique_ptr<Device> CreateGpuDevice(SDL_Window* window) {
  auto d = std::make_unique<GpuDevice>(window);
  if (!d->Init()) return nullptr;
  return d;
}

}  // namespace recomp::gfx
