// Direct3D 11's shader programs (d3d11_impl.h, "shader programs"): the
// programs the translator links, kept per combination of shaders and
// input layout, and the vertex fetch the layer builds from an input
// layout for the linker.

#include <algorithm>
#include <cctype>
#include <string>

#include "runtime/log.h"
#include "win32/d3d11_impl.h"

namespace recomp::hle::d3d11 {

std::shared_ptr<const LinkedProgram> ProgramFor(Device& dev, const Shader& vs, const Shader* ps,
                                                const InputLayout* layout) {
  const auto key = std::make_tuple(vs.serial, ps ? ps->serial : 0, layout ? layout->serial : 0);
  auto it = dev.programs.find(key);
  if (it != dev.programs.end()) return it->second;
  auto p = LinkProgram(*dev.gfx, vs, ps, layout);
  dev.programs.emplace(key, p);
  return p;
}

std::shared_ptr<const LinkedProgram> ComputeProgramFor(Device& dev, const Shader& cs) {
  const auto key = std::make_tuple(cs.serial, uint64_t(0), uint64_t(0));
  auto it = dev.programs.find(key);
  if (it != dev.programs.end()) return it->second;
  auto p = LinkComputeProgram(*dev.gfx, cs);
  dev.programs.emplace(key, p);
  return p;
}

namespace {

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
    case 6: *words = 3; return "uvec4(w0, w1, w2, R_ONE)";
    case 7: case 8: *words = 3; return "uvec4(w0, w1, w2, 1u)";
    case 16: *words = 2; return "uvec4(w0, w1, 0u, R_ONE)";
    case 17: case 18: *words = 2; return "uvec4(w0, w1, 0u, 1u)";
    case 41: return "uvec4(w0, 0u, 0u, R_ONE)";
    case 42: case 43: return "uvec4(w0, 0u, 0u, 1u)";
    case 10: *words = 2; return "floatBitsToUint(vec4(unpackHalf2x16(w0), unpackHalf2x16(w1)))";
    case 11: *words = 2; return "floatBitsToUint(vec4(unpackUnorm2x16(w0), unpackUnorm2x16(w1)))";
    case 12: *words = 2; return "uvec4(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu, w1 >> 16u)";
    case 13: *words = 2; return "floatBitsToUint(vec4(unpackSnorm2x16(w0), unpackSnorm2x16(w1)))";
    case 14: *words = 2; return "uvec4(ivec4(r_lo16(w0), r_hi16(w0), r_lo16(w1), r_hi16(w1)))";
    case 34: return "floatBitsToUint(vec4(unpackHalf2x16(w0), 0.0, 1.0))";
    case 35: return "floatBitsToUint(vec4(unpackUnorm2x16(w0), 0.0, 1.0))";
    case 36: return "uvec4(w0 & 0xFFFFu, w0 >> 16u, 0u, 1u)";
    case 37: return "floatBitsToUint(vec4(unpackSnorm2x16(w0), 0.0, 1.0))";
    case 38: return "uvec4(ivec4(r_lo16(w0), r_hi16(w0), 0, 1))";
    case 54: return "floatBitsToUint(vec4(unpackHalf2x16(w0).x, 0.0, 0.0, 1.0))";
    case 56: return "floatBitsToUint(vec4(unpackUnorm2x16(w0).x, 0.0, 0.0, 1.0))";
    case 57: return "uvec4(w0 & 0xFFFFu, 0u, 0u, 1u)";
    case 58: return "floatBitsToUint(vec4(unpackSnorm2x16(w0).x, 0.0, 0.0, 1.0))";
    case 59: return "uvec4(ivec4(r_lo16(w0), 0, 0, 1))";
    case 28: case 29: return "floatBitsToUint(unpackUnorm4x8(w0))";
    case 30: return "uvec4(w0 & 0xFFu, (w0 >> 8u) & 0xFFu, (w0 >> 16u) & 0xFFu, w0 >> 24u)";
    case 31: return "floatBitsToUint(unpackSnorm4x8(w0))";
    case 32: return "uvec4(ivec4(r_s8(w0, 0u), r_s8(w0, 1u), r_s8(w0, 2u), r_s8(w0, 3u)))";
    case 49: return "floatBitsToUint(vec4(unpackUnorm4x8(w0).xy, 0.0, 1.0))";
    case 50: return "uvec4(w0 & 0xFFu, (w0 >> 8u) & 0xFFu, 0u, 1u)";
    case 51: return "floatBitsToUint(vec4(unpackSnorm4x8(w0).xy, 0.0, 1.0))";
    case 52: return "uvec4(ivec4(r_s8(w0, 0u), r_s8(w0, 1u), 0, 1))";
    case 61: return "floatBitsToUint(vec4(unpackUnorm4x8(w0).x, 0.0, 0.0, 1.0))";
    case 62: return "uvec4(w0 & 0xFFu, 0u, 0u, 1u)";
    case 63: return "floatBitsToUint(vec4(unpackSnorm4x8(w0).x, 0.0, 0.0, 1.0))";
    case 64: return "uvec4(ivec4(r_s8(w0, 0u), 0, 0, 1))";
    case 87: return "floatBitsToUint(unpackUnorm4x8(w0).zyxw)";
    case 88: return "floatBitsToUint(vec4(unpackUnorm4x8(w0).zyx, 1.0))";
    case 24:
      return "floatBitsToUint(vec4(uvec3(w0, w0 >> 10u, w0 >> 20u) & 0x3FFu, w0 >> 30u) / "
             "vec4(1023.0, 1023.0, 1023.0, 3.0))";
    case 25: return "uvec4(w0 & 0x3FFu, (w0 >> 10u) & 0x3FFu, (w0 >> 20u) & 0x3FFu, w0 >> 30u)";
    case 26:
      return "floatBitsToUint(vec4(r_f11(w0 & 0x7FFu), r_f11((w0 >> 11u) & 0x7FFu), "
             "r_f10(w0 >> 22u), 1.0))";
    default: return nullptr;
  }
}

