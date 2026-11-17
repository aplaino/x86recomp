#pragma once
// Direct3D 11's host objects (d3d11.cpp, d3d11_objects.cpp,
// d3d11_context.cpp, d3d11_gpu.cpp, d3d11_program.cpp, dxgi.cpp).
// Resources, views, states and shaders are records of what the guest
// created: descriptions and data, made into gfx resources when the device
// first draws with them (d3d11_gpu.cpp).

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "runtime/gfx/device.h"
#include "win32/com.h"

namespace recomp::hle::d3d11 {

constexpr uint32_t kS_OK = 0, kS_FALSE = 1, kE_INVALIDARG = 0x80070057, kE_FAIL = 0x80004005,
                   kE_OUTOFMEMORY = 0x8007000E, kDXGI_ERROR_NOT_FOUND = 0x887A0002,
                   kDXGI_ERROR_INVALID_CALL = 0x887A0001;

struct Device;

// Every D3D11 object but the device: private data (SetPrivateData, by
// GUID) and its device (GetDevice). `serial` names the object for caches
// (never reused, unlike its address).
struct Child : com::Object {
  Child();
  std::map<std::string, std::vector<uint8_t>> private_data;  // by the GUID's bytes
  uint64_t serial;
};

enum class Dim : uint32_t {  // D3D11_RESOURCE_DIMENSION
  Unknown = 0, Buffer = 1, Texture1D = 2, Texture2D = 3, Texture3D = 4,
};

// D3D11_USAGE.
enum class Usage : uint32_t { Default = 0, Immutable = 1, Dynamic = 2, Staging = 3 };

constexpr uint32_t kBindVertexBuffer = 0x1, kBindIndexBuffer = 0x2, kBindConstantBuffer = 0x4,
                   kBindShaderResource = 0x8, kBindStreamOutput = 0x10, kBindRenderTarget = 0x20,
                   kBindDepthStencil = 0x40, kBindUnorderedAccess = 0x80;
constexpr uint32_t kMiscGenerateMips = 0x1, kMiscTextureCube = 0x4,
                   kMiscBufferAllowRawViews = 0x20, kMiscBufferStructured = 0x40;

// Guest memory holding a resource's contents (zeroed when made), freed
// with the last reference. A resource has one current block; Map with
// WRITE_DISCARD gives it a fresh one when anything else still holds the
// current one (a command list that installs it, below), so what was
// recorded with the old one keeps seeing it: buffer renaming.
struct Block {
  explicit Block(uint64_t size);
  ~Block();
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;
  uint8_t* Data() const;
  gaddr mem = 0;
  uint64_t size = 0;
};

// A buffer or texture. Its contents live in guest memory (its Block:
// every subresource one after the other, tightly packed), so Map hands
// the guest a pointer into them; creation, Map, UpdateSubresource and
// copies on the CPU write them. The GPU's copy (an image, a depth target
// or a GPU buffer, d3d11_gpu.cpp) is made the first time a draw, clear or
// copy needs it and is brought up to date from the guest memory before
// each use (`dirty`); what the GPU writes (render targets, GPU copies) is
// read back into the guest memory only when the guest maps the resource
// or copies from it on the CPU (`gpu_newer`).
struct Resource : Child {
  ~Resource() override;
  Dim dim = Dim::Unknown;
  Usage usage = Usage::Default;
  uint32_t bind = 0, cpu_access = 0, misc = 0;
  // Buffers.
  uint32_t byte_width = 0, stride = 0;
  // Textures.
  uint32_t width = 1, height = 1, depth = 1, mips = 1, array = 1, format = 0;
  uint32_t samples = 1, quality = 0;
  uint64_t size = 0;

  // The current contents. Deferred contexts read it from other threads.
  std::shared_ptr<Block> Contents() const;
  void SetContents(std::shared_ptr<Block> b);
  gaddr Mem() const { return Contents()->mem; }
  uint8_t* Data() const { return Contents()->Data(); }

  uint64_t version = 0;  // bumped on every CPU write

  // The subresource's offset in data and its row pitch and slice pitch.
  uint64_t SubOffset(uint32_t sub) const;
  uint32_t RowPitch(uint32_t mip) const;
  uint32_t SlicePitch(uint32_t mip) const;
  uint32_t MipWidth(uint32_t mip) const { return std::max(width >> mip, 1u); }
  uint32_t MipHeight(uint32_t mip) const { return std::max(height >> mip, 1u); }
  uint32_t MipDepth(uint32_t mip) const { return std::max(depth >> mip, 1u); }
  uint32_t Subresources() const { return dim == Dim::Buffer ? 1 : mips * array; }

