// Reversed depth on the Metal backend (gfx/reversed_z.h): translated
// shaders, completed with the Metal prelude (gfx/metal_prelude.h) and
// compiled as the backend compiles them, drawn on the GPU with depth as
// Direct3D 9 stores it and reversed (1 - d), must store mirrored values,
// make the same depth-test decisions, and read the same values back from
// a depth texture. Depth from w: which vertex shaders and matrices
// qualify, and two layers 3 mm apart 30 m away, in world coordinates
// kilometres from the origin, must keep their order, store what D3D9
// would (reversed), clipped at the same near and far, and read back as
// D3D9 stores them. Depth bias by D3D9's amount wherever the stored value
// is; border colours and vertex texture reads of reversed depth textures.
//
//   metal_depth_test        (skipped, exit 77, without a Metal device)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/gfx/metal_prelude.h"
#include "runtime/gfx/reversed_z.h"
#include "runtime/gfx/shader_compile.h"
#include "runtime/hlsl/hlsl.h"

namespace {

using namespace recomp;
using Tokens = std::vector<uint32_t>;

// ---- hand-assembled shaders (as tests/hlsl_bytecode_test.cpp)

enum Reg : uint32_t {
  R = 0, V = 1, C = 2, A = 3, O = 6, I = 7, OC = 8, ODEPTH = 9, S = 10, B = 14, AL = 15, LBL = 18, P = 19,
};
enum Op : uint32_t {
  MOV = 1, ADD = 2, MAD = 4, MUL = 5, CALL = 25, LOOP = 27, RET = 28, ENDLOOP = 29, LABEL = 30, IF = 40,
  ENDIF = 43, MOVA = 46, DEFI = 48, TEX = 66, DEF = 81, SETP = 94, TEXLDL = 95,
};

uint32_t TypeBits(uint32_t type) { return ((type & 7) << 28) | ((type & 0x18) << 8); }
uint32_t Dst(uint32_t type, uint32_t n, uint32_t mask = 0xF) {
  return 0x80000000u | n | TypeBits(type) | mask << 16;
}
uint32_t Src(uint32_t type, uint32_t n, uint32_t swz = 0xE4) {
  return 0x80000000u | n | TypeBits(type) | swz << 16;
}

struct Asm {
  Tokens t;
  explicit Asm(bool pixel) { t.push_back((pixel ? 0xFFFF0000u : 0xFFFE0000u) | 3u << 8); }
  void I(uint32_t op, std::initializer_list<uint32_t> params, uint32_t ctl = 0, uint32_t flags = 0) {
    t.push_back(op | ctl << 16 | uint32_t(params.size()) << 24 | flags);
    t.insert(t.end(), params);
  }
  void Dcl(uint32_t usage, uint32_t index, uint32_t dst) { I(31, {0x80000000u | usage | index << 16, dst}); }
  void DclSampler(uint32_t type, uint32_t n) { I(31, {0x80000000u | type << 27, Dst(S, n)}); }
  Tokens End() {
    t.push_back(0x0000FFFF);
    return t;
  }
};

// vs_3_0: POSITION and TEXCOORD0 passed through.
Tokens PassVs() {
  Asm a(false);
  a.Dcl(0, 0, Dst(V, 0));
  a.Dcl(5, 0, Dst(V, 1));
  a.Dcl(0, 0, Dst(O, 0));
  a.Dcl(5, 0, Dst(O, 1));
  a.I(MOV, {Dst(O, 0), Src(V, 0)});
  a.I(MOV, {Dst(O, 1), Src(V, 1)});
  return a.End();
}

// ps_3_0: c0 as the colour; with `depth`, c1.x as the depth (oDepth).
Tokens ConstPs(bool depth) {
  Asm a(true);
  a.I(MOV, {Dst(OC, 0), Src(C, 0)});
  if (depth) a.I(MOV, {Dst(ODEPTH, 0, 1), Src(C, 1, 0x00)});
  return a.End();
}

// vs_3_0 as the HLSL compiler makes mul(float4(pos, 1), M) with M in
// c8-c11 (rows): r0 = y c9 + x c8 + z c10, then + c11. `variant`: 0 that;
// 1 z pushed towards the camera (z - c12.x, as emissive signs drawn over
// a wall are); 2 z = w (a sky: the translation's .xyww); 3 the matrix's
// rows in c8-c10 but the translation from c12; 4 the position's write
// predicated; 5 a row added in an if block; 6 a loop; 7 a subroutine; 8
// c11 defined in the shader; 9 the translation read as c[a0.x + 11]; 10
// the translation carried negated (r1 = c11, the position r0 - r1); 11
// the position's x, y, w and its z written apart (the same values); 12 a
// row negated (z and w alike: still the matrix).
Tokens WvpVs(int variant = 0) {
  constexpr uint32_t kX = 0x00, kY = 0x55, kZ = 0xAA, kW = 0xFF;
  constexpr uint32_t kNeg = 1u << 24;
  Asm a(false);
  if (variant == 8) a.I(DEF, {Dst(C, 11), 0, 0, 0x3F800000u, 0x3F800000u});
  if (variant == 6) a.I(DEFI, {Dst(I, 0), 2, 0, 1, 0});
  a.Dcl(0, 0, Dst(V, 0));
  a.Dcl(0, 0, Dst(O, 0));
  a.I(MUL, {Dst(R, 0), Src(V, 0, kY), Src(C, 9)});
  a.I(MAD, {Dst(R, 0), Src(V, 0, kX), Src(C, 8), Src(R, 0)});
  if (variant == 5) a.I(IF, {Src(B, 0)});
  a.I(MAD, {Dst(R, 0), Src(V, 0, kZ), Src(C, 10) | (variant == 12 ? kNeg : 0), Src(R, 0)});
  if (variant == 5) a.I(ENDIF, {});
  if (variant == 6) {
    a.I(LOOP, {Src(AL, 0), Src(I, 0)});
    a.I(ADD, {Dst(R, 1), Src(R, 1), Src(C, 12)});
    a.I(ENDLOOP, {});
  }
  if (variant == 7) a.I(CALL, {Src(LBL, 0)});
  if (variant == 1) {
    a.I(ADD, {Dst(R, 0), Src(R, 0), Src(C, 11)});
    a.I(MOV, {Dst(O, 0, 0xB), Src(R, 0)});
    a.I(ADD, {Dst(O, 0, 0x4), Src(R, 0, kZ), Src(C, 12, kX) | kNeg});
  } else if (variant == 2) {
    a.I(MAD, {Dst(O, 0), Src(V, 0, kW), Src(C, 11, 0xF4), Src(R, 0)});  // c11.xyww
  } else if (variant == 4) {
    a.I(SETP, {Dst(P, 0, 1), Src(V, 0, kX), Src(C, 12, kX)}, 1);  // p0.x = v0.x > c12.x
    a.I(ADD, {Dst(O, 0), Src(P, 0, kX), Src(R, 0), Src(C, 11)}, 0, 0x10000000u);
  } else if (variant == 9) {
    a.I(MOVA, {Dst(A, 0, 1), Src(C, 12, kX)});
    // c[a0.x + 11]: the relative address token follows (vs_3_0).
    a.I(ADD, {Dst(O, 0), Src(R, 0), Src(C, 11) | 0x2000u, 0x80000000u | TypeBits(A)});
  } else if (variant == 10) {
    a.I(MOV, {Dst(R, 1), Src(C, 11)});
    a.I(ADD, {Dst(O, 0), Src(R, 0), Src(R, 1) | kNeg});
  } else if (variant == 11) {
    a.I(ADD, {Dst(R, 0), Src(R, 0), Src(C, 11)});
    a.I(MOV, {Dst(O, 0, 0xB), Src(R, 0)});
    a.I(MOV, {Dst(O, 0, 0x4), Src(R, 0)});
  } else {
    a.I(ADD, {Dst(O, 0), Src(R, 0), Src(C, variant == 3 ? 12 : 11)});
  }
  if (variant == 7) {
    a.I(RET, {});
    a.I(LABEL, {Src(LBL, 0)});
    a.I(MOV, {Dst(R, 1), Src(C, 12)});
    a.I(RET, {});
  }
  return a.End();
}

// vs_3_0: POSITION passed through, TEXCOORD0 what s0 reads at TEXCOORD0
// (texldl: its w, 0, the level).
Tokens VertexSampleVs() {
  Asm a(false);
  a.Dcl(0, 0, Dst(V, 0));
  a.Dcl(5, 0, Dst(V, 1));
  a.Dcl(0, 0, Dst(O, 0));
  a.Dcl(5, 0, Dst(O, 1));
  a.DclSampler(2, 0);
  a.I(TEXLDL, {Dst(R, 0), Src(V, 1), Src(S, 0)});
  a.I(MOV, {Dst(O, 0), Src(V, 0)});
  a.I(MOV, {Dst(O, 1), Src(R, 0)});
  return a.End();
}

// ps_3_0: TEXCOORD0 as the colour.
Tokens VaryingPs() {
  Asm a(true);
  a.Dcl(5, 0, Dst(V, 0));
  a.I(MOV, {Dst(OC, 0), Src(V, 0)});
  return a.End();
}

// vs_3_0: a full-target pass with c0 (x, y scaled and offset), as games'
// blits.
Tokens BlitVs() {
  Asm a(false);
  a.Dcl(0, 0, Dst(V, 0));
  a.Dcl(0, 0, Dst(O, 0));
  a.I(MAD, {Dst(O, 0), Src(V, 0, 0x24), Src(C, 0, 0x40), Src(C, 0, 0x15)});  // v0.xyzx, c0.xxxy, c0.yyyx
  return a.End();
}

// ps_3_0: s0 read at TEXCOORD0 as the colour.
Tokens SamplePs() {
  Asm a(true);
  a.Dcl(5, 0, Dst(V, 0));
  a.DclSampler(2, 0);
  a.I(TEX, {Dst(R, 0), Src(V, 0), Src(S, 0)});
  a.I(MOV, {Dst(OC, 0), Src(R, 0)});
  return a.End();
}

// ---- the GPU

constexpr int kSize = 8;

struct Vertex {
  float pos[4], uv[4];
};

// The prelude's state blocks (std140), as metal_device.mm fills them.
struct VsState {
  int32_t in[16][4];
  float pixel[4];
  float clip[6][4];
  int32_t clip_mask[4];
  float depth[4];
};
struct PsState {
  int32_t alpha[4];
  float bias[16];
  float vscale[4];
};

struct Gpu {
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> queue = [dev newCommandQueue];
  std::vector<hlsl::Semantic> vs_inputs;

