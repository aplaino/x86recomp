// SSE/SSE2 floating point and MXCSR (see sse.h).
//
// Lanes are raw bit patterns. NaN operands are handled here with the SSE
// rules (SDM vol. 1, "SSE and SSE2 response to NaN operands"): a single
// NaN operand is returned quieted; with two, the first source (the
// destination register) wins; an SNaN operand raises IE. Everything else
// goes to SoftFloat, whose 8086 specialization already matches x86 for
// non-NaN inputs: default NaN 0xFFC00000 / 0xFFF8000000000000, tininess
// after rounding, 0x80000000 for out-of-range conversions.

#include "runtime/sse.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

extern "C" {
#include "softfloat.h"
}

#include "runtime/log.h"

namespace recomp::sse {
namespace {

// MXCSR fields.
constexpr uint32_t kIE = 0x01, kDE = 0x02, kZE = 0x04, kOE = 0x08, kUE = 0x10,
                   kPE = 0x20, kDAZ = 0x40, kUM = 0x800, kFTZ = 0x8000;

// A lane format: float or double.
template <bool Dbl>
struct Fmt;
template <>
struct Fmt<false> {
  using U = uint32_t;
  static constexpr U kExp = 0x7F800000u, kFrac = 0x007FFFFFu, kQuiet = 0x00400000u;
};
template <>
struct Fmt<true> {
  using U = uint64_t;
  static constexpr U kExp = 0x7FF0000000000000ull, kFrac = 0x000FFFFFFFFFFFFFull,
                     kQuiet = 0x0008000000000000ull;
};

template <bool D>
bool IsNaN(typename Fmt<D>::U v) {
  return (v & Fmt<D>::kExp) == Fmt<D>::kExp && (v & Fmt<D>::kFrac);
}
template <bool D>
bool IsSNaN(typename Fmt<D>::U v) {
  return IsNaN<D>(v) && !(v & Fmt<D>::kQuiet);
}
template <bool D>
bool IsDenormal(typename Fmt<D>::U v) {
  return !(v & Fmt<D>::kExp) && (v & Fmt<D>::kFrac);
}
template <bool D>
typename Fmt<D>::U Quiet(typename Fmt<D>::U v) {
  return v | Fmt<D>::kQuiet;
}

uint8_t RoundingMode(uint32_t mxcsr) {
  static const uint8_t kModes[4] = {softfloat_round_near_even, softfloat_round_min,
                                    softfloat_round_max, softfloat_round_minMag};
  return kModes[(mxcsr >> 13) & 3];
}

uint32_t FromSf(uint_fast8_t f) {
  uint32_t r = 0;
  if (f & softfloat_flag_invalid) r |= kIE;
  if (f & softfloat_flag_infinite) r |= kZE;
  if (f & softfloat_flag_overflow) r |= kOE;
  if (f & softfloat_flag_underflow) r |= kUE;
  if (f & softfloat_flag_inexact) r |= kPE;
  return r;
}

// One instruction's MXCSR bookkeeping: rounding mode in, flags out.
struct Scope {
  Ctx& c;
  uint64_t va;
  uint32_t flags = 0;

