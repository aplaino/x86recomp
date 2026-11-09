#pragma once
// Backend-neutral rendering device with Direct3D 7 fixed-function
// semantics. The ddraw/d3d7 and OpenGL HLE talk only to this; backends
// translate:
//   - d3d9 via dxvk-native (gfx/d3d9_device.cpp): Linux bring-up and the
//     reference image;
//   - SDL_GPU with fixed-function uber-shaders (gfx/gpu_device.cpp):
//     Metal on macOS; Vulkan on Linux once its shaders exist;
//   - Metal directly (gfx/metal_device.mm, Apple platforms): Direct3D 9's
//     programmable pipeline only (HasFixedFunction false).
// Numeric enums (render states, texture stage states, transform types,
// primitive types, FVF codes) are D3D7's; render states D3D7 lacks use
// D3D9's numbers (COLORWRITEENABLE 168, SCISSORTESTENABLE 174,
// SLOPESCALEDEPTHBIAS 175, DEPTHBIAS 195), and FVFs may use D3D9's
// XYZW position (0x4002: homogeneous, untransformed). Plain data structs mirror the
// D3D7 ones (which D3D9 also kept unchanged).
//
// Direct3D 9's programmable pipeline (shaders translated from HLSL,
// runtime/hlsl) is a second way to draw: CreateShader / DrawShader, with
// up to four render targets (SetRenderTargets). Its render states are
// D3D9's (also COLORWRITEENABLE1-3 190-192, BLENDFACTOR 193,
// SEPARATEALPHABLENDENABLE 206 and the alpha blend 207-209). The d3d9
// backend does not have it.

#include <cstdint>
#include <memory>
#include <string>

#include "runtime/hlsl/hlsl.h"

struct SDL_Window;

