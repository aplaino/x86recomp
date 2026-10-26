#pragma once
// Backend-neutral rendering device with Direct3D 7 fixed-function
// semantics. The ddraw/d3d7 and OpenGL HLE talk only to this; backends
// translate:
//   - d3d9 via dxvk-native (gfx/d3d9_device.cpp): Linux bring-up and the
//     reference image;
//   - SDL_GPU with fixed-function uber-shaders (gfx/gpu_device.cpp):
//     Metal on macOS; Vulkan on Linux once its shaders exist.
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
  // Console GPUs' formats (programs, below), stored as they are in memory:
  // components named from the lowest bits, so R8G8B8A8 is red first and
  // R11G11B10F has red in bits 0-10. Uploaded and read back unconverted.
  R8, R8G8, R8G8B8A8, R8G8B8A8Srgb, B8G8R8A8Srgb, R16, R16G16, R16G16B16A16,
  R10G10B10A2, R11G11B10F, R8G8B8A8Snorm, R16G16Snorm, R16G16B16A16Snorm,
  R8Uint, R8G8Uint, R8G8B8A8Uint, R16Uint, R16G16Uint, R16G16B16A16Uint, R32Uint,
  R32G32Uint, R32G32B32A32Uint, R32Sint, R32G32Sint, R32G32B32A32Sint,
  Bc1, Bc1Srgb, Bc2, Bc2Srgb, Bc3, Bc3Srgb, Bc4, Bc5, Bc6hUfloat, Bc6hSfloat, Bc7, Bc7Srgb,
  D32FS8,
};
uint32_t BitsPerPixel(Format f);
bool IsCompressed(Format f);
bool IsFloat(Format f);
// Formats uploaded and read back as they are (the float and console ones).
bool IsRaw(Format f);
// Bytes per row of `width` pixels (per row of 4x4 blocks if compressed).
uint32_t RowPitch(Format f, uint32_t width);
// Rows in the data for `height` (block rows if compressed).
uint32_t RowCount(Format f, uint32_t height);

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
  // Samplers 0-15, then the vertex samplers 0-3: texture (0: none) and
  // D3DSAMPLERSTATETYPE values (index 1-13).
  TextureId textures[20] = {};
  const uint32_t (*sampler_states)[14] = nullptr;
};

// ---- programs: console GPUs' shaders, which their front ends translate
// to GLSL (the third way to draw, beside the fixed-function pipeline and
// Direct3D 9's shaders). The device puts a prelude before each stage's
// GLSL: `#version 450`, the binding macros for the stage's resources and
// their declarations:
//   - textures (combined image and sampler): `layout(R_TEXTURE(n))
//     uniform sampler2D ...;` for n < textures, of the kind the GLSL says;
//   - read-only buffers: their words in `r_mem[]`, buffer b from word
//     r_buf[b].x, r_buf[b].y bytes long;
//   - compute's read-write buffers likewise in `r_rw[]` and r_rwbuf[b];
//     its storage images `layout(R_IMAGE(n), <format>) uniform image2D`;
//   - the stage's own uniform block: `layout(R_UNIFORMS) uniform ...`.
using ProgramId = uint32_t;  // 0: none

enum class TextureKind : uint8_t { Tex2D, Tex2DArray, Cube, Tex3D };

struct ProgramStage {
  std::string glsl;  // empty: no such stage
  uint32_t textures = 0, buffers = 0;
  uint32_t rw_buffers = 0, rw_images = 0;  // compute only
  uint32_t uniform_bytes = 0;              // 0: no uniform block
  TextureKind texture_kinds[16] = {};      // what an unbound texture is
};
// Vertex and fragment stages, or a compute one (cs) with its workgroup
// size.
struct ProgramDesc {
  ProgramStage vs, fs, cs;
  uint32_t local_size[3] = {1, 1, 1};
};

// A texture or render target for programs: `layers` array layers (6 for
// a cube) or a 3D texture's depth. A storage image can be a compute
// stage's read-write image.
struct ImageDesc {
  int w = 1, h = 1, layers = 1, levels = 1;
  Format fmt = Format::R8G8B8A8;
  TextureKind kind = TextureKind::Tex2D;
  bool color_target = false, storage = false;
};

enum class Topology : uint8_t { PointList, LineList, LineStrip, TriangleList, TriangleStrip };
enum class BlendFactor : uint8_t {
  Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor, SrcAlpha,
  OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, ConstantColor, OneMinusConstantColor,
  SrcAlphaSaturate,
};
enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
enum class CompareOp : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class StencilOp : uint8_t {
  Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap,
};
enum class CullMode : uint8_t { None, Front, Back };
enum class AddressMode : uint8_t { Repeat, MirroredRepeat, ClampToEdge };

