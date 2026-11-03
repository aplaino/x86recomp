// Direct3D 9 draws. The device's state reaches gfx::Device at each draw
// (render states, viewport and scissor that changed; targets when bound). Draws with
// shaders (translated from D3DXCompileShader's HLSL or from bytecode)
// become gfx::ShaderDraw: the vertex declaration (or FVF) is matched to the
// vertex shader's input semantics (once per pair), and the streams, indices, constant
// registers and samplers are passed as they are. Draws without shaders
// use gfx::Device's fixed-function pipeline (D3D7 semantics) with the
// D3D9 state in its terms: sampler states at D3D7's texture-stage
// positions, D3D9's transform numbers mapped. Mixing a shader with the
// fixed-function pipeline and fixed-function draws with a vertex
// declaration (no FVF) are not supported yet (fatal, named). A pixel shader
// after the fixed-function vertex stage gets a vertex shader standing in
// for that stage (FfVertexSource).

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "runtime/hle/d3d9_impl.h"
#include "runtime/hle/hle.h"
#include "runtime/log.h"
#include "runtime/perf.h"
#include "runtime/platform.h"

namespace recomp::d3d9 {

namespace {

// What SyncState sends: the render states that changed, and the viewport
// and scissor rectangle if they changed.
struct StateChanges {
  uint32_t states[256][2];
  uint32_t count = 0;
  gfx::Viewport viewport;
  gfx::Rect scissor;
  bool has_viewport = false, has_scissor = false;
};

// Only the render states marked dirty are compared (DXVK's dirty flags,
// D3D9DeviceEx::PrepareDraw); one set back to the value gfx has is not
// sent. Recorded as sent.
void TakeChanges(Device& d, StateChanges& c) {
  for (uint32_t w = 0; w < 4; w++) {
    uint64_t m = d.rs.dirty[w];
    d.rs.dirty[w] = 0;
    for (; m; m &= m - 1) {
      const uint32_t s = w * 64 + uint32_t(std::countr_zero(m));
      if (d.synced && d.sent_rs[s] == d.rs[s]) continue;
      c.states[c.count][0] = s;
      c.states[c.count][1] = d.rs[s];
      c.count++;
      d.sent_rs[s] = d.rs[s];
    }
  }
  const guest::D3DVIEWPORT9& v = d.viewport;
  if (!d.synced || std::memcmp(&v, &d.sent_viewport, sizeof v)) {
    c.viewport = {v.X, v.Y, v.Width, v.Height, v.MinZ, v.MaxZ};
    c.has_viewport = true;
    d.sent_viewport = v;
  }
  const guest::D3DRECT& r = d.scissor;
  if (!d.synced || std::memcmp(&r, &d.sent_scissor, sizeof r)) {
    c.scissor = {r.x1, r.y1, r.x2, r.y2};
    c.has_scissor = true;
    d.sent_scissor = r;
  }
  d.synced = true;
}

}  // namespace

void SyncState(Device& d) {
  StateChanges c;
  TakeChanges(d, c);
  for (uint32_t i = 0; i < c.count; i++) d.gfx->SetRenderState(c.states[i][0], c.states[i][1]);
  if (c.has_viewport) d.gfx->SetViewport(c.viewport);
  if (c.has_scissor) d.gfx->SetScissor(c.scissor);
}

namespace {

// D3DDECLTYPE, D3DDECLUSAGE
enum : uint8_t {
  FLOAT1 = 0, FLOAT2 = 1, FLOAT3 = 2, FLOAT4 = 3, D3DCOLOR = 4, UBYTE4 = 5, UNUSED = 17,
};
enum : uint8_t {
  USAGE_POSITION = 0, USAGE_BLENDWEIGHT = 1, USAGE_BLENDINDICES = 2, USAGE_NORMAL = 3,
  USAGE_PSIZE = 4, USAGE_TEXCOORD = 5, USAGE_POSITIONT = 9, USAGE_COLOR = 10,
};

// The vertex declaration D3D9 makes for an FVF.
std::vector<guest::D3DVERTEXELEMENT9> FvfElements(uint32_t fvf) {
  std::vector<guest::D3DVERTEXELEMENT9> e;
  uint16_t off = 0;
  auto add = [&](uint8_t type, uint8_t usage, uint8_t index, uint16_t size) {
    e.push_back({0, off, type, 0, usage, index});
    off = uint16_t(off + size);
  };
  uint32_t pos = fvf & 0x400E;
  if (pos == 0x004) {  // XYZRHW
    add(FLOAT4, USAGE_POSITIONT, 0, 16);
  } else if (pos == 0x4002) {  // XYZW
    add(FLOAT4, USAGE_POSITION, 0, 16);
  } else if (pos >= 0x002 && pos <= 0x00E) {
    add(FLOAT3, USAGE_POSITION, 0, 12);
    // XYZB1-5: blend weights; with LASTBETA_UBYTE4 / _D3DCOLOR the last
    // one is the blend indices.
    int betas = pos == 0x002 ? 0 : int((pos - 4) / 2);
    bool indices = fvf & (0x1000 | 0x8000);
    int weights = betas - (indices ? 1 : 0);
    if (weights > 0) add(uint8_t(FLOAT1 + weights - 1), USAGE_BLENDWEIGHT, 0, uint16_t(4 * weights));
    if (indices) add((fvf & 0x8000) ? D3DCOLOR : UBYTE4, USAGE_BLENDINDICES, 0, 4);
  }
  if (fvf & 0x010) add(FLOAT3, USAGE_NORMAL, 0, 12);
  if (fvf & 0x020) add(FLOAT1, USAGE_PSIZE, 0, 4);
  if (fvf & 0x040) add(D3DCOLOR, USAGE_COLOR, 0, 4);
  if (fvf & 0x080) add(D3DCOLOR, USAGE_COLOR, 1, 4);
  uint32_t ntex = (fvf >> 8) & 0xF;
  for (uint32_t i = 0; i < ntex && i < 8; i++) {
    static const uint8_t kTypes[4] = {FLOAT2, FLOAT3, FLOAT4, FLOAT1};
    uint8_t t = kTypes[(fvf >> (16 + 2 * i)) & 3];
    add(t, USAGE_TEXCOORD, uint8_t(i), uint16_t(4 * (t - FLOAT1 + 1)));
  }
  e.push_back({0xFF, 0, UNUSED, 0, 0, 0});  // D3DDECL_END
  return e;
}

// Vertices (or indices) a draw of `count` primitives uses.
uint32_t VerticesFor(uint32_t prim, uint32_t count) {
  switch (prim) {
    case 1: return count;          // POINTLIST
    case 2: return count * 2;      // LINELIST
    case 3: return count + 1;      // LINESTRIP
    case 4: return count * 3;      // TRIANGLELIST
    case 5: case 6: return count + 2;  // TRIANGLESTRIP, TRIANGLEFAN
    default: return 0;
  }
}

// The fixed-function state in gfx::Device's (D3D7's) terms.
void SyncFixedFunction(Device& d) {
  // Stages from the first with COLOROP DISABLE (1) on are unused.
  bool active = true;
  for (uint32_t s = 0; s < 8; s++) {
    active = active && d.tss[s][1] != 1;
    for (uint32_t t = 1; t < 33; t++)
      if (t < 13 || t > 21) d.gfx->SetTextureStageState(s, t, d.tss[s][t]);
    // D3D9 moved these to sampler states. Filters: D3DTEXF (NONE 0, POINT
    // 1, LINEAR 2, ANISOTROPIC 3) to D3D7's magnification (anisotropic 5)
    // and mip (NONE 1, POINT 2, LINEAR 3) values.
    const uint32_t* ss = d.ss[s];
    d.gfx->SetTextureStageState(s, 13, ss[1]);  // ADDRESSU
    d.gfx->SetTextureStageState(s, 14, ss[2]);  // ADDRESSV
    d.gfx->SetTextureStageState(s, 15, ss[4]);  // BORDERCOLOR
    d.gfx->SetTextureStageState(s, 16, ss[5] == 3 ? 5 : ss[5]);  // MAGFILTER
    d.gfx->SetTextureStageState(s, 17, ss[6]);  // MINFILTER
    d.gfx->SetTextureStageState(s, 18, ss[7] + 1);  // MIPFILTER
    d.gfx->SetTextureStageState(s, 19, ss[8]);  // MIPMAPLODBIAS
    d.gfx->SetTextureStageState(s, 20, ss[9]);  // MAXMIPLEVEL
    d.gfx->SetTextureStageState(s, 21, ss[10]);  // MAXANISOTROPY
    Texture* t = active ? d.textures[s].obj : nullptr;
    if (t && t->cube) FatalF("d3d9: fixed-function stage %u with a cube texture", s);
    d.gfx->SetTexture(s, t ? t->tex : 0);
  }
  // D3DTS_VIEW 2, PROJECTION 3 and TEXTURE0-7 16-23 are D3D7's numbers;
  // WORLD-WORLD3 are 256-259 (D3D7: 1, 4-6).
  static const std::pair<uint32_t, uint32_t> kTransforms[] = {
      {2, 2}, {3, 3}, {256, 1}, {257, 4}, {258, 5}, {259, 6}, {16, 16}, {17, 17}, {18, 18},
      {19, 19}, {20, 20}, {21, 21}, {22, 22}, {23, 23},
  };
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  for (const auto& [d3d9, d3d7] : kTransforms) {
    auto it = d.transforms.find(d3d9);
    d.gfx->SetTransform(d3d7, it != d.transforms.end() ? it->second.data() : kIdentity);
  }
}

// A draw without shaders: stream 0 with the FVF.
void DrawFixed(Device& d, gfx::ShaderDraw& sd, const char* what) {
  if (d.decl.obj || !d.fvf)
    FatalF("d3d9: fixed-function %s with a vertex declaration (only FVFs are supported)", what);
  if (!sd.streams[0].data) {
    const Device::Stream& st = d.streams[0];
    Buffer* b = st.buffer.obj;
    if (!b || st.offset >= b->length) return;
    sd.streams[0] = {static_cast<const uint8_t*>(GPtr(b->mem)) + st.offset, b->length - st.offset,
                     st.stride};
  }
  const gfx::VertexStream& vs = sd.streams[0];
  uint32_t first = uint32_t(std::max(sd.base_vertex, 0));
  // Vertices from base_vertex; indexed draws index from there.
  uint32_t nv = sd.indices ? sd.min_index + sd.vertex_count : sd.vertex_count;
  if (!vs.stride || uint64_t(first + nv) * vs.stride > vs.size)
    FatalF("d3d9: fixed-function %s reads past its vertex buffer", what);
  std::vector<uint16_t> idx;
  uint32_t ni = 0;
  if (sd.indices) {
    ni = VerticesFor(sd.prim, sd.primitives);
    idx.resize(ni);
    for (uint32_t i = 0; i < ni; i++) {
      uint32_t v = sd.index_size == 4 ? static_cast<const uint32_t*>(sd.indices)[i]
                                      : static_cast<const uint16_t*>(sd.indices)[i];
      if (v > 0xFFFF) FatalF("d3d9: fixed-function %s with an index above 65535", what);
      idx[i] = uint16_t(v);
    }
  }
  SyncState(d);
  SyncFixedFunction(d);
  d.gfx->Draw(sd.prim, d.fvf, vs.data + size_t(first) * vs.stride, nv, vs.stride,
              sd.indices ? idx.data() : nullptr, ni);
}

// The fixed-function vertex stage of an FVF draw as HLSL, for draws that
// pair it with a pixel shader. Constants c0-c3: the columns of
// World*View*Projection (untransformed vertices); c4: the viewport's x, y,
// 2/width, -2/height (pretransformed XYZRHW ones, which become clip space
// with w = 1/rhw); c5-c8: the columns of World*View; c12 + 4s: those of
// texture stage s's transform. The outputs are what that stage gives a
// pixel shader: diffuse (white without vertex colour) and specular
// (black), and each stage's texture coordinates: its TEXCOORDINDEX set, or
// generated in camera space (normal, position, reflection vector, sphere
// map), then through the stage's transform when TEXTURETRANSFORMFLAGS has a
// count (D3D's rules: a 2D set enters as (u, v, 1, 0)). Not done here
// (warned once): lighting, fog.
std::string FfVertexSource(const Device& d, uint32_t fvf) {
  const bool rhw = (fvf & 0x400E) == 0x004;
  const bool normal = (fvf & 0x010) != 0;
  const uint32_t ntex = (fvf >> 8) & 0xF;
  std::string in = "struct VSIN {\n  float4 pos : POSITION;\n";
  if (normal && !rhw) in += "  float3 nrm : NORMAL;\n";
  if (fvf & 0x040) in += "  float4 diff : COLOR0;\n";
  if (fvf & 0x080) in += "  float4 spec : COLOR1;\n";
  for (uint32_t i = 0; i < ntex && i < 8; i++)
    in += "  float4 t" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";\n";
  in += "};\n";
  std::string out = "struct VSOUT {\n  float4 pos : POSITION;\n  float4 diff : COLOR0;\n"
                    "  float4 spec : COLOR1;\n";
  std::string body;
  if (rhw) {
    body += "  float w = v.pos.w != 0.0 ? 1.0 / v.pos.w : 1.0;\n"
            "  o.pos = float4((v.pos.x - vp.x) * vp.z - 1.0, (v.pos.y - vp.y) * vp.w + 1.0,"
            " v.pos.z, 1.0) * w;\n";
  } else {
    body += "  float4 p = float4(v.pos.xyz, 1.0);\n"
            "  o.pos = float4(dot(p, m0), dot(p, m1), dot(p, m2), dot(p, m3));\n";
  }
  body += (fvf & 0x040) ? "  o.diff = v.diff;\n" : "  o.diff = float4(1.0, 1.0, 1.0, 1.0);\n";
  body += (fvf & 0x080) ? "  o.spec = v.spec;\n" : "  o.spec = float4(0.0, 0.0, 0.0, 0.0);\n";
  bool camera = false;  // camera-space position / normal needed
  std::string gen;
  for (uint32_t s = 0; s < 8; s++) {
    const uint32_t set = d.tss[s][11] & 0xFFFF, tci = d.tss[s][11] >> 16;
    const uint32_t count = d.tss[s][24] & 0xFF;
    const std::string n = std::to_string(s);
    std::string c;
    if (tci == 0 || rhw) {
      if (set >= ntex) continue;
      // FVF texture coordinate size: 0 2D, 1 3D, 2 4D, 3 1D.
      const uint32_t size = (fvf >> (16 + 2 * set)) & 3;
      const std::string t = "v.t" + std::to_string(set);
      c = !count ? t
          : size == 0 ? "float4(" + t + ".xy, 1.0, 0.0)"
          : size == 3 ? "float4(" + t + ".x, 1.0, 0.0, 0.0)"
          : size == 1 ? "float4(" + t + ".xyz, 1.0)" : t;
    } else {
      camera = true;
      if (tci == 1) c = "float4(cn, 1.0)";
      else if (tci == 2) c = "float4(cp, 1.0)";
      else if (tci == 3) c = "float4(cr, 1.0)";
      else c = "float4(cr.xy / (2.0 * sqrt(cr.x * cr.x + cr.y * cr.y + (cr.z + 1.0) * (cr.z + 1.0))) + 0.5, 0.0, 1.0)";
    }
    out += "  float4 t" + n + " : TEXCOORD" + n + ";\n";
    if (count) {
      const std::string m = "tm" + n + "_";
      gen += "  { float4 c = " + c + ";\n    o.t" + n + " = float4(dot(c, " + m + "0), dot(c, " + m +
             "1), dot(c, " + m + "2), dot(c, " + m + "3)); }\n";
    } else {
      gen += "  o.t" + n + " = " + c + ";\n";
    }
  }
  if (camera) {
    // Camera space (World*View); the local viewer's eye vector.
    body += "  float3 cp = float3(dot(p, wv0), dot(p, wv1), dot(p, wv2));\n";
    body += normal ? "  float3 cn = normalize(float3(dot(v.nrm, wv0.xyz), dot(v.nrm, wv1.xyz),"
                     " dot(v.nrm, wv2.xyz)));\n"
                   : "  float3 cn = float3(0.0, 0.0, 0.0);\n";
    body += "  float3 e = -normalize(cp);\n  float3 cr = 2.0 * dot(e, cn) * cn - e;\n";
  }
  body += gen;
  out += "};\n";
  std::string decl = "float4 m0 : register(c0);\nfloat4 m1 : register(c1);\nfloat4 m2 : register(c2);\n"
                     "float4 m3 : register(c3);\nfloat4 vp : register(c4);\nfloat4 wv0 : register(c5);\n"
                     "float4 wv1 : register(c6);\nfloat4 wv2 : register(c7);\n";
  for (uint32_t s = 0; s < 8; s++)
    if (d.tss[s][24] & 0xFF)
      for (uint32_t r = 0; r < 4; r++)
        decl += "float4 tm" + std::to_string(s) + "_" + std::to_string(r) + " : register(c" +
                std::to_string(12 + 4 * s + r) + ");\n";
  return decl + in + out + "VSOUT main(VSIN v) {\n  VSOUT o;\n" + body + "  return o;\n}\n";
}

// Whether the fixed-function vertex state is beyond FfVertexSource.
void NoteFfVertexGaps(const Device& d, uint32_t fvf) {
  static bool warned_light = false, warned_fog = false;
  const bool rhw = (fvf & 0x400E) == 0x004;
  if (!rhw && d.rs[137] && (fvf & 0x010) && !warned_light) {  // LIGHTING with normals
    warned_light = true;
    WarnF("d3d9: fixed-function lighting with a pixel shader is not emulated (vertex colours used)\n");
  }
  if (!rhw && d.rs[28] && d.rs[140] && !warned_fog) {  // FOGENABLE with FOGVERTEXMODE
    warned_fog = true;
    WarnF("d3d9: fixed-function vertex fog with a pixel shader is not emulated\n");
  }
}

// The stand-in vertex shader for d.fvf (compiled once per source, found by
// the state the source is made from) and its constants in `c`.
const Device::FfVertexShader& FfVertexShaderFor(Device& d, float (*c)[4]) {
  // What FfVertexSource reads: the FVF, and each stage's TEXCOORDINDEX and
  // TEXTURETRANSFORMFLAGS.
  std::array<uint32_t, 17> key;
  key[0] = d.fvf;
  for (uint32_t s = 0; s < 8; s++) {
    key[1 + 2 * s] = d.tss[s][11];
    key[2 + 2 * s] = d.tss[s][24];
  }
  const Device::FfVertexShader*& found = d.ff_vs_by_state[key];
  if (!found) {
    std::string src = FfVertexSource(d, d.fvf);
    auto it = d.ff_vs.find(src);
    if (it == d.ff_vs.end()) {
      auto p = std::make_shared<hlsl::Program>();
      std::string err;
      if (!hlsl::Compile(src, {}, "main", "vs_2_0", 0, p.get(), &err))
        FatalF("d3d9: the fixed-function vertex shader for FVF %X does not translate: %s", d.fvf,
               err.c_str());
      Device::FfVertexShader v;
      v.id = d.gfx->CreateShader(*p);
      v.program = std::move(p);
      it = d.ff_vs.emplace(src, std::move(v)).first;
    }
    found = &it->second;
  }
  NoteFfVertexGaps(d, d.fvf);
  // World * View * Projection (row vectors), stored by column.
  static const std::array<float, 16> kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto get = [&](uint32_t t) {
    auto f = d.transforms.find(t);
    return f != d.transforms.end() ? f->second : kIdentity;
  };
  auto mul = [](const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> r{};
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
        for (int k = 0; k < 4; k++) r[size_t(i * 4 + j)] += a[size_t(i * 4 + k)] * b[size_t(k * 4 + j)];
    return r;
  };
  const std::array<float, 16> wv = mul(get(256), get(2));
  std::array<float, 16> m = mul(wv, get(3));
  for (int j = 0; j < 4; j++)
    for (int i = 0; i < 4; i++) {
      c[j][i] = m[size_t(i * 4 + j)];
      c[5 + j][i] = wv[size_t(i * 4 + j)];
    }
  // Texture transforms (D3DTS_TEXTURE0 + s = 16 + s), by column.
  for (uint32_t s = 0; s < 8; s++) {
    if (!(d.tss[s][24] & 0xFF)) continue;
    const std::array<float, 16> t = get(16 + s);
    for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++) c[12 + 4 * s + uint32_t(j)][i] = t[size_t(i * 4 + j)];
  }
  const guest::D3DVIEWPORT9& v = d.viewport;
  c[4][0] = float(v.X);
  c[4][1] = float(v.Y);
  c[4][2] = v.Width ? 2.0f / float(v.Width) : 0.0f;
  c[4][3] = v.Height ? -2.0f / float(v.Height) : 0.0f;
  return *found;
}

// The fixed-function texture stages (D3D9 numbering) as HLSL, for draws
// with a vertex shader and no pixel shader. Stage s samples its texture at
// TEXCOORD[TEXCOORDINDEX] (the vertex shader's oT registers); c0 is
// TEXTUREFACTOR. Each stage's colour and alpha operations, arguments and
// modifiers, and its result to CURRENT or TEMP, are D3D's, as are
// projected coordinates; specular is added after the last stage when
// SPECULARENABLE is set. Not done here (fatal or warned once): bump
// mapping, PREMODULATE, fog.
std::string FfPixelSource(const Device& d) {
  std::string decl = "float4 tf : register(c0);\n";
  std::string in = "struct PSIN {\n  float4 diff : COLOR0;\n  float4 spec : COLOR1;\n";
  std::string body = "  float4 cur = i.diff;\n  float4 temp = float4(0.0, 0.0, 0.0, 0.0);\n"
                     "  float4 tex;\n  float4 r;\n";
  std::set<uint32_t> sets;
  auto arg = [&](uint32_t a, bool alpha_replicate_ok) {
    std::string v;
    switch (a & 0xF) {
      case 0: v = "i.diff"; break;
      case 1: v = "cur"; break;
      case 2: v = "tex"; break;
      case 3: v = "tf"; break;
      case 4: v = "i.spec"; break;
      case 5: v = "temp"; break;
      default: FatalF("d3d9: fixed-function pixel stage argument %X with a vertex shader", a);
    }
    if (a & 0x10) v = "(1.0 - " + v + ")";              // COMPLEMENT
    if ((a & 0x20) && alpha_replicate_ok) v = v + ".aaaa";  // ALPHAREPLICATE
    return v;
  };
  // The operation on float4 arguments a1, a2, a0 (tex and cur in scope).
  auto op = [&](uint32_t o, const std::string& a1, const std::string& a2, const std::string& a0) {
    switch (o) {
      case 2: return a1;
      case 3: return a2;
      case 4: return a1 + " * " + a2;
      case 5: return a1 + " * " + a2 + " * 2.0";
      case 6: return a1 + " * " + a2 + " * 4.0";
      case 7: return a1 + " + " + a2;
      case 8: return a1 + " + " + a2 + " - 0.5";
      case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2.0";
      case 10: return a1 + " - " + a2;
      case 11: return a1 + " + " + a2 + " - " + a1 + " * " + a2;
      case 12: return "lerp(" + a2 + ", " + a1 + ", i.diff.a)";
      case 13: return "lerp(" + a2 + ", " + a1 + ", tex.a)";
      case 14: return "lerp(" + a2 + ", " + a1 + ", tf.a)";
      case 15: return a1 + " + " + a2 + " * (1.0 - tex.a)";
      case 16: return "lerp(" + a2 + ", " + a1 + ", cur.a)";
      case 18: return "(" + a1 + " + " + a1 + ".aaaa * " + a2 + ")";
      case 19: return "(" + a1 + " * " + a2 + " + " + a1 + ".aaaa)";
      case 20: return "((1.0 - " + a1 + ".aaaa) * " + a2 + " + " + a1 + ")";
      case 21: return "((1.0 - " + a1 + ") * " + a2 + " + " + a1 + ".aaaa)";
      case 24: return "dot((" + a1 + ".rgb - 0.5) * 2.0, (" + a2 + ".rgb - 0.5) * 2.0).xxxx";
      case 25: return a0 + " + " + a1 + " * " + a2;
      case 26: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";
      default:
        FatalF("d3d9: fixed-function pixel operation %u with a vertex shader is not supported", o);
    }
  };
  for (uint32_t s = 0; s < 8; s++) {
    const uint32_t* t = d.tss[s];
    if (t[1] == 1) break;  // COLOROP DISABLE: this and the later stages are off
    const Texture* tx = d.textures[s].obj;
    const uint32_t set = t[11] & 0xFFFF;
    if (tx) {
      const std::string n = std::to_string(s), tc = "i.t" + std::to_string(set);
      decl += std::string(tx->cube ? "samplerCUBE" : "sampler2D") + " s" + n + " : register(s" + n + ");\n";
      sets.insert(set);
      // TEXTURETRANSFORMFLAGS PROJECTED: the coordinates divided by their
      // last one (COUNTn: the n-th; no count, as from a vertex shader: w).
      const uint32_t ttff = t[24], count = ttff & 0xFF;
      std::string uv = tc + ".xy";
      if ((ttff & 0x100) && !tx->cube)
        uv = count == 2 ? "float2(" + tc + ".x / " + tc + ".y, 0.0)"
           : count == 3 ? tc + ".xy / " + tc + ".z"
                        : tc + ".xy / " + tc + ".w";
      body += "  tex = " + std::string(tx->cube ? "texCUBE(s" + n + ", " + tc + ".xyz)"
                                                : "tex2D(s" + n + ", " + uv + ")") + ";\n";
    } else {
      body += "  tex = float4(0.0, 0.0, 0.0, 1.0);\n";
    }
    std::string color = op(t[1], arg(t[2], true), arg(t[3], true), arg(t[26], true));
    // ALPHAOP DISABLE with colour on: the alpha passes through.
    std::string alpha = t[4] == 1 ? "cur" : op(t[4], arg(t[5], false), arg(t[6], false), arg(t[27], false));
    const std::string dst = t[28] == 5 ? "temp" : "cur";  // RESULTARG
    body += "  r = float4((" + color + ").rgb, (" + alpha + ").a);\n";
    body += "  " + dst + " = saturate(r);\n";
  }
  if (d.rs[29]) body += "  cur.rgb = saturate(cur.rgb + i.spec.rgb);\n";  // SPECULARENABLE
  for (uint32_t set : sets)
    in += "  float4 t" + std::to_string(set) + " : TEXCOORD" + std::to_string(set) + ";\n";
  in += "};\n";
  return decl + in + "float4 main(PSIN i) : COLOR {\n" + body + "  return cur;\n}\n";
}

void NoteFfPixelGaps(const Device& d) {
  static bool warned_fog = false;
  if (d.rs[28] && !warned_fog) {  // FOGENABLE
    warned_fog = true;
    WarnF("d3d9: fog with a vertex shader and the fixed-function pixel stage is not emulated\n");
  }
}

// The stand-in pixel shader for the current stages (compiled once per
// source, found by the state the source is made from) and its constants
// in `c`.
const Device::FfVertexShader& FfPixelShaderFor(Device& d, float (*c)[4]) {
  // What FfPixelSource reads: each stage's operations, arguments, result,
  // coordinate set and transform flags and its texture's kind, up to the
  // first disabled stage; SPECULARENABLE.
  std::array<uint32_t, 97> key{};
  for (uint32_t s = 0; s < 8; s++) {
    const uint32_t* t = d.tss[s];
    uint32_t* k = &key[12 * s];
    static const uint8_t kStates[11] = {1, 2, 3, 4, 5, 6, 11, 24, 26, 27, 28};
    for (int i = 0; i < 11; i++) k[i] = t[kStates[i]];
    const Texture* tx = d.textures[s].obj;
    k[11] = tx ? (tx->cube ? 2 : 1) : 0;
    if (t[1] == 1) break;
  }
  key[96] = d.rs[29];
  const Device::FfVertexShader*& found = d.ff_ps_by_state[key];
  if (!found) {
    std::string src = FfPixelSource(d);
    auto it = d.ff_ps.find(src);
    if (it == d.ff_ps.end()) {
      auto p = std::make_shared<hlsl::Program>();
      std::string err;
      if (!hlsl::Compile(src, {}, "main", "ps_2_0", 0, p.get(), &err))
        FatalF("d3d9: the fixed-function pixel shader does not translate: %s\n%s", err.c_str(),
               src.c_str());
      Device::FfVertexShader v;
      v.id = d.gfx->CreateShader(*p);
      v.program = std::move(p);
      it = d.ff_ps.emplace(src, std::move(v)).first;
    }
    found = &it->second;
  }
  NoteFfPixelGaps(d);
  // TEXTUREFACTOR (A8R8G8B8) as a colour.
  const uint32_t f = d.rs[60];
  c[0][0] = float((f >> 16) & 0xFF) / 255.0f;
  c[0][1] = float((f >> 8) & 0xFF) / 255.0f;
  c[0][2] = float(f & 0xFF) / 255.0f;
  c[0][3] = float(f >> 24) / 255.0f;
  return *found;
}

void DrawCounted(Device& d, gfx::ShaderDraw& sd, const char* what);

// A table of input layouts with as many entries as this: its entries left
// by programs that are gone are dropped (a game creating and releasing vertex
// shaders across levels would otherwise keep every program's entry, and the
// program, as long as the table). A miss is a cold path; a hit is a few
// comparisons either way.
constexpr size_t kPruneLayoutsAt = 256;

template <typename Map>
void PruneLayouts(Map& layouts) {
  for (auto it = layouts.begin(); it != layouts.end();) {
    if (it->second.program.expired()) it = layouts.erase(it);
    else ++it;
  }
}

// The vertex shader's inputs from the vertex declaration or the FVF
// (`ff`: for a stand-in shader), worked out on their first draw together.
const std::vector<gfx::VertexInput>& InputsFor(Device& d,
                                               const std::shared_ptr<const hlsl::Program>& prog,
                                               bool ff, const char* what) {
  InputLayout* l;
  std::vector<guest::D3DVERTEXELEMENT9> from_fvf;
  const std::vector<guest::D3DVERTEXELEMENT9>* elements;
  if (VertexDecl* decl = d.decl.obj) {
    if (decl->layouts.size() >= kPruneLayoutsAt) PruneLayouts(decl->layouts);
    auto [it, fresh] = decl->layouts.try_emplace(prog.get());
    l = &it->second;
    if (!fresh && l->Of(prog.get())) return l->inputs;
    elements = &decl->elements;
  } else if (d.fvf) {
    if (d.fvf_layouts.size() >= kPruneLayoutsAt) PruneLayouts(d.fvf_layouts);
    auto [it, fresh] = d.fvf_layouts.try_emplace({d.fvf | uint64_t(ff) << 32, prog.get()});
    l = &it->second;
    if (!fresh && l->Of(prog.get())) return l->inputs;
    from_fvf = FvfElements(d.fvf);
    // The stand-in shader reads a pretransformed position as POSITION.
    if (ff)
      for (auto& e : from_fvf)
        if (e.Usage == USAGE_POSITIONT) e.Usage = USAGE_POSITION;
    elements = &from_fvf;
  } else {
    FatalF("d3d9: %s without a vertex declaration or FVF", what);
  }
  l->program = prog;
  const auto& sem = prog->inputs;
  l->inputs.resize(sem.size());
  for (size_t i = 0; i < sem.size(); i++) {
    for (const auto& e : *elements) {
      if (e.Stream == 0xFF) break;
      if (e.Usage == sem[i].usage && e.UsageIndex == sem[i].index) {
        l->inputs[i] = {e.Stream, e.Offset, e.Type};
        break;
      }
    }
  }
  return l->inputs;
}

// Completes `sd` (primitive, counts, and any streams already given) from
// the device's bindings and draws it (timed for <ID>_PERF).
void Draw(Device& d, gfx::ShaderDraw& sd, const char* what) {
  if (!perf::On()) return DrawCounted(d, sd, what);
  const uint64_t t0 = platform::MonotonicNs();
  DrawCounted(d, sd, what);
  g_perf.draws.fetch_add(1, std::memory_order_relaxed);
  g_perf.draw_ns.fetch_add(platform::MonotonicNs() - t0, std::memory_order_relaxed);
}

void DrawCounted(Device& d, gfx::ShaderDraw& sd, const char* what) {
  static const bool check = [] {
    const char* e = Env("D3D9_CHECK_BINDINGS");
    return e && *e == '1';
  }();
  if (check) CheckBindings(d);
  Shader* vs = d.vs.obj;
  Shader* ps = d.ps.obj;
  // Without the backend's fixed-function pipeline, stand-in shaders for
  // both stages.
  if (!vs && !ps && d.gfx->HasFixedFunction()) {
    DrawFixed(d, sd, what);
    return;
  }
  // The fixed-function pixel stage after a vertex shader: a pixel shader in
  // its place.
  const Device::FfVertexShader* ffp = nullptr;
  static float ffp_consts[224][4];
  if (!ps) {
    ffp = &FfPixelShaderFor(d, ffp_consts);
    if (perf::On()) g_perf.standin_ps.fetch_add(1, std::memory_order_relaxed);
  }
  // A pixel shader after the fixed-function vertex stage: a vertex shader
  // in its place (FVF draws only).
  const Device::FfVertexShader* ff = nullptr;
  static float ff_consts[256][4];
  if (!vs) {
    if (d.decl.obj || !d.fvf)
      FatalF("d3d9: %s with a pixel shader, no vertex shader and a vertex declaration", what);
    ff = &FfVertexShaderFor(d, ff_consts);
    if (perf::On()) g_perf.standin_vs.fetch_add(1, std::memory_order_relaxed);
  }
  const std::vector<gfx::VertexInput>& inputs =
      InputsFor(d, ff ? ff->program : vs->program, ff != nullptr, what);
  sd.inputs = inputs.data();
  sd.input_count = uint32_t(inputs.size());

  // Instancing (SetStreamSourceFreq, indexed draws only): stream 0 has
  // INDEXEDDATA | the number of instances, the others INSTANCEDATA | how
  // many instances share an element. Drawn once per instance, those streams
  // at the instance's element with a stride of 0 (every vertex reads it).
  constexpr uint32_t kIndexedData = 0x40000000, kInstanceData = 0x80000000;
  const uint32_t freq0 = d.streams[0].freq;
  const bool instanced = sd.indices && !sd.streams[0].data && (freq0 & kIndexedData);
  const uint32_t instances = instanced ? std::max<uint32_t>(freq0 & 0x3FFFFFFF, 1) : 1;
  gfx::VertexStream per_instance[16];
  for (int s = 0; s < 16; s++) {
    const Device::Stream& st = d.streams[s];
    if (st.freq != 1 && st.buffer.guest && !(st.freq & (kIndexedData | kInstanceData)))
      FatalF("d3d9: %s with stream %d frequency %08X (not supported)", what, s, st.freq);
    if (sd.streams[s].data) continue;  // given by the caller (UP draws)
    Buffer* b = st.buffer.obj;
    if (!b || st.offset >= b->length) continue;
    sd.streams[s] = {static_cast<const uint8_t*>(GPtr(b->mem)) + st.offset, b->length - st.offset,
                     st.stride};
    NoteBufferUse(d, b);
    if (instanced && (st.freq & kInstanceData)) {
      per_instance[s] = sd.streams[s];
      sd.streams[s].stride = 0;
    }
  }

  sd.vs = ff ? ff->id : vs->id;
  sd.ps = ffp ? ffp->id : ps->id;
  sd.vs_float = ff ? ff_consts : d.vs_f;
  sd.vs_int = d.vs_i;
  sd.vs_bool = reinterpret_cast<const uint32_t*>(d.vs_b);
  sd.ps_float = ffp ? ffp_consts : d.ps_f;
  sd.ps_int = d.ps_i;
  sd.ps_bool = reinterpret_cast<const uint32_t*>(d.ps_b);
  sd.vs_float_version = ff ? 0 : d.vs_f_version;  // stand-ins' constants change per draw
  sd.ps_float_version = ffp ? 0 : d.ps_f_version;
  sd.vs_ib_version = d.vs_ib_version;
  sd.ps_ib_version = d.ps_ib_version;
  for (int i = 0; i < 20; i++) {
    const Texture* t = d.textures[i].obj;
    sd.textures[i] = t ? t->tex : 0;
  }
  sd.sampler_states = d.ss;
  sd.sampler_versions = d.ss_version;
  // The changed state goes with the draw (one call into gfx).
  StateChanges changes;
  TakeChanges(d, changes);
  sd.states = changes.states;
  sd.state_count = changes.count;
  sd.viewport = changes.has_viewport ? &changes.viewport : nullptr;
  sd.scissor = changes.has_scissor ? &changes.scissor : nullptr;
  d.gfx->DrawShader(sd);
  sd.states = nullptr;  // (applied by the first instance)
  sd.state_count = 0;
  sd.viewport = nullptr;
  sd.scissor = nullptr;
  for (uint32_t i = 1; i < instances; i++) {
    for (int s = 0; s < 16; s++) {
      const gfx::VertexStream& first = per_instance[s];
      if (!first.data) continue;
      const uint32_t at = i / std::max<uint32_t>(d.streams[s].freq & 0x3FFFFFFF, 1) * first.stride;
      if (at >= first.size) continue;  // past the buffer: the last one that fits
      sd.streams[s] = {first.data + at, first.size - at, 0};
    }
    d.gfx->DrawShader(sd);
  }
}

}  // namespace
}  // namespace recomp::d3d9

