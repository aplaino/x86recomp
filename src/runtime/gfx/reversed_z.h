#pragma once
// Reversed depth (the Metal backend, RECOMP_REVERSED_Z): a float depth
// buffer holds 1 - d for the d Direct3D 9 would store, so the values far
// from the camera, the ones a standard projection crowds next to 1, sit
// next to 0 where a float has the most precision (NVIDIA, "Depth
// Precision Visualized", 2015: a float buffer with near and far swapped
// keeps a nearly even relative error over the whole range). D16 buffers
// are not reversed (fixed point: nothing to gain).
//
// The game never sees it: everything that writes or reads depth is
// mirrored where Direct3D 9's convention turns into the buffer's.
//   - Vertex shaders: clip-space z becomes w - z (metal_prelude.h's
//     r_position), so the depth after the divide is 1 - z / w. Clipping
//     (0 <= z <= w) is unchanged.
//   - Depth tests: ZFUNC mirrored (MirrorDepthCompare).
//   - Viewport: MinZ..MaxZ becomes 1 - MaxZ..1 - MinZ, so the depth after
//     the viewport is 1 - (MinZ + z (MaxZ - MinZ)) (ReversedViewportDepth).
//   - Clears to z clear to 1 - z; DEPTHBIAS and SLOPESCALEDEPTHBIAS move
//     the stored value the other way, DEPTHBIAS from the vertex shader
//     (DepthBiasOf).
//   - Pixel shaders: depth they write (oDepth) is stored as 1 - depth, and
//     what they sample from a reversed depth texture (INTZ: the scene's
//     depth read by lighting, fog, depth of field, soft particles) is read
//     as 1 - the value stored, in vertex shaders too; such a texture's
//     border colour is black for white and white for black
//     (ReversedDepthBorder).
//
// Depth from w (RECOMP_DEPTH_FROM_W, on unless 0; with reversed depth
// only, for a game that names its world-view-projection registers,
// Device::SetDepthFromW). A perspective projection gives z = a (w - n),
// a = f / (f - n), so the reversed clip-space depth w - z is a n -
// (a - 1) w: computed from w alone it is what D3D9 stores (the same
// value, the same near and far clipping), without z's rounding. z is made
// from the full world-view-projection over world-sized coordinates, and
// its float error (about 1e-4 at a kilometre) is more than the depth
// between layers a few millimetres apart at 30 m (posters on a wall), so
// they fought; w's error is a distance, about 1e-4 m.
//   - A draw qualifies when its vertex shader's position is the vertex
//     times those registers, z and w alike (hlsl::Program::
//     position_matrix: no z of its own, such as a push towards the camera
//     or z = w for the sky), and its matrix is such a perspective
//     (PerspectiveOf: not orthographic, not oblique).
//   - One a and n per depth buffer content (WDepth): the first such draw
//     that tests or writes depth after a clear of the whole buffer sets
//     them (the game's own near and far planes where it gives them and
//     they agree: Device::SetDepthFromW), the next ones use them if their
//     own near and far planes are those (WDepth::Same), so their order is
//     the order of their w whatever their own matrices' rounding. The
//     others keep w - z, which stores the same values for the same
//     projection: nothing reads the buffer differently.

#include <cmath>
#include <cstdint>