  id<MTLFunction> Compile(const Tokens& tokens) {
    hlsl::Program p;
    std::string err;
    if (!hlsl::TranslateBytecode(tokens.data(), tokens.size(), &p, &err)) {
      std::printf("FAIL translate: %s\n", err.c_str());
      return nil;
    }
    const bool vertex = p.stage == hlsl::Stage::Vertex;
    if (vertex) vs_inputs = p.inputs;
    const std::string glsl = "#version 450\n" + gfx::MetalShaderPrelude(p.stage) + p.glsl;
    std::vector<uint32_t> spirv;
    std::string msl, log;
    if (!gfx::GlslToSpirv(glsl, vertex ? gfx::ShaderStage::Vertex : gfx::ShaderStage::Fragment, &spirv, &log) ||
        !gfx::SpirvToMsl(spirv, &msl, &log)) {
      std::printf("FAIL compile: %s\n%s\n", log.c_str(), glsl.c_str());
      return nil;
    }
    // The alpha test's switch as the backend compiles it (on: ALPHAFUNC
    // ALWAYS below).
    MTLCompileOptions* o = nil;
    if (gfx::MetalLeanMacros(&msl, vertex)) {
      o = [MTLCompileOptions new];
      o.preprocessorMacros = @{@"R_ALPHA_TEST" : @"true"};
    }
    NSError* e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(msl.c_str()) options:o error:&e];
    if (!lib) {
      std::printf("FAIL MSL: %s\n", e.localizedDescription.UTF8String);
      return nil;
    }
    return [lib newFunctionWithName:@"main0"];
  }