namespace recomp::hle {
namespace {

using namespace d3d9;

Device* Dev(Ctx& c) { return DeviceOf(Arg(c, 0)); }

// (PrimitiveType, StartVertex, PrimitiveCount)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawPrimitive", D3DDev9_DrawPrimitive, 4) {
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.primitives = Arg(c, 3);
  sd.base_vertex = int32_t(Arg(c, 2));
  sd.vertex_count = VerticesFor(sd.prim, sd.primitives);
  if (!sd.vertex_count) return D3DERR_INVALIDCALL;
  Draw(*Dev(c), sd, "DrawPrimitive");
  return D3D_OK;
}

// (Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawIndexedPrimitive", D3DDev9_DrawIndexedPrimitive, 7) {
  Device* d = Dev(c);
  Buffer* ib = d->indices.obj;
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.primitives = Arg(c, 6);
  uint32_t n = VerticesFor(sd.prim, sd.primitives);
  if (!ib || !n) return D3DERR_INVALIDCALL;
  sd.index_size = ib->fvf_or_format == D3DFMT_INDEX32 ? 4 : 2;
  uint64_t start = uint64_t(Arg(c, 5)) * sd.index_size;
  if (start + uint64_t(n) * sd.index_size > ib->length) return D3DERR_INVALIDCALL;
  sd.indices = static_cast<const uint8_t*>(GPtr(ib->mem)) + start;
  NoteBufferUse(*d, ib);
  sd.base_vertex = int32_t(Arg(c, 2));
  sd.min_index = Arg(c, 3);
  sd.vertex_count = Arg(c, 4);
  Draw(*d, sd, "DrawIndexedPrimitive");
  return D3D_OK;
}

