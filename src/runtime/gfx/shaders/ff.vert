#version 450
// The SDL_GPU backend's fixed-function vertex stage: one "uber-shader"
// whose behaviour comes from uniforms (the D3D state), so no shader is
// ever compiled per state combination.
//
// The uniform layouts here and VsUniforms, VsLayout in gpu_device.cpp
// must agree (std140: every member is a vec4, ivec4 or mat4). Matrices are
// uploaded as D3D stores them (row-major, row vectors), which GLSL reads
// as the transpose: D3D's v * M is M * v here.
//
// Vertices are pulled from a storage buffer (vb): each attribute of vertex
// v at its address + v * its stride, both in the uniforms, so the vertex
// layout is not part of any pipeline and the attributes may be
// interleaved or in separate streams.
//
// Bindings follow SDL_GPU's SPIR-V conventions for vertex shaders: storage
// buffers in set 0, uniform buffers in set 1. The Metal build (METAL
// defined, converted by SPIRV-Cross with the binding numbers as Metal
// indices) needs its order instead: uniform buffers, then storage buffers.
#ifdef METAL
#define UNIFORMS_BINDING set = 0, binding = 0
#define LAYOUT_BINDING set = 0, binding = 1
#define VERTICES_BINDING set = 0, binding = 2
#else
#define UNIFORMS_BINDING set = 1, binding = 0
#define LAYOUT_BINDING set = 1, binding = 1
#define VERTICES_BINDING set = 0, binding = 0
#endif

struct Light {
  vec4 diffuse, specular, ambient;
  vec4 position;   // view space; w: range
  vec4 direction;  // view space, normalised, toward the light; w: falloff
  vec4 atten;      // xyz: attenuation 0, 1, 2; w: type (1 point, 2 spot, 3 directional)
  vec4 spot;       // x: cos(theta / 2), y: cos(phi / 2)
};

layout(std140, UNIFORMS_BINDING) uniform VsUniforms {
  mat4 world_view[4];   // WORLD..WORLD3 times VIEW
  mat4 normal[4];       // inverse transpose of their 3x3 parts
  mat4 proj;
  mat4 tex[8];          // TEXTURE0..7
  vec4 viewport;        // x, y, width, height (pixels)
  vec4 target;          // render target width, height; z: 1 if PROJECTION is perspective
  vec4 ambient;         // AMBIENT render state
  vec4 mat_diffuse, mat_ambient, mat_specular, mat_emissive;
  vec4 mat_power;       // x
  vec4 fog;             // start, end, density
  vec4 clip[8];         // user clip planes, view space
  Light lights[8];
  ivec4 mode;           // position kind (0 xyz, 1 xyzrhw, 2 xyzw), lighting, normalize, local viewer
  ivec4 mode2;          // color vertex, diffuse/specular/ambient material source
  ivec4 mode3;          // emissive source, fog vertex mode, fog table mode, range fog
  ivec4 mode4;          // specular enable, blend matrices, clip plane mask, lights
  ivec4 mode5;          // fog enable, -, -, -
  ivec4 tci[2];         // per stage TEXCOORDINDEX
  ivec4 ttf[2];         // per stage TEXTURETRANSFORMFLAGS
  vec4 point;           // x: POINTSIZE
} u;

// Where the draw's vertices are (VsLayout), apart so that the block above
// stays the same from draw to draw. Attribute addresses are vertex 0's in
// bytes, -1 if absent.
layout(std140, LAYOUT_BINDING) uniform VsLayout {
  ivec4 layout0;        // position stride, position, normal, diffuse addresses
  ivec4 layout2;        // specular, point size addresses, blend weights in the FVF, R, G, B, A colours
  ivec4 strides;        // normal, diffuse, specular, point size strides
  ivec4 tex_offset[2];  // per coordinate set: address
  ivec4 tex_stride[2];  // per coordinate set: stride
  ivec4 tex_size[2];    // per coordinate set: components
} l;

layout(std430, VERTICES_BINDING) readonly buffer Vertices { uint vb[]; };

out gl_PerVertex {
  vec4 gl_Position;
  float gl_PointSize;
  float gl_ClipDistance[8];
};

layout(location = 0) out vec4 o_diff;
layout(location = 1) out vec4 o_spec;
layout(location = 2) flat out vec4 o_diff_flat;
layout(location = 3) flat out vec4 o_spec_flat;
layout(location = 4) out vec4 o_t[8];
layout(location = 12) out float o_fog;  // vertex fog factor, or the depth table fog uses