  id<MTLTexture> Texture(MTLPixelFormat pf) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                                                  width:kSize
                                                                                 height:kSize
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [dev newTextureWithDescriptor:td];
  }

  id<MTLRenderPipelineState> Pipeline(id<MTLFunction> vs, id<MTLFunction> ps, MTLPixelFormat color,
                                      MTLPixelFormat depth) {
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs;
    pd.fragmentFunction = ps;
    pd.colorAttachments[0].pixelFormat = color;
    pd.depthAttachmentPixelFormat = depth;
    NSError* e = nil;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!p) std::printf("FAIL pipeline: %s\n", e.localizedDescription.UTF8String);
    return p;
  }

  id<MTLDepthStencilState> DepthState(MTLCompareFunction f, bool write) {
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = f;
    dd.depthWriteEnabled = write;
    return [dev newDepthStencilStateWithDescriptor:dd];
  }

  // One draw of `verts` (a triangle list) with the prelude's state.
  struct Draw {
    id<MTLRenderPipelineState> pipe;
    id<MTLDepthStencilState> ds;
    std::vector<Vertex> verts;
    float c[2][4];  // pixel shader constants c0, c1
    bool reversed;  // as the device draws reversed: mode 1 (w - z), or 2 with `k`
    float min_z = 0, max_z = 1;
    float bias = 0, slope = 0;  // D3D9's DEPTHBIAS, SLOPESCALEDEPTHBIAS
    uint32_t depth_samplers = 0;
    int mode = -1;               // r_position's mode, -1: from `reversed`
    float k[2] = {};             // depth from w's k0, k1
    std::vector<float> vc;       // vertex shader constants c0.. (4 floats each)
    id<MTLTexture> tex = nil;  // s0
    int border = -1;           // s0 clamped to this MTLSamplerBorderColor (-1: the default sampler)
    id<MTLTexture> vtex = nil;  // vertex s0
    uint32_t vertex_depth_samplers = 0;
  };

  id<MTLSamplerState> SamplerFor(int border) {
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    if (border >= 0) {
      sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToBorderColor;
      sd.borderColor = MTLSamplerBorderColor(border);
    }
    return [dev newSamplerStateWithDescriptor:sd];
  }

  void Encode(id<MTLRenderCommandEncoder> e, const Draw& d) {
    VsState vst{};
    for (int i = 0; i < 16; i++) vst.in[i][2] = -1;
    for (size_t i = 0; i < vs_inputs.size(); i++) {
      vst.in[i][0] = vs_inputs[i].usage == 0 ? 0 : 16;  // POSITION, TEXCOORD0
      vst.in[i][1] = sizeof(Vertex);
      vst.in[i][2] = 3;  // FLOAT4
    }
    vst.clip_mask[1] = d.mode >= 0 ? d.mode : d.reversed ? 1 : 0;
    vst.clip_mask[2] = int32_t(d.vertex_depth_samplers);
    vst.depth[0] = d.k[0];
    vst.depth[1] = d.k[1];
    // metal_device.mm's: float depth, 2^23 per unit of depth range.
    const gfx::DepthBias db = gfx::DepthBiasOf(d.bias, d.slope, d.reversed, d.min_z, d.max_z, 8388608.0f);
    vst.depth[2] = db.clip;
    PsState pst{};
    pst.alpha[0] = 8;  // ALWAYS
    pst.alpha[2] = int32_t(d.depth_samplers);
    pst.alpha[3] = d.reversed ? 1 : 0;
    pst.vscale[0] = pst.vscale[1] = 1;
    // The vertices, then room for the shader's unaligned reads.
    std::vector<Vertex> vb = d.verts;
    vb.push_back({});
    [e setRenderPipelineState:d.pipe];
    [e setDepthStencilState:d.ds];
    [e setFrontFacingWinding:MTLWindingClockwise];
    [e setCullMode:MTLCullModeNone];
    MTLViewport vp = {0, 0, kSize, kSize, d.min_z, d.max_z};
    if (d.reversed) {
      const gfx::DepthRange r = gfx::ReversedViewportDepth(d.min_z, d.max_z);
      vp.znear = r.min_z;
      vp.zfar = r.max_z;
    }
    [e setViewport:vp];
    [e setDepthBias:db.constant slopeScale:db.slope clamp:0];
    [e setVertexBytes:&vst length:sizeof vst atIndex:2];
    if (!d.vc.empty()) [e setVertexBytes:d.vc.data() length:d.vc.size() * sizeof(float) atIndex:0];
    [e setVertexBytes:vb.data() length:vb.size() * sizeof(Vertex) atIndex:3];
    [e setFragmentBytes:d.c length:sizeof d.c atIndex:0];
    [e setFragmentBytes:&pst length:sizeof pst atIndex:2];
    if (d.tex) {
      [e setFragmentTexture:d.tex atIndex:0];
      [e setFragmentSamplerState:SamplerFor(d.border) atIndex:0];
    }
    if (d.vtex) {
      [e setVertexTexture:d.vtex atIndex:0];
      [e setVertexSamplerState:SamplerFor(-1) atIndex:0];
    }
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:NSUInteger(d.verts.size())];
  }

  // Draws into colour `color` and depth `depth` (cleared to the given
  // values first), then reads both back.
  void Run(id<MTLTexture> color, id<MTLTexture> depth, double clear_depth, const std::vector<Draw>& draws,
           std::vector<float>* rgba, std::vector<float>* z) {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = color;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (depth) {
      rp.depthAttachment.texture = depth;
      rp.depthAttachment.loadAction = MTLLoadActionClear;
      rp.depthAttachment.clearDepth = clear_depth;
      rp.depthAttachment.storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    for (const Draw& d : draws) Encode(e, d);
    [e endEncoding];
    id<MTLBuffer> cbuf = [dev newBufferWithLength:kSize * kSize * 16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> zbuf = [dev newBufferWithLength:kSize * kSize * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(kSize, kSize, 1) toBuffer:cbuf destinationOffset:0
        destinationBytesPerRow:kSize * 16 destinationBytesPerImage:kSize * kSize * 16];
    if (depth)
      [bl copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(kSize, kSize, 1) toBuffer:zbuf destinationOffset:0
          destinationBytesPerRow:kSize * 4 destinationBytesPerImage:kSize * kSize * 4];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (rgba) rgba->assign((float*)cbuf.contents, (float*)cbuf.contents + kSize * kSize * 4);
    if (z) z->assign((float*)zbuf.contents, (float*)zbuf.contents + kSize * kSize);
  }
};

// A triangle over the whole target, z and w per vertex (z / w varies
// across it, perspective-correct); uv the screen position at w = 1.
std::vector<Vertex> Cover(const float z[3], const float w[3]) {
  const float xy[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
  std::vector<Vertex> v(3);
  for (int i = 0; i < 3; i++) {
    v[i] = {{xy[i][0] * w[i], xy[i][1] * w[i], z[i] * w[i], w[i]},
            {(xy[i][0] + 1) * 0.5f, (1 - xy[i][1]) * 0.5f, 0, 0}};
  }
  return v;
}

bool Compare(uint32_t d3d, float a, float b) {  // a: the incoming value, b: the stored one
  switch (d3d) {
    case 1: return false;
    case 2: return a < b;
    case 3: return a == b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a != b;
    case 7: return a >= b;
    default: return true;
  }
}

MTLCompareFunction CompareOp(uint32_t d3d) { return MTLCompareFunction(d3d - 1); }

// MirrorDepthCompare on values: the same decision for 1 - a, 1 - b; and
// ReversedViewportDepth: the viewport's depth for 1 - z is 1 - its depth
// for z.
int TableChecks() {
  int failures = 0;
  const float vals[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
  for (uint32_t f = 1; f <= 8; f++)
    for (float a : vals)
      for (float b : vals)
        if (Compare(f, a, b) != Compare(gfx::MirrorDepthCompare(f), 1 - a, 1 - b)) {
          std::printf("FAIL mirror: func %u, %g vs %g\n", f, a, b);
          failures++;
        }
  for (uint32_t f = 1; f <= 8; f++)
    if (gfx::MirrorDepthCompare(gfx::MirrorDepthCompare(f)) != f) {
      std::printf("FAIL mirror: func %u twice\n", f);
      failures++;
    }
  const float ranges[][2] = {{0, 1}, {0.1f, 0.9f}, {0, 0}, {1, 1}, {0.5f, 0.75f}};
  for (const auto& r : ranges) {
    const gfx::DepthRange m = gfx::ReversedViewportDepth(r[0], r[1]);
    for (float z : vals) {
      const float d = r[0] + z * (r[1] - r[0]), dm = m.min_z + (1 - z) * (m.max_z - m.min_z);
      if (std::fabs((1 - d) - dm) > 1e-6f) {
        std::printf("FAIL viewport %g..%g at %g: %g, %g\n", r[0], r[1], z, 1 - d, dm);
        failures++;
      }
    }
  }
  std::printf("%s compare table and viewport\n", failures ? "FAIL" : "ok");
  return failures;
}

struct Fixture {
  Gpu& g;
  id<MTLFunction> vs, ps, ps_depth, ps_sample;
  id<MTLTexture> color, depth;
};

// r_position and the viewport: a perspective triangle drawn reversed
// stores 1 - what it stores drawn as D3D9 does; the clear too.
int PositionChecks(Fixture& f, std::vector<float>* z_d3d, std::vector<float>* z_rev) {
  Gpu& g = f.g;
  id<MTLRenderPipelineState> pipe = g.Pipeline(f.vs, f.ps, MTLPixelFormatRGBA32Float, MTLPixelFormatDepth32Float);
  if (!pipe) return 1;
  id<MTLDepthStencilState> ds = g.DepthState(MTLCompareFunctionAlways, true);
  const float z[3] = {0.2f, 0.5f, 0.9f}, w[3] = {1.0f, 2.0f, 4.0f};
  // The left half of the target only: the clear stays on the right.
  std::vector<Vertex> tri = Cover(z, w);
  for (Vertex& v : tri) v.pos[0] = v.pos[0] * 0.25f - 0.5f * v.pos[3];
  int failures = 0;
  static const float kRanges[2][2] = {{0, 1}, {0.1f, 0.9f}};
  for (const float* vpz : kRanges) {
    Gpu::Draw d{pipe, ds, tri, {{1, 0, 0, 1}, {}}, false, vpz[0], vpz[1]};
    g.Run(f.color, f.depth, 1.0, {d}, nullptr, z_d3d);
    d.reversed = true;
    g.Run(f.color, f.depth, 1.0 - 1.0, {d}, nullptr, z_rev);
    int drawn = 0;
    for (int i = 0; i < kSize * kSize; i++) {
      const float a = (*z_d3d)[i], b = (*z_rev)[i];
      if (a != 1.0f) drawn++;
      if (std::fabs((1 - a) - b) > 2e-6f) {
        if (failures++ < 8) std::printf("FAIL position: pixel %d, viewport %g..%g: %.9g, reversed %.9g\n", i, vpz[0], vpz[1], a, b);
      }
    }
    if (drawn < 8 || drawn == kSize * kSize) {
      std::printf("FAIL position: %d pixels drawn\n", drawn);
      failures++;
    }
  }
  std::printf("%s position (r_position, viewport, clear)\n", failures ? "FAIL" : "ok");
  return failures;
}

// The depth test: layer A written, then layer B tested with each ZFUNC;
// drawn reversed (mirrored function, clear and bias) B must pass exactly
// where it passes as D3D9 draws it, and as the function says.
int CompareChecks(Fixture& f) {
  Gpu& g = f.g;
  id<MTLRenderPipelineState> pipe = g.Pipeline(f.vs, f.ps, MTLPixelFormatRGBA32Float, MTLPixelFormatDepth32Float);
  if (!pipe) return 1;
  int failures = 0;
  const float one[3] = {1, 1, 1};
  struct Pair {
    float a, b, bias;  // bias: D3D9's direction (negative: toward the camera)
  };
  const Pair pairs[] = {{0.3f, 0.6f, 0}, {0.6f, 0.3f, 0}, {0.5f, 0.5f, 0}, {0.5f, 0.5f, -1e-3f}, {0.5f, 0.5f, 1e-3f}};
  for (uint32_t func = 1; func <= 8; func++)
    for (const Pair& pr : pairs) {
      const float za[3] = {pr.a, pr.a, pr.a}, zb[3] = {pr.b, pr.b, pr.b};
      float got[2];
      for (int rev = 0; rev < 2; rev++) {
        Gpu::Draw a{pipe, g.DepthState(MTLCompareFunctionAlways, true), Cover(za, one), {{1, 0, 0, 1}, {}}, rev != 0};
        const uint32_t fn = rev ? gfx::MirrorDepthCompare(func) : func;
        Gpu::Draw b{pipe, g.DepthState(CompareOp(fn), false), Cover(zb, one), {{0, 1, 0, 1}, {}}, rev != 0};
        b.bias = pr.bias;
        std::vector<float> rgba;
        g.Run(f.color, f.depth, rev ? 0.0 : 1.0, {a, b}, &rgba, nullptr);
        got[rev] = rgba[(kSize / 2 * kSize + kSize / 2) * 4 + 1];
      }
      const bool want = Compare(func, pr.b + (pr.bias < 0 ? -1e-4f : pr.bias > 0 ? 1e-4f : 0), pr.a);
      if (got[0] != got[1] || (got[0] == 1.0f) != want) {
        std::printf("FAIL compare: func %u, A %g, B %g, bias %g: D3D9 %s, reversed %s, want %s\n", func, pr.a,
                    pr.b, pr.bias, got[0] == 1.0f ? "passes" : "fails", got[1] == 1.0f ? "passes" : "fails",
                    want ? "passes" : "fails");
        failures++;
      }
    }
  std::printf("%s depth test (compare functions, bias)\n", failures ? "FAIL" : "ok");
  return failures;
}

// oDepth: stored as 1 - depth when reversed.
int DepthOutChecks(Fixture& f) {
  Gpu& g = f.g;
  id<MTLRenderPipelineState> pipe =
      g.Pipeline(f.vs, f.ps_depth, MTLPixelFormatRGBA32Float, MTLPixelFormatDepth32Float);
  if (!pipe) return 1;
  int failures = 0;
  const float z[3] = {0.5f, 0.5f, 0.5f}, one[3] = {1, 1, 1};
  for (float out : {0.0f, 0.25f, 0.8f, 1.0f})
    for (int rev = 0; rev < 2; rev++) {
      Gpu::Draw d{pipe, g.DepthState(MTLCompareFunctionAlways, true), Cover(z, one), {{1, 1, 1, 1}, {out, 0, 0, 0}},
                  rev != 0};
      std::vector<float> zs;
      g.Run(f.color, f.depth, 0.5, {d}, nullptr, &zs);
      const float want = rev ? 1 - out : out;
      if (std::fabs(zs[0] - want) > 1e-7f) {
        std::printf("FAIL oDepth %g%s: stored %.9g, want %.9g\n", out, rev ? " reversed" : "", zs[0], want);
        failures++;
      }
    }
  std::printf("%s oDepth\n", failures ? "FAIL" : "ok");
  return failures;
}

// A depth texture read by a pixel shader (R_DEPTH_TEX): the perspective
// triangle's depth drawn as D3D9 stores it and reversed, each read back
// through the sampler (reversed: its bit in the prelude's mask) must give
// the same colour, the depth D3D9 stores.
int SampleChecks(Fixture& f) {
  Gpu& g = f.g;
  id<MTLRenderPipelineState> draw = g.Pipeline(f.vs, f.ps, MTLPixelFormatRGBA32Float, MTLPixelFormatDepth32Float);
  id<MTLRenderPipelineState> read = g.Pipeline(f.vs, f.ps_sample, MTLPixelFormatRGBA32Float, MTLPixelFormatInvalid);
  if (!draw || !read) return 1;
  const float z[3] = {0.2f, 0.5f, 0.9f}, w[3] = {1.0f, 2.0f, 4.0f}, one[3] = {1, 1, 1};
  std::vector<Vertex> tri = Cover(z, w);
  for (Vertex& v : tri) v.pos[0] = v.pos[0] * 0.25f - 0.5f * v.pos[3];
  std::vector<float> rgba[2], z_d3d;
  for (int rev = 0; rev < 2; rev++) {
    id<MTLTexture> depth = g.Texture(MTLPixelFormatDepth32Float);
    Gpu::Draw d{draw, g.DepthState(MTLCompareFunctionAlways, true), tri, {{1, 0, 0, 1}, {}}, rev != 0};
    g.Run(f.color, depth, rev ? 0.0 : 1.0, {d}, nullptr, rev ? nullptr : &z_d3d);
    Gpu::Draw r{read, g.DepthState(MTLCompareFunctionAlways, false), Cover(one, one), {}, false};
    r.tex = depth;
    r.depth_samplers = rev ? 1 : 0;
    g.Run(f.color, nil, 0, {r}, &rgba[rev], nullptr);
  }
  int failures = 0;
  for (int i = 0; i < kSize * kSize; i++) {
    const float* a = &rgba[0][i * 4];
    const float* b = &rgba[1][i * 4];
    bool ok = std::fabs(a[0] - z_d3d[i]) <= 1e-7f && a[3] == b[3];
    for (int k = 0; k < 3; k++) ok = ok && a[k] == a[0] && std::fabs(a[k] - b[k]) <= 2e-6f;
    if (!ok && failures++ < 8)
      std::printf("FAIL sample: pixel %d: %.9g %.9g %.9g %.9g, reversed %.9g %.9g %.9g %.9g, stored %.9g\n", i,
                  a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], z_d3d[i]);
  }
  std::printf("%s depth texture read (R_DEPTH_TEX)\n", failures ? "FAIL" : "ok");
  return failures;
}

// DEPTHBIAS on a reversed buffer (DepthBiasOf): the stored value moves by
// D3D9's amount (less the bias: it moves the other way) wherever it is,
// near 0 (far from the camera, where the rasterizer's constant bias is
// thousands of times smaller) as near 0.5, with z (w - z) and from w, over
// a perspective triangle (w 1 to 4), and in a viewport narrower than 0..1.
int BiasChecks(Fixture& f) {
  Gpu& g = f.g;
  id<MTLRenderPipelineState> pipe = g.Pipeline(f.vs, f.ps, MTLPixelFormatRGBA32Float, MTLPixelFormatDepth32Float);
  if (!pipe) return 1;
  const struct {
    const char* name;
    int mode;            // r_position's: 1 w - z (z / w the D3D9 depth `z`), 2 from w
    float z, k0;         // mode 2: k0 / w stored (k1 0): 0.001 to 0.004
    float min_z, max_z;  // the viewport's
  } cases[] = {
      {"z, stored 0.002", 1, 0.998f, 0, 0, 1},
      {"z, stored 0.5", 1, 0.5f, 0, 0, 1},
      {"from w, stored 0.001-0.004", 2, 0, 0.004f, 0, 1},
      {"z, viewport 0.25..0.75, stored 0.5", 1, 0.5f, 0, 0.25f, 0.75f},
  };
  const float w[3] = {1.0f, 2.0f, 4.0f};
  int failures = 0;
  double worst = 0;
  for (const auto& c : cases)
    for (float bias : {1e-5f, -1e-5f, 2e-4f, -2e-4f}) {
      const float z[3] = {c.z, c.z, c.z};
      Gpu::Draw d{pipe, g.DepthState(MTLCompareFunctionAlways, true), Cover(z, w), {{1, 0, 0, 1}, {}}, true,
                  c.min_z, c.max_z};
      if (c.mode == 2) {
        d.mode = 2;
        d.k[0] = c.k0;
      }
      std::vector<float> z0, z1;
      g.Run(f.color, f.depth, 0.0, {d}, nullptr, &z0);
      d.bias = bias;
      g.Run(f.color, f.depth, 0.0, {d}, nullptr, &z1);
      // Within a percent of the bias, and the stored values' rounding.
      const double tol = 0.01 * std::fabs(bias) + 1.2e-7;
      int bad = 0;
      for (int i = 0; i < kSize * kSize; i++) {
        const double moved = double(z1[i]) - double(z0[i]);
        worst = std::fmax(worst, std::fabs(moved + bias) / std::fabs(bias));
        if (std::fabs(moved + bias) > tol && bad++ < 2)
          std::printf("FAIL bias %g, %s: pixel %d stored %.9g, biased %.9g (moved %.4g, want %.4g)\n", bias, c.name,
                      i, z0[i], z1[i], moved, -bias);
      }
      failures += bad;
    }
  std::printf("%s depth bias, reversed (D3D9's amount at 0.002 and 0.5: off by %.2g of it at most)\n",
              failures ? "FAIL" : "ok", worst);
  return failures;
}

// A reversed depth texture's border colour (ReversedDepthBorder): read
// through R_DEPTH_TEX, what D3D9's border colour reads (depth in r, g, b):
// 0 for black, 1 for white.
int BorderChecks(Fixture& f) {
  Gpu& g = f.g;
  id<MTLRenderPipelineState> read = g.Pipeline(f.vs, f.ps_sample, MTLPixelFormatRGBA32Float, MTLPixelFormatInvalid);
  if (!read) return 1;
  const float one[3] = {1, 1, 1};
  // Read outside the texture everywhere.
  std::vector<Vertex> outside = Cover(one, one);
  for (Vertex& v : outside) v.uv[0] = v.uv[1] = 1.5f;
  int failures = 0;
  for (int rev = 0; rev < 2; rev++) {
    id<MTLTexture> depth = g.Texture(MTLPixelFormatDepth32Float);
    g.Run(f.color, depth, 0.3, {}, nullptr, nullptr);  // the texture's content, not read
    for (uint8_t border : {uint8_t(0), uint8_t(1), uint8_t(2)}) {
      Gpu::Draw r{read, g.DepthState(MTLCompareFunctionAlways, false), outside, {}, false};
      r.tex = depth;
      r.depth_samplers = rev ? 1 : 0;
      r.border = rev ? gfx::ReversedDepthBorder(border) : border;
      std::vector<float> rgba;
      g.Run(f.color, nil, 0, {r}, &rgba, nullptr);
      const float want = border == 2 ? 1.0f : 0.0f;
      const float* px = &rgba[(kSize / 2 * kSize + kSize / 2) * 4];
      if (px[0] != want || px[1] != want || px[2] != want) {
        std::printf("FAIL border %u%s: read %g %g %g %g, want %g\n", border, rev ? " reversed" : "", px[0], px[1],
                    px[2], px[3], want);
        failures++;
      }
    }
  }
  std::printf("%s depth texture border colour\n", failures ? "FAIL" : "ok");
  return failures;
}

// A depth texture read by a vertex shader (texldl, R_DEPTH_TEX with the
// vertex state's mask): the depth D3D9 stores, drawn either way.
int VertexSampleChecks(Fixture& f) {
  Gpu& g = f.g;
  id<MTLFunction> vs = g.Compile(VertexSampleVs()), ps = g.Compile(VaryingPs());
  if (!vs || !ps) return 1;
  id<MTLRenderPipelineState> draw = g.Pipeline(f.vs, f.ps, MTLPixelFormatRGBA32Float, MTLPixelFormatDepth32Float);
  id<MTLRenderPipelineState> read = g.Pipeline(vs, ps, MTLPixelFormatRGBA32Float, MTLPixelFormatInvalid);
  if (!draw || !read) return 1;
  const float z[3] = {0.3f, 0.3f, 0.3f}, one[3] = {1, 1, 1};
  int failures = 0;
  for (int rev = 0; rev < 2; rev++) {
    id<MTLTexture> depth = g.Texture(MTLPixelFormatDepth32Float);
    Gpu::Draw d{draw, g.DepthState(MTLCompareFunctionAlways, true), Cover(z, one), {{1, 0, 0, 1}, {}}, rev != 0};
    g.Run(f.color, depth, rev ? 0.0 : 1.0, {d}, nullptr, nullptr);
    Gpu::Draw r{read, g.DepthState(MTLCompareFunctionAlways, false), Cover(one, one), {}, false};
    r.vtex = depth;
    r.vertex_depth_samplers = rev ? 1 : 0;
    std::vector<float> rgba;
    g.Run(f.color, nil, 0, {r}, &rgba, nullptr);
    const float* px = &rgba[(kSize / 2 * kSize + kSize / 2) * 4];
    for (int k = 0; k < 3; k++)
      if (std::fabs(px[k] - 0.3f) > 1e-6f) {
        std::printf("FAIL vertex texture read%s: %g %g %g, want 0.3\n", rev ? " reversed" : "", px[0], px[1], px[2]);
        failures++;
        break;
      }
  }
  std::printf("%s depth texture read by a vertex shader\n", failures ? "FAIL" : "ok");
  return failures;
}

// ---- depth from w

// hlsl::Program::position_matrix of the vertex shaders above, and for
// those that do not qualify, position_note (what it says, in part).
int AnalysisChecks() {
  int failures = 0;
  const struct {
    const char* name;
    Tokens t;
    int want;
    const char* note;
  } cases[] = {
      {"c8-c11", WvpVs(0), 8, ""},
      {"z pushed", WvpVs(1), -1, "more than once"},
      {"z = w", WvpVs(2), -1, "product"},
      {"other translation", WvpVs(3), -1, "not with the translation c12"},
      {"predicated", WvpVs(4), -1, "predicated"},
      {"a row in an if block", WvpVs(5), -1, "if block"},
      {"a loop", WvpVs(6), -1, "loop"},
      {"a subroutine", WvpVs(7), -1, "subroutine"},
      {"c11 defined", WvpVs(8), -1, "defines"},
      {"relative translation", WvpVs(9), -1, "a0"},
      {"translation carried negated", WvpVs(10), -1, "negated"},
      {"xyw and z written apart", WvpVs(11), -1, "more than once"},
      {"a row negated", WvpVs(12), 8, ""},
      {"blit", BlitVs(), -1, ""},
      {"pass-through", PassVs(), -1, ""},
  };
  for (const auto& c : cases) {
    hlsl::Program p;
    std::string err;
    if (!hlsl::TranslateBytecode(c.t.data(), c.t.size(), &p, &err)) {
      std::printf("FAIL translate %s: %s\n", c.name, err.c_str());
      failures++;
    } else if (p.position_matrix != c.want) {
      std::printf("FAIL position matrix of %s: %d, want %d (%s)\n", c.name, p.position_matrix, c.want,
                  p.position_note.c_str());
      failures++;
    } else if ((c.want < 0) == p.position_note.empty() || p.position_note.find(c.note) == std::string::npos) {
      std::printf("FAIL position note of %s: \"%s\", want one with \"%s\"\n", c.name, p.position_note.c_str(),
                  c.note);
      failures++;
    }
  }
  // The registers read, for the device's note of shaders that read the
  // matrix but do not qualify.
  {
    hlsl::Program p;
    std::string err;
    const Tokens t = WvpVs(8);
    if (!hlsl::TranslateBytecode(t.data(), t.size(), &p, &err) || !p.float_reads.test(8) || !p.float_reads.test(10) ||
        p.float_reads.test(11) || p.float_reads.test(12)) {
      std::printf("FAIL float registers read: c8-c10 (c11 defined)\n");
      failures++;
    }
  }
  std::printf("%s position matrix (which vertex shaders qualify)\n", failures ? "FAIL" : "ok");
  return failures;
}

using M4 = std::array<std::array<float, 4>, 4>;

M4 Mul(const M4& a, const M4& b) {  // in float, as a game makes its matrices
  M4 o{};
  for (int r = 0; r < 4; r++)
    for (int k = 0; k < 4; k++) {
      float v = 0;
      for (int i = 0; i < 4; i++) v += a[r][i] * b[i][k];
      o[r][k] = v;
    }
  return o;
}

constexpr float kNear = 0.5f, kFar = 1500.0f;
// The camera: at (cx, cy, cz) looking along +y, z up; D3D's perspective
// (90 degrees across).
M4 ViewProj(double cx, double cy, double cz, float n = kNear, float f = kFar) {
  const M4 view = {{{1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}, {float(-cx), float(-cz), float(-cy), 1}}};
  const float a = f / (f - n);
  const M4 proj = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, a, 1}, {0, 0, -n * a, 0}}};
  return Mul(view, proj);
}
M4 Translate(float x, float y, float z) { return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {x, y, z, 1}}}; }

