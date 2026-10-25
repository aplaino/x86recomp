// The PowerPC floating-point unit, exactly: SoftFloat under FPSCR[RN] with
// tininess detected before rounding, the PowerPC's NaN rules applied here
// (the first NaN of FRA, FRB, FRC, quieted; the default NaN 0x7FF8...),
// and every FPSCR effect of the ISA: the exception bits with their invalid
// operation causes, FX, the VX and FEX summaries, FR, FI and FPRF.
//
// Single-precision arithmetic rounds the exact result of its (double)
// operands once, to single precision: the operation runs on float128 with
// round-to-odd, which keeps enough of the exact result for the final
// rounding to f32 under RN to be the correct one.
//
// An exception whose enable bit (VE OE UE ZE XE) is set changes results in
// ways not modelled yet; it is a fatal error. So is FPSCR[NI].
//
// RECOMP_PPC_MUTATION 4-6 plant known bugs here (ppc/lifted.h); the form
// test builds its own copy of this file for them.

#include <cstring>
#include <utility>

extern "C" {
#include "softfloat.h"
}

#include "ppc/fatal.h"
#include "ppc/lifted.h"

namespace recomp::ppc::lift {
namespace {

constexpr uint64_t kSign = 1ull << 63, kExp = 0x7FF0000000000000ull, kFrac = 0x000FFFFFFFFFFFFFull;
constexpr uint64_t kDefaultNaN = 0x7FF8000000000000ull, kQuiet = 1ull << 51;
constexpr uint32_t kVxAll = kVxsnan | kVxisi | kVxidi | kVxzdz | kVximz | kVxvc | kVxsoft | kVxsqrt | kVxcvi;
// The exception bits FX tracks (VX and FEX are summaries).
constexpr uint32_t kExceptions = kOx | kUx | kZx | kXx | kVxAll;

bool IsNaN(uint64_t v) { return (v & ~kSign) > kExp; }
bool IsSNaN(uint64_t v) { return IsNaN(v) && !(v & kQuiet); }
bool IsInf(uint64_t v) { return (v & ~kSign) == kExp; }
bool IsZero(uint64_t v) { return !(v & ~kSign); }
bool Neg(uint64_t v) { return v >> 63; }

// FPRF of a double result.
uint32_t Class(uint64_t v) {
  uint32_t c;
  if (IsNaN(v)) c = 0x11;
  else if (IsInf(v)) c = Neg(v) ? 0x09 : 0x05;
  else if (IsZero(v)) c = Neg(v) ? 0x12 : 0x02;
  else if (!(v & kExp)) c = Neg(v) ? 0x18 : 0x14;
  else c = Neg(v) ? 0x08 : 0x04;
  return c << 12;
}

// The summaries from the bits they summarise.
uint32_t Summaries(uint32_t f) {
  f &= ~(kVx | kFex);
  if (f & kVxAll) f |= kVx;
  if (((f & kVx) && (f & kVe)) || ((f & kOx) && (f & kOe)) || ((f & kUx) && (f & kUe)) ||
      ((f & kZx) && (f & kZe)) || ((f & kXx) && (f & kXe)))
    f |= kFex;
  return f;
}

// Raises exception bits: sticky, FX when any goes from 0 to 1.
uint32_t Raise(uint32_t f, uint32_t exc) {
  if (exc & ~f & kExceptions) f |= kFx;
  return Summaries(f | exc);
}

uint_fast8_t Mode(uint32_t fpscr) {
  static const uint_fast8_t kModes[4] = {softfloat_round_near_even, softfloat_round_minMag,
                                         softfloat_round_max, softfloat_round_min};
  return kModes[fpscr & kRn];
}

// Fatal if an exception about to be raised is enabled (or NI is set): the
// enabled semantics (results left in place, scaled exponents) are not
// modelled.
void CheckEnables(Ctx& c, uint32_t exc) {
  uint32_t f = c.fpscr;
  if (((exc & kVxAll) && (f & kVe)) || ((exc & kOx) && (f & kOe)) || ((exc & kUx) && (f & kUe)) ||
      ((exc & kZx) && (f & kZe)) || ((exc & kXx) && (f & kXe)))
    Fatal(&c, "enabled floating-point exception (FPSCR %08x, raising %08x) is not modelled", f, exc);
  if (f & kNi) Fatal(&c, "FPSCR[NI] (non-IEEE mode) is not modelled");
}

// The end of an arithmetic operation: exceptions raised, FR, FI, FPRF.
uint64_t Finish(Ctx& c, uint64_t r, uint32_t exc, bool fr, bool fi, bool fprf = true) {
  CheckEnables(c, exc);
  uint32_t f = c.fpscr & ~(kFr | kFi | (fprf ? kFprf : 0));
  if (fr) f |= kFr;
  if (fi) f |= kFi;
  if (fprf) f |= Class(r);
  c.fpscr = Raise(f, exc);
  return r;
}

uint32_t FromFlags(uint_fast8_t flags) {
  uint32_t exc = 0;
  if (flags & softfloat_flag_overflow) exc |= kOx;
  if (flags & softfloat_flag_underflow) exc |= kUx;
  if (flags & softfloat_flag_inexact) exc |= kXx;
  return exc;
}

// A single-precision value back in double format (exact).
uint64_t Widen(float32_t s) { return f32_to_f64(s).v; }
// A NaN result in single precision: quieted, its fraction cut to single's.
uint64_t SingleNaN(uint64_t v) { return (v | kQuiet) & ~0x1FFFFFFFull; }

float64_t F64(uint64_t v) { return float64_t{v}; }
float128_t F128(uint64_t v) { return f64_to_f128(F64(v)); }

// One operation, with its NaN and invalid cases already handled: the result
// in RN (double) or rounded once to single, and its SoftFloat flags.
uint64_t Compute(unsigned op, bool single, uint64_t a, uint64_t b, uint64_t cc, uint_fast8_t mode,
                 uint_fast8_t& flags) {
  softfloat_exceptionFlags = 0;
  softfloat_detectTininess = Mutated(5) ? softfloat_tininess_afterRounding
                                        : softfloat_tininess_beforeRounding;
  uint64_t r;
  if (Mutated(4) && single && op != kFRes) {
    // Double rounding: to double, then to single.
    r = Compute(op, false, a, b, cc, mode, flags);
    softfloat_roundingMode = mode;
    r = Widen(f64_to_f32(F64(r)));
    flags |= softfloat_exceptionFlags;
    return r;
  }
  if (!single) {
    softfloat_roundingMode = mode;
    switch (op) {
      case kFAdd: r = f64_add(F64(a), F64(b)).v; break;
      case kFSub: r = f64_sub(F64(a), F64(b)).v; break;
      case kFMul: r = f64_mul(F64(a), F64(cc)).v; break;
      case kFDiv: r = f64_div(F64(a), F64(b)).v; break;
      case kFSqrt: r = f64_sqrt(F64(b)).v; break;
      case kFRsqrte: r = f64_div(F64(0x3FF0000000000000ull), f64_sqrt(F64(b))).v; break;
      default: {
        // FMADD family: FRA * FRC + FRB, with FRB's sign flipped for the
        // subtracting forms; the negating forms negate after rounding.
        bool sub = op == kFMsub || op == kFNmsub;
        r = f64_mulAdd(F64(a), F64(cc), F64(sub ? b ^ kSign : b)).v;
        break;
      }
    }
    flags = softfloat_exceptionFlags;
    return r;
  }
  // Single: exact (or round-to-odd) in float128, then one rounding to f32.
  softfloat_roundingMode = softfloat_round_odd;
  float128_t x;
  switch (op) {
    case kFAdd: x = f128_add(F128(a), F128(b)); break;
    case kFSub: x = f128_sub(F128(a), F128(b)); break;
    case kFMul: x = f128_mul(F128(a), F128(cc)); break;
    case kFDiv: x = f128_div(F128(a), F128(b)); break;
    case kFSqrt: x = f128_sqrt(F128(b)); break;
    case kFRes: x = f128_div(F128(0x3FF0000000000000ull), F128(b)); break;
    default: {
      bool sub = op == kFMsub || op == kFNmsub;
      x = f128_add(f128_mul(F128(a), F128(cc)), F128(sub ? b ^ kSign : b));
      break;
    }
  }
  // An exact zero sum takes its sign from RN, which round-to-odd does not
  // know: redo it under RN (exact, so nothing else changes).
  if (!(x.v[1] & 0x7FFFFFFFFFFFFFFFull) && !x.v[0] && (op <= kFSub || op >= kFMadd)) {
    softfloat_roundingMode = mode;
    bool sub = op == kFMsub || op == kFNmsub;
    if (op == kFAdd) x = f128_add(F128(a), F128(b));
    else if (op == kFSub) x = f128_sub(F128(a), F128(b));
    else x = f128_add(f128_mul(F128(a), F128(cc)), F128(sub ? b ^ kSign : b));
  }
  softfloat_exceptionFlags = 0;
  softfloat_roundingMode = mode;
  r = Widen(f128_to_f32(x));
  flags = softfloat_exceptionFlags;
  return r;
}

}  // namespace

uint64_t FpArith(Ctx& c, unsigned op, bool single, uint64_t a, uint64_t b, uint64_t cc) {
  // The operands the operation reads, in the ISA's NaN priority.
  uint64_t ops[3];
  int n = 0;
  switch (op) {
    case kFAdd: case kFSub: case kFDiv: ops[n++] = a; ops[n++] = b; break;
    case kFMul: ops[n++] = a; ops[n++] = cc; break;
    case kFSqrt: case kFRes: case kFRsqrte: ops[n++] = b; break;
    default: ops[n++] = a; ops[n++] = b; ops[n++] = cc; break;
  }
  bool negate = op == kFNmadd || op == kFNmsub;
  uint32_t exc = 0;
  for (int k = 0; k < n; k++)
    if (IsSNaN(ops[k])) exc |= kVxsnan;
  if constexpr (Mutated(6))
    for (int k = 0; k < n / 2; k++) std::swap(ops[k], ops[n - 1 - k]);
  for (int k = 0; k < n; k++)
    if (IsNaN(ops[k])) {
      uint64_t r = ops[k] | kQuiet;
      return Finish(c, single ? SingleNaN(r) : r, exc, false, false);
    }

  // Invalid operations without a NaN operand: the default NaN.
  bool sub = op == kFSub || op == kFMsub || op == kFNmsub;
  switch (op) {
    case kFAdd: case kFSub:
      if (IsInf(a) && IsInf(b) && (Neg(a) != Neg(b)) != sub) exc |= kVxisi;
      break;
    case kFMul:
      if ((IsInf(a) && IsZero(cc)) || (IsZero(a) && IsInf(cc))) exc |= kVximz;
      break;
    case kFDiv:
      if (IsInf(a) && IsInf(b)) exc |= kVxidi;
      else if (IsZero(a) && IsZero(b)) exc |= kVxzdz;
      else if (IsZero(b) && !IsInf(a)) exc |= kZx;
      break;
    case kFSqrt:
      if (Neg(b) && !IsZero(b)) exc |= kVxsqrt;
      break;
    case kFRes: case kFRsqrte:
      if (op == kFRsqrte && Neg(b) && !IsZero(b)) exc |= kVxsqrt;
      else if (IsZero(b)) exc |= kZx;
      break;
    default:  // FMADD family
      if ((IsInf(a) && IsZero(cc)) || (IsZero(a) && IsInf(cc))) {
        exc |= kVximz;
      } else if ((IsInf(a) || IsInf(cc)) && IsInf(b)) {
        bool product_neg = Neg(a) != Neg(cc);
        if ((product_neg != Neg(b)) != sub) exc |= kVxisi;
      }
      break;
  }
  if (exc & kVxAll) return Finish(c, kDefaultNaN, exc, false, false);

  uint_fast8_t mode = Mode(c.fpscr), flags = 0;
  uint64_t r = Compute(op, single, a, b, cc, mode, flags);
  exc |= FromFlags(flags);
  bool fi = flags & softfloat_flag_inexact, fr = false;
  if (fi) {
    // FR: the rounding increased the magnitude (against the result
    // truncated toward zero).
    uint_fast8_t ignored;
    uint64_t t = Compute(op, single, a, b, cc, softfloat_round_minMag, ignored);
    fr = (r & ~kSign) != (t & ~kSign);
  }
  if (negate) r ^= kSign;
  return Finish(c, r, exc, fr, fi);
}

uint64_t Frsp(Ctx& c, uint64_t b) {
  if (IsNaN(b)) return Finish(c, SingleNaN(b), IsSNaN(b) ? kVxsnan : 0, false, false);
  softfloat_exceptionFlags = 0;
  softfloat_detectTininess = softfloat_tininess_beforeRounding;
  softfloat_roundingMode = Mode(c.fpscr);
  uint64_t r = Widen(f64_to_f32(F64(b)));
  uint_fast8_t flags = softfloat_exceptionFlags;
  bool fi = flags & softfloat_flag_inexact, fr = false;
  if (fi) {
    softfloat_roundingMode = softfloat_round_minMag;
    fr = (r & ~kSign) != (Widen(f64_to_f32(F64(b))) & ~kSign);
  }
  return Finish(c, r, FromFlags(flags), fr, fi);
}

uint64_t Fctiw(Ctx& c, unsigned mode, uint64_t b) {
  bool zero = mode & 1, dword = mode & 2;
  // Invalid: a NaN, or outside the target's range after rounding; the
  // result saturates (a NaN gives the most negative value).
  uint64_t lo = dword ? 0x8000000000000000ull : 0xFFFFFFFF80000000ull;
  uint64_t hi = dword ? 0x7FFFFFFFFFFFFFFFull : 0x000000007FFFFFFFull;
  if (IsNaN(b)) return Finish(c, lo, kVxcvi | (IsSNaN(b) ? kVxsnan : 0), false, false, false);
  softfloat_exceptionFlags = 0;
  uint_fast8_t rm = zero ? softfloat_round_minMag : Mode(c.fpscr);
  float64_t rounded = f64_roundToInt(F64(b), rm, true);
  bool fi = softfloat_exceptionFlags & softfloat_flag_inexact;
  // In range when the rounded value fits.
  double limit_hi = dword ? 9223372036854775808.0 : 2147483648.0;
  float64_t lim;
  std::memcpy(&lim.v, &limit_hi, 8);
  float64_t neg_lim{lim.v ^ kSign};
  if (!f64_lt(rounded, lim) || f64_lt(rounded, neg_lim))
    return Finish(c, Neg(b) ? lo : hi, kVxcvi, false, false, false);
  int64_t v = dword ? f64_to_i64(rounded, softfloat_round_minMag, false)
                    : int64_t(f64_to_i32(rounded, softfloat_round_minMag, false));
  bool fr = fi && (rounded.v & ~kSign) != (f64_roundToInt(F64(b), softfloat_round_minMag, false).v & ~kSign);
  // FCTIW leaves the word in the low half; the high half is undefined
  // (this keeps the sign extension of the word).
  return Finish(c, uint64_t(v), fi ? kXx : 0, fr, fi, false);
}

uint64_t Fcfid(Ctx& c, uint64_t b) {
  softfloat_exceptionFlags = 0;
  softfloat_roundingMode = Mode(c.fpscr);
  uint64_t r = i64_to_f64(int64_t(b)).v;
  bool fi = softfloat_exceptionFlags & softfloat_flag_inexact, fr = false;
  if (fi) {
    softfloat_roundingMode = softfloat_round_minMag;
    fr = (r & ~kSign) != (i64_to_f64(int64_t(b)).v & ~kSign);
  }
  return Finish(c, r, fi ? kXx : 0, fr, fi);
}

uint32_t Fcmp(Ctx& c, bool ordered, uint64_t a, uint64_t b) {
  uint32_t field;
  uint32_t exc = 0;
  if (IsNaN(a) || IsNaN(b)) {
    field = 1;
    bool snan = IsSNaN(a) || IsSNaN(b);
    if (snan) exc |= kVxsnan;
    // FCMPO: any NaN is invalid (VXVC); with an SNaN only if VE is clear.
    if (ordered && (!snan || !(c.fpscr & kVe))) exc |= kVxvc;
  } else if (f64_lt(F64(a), F64(b))) {
    field = 8;
  } else if (f64_lt(F64(b), F64(a))) {
    field = 4;
  } else {
    field = 2;
  }
  CheckEnables(c, exc);
  c.fpscr = Raise((c.fpscr & ~kFpcc) | (field << 12), exc);
  return field;
}

// Bit 20 is reserved: MTFSF and MTFSFI do not write it.
constexpr uint32_t kReserved = 1u << 11;

void Mtfsf(Ctx& c, uint32_t flm, uint64_t b) {
  uint32_t mask = 0;
  for (int k = 0; k < 8; k++)
    if (flm & (0x80u >> k)) mask |= 0xFu << (28 - 4 * k);
  mask &= ~kReserved;
  c.fpscr = Summaries((c.fpscr & ~mask) | (uint32_t(b) & mask));
}

void Mtfsfi(Ctx& c, unsigned field, unsigned imm) {
  uint32_t shift = 28 - 4 * field, mask = (0xFu << shift) & ~kReserved;
  c.fpscr = Summaries((c.fpscr & ~mask) | (((imm & 0xF) << shift) & mask));
}

void Mtfsb(Ctx& c, unsigned bit, bool v) {
  uint32_t m = 1u << (31 - bit);
  if (m & (kFex | kVx)) return;  // summaries cannot be set or cleared directly
  if (!v) c.fpscr = Summaries(c.fpscr & ~m);
  else c.fpscr = Raise(c.fpscr, m) | m;
  c.fpscr = Summaries(c.fpscr);
}

uint32_t Mcrfs(Ctx& c, unsigned field) {
  uint32_t shift = 28 - 4 * field, v = (c.fpscr >> shift) & 0xF;
  uint32_t clear = (0xFu << shift) & (kFx | kExceptions);
  c.fpscr = Summaries(c.fpscr & ~clear);
  return v;
}

uint64_t SingleToDouble(uint32_t w) {
  uint64_t sign = uint64_t(w >> 31) << 63;
  uint32_t exp = (w >> 23) & 0xFF, frac = w & 0x7FFFFF;
  if (exp == 0) {
    if (!frac) return sign;
    // Denormal: normalised in double format.
    int e = -126;
    while (!(frac & 0x800000)) {
      frac <<= 1;
      e--;
    }
    return sign | uint64_t(e + 1023) << 52 | uint64_t(frac & 0x7FFFFF) << 29;
  }
  // Normal, infinity and NaN alike: W0 W1 ~W1 ~W1 ~W1 W2..W31, then zeros.
  uint64_t w1 = (w >> 30) & 1;
  uint64_t top = uint64_t(w >> 30) << 62 | (w1 ? 0 : 7ull << 59);
  return top | uint64_t(w & 0x3FFFFFFF) << 29;
}

uint32_t DoubleToSingle(uint64_t d) {
  uint32_t exp = uint32_t((d >> 52) & 0x7FF);
  if (exp > 896 || !(d << 1)) return uint32_t((d >> 62) << 30 | ((d >> 29) & 0x3FFFFFFF));
  if (exp >= 874) {
    // Denormalised into single format, no rounding.
    uint64_t frac = (1ull << 52) | (d & kFrac);
    int e = int(exp) - 1023;
    while (e < -126) {
      frac >>= 1;
      e++;
    }
    return uint32_t(d >> 63) << 31 | uint32_t((frac >> 29) & 0x7FFFFF);
  }
  // Too small for single: the ISA leaves the word undefined; this keeps
  // the sign and gives zero.
  return uint32_t(d >> 63) << 31;
}

}  // namespace recomp::ppc::lift