// (PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride).
// Stream 0 is unbound afterwards, as in D3D9.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawPrimitiveUP", D3DDev9_DrawPrimitiveUP, 5) {
  Device* d = Dev(c);
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.primitives = Arg(c, 2);
  uint32_t stride = Arg(c, 4);
  sd.vertex_count = VerticesFor(sd.prim, sd.primitives);
  uint32_t bytes = sd.vertex_count * stride;
  if (!sd.vertex_count || !EnsureReadable(c, Arg(c, 3), bytes)) return D3DERR_INVALIDCALL;
  sd.streams[0] = {static_cast<const uint8_t*>(GPtr(Arg(c, 3))), bytes, stride};
  Draw(*d, sd, "DrawPrimitiveUP");
  Bind(d->streams[0].buffer, 0);
  d->streams[0].offset = d->streams[0].stride = 0;
  return D3D_OK;
}

// (PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData,
// IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride). Stream 0
// and the indices are unbound afterwards, as in D3D9.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawIndexedPrimitiveUP", D3DDev9_DrawIndexedPrimitiveUP, 9) {
  Device* d = Dev(c);
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.min_index = Arg(c, 2);
  sd.vertex_count = Arg(c, 3);
  sd.primitives = Arg(c, 4);
  sd.index_size = Arg(c, 6) == D3DFMT_INDEX32 ? 4 : 2;
  uint32_t n = VerticesFor(sd.prim, sd.primitives), stride = Arg(c, 8);
  uint32_t vbytes = (sd.min_index + sd.vertex_count) * stride;
  if (!n || !EnsureReadable(c, Arg(c, 5), n * sd.index_size) ||
      !EnsureReadable(c, Arg(c, 7), vbytes))
    return D3DERR_INVALIDCALL;
  sd.indices = GPtr(Arg(c, 5));
  sd.streams[0] = {static_cast<const uint8_t*>(GPtr(Arg(c, 7))), vbytes, stride};
  Draw(*d, sd, "DrawIndexedPrimitiveUP");
  Bind(d->streams[0].buffer, 0);
  d->streams[0].offset = d->streams[0].stride = 0;
  Bind(d->indices, 0);
  return D3D_OK;
}