namespace recomp::gfx {

enum class Format : uint32_t {
  Unknown, X8R8G8B8, A8R8G8B8, R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4,
  X4R4G4B4, R3G3B2, A8, L8, A8L8, P8,
  D16, D24X8, D24S8, D32, D15S1,
  DXT1, DXT2, DXT3, DXT4, DXT5,
  // Floating point, in D3D9's memory layout (red first): uploaded and
  // stored as is.
  R16F, G16R16F, A16B16G16R16F, R32F, G32R32F, A32B32G32R32F,
};
uint32_t BitsPerPixel(Format f);
bool IsCompressed(Format f);
bool IsFloat(Format f);
// Bytes per row of `width` pixels (per row of 4x4 blocks if compressed).
uint32_t RowPitch(Format f, uint32_t width);
// Rows in the data for `height` (block rows if compressed).
uint32_t RowCount(Format f, uint32_t height);
// A DXT1-5 image (`pitch` bytes per row of 4x4 blocks) decoded into w x h
// A8R8G8B8, for GPUs that cannot sample BC formats (the A15 of an Apple TV).
void DecodeDxt(const uint8_t* src, uint32_t pitch, Format f, int w, int h, uint32_t* out);

// Single-pixel conversion between uncompressed colour formats and 32-bit
// A8R8G8B8 (missing alpha reads as 0xFF). For blits and readbacks.
uint32_t DecodePixel(const uint8_t* p, Format f);
void EncodePixel(uint8_t* p, Format f, uint32_t argb);
// Raw pixel value (as stored) for colour keys and fills.
uint32_t RawPixel(const uint8_t* p, Format f);
void PutRawPixel(uint8_t* p, Format f, uint32_t raw);

struct Rect {
  int32_t left, top, right, bottom;
};

struct Viewport {
  uint32_t x, y, width, height;
  float min_z, max_z;
};

struct Color {
  float r, g, b, a;
};

struct Material {
  Color diffuse, ambient, specular, emissive;
  float power;
};

struct Light {
  uint32_t type;
  Color diffuse, specular, ambient;
  float position[3], direction[3];
  float range, falloff, attenuation0, attenuation1, attenuation2, theta, phi;
};

using TextureId = uint32_t;  // 0: none
using DepthId = uint32_t;    // 0: none
using ShaderId = uint32_t;   // 0: none

// A colour target: a mip level of a render-target texture, or the back
// buffer (tex 0, level 0). A level is max(1, size >> level) on each side.
struct ColorTarget {
  TextureId tex = 0;
  int level = 0;
};

// ---- the programmable pipeline (Direct3D 9 shaders translated from HLSL)

// Where a vertex shader input (hlsl::Program::inputs slot) comes from: a
// stream, the byte offset in its vertices and the D3DDECLTYPE; type 17
// (D3DDECLTYPE_UNUSED) if the vertex declaration does not supply it.
struct VertexInput {
  uint32_t stream = 0, offset = 0, type = 17;
};

struct VertexStream {
  const uint8_t* data = nullptr;  // vertex 0 (the buffer at its stream offset)
  uint32_t size = 0;              // bytes readable from data
  uint32_t stride = 0;
};

// A Direct3D 9 draw with shaders. Vertex v of a stream is at data +
// v * stride; v is index + base_vertex (indexed) or base_vertex + i, i <
// vertex_count (not indexed). Indexed draws read only vertices
// base_vertex + min_index .. + vertex_count - 1.
struct ShaderDraw {
  ShaderId vs = 0, ps = 0;
  uint32_t prim = 0;        // D3DPRIMITIVETYPE
  uint32_t primitives = 0;  // primitive count
  const VertexInput* inputs = nullptr;  // one per vertex shader input slot
  uint32_t input_count = 0;
  VertexStream streams[16];
  int32_t base_vertex = 0;
  uint32_t min_index = 0, vertex_count = 0;
  const void* indices = nullptr;  // null: not indexed
  uint32_t index_size = 2;        // bytes
  // Constant registers: float c (vs 256, ps 224), int i (16 x 4), bool b
  // (16).
  const float (*vs_float)[4] = nullptr;
  const int32_t (*vs_int)[4] = nullptr;
  const uint32_t* vs_bool = nullptr;
  const float (*ps_float)[4] = nullptr;
  const int32_t (*ps_int)[4] = nullptr;
  const uint32_t* ps_bool = nullptr;
  // Change counters of vs_float and ps_float (0: unknown): a backend need
  // not compare registers whose counter has not moved.
  uint64_t vs_float_version = 0, ps_float_version = 0;
  // The same for the int and bool registers together.
  uint64_t vs_ib_version = 0, ps_ib_version = 0;
  // Samplers 0-15, then the vertex samplers 0-3: texture (0: none) and
  // D3DSAMPLERSTATETYPE values (index 1-13).
  TextureId textures[20] = {};
  const uint32_t (*sampler_states)[14] = nullptr;
  // Change counters of the sampler_states rows (0: unknown): a backend
  // need not convert a row whose counter has not moved.
  const uint64_t* sampler_versions = nullptr;
  // State changed since the last draw, applied first as SetRenderState,
  // SetViewport and SetScissor would (one call and one lock per draw):
  // `state_count` (state, value) pairs, and the viewport and scissor
  // rectangle where not null.
  const uint32_t (*states)[2] = nullptr;
  uint32_t state_count = 0;
  const Viewport* viewport = nullptr;
  const Rect* scissor = nullptr;
};

class Device {
 public:
  virtual ~Device() = default;

  // Whether Draw / MapDraw (the fixed-function pipeline) and the 2D blits
  // work. Without them, front ends draw fixed-function state with shaders
  // of their own.
  virtual bool HasFixedFunction() const { return true; }

  // ---- memory the CPU and the GPU share (Apple's unified memory)
  // Whether the device can read host memory in place (MapSharedMemory).
  virtual bool SharesMemory() const { return false; }
  // Lets the device read [host, host + size) in place: draws whose vertex
  // and index data lie there do not copy it. Page-aligned; the memory must
  // stay mapped for the device's lifetime. False if it cannot.
  virtual bool MapSharedMemory(void* host, size_t size) {
    (void)host, (void)size;
    return false;
  }
  // Frames, for memory read in place: what draws recorded now belong to,
  // and the last one the GPU has finished (everything up to it). Data a
  // draw reads in place must not change until its frame has finished.
  virtual uint64_t FrameSerial() const { return 0; }
  virtual uint64_t CompletedSerial() const { return 0; }

