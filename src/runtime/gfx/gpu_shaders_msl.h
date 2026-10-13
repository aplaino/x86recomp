#pragma once
// The SDL_GPU backend's fixed-function pipeline for Metal: one vertex and
// one fragment "uber-shader" whose behaviour comes from uniforms (the
// D3D state), so no shader is ever compiled per state combination.
//
// Layouts here and in gpu_device.cpp (VsUniforms, FsUniforms) must agree.
// Matrices are uploaded as D3D stores them (row-major, row vectors), which
// MSL reads as the transpose: D3D's v * M is M * v here.
//
// Vertices are pulled from a storage buffer (vb) with the FVF's offsets in
// the uniforms, so the vertex layout is not part of any pipeline.
//
// Binding (SDL_GPU's MSL conventions): uniform buffer 0 at [[buffer(0)]],
// the vertex storage buffer after it; fragment textures/samplers 0..7.

namespace recomp::gfx {

inline constexpr const char* kGpuShadersMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Light {
  float4 diffuse, specular, ambient;
  float4 position;   // view space; w: range
  float4 direction;  // view space, normalised, toward the light; w: falloff
  float4 atten;      // xyz: attenuation 0, 1, 2; w: type (1 point, 2 spot, 3 directional)
  float4 spot;       // x: cos(theta / 2), y: cos(phi / 2)
};

struct VsUniforms {
  float4x4 world_view[4];   // WORLD..WORLD3 times VIEW
  float4x4 normal[4];       // inverse transpose of their 3x3 parts
  float4x4 proj;
  float4x4 tex[8];          // TEXTURE0..7
  float4 viewport;          // x, y, width, height (pixels)
  float4 target;            // render target width, height; z: 1 if PROJECTION is perspective
  float4 ambient;           // AMBIENT render state
  float4 mat_diffuse, mat_ambient, mat_specular, mat_emissive;
  float4 mat_power;         // x
  float4 fog;               // start, end, density
  float4 clip[8];           // user clip planes, view space
  Light lights[8];
  int4 layout;              // stride, position offset, normal offset, diffuse offset
  int4 layout2;             // specular offset, point size offset, blend weights in the FVF, base (bytes)
  int4 tex_offset[2];       // per coordinate set: byte offset, -1 if absent
  int4 tex_size[2];         // per coordinate set: components
  int4 mode;                // position kind (0 xyz, 1 xyzrhw, 2 xyzw), lighting, normalize, local viewer
  int4 mode2;               // color vertex, diffuse/specular/ambient material source
  int4 mode3;               // emissive source, fog vertex mode, fog table mode, range fog
  int4 mode4;               // specular enable, blend matrices, clip plane mask, lights
  int4 mode5;               // fog enable, -, -, -
  int4 tci[2];              // per stage TEXCOORDINDEX
  int4 ttf[2];              // per stage TEXTURETRANSFORMFLAGS
  float4 point;             // x: POINTSIZE
};

struct VOut {
  float4 pos [[position]];
  float psize [[point_size]];
  float4 diff [[user(diff)]];
  float4 spec [[user(spec)]];
  float4 diff_flat [[user(diff_flat), flat]];
  float4 spec_flat [[user(spec_flat), flat]];
  float4 t0 [[user(t0)]];
  float4 t1 [[user(t1)]];
  float4 t2 [[user(t2)]];
  float4 t3 [[user(t3)]];
  float4 t4 [[user(t4)]];
  float4 t5 [[user(t5)]];
  float4 t6 [[user(t6)]];
  float4 t7 [[user(t7)]];
  float fog [[user(fog)]];  // vertex fog factor, or the depth table fog uses
  float clip [[clip_distance]] [8];
};

static float Rf(const device uint* vb, int byte_offset) {
  return as_type<float>(vb[byte_offset >> 2]);
}

static float4 Color(uint c) {
  return float4(float((c >> 16) & 0xFF), float((c >> 8) & 0xFF), float(c & 0xFF),
                float(c >> 24)) / 255.0;
}

// Material colour source: 0 material, 1 vertex diffuse, 2 vertex specular
// (the material when the vertex has no such colour).
static float4 Source(int src, float4 mat, bool has_d, float4 d, bool has_s, float4 s) {
  if (src == 1 && has_d) return d;
  if (src == 2 && has_s) return s;
  return mat;
}

static float FogFactor(int mode, float d, float4 fog) {
  if (mode == 1) return exp(-d * fog.z);
  if (mode == 2) { float x = d * fog.z; return exp(-x * x); }
  if (mode == 3) return fog.y == fog.x ? (d < fog.y ? 1.0 : 0.0) : (fog.y - d) / (fog.y - fog.x);
  return 1.0;
}

vertex VOut vs_main(uint vid [[vertex_id]],
                    constant VsUniforms& u [[buffer(0)]],
                    const device uint* vb [[buffer(1)]]) {
  VOut o;
  int base = u.layout2.w + int(vid) * u.layout.x;
  int po = base + u.layout.y;

  bool has_normal = u.layout.z >= 0, has_diffuse = u.layout.w >= 0;
  bool has_specular = u.layout2.x >= 0;
  float4 vdiff = has_diffuse ? Color(vb[(base + u.layout.w) >> 2]) : float4(1.0);
  float4 vspec = has_specular ? Color(vb[(base + u.layout2.x) >> 2]) : float4(0.0);
  float3 nrm = has_normal ? float3(Rf(vb, base + u.layout.z), Rf(vb, base + u.layout.z + 4),
                                   Rf(vb, base + u.layout.z + 8))
                          : float3(0.0);

  float4 view_pos = float4(0.0, 0.0, 0.0, 1.0);
  float3 view_n = float3(0.0);
  float table_depth;
  if (u.mode.x == 1) {
    // Pre-transformed: screen pixels of the render target, z already in
    // 0..1. D3D9 puts pixel centres on integers, Metal on halves.
    float x = Rf(vb, po), y = Rf(vb, po + 4), z = Rf(vb, po + 8), rhw = Rf(vb, po + 12);
    float w = (rhw == 0.0 || !isfinite(rhw)) ? 1.0 : 1.0 / rhw;
    float nx = (x + 0.5) / u.target.x * 2.0 - 1.0;
    float ny = 1.0 - (y + 0.5) / u.target.y * 2.0;
    o.pos = float4(nx * w, ny * w, z * w, w);
    table_depth = u.target.z != 0.0 ? w : z;
  } else {
    float4 p = float4(Rf(vb, po), Rf(vb, po + 4), Rf(vb, po + 8),
                      u.mode.x == 2 ? Rf(vb, po + 12) : 1.0);
    // Vertex blending: weights follow the position; the last matrix gets
    // what the others leave.
    int nb = u.mode4.y;
    if (nb > 0) {
      float wsum = 0.0;
      view_pos = float4(0.0);
      for (int i = 0; i < nb; i++) {
        float wt = i < u.layout2.z ? Rf(vb, po + 12 + 4 * i) : 0.0;
        wsum += wt;
        view_pos += wt * (u.world_view[i] * p);
        view_n += wt * (u.normal[i] * float4(nrm, 0.0)).xyz;
      }
      view_pos += (1.0 - wsum) * (u.world_view[nb] * p);
      view_n += (1.0 - wsum) * (u.normal[nb] * float4(nrm, 0.0)).xyz;
    } else {
      view_pos = u.world_view[0] * p;
      view_n = (u.normal[0] * float4(nrm, 0.0)).xyz;
    }
    if (u.mode.z != 0 && dot(view_n, view_n) > 0.0) view_n = normalize(view_n);
    float4 c = u.proj * view_pos;
    table_depth = u.target.z != 0.0 ? view_pos.z / view_pos.w : c.z / c.w;
    // D3D9's integer pixel centres: half a pixel right and down.
    c.x += c.w / u.viewport.z;
    c.y -= c.w / u.viewport.w;
    o.pos = c;
  }

  // ---- lighting
  float4 diff = vdiff, spec = vspec;
  if (u.mode.y != 0 && u.mode.x != 1) {
    bool cv = u.mode2.x != 0;
    float4 md = cv ? Source(u.mode2.y, u.mat_diffuse, has_diffuse, vdiff, has_specular, vspec) : u.mat_diffuse;
    float4 ms = cv ? Source(u.mode2.z, u.mat_specular, has_diffuse, vdiff, has_specular, vspec) : u.mat_specular;
    float4 ma = cv ? Source(u.mode2.w, u.mat_ambient, has_diffuse, vdiff, has_specular, vspec) : u.mat_ambient;
    float4 me = cv ? Source(u.mode3.x, u.mat_emissive, has_diffuse, vdiff, has_specular, vspec) : u.mat_emissive;
    float3 P = view_pos.xyz / view_pos.w;
    // Toward the viewer: the camera looks down +z, so a viewer at infinity
    // (LOCALVIEWER off) is toward -z.
    float3 V = u.mode.w != 0 ? normalize(-P) : float3(0.0, 0.0, -1.0);
    float3 amb = u.ambient.rgb, dsum = float3(0.0), ssum = float3(0.0);
    for (int i = 0; i < u.mode4.w; i++) {
      Light l = u.lights[i];
      int type = int(l.atten.w);
      float3 L;
      float att = 1.0;
      if (type == 3) {
        L = l.direction.xyz;
      } else {
        float3 d = l.position.xyz - P;
        float dist = length(d);
        if (dist > l.position.w) continue;
        L = dist > 0.0 ? d / dist : float3(0.0, 0.0, 1.0);
        float den = l.atten.x + l.atten.y * dist + l.atten.z * dist * dist;
        att = den > 0.0 ? 1.0 / den : 1.0;
        if (type == 2) {
          float rho = dot(L, l.direction.xyz);
          if (rho <= l.spot.y) att = 0.0;
          else if (rho < l.spot.x)
            att *= pow((rho - l.spot.y) / (l.spot.x - l.spot.y), l.direction.w);
        }
      }
      amb += l.ambient.rgb * att;
      float ndl = dot(view_n, L);
      if (ndl > 0.0) {
        dsum += l.diffuse.rgb * ndl * att;
        if (u.mode4.x != 0) {
          float ndh = dot(view_n, normalize(L + V));
          if (ndh > 0.0) ssum += l.specular.rgb * pow(ndh, u.mat_power.x) * att;
        }
      }
    }
    diff = float4(saturate(me.rgb + ma.rgb * amb + md.rgb * dsum), md.a);
    spec = float4(saturate(ms.rgb * ssum), ms.a);
  }
  o.diff = diff;
  o.spec = spec;
  o.diff_flat = diff;
  o.spec_flat = spec;

  // ---- fog
  if (u.mode5.x != 0 && u.mode3.z != 0) {
    o.fog = table_depth;  // table (pixel) fog: the fragment shader applies the table
  } else if (u.mode5.x != 0 && u.mode3.y != 0 && u.mode.x != 1) {
    float d = u.mode3.w != 0 ? length(view_pos.xyz / view_pos.w) : view_pos.z / view_pos.w;
    o.fog = saturate(FogFactor(u.mode3.y, d, u.fog));
  } else {
    o.fog = spec.a;  // D3D's vertex fog from the specular alpha
  }

  // ---- texture coordinates per stage
  float4 tc[8];
  for (int s = 0; s < 8; s++) {
    int idx = u.tci[s >> 2][s & 3];
    int gen = idx >> 16;
    int set = idx & 7;
    float4 t;
    int comps;
    if (gen == 1) {
      t = float4(view_n, 1.0); comps = 3;
    } else if (gen == 2) {
      t = float4(view_pos.xyz / view_pos.w, 1.0); comps = 3;
    } else if (gen == 3) {
      float3 E = u.mode.w != 0 ? normalize(-view_pos.xyz / view_pos.w) : float3(0.0, 0.0, -1.0);
      t = float4(2.0 * dot(E, view_n) * view_n - E, 1.0); comps = 3;
    } else {
      int off = u.tex_offset[set >> 2][set & 3];
      comps = u.tex_size[set >> 2][set & 3];
      t = float4(0.0, 0.0, 0.0, 1.0);
      if (off >= 0) {
        for (int k = 0; k < 4; k++)
          if (k < comps) t[k] = Rf(vb, base + off + 4 * k);
      }
    }
    int ttf = u.ttf[s >> 2][s & 3];
    if ((ttf & 0xFF) != 0) {
      // The transform sees (u, 1, 0, 0), (u, v, 1, 0) or (u, v, w, 1).
      float4 in = t;
      if (comps == 1) in = float4(t.x, 1.0, 0.0, 0.0);
      else if (comps == 2) in = float4(t.xy, 1.0, 0.0);
      else if (comps == 3) in = float4(t.xyz, 1.0);
      t = u.tex[s] * in;
    }
    tc[s] = t;
  }
  o.t0 = tc[0]; o.t1 = tc[1]; o.t2 = tc[2]; o.t3 = tc[3];
  o.t4 = tc[4]; o.t5 = tc[5]; o.t6 = tc[6]; o.t7 = tc[7];

  for (int i = 0; i < 8; i++)
    o.clip[i] = (u.mode.x != 1 && (u.mode4.z & (1 << i)) != 0)
                    ? dot(u.clip[i], view_pos) : 1.0;
  o.psize = u.layout2.y >= 0 ? Rf(vb, base + u.layout2.y) : u.point.x;
  return o;
}

struct FsUniforms {
  int4 color[8];     // per stage: COLOROP, COLORARG1, COLORARG2, COLORARG0
  int4 alpha[8];     // per stage: ALPHAOP, ALPHAARG1, ALPHAARG2, ALPHAARG0
  int4 stage[8];     // per stage: RESULTARG, texture bound, projected divisor index (-1 none), -
  float4 tfactor;
  float4 fog_color;
  float4 fog;        // start, end, density
  int4 mode;         // alpha test func (0 off), alpha ref, fog: 0 off / 1 vertex / 2 table, table mode
  int4 mode2;        // flat shading, specular enable, -, -
};

struct FIn {
  float4 pos [[position]];
  float4 diff [[user(diff)]];
  float4 spec [[user(spec)]];
  float4 diff_flat [[user(diff_flat), flat]];
  float4 spec_flat [[user(spec_flat), flat]];
  float4 t0 [[user(t0)]];
  float4 t1 [[user(t1)]];
  float4 t2 [[user(t2)]];
  float4 t3 [[user(t3)]];
  float4 t4 [[user(t4)]];
  float4 t5 [[user(t5)]];
  float4 t6 [[user(t6)]];
  float4 t7 [[user(t7)]];
  float fog [[user(fog)]];
};

static float4 Arg(int a, float4 diff, float4 cur, float4 tex, float4 tf, float4 spec, float4 temp) {
  float4 v;
  switch (a & 0xF) {
    case 0: v = diff; break;
    case 1: v = cur; break;
    case 2: v = tex; break;
    case 3: v = tf; break;
    case 4: v = spec; break;
    case 5: v = temp; break;
    default: v = float4(0.0); break;
  }
  if (a & 0x20) v = v.aaaa;    // ALPHAREPLICATE
  if (a & 0x10) v = 1.0 - v;   // COMPLEMENT
  return v;
}

static float4 Op(int op, float4 a1, float4 a2, float4 a0, float4 diff, float4 tex,
                 float4 tf, float4 cur) {
  switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return a1 * a2;
    case 5: return a1 * a2 * 2.0;
    case 6: return a1 * a2 * 4.0;
    case 7: return a1 + a2;
    case 8: return a1 + a2 - 0.5;
    case 9: return (a1 + a2 - 0.5) * 2.0;
    case 10: return a1 - a2;
    case 11: return a1 + a2 - a1 * a2;
    case 12: return mix(a2, a1, diff.a);
    case 13: return mix(a2, a1, tex.a);
    case 14: return mix(a2, a1, tf.a);
    case 15: return a1 + a2 * (1.0 - tex.a);
    case 16: return mix(a2, a1, cur.a);
    case 17: return a1;  // PREMODULATE: approximated
    case 18: return float4(a1.rgb + a1.a * a2.rgb, a1.a);
    case 19: return float4(a1.rgb * a2.rgb + a1.a, a1.a);
    case 20: return float4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a);
    case 21: return float4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a);
    case 24: { float d = saturate(4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5)); return float4(d); }
    case 25: return a0 + a1 * a2;
    case 26: return mix(a2, a1, a0);
    default: return a1;  // bump mapping: not modelled
  }
}

