// Direct3D 11's programs from translated shaders (d3d11_impl.h, "shader
// programs"; dxbc.h for the translation's conventions). Each DXBC is
// translated once (by its hash and size); a program puts link-time code
// before each stage's translation (the device's prelude comes first):
// the varyings' locations for the pair of stages, the layer's uniform
// block with the typed buffer views' formats, and in the vertex stage the
// input fetch built from the input layout. A shader that is not
// translated (a stage, instruction or declaration the translator lacks)
// gets no program, so the draws with it are skipped.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "runtime/log.h"
#include "win32/d3d11_impl.h"
#include "win32/dxbc.h"

namespace recomp::hle::d3d11 {

namespace {

// ---- translations -------------------------------------------------------------------

struct Translation {
  dxbc::Shader s;
  bool ok = false;
};

const char* StageName(uint32_t stage) {
  static const char* const kNames[] = {"vertex", "pixel", "compute", "hull", "domain", "geometry"};
  return stage < 6 ? kNames[stage] : "unknown";
}

// The shader's translation, made the first time any object with its
// bytes is drawn with. A failure is reported once, by the DXBC's hash.
std::shared_ptr<const Translation> Translated(const Shader& sh) {
  static std::mutex mu;
  static std::map<std::pair<uint64_t, size_t>, std::shared_ptr<const Translation>> cache;
  std::lock_guard<std::mutex> l(mu);
  const auto key = std::make_pair(sh.hash, sh.dxbc.size());
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  auto t = std::make_shared<Translation>();
  t->ok = dxbc::Translate(sh.dxbc.data(), sh.dxbc.size(), &t->s);
  if (!t->ok)
    WarnF("d3d11: %s shader %016llX (%zu bytes) not translated, its draws are skipped: %s\n",
          StageName(sh.stage), (unsigned long long)sh.hash, sh.dxbc.size(), t->s.error.c_str());
  cache.emplace(key, t);
  return t;
}

// ---- bindings ---------------------------------------------------------------------------

// What the translation's buffers, textures, UAVs and typed views are
// among Direct3D's bindings (dxbc.h: constant buffers by slot, then the
// buffer SRVs, numbered by ShaderResource::buffer).
StageBindings BindingsOf(const dxbc::Shader& s) {
  StageBindings b;
  for (const dxbc::ConstantBuffer& cb : s.cbuffers)
    b.buffers.push_back({StageBindings::BufferKind::Constant, cb.slot, cb.size * 16});
  b.buffers.resize(s.stage.buffers);
  for (const dxbc::ShaderResource& r : s.resources) {
    if (r.buffer >= 0) b.buffers[size_t(r.buffer)] = {StageBindings::BufferKind::View, r.slot, 0};
    if (r.dim == dxbc::ResourceDim::Buffer) b.formats.push_back({false, r.slot});
  }
  for (const dxbc::TextureBinding& t : s.textures) b.textures.push_back({t.resource, t.sampler});
  b.rw_buffers.resize(s.stage.rw_buffers);
  b.rw_images.resize(s.stage.rw_images);
  for (const dxbc::UnorderedAccess& u : s.uavs) {
    if (u.buffer >= 0) b.rw_buffers[size_t(u.buffer)] = u.slot;
    if (u.image >= 0) b.rw_images[size_t(u.image)] = u.slot;
    if (u.dim == dxbc::ResourceDim::Buffer) b.formats.push_back({true, u.slot});
  }
  return b;
}

// ---- link-time code -------------------------------------------------------------------

// The layer's uniform block (StageUniforms) with `ia` vertex buffers,
// and R_FMT_T<slot> / R_FMT_U<slot> reading the formats.
std::string UniformBlock(const StageBindings& b, uint32_t ia, uint32_t* bytes) {
  std::string g = "layout(std140, R_UNIFORMS) uniform LStage {\n  ivec4 l_draw;\n  uvec4 l_fmt[4];\n";
  if (ia) g += "  uvec4 l_ia[" + std::to_string(ia) + "];\n";
  g += "};\n";
  for (size_t i = 0; i < b.formats.size(); i++) {
    char d[96];
    std::snprintf(d, sizeof d, "#define R_FMT_%c%u l_fmt[%zu].%c\n", b.formats[i].uav ? 'U' : 'T',
                  b.formats[i].slot, i / 4, "xyzw"[i % 4]);
    g += d;
  }
  *bytes = 16 + 4 * kMaxTypedViews + 16 * ia;
  return g;
}

bool SameSemantic(const std::string& a, const std::string& b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::toupper(static_cast<unsigned char>(x)) ==
                  std::toupper(static_cast<unsigned char>(y));
         });
}