  // The formats of the first views made of it: a typeless resource is
  // made on the GPU in the format it is drawn with (gfx has no views).
  uint32_t rtv_format = 0, view_format = 0;
  uint32_t srv_dim = 0;  // the first shader resource view's dimension

  // ---- the GPU side (d3d11_gpu.cpp), under Device::gpu_mu
  gfx::TextureId image = 0;  // a texture that is not a depth target
  gfx::DepthId depth_target = 0;
  gfx::BufferId gpu_buffer = 0;  // a buffer read in place by draws
  uint32_t gfx_format = 0;       // gfx::Format of `image`
  std::vector<uint8_t> dirty;    // per subresource: CPU writes `image` lacks
  uint64_t dirty_lo = 0, dirty_hi = 0;  // a GPU buffer's bytes it lacks
  bool gpu_newer = false;        // GPU writes the guest memory lacks
  bool host_reads = false;       // a buffer draws copy from guest memory
  uint64_t upload_frame = 0;     // the last frame a GPU buffer was refilled in
  uint32_t uploads = 0;          // ... and how many times in it

 private:
  mutable std::mutex mu_;
  std::shared_ptr<Block> block_;
};

// A view of a resource (SRV, RTV, DSV, UAV): its description as given.
enum class ViewKind { ShaderResource, RenderTarget, DepthStencil, UnorderedAccess };
struct View : Child {
  ViewKind kind;
  std::shared_ptr<Resource> res;
  uint32_t format = 0, view_dim = 0;
  uint32_t first_mip = 0, mips = 1, first_slice = 0, slices = 1;
  uint32_t first_element = 0, elements = 0, flags = 0;  // buffer views
  std::vector<uint8_t> desc;  // its description, as given or as Windows fills it in
};

struct Shader : Child {
  uint32_t stage = 0;  // 0 vs, 1 ps, 2 cs; 3 hs, 4 ds, 5 gs (kept, not drawn with)
  std::vector<uint8_t> dxbc;
  uint64_t hash = 0;
};

struct InputLayout : Child {
  struct Element {
    std::string semantic;
    uint32_t index = 0, format = 0, slot = 0, offset = 0, per_instance = 0, step_rate = 0;
  };
  std::vector<Element> elements;
};

// The states keep their description's bytes.
enum class StateKind { Blend, DepthStencil, Rasterizer, Sampler };
struct State : Child {
  StateKind kind;
  std::vector<uint8_t> desc;
};

struct Query : Child {
  uint32_t type = 0, misc = 0;
  bool ended = false;
};

// A context's pipeline state, as the guest set it.
constexpr int kStages = 3;  // vs, ps, cs (no geometry or tessellation)
struct ContextState {
  struct VertexBuffer {
    std::shared_ptr<Resource> res;
    uint32_t stride = 0, offset = 0;
  };
  struct ConstantBuffer {
    std::shared_ptr<Resource> res;
    uint32_t first = 0, count = 0;  // in 16-byte constants; count 0: all
  };
  struct StageState {
    std::shared_ptr<Shader> shader;
    ConstantBuffer cbs[14];
    std::shared_ptr<View> srvs[128];
    std::shared_ptr<State> samplers[16];
  };
  StageState stage[kStages];
  bool geometry = false;  // a geometry shader is bound: draws are skipped
  std::shared_ptr<View> uavs[8];  // compute
  std::shared_ptr<InputLayout> layout;
  uint32_t topology = 0;
  VertexBuffer vbs[32];
  std::shared_ptr<Resource> ib;
  uint32_t ib_format = 0, ib_offset = 0;
  std::shared_ptr<View> rtvs[8];
  std::shared_ptr<View> dsv;
  std::shared_ptr<State> blend, depth_stencil, rasterizer;
  float blend_factor[4] = {1, 1, 1, 1};
  uint32_t sample_mask = 0xFFFFFFFF, stencil_ref = 0;
  struct Viewport {
    float x = 0, y = 0, w = 0, h = 0, min_z = 0, max_z = 1;
  };
  Viewport viewports[16];
  uint32_t num_viewports = 0;
  int32_t scissors[16][4] = {};
  uint32_t num_scissors = 0;
};

struct Context;
// What a deferred context records: an operation on the context it runs
// on (the immediate one, by ExecuteCommandList). State changes apply to
// the recording context at once too, for its Get* methods.
using Op = std::function<void(Context&)>;

// The immediate context or a deferred one (CreateDeferredContext). A
// deferred context records state changes, draws, clears, copies,
// updates and the blocks its Maps hand out (renamed, so each recorded
// draw keeps the contents it was recorded with) until FinishCommandList
// makes them a command list.
struct Context : Child, ContextState {
  bool deferred = false;
  std::vector<Op> ops;
  // A deferred context's blocks mapped in this recording: WRITE_NO_OVERWRITE
  // appends to them.
  std::map<Resource*, std::shared_ptr<Block>> mapped;
};

// FinishCommandList's result: the operations in the order recorded.
struct CommandList : Child {
  std::vector<Op> ops;
};

// ---- shader programs (d3d11_link.cpp, d3d11_program.cpp)
//
// A draw needs one gfx program for its vertex shader, pixel shader and
// input layout (a dispatch for its compute shader). LinkProgram makes it
// from the shaders' translations (dxbc.h) and says what each of the
// program's gfx resources is among Direct3D's bindings; the layer calls
// it once per combination (d3d11_program.cpp keeps the result, null
// included, by the objects' serials) and binds the resources it names
// for every draw. A null program (a shader that is not translated) skips
// the draw, counted in the frame's trace line (Present).
//
// The binding plan (StageBindings), per stage, in the program's order:
//   - read-only buffers (gfx::ProgramStage::buffers, `r_ld(b, word)`):
//     constant buffers (the bound buffer's bytes from its start, padded
//     with zeros to at least `bytes`), buffer SRVs (the view's bytes from
//     its first element on: its first element times the structure stride
//     (structured), 4 (raw) or the format's size (typed)) and, in the
//     vertex stage, vertex buffers (the bytes from the binding's offset
//     on, read by the link code's r_fetch; a draw may bind only the
//     vertices it reads of a buffer it copies, StageUniforms::ia);
//   - textures (combined image and sampler, `R_TEXTURE(n)`): the SRV
//     slot and the sampler slot it is sampled with, -1 for none (texel
//     loads and size queries: point sampling);
//   - compute: read-write buffers and storage images, by UAV slot;
//   - `formats`: the typed buffer views (and a compute stage's typed
//     buffer UAVs) whose DXGI formats the stage's uniforms carry, for the
//     translation's R_FMT_T<slot> and R_FMT_U<slot>;
//   - `uniforms`: the stage reads the layer's uniform block (R_UNIFORMS,
//     StageUniforms).
struct StageBindings {
  enum class BufferKind : uint8_t { Constant, View, Vertex };
  struct Buffer {
    BufferKind kind = BufferKind::Constant;
    uint32_t slot = 0;
    uint32_t bytes = 0;  // Constant: at least this many (the declared size)
  };
  struct Texture {
    uint32_t view = 0;
    int sampler = -1;
  };
  struct Format {
    bool uav = false;
    uint32_t slot = 0;
  };
  std::vector<Buffer> buffers;
  std::vector<Texture> textures;
  std::vector<uint32_t> rw_buffers, rw_images;  // compute: UAV slots
  std::vector<Format> formats;
  bool uniforms = false;
};

struct LinkedProgram {
  gfx::ProgramId id = 0;
  StageBindings vs, ps, cs;
};

// A program for the bound shaders (null: none, the draw is skipped).
// `ps` and `layout` may be null. Called with Device::gpu_mu held, on the
// thread running the immediate context.
std::shared_ptr<const LinkedProgram> LinkProgram(gfx::Device& dev, const Shader& vs,
                                                 const Shader* ps, const InputLayout* layout);
std::shared_ptr<const LinkedProgram> LinkComputeProgram(gfx::Device& dev, const Shader& cs);

// A stage's uniform block (std140, `l_` names: the translation keeps to
// `r_`), filled per draw:
//   ivec4 l_draw:   vertex stage: x what R_VERTEX_ID subtracts from
//                   gl_VertexIndex (the draw's base vertex, or its first
//                   vertex when it is not indexed), y the draw's first
//                   instance (StartInstanceLocation);
//   uvec4 l_fmt[4]: the DXGI formats of StageBindings::formats, entry i
//                   in l_fmt[i / 4][i % 4];
//   uvec4 l_ia[n]:  vertex stage: per vertex buffer, x its stride and y
//                   the vertex (or instance) its gfx buffer starts at.
constexpr uint32_t kMaxTypedViews = 16;
struct StageUniforms {
  int32_t draw[4];
  uint32_t formats[kMaxTypedViews];
  uint32_t ia[32][4];
};

// ---- the device

struct Device : com::Object {
  std::shared_ptr<Context> context;  // the immediate context
  std::shared_ptr<com::Object> dxgi;  // its IDXGIDevice1 (dxgi.cpp)
  std::unique_ptr<gfx::Device> gfx;  // made on the first swap chain's window
  // Held by everything that touches gfx or the resources' GPU side: the
  // immediate context's operations and Present.
  std::recursive_mutex gpu_mu;
  // Programs by the serials of (vertex shader, pixel shader, input
  // layout), or (compute shader, 0, 0); and by their content (DXBC
  // hashes, the layout's elements), d3d11_program.cpp.
  std::map<std::tuple<uint64_t, uint64_t, uint64_t>, std::shared_ptr<const LinkedProgram>> programs;
  std::map<std::tuple<uint64_t, uint64_t, uint64_t>, std::shared_ptr<const LinkedProgram>> linked;
  // This frame's work, reported at Present.
  struct Stats {
    uint32_t draws = 0, skipped = 0, dispatches = 0, skipped_dispatches = 0, clears = 0;
    uint32_t copies = 0, executed_lists = 0, linked = 0;
    uint32_t waiting = 0;  // draws left out: their program is being made
  } stats;
  uint64_t frames = 0;
  // Where a dispatch's writes to a UAV slot with nothing bound go: a GPU
  // buffer nothing reads (d3d11_gpu.cpp, GpuDispatch).
  gfx::BufferId scratch_uav = 0;
};

// The device's IDXGIDevice1 interface, with a reference added (dxgi.cpp).
gaddr DxgiDeviceOf(Device& d);

// A swap chain's back buffer: a w x h Texture2D with `bind`, published
// with the swap chain's own reference (d3d11_objects.cpp).
std::shared_ptr<Resource> NewBackBuffer(uint32_t w, uint32_t h, uint32_t format, uint32_t bind);

// The process's device (null before D3D11CreateDevice).
Device* TheDevice();

// Bytes per pixel (or per 4x4 block for block-compressed formats) of a
// DXGI_FORMAT, and whether it is block-compressed; 0 for an unknown one.
uint32_t FormatBytes(uint32_t format, bool* block);

// ---- the GPU side (d3d11_gpu.cpp): what the immediate context runs.
// Each takes Device::gpu_mu.

struct DrawCall {
  bool indexed = false;
  uint32_t count = 0, instances = 1;
  uint32_t first = 0;      // first vertex, or first index
  int32_t base_vertex = 0;  // indexed
  uint32_t first_instance = 0;
};
void GpuDraw(Device& dev, Context& x, const DrawCall& d);
void GpuDispatch(Device& dev, Context& x, uint32_t gx, uint32_t gy, uint32_t gz);
void GpuClearTarget(Device& dev, const View& v, const float color[4]);
void GpuClearDepth(Device& dev, const View& v, uint32_t flags, float z, uint8_t stencil);
void GpuClearUav(Device& dev, const View& v, const uint32_t values[4], bool floats);
void GpuGenerateMips(Device& dev, const View& v);
// Copies between resources, on the GPU when either side's newest
// contents are there, otherwise in guest memory. `box` is a D3D11_BOX
// (left, top, front, right, bottom, back), null for the whole
// subresource.
void GpuCopy(Device& dev, Resource& dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
             Resource& src, uint32_t src_sub, const uint32_t* box);
void GpuCopyResource(Device& dev, Resource& dst, Resource& src);
// UpdateSubresource's write of host bytes (rows `pitch` apart, slices
// `slice_pitch`) into a subresource's box (null: all of it).
void GpuUpdate(Device& dev, Resource& r, uint32_t sub, const uint32_t* box, const uint8_t* src,
               uint32_t pitch, uint32_t slice_pitch);
// Before the CPU reads or partly rewrites a resource: the GPU's writes
// read back into its guest memory.
void GpuSyncToCpu(Device& dev, Resource& r);
// After the CPU wrote subresource `sub` (~0u: all of them) or, for a
// buffer, bytes lo..hi.
void CpuWrote(Resource& r, uint32_t sub, uint64_t lo = 0, uint64_t hi = ~0ull);
// Shows a swap chain's back buffer and reports the frame.
void GpuPresent(Device& dev, Resource& back_buffer, int w, int h);

// The program for a draw's (dispatch's) shaders (d3d11_program.cpp).
std::shared_ptr<const LinkedProgram> ProgramFor(Device& dev, const Shader& vs, const Shader* ps,
                                                const InputLayout* layout);
std::shared_ptr<const LinkedProgram> ComputeProgramFor(Device& dev, const Shader& cs);

}  // namespace recomp::hle::d3d11