// The gfx colour target behind a surface for StretchRect (tex 0: the back
// buffer); fatal for the kinds it does not support.
gfx::ColorTarget StretchTarget(Surface* s, const char* which) {
  switch (s->kind) {
    case Surface::Kind::BackBuffer:
      return {};
    case Surface::Kind::RenderTarget:
      return {s->tex, 0};
    case Surface::Kind::TextureLevel:
      if (!s->texture->cube && (s->texture->usage & D3DUSAGE_RENDERTARGET))
        return {s->texture->tex, int(s->level)};
      break;
    default:
      break;
  }
  FatalF("d3d9: StretchRect %s is not a colour target (kind %d, level %d)", which, int(s->kind),
         s->level);
}

gfx::Rect SurfaceRect(Surface* s, uint32_t rect) {
  if (!rect) return {0, 0, int32_t(s->desc.Width), int32_t(s->desc.Height)};
  const auto* r = G<guest::RECT>(rect);
  return {r->left, r->top, r->right, r->bottom};
}

// (pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::StretchRect", D3DDev9_StretchRect, 6) {
  Device* d = Dev(c);
  auto* src = com::As<Surface>(Arg(c, 1));
  auto* dst = com::As<Surface>(Arg(c, 3));
  if (!src || !dst || src == dst) return D3DERR_INVALIDCALL;
  gfx::ColorTarget s = StretchTarget(src, "source"), t = StretchTarget(dst, "destination");
  // D3DTEXF_LINEAR (2) filters; NONE and POINT take the nearest texel.
  d->gfx->StretchRect(s, SurfaceRect(src, Arg(c, 2)), t, SurfaceRect(dst, Arg(c, 4)),
                      Arg(c, 5) >= 2);
  return D3D_OK;
}

}  // namespace
}  // namespace recomp::hle