float Rf(int byte_offset) { return uintBitsToFloat(vb[byte_offset >> 2]); }

// A D3DCOLOR, or bytes R, G, B, A when layout2.w is set.
vec4 Color(uint c) {
  if (l.layout2.w != 0) c = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
  return vec4(float((c >> 16) & 0xFFu), float((c >> 8) & 0xFFu), float(c & 0xFFu),
              float(c >> 24)) / 255.0;
}

// Material colour source: 0 material, 1 vertex diffuse, 2 vertex specular
// (the material when the vertex has no such colour).
vec4 Source(int src, vec4 mat, bool has_d, vec4 d, bool has_s, vec4 s) {
  if (src == 1 && has_d) return d;
  if (src == 2 && has_s) return s;
  return mat;
}

float FogFactor(int mode, float d, vec4 fog) {
  if (mode == 1) return exp(-d * fog.z);
  if (mode == 2) { float x = d * fog.z; return exp(-x * x); }
  if (mode == 3) return fog.y == fog.x ? (d < fog.y ? 1.0 : 0.0) : (fog.y - d) / (fog.y - fog.x);
  return 1.0;
}

void main() {
  int v = gl_VertexIndex;
  int po = l.layout0.y + v * l.layout0.x;

  bool has_normal = l.layout0.z >= 0, has_diffuse = l.layout0.w >= 0;
  bool has_specular = l.layout2.x >= 0;
  vec4 vdiff = has_diffuse ? Color(vb[(l.layout0.w + v * l.strides.y) >> 2]) : vec4(1.0);
  vec4 vspec = has_specular ? Color(vb[(l.layout2.x + v * l.strides.z) >> 2]) : vec4(0.0);
  int no = l.layout0.z + v * l.strides.x;
  vec3 nrm = has_normal ? vec3(Rf(no), Rf(no + 4), Rf(no + 8))
                        : vec3(0.0);

  vec4 view_pos = vec4(0.0, 0.0, 0.0, 1.0);
  vec3 view_n = vec3(0.0);
  float table_depth;
  if (u.mode.x == 1) {
    // Pre-transformed: screen pixels of the render target, z already in
    // 0..1. D3D9 puts pixel centres on integers, SDL_GPU on halves.
    float x = Rf(po), y = Rf(po + 4), z = Rf(po + 8), rhw = Rf(po + 12);
    float w = (rhw == 0.0 || isinf(rhw) || isnan(rhw)) ? 1.0 : 1.0 / rhw;
    float nx = (x + 0.5) / u.target.x * 2.0 - 1.0;
    float ny = 1.0 - (y + 0.5) / u.target.y * 2.0;
    gl_Position = vec4(nx * w, ny * w, z * w, w);
    table_depth = u.target.z != 0.0 ? w : z;
  } else {
    vec4 p = vec4(Rf(po), Rf(po + 4), Rf(po + 8), u.mode.x == 2 ? Rf(po + 12) : 1.0);
    // Vertex blending: weights follow the position; the last matrix gets
    // what the others leave.
    int nb = u.mode4.y;
    if (nb > 0) {
      float wsum = 0.0;
      view_pos = vec4(0.0);
      for (int i = 0; i < nb; i++) {
        float wt = i < l.layout2.z ? Rf(po + 12 + 4 * i) : 0.0;
        wsum += wt;
        view_pos += wt * (u.world_view[i] * p);
        view_n += wt * (u.normal[i] * vec4(nrm, 0.0)).xyz;
      }
      view_pos += (1.0 - wsum) * (u.world_view[nb] * p);
      view_n += (1.0 - wsum) * (u.normal[nb] * vec4(nrm, 0.0)).xyz;
    } else {
      view_pos = u.world_view[0] * p;
      view_n = (u.normal[0] * vec4(nrm, 0.0)).xyz;
    }
    if (u.mode.z != 0 && dot(view_n, view_n) > 0.0) view_n = normalize(view_n);
    vec4 c = u.proj * view_pos;
    table_depth = u.target.z != 0.0 ? view_pos.z / view_pos.w : c.z / c.w;
    // D3D9's integer pixel centres: half a pixel right and down.
    c.x += c.w / u.viewport.z;
    c.y -= c.w / u.viewport.w;
    gl_Position = c;
  }

  // ---- lighting
  vec4 diff = vdiff, spec = vspec;
  if (u.mode.y != 0 && u.mode.x != 1) {
    bool cv = u.mode2.x != 0;
    vec4 md = cv ? Source(u.mode2.y, u.mat_diffuse, has_diffuse, vdiff, has_specular, vspec) : u.mat_diffuse;
    vec4 ms = cv ? Source(u.mode2.z, u.mat_specular, has_diffuse, vdiff, has_specular, vspec) : u.mat_specular;
    vec4 ma = cv ? Source(u.mode2.w, u.mat_ambient, has_diffuse, vdiff, has_specular, vspec) : u.mat_ambient;
    vec4 me = cv ? Source(u.mode3.x, u.mat_emissive, has_diffuse, vdiff, has_specular, vspec) : u.mat_emissive;
    vec3 P = view_pos.xyz / view_pos.w;
    // Toward the viewer: the camera looks down +z, so a viewer at infinity
    // (LOCALVIEWER off) is toward -z.
    vec3 V = u.mode.w != 0 ? normalize(-P) : vec3(0.0, 0.0, -1.0);
    vec3 amb = u.ambient.rgb, dsum = vec3(0.0), ssum = vec3(0.0);
    for (int i = 0; i < u.mode4.w; i++) {
      Light l = u.lights[i];
      int type = int(l.atten.w);
      vec3 L;
      float att = 1.0;
      if (type == 3) {
        L = l.direction.xyz;
      } else {
        vec3 d = l.position.xyz - P;
        float dist = length(d);
        if (dist > l.position.w) continue;
        L = dist > 0.0 ? d / dist : vec3(0.0, 0.0, 1.0);
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
    diff = vec4(clamp(me.rgb + ma.rgb * amb + md.rgb * dsum, 0.0, 1.0), md.a);
    spec = vec4(clamp(ms.rgb * ssum, 0.0, 1.0), ms.a);
  }
  o_diff = diff;
  o_spec = spec;
  o_diff_flat = diff;
  o_spec_flat = spec;

  // ---- fog
  if (u.mode5.x != 0 && u.mode3.z != 0) {
    o_fog = table_depth;  // table (pixel) fog: the fragment shader applies the table
  } else if (u.mode5.x != 0 && u.mode3.y != 0 && u.mode.x != 1) {
    float d = u.mode3.w != 0 ? length(view_pos.xyz / view_pos.w) : view_pos.z / view_pos.w;
    o_fog = clamp(FogFactor(u.mode3.y, d, u.fog), 0.0, 1.0);
  } else {
    o_fog = spec.a;  // D3D's vertex fog from the specular alpha
  }

  // ---- texture coordinates per stage
  for (int s = 0; s < 8; s++) {
    int idx = u.tci[s >> 2][s & 3];
    int gen = idx >> 16;
    int set = idx & 7;
    vec4 t;
    int comps;
    if (gen == 1) {
      t = vec4(view_n, 1.0); comps = 3;
    } else if (gen == 2) {
      t = vec4(view_pos.xyz / view_pos.w, 1.0); comps = 3;
    } else if (gen == 3) {
      vec3 E = u.mode.w != 0 ? normalize(-view_pos.xyz / view_pos.w) : vec3(0.0, 0.0, -1.0);
      t = vec4(2.0 * dot(E, view_n) * view_n - E, 1.0); comps = 3;
    } else {
      int off = l.tex_offset[set >> 2][set & 3];
      comps = l.tex_size[set >> 2][set & 3];
      t = vec4(0.0, 0.0, 0.0, 1.0);
      if (off >= 0) {
        off += v * l.tex_stride[set >> 2][set & 3];
        for (int k = 0; k < 4; k++)
          if (k < comps) t[k] = Rf(off + 4 * k);
      }
    }
    int ttf = u.ttf[s >> 2][s & 3];
    if ((ttf & 0xFF) != 0) {
      // The transform sees (u, 1, 0, 0), (u, v, 1, 0) or (u, v, w, 1).
      vec4 tin = t;
      if (comps == 1) tin = vec4(t.x, 1.0, 0.0, 0.0);
      else if (comps == 2) tin = vec4(t.xy, 1.0, 0.0);
      else if (comps == 3) tin = vec4(t.xyz, 1.0);
      t = u.tex[s] * tin;
    }
    o_t[s] = t;
  }

  for (int i = 0; i < 8; i++)
    gl_ClipDistance[i] = (u.mode.x != 1 && (u.mode4.z & (1 << i)) != 0)
                             ? dot(u.clip[i], view_pos) : 1.0;
  gl_PointSize = l.layout2.y >= 0 ? Rf(l.layout2.y + v * l.strides.w) : u.point.x;
}