// The GLSL turning words w0..w3 (the element's bytes from its offset) of
// a DXGI_FORMAT into the input register's bits; null for a format the
// input assembler does not read. `words` is how many it reads.
const char* Decode(uint32_t f, int* words) {
  *words = 1;
  switch (f) {
    case 2: case 3: case 4: *words = 4; return "uvec4(w0, w1, w2, w3)";
    case 6: *words = 3; return "uvec4(w0, w1, w2, L_ONE)";
    case 7: case 8: *words = 3; return "uvec4(w0, w1, w2, 1u)";
    case 16: *words = 2; return "uvec4(w0, w1, 0u, L_ONE)";
    case 17: case 18: *words = 2; return "uvec4(w0, w1, 0u, 1u)";
    case 41: return "uvec4(w0, 0u, 0u, L_ONE)";
    case 42: case 43: return "uvec4(w0, 0u, 0u, 1u)";
    case 10: *words = 2; return "floatBitsToUint(vec4(unpackHalf2x16(w0), unpackHalf2x16(w1)))";
    case 11: *words = 2; return "floatBitsToUint(vec4(unpackUnorm2x16(w0), unpackUnorm2x16(w1)))";
    case 12: *words = 2; return "uvec4(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu, w1 >> 16u)";
    case 13: *words = 2; return "floatBitsToUint(vec4(unpackSnorm2x16(w0), unpackSnorm2x16(w1)))";
    case 14: *words = 2; return "uvec4(ivec4(l_lo16(w0), l_hi16(w0), l_lo16(w1), l_hi16(w1)))";
    case 34: return "floatBitsToUint(vec4(unpackHalf2x16(w0), 0.0, 1.0))";
    case 35: return "floatBitsToUint(vec4(unpackUnorm2x16(w0), 0.0, 1.0))";
    case 36: return "uvec4(w0 & 0xFFFFu, w0 >> 16u, 0u, 1u)";
    case 37: return "floatBitsToUint(vec4(unpackSnorm2x16(w0), 0.0, 1.0))";
    case 38: return "uvec4(ivec4(l_lo16(w0), l_hi16(w0), 0, 1))";
    case 54: return "floatBitsToUint(vec4(unpackHalf2x16(w0).x, 0.0, 0.0, 1.0))";
    case 56: return "floatBitsToUint(vec4(unpackUnorm2x16(w0).x, 0.0, 0.0, 1.0))";
    case 57: return "uvec4(w0 & 0xFFFFu, 0u, 0u, 1u)";
    case 58: return "floatBitsToUint(vec4(unpackSnorm2x16(w0).x, 0.0, 0.0, 1.0))";
    case 59: return "uvec4(ivec4(l_lo16(w0), 0, 0, 1))";
    case 28: case 29: return "floatBitsToUint(unpackUnorm4x8(w0))";
    case 30: return "uvec4(w0 & 0xFFu, (w0 >> 8u) & 0xFFu, (w0 >> 16u) & 0xFFu, w0 >> 24u)";
    case 31: return "floatBitsToUint(unpackSnorm4x8(w0))";
    case 32: return "uvec4(ivec4(l_s8(w0, 0u), l_s8(w0, 1u), l_s8(w0, 2u), l_s8(w0, 3u)))";
    case 49: return "floatBitsToUint(vec4(unpackUnorm4x8(w0).xy, 0.0, 1.0))";
    case 50: return "uvec4(w0 & 0xFFu, (w0 >> 8u) & 0xFFu, 0u, 1u)";
    case 51: return "floatBitsToUint(vec4(unpackSnorm4x8(w0).xy, 0.0, 1.0))";
    case 52: return "uvec4(ivec4(l_s8(w0, 0u), l_s8(w0, 1u), 0, 1))";
    case 61: return "floatBitsToUint(vec4(unpackUnorm4x8(w0).x, 0.0, 0.0, 1.0))";
    case 62: return "uvec4(w0 & 0xFFu, 0u, 0u, 1u)";
    case 63: return "floatBitsToUint(vec4(unpackSnorm4x8(w0).x, 0.0, 0.0, 1.0))";
    case 64: return "uvec4(ivec4(l_s8(w0, 0u), 0, 0, 1))";
    case 87: return "floatBitsToUint(unpackUnorm4x8(w0).zyxw)";
    case 88: return "floatBitsToUint(vec4(unpackUnorm4x8(w0).zyx, 1.0))";
    case 24:
      return "floatBitsToUint(vec4(uvec3(w0, w0 >> 10u, w0 >> 20u) & 0x3FFu, w0 >> 30u) / "
             "vec4(1023.0, 1023.0, 1023.0, 3.0))";
    case 25: return "uvec4(w0 & 0x3FFu, (w0 >> 10u) & 0x3FFu, (w0 >> 20u) & 0x3FFu, w0 >> 30u)";
    case 26:
      return "floatBitsToUint(vec4(l_f11(w0 & 0x7FFu), l_f11((w0 >> 11u) & 0x7FFu), "
             "l_f10(w0 >> 22u), 1.0))";
    default: return nullptr;
  }
}

