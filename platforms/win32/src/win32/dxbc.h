#pragma once
// Direct3D 11 shaders to gfx::Device programs. Translate() takes a DXBC
// container (the bytes a game hands CreateVertexShader, CreatePixelShader
// or CreateComputeShader: shader model 4.0-5.0 vertex, pixel and compute
// shaders, which share one token format) and gives the GLSL of one
// gfx::ProgramStage with what the Direct3D 11 layer binds it by. One
// translation serves every draw: what depends on the other stage or on
// the input layout comes in at link time, as code and macros the layer
// puts before the translation's GLSL (below).
//
// Registers are typeless in DXBC, so every register (r#, x#[], v#, o#,
// the immediate constant buffer) is a uvec4 of bits; each instruction
// reads its operands as the type it works on (uintBitsToFloat, int(...))
// and writes its result's bits back, so moves, movc and the bit
// operations keep bit patterns exactly.
//
// What the GLSL expects of the stage (the device's prelude, device.h,
// provides r_ld, r_buf, r_rw, r_rwbuf, R_TEXTURE and R_IMAGE):
//
// Read-only buffers (ProgramStage::buffers, `r_ld(b, word)`):
//   - constant buffers first: buffer i is Shader::cbuffers[i] (sorted by
//     slot); cb#[n] component c is word 4n + c. Reads at constant
//     indices are not checked: bind at least ConstantBuffer::size * 16
//     bytes, padded with zeros (Direct3D reads past a smaller buffer as
//     0). Reads at run-time indices past r_buf[b].y bytes give 0.
//   - then the buffer SRVs (typed Buffer, ByteAddressBuffer,
//     StructuredBuffer), in Shader::resources order (ShaderResource::
//     buffer is the index): the view's bytes from its first element,
//     r_buf[b].y the view's size in bytes (reads past it give 0, as in
//     Direct3D; bufinfo derives element counts from it).
//   - the layer binds its own (vertex buffers for r_fetch) after these,
//     raising ProgramStage::buffers.
// A typed Buffer SRV's element i is the format's bytes at i * the
// format's size; the layer defines R_FMT_T<slot> (R_FMT_T3) to the
// view's DXGI_FORMAT number, which the GLSL decodes (FormatSupported()
// lists the formats it knows; others read as 0). A typed RWBuffer UAV
// likewise with R_FMT_U<slot>.
//
// Textures (ProgramStage::textures, `layout(R_TEXTURE(n))`): one
// combined image and sampler per Shader::textures entry, the pair of an
// SRV slot and the sampler slot the code samples it with (sampler -1:
// texel loads and size queries only, any sampler state). texture_kinds
// says what each is. A Texture1D(Array) is read as a 2D (array) texture
// of height 1. A Texture2DMS(Array) is read as a single-sampled 2D
// (array) texture, as the device has no multisampled images: ld_ms reads
// level 0's texel whatever the sample, sample counts are 1. A
// TextureCubeArray is read as the 2D array of its faces (six layers a
// cube, in Direct3D's order: a cube array's own slices), kind
// Tex2DArray; the GLSL picks the face, so filtering does not cross the
// faces' edges.
//
// Compute stages' UAVs: buffers (typed, raw, structured) are read-write
// buffers in Shader::uavs order (UnorderedAccess::buffer; r_rwbuf[b].y
// the view's size in bytes, accesses past it are dropped or read 0),
// textures storage images (UnorderedAccess::image, `layout(R_IMAGE(n))`,
// writeonly unless the code reads them, then r32f/r32ui/r32i by the
// declared return type, as Direct3D 11.0's typed UAV loads are only of
// those formats). Pixel and vertex shaders with UAVs are not translated.
//
// The stage's own uniform block (R_UNIFORMS) is the layer's: the
// translation never declares it (uniform_bytes 0). The translation's
// names are r<n>, x<n> and r_<name>, its macros R_<NAME>: link-time code
// keeps to other names, but for r_fetch and the macros below.
//
// Vertex shaders:
//   - inputs come from `uvec4 r_fetch(uint reg)`, which the layer defines
//     from the input layout: the bits of input register `reg` (its
//     elements' formats converted as Direct3D does: UNORM/SNORM/FLOAT to
//     float bits, UINT/SINT as integers; missing components 0, 0, 0, 1 -
//     1.0 for float formats). It is called once per declared input
//     register (Shader::inputs with system_value 0), with a constant.
//   - SV_VertexID and SV_InstanceID are the macros R_VERTEX_ID and
//     R_INSTANCE_ID, uint expressions the layer may define; they default
//     to uint(gl_VertexIndex) and uint(gl_InstanceIndex), which include
//     the device draw's base vertex. Direct3D's SV_VertexID of an indexed
//     draw is the index without BaseVertexLocation and its SV_InstanceID
//     does not count StartInstanceLocation, so a layer that draws with a
//     base vertex defines R_VERTEX_ID to take it off (a uniform of its
//     own).
//   - SV_Position goes to gl_Position as it is: Direct3D's clip space is
//     the device's (y up to the top of the target, z from 0 to w), as in
//     the device's other front ends. SV_ClipDistance goes to
//     gl_ClipDistance (in signature order); gl_PointSize is 1.
//   - every varying of the output signature is written (0 where the
//     shader writes nothing), so that any pixel shader linked with it
//     finds its inputs.
// Varyings (vertex outputs and pixel inputs that are not system values):
// each signature element is its own location, the macro
// LocationMacro(element) (R_LOC_ + upper-case semantic + index, e.g.
// R_LOC_TEXCOORD3), which the layer defines for both stages
// (VaryingDefines() does it for a pair). Each is a 4-component vector of
// the element's component type (vec4, uvec4, ivec4) holding the
// element's components from its first one (an element in .zw of its
// register is .xy of its varying), the rest 0; pixel inputs take the
// interpolation the pixel shader declares (flat, noperspective,
// centroid, sample).
// Pixel shaders: SV_Position is gl_FragCoord with w = 1 / gl_FragCoord.w
// (Direct3D gives clip w), SV_IsFrontFace gl_FrontFacing, SV_SampleIndex
// gl_SampleID, SV_PrimitiveID gl_PrimitiveID (on Metal, MSL 2.2 and
// later); SV_TargetN is location N
// (vec4, uvec4 or ivec4 by its type; the device draws to 8 targets),
// SV_Depth gl_FragDepth, SV_Coverage gl_SampleMask.
// Compute shaders: SV_DispatchThreadID, SV_GroupID, SV_GroupThreadID and
// SV_GroupIndex are gl_GlobalInvocationID, gl_WorkGroupID,
// gl_LocalInvocationID and gl_LocalInvocationIndex; the group size is
// Shader::local_size (ProgramDesc::local_size: the device's prelude
// declares it); group-shared memory is `shared` arrays.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"

