// VMX floating point, lane by lane on Berkeley SoftFloat: round to nearest
// even always, no exceptions, the first NaN of the operands (A, B, C)
// quieted, the default NaN 0x7FC00000 for invalid operations, and in
// non-Java mode (VSCR[NJ]) denormal operands and results flushed to zero
// of the same sign. VEXPTEFP and VLOGEFP are estimates (the CPU's own);
// these compute 2^x and log2 x accurately in double, then round once.
//
// RECOMP_PPC_MUTATION 7-9 plant known bugs here and in ppc/vmx.h.

extern "C" {
#include "softfloat.h"
}

#include "ppc/lifted.h"

namespace recomp::ppc::lift {
namespace {

constexpr uint32_t kSign = 0x80000000u, kExp = 0x7F800000u, kQuiet = 0x00400000u;
constexpr uint32_t kDefaultNaN = 0x7FC00000u;

bool IsNaN(uint32_t v) { return (v & ~kSign) > kExp; }
bool IsDenormal(uint32_t v) { return !(v & kExp) && (v & ~kSign & 0x7FFFFF); }

float32_t F32(uint32_t v) { return float32_t{v}; }
float64_t F64(uint64_t v) { return float64_t{v}; }

class Lanes {
 public:
  explicit Lanes(Ctx& c) : nj_(c.vscr & kVscrNj) {
    softfloat_roundingMode = softfloat_round_near_even;
    softfloat_detectTininess = softfloat_tininess_beforeRounding;
  }
  uint32_t In(uint32_t v) const { return nj_ && !Mutated(8) && IsDenormal(v) ? v & kSign : v; }
  uint32_t Out(uint32_t v) const { return nj_ && IsDenormal(v) ? v & kSign : v; }