// SV_VertexID and SV_InstanceID (StageUniforms::draw).
constexpr const char* kVertexIds = R"(#define R_VERTEX_ID (uint(gl_VertexIndex) - uint(l_draw.x))
#define R_INSTANCE_ID uint(gl_InstanceIndex)
)";

// What reading the vertex buffers takes (r_ld is the prelude's, there
// only when the stage has read-only buffers).
constexpr const char* kFetchHelpers = R"(const uint L_ONE = 0x3F800000u;
// 32 bits of read-only buffer b from byte `at`, which need not be aligned.
uint l_vb(uint b, uint at) {
  uint w = at >> 2u, s = (at & 3u) * 8u;
  uint lo = r_ld(b, w);
  return s == 0u ? lo : (lo >> s) | (r_ld(b, w + 1u) << (32u - s));
}
int l_lo16(uint w) { return int(w << 16) >> 16; }
int l_hi16(uint w) { return int(w) >> 16; }
int l_s8(uint w, uint i) { return int(w << (24u - 8u * i)) >> 24; }
// The unsigned 11- and 10-bit floats of R11G11B10_FLOAT, as halves.
float l_f11(uint v) { return unpackHalf2x16(((v >> 6u) << 10u) | ((v & 0x3Fu) << 4u)).x; }
float l_f10(uint v) { return unpackHalf2x16(((v >> 5u) << 10u) | ((v & 0x1Fu) << 5u)).x; }
)";

// The vertex stage's r_fetch over the input layout: each input register
// the shader declares (its input signature's elements that are not
// system values) from the layout's element of the same semantic and
// index, read from the vertex buffers as read-only buffers first_buffer
// on, one per input slot listed in *slots. Registers the layout does not
// fill read 0, as do formats the input assembler does not read.
std::string VertexFetch(const InputLayout* layout, const dxbc::Shader& vs, uint32_t first_buffer,
                        std::vector<uint32_t>* slots) {
  slots->clear();
  struct Use {
    const InputLayout::Element* e;
    uint32_t reg;
  };
  std::vector<Use> uses;
  for (const dxbc::SignatureElement& in : vs.inputs) {
    if (in.system_value) continue;
    const InputLayout::Element* e = nullptr;
    if (layout)
      for (const InputLayout::Element& el : layout->elements)
        if (el.index == in.index && SameSemantic(el.semantic, in.semantic)) {
          e = &el;
          break;
        }
    if (!e) WarnF("d3d11: the input layout has no %s%u: it reads 0\n", in.semantic.c_str(), in.index);
    uses.push_back({e, in.reg});
    if (e && std::find(slots->begin(), slots->end(), e->slot) == slots->end())
      slots->push_back(e->slot);
  }
  std::string g = kVertexIds;
  if (!slots->empty()) g += kFetchHelpers;
  g += "uvec4 r_fetch(uint reg) {\n  switch (reg) {\n";
  for (const Use& u : uses) {
    if (!u.e) continue;
    int words;
    const char* decode = Decode(u.e->format, &words);
    if (!decode) {
      WarnF("d3d11: vertex format %u (%s%u) is not read: it reads 0\n", u.e->format,
            u.e->semantic.c_str(), u.e->index);
      continue;
    }
    const size_t k = size_t(std::find(slots->begin(), slots->end(), u.e->slot) - slots->begin());
    const std::string ks = std::to_string(k), b = std::to_string(first_buffer + k) + "u";
    std::string index;
    if (!u.e->per_instance)
      index = "uint(gl_VertexIndex) - l_ia[" + ks + "].y";
    else if (u.e->step_rate)
      index = "uint(gl_InstanceIndex) / " + std::to_string(u.e->step_rate) +
              "u + uint(l_draw.y) - l_ia[" + ks + "].y";
    else
      index = "uint(l_draw.y) - l_ia[" + ks + "].y";
    g += "    case " + std::to_string(u.reg) + "u: {\n";
    g += "      uint a = (" + index + ") * l_ia[" + ks + "].x + " + std::to_string(u.e->offset) +
         "u;\n";
    for (int w = 0; w < words; w++)
      g += "      uint w" + std::to_string(w) + " = l_vb(" + b + ", a + " + std::to_string(4 * w) +
           "u);\n";
    g += std::string("      return ") + decode + ";\n    }\n";
  }
  g += "  }\n  return uvec4(0u);\n}\n";
  return g;
}