  // GPU time per presented frame, in ms, averaged over the frames the GPU
  // finished since the last call; negative if the backend cannot tell.
  virtual double TakeGpuFrameMs() { return -1; }
  // Memory the GPU allocations take, in bytes (-1: unknown).
  virtual int64_t GpuAllocatedBytes() const { return -1; }
  // What the GPU allocations are, for the FPS report (empty: not known).
  virtual std::string GpuMemoryText() const { return {}; }
  // Intervals between frames as the display showed them, in ms, since the
  // last call: median, 99th percentile and maximum; false if the backend
  // cannot tell.
  // Milliseconds from Present to the display, since the last call: median
  // and 99th percentile; false if the backend cannot tell.
  virtual bool TakePresentLatency(double* median, double* p99) {
    (void)median, (void)p99;
    return false;
  }
  virtual bool TakeShownIntervals(double* median, double* p99, double* max) {
    (void)median, (void)p99, (void)max;
    return false;
  }
  // Counters for tuning (the runtime's <ID>_PERF): kept once enabled;
  // TakePerfStats says what they show since the last call, per presented
  // frame ("" from a backend without them).
  virtual void EnablePerfStats() {}
  virtual std::string TakePerfStats() { return {}; }

  // (Re)creates the swap chain: a w x h back buffer presented to the
  // window. Called on SetDisplayMode / device creation.
  virtual void ResetSwapChain(int w, int h, bool vsync) = 0;
  // Whether the back buffer keeps alpha (A8R8G8B8, A2R10G10B10): blending
  // with its destination alpha then reads it, as a game masking with it
  // expects (GTA IV's round radar); without, alpha reads 1.
  virtual void SetBackBufferAlpha(bool) {}

  // ---- resources
  virtual TextureId CreateTexture(int w, int h, int levels, Format fmt,
                                  bool render_target) = 0;
  virtual void DestroyTexture(TextureId t) = 0;
  // Fills `r` of a render-target texture's top level with an A8R8G8B8
  // colour (DirectDraw colour-fill Blt; its memory is GPU-only).
  virtual void FillTexture(TextureId t, const Rect& r, uint32_t argb) = 0;
  // Uploads `r` of mip `level` from linear memory (pitch in bytes).
  virtual void UpdateTexture(TextureId t, int level, const Rect& r,
                             const void* src, uint32_t pitch) = 0;
  // A cube texture: six square faces in D3D's order (+X, -X, +Y, -Y, +Z,
  // -Z), filled with UpdateCubeTexture and freed with DestroyTexture.
  virtual TextureId CreateCubeTexture(int size, int levels, Format fmt) = 0;
  virtual void UpdateCubeTexture(TextureId t, int face, int level, const Rect& r,
                                 const void* src, uint32_t pitch) = 0;
  virtual DepthId CreateDepth(int w, int h, Format fmt) = 0;
  // A depth-stencil buffer (D24S8) that shaders can also sample, Direct3D 9's
  // INTZ: the TextureId SetTexture binds (depth in every channel read) and,
  // in *depth, the DepthId render targets use. DestroyTexture releases it.
  virtual TextureId CreateDepthTexture(int w, int h, DepthId* depth) {
    *depth = 0;
    (void)w, (void)h;
    return 0;
  }
  // A depth texture of a depth-stencil format (D24S8, D16) rather than
  // INTZ: Direct3D 9 compares a read of it with the coordinate's depth
  // (hardware shadow maps; R_SHADOW, hlsl.h). Backends without R_SHADOW
  // read it as INTZ.
  virtual void SetDepthCompare(TextureId t, bool on) { (void)t, (void)on; }
  virtual void DestroyDepth(DepthId d) = 0;