 private:
  bool nj_;
};

// 2^x and log2 x for the estimates, in double: exact for the integer
// powers, otherwise within a few double ulps.
float64_t Mul(float64_t a, float64_t b) { return f64_mul(a, b); }
float64_t Add(float64_t a, float64_t b) { return f64_add(a, b); }
float64_t D(int64_t v) { return i64_to_f64(v); }
float64_t Bits(uint64_t v) { return F64(v); }

uint32_t Exp2(uint32_t x) {
  float64_t v = f32_to_f64(F32(x));
  if (f64_le(Bits(0x4060000000000000ull), v)) return kExp;  // >= 128: +inf
  if (f64_lt(v, Bits(0xC062C00000000000ull))) return 0;     // < -150: +0
  float64_t n = f64_roundToInt(v, softfloat_round_min, false);
  float64_t f = f64_sub(v, n);  // [0, 1)
  // 2^f = e^(f ln 2), by its Taylor series (f ln 2 < 0.7: 20 terms).
  float64_t t = Mul(f, Bits(0x3FE62E42FEFA39EFull)), term = D(1), sum = D(1);
  for (int k = 1; k <= 20; k++) {
    term = f64_div(Mul(term, t), D(k));
    sum = Add(sum, term);
  }
  int64_t e = f64_to_i64(n, softfloat_round_minMag, false);
  // Scale by 2^e (an exact exponent adjustment while in double's range).
  uint64_t bits = sum.v + (uint64_t(e) << 52);
  return f64_to_f32(F64(bits)).v;
}

uint32_t Log2(uint32_t x) {
  if (x & kSign) return x & ~kSign ? kDefaultNaN : 0xFF800000u;  // < 0: NaN; -0: -inf
  if (!x) return 0xFF800000u;
  if (x == kExp) return kExp;
  int e = int((x >> 23) & 0xFF) - 127;
  uint32_t m = x & 0x7FFFFF;
  if (e == -127) {  // denormal: normalise
    e = -126;
    while (!(m & 0x800000)) {
      m <<= 1;
      e--;
    }
    m &= 0x7FFFFF;
  }
  // log2(1 + m) = 2 atanh(s) / ln 2 with s = m / (2 + m): the series in s^2.
  float64_t mant = f32_to_f64(F32(0x3F800000u | m));
  float64_t s = f64_div(f64_sub(mant, D(1)), f64_add(mant, D(1)));
  float64_t s2 = Mul(s, s), term = s, sum = s;
  for (int k = 3; k <= 61; k += 2) {
    term = Mul(term, s2);
    sum = Add(sum, f64_div(term, D(k)));
  }
  float64_t lg = f64_div(Mul(sum, D(2)), Bits(0x3FE62E42FEFA39EFull));
  return f64_to_f32(Add(lg, D(e))).v;
}

}  // namespace

Vec VmxFloat(Ctx& c, unsigned op, const Vec& a, const Vec& b, const Vec& cc, uint32_t imm) {
  Lanes l(c);
  Vec r;
  for (int k = 0; k < 4; k++) {
    uint32_t x = l.In(VGet<uint32_t>(a, k)), y = l.In(VGet<uint32_t>(b, k)), z = l.In(VGet<uint32_t>(cc, k));
    uint32_t out = 0;
    // NaN operands, in the order A, B, C of the operation's own operands.
    auto nan = [&](std::initializer_list<uint32_t> ops, uint32_t& res) {
      for (uint32_t v : ops)
        if (IsNaN(v)) {
          res = v | kQuiet;
          return true;
        }
      return false;
    };
    switch (op) {
      case kVaddfp: case kVsubfp:
        if (!nan({x, y}, out)) {
          float32_t s = op == kVaddfp ? f32_add(F32(x), F32(y)) : f32_sub(F32(x), F32(y));
          out = IsNaN(s.v) ? kDefaultNaN : s.v;
        }
        break;
      case kVmaddfp: case kVnmsubfp:
        // vD = vA * vC + vB (vnmsubfp: -(vA * vC - vB)), fused.
        if (!nan({x, y, z}, out)) {
          float32_t s = f32_mulAdd(F32(x), F32(z), F32(op == kVnmsubfp ? y ^ kSign : y));
          out = IsNaN(s.v) ? kDefaultNaN : (op == kVnmsubfp ? s.v ^ kSign : s.v);
        }
        break;
      case kVmaxfp: case kVminfp:
        if (!nan({x, y}, out)) {
          bool lt = f32_lt(F32(x), F32(y));
          if (!lt && !f32_lt(F32(y), F32(x)))  // equal: +0 for max, -0 for min
            out = op == kVmaxfp ? (x & y) : (x | y);
          else
            out = (op == kVmaxfp) != lt ? x : y;
        }
        break;
      case kVrefp:
        if (!nan({y}, out)) out = f32_div(F32(0x3F800000u), F32(y)).v;
        break;
      case kVrsqrtefp:
        if (!nan({y}, out)) {
          float32_t s = f32_sqrt(F32(y));
          out = IsNaN(s.v) ? kDefaultNaN : f32_div(F32(0x3F800000u), s).v;
        }
        break;
      case kVexptefp:
        if (!nan({y}, out)) out = Exp2(y);
        break;
      case kVlogefp:
        if (!nan({y}, out)) out = Log2(y);
        break;
      case kVrfin: case kVrfiz: case kVrfip: case kVrfim: {
        static const uint_fast8_t kMode[4] = {softfloat_round_near_even, softfloat_round_minMag,
                                              softfloat_round_max, softfloat_round_min};
        if (!nan({y}, out)) out = f32_roundToInt(F32(y), kMode[op - kVrfin], false).v;
        break;
      }
      // Integers to float, scaled by 2^-UIMM (exact once converted).
      case kVcfux: case kVcfsx: {
        float32_t f = op == kVcfux ? ui32_to_f32(VGet<uint32_t>(b, k)) : i32_to_f32(VGet<int32_t>(b, k));
        if (f.v & kExp) f.v -= (imm & 31) << 23;  // exponent adjust: never denormal here
        out = f.v;
        break;
      }
      // Float times 2^UIMM to integer, toward zero, saturating (a NaN is 0).
      case kVctuxs: case kVctsxs: {
        bool sat = false;
        bool u = op == kVctuxs;
        if (IsNaN(y)) {
          out = 0;  // not a saturation: SAT stays (as QEMU's 970; a console check)
        } else {
          float64_t v = f32_to_f64(F32(y));
          v = f64_mul(v, F64(uint64_t(1023 + (imm & 31)) << 52));
          float64_t t = f64_roundToInt(v, softfloat_round_minMag, false);
          float64_t hi = u ? Bits(0x41F0000000000000ull) : Bits(0x41E0000000000000ull);  // 2^32, 2^31
          float64_t lo = u ? D(0) : Bits(0xC1E0000000000000ull);
          if (!f64_lt(t, hi)) {
            out = u ? 0xFFFFFFFFu : 0x7FFFFFFFu;
            sat = true;
          } else if (f64_lt(t, lo)) {
            out = u ? 0 : 0x80000000u;
            sat = true;
          } else {
            out = u ? uint32_t(f64_to_ui64(t, softfloat_round_minMag, false))
                    : uint32_t(f64_to_i32(t, softfloat_round_minMag, false));
          }
        }
        if (sat) c.vscr |= kVscrSat;
        VPut<uint32_t>(r, k, out);
        continue;
      }
      case kVcmpeqfp: out = f32_eq(F32(x), F32(y)) ? ~0u : 0u; VPut<uint32_t>(r, k, out); continue;
      case kVcmpgefp: out = f32_le(F32(y), F32(x)) ? ~0u : 0u; VPut<uint32_t>(r, k, out); continue;
      case kVcmpgtfp: out = f32_lt(F32(y), F32(x)) ? ~0u : 0u; VPut<uint32_t>(r, k, out); continue;
      case kVcmpbfp: {
        // Bit 0: not (a <= b); bit 1: not (a >= -b). A NaN fails both.
        bool le = f32_le(F32(x), F32(y)), ge = f32_le(F32(y ^ kSign), F32(x));
        out = (le ? 0 : kSign) | (ge ? 0 : 0x40000000u);
        VPut<uint32_t>(r, k, out);
        continue;
      }
      default:
        Unimplemented(c, 0, "VMX operation");
    }
    VPut<uint32_t>(r, k, l.Out(out));
  }
  return r;
}

}  // namespace recomp::ppc::lift