// Whether the device can take a stage this large; if not, says why.
bool Fits(const Shader& sh, const dxbc::Shader& s, const StageBindings& b) {
  const char* why = s.stage.textures > 16      ? "more than 16 textures"
                    : s.stage.rw_images > 8    ? "more than 8 storage images"
                    : b.formats.size() > kMaxTypedViews ? "more than 16 typed buffer views"
                                                        : nullptr;
  if (!why) return true;
  WarnF("d3d11: %s shader %016llX reads %s: its draws are skipped\n", StageName(sh.stage),
        (unsigned long long)sh.hash, why);
  return false;
}

}  // namespace

std::shared_ptr<const LinkedProgram> LinkProgram(gfx::Device& dev, const Shader& vs,
                                                 const Shader* ps, const InputLayout* layout) {
  if (vs.stage != 0 || (ps && ps->stage != 1)) return nullptr;
  const auto tv = Translated(vs);
  const auto tp = ps ? Translated(*ps) : nullptr;
  if (!tv->ok || (tp && !tp->ok)) return nullptr;
  auto p = std::make_shared<LinkedProgram>();
  p->vs = BindingsOf(tv->s);
  if (tp) p->ps = BindingsOf(tp->s);
  if (!Fits(vs, tv->s, p->vs) || (ps && !Fits(*ps, tp->s, p->ps))) return nullptr;

  const std::string varyings = dxbc::VaryingDefines(&tv->s, tp ? &tp->s : nullptr);
  gfx::ProgramDesc d;
  // Vertex: the uniform block, the fetch, the translation; the vertex
  // buffers after the translation's buffers.
  std::vector<uint32_t> slots;
  const std::string fetch = VertexFetch(layout, tv->s, tv->s.stage.buffers, &slots);
  for (uint32_t s : slots) p->vs.buffers.push_back({StageBindings::BufferKind::Vertex, s, 0});
  d.vs = tv->s.stage;
  d.vs.buffers = uint32_t(p->vs.buffers.size());
  d.vs.glsl = varyings + UniformBlock(p->vs, uint32_t(slots.size()), &d.vs.uniform_bytes) + fetch +
              tv->s.stage.glsl;
  p->vs.uniforms = true;
  // Pixel: the translation, or none (nothing written but depth).
  if (tp) {
    d.fs = tp->s.stage;
    std::string block;
    if (!p->ps.formats.empty()) {
      block = UniformBlock(p->ps, 0, &d.fs.uniform_bytes);
      p->ps.uniforms = true;
    }
    d.fs.glsl = varyings + block + tp->s.stage.glsl;
  } else {
    d.fs.glsl = varyings + "void main() {}\n";
  }
  p->id = dev.CreateProgram(d);
  RECOMP_TRACE("  d3d11: program %u: vertex %016llX, pixel %016llX\n", p->id,
               (unsigned long long)vs.hash, ps ? (unsigned long long)ps->hash : 0ull);
  return p;
}

std::shared_ptr<const LinkedProgram> LinkComputeProgram(gfx::Device& dev, const Shader& cs) {
  if (cs.stage != 2) return nullptr;
  const auto t = Translated(cs);
  if (!t->ok) return nullptr;
  auto p = std::make_shared<LinkedProgram>();
  p->cs = BindingsOf(t->s);
  if (!Fits(cs, t->s, p->cs)) return nullptr;
  gfx::ProgramDesc d;
  d.cs = t->s.stage;
  std::string block;
  if (!p->cs.formats.empty()) {
    block = UniformBlock(p->cs, 0, &d.cs.uniform_bytes);
    p->cs.uniforms = true;
  }
  d.cs.glsl = block + t->s.stage.glsl;
  for (int i = 0; i < 3; i++) d.local_size[i] = t->s.local_size[i];
  p->id = dev.CreateProgram(d);
  return p;
}

}  // namespace recomp::hle::d3d11