bool Perspective(const M4& m, gfx::WProjection* p) {
  float r[4][4];
  std::memcpy(r, m.data(), sizeof r);
  return gfx::PerspectiveOf(r, p);
}

// PerspectiveOf and WDepth: what is drawn from w.
int ProjectionChecks() {
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL projection: %s\n", what);
      failures++;
    }
  };
  const double cx = 1182.37, cy = 971.53, cz = 36.81;
  gfx::WProjection p, q;
  const M4 vp = ViewProj(cx, cy, cz);
  check(Perspective(vp, &p), "a perspective camera");
  check(std::fabs(p.a - kFar / (kFar - kNear)) < 1e-6 && std::fabs(p.n - kNear) <= p.tol, "its a and n");
  check(Perspective(Mul(Translate(1180, 1000, 35), vp), &q) && std::fabs(q.n - kNear) <= q.tol,
        "an object's");
  M4 ortho = {{{0.01f, 0, 0, 0}, {0, 0, 0.002f, 0}, {0, 0.01f, 0, 0}, {-11.8f, 0.3f, -9.7f, 1}}};
  check(!Perspective(ortho, &q), "orthographic: not one");
  M4 oblique = vp;
  oblique[0][2] += 0.3f;  // the near plane tilted (a reflection's clip plane)
  check(!Perspective(oblique, &q), "oblique: not one");
  check(!Perspective(M4{}, &q), "zero: not one");

  gfx::WDepth w;
  check(w.Use(p) && w.n == p.n && w.a == p.a, "the first draw after a clear sets the projection");
  gfx::WProjection near2 = p;
  near2.n = p.n * 1.02;
  check(!w.Use(near2), "another near plane: not from w");
  gfx::WProjection far2 = p;
  far2.a = p.a * (1 + 1e-4);
  check(!w.Use(far2), "another far plane: not from w");
  gfx::WProjection noisy = p;
  noisy.n = p.n * (1 + 1e-4);
  check(w.Use(noisy) && w.n == p.n, "the same projection rounded otherwise: from w, the content's kept");
  const gfx::WProjection exact = gfx::PerspectiveOfPlanes(kNear, kFar);
  w.fresh = true;
  check(w.Use(noisy, &exact) && w.n == double(kNear) && w.a == exact.a, "the game's own planes, if they agree");
  w.fresh = true;
  check(w.Use(near2, &exact) && w.n == near2.n, "else the matrix's");
  w.fresh = true;
  check(!w.Use(p, nullptr, false) && w.fresh, "a draw without depth test or write: not from w, nothing set");
  check(w.Use(near2) && w.n == near2.n && !w.fresh, "the next one with depth sets it");

  // The far plane, relative to the near one (a - 1): with n / f 1 / 50
  // (a - 1 0.02), 0.1% further is another projection, 0.001% the same.
  {
    gfx::WDepth wide;
    gfx::WProjection base = gfx::PerspectiveOfPlanes(1, 50), off = gfx::PerspectiveOfPlanes(1, 50.05),
                     close = gfx::PerspectiveOfPlanes(1, 50.0005);
    base.a_tol = off.a_tol = close.a_tol = 1e-6 * base.a;  // as PerspectiveOf's
    base.tol = off.tol = close.tol = 1e-3;
    check(wide.Use(base) && !wide.Use(off), "a far plane 0.1% further (n / f 1 / 50): another projection");
    check(wide.Use(close), "0.001% further: the same");
  }

  // Objects' matrices, rotated and kilometres from the origin, under a
  // turned and tilted camera: their a and n within PerspectiveOf's
  // rounding (a_tol, tol) of the planes', so the same projection.
  {
    auto rot = [](int axis, float t) {  // about z (0) or x (1)
      const float c = std::cos(t), s = std::sin(t);
      return axis == 0 ? M4{{{c, s, 0, 0}, {-s, c, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}
                       : M4{{{1, 0, 0, 0}, {0, c, s, 0}, {0, -s, c, 0}, {0, 0, 0, 1}}};
    };
    const float a = kFar / (kFar - kNear);
    const M4 axes_proj = Mul(M4{{{1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}}},
                             M4{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, a, 1}, {0, 0, -kNear * a, 0}}});
    uint32_t seed = 12345;
    auto rnd = [&]() {  // 0..1
      seed = seed * 1664525u + 1013904223u;
      return float(seed >> 8) / float(1u << 24);
    };
    double worst_a = 0, worst_n = 0;
    int bad = 0;
    for (int i = 0; i < 500; i++) {
      const float cxr = (rnd() - 0.5f) * 5000, cyr = (rnd() - 0.5f) * 5000, czr = rnd() * 200;
      const M4 cam = Mul(Mul(Translate(-cxr, -cyr, -czr), rot(0, rnd() * 6.2832f)), rot(1, (rnd() - 0.5f) * 1.2f));
      const M4 world = Mul(rot(0, rnd() * 6.2832f), Translate(cxr + (rnd() - 0.5f) * 200, cyr + (rnd() - 0.5f) * 200,
                                                            czr + (rnd() - 0.5f) * 20));
      gfx::WProjection q2;
      if (!Perspective(Mul(world, Mul(cam, axes_proj)), &q2)) {
        bad++;
        continue;
      }
      worst_a = std::fmax(worst_a, std::fabs(q2.a - exact.a) / q2.a_tol);
      worst_n = std::fmax(worst_n, std::fabs(q2.n - exact.n) / q2.tol);
      if (!gfx::WDepth::Same(exact, q2)) bad++;
    }
    if (bad) {
      std::printf("FAIL projection: %d of 500 rotated objects' matrices not the planes' (a off by %.2g of its "
                  "tolerance, n by %.2g)\n",
                  bad, worst_a, worst_n);
      failures++;
    } else {
      std::printf("   rotated objects: a off by %.2g of its tolerance at most, n by %.2g\n", worst_a, worst_n);
    }
  }
  std::printf("%s projection (orthographic, oblique; one per content)\n", failures ? "FAIL" : "ok");
  return failures;
}