  // ---- render target (color 0 = back buffer; Present copies it to the
  // front buffer and shows that)
  virtual void SetRenderTarget(TextureId color, DepthId depth) = 0;
  // Up to four colour targets drawn at once (shaders' COLOR0-3): color[0]
  // as SetRenderTarget's (or a level of it), the others texture levels or
  // tex 0 (none, and none after the first one).
  virtual void SetRenderTargets(const ColorTarget color[4], DepthId depth) = 0;
  // Copies `sr` of a colour target to `dr` of another, scaled with linear
  // or nearest filtering (Direct3D 9's StretchRect).
  virtual void StretchRect(ColorTarget src, const Rect& sr, ColorTarget dst, const Rect& dr,
                           bool linear) = 0;

  // ---- 2D (DirectDraw Blt onto the back buffer / render target)
  // Copies `src_rect` of a linear image into `dst_rect` of the current
  // render target, stretching if the sizes differ. If `colorkey`, source
  // pixels equal to `key` (in the source format) are skipped.
  virtual void Blit(const void* src, uint32_t pitch, Format fmt,
                    const Rect& src_rect, const Rect& dst_rect, bool colorkey,
                    uint32_t key) = 0;
  // Draws `src_rect` of an A8R8G8B8 image over `dst_rect` of the current
  // render target, blended by the image's alpha (source over, straight
  // alpha). For what a host hook draws in place of a game's own CPU
  // blending onto read-back pixels (a software cursor), which would make
  // the CPU wait for the frame's GPU work.
  virtual void BlitBlend(const void* src, uint32_t pitch,
                         const Rect& src_rect, const Rect& dst_rect) = 0;
  // Reads `r` of the current render target into linear memory.
  virtual void ReadBack(const Rect& r, void* dst, uint32_t pitch,
                        Format fmt) = 0;
  // ReadBack in two steps, so the CPU need not wait for the GPU:
  // StartReadBack takes `r` of the current render target as the draws so
  // far leave it and returns a ticket (never 0); FinishReadBack waits for
  // that copy and converts it as ReadBack does, or only drops it if `dst`
  // is null. Every ticket is finished exactly once.
  virtual uint32_t StartReadBack(const Rect& r) = 0;
  virtual void FinishReadBack(uint32_t ticket, void* dst, uint32_t pitch,
                              Format fmt) = 0;

  // ---- front buffer (the DirectDraw primary): the last presented frame,
  // which the game may also read and draw on between frames (its cursor).
  virtual void BlitFront(const void* src, uint32_t pitch, Format fmt,
                         const Rect& src_rect, const Rect& dst_rect) = 0;
  virtual void ReadBackFront(const Rect& r, void* dst, uint32_t pitch,
                             Format fmt) = 0;
  // Shows the front buffer as it is now.
  virtual void PresentFront() = 0;

  // ---- fixed-function state (D3D7 enums)
  virtual void SetRenderState(uint32_t state, uint32_t value) = 0;
  virtual void SetTextureStageState(uint32_t stage, uint32_t type,
                                    uint32_t value) = 0;
  virtual void SetTexture(uint32_t stage, TextureId t) = 0;
  virtual void SetTransform(uint32_t type, const float m[16]) = 0;
  virtual void SetViewport(const Viewport& v) = 0;
  // In render-target pixels; used while SCISSORTESTENABLE (174) is set.
  virtual void SetScissor(const Rect& r) = 0;
  virtual void SetMaterial(const Material& m) = 0;
  virtual void SetLight(uint32_t index, const Light& l) = 0;
  virtual void LightEnable(uint32_t index, bool enable) = 0;
  virtual void SetClipPlane(uint32_t index, const float plane[4]) = 0;

