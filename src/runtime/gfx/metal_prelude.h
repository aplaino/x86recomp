#pragma once
// The prelude the Metal backend puts before a translated shader's GLSL
// (runtime/hlsl/hlsl.h): its binding numbers, vertex fetch, position and
// pixel shader helpers. Shared with the offline shader checker
// (tools/shader_check), so it compiles exactly what the backend does.

#include <string>

#include "runtime/hlsl/hlsl.h"

namespace recomp::gfx {

inline std::string MetalShaderPrelude(hlsl::Stage stage) {
  std::string p;
  if (stage == hlsl::Stage::Vertex) {
    p += R"(#define R_FLOAT_BINDING set = 0, binding = 0
#define R_INT_BINDING set = 0, binding = 1
#define R_SAMPLER_BINDING(n) set = 0, binding = n
layout(std140, set = 0, binding = 2) uniform r_vs_state {
  ivec4 r_in[16];
  vec4 r_pixel;
  vec4 r_clip[6];      // the user clip planes, in clip space (D3D9's with shaders)
  ivec4 r_clip_mask;   // x: CLIPPLANEENABLE; y: depth (r_position); z: R_DEPTH_TEX's
  vec4 r_depth;        // xy: depth from w's k0, k1; z: depth bias (reversed_z.h)
};
out float gl_ClipDistance[6];
// r_pixel: 1 / viewport width and height; zw: the frame's jitter (the
// upscaler's), in clip space.
layout(std430, set = 0, binding = 3) readonly buffer r_vertices { uint r_vb[]; };
uint r_word(int b) {
  int w = b >> 2, s = (b & 3) * 8;
  return s == 0 ? r_vb[w] : (r_vb[w] >> s) | (r_vb[w + 1] << (32 - s));
}
float r_f(int b) { return uintBitsToFloat(r_word(b)); }
int r_lo(uint w) { return int(w << 16) >> 16; }
int r_hi(uint w) { return int(w) >> 16; }
// The input register as D3D9 fills it from the declaration's type;
// components the type lacks are 0 (w 1), inputs it lacks (0, 0, 0, 1).
vec4 r_fetch(int slot) {
  ivec4 d = r_in[slot];
  if (d.z < 0) return vec4(0.0, 0.0, 0.0, 1.0);
  int b = d.x + gl_VertexIndex * d.y;
  uint w = r_word(b);
  switch (d.z) {
    case 0: return vec4(uintBitsToFloat(w), 0.0, 0.0, 1.0);
    case 1: return vec4(uintBitsToFloat(w), r_f(b + 4), 0.0, 1.0);
    case 2: return vec4(uintBitsToFloat(w), r_f(b + 4), r_f(b + 8), 1.0);
    case 3: return vec4(uintBitsToFloat(w), r_f(b + 4), r_f(b + 8), r_f(b + 12));
    case 4: return vec4(uvec4(w >> 16, w >> 8, w, w >> 24) & 0xFFu) / 255.0;
    case 5: return vec4(uvec4(w, w >> 8, w >> 16, w >> 24) & 0xFFu);
    case 6: return vec4(r_lo(w), r_hi(w), 0.0, 1.0);
    case 7: { uint v = r_word(b + 4); return vec4(r_lo(w), r_hi(w), r_lo(v), r_hi(v)); }
    case 8: return vec4(uvec4(w, w >> 8, w >> 16, w >> 24) & 0xFFu) / 255.0;
    case 9: return vec4(max(vec2(r_lo(w), r_hi(w)) / 32767.0, -1.0), 0.0, 1.0);
    case 10: {
      uint v = r_word(b + 4);
      return max(vec4(r_lo(w), r_hi(w), r_lo(v), r_hi(v)) / 32767.0, -1.0);
    }
    case 11: return vec4(vec2(w & 0xFFFFu, w >> 16) / 65535.0, 0.0, 1.0);
    case 12: {
      uint v = r_word(b + 4);
      return vec4(w & 0xFFFFu, w >> 16, v & 0xFFFFu, v >> 16) / 65535.0;
    }
    case 13: return vec4(uvec3(w, w >> 10, w >> 20) & 0x3FFu, 1.0);
    case 14: {
      ivec3 v = ivec3(int(w << 22), int(w << 12), int(w << 2)) >> 22;
      return vec4(max(vec3(v) / 511.0, -1.0), 1.0);
    }
    case 15: return vec4(unpackHalf2x16(w), 0.0, 1.0);
    case 16: return vec4(unpackHalf2x16(w), unpackHalf2x16(r_word(b + 4)));
  }
  return vec4(0.0, 0.0, 0.0, 1.0);
}
// D3D9 puts pixel centres on integers, Metal on halves: half a pixel
// right and down (clip space is y-up). Depth (reversed_z.h), by
// r_clip_mask.y: 0 z; 1 (a reversed buffer) w - z: depth 1 - z / w,
// clipped where z was; 2 (reversed, from w) the same as k0 - k1 w. Less
// r_depth.z w: a reversed buffer's depth bias (DepthBiasOf; 0 otherwise).
vec4 r_position(vec4 p) {
  for (int i = 0; i < 6; i++)
    gl_ClipDistance[i] = (r_clip_mask.x & (1 << i)) != 0 ? dot(p, r_clip[i]) : 1.0;
  float z = r_clip_mask.y == 0 ? p.z : r_clip_mask.y == 1 ? p.w - p.z : fma(-r_depth.y, p.w, r_depth.x);
  if (r_depth.z != 0.0) z = fma(-r_depth.z, p.w, z);
  return vec4(p.x + p.w * (r_pixel.x + r_pixel.z), p.y - p.w * (r_pixel.y + r_pixel.w), z, p.w);
}
// What vertex sampler n read, as the shader expects it: from a reversed
// depth texture (r_clip_mask.z bit n), 1 - the depth stored.
vec4 r_depth_tex(int n, vec4 v) { return (r_clip_mask.z & (1 << n)) != 0 ? vec4(1.0 - v.rgb, v.a) : v; }
#define R_DEPTH_TEX(n, v) r_depth_tex(n, v)
)";
  } else {
    p += R"(#define R_FLOAT_BINDING set = 0, binding = 0
#define R_INT_BINDING set = 0, binding = 1
#define R_SAMPLER_BINDING(n) set = 0, binding = n
// r_alpha: x ALPHAFUNC, y ALPHAREF; z bit n: sampler n reads a reversed
// depth texture, bit 16 + n: a depth-stencil format one (R_SHADOW); w 1 if
// the depth buffer is reversed (reversed_z.h).
layout(std140, set = 0, binding = 2) uniform r_ps_state { ivec4 r_alpha; vec4 r_bias[4]; vec4 r_vscale; };
// The depth a shader writes, as a reversed depth buffer stores it.
#define R_DEPTH_OUT(d) (r_alpha.w != 0 ? 1.0 - (d) : (d))
// What sampler n read, as the shader expects it: from a reversed depth
// texture (depth in r, g and b), 1 - the depth stored.
vec4 r_depth_tex(int n, vec4 v) { return (r_alpha.z & (1 << n)) != 0 ? vec4(1.0 - v.rgb, v.a) : v; }
#define R_DEPTH_TEX(n, v) r_depth_tex(n, v)
// D3D9's read of a depth-stencil format texture (a hardware shadow map):
// 1 where the coordinate's depth is at most the depth stored, else 0.
vec4 r_shadow(int n, vec4 v, float ref) {
  return (r_alpha.z & (1 << (16 + n))) != 0 ? vec4(ref <= v.r ? 1.0 : 0.0) : v;
}
#define R_SHADOW(n, v, ref) r_shadow(n, v, ref)
// Metal samplers have no LOD bias: the shader adds it where the level is
// implicit.
#define R_BIAS(n) r_bias[(n) >> 2][(n) & 3]
// The pixel as the game sees it, integral as in D3D9: in a target drawn
// at a lower internal resolution, the game's pixel it falls in
// (r_vscale), so patterns made from it (screen-door fades) stay whole.
vec2 r_vpos() { return floor(gl_FragCoord.xy * r_vscale.xy); }
float r_vface() { return gl_FrontFacing ? 1.0 : -1.0; }
// The alpha test, on the alpha at 8-bit precision. Compiled out (function
// constant 0 false) for draws without it: a pipeline whose shader may
// discard loses the GPU's hidden surface removal.
layout(constant_id = 0) const bool r_alpha_test = true;
void r_color0(inout vec4 c) {
  if (!r_alpha_test) return;
  int a = int(round(clamp(c.a, 0.0, 1.0) * 255.0)), ref = r_alpha.y;
  bool pass = true;
  switch (r_alpha.x) {
    case 1: pass = false; break;
    case 2: pass = a < ref; break;
    case 3: pass = a == ref; break;
    case 4: pass = a <= ref; break;
    case 5: pass = a > ref; break;
    case 6: pass = a != ref; break;
    case 7: pass = a >= ref; break;
  }
  if (!pass) discard;
}
)";
  }
  return p;
}

// The lean variants (a pixel shader without the alpha test): the MSL of a
// translation (with the prelude) gets the switch as the macro R_ALPHA_TEST
// in place of the function constant r_alpha_test, so the full variant
// (R_ALPHA_TEST true) and the lean one (false) compile from the text as it
// is. True if the text has the macro (made now or before).
inline bool MetalLeanMacros(std::string* msl, bool vertex) {
  if (vertex) return false;
  if (msl->find("R_ALPHA_TEST") != std::string::npos) return true;
  static const std::string decl = "constant bool r_alpha_test_tmp [[function_constant(0)]];\n";
  static const std::string value = "is_function_constant_defined(r_alpha_test_tmp) ? r_alpha_test_tmp : true";
  const size_t d = msl->find(decl), v = msl->find(value);
  if (d == std::string::npos || v == std::string::npos || v < d) return false;
  msl->replace(v, value.size(), "R_ALPHA_TEST");
  msl->erase(d, decl.size());
  return true;
}

}  // namespace recomp::gfx