// The camera of the layers below: 2.5 km from the origin (the far side of
// a city-sized map).
constexpr double kCx = -1817.63, kCy = 2471.53, kCz = 136.81;

// A layer at y = kCy + dist across the whole view, its vertices less
// `org` (an object's world translation; 0: world-space vertices).
std::vector<Vertex> Layer(double dist, const float org[3]) {
  const double yw = kCy + dist, h = dist * 2;
  const double q[4][2] = {{-h, -h}, {h, -h}, {-h, h}, {h, h}};
  const int order[6] = {0, 1, 2, 2, 1, 3};
  std::vector<Vertex> v;
  for (int i : order)
    v.push_back({{float(kCx + q[i][0] - org[0]), float(yw - org[1]), float(kCz + q[i][1] - org[2]), 1}, {}});
  return v;
}

// A world-view-projection as vertex shader constants c8-c11 (WvpVs).
std::vector<float> WvpConsts(const M4& m) {
  std::vector<float> c(12 * 4, 0.0f);
  for (int r = 0; r < 4; r++)
    for (int k = 0; k < 4; k++) c[(8 + r) * 4 + k] = m[r][k];
  return c;
}

// Two layers 3 mm apart, 30 m from the camera, in world coordinates 2.5
// km from the origin (the far side of a city-sized map), each with its own
// matrix (one with its world translation, one with world-space vertices);
// then layers past the far plane and before the near one. Drawn
// reversed, z from the matrix and from w. (From z, this GPU's rounding
// decides the order; it is printed, not checked.)
int FromWChecks(Gpu& g, id<MTLTexture> color, id<MTLTexture> depth) {
  id<MTLFunction> vs = g.Compile(WvpVs(0)), ps = g.Compile(ConstPs(false));
  if (!vs || !ps) return 1;
  id<MTLRenderPipelineState> pipe = g.Pipeline(vs, ps, MTLPixelFormatRGBA32Float, MTLPixelFormatDepth32Float);
  if (!pipe) return 1;
  const M4 vp = ViewProj(kCx, kCy, kCz);
  const float ox = -1820, oy = 2500, oz = 135;  // the first layer's world translation
  const M4 wvp_a = Mul(Translate(ox, oy, oz), vp);
  auto layer = [](double dist, const float org[3]) { return Layer(dist, org); };
  const float at_a[3] = {ox, oy, oz}, at_0[3] = {0, 0, 0};
  auto consts = WvpConsts;
  int failures = 0;
  int behind[3] = {};
  const gfx::WProjection exact = gfx::PerspectiveOfPlanes(kNear, kFar);
  // 1: z (w - z); 2: from w, the matrix's planes; 3: from w, the exact ones.
  for (int mode = 1; mode <= 3; mode++) {
    gfx::WDepth wd;
    auto draw = [&](const M4& m, std::vector<Vertex> v, MTLCompareFunction f, float g_) {
      Gpu::Draw d{pipe, g.DepthState(f, true), std::move(v), {{1, g_, 0, 1}, {}}, true};
      d.vc = consts(m);
      gfx::WProjection p;
      if (mode >= 2 && Perspective(m, &p) && wd.Use(p, mode == 3 ? &exact : nullptr)) {
        d.mode = 2;
        d.k[0] = wd.K0();
        d.k[1] = wd.K1();
      } else if (mode >= 2) {
        std::printf("FAIL from w: a layer's draw does not qualify\n");
        failures++;
      }
      return d;
    };
    // The far layer (green 0) first, the near one (green 1) over it with
    // LESS, as D3D9 tests (GREATER reversed): the near one must cover it
    // everywhere.
    const double dist = 30.0, gap = 0.003;
    std::vector<Gpu::Draw> draws = {draw(wvp_a, layer(dist + gap, at_a), MTLCompareFunctionAlways, 0),
                                    draw(vp, layer(dist, at_0), MTLCompareFunctionGreater, 1)};
    // And the other way round: the far one after, it must stay hidden.
    std::vector<Gpu::Draw> back = {draw(vp, layer(dist, at_0), MTLCompareFunctionAlways, 1),
                                   draw(wvp_a, layer(dist + gap, at_a), MTLCompareFunctionGreater, 0)};
    std::vector<float> rgba, z;
    g.Run(color, depth, 0.0, draws, &rgba, &z);
    int wrong = 0;
    // What D3D9 stores for the near layer, reversed: 1 - a (w - n) / w;
    // from w, with the content's a and n (the matrix's rounded: n off by
    // up to its tol).
    const double a = mode == 2 ? wd.a : exact.a, n = mode == 2 ? wd.n : exact.n;
    const double want = 1.0 - a * (dist - n) / dist;
    double worst = 0;
    for (int i = 0; i < kSize * kSize; i++) {
      if (rgba[i * 4 + 1] != 1.0f) wrong++;
      worst = std::fmax(worst, std::fabs(z[i] - want));
    }
    g.Run(color, depth, 0.0, back, &rgba, nullptr);
    for (int i = 0; i < kSize * kSize; i++)
      if (rgba[i * 4 + 1] != 1.0f) wrong++;
    behind[mode - 1] = wrong;
    // From w: exact (the order, and the value within float rounding).
    if (mode >= 2 && (wrong || worst > 2e-7)) {
      std::printf("FAIL from w: %d pixels of the far layer in front, stored off by %.3g\n", wrong, worst);
      failures++;
    }
    if (mode == 1 && worst > 1e-4) {
      std::printf("FAIL z: stored off by %.3g\n", worst);
      failures++;
    }
    // Clipping as D3D9's: nothing past the far plane or before the near one.
    for (double d : {kFar * 1.05, kNear * 0.8}) {
      std::vector<Gpu::Draw> one = {draw(vp, layer(d, at_0), MTLCompareFunctionAlways, 1)};
      g.Run(color, depth, 0.0, one, &rgba, nullptr);
      int drawn = 0;
      for (int i = 0; i < kSize * kSize; i++) drawn += rgba[i * 4] != 0.0f;
      if (drawn) {
        std::printf("FAIL %s: a layer at %g m drawn (%d pixels)\n", mode >= 2 ? "from w" : "z", d, drawn);
        failures++;
      }
    }
  }
  std::printf("%s depth from w (3 mm at 30 m: %d of %d pixels wrong from z, %d and %d from w; clipping)\n",
              failures ? "FAIL" : "ok", behind[0], 2 * kSize * kSize, behind[1], behind[2]);
  return failures;
}