  // ---- drawing
  virtual void Clear(uint32_t count, const Rect* rects, uint32_t flags,
                     uint32_t color, float z, uint32_t stencil) = 0;
  virtual void BeginScene() = 0;
  virtual void EndScene() = 0;
  // Non-indexed when `indices` is null. `stride` from the FVF.
  virtual void Draw(uint32_t prim, uint32_t fvf, const void* vertices,
                    uint32_t vertex_count, uint32_t stride,
                    const uint16_t* indices, uint32_t index_count) = 0;
  // Indexed draw built in place: MapDraw returns room for `vertex_count`
  // vertices (`stride` bytes each) and `index_count` indices in memory the
  // device reads them from, for the caller to fill and then draw with
  // DrawMapped before the next MapDraw. Saves Draw's copy for vertices
  // made per draw.
  struct Mapped {
    uint8_t* vertices;
    uint16_t* indices;
  };
  virtual Mapped MapDraw(uint32_t vertex_count, uint32_t stride,
                         uint32_t index_count) = 0;
  virtual void DrawMapped(uint32_t prim, uint32_t fvf) = 0;
  virtual void Present() = 0;
  // Post-process anti-aliasing (SMAA 1x) of each presented frame, where the
  // backend has it (SDL_GPU).
  virtual void SetPostAntialiasing(bool on) { (void)on; }
  // Anisotropic filtering of at least `level` (2..16; 0: as the game sets
  // it) for mipmapped textures sampled with linear filtering.
  virtual void SetMinAnisotropy(int level) { (void)level; }
  // Depth from w (Metal: gfx/reversed_z.h): the vertex shader constants
  // c[wvp_register]..+3 where the game keeps its world-view-projection
  // (rows, D3D's row vectors; -1: none), perspective draws whose position
  // is made from them getting their depth from w alone; and, optional,
  // the near and far planes of the projection the game draws with at the
  // time of the call (false if it cannot tell), exact where the matrix's
  // are rounded. Called from the drawing thread, at a draw.
  using DepthPlanes = bool (*)(float* near_plane, float* far_plane);
  virtual void SetDepthFromW(int wvp_register, DepthPlanes planes) { (void)wvp_register, (void)planes; }
  // Decode DXT textures on upload even where the GPU samples BC formats
  // (to check the path GPUs without them take). Before any texture exists.
  virtual void DecodeDxtOnUpload() {}

  // ---- programmable pipeline. A shader from the HLSL translation (null
  // on failure); render states (blend, depth, stencil, cull, colour
  // write, alpha test, scissor), the viewport and the targets as set
  // above, the rest per draw.
  virtual ShaderId CreateShader(const hlsl::Program& p) = 0;
  virtual void DestroyShader(ShaderId s) = 0;
  virtual void DrawShader(const ShaderDraw& d) = 0;
};

// A ShaderDraw's state changes through the setters, for backends that
// take them no faster way.
inline void ApplyDrawState(Device& dev, const ShaderDraw& d) {
  for (uint32_t i = 0; i < d.state_count; i++) dev.SetRenderState(d.states[i][0], d.states[i][1]);
  if (d.viewport) dev.SetViewport(*d.viewport);
  if (d.scissor) dev.SetScissor(*d.scissor);
}

// Where a src_w x src_h frame is shown in a dst_w x dst_h output: scaled
// to fit with its aspect kept and centred, black around it (letterbox or
// pillarbox). Backends present the front buffer through it; the window
// system maps the pointer back through it.
Rect FitRect(int src_w, int src_h, int dst_w, int dst_h);

// The last frame any device showed, in pixels: the frame's size, the
// output's (swap chain) and FitRect's result. Zeros before the first.
struct Presentation {
  int src_w, src_h, out_w, out_h;
  Rect rect;
};
// Backends call this when they show the front buffer.
void NotePresentation(const Presentation& p);
Presentation LastPresentation();

// Vertex size in bytes for a D3D7 FVF code.
uint32_t FvfStride(uint32_t fvf);
uint32_t PrimitiveCountFromVertices(uint32_t prim_type, uint32_t verts);

enum class Backend {
  D3D9,  // dxvk-native; only in builds with RECOMP_HAVE_D3D9
  Gpu,   // SDL_GPU
  Metal,  // Metal directly; only on Apple platforms
};
// Null if `b` is not in this build or cannot start on this host.
std::unique_ptr<Device> CreateDevice(Backend b, SDL_Window* window);

std::unique_ptr<Device> CreateD3D9Device(SDL_Window* window);
std::unique_ptr<Device> CreateGpuDevice(SDL_Window* window);
std::unique_ptr<Device> CreateMetalDevice(SDL_Window* window);

}  // namespace recomp::gfx