struct TargetBlend {
  bool enable = false;
  BlendFactor src_color = BlendFactor::One, dst_color = BlendFactor::Zero;
  BlendOp color_op = BlendOp::Add;
  BlendFactor src_alpha = BlendFactor::One, dst_alpha = BlendFactor::Zero;
  BlendOp alpha_op = BlendOp::Add;
  uint8_t write_mask = 0xF;  // bit 0 red .. bit 3 alpha
};
struct StencilFace {
  StencilOp fail = StencilOp::Keep, depth_fail = StencilOp::Keep, pass = StencilOp::Keep;
  CompareOp compare = CompareOp::Always;
};
struct PipelineState {
  Topology topology = Topology::TriangleList;
  CullMode cull = CullMode::None;
  bool front_ccw = false;
  bool depth_test = false, depth_write = false, depth_clip = true;
  CompareOp depth_compare = CompareOp::Always;
  float depth_bias = 0, depth_bias_slope = 0, depth_bias_clamp = 0;
  bool stencil = false;
  StencilFace stencil_front, stencil_back;
  uint8_t stencil_read_mask = 0xFF, stencil_write_mask = 0xFF, stencil_ref = 0;
  TargetBlend blend[4];
  Color blend_constant{0, 0, 0, 0};
};
struct SamplerDesc {
  bool min_linear = false, mag_linear = false, mip_linear = false;
  AddressMode u = AddressMode::Repeat, v = AddressMode::Repeat, w = AddressMode::Repeat;
  float lod_bias = 0, min_lod = 0, max_lod = 1000, max_anisotropy = 1;
  bool compare = false;
  CompareOp compare_op = CompareOp::Always;
};

struct BufferData {  // copied when the draw or dispatch is recorded
  const void* data = nullptr;
  uint32_t size = 0;
};
struct RwBufferData {  // read, and written back once the dispatch has run
  void* data = nullptr;
  uint32_t size = 0;
};
// A stage's resources for one draw or dispatch, as many as the stage
// declares.
struct ProgramResources {
  const TextureId* textures = nullptr;
  const SamplerDesc* samplers = nullptr;  // one per texture
  const BufferData* buffers = nullptr;
  const void* uniforms = nullptr;  // uniform_bytes
};
// Targets are ImageDesc color targets (tex 0 is no target here, not the
// back buffer); the scissor is always on.
struct ProgramDraw {
  ProgramId program = 0;
  PipelineState state;
  ColorTarget targets[4];
  DepthId depth = 0;
  Viewport viewport{};
  Rect scissor{};
  uint32_t vertex_count = 0, instance_count = 1;
  int32_t base_vertex = 0;
  const void* indices = nullptr;  // vertex_count of them; null: not indexed
  uint32_t index_size = 2;
  ProgramResources vs, fs;
};
struct ProgramDispatch {
  ProgramId program = 0;
  uint32_t groups[3] = {1, 1, 1};
  ProgramResources cs;
  const RwBufferData* rw_buffers = nullptr;
  const ColorTarget* rw_images = nullptr;  // storage images, at a level
};

class Device {
 public:
  virtual ~Device() = default;

  // (Re)creates the swap chain: a w x h back buffer presented to the
  // window. Called on SetDisplayMode / device creation.
  virtual void ResetSwapChain(int w, int h, bool vsync) = 0;

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

  // ---- programmable pipeline. A shader from the HLSL translation (null
  // on failure); render states (blend, depth, stencil, cull, colour
  // write, alpha test, scissor), the viewport and the targets as set
  // above, the rest per draw.
  virtual ShaderId CreateShader(const hlsl::Program& p) = 0;
  virtual void DestroyShader(ShaderId s) = 0;
  virtual void DrawShader(const ShaderDraw& d) = 0;

  // ---- programs (see ProgramDesc). Backends without them (d3d9) fail
  // fatally. A program that does not compile is fatal too, with its
  // source in the log.
  virtual ProgramId CreateProgram(const ProgramDesc& p);
  virtual void DestroyProgram(ProgramId p);
  virtual TextureId CreateImage(const ImageDesc& d);
  // Uploads `r` of a level and layer of an image, as it is in memory.
  virtual void UpdateImage(TextureId t, int level, int layer, const Rect& r, const void* src,
                           uint32_t pitch);
  // Reads `r` of a level and layer of an image back as it is (waits for
  // the GPU).
  virtual void ReadImage(TextureId t, int level, int layer, const Rect& r, void* dst,
                         uint32_t pitch);
  // Clear a colour image's level (all layers) or a depth target (depth
  // and, if `stencil` >= 0, stencil).
  virtual void ClearImage(TextureId t, int level, const Color& c);
  virtual void ClearDepth(DepthId d, float z, int stencil);
  virtual void DrawProgram(const ProgramDraw& d);
  // Runs at once (with what was drawn before it) when it has read-write
  // buffers, which it writes back before returning.
  virtual void DispatchProgram(const ProgramDispatch& d);
  // Draws `r` of a level 0 colour image into the back buffer from its
  // top-left corner, for Present to show: its texels as they are stored,
  // so an sRGB image's encoded values.
  virtual void DrawImage(TextureId t, const Rect& r);
};

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
};
// Null if `b` is not in this build or cannot start on this host.
std::unique_ptr<Device> CreateDevice(Backend b, SDL_Window* window);

std::unique_ptr<Device> CreateD3D9Device(SDL_Window* window);
std::unique_ptr<Device> CreateGpuDevice(SDL_Window* window);

}  // namespace recomp::gfx
