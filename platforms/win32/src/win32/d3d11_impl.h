#pragma once
// Direct3D 11's host objects (d3d11.cpp, d3d11_context.cpp, dxgi.cpp).
// Resources, views, states and shaders are records of what the guest
// created: descriptions and data, made into gfx resources when the device
// draws.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"
#include "win32/com.h"

namespace recomp::hle::d3d11 {

constexpr uint32_t kS_OK = 0, kS_FALSE = 1, kE_INVALIDARG = 0x80070057, kE_FAIL = 0x80004005,
                   kE_OUTOFMEMORY = 0x8007000E, kDXGI_ERROR_NOT_FOUND = 0x887A0002,
                   kDXGI_ERROR_INVALID_CALL = 0x887A0001;

struct Device;

// Every D3D11 object but the device: private data (SetPrivateData, by
// GUID) and its device (GetDevice).
struct Child : com::Object {
  std::map<std::string, std::vector<uint8_t>> private_data;  // by the GUID's bytes
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

// A buffer or texture. Its contents live in guest memory (mem: every
// subresource one after the other, tightly packed), so Map hands the guest
// a pointer into them; creation, Map, UpdateSubresource and copies write
// them. GPU writes (render targets, UAVs) are read back into them only
// when the guest maps or copies from the resource.
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
  gaddr mem = 0;
  uint64_t size = 0;
  uint8_t* Data() const;
  uint64_t version = 0;  // bumped on every CPU write
  // The subresource's offset in data and its row pitch and slice pitch.
  uint64_t SubOffset(uint32_t sub) const;
  uint32_t RowPitch(uint32_t mip) const;
  uint32_t SlicePitch(uint32_t mip) const;
  uint32_t MipWidth(uint32_t mip) const { return std::max(width >> mip, 1u); }
  uint32_t MipHeight(uint32_t mip) const { return std::max(height >> mip, 1u); }
  uint32_t MipDepth(uint32_t mip) const { return std::max(depth >> mip, 1u); }
};

// A view of a resource (SRV, RTV, DSV, UAV): its description as given.
enum class ViewKind { ShaderResource, RenderTarget, DepthStencil, UnorderedAccess };
struct View : Child {
  ViewKind kind;
  std::shared_ptr<Resource> res;
  uint32_t format = 0, view_dim = 0;
  uint32_t first_mip = 0, mips = 1, first_slice = 0, slices = 1;
  uint32_t first_element = 0, elements = 0, flags = 0;  // buffer views
};

struct Shader : Child {
  uint32_t stage = 0;  // 0 vs, 1 ps, 2 cs (the shader type in the DXBC)
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

// The immediate context's pipeline state, as the guest set it.
constexpr int kStages = 3;  // vs, ps, cs (no geometry or tessellation)
struct Context : Child {
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

struct Device : com::Object {
  std::shared_ptr<Context> context;  // the immediate context
  std::shared_ptr<com::Object> dxgi;  // its IDXGIDevice1 (dxgi.cpp)
  std::unique_ptr<gfx::Device> gfx;  // made on the first swap chain's window
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

}  // namespace recomp::hle::d3d11