namespace recomp::gfx {

// A D3DCMPFUNC (NEVER 1 .. ALWAYS 8) for depth stored as 1 - d: true for
// (1 - a, 1 - b) exactly when the original is for (a, b).
constexpr uint32_t MirrorDepthCompare(uint32_t d3d) {
  switch (d3d) {
    case 2: return 5;  // LESS -> GREATER
    case 4: return 7;  // LESSEQUAL -> GREATEREQUAL
    case 5: return 2;  // GREATER -> LESS
    case 7: return 4;  // GREATEREQUAL -> LESSEQUAL
    default: return d3d;  // NEVER, EQUAL, NOTEQUAL, ALWAYS
  }
}

// A viewport's MinZ, MaxZ for depth stored as 1 - d.
struct DepthRange {
  float min_z, max_z;
};
constexpr DepthRange ReversedViewportDepth(float min_z, float max_z) {
  return {1.0f - max_z, 1.0f - min_z};
}

// A draw's depth bias as the Metal device applies it. D3D9 adds DEPTHBIAS
// (`bias`, in units of depth: the legacy ZBIAS already folded in) and
// SLOPESCALEDEPTHBIAS times the depth's largest screen slope to the depth
// after the viewport. Metal's constant bias on a float buffer is in units
// of the stored value's own exponent (2^(e - 23)): `scale` (2^23 for float
// buffers) makes it D3D9's amount near 0.5 to 1 only, and with reversed
// storage the far geometry's stored values are near 0, where it is
// thousands of times weaker. So on a reversed buffer the constant part is
// made in the vertex shader in units of depth: r_position takes `clip`
// times w from the clip-space z, which after the divide and the viewport
// (MaxZ - MinZ wide) is `bias` taken from the stored value, as D3D9 adds
// it. The slope part stays with the rasterizer, negated (the stored
// depth's slope is the same size). D3D9 clips before adding the bias, the
// shader's z is clipped after: the part of a biased draw that the bias
// moves past the near or far plane (within `bias` of it) is clipped, not
// clamped. A viewport with no depth range (MaxZ == MinZ) has no z to
// carry it: the rasterizer's, negated.
struct DepthBias {
  float constant;  // setDepthBias's
  float slope;     // its slopeScale
  float clip;      // r_position's (r_depth.z), 0 unless reversed
};
inline DepthBias DepthBiasOf(float bias, float slope, bool reversed, float min_z, float max_z, float scale) {
  if (!reversed) return {bias * scale, slope, 0.0f};
  const float range = max_z - min_z;
  if (!(range > 0.0f)) return {-bias * scale, -slope, 0.0f};
  return {0.0f, -slope, bias / range};
}

// A sampler's fixed border colour (MTLSamplerBorderColor: 0 transparent
// black, 1 opaque black, 2 opaque white) for a reversed depth texture:
// what it reads is taken as 1 - rgb (R_DEPTH_TEX), so black and white
// swap. Transparent black becomes opaque white: depth 0 as D3D9 reads
// it, alpha 1 (Metal has no transparent white).
constexpr uint8_t ReversedDepthBorder(uint8_t border) { return border == 2 ? 1 : 2; }

// A world-view-projection's depth as a function of w: z = a (w - n).
struct WProjection {
  double a = 1, n = 0;
  double tol = 0;    // how far n may be off: the matrix's float rounding
  double a_tol = 0;  // how far a may be off, the same
};

// The rows of a world-view-projection (D3D's row vectors: clip = v M).
// False unless z = a (w - n) for every v, a >= 1, n > 0: rows 0-2 have z
// = a w (within float rounding), w not all 0 (orthographic: w column 0,
// 0, 0, 1), and row 3 gives n = -(z - a w) / a.
inline bool PerspectiveOf(const float m[4][4], WProjection* out) {
  int r = 0;
  double scale = 0;
  for (int i = 0; i < 3; i++) {
    if (std::fabs(m[i][3]) > std::fabs(m[r][3])) r = i;
    scale = std::fmax(scale, std::fmax(std::fabs(m[i][2]), std::fabs(m[i][3])));
  }
  if (!(std::fabs(m[r][3]) > 1e-6 * scale) || !std::isfinite(scale)) return false;
  const double a = double(m[r][2]) / double(m[r][3]);
  if (!(a >= 1.0) || !(a < 2.0)) return false;
  for (int i = 0; i < 3; i++)
    if (!(std::fabs(double(m[i][2]) - a * double(m[i][3])) <= 1e-5 * scale)) return false;
  const double n = -(double(m[3][2]) - a * double(m[3][3])) / a;
  if (!(n > 0) || !std::isfinite(n)) return false;
  out->a = a;
  out->n = n;
  // Row 3 is a sum of products of world-sized numbers (a world matrix's
  // translation times the view's), rounded to about 1e-4 of a metre
  // where the world is kilometres across: n is known to a millimetre,
  // and to a millionth of row 3's own size.
  out->tol = 1e-3 + 1e-6 * (std::fabs(m[3][2]) + std::fabs(m[3][3]));
  // a is the ratio of two sums of float products (a rotation's rows times
  // the projection's): a few float roundings of it, about 4e-7 a. Of
  // a - 1 (n / (f - n)), the part that tells the far plane, that is up to
  // 4e-7 a / (a - 1): a thousandth with f 3000 n.
  out->a_tol = 1e-6 * a;
  return true;
}

// The perspective of planes n, f (D3D's: depth 0 at n, 1 at f).
inline WProjection PerspectiveOfPlanes(double n, double f) { return {f / (f - n), n, 0, 0}; }

// The projection a depth buffer's content is drawn from w with: one per
// content, so the order of those draws is that of their w.
struct WDepth {
  bool fresh = true;    // cleared whole since the last draw from w
  double a = 1, n = 0;  // the content's

  // Whether a draw of projection p is drawn from w here. `depth`: it tests
  // or writes depth (a draw that does neither has nothing to order, and
  // does not set the content's projection). The first that does after a
  // clear sets the content's projection: `exact`'s if given (the game's
  // own planes, not rounded as a matrix's are) and p agrees with it.
  bool Use(const WProjection& p, const WProjection* exact = nullptr, bool depth = true) {
    if (!depth) return false;
    if (fresh) {
      fresh = false;
      const bool take = exact && Same(*exact, p);
      a = take ? exact->a : p.a;
      n = take ? exact->n : p.n;
      return true;
    }
    return Same({a, n, 0, 0}, p);
  }
  // Whether p (from a matrix, within its rounding: p.tol, p.a_tol) has
  // c's near and far planes: n within p.tol, and a - 1 (n / (f - n): the
  // far plane, relative to the near one) within a ten-thousandth of
  // itself, or of p's rounding where that is more.
  static bool Same(const WProjection& c, const WProjection& p) {
    const double k = c.a - 1.0;
    return std::fabs((p.a - 1.0) - k) <= std::fmax(1e-4 * k, p.a_tol) && std::fabs(p.n - c.n) <= p.tol;
  }
  // The reversed clip-space depth w - z = k0 - k1 w.
  float K0() const { return float(a * n); }
  float K1() const { return float(a - 1.0); }
};

}  // namespace recomp::gfx