constexpr const char* kHelpers = R"(const uint R_ONE = 0x3F800000u;
// 32 bits of read-only buffer b from byte `at`, which need not be aligned.
uint r_vb(uint b, uint at) {
  uint w = at >> 2u, s = (at & 3u) * 8u;
  uint lo = r_ld(b, w);
  return s == 0u ? lo : (lo >> s) | (r_ld(b, w + 1u) << (32u - s));
}
int r_lo16(uint w) { return int(w << 16) >> 16; }
int r_hi16(uint w) { return int(w) >> 16; }
int r_s8(uint w, uint i) { return int(w << (24u - 8u * i)) >> 24; }
// The unsigned 11- and 10-bit floats of R11G11B10_FLOAT, as halves.
float r_f11(uint v) { return unpackHalf2x16(((v >> 6u) << 10u) | ((v & 0x3Fu) << 4u)).x; }
float r_f10(uint v) { return unpackHalf2x16(((v >> 5u) << 10u) | ((v & 0x1Fu) << 5u)).x; }
)";

}  // namespace

std::string VertexFetch(const InputLayout* layout, const std::vector<VertexInput>& inputs,
                        uint32_t first_buffer, std::vector<uint32_t>* slots,
                        uint32_t* uniform_bytes) {
  slots->clear();
  struct Use {
    const InputLayout::Element* e;
    uint32_t reg;
  };
  std::vector<Use> uses;
  for (const VertexInput& in : inputs) {
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
  if (slots->size() > 32) FatalF("d3d11: an input layout reading more than 32 vertex buffers");
  const size_t n = slots->size();
  *uniform_bytes = uint32_t(16 + 16 * n);
  std::string g = "layout(std140, R_UNIFORMS) uniform RVertex {\n  ivec4 r_draw;\n";
  if (n) g += "  uvec4 r_ia[" + std::to_string(n) + "];\n";
  g += "};\n";
  g += "#define R_VERTEX_ID (uint(gl_VertexIndex) - uint(r_draw.x))\n";
  g += "#define R_INSTANCE_ID uint(gl_InstanceIndex)\n";
  g += kHelpers;
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
      index = "uint(gl_VertexIndex) - r_ia[" + ks + "].y";
    else if (u.e->step_rate)
      index = "uint(gl_InstanceIndex) / " + std::to_string(u.e->step_rate) +
              "u + uint(r_draw.y) - r_ia[" + ks + "].y";
    else
      index = "uint(r_draw.y) - r_ia[" + ks + "].y";
    g += "    case " + std::to_string(u.reg) + "u: {\n";
    g += "      uint a = (" + index + ") * r_ia[" + ks + "].x + " + std::to_string(u.e->offset) +
         "u;\n";
    for (int w = 0; w < words; w++)
      g += "      uint w" + std::to_string(w) + " = r_vb(" + b + ", a + " + std::to_string(4 * w) +
           "u);\n";
    g += std::string("      return ") + decode + ";\n    }\n";
  }
  g += "  }\n  return uvec4(0u);\n}\n";
  return g;
}

}  // namespace recomp::hle::d3d11