// A depth texture drawn from w, read by a pixel shader (R_DEPTH_TEX): the
// depth D3D9 stores for the layer, within float rounding, as from z.
int FromWSampleChecks(Fixture& f) {
  Gpu& g = f.g;
  id<MTLFunction> wvs = g.Compile(WvpVs(0));
  if (!wvs) return 1;
  id<MTLRenderPipelineState> draw = g.Pipeline(wvs, f.ps, MTLPixelFormatRGBA32Float, MTLPixelFormatDepth32Float);
  id<MTLRenderPipelineState> read = g.Pipeline(f.vs, f.ps_sample, MTLPixelFormatRGBA32Float, MTLPixelFormatInvalid);
  if (!draw || !read) return 1;
  const M4 vp = ViewProj(kCx, kCy, kCz);
  const gfx::WProjection exact = gfx::PerspectiveOfPlanes(kNear, kFar);
  const float at_0[3] = {0, 0, 0}, one[3] = {1, 1, 1};
  const double dist = 30.0, want = exact.a * (dist - exact.n) / dist;
  int failures = 0;
  double worst[2] = {};
  for (int mode = 1; mode <= 2; mode++) {
    id<MTLTexture> depth = g.Texture(MTLPixelFormatDepth32Float);
    Gpu::Draw d{draw, g.DepthState(MTLCompareFunctionAlways, true), Layer(dist, at_0), {{1, 0, 0, 1}, {}}, true};
    d.vc = WvpConsts(vp);
    gfx::WProjection p;
    gfx::WDepth wd;
    if (mode == 2) {
      if (!Perspective(vp, &p) || !wd.Use(p, &exact)) return 1;
      d.mode = 2;
      d.k[0] = wd.K0();
      d.k[1] = wd.K1();
    }
    g.Run(f.color, depth, 0.0, {d}, nullptr, nullptr);
    Gpu::Draw r{read, g.DepthState(MTLCompareFunctionAlways, false), Cover(one, one), {}, false};
    r.tex = depth;
    r.depth_samplers = 1;
    std::vector<float> rgba;
    g.Run(f.color, nil, 0, {r}, &rgba, nullptr);
    for (int i = 0; i < kSize * kSize; i++) worst[mode - 1] = std::fmax(worst[mode - 1], std::fabs(rgba[i * 4] - want));
  }
  if (worst[0] > 1e-4 || worst[1] > 2e-7) {
    std::printf("FAIL depth texture read, from w: off by %.3g from z, %.3g from w\n", worst[0], worst[1]);
    failures++;
  }
  std::printf("%s depth texture read, from w (off by %.3g from z, %.3g from w)\n", failures ? "FAIL" : "ok", worst[0],
              worst[1]);
  return failures;
}

}  // namespace

int main() {
  @autoreleasepool {
    int failures = TableChecks() + AnalysisChecks() + ProjectionChecks();
    Gpu g;
    if (!g.dev) {
      std::printf("skipped: no Metal device\n");
      return failures ? 1 : 77;
    }
    Fixture f{g, nil, nil, nil, nil, g.Texture(MTLPixelFormatRGBA32Float), g.Texture(MTLPixelFormatDepth32Float)};
    f.vs = g.Compile(PassVs());
    f.ps = g.Compile(ConstPs(false));
    f.ps_depth = g.Compile(ConstPs(true));
    f.ps_sample = g.Compile(SamplePs());
    if (!f.vs || !f.ps || !f.ps_depth || !f.ps_sample) return 1;
    std::vector<float> z_d3d, z_rev;
    failures += PositionChecks(f, &z_d3d, &z_rev);
    failures += SampleChecks(f);
    failures += CompareChecks(f);
    failures += DepthOutChecks(f);
    failures += BiasChecks(f);
    failures += BorderChecks(f);
    failures += VertexSampleChecks(f);
    failures += FromWChecks(g, f.color, f.depth);
    failures += FromWSampleChecks(f);
    return failures ? 1 : 0;
  }
}