static bool AlphaPass(int func, float a, int ref) {
  int v = int(rint(saturate(a) * 255.0));
  switch (func) {
    case 1: return false;
    case 2: return v < ref;
    case 3: return v == ref;
    case 4: return v <= ref;
    case 5: return v > ref;
    case 6: return v != ref;
    case 7: return v >= ref;
    default: return true;
  }
}

fragment float4 fs_main(FIn in [[stage_in]], constant FsUniforms& u [[buffer(0)]],
                        texture2d<float> tx0 [[texture(0)]], sampler sm0 [[sampler(0)]],
                        texture2d<float> tx1 [[texture(1)]], sampler sm1 [[sampler(1)]],
                        texture2d<float> tx2 [[texture(2)]], sampler sm2 [[sampler(2)]],
                        texture2d<float> tx3 [[texture(3)]], sampler sm3 [[sampler(3)]],
                        texture2d<float> tx4 [[texture(4)]], sampler sm4 [[sampler(4)]],
                        texture2d<float> tx5 [[texture(5)]], sampler sm5 [[sampler(5)]],
                        texture2d<float> tx6 [[texture(6)]], sampler sm6 [[sampler(6)]],
                        texture2d<float> tx7 [[texture(7)]], sampler sm7 [[sampler(7)]]) {
  float4 diff = u.mode2.x != 0 ? in.diff_flat : in.diff;
  float4 spec = u.mode2.x != 0 ? in.spec_flat : in.spec;
  float4 tcs[8] = {in.t0, in.t1, in.t2, in.t3, in.t4, in.t5, in.t6, in.t7};
  float4 cur = diff, temp = float4(0.0);
  for (int s = 0; s < 8; s++) {
    int cop = u.color[s].x;
    if (cop == 1) break;  // DISABLE ends the cascade
    float4 tc = tcs[s];
    int proj = u.stage[s].z;
    float2 uv = proj >= 0 ? tc.xy / tc[proj] : tc.xy;
    float4 tex = float4(0.0, 0.0, 0.0, 1.0);
    if (u.stage[s].y != 0) {
      switch (s) {
        case 0: tex = tx0.sample(sm0, uv); break;
        case 1: tex = tx1.sample(sm1, uv); break;
        case 2: tex = tx2.sample(sm2, uv); break;
        case 3: tex = tx3.sample(sm3, uv); break;
        case 4: tex = tx4.sample(sm4, uv); break;
        case 5: tex = tx5.sample(sm5, uv); break;
        case 6: tex = tx6.sample(sm6, uv); break;
        default: tex = tx7.sample(sm7, uv); break;
      }
    }
    int4 c = u.color[s], a = u.alpha[s];
    float4 c1 = Arg(c.y, diff, cur, tex, u.tfactor, spec, temp);
    float4 c2 = Arg(c.z, diff, cur, tex, u.tfactor, spec, temp);
    float4 c0 = Arg(c.w, diff, cur, tex, u.tfactor, spec, temp);
    float3 rgb = saturate(Op(cop, c1, c2, c0, diff, tex, u.tfactor, cur).rgb);
    float al = cur.a;
    if (a.x != 1) {
      float4 a1 = Arg(a.y, diff, cur, tex, u.tfactor, spec, temp);
      float4 a2 = Arg(a.z, diff, cur, tex, u.tfactor, spec, temp);
      float4 a0 = Arg(a.w, diff, cur, tex, u.tfactor, spec, temp);
      al = saturate(Op(a.x, a1, a2, a0, diff, tex, u.tfactor, cur).a);
    }
    if (cop == 24) al = rgb.r;  // DOTPRODUCT3 replicates into alpha too
    if (u.stage[s].x == 5) temp = float4(rgb, al);
    else cur = float4(rgb, al);
  }
  if (u.mode2.y != 0) cur.rgb = saturate(cur.rgb + spec.rgb);
  if (!AlphaPass(u.mode.x, cur.a, u.mode.y)) discard_fragment();
  if (u.mode.z != 0) {
    float f = u.mode.z == 2 ? FogFactor(u.mode.w, in.fog, u.fog) : in.fog;
    cur.rgb = mix(u.fog_color.rgb, cur.rgb, saturate(f));
  }
  return cur;
}
)MSL";

}  // namespace recomp::gfx
