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
//   - Clears to z clear to 1 - z; DEPTHBIAS and SLOPESCALEDEPTHBIAS are
//     negated (the bias moves the stored value the other way).
//   - Pixel shaders: depth they write (oDepth) is stored as 1 - depth, and
//     what they sample from a reversed depth texture (INTZ: the scene's
//     depth read by lighting, fog, depth of field, soft particles) is read
//     as 1 - the value stored.
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
//     after a clear of the whole buffer sets them (the game's own near and
//     far planes where it gives them and they agree: Device::
//     SetDepthFromW), the next ones use them if their own projection is
//     the same (within the matrix's rounding), so their order is the
//     order of their w whatever their own matrices' rounding. The others
//     keep w - z, which stores the same values for the same projection:
//     nothing reads the buffer differently.

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

// A world-view-projection's depth as a function of w: z = a (w - n).
struct WProjection {
  double a = 1, n = 0;
  double tol = 0;  // how far n may be off: the matrix's float rounding
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
  return true;
}

// The perspective of planes n, f (D3D's: depth 0 at n, 1 at f).
inline WProjection PerspectiveOfPlanes(double n, double f) { return {f / (f - n), n, 0}; }

// The projection a depth buffer's content is drawn from w with: one per
// content, so the order of those draws is that of their w.
struct WDepth {
  bool fresh = true;    // cleared whole since the last draw from w
  double a = 1, n = 0;  // the content's

  // Whether a draw of projection p is drawn from w here. The first after
  // a clear sets the content's projection: `exact`'s if given (the game's
  // own planes, not rounded as a matrix's are) and p agrees with it.
  bool Use(const WProjection& p, const WProjection* exact = nullptr) {
    if (fresh) {
      fresh = false;
      const bool take = exact && Same(*exact, p);
      a = take ? exact->a : p.a;
      n = take ? exact->n : p.n;
      return true;
    }
    return Same({a, n, 0}, p);
  }
  static bool Same(const WProjection& c, const WProjection& p) {
    return std::fabs(p.a - c.a) <= 1e-6 * c.a && std::fabs(p.n - c.n) <= p.tol;
  }
  // The reversed clip-space depth w - z = k0 - k1 w.
  float K0() const { return float(a * n); }
  float K1() const { return float(a - 1.0); }
};

}  // namespace recomp::gfx