  Scope(Ctx& cc, uint64_t v) : c(cc), va(v) {
    softfloat_roundingMode = RoundingMode(c.mxcsr);
  }
  // Denormals are zero (DAZ): a denormal operand is used as a zero of its
  // sign, raising nothing (SDM vol. 1, 10.2.3.4). Applied to every operand
  // before the operation looks at it, so it decides e.g. that a negative
  // denormal square root is valid and that a denormal divisor divides by
  // zero.
  template <bool D>
  typename Fmt<D>::U Daz(typename Fmt<D>::U v) const {
    if (!(c.mxcsr & kDAZ) || !IsDenormal<D>(v)) return v;
    return v & ~Fmt<D>::kFrac;
  }
  // Denormal operand (after Daz): DE.
  template <bool D>
  void Operand(typename Fmt<D>::U v) {
    if (IsDenormal<D>(v)) flags |= kDE;
  }
  void Begin() { softfloat_exceptionFlags = 0; }
  // Adds the operation's flags; true if it underflowed.
  bool End() {
    uint_fast8_t f = softfloat_exceptionFlags;
    flags |= FromSf(f);
    return f & softfloat_flag_underflow;
  }
  // Flush to zero (FTZ, underflow masked): a tiny result becomes a zero of
  // its sign, raising UE and PE (SDM vol. 1, 10.2.3.3). Tiny is after
  // rounding, as SoftFloat's 8086 build detects it; SoftFloat reports an
  // underflow only when the result is also inexact, so an exact denormal
  // result counts too. With underflow unmasked the result stands and #XM
  // is raised (Finish).
  template <bool D>
  typename Fmt<D>::U Flush(typename Fmt<D>::U r, bool underflow) {
    if ((c.mxcsr & (kFTZ | kUM)) != (kFTZ | kUM) || !(underflow || IsDenormal<D>(r))) return r;
    flags |= kUE | kPE;
    return r & ~(Fmt<D>::kExp | Fmt<D>::kFrac);
  }
  // Commits the flags; an unmasked one would have raised #XM instead.
  void Finish() {
    uint32_t unmasked = flags & ~(c.mxcsr >> 7) & 0x3F;
    if (unmasked)
      FatalF("sse: unmasked SIMD exception %02X at %08llX (#XM not modelled)",
             unmasked, (unsigned long long)va);
    c.mxcsr |= flags;
  }
};

template <bool D>
typename Fmt<D>::U ArithLane(Scope& sc, Op op, typename Fmt<D>::U a,
                             typename Fmt<D>::U b) {
  if (IsNaN<D>(a) || IsNaN<D>(b)) {
    if (IsSNaN<D>(a) || IsSNaN<D>(b)) sc.flags |= kIE;
    return IsNaN<D>(a) ? Quiet<D>(a) : Quiet<D>(b);
  }
  a = sc.Daz<D>(a);
  b = sc.Daz<D>(b);
  // Division by zero takes precedence over a denormal dividend (no DE).
  using U = typename Fmt<D>::U;
  constexpr U kSign = U(1) << (sizeof(U) * 8 - 1);
  if (!(op == kDiv && !(b & ~kSign))) sc.Operand<D>(a);
  sc.Operand<D>(b);
  sc.Begin();
  U r;
  if constexpr (D) {
    float64_t x{a}, y{b};
    switch (op) {
      case kAdd: r = f64_add(x, y).v; break;
      case kSub: r = f64_sub(x, y).v; break;
      case kMul: r = f64_mul(x, y).v; break;
      default: r = f64_div(x, y).v; break;
    }
  } else {
    float32_t x{a}, y{b};
    switch (op) {
      case kAdd: r = f32_add(x, y).v; break;
      case kSub: r = f32_sub(x, y).v; break;
      case kMul: r = f32_mul(x, y).v; break;
      default: r = f32_div(x, y).v; break;
    }
  }
  return sc.Flush<D>(r, sc.End());
}

// The host's IEEE arithmetic gives SoftFloat's results bit for bit where
// nothing exceptional can happen: MXCSR in its usual state (round to
// nearest, no DAZ/FTZ, every exception masked) with PE already set, both
// operands normal or zero, the result normal, or a zero that is exact (a
// sum of equals, or a product/quotient with a zero operand). The only flag
// such an operation can raise is PE, which is set already. Anything else
// returns false and takes the exact path.
bool FastMxcsr(uint32_t mxcsr) { return (mxcsr & 0xFFE0u) == 0x1FA0u; }

template <bool D>
bool NormalOrZero(typename Fmt<D>::U v) {
  const typename Fmt<D>::U e = v & Fmt<D>::kExp;
  return e != Fmt<D>::kExp && (e || !(v & Fmt<D>::kFrac));
}

template <bool D>
bool FastArithLane(Op op, typename Fmt<D>::U a, typename Fmt<D>::U b, typename Fmt<D>::U* out) {
  using U = typename Fmt<D>::U;
  using F = std::conditional_t<D, double, float>;
  constexpr U kSign = U(1) << (sizeof(U) * 8 - 1);
  if (!NormalOrZero<D>(a) || !NormalOrZero<D>(b)) return false;
  const F x = std::bit_cast<F>(a), y = std::bit_cast<F>(b);
  F r;
  switch (op) {
    case kAdd: r = x + y; break;
    case kSub: r = x - y; break;
    case kMul: r = x * y; break;
    default: r = x / y; break;
  }
  const U v = std::bit_cast<U>(r);
  const U e = v & Fmt<D>::kExp;
  if (e == Fmt<D>::kExp) return false;  // overflow, a quotient by zero
  if (!e) {
    if (v & Fmt<D>::kFrac) return false;  // denormal: underflow
    // A zero product or quotient of non-zero operands underflowed.
    if ((op == kMul || op == kDiv) && (a & ~kSign) && (b & ~kSign)) return false;
  }
  *out = v;
  return true;
}

template <bool D>
typename Fmt<D>::U SqrtLane(Scope& sc, typename Fmt<D>::U a) {
  if (IsNaN<D>(a)) {
    if (IsSNaN<D>(a)) sc.flags |= kIE;
    return Quiet<D>(a);
  }
  a = sc.Daz<D>(a);
  // A negative operand is invalid, which takes precedence over a denormal
  // one (no DE).
  using U = typename Fmt<D>::U;
  constexpr U kSign = U(1) << (sizeof(U) * 8 - 1);
  if (!(a & kSign) || !(a & ~kSign)) sc.Operand<D>(a);
  sc.Begin();
  U r;
  if constexpr (D)
    r = f64_sqrt(float64_t{a}).v;  // negative: default NaN, invalid
  else
    r = f32_sqrt(float32_t{a}).v;
  sc.End();
  return r;
}

template <bool D>
void SqrtT(Ctx& c, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  using U = typename Fmt<D>::U;
  Scope sc(c, va);
  Xmm r = d;
  for (int i = 0; i < lanes; i++) SetLane<U>(r, i, SqrtLane<D>(sc, Lane<U>(s, i)));
  sc.Finish();
  d = r;
}

template <bool D>
void ArithT(Ctx& c, Op op, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  using U = typename Fmt<D>::U;
  if (FastMxcsr(c.mxcsr)) {
    Xmm r = d;
    int i = 0;
    for (U v; i < lanes && FastArithLane<D>(op, Lane<U>(d, i), Lane<U>(s, i), &v); i++)
      SetLane<U>(r, i, v);
    if (i == lanes) {
      d = r;
      return;
    }
  }
  Scope sc(c, va);
  Xmm r = d;
  for (int i = 0; i < lanes; i++)
    SetLane<U>(r, i, ArithLane<D>(sc, op, Lane<U>(d, i), Lane<U>(s, i)));
  sc.Finish();
  d = r;
}

// Ordered comparison of two non-NaN values: -1, 0, 1. Zeros of either
// sign compare equal.
template <bool D>
int Order(typename Fmt<D>::U a, typename Fmt<D>::U b) {
  using U = typename Fmt<D>::U;
  constexpr U kSign = U(1) << (sizeof(U) * 8 - 1);
  if (!((a | b) & ~kSign)) return 0;
  bool na = a & kSign, nb = b & kSign;
  if (na != nb) return na ? -1 : 1;
  if (a == b) return 0;
  bool less = (a < b) != na;  // magnitude order flips for negatives
  return less ? -1 : 1;
}

template <bool D>
void CmpT(Ctx& c, int lanes, Xmm& d, const Xmm& s, uint8_t pred, uint64_t va) {
  using U = typename Fmt<D>::U;
  Scope sc(c, va);
  const bool signal_qnan = pred == 1 || pred == 2 || pred == 5 || pred == 6;
  Xmm r = d;
  for (int i = 0; i < lanes; i++) {
    U a = Lane<U>(d, i), b = Lane<U>(s, i);
    bool unord = IsNaN<D>(a) || IsNaN<D>(b);
    bool res;
    if (unord) {
      if (IsSNaN<D>(a) || IsSNaN<D>(b) || signal_qnan) sc.flags |= kIE;
      res = pred == 3 || pred == 4 || pred == 5 || pred == 6;
    } else {
      a = sc.Daz<D>(a);
      b = sc.Daz<D>(b);
      sc.Operand<D>(a);
      sc.Operand<D>(b);
      int o = Order<D>(a, b);
      switch (pred & 7) {
        case 0: res = o == 0; break;
        case 1: res = o < 0; break;
        case 2: res = o <= 0; break;
        case 3: res = false; break;
        case 4: res = o != 0; break;
        case 5: res = !(o < 0); break;
        case 6: res = !(o <= 0); break;
        default: res = true; break;
      }
    }
    SetLane<U>(r, i, res ? U(~U(0)) : U(0));
  }
  sc.Finish();
  d = r;
}

template <bool D>
int ComiT(Ctx& c, bool ordered, const Xmm& x, const Xmm& y, uint64_t va) {
  using U = typename Fmt<D>::U;
  Scope sc(c, va);
  U a = Lane<U>(x, 0), b = Lane<U>(y, 0);
  int r;
  if (IsNaN<D>(a) || IsNaN<D>(b)) {
    if (ordered || IsSNaN<D>(a) || IsSNaN<D>(b)) sc.flags |= kIE;
    r = 3;
  } else {
    a = sc.Daz<D>(a);
    b = sc.Daz<D>(b);
    sc.Operand<D>(a);
    sc.Operand<D>(b);
    int o = Order<D>(a, b);
    r = o > 0 ? 0 : o < 0 ? 1 : 2;
  }
  sc.Finish();
  return r;
}

template <bool D>
void MinMaxT(Ctx& c, bool max, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  using U = typename Fmt<D>::U;
  Scope sc(c, va);
  Xmm r = d;
  for (int i = 0; i < lanes; i++) {
    U a = Lane<U>(d, i), b = Lane<U>(s, i);
    U v = b;  // NaNs and equal values (zeros of either sign too) give s
    if (IsNaN<D>(a) || IsNaN<D>(b)) {
      sc.flags |= kIE;  // an ordered comparison: QNaNs are invalid too
    } else {
      a = sc.Daz<D>(a);
      b = sc.Daz<D>(b);
      v = b;
      sc.Operand<D>(a);
      sc.Operand<D>(b);
      int o = Order<D>(a, b);
      if (max ? o > 0 : o < 0) v = a;
    }
    SetLane<U>(r, i, v);
  }
  sc.Finish();
  d = r;
}

// One float <-> double conversion. NaNs keep their payload (truncated or
// extended) and are quieted, as SoftFloat's 8086 NaN conversion does.
template <bool FromDbl>
typename Fmt<!FromDbl>::U CvtLane(Scope& sc, typename Fmt<FromDbl>::U a) {
  if (!IsNaN<FromDbl>(a)) {
    a = sc.Daz<FromDbl>(a);
    sc.Operand<FromDbl>(a);
  }
  sc.Begin();
  typename Fmt<!FromDbl>::U r;
  if constexpr (FromDbl)
    r = f64_to_f32(float64_t{a}).v;
  else
    r = f32_to_f64(float32_t{a}).v;
  return sc.Flush<!FromDbl>(r, sc.End());
}

}  // namespace

void Misaligned(uint64_t a, uint64_t va, unsigned align) {
  FatalF("sse: misaligned %u-byte operand %08llX at %08llX (#GP)", align,
         (unsigned long long)a, (unsigned long long)va);
}

Xmm ArithModel(Ctx& c, Op op, bool dbl, int lanes, Xmm d, Xmm s, uint64_t va) {
  if (dbl)
    ArithT<true>(c, op, lanes, d, s, va);
  else
    ArithT<false>(c, op, lanes, d, s, va);
  return d;
}

Xmm CmpModel(Ctx& c, bool dbl, int lanes, Xmm d, Xmm s, uint8_t pred, uint64_t va) {
  if (dbl)
    CmpT<true>(c, lanes, d, s, pred, va);
  else
    CmpT<false>(c, lanes, d, s, pred, va);
  return d;
}

int ComiModel(Ctx& c, bool dbl, bool ordered, Xmm a, Xmm b, uint64_t va) {
  return dbl ? ComiT<true>(c, ordered, a, b, va) : ComiT<false>(c, ordered, a, b, va);
}

Xmm SqrtModel(Ctx& c, bool dbl, int lanes, Xmm d, Xmm s, uint64_t va) {
  if (dbl)
    SqrtT<true>(c, lanes, d, s, va);
  else
    SqrtT<false>(c, lanes, d, s, va);
  return d;
}

// For a positive normal x, the float nearest to y = 1/sqrt(x). A first
// guess from binary64 (sqrt, then a division: two roundings, then a
// third to binary32) is checked exactly and moved by an ulp if needed:
// r is correct iff y lies between the midpoints to its neighbours, i.e.
// lo^2 * x <= 1 <= hi^2 * x, evaluated in 128-bit integers. y is never a
// midpoint (it would be a dyadic rational with a 25-bit significand whose
// square's reciprocal is a float), so there are no ties.
uint32_t RsqrtLane(uint32_t x) {
  const uint32_t exp = (x >> 23) & 0xFF, frac = x & 0x7FFFFF;
  if (exp == 0xFF && frac) return x | 0x00400000u;     // NaN: quieted
  if (exp == 0) return (x & 0x80000000u) | 0x7F800000u;  // ±0, denormal: ±inf
  if (x & 0x80000000u) return 0xFFC00000u;               // negative: default NaN
  if (exp == 0xFF) return 0;                             // +inf: +0
  // The first guess on the host: binary64 sqrt and division and the
  // narrowing are IEEE's, correctly rounded to nearest even on both (the
  // host's FP environment is the default one), so they give the bits
  // SoftFloat did, at a fraction of the cost.
  float xf;
  std::memcpy(&xf, &x, sizeof xf);
  const float guess = float(1.0 / std::sqrt(double(xf)));
  uint32_t r;
  std::memcpy(&r, &guess, sizeof r);
  const unsigned __int128 mx = frac | 0x800000u;
  const int ex = int(exp) - 150;  // x = mx * 2^ex
  for (;;) {
    const uint32_t rexp = (r >> 23) & 0xFF;
    const unsigned __int128 mr = (r & 0x7FFFFF) | 0x800000u;
    const int er = int(rexp) - 150;  // r = mr * 2^er
    // Midpoints in units of 2^(er-2): the one below is closer when r's
    // significand is a power of two (the next float down has half the ulp).
    const unsigned __int128 lo = 4 * mr - (mr == 0x800000u ? 1 : 2), hi = 4 * mr + 2;
    const int shift = -(2 * (er - 2) + ex);  // 1 = 2^shift in those units
    const unsigned __int128 one_scaled = static_cast<unsigned __int128>(1) << shift;
    if (lo * lo * mx > one_scaled)
      r--;
    else if (hi * hi * mx < one_scaled)
      r++;
    else
      return r;
  }
}

// The float nearest to 1/x (one rounding of the exact quotient). Like the
// CPU, a zero or denormal operand gives the signed infinity and a result
// below the normal range the signed zero.
uint32_t RcpLane(uint32_t x) {
  const uint32_t exp = (x >> 23) & 0xFF, frac = x & 0x7FFFFF, sign = x & 0x80000000u;
  if (exp == 0xFF && frac) return x | 0x00400000u;  // NaN: quieted
  if (exp == 0) return sign | 0x7F800000u;           // ±0, denormal: ±inf
  if (exp == 0xFF) return sign;                      // ±inf: ±0
  // IEEE binary32 division, correctly rounded to nearest even on the host
  // as in SoftFloat (see RsqrtLane).
  float xf;
  std::memcpy(&xf, &x, sizeof xf);
  const float q = 1.0f / xf;
  uint32_t r;
  std::memcpy(&r, &q, sizeof r);
  if (((r >> 23) & 0xFF) == 0) return sign;  // denormal result: flushed
  return r;
}

uint64_t Cvtsi2sdLane(int32_t v) { return i32_to_f64(v).v; }

namespace {
// Lane i of a float-to-integer conversion's source, after DAZ.
void DazLane(const Scope& sc, bool dbl, Xmm& s, int i) {
  if (dbl)
    SetLane<uint64_t>(s, i, sc.Daz<true>(Lane<uint64_t>(s, i)));
  else
    SetLane<uint32_t>(s, i, sc.Daz<false>(Lane<uint32_t>(s, i)));
}
}  // namespace

uint32_t CvtToI32Model(Ctx& c, bool dbl, Xmm s, bool truncate, uint64_t va) {
  Scope sc(c, va);
  DazLane(sc, dbl, s, 0);
  uint8_t mode = truncate ? uint8_t(softfloat_round_minMag) : RoundingMode(c.mxcsr);
  sc.Begin();
  int32_t r = dbl ? f64_to_i32(float64_t{Lane<uint64_t>(s, 0)}, mode, true)
                  : f32_to_i32(float32_t{Lane<uint32_t>(s, 0)}, mode, true);
  sc.End();
  sc.Finish();
  return uint32_t(r);
}

uint64_t CvtToI64(Ctx& c, bool dbl, Xmm s, bool truncate, uint64_t va) {
  Scope sc(c, va);
  DazLane(sc, dbl, s, 0);
  uint8_t mode = truncate ? uint8_t(softfloat_round_minMag) : RoundingMode(c.mxcsr);
  sc.Begin();
  int64_t r = dbl ? f64_to_i64(float64_t{Lane<uint64_t>(s, 0)}, mode, true)
                  : f32_to_i64(float32_t{Lane<uint32_t>(s, 0)}, mode, true);
  sc.End();
  sc.Finish();
  return uint64_t(r);
}

Xmm MinMaxModel(Ctx& c, bool max, bool dbl, int lanes, Xmm d, Xmm s, uint64_t va) {
  if (dbl)
    MinMaxT<true>(c, max, lanes, d, s, va);
  else
    MinMaxT<false>(c, max, lanes, d, s, va);
  return d;
}

Xmm CvtFloatModel(Ctx& c, bool to_dbl, int lanes, Xmm d, Xmm s, uint64_t va) {
  Scope sc(c, va);
  Xmm r = d;
  if (to_dbl) {
    for (int i = 0; i < lanes; i++)
      SetLane<uint64_t>(r, i, CvtLane<false>(sc, Lane<uint32_t>(s, i)));
  } else {
    for (int i = 0; i < lanes; i++)
      SetLane<uint32_t>(r, i, CvtLane<true>(sc, Lane<uint64_t>(s, i)));
    if (lanes == 2) SetLane<uint64_t>(r, 1, 0);  // CVTPD2PS zeroes the top
  }
  sc.Finish();
  return r;
}

Xmm CvtIntToFModel(Ctx& c, bool dbl, bool src64, uint64_t v, Xmm d, uint64_t va) {
  Scope sc(c, va);
  sc.Begin();
  if (dbl)
    SetLane<uint64_t>(d, 0, src64 ? i64_to_f64(int64_t(v)).v : i32_to_f64(int32_t(v)).v);
  else
    SetLane<uint32_t>(d, 0, src64 ? i64_to_f32(int64_t(v)).v : i32_to_f32(int32_t(v)).v);
  sc.End();
  sc.Finish();
  return d;
}

Xmm CvtToDqModel(Ctx& c, bool dbl, bool truncate, Xmm s, uint64_t va) {
  Scope sc(c, va);
  const uint8_t mode = truncate ? uint8_t(softfloat_round_minMag) : RoundingMode(c.mxcsr);
  Xmm r{};
  sc.Begin();
  for (int i = 0; i < (dbl ? 2 : 4); i++) {
    DazLane(sc, dbl, s, i);
    int32_t v = dbl ? f64_to_i32(float64_t{Lane<uint64_t>(s, i)}, mode, true)
                    : f32_to_i32(float32_t{Lane<uint32_t>(s, i)}, mode, true);
    SetLane<int32_t>(r, i, v);
  }
  sc.End();
  sc.Finish();
  return r;
}

Xmm Cvtdq2psModel(Ctx& c, Xmm s, uint64_t va) {
  Scope sc(c, va);
  Xmm r;
  sc.Begin();
  for (int i = 0; i < 4; i++) SetLane<uint32_t>(r, i, i32_to_f32(Lane<int32_t>(s, i)).v);
  sc.End();
  sc.Finish();
  return r;
}

void (*g_ldmxcsr_note)(uint32_t v, uint64_t va) = nullptr;

void Ldmxcsr(Ctx& c, uint32_t v, uint64_t va) {
  if (g_ldmxcsr_note) g_ldmxcsr_note(v, va);
  if (v & 0xFFFF0000u)
    FatalF("sse: ldmxcsr %08X sets reserved bits at %08llX (#GP)", v,
           (unsigned long long)va);
  c.mxcsr = v;
}


// ---- RECOMP_SSE_HOST with FTZ and DAZ (sse.h, ArithFtzDaz) ----------------

namespace {

constexpr uint32_t kUePe = 0x30;  // MXCSR UE | PE
constexpr uint32_t kDaz = 0x40;

// The lane with a denormal made a zero of its sign if `daz` (DAZ).
template <bool D>
typename fast::Bits<D>::U Daz(typename fast::Bits<D>::U v, bool daz) {
  using B = fast::Bits<D>;
  return daz && (v & B::kExp) == 0 ? v & ~(B::kExp | B::kFrac) : v;
}

// The first `lanes` lanes of x, DAZ'd if `daz`; the others as they are
// (scalar forms keep d's upper lanes).
template <bool D>
Xmm DazLanes(int lanes, Xmm x, bool daz) {
  using U = typename fast::Bits<D>::U;
  if (daz)
    for (int i = 0; i < lanes; i++) SetLane<U>(x, i, Daz<D>(Lane<U>(x, i), true));
  return x;
}

enum class Ftz { kKeep, kFlushed, kModel };

// FTZ on a host result: a denormal becomes a zero of its sign; the
// smallest normal magnitude is left to the model (see sse.h).
template <bool D>
Ftz FlushResult(typename fast::Bits<D>::U& v) {
  using B = fast::Bits<D>;
  using U = typename B::U;
  const U mag = v & (B::kExp | B::kFrac);
  constexpr U kMinNormal = B::kFrac + 1;
  if (mag == kMinNormal) return Ftz::kModel;
  if ((v & B::kExp) == 0 && mag != 0) {
    v &= ~(B::kExp | B::kFrac);
    return Ftz::kFlushed;
  }
  return Ftz::kKeep;
}

template <bool D>
bool FiniteNonzero(typename fast::Bits<D>::U v) {
  using B = fast::Bits<D>;
  return (v & (B::kExp | B::kFrac)) != 0 && (v & B::kExp) != B::kExp;
}

template <bool D>
[[gnu::always_inline]] inline bool ArithFtzDazT(Ctx& c, Op op, int lanes, Xmm& d,
                                                const Xmm& s) {
  using U = typename fast::Bits<D>::U;
  using F = typename fast::Bits<D>::F;
  Xmm r = d;
  bool flushed = false;
  const bool daz = c.mxcsr & kDaz;
  for (int i = 0; i < lanes; i++) {
    const U xb = Daz<D>(Lane<U>(d, i), daz), yb = Daz<D>(Lane<U>(s, i), daz);
    F x, y;
    std::memcpy(&x, &xb, sizeof x);
    std::memcpy(&y, &yb, sizeof y);
    F v;
    switch (op) {
      case kAdd: v = x + y; break;
      case kSub: v = x - y; break;
      case kMul: v = x * y; break;
      default: v = x / y; break;
    }
    U vb;
    std::memcpy(&vb, &v, sizeof vb);
    // A sum or difference is zero only exactly (a nonzero one is at least
    // the smallest denormal); a product or quotient of nonzero finite
    // operands only by underflow.
    if ((op == kMul || op == kDiv) && (vb & ~(U(1) << (sizeof(U) * 8 - 1))) == 0 &&
        FiniteNonzero<D>(xb) && FiniteNonzero<D>(yb))
      return false;
    switch (FlushResult<D>(vb)) {
      case Ftz::kModel: return false;
      case Ftz::kFlushed: flushed = true; break;
      case Ftz::kKeep: break;
    }
    SetLane<U>(r, i, vb);
  }
  if (flushed) c.mxcsr |= kUePe;
  d = r;
  return true;
}

}  // namespace

bool ArithFtzDaz(Ctx& c, Op op, bool dbl, int lanes, Xmm& d, const Xmm& s) {
  return dbl ? ArithFtzDazT<true>(c, op, lanes, d, s) : ArithFtzDazT<false>(c, op, lanes, d, s);
}

template <bool D, Op kOp, int kLanes>
bool ArithFtz(Ctx& c, Xmm& d, const Xmm& s) {
  return ArithFtzDazT<D>(c, kOp, kLanes, d, s);
}
#define RECOMP_SSE_FTZ_INST(D, L)                             \
  template bool ArithFtz<D, kAdd, L>(Ctx&, Xmm&, const Xmm&); \
  template bool ArithFtz<D, kSub, L>(Ctx&, Xmm&, const Xmm&); \
  template bool ArithFtz<D, kMul, L>(Ctx&, Xmm&, const Xmm&); \
  template bool ArithFtz<D, kDiv, L>(Ctx&, Xmm&, const Xmm&);
RECOMP_SSE_FTZ_INST(false, 1)
RECOMP_SSE_FTZ_INST(false, 4)
RECOMP_SSE_FTZ_INST(true, 1)
RECOMP_SSE_FTZ_INST(true, 2)
#undef RECOMP_SSE_FTZ_INST

// Compares and MIN/MAX have no rounded result: DAZ'd operands, then the
// host's rule (MIN/MAX return the DAZ'd lane, as the operation sees it).
void CmpDaz(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint8_t pred) {
  const bool daz = c.mxcsr & kDaz;
  if (dbl) {
    Xmm a = DazLanes<true>(lanes, d, daz);
    fast::HostCmp<true>(lanes, a, DazLanes<true>(lanes, s, daz), pred);
    d = a;
  } else {
    Xmm a = DazLanes<false>(lanes, d, daz);
    fast::HostCmp<false>(lanes, a, DazLanes<false>(lanes, s, daz), pred);
    d = a;
  }
}

void MinMaxDaz(Ctx& c, bool max, bool dbl, int lanes, Xmm& d, const Xmm& s) {
  const bool daz = c.mxcsr & kDaz;
  if (dbl) {
    Xmm a = DazLanes<true>(lanes, d, daz);
    fast::HostMinMax<true>(max, lanes, a, DazLanes<true>(lanes, s, daz));
    d = a;
  } else {
    Xmm a = DazLanes<false>(lanes, d, daz);
    fast::HostMinMax<false>(max, lanes, a, DazLanes<false>(lanes, s, daz));
    d = a;
  }
}

// The square root of a normal or denormal number is normal: only DAZ
// applies.
void SqrtDaz(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s) {
  const bool daz = c.mxcsr & kDaz;
  Xmm r = d;
  for (int i = 0; i < lanes; i++) {
    if (dbl) {
      const uint64_t b = Daz<true>(Lane<uint64_t>(s, i), daz);
      double x;
      std::memcpy(&x, &b, sizeof x);
      SetLane<double>(r, i, std::sqrt(x));
    } else {
      const uint32_t b = Daz<false>(Lane<uint32_t>(s, i), daz);
      float x;
      std::memcpy(&x, &b, sizeof x);
      SetLane<float>(r, i, std::sqrt(x));
    }
  }
  d = r;
}

// Widening a DAZ'd float is exact; narrowing flushes as ArithFtzDaz does.
bool CvtFloatFtzDaz(Ctx& c, bool to_dbl, int lanes, Xmm& d, const Xmm& s) {
  Xmm r = d;
  bool flushed = false;
  const bool daz = c.mxcsr & kDaz;
  if (to_dbl) {
    for (int i = 0; i < lanes; i++) {
      const uint32_t b = Daz<false>(Lane<uint32_t>(s, i), daz);
      float x;
      std::memcpy(&x, &b, sizeof x);
      SetLane<double>(r, i, double(x));
    }
  } else {
    for (int i = 0; i < lanes; i++) {
      const uint64_t b = Daz<true>(Lane<uint64_t>(s, i), daz);
      double x;
      std::memcpy(&x, &b, sizeof x);
      const float v = float(x);
      uint32_t vb;
      std::memcpy(&vb, &v, sizeof vb);
      if ((vb & 0x7FFFFFFFu) == 0 && FiniteNonzero<true>(b)) return false;
      switch (FlushResult<false>(vb)) {
        case Ftz::kModel: return false;
        case Ftz::kFlushed: flushed = true; break;
        case Ftz::kKeep: break;
      }
      SetLane<uint32_t>(r, i, vb);
    }
    if (lanes == 2) SetLane<uint64_t>(r, 1, 0);
  }
  if (flushed) c.mxcsr |= kUePe;
  d = r;
  return true;
}

}  // namespace recomp::sse