namespace recomp::hle::d3d11::dxbc {

// The program type in the version token (D3D10_SB_TOKENIZED_PROGRAM_TYPE).
enum class ShaderType : uint8_t { Pixel = 0, Vertex = 1, Geometry = 2, Hull = 3, Domain = 4, Compute = 5 };

// D3D10_SB_RESOURCE_DIMENSION.
enum class ResourceDim : uint8_t {
  Unknown, Buffer, Texture1D, Texture2D, Texture2DMS, Texture3D, TextureCube, Texture1DArray,
  Texture2DArray, Texture2DMSArray, TextureCubeArray, RawBuffer, StructuredBuffer,
};

// D3D10_SB_RESOURCE_RETURN_TYPE (0: none declared).
enum class ReturnType : uint8_t { None, Unorm, Snorm, Sint, Uint, Float, Mixed, Double, Continued, Unused };

// An element of an input or output signature (ISGN/ISG1, OSGN/OSG1/OSG5).
struct SignatureElement {
  std::string semantic;         // as the signature has it (no index digits)
  uint32_t index = 0;           // semantic index
  uint32_t reg = 0;             // register (~0u: SV_Depth and the like)
  uint32_t system_value = 0;    // D3D_NAME: 0 none, 1 SV_Position, 2 SV_ClipDistance, 6 SV_VertexID,
                                // 8 SV_InstanceID, 9 SV_IsFrontFace, 64 SV_Target, 65 SV_Depth, ...
  uint32_t component_type = 0;  // D3D_REGISTER_COMPONENT_TYPE: 1 uint, 2 sint, 3 float
  uint8_t mask = 0;             // its components (bit 0: x)
  uint8_t rw_mask = 0;          // inputs: those the shader reads; outputs: those it never writes
  uint32_t stream = 0;
};

struct ConstantBuffer {
  uint32_t slot = 0;
  uint32_t size = 0;      // in 16-byte constants, as declared
  bool dynamic = false;   // indexed at run time
};

// A shader resource view slot (t#) the shader declares.
struct ShaderResource {
  uint32_t slot = 0;
  ResourceDim dim = ResourceDim::Unknown;
  ReturnType ret = ReturnType::None;  // typed ones (x component; all four agree in practice)
  uint32_t stride = 0;                // StructuredBuffer: bytes per element
  int buffer = -1;                    // buffers: the gfx read-only buffer
};

// An unordered access view slot (u#), compute shaders only.
struct UnorderedAccess {
  uint32_t slot = 0;
  ResourceDim dim = ResourceDim::Unknown;
  ReturnType ret = ReturnType::None;
  uint32_t stride = 0;
  bool counter = false;  // structured with a hidden counter (not translated yet)
  bool read = false;     // the code reads it (ld_uav_typed, atomics)
  int buffer = -1;       // buffers: the gfx read-write buffer
  int image = -1;        // textures: the gfx storage image
};

struct Sampler {
  uint32_t slot = 0;
  bool comparison = false;  // declared mode_comparison (SamplerComparisonState)
};

// A gfx texture of the stage: an SRV slot and the sampler slot the code
// samples it with (-1: loads and queries only).
struct TextureBinding {
  uint32_t resource = 0;
  int sampler = -1;
  bool comparison = false;  // sampled with comparison (a shadow sampler)
};

struct Shader {
  ShaderType type = ShaderType::Pixel;
  uint32_t major = 0, minor = 0;
  std::vector<SignatureElement> inputs, outputs;
  std::vector<ConstantBuffer> cbuffers;     // gfx buffers 0 .. size - 1
  std::vector<ShaderResource> resources;    // by slot
  std::vector<UnorderedAccess> uavs;        // by slot
  std::vector<Sampler> samplers;            // by slot
  std::vector<TextureBinding> textures;     // gfx textures, in this order
  uint32_t local_size[3] = {1, 1, 1};       // compute: dcl_thread_group
  uint32_t tgsm_bytes = 0;                  // compute: group-shared memory declared
  bool writes_depth = false;                // pixel: SV_Depth
  bool discards = false;                    // pixel: discard
  uint32_t targets = 0;                     // pixel: SV_Target bits
  // The stage: GLSL and counts (textures, buffers, rw_buffers, rw_images,
  // texture_kinds; uniform_bytes 0).
  gfx::ProgramStage stage;
  std::string error;  // why it was not translated (empty: it was)
};

// Translates a DXBC container. False with out->error set (and the
// reflection as far as it got) for one that is malformed or uses what is
// not translated yet; the error names the instruction (its dword offset
// in the program and its disassembly). With `annotate`, each
// instruction's listing precedes its GLSL as a comment.
bool Translate(const void* dxbc, size_t size, Shader* out, bool annotate = false);

// Point patches, the tessellation games use to draw sprites: a patch of
// one control point that the tessellator makes one quad of. A domain
// shader translates as a vertex stage that runs for each corner of the
// quad: its control point's registers are the stage's inputs (v#, from
// r_fetch, the vertex shader's inputs) and its domain point is
// R_DOMAIN_POINT (a vec3, defined by the link code). These say whether a
// draw's other stages fit that:
// - a hull shader whose control points pass through unchanged and whose
//   tessellation factors on a quad domain are whole numbers of segments
//   (literals; the partitioning making that many of each), the largest N:
//   the quad is drawn as an N x N grid (*segments) of two triangles each
//   (*ccw: their winding in the domain);
bool PointToQuadHull(const void* dxbc, size_t size, bool* ccw, uint32_t* segments);
// - a vertex shader that only copies each input register to the output
//   register of the same number, or writes constants (*constants) into
//   them, so the domain shader's control point registers are the vertex's
//   inputs with those constants.
struct ConstantOutput {
  uint32_t reg = 0, comp = 0, bits = 0;
};
bool PassThroughVertexShader(const void* dxbc, size_t size, std::vector<ConstantOutput>* constants);

// A listing of the container's program in the style of fxc's
// disassembly, each instruction preceded by its dword offset; empty with
// *error set for one that cannot be read.
std::string Disassemble(const void* dxbc, size_t size, std::string* error = nullptr);

// The macro that gives a varying element its location: "R_LOC_" plus the
// semantic in upper case and the index (R_LOC_TEXCOORD3).
std::string LocationMacro(const SignatureElement& e);

// Location #defines for a vertex shader's outputs and a pixel shader's
// inputs that are not system values: matched by semantic (case does not
// matter), numbered from 0 in the vertex shader's output order, pixel
// inputs no vertex output has after them. Either may be null. The same
// text goes before both stages' GLSL.
std::string VaryingDefines(const Shader* vs, const Shader* ps);

// Whether typed buffer views (R_FMT_T#, R_FMT_U#) of this DXGI_FORMAT are
// decoded (and encoded, for UAV stores).
bool FormatSupported(uint32_t dxgi_format);

}  // namespace recomp::hle::d3d11::dxbc
