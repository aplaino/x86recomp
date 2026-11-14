// The FPSCR bits the form test does not compare where QEMU departs from
// the ISA (lift_forms_test.cpp, UndefinedIn), checked against the ISA's
// definitions instead, with exact arithmetic:
// - FI and FR after arithmetic, FRSP, FCFID and FCTIW(Z)/FCTID(Z): FI when
//   the result differs from the exact one, FR when its magnitude is the
//   larger (the rounding incremented the fraction). The exact value is
//   integer arithmetic on the operands' significands, not SoftFloat.
// - FX: set only when an exception bit goes from 0 to 1.
// - CR1 of the record forms: FX FEX VX OX after the instruction (each of
//   the fixture's opcode 59/63 forms with Rc, as lifted).
// - FCMPU, FCMPO: only FPCC changes; FI, FR and FPRF's C bit stay.
// - FRES: FPRF the class of its single-precision result.
// Built with planted bugs (RECOMP_PPC_MUTATION 13-16, ppc/fp.cpp) that it
// must catch: QEMU's own behaviour in each of these bits.
//   ppc_fpscr_test

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lift_forms.h"
#include "ppc/lifted.h"

namespace recomp::ppc {
// The record forms never trap, call or read the time base.
void Sc(Ctx&, uint64_t va) { std::fprintf(stderr, "sc in a form at %llx\n", (unsigned long long)va), std::abort(); }
void Trap(Ctx&, uint32_t, uint64_t va) { std::fprintf(stderr, "trap in a form at %llx\n", (unsigned long long)va), std::abort(); }
uint64_t TimeBase(Ctx&) { return 0; }
void Import(Ctx&, const char*, uint32_t, uint64_t va) { std::fprintf(stderr, "import in a form at %llx\n", (unsigned long long)va), std::abort(); }
}  // namespace recomp::ppc

namespace {

using namespace recomp::ppc;
using namespace recomp::ppc::lift;
using u128 = unsigned __int128;

constexpr uint32_t kC = 1u << 16;  // FPRF's C bit
constexpr uint32_t kExceptions = kOx | kUx | kZx | kXx | kVxsnan | kVxisi | kVxidi | kVxzdz | kVximz |
                                 kVxvc | kVxsoft | kVxsqrt | kVxcvi;

int failures = 0;
const char* g_case = "";

void Check(bool ok, const char* what, uint32_t fpscr_in, uint32_t fpscr_out) {
  if (ok) return;
  if (failures++ < 20)
    std::printf("FAIL %s: %s (fpscr in %08x, out %08x)\n", g_case, what, fpscr_in, fpscr_out);
}

uint64_t g_rng = 0x2545F4914F6CDD1Dull;
uint64_t Next() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

// A finite double of either sign with its unbiased exponent in [lo, hi];
// its fraction is often short, so that some results are exact.
uint64_t RandomDouble(int lo, int hi, bool positive = false) {
  uint64_t frac = Next() & 0x000FFFFFFFFFFFFFull;
  if (Next() % 3 == 0) frac &= ~0ull << (52 - Next() % 12);
  int e = lo + int(Next() % uint64_t(hi - lo + 1));
  uint64_t sign = positive ? 0 : Next() & (1ull << 63);
  return sign | uint64_t(e + 1023) << 52 | frac;
}

// An FPSCR to start from: random sticky and status bits and rounding mode,
// no enables (an enabled exception is fatal in the model) and NI clear.
uint32_t RandomFpscr() {
  uint32_t keep = kFx | kExceptions | kFr | kFi | kFprf | kRn;
  uint32_t f = uint32_t(Next()) & keep;
  if (f & (kVxsnan | kVxisi | kVxidi | kVxzdz | kVximz | kVxvc | kVxsoft | kVxsqrt | kVxcvi)) f |= kVx;
  return f;
}

// A magnitude m * 2^e, exactly.
struct Exact {
  u128 m;
  int e;
};
int Bits(u128 m) {
  uint64_t hi = uint64_t(m >> 64), lo = uint64_t(m);
  return hi ? 128 - __builtin_clzll(hi) : lo ? 64 - __builtin_clzll(lo) : 0;
}
Exact Abs(uint64_t d) {
  uint64_t e = (d >> 52) & 0x7FF, f = d & 0x000FFFFFFFFFFFFFull;
  if (!e) return {f, -1074};
  return {f | 1ull << 52, int(e) - 1075};
}
Exact Int(int64_t v) { return {v < 0 ? u128(-uint64_t(v)) : u128(v), 0}; }
Exact Mul(Exact a, Exact b) {
  if (Bits(a.m) + Bits(b.m) > 127) std::abort();
  return {a.m * b.m, a.e + b.e};
}
// Both to the smaller exponent (the operands' ranges keep it in 127 bits).
void Align(Exact& a, Exact& b) {
  if (!a.m) a.e = b.e;
  if (!b.m) b.e = a.e;
  Exact& hi = a.e > b.e ? a : b;
  int d = hi.e - (a.e > b.e ? b.e : a.e);
  if (Bits(hi.m) + d > 127) std::abort();
  hi.m <<= d;
  hi.e -= d;
}
// a +- b of signed values; the magnitude of the result.
Exact AddMag(Exact a, bool a_neg, Exact b, bool b_neg) {
  Align(a, b);
  if (a_neg == b_neg) return {a.m + b.m, a.e};
  return {a.m > b.m ? a.m - b.m : b.m - a.m, a.e};
}
// The sign of |a| - |b|.
int Cmp(Exact a, Exact b) {
  if (!a.m || !b.m) return (a.m != 0) - (b.m != 0);
  int ta = Bits(a.m) + a.e, tb = Bits(b.m) + b.e;
  if (ta != tb) return ta < tb ? -1 : 1;
  Align(a, b);
  return a.m < b.m ? -1 : a.m > b.m;
}
bool Neg(uint64_t d) { return d >> 63; }

// FI and FR from the result against the exact value (`cmp`, the sign of
// |result| - |exact|), XX with FI, and FX if XX went from 0 to 1 (the
// operands raise nothing else).
void CheckRounding(int cmp, uint32_t in, uint32_t out) {
  Check(((out & kFi) != 0) == (cmp != 0), "FI is not 'the result is inexact'", in, out);
  Check(((out & kFr) != 0) == (cmp > 0), "FR is not 'the rounding increased the magnitude'", in, out);
  uint32_t xx = cmp ? kXx : 0;
  Check((out & kXx) == ((in & kXx) | xx), "XX", in, out);
  bool fx = (in & kFx) || (xx & ~in);
  Check(((out & kFx) != 0) == fx, "FX is not 'an exception bit went from 0 to 1'", in, out);
}

void Arithmetic() {
  static const struct {
    FpOp op;
    const char* name;
  } kOps[] = {{kFAdd, "fadd"}, {kFSub, "fsub"}, {kFMul, "fmul"}, {kFDiv, "fdiv"}, {kFSqrt, "fsqrt"},
              {kFMadd, "fmadd"}, {kFMsub, "fmsub"}, {kFNmadd, "fnmadd"}, {kFNmsub, "fnmsub"}};
  for (const auto& o : kOps)
    for (bool single : {false, true}) {
      char name[32];
      std::snprintf(name, sizeof name, "%s%s", o.name, single ? "s" : "");
      g_case = name;
      for (int t = 0; t < 20000; t++) {
        bool fused = o.op >= kFMadd;
        int lo = fused ? -4 : -30, hi = fused ? 4 : 30;
        uint64_t a = RandomDouble(lo, hi), cc = RandomDouble(lo, hi);
        uint64_t b = o.op == kFSqrt ? RandomDouble(-30, 30, true) : fused ? RandomDouble(-8, 8) : RandomDouble(lo, hi);
        Ctx c{};
        c.fpscr = RandomFpscr();
        uint32_t in = c.fpscr;
        uint64_t r = FpArith(c, o.op, single, a, b, cc);
        Exact R = Abs(r);
        int cmp;
        switch (o.op) {
          case kFAdd: cmp = Cmp(R, AddMag(Abs(a), Neg(a), Abs(b), Neg(b))); break;
          case kFSub: cmp = Cmp(R, AddMag(Abs(a), Neg(a), Abs(b), !Neg(b))); break;
          case kFMul: cmp = Cmp(R, Mul(Abs(a), Abs(cc))); break;
          case kFDiv: cmp = Cmp(Mul(R, Abs(b)), Abs(a)); break;      // |r| |b| against |a|
          case kFSqrt: cmp = Cmp(Mul(R, R), Abs(b)); break;          // r^2 against b
          default: {
            bool sub = o.op == kFMsub || o.op == kFNmsub;
            cmp = Cmp(R, AddMag(Mul(Abs(a), Abs(cc)), Neg(a) != Neg(cc), Abs(b), Neg(b) != sub));
            break;
          }
        }
        CheckRounding(cmp, in, c.fpscr);
      }
    }
}

void Conversions() {
  g_case = "frsp";
  for (int t = 0; t < 20000; t++) {
    uint64_t b = RandomDouble(-120, 120);
    Ctx c{};
    c.fpscr = RandomFpscr();
    uint32_t in = c.fpscr;
    uint64_t r = Frsp(c, b);
    CheckRounding(Cmp(Abs(r), Abs(b)), in, c.fpscr);
  }
  g_case = "fcfid";
  for (int t = 0; t < 20000; t++) {
    int64_t v = int64_t(Next()) >> (Next() % 63);
    Ctx c{};
    c.fpscr = RandomFpscr();
    uint32_t in = c.fpscr;
    uint64_t r = Fcfid(c, uint64_t(v));
    CheckRounding(Cmp(Abs(r), Int(v)), in, c.fpscr);
  }
  // In range: |b| below 2^30 (word) or 2^62 (doubleword).
  static const char* kNames[4] = {"fctiw", "fctiwz", "fctid", "fctidz"};
  for (unsigned mode = 0; mode < 4; mode++) {
    g_case = kNames[mode];
    for (int t = 0; t < 20000; t++) {
      uint64_t b = RandomDouble(-4, mode & 2 ? 61 : 29);
      Ctx c{};
      c.fpscr = RandomFpscr();
      uint32_t in = c.fpscr;
      uint64_t r = Fctiw(c, mode, b);
      int64_t v = mode & 2 ? int64_t(r) : int64_t(int32_t(r));
      CheckRounding(Cmp(Int(v), Abs(b)), in, c.fpscr);
    }
  }
}

// FCMPU and FCMPO: FPCC from the exact order, VXSNAN and (FCMPO, VE clear)
// VXVC for NaNs, FX if those went from 0 to 1, every other bit unchanged.
void Compares() {
  const uint64_t kSpecial[] = {0x7FF8000000000000ull, 0x7FF0000000000001ull, 0xFFF4000000000000ull,
                               0x7FF0000000000000ull, 0xFFF0000000000000ull, 0, 1ull << 63, 1};
  for (bool ordered : {false, true}) {
    g_case = ordered ? "fcmpo" : "fcmpu";
    for (int t = 0; t < 20000; t++) {
      uint64_t a = Next() % 4 ? RandomDouble(-3, 3) : kSpecial[Next() % 8];
      uint64_t b = Next() % 4 ? (Next() % 4 ? RandomDouble(-3, 3) : a) : kSpecial[Next() % 8];
      Ctx c{};
      c.fpscr = RandomFpscr();
      uint32_t in = c.fpscr;
      Fcmp(c, ordered, a, b);
      auto nan = [](uint64_t d) { return (d & ~(1ull << 63)) > 0x7FF0000000000000ull; };
      auto snan = [&](uint64_t d) { return nan(d) && !(d & (1ull << 51)); };
      uint32_t fpcc, exc = 0;
      if (nan(a) || nan(b)) {
        fpcc = 1;
        if (snan(a) || snan(b)) exc |= kVxsnan;
        if (ordered) exc |= kVxvc;
      } else {
        // The order of sign and magnitude, -0 equal to +0.
        auto key = [](uint64_t d) {
          int64_t m = int64_t(d & ~(1ull << 63));
          return d >> 63 ? -m : m;
        };
        fpcc = key(a) < key(b) ? 8 : key(a) > key(b) ? 4 : 2;
      }
      uint32_t out = c.fpscr;
      Check((out & kFpcc) == fpcc << 12, "FPCC", in, out);
      Check((out & (kFi | kFr | kC)) == (in & (kFi | kFr | kC)), "FI, FR or FPRF's C changed", in, out);
      Check((out & kExceptions) == ((in & kExceptions) | exc), "exception bits", in, out);
      bool fx = (in & kFx) || (exc & ~in);
      Check(((out & kFx) != 0) == fx, "FX is not 'an exception bit went from 0 to 1'", in, out);
    }
  }
}

// FRES: FPRF is the class of the single-precision result, which these
// operands give exactly (reciprocals of powers of two, zeros, infinities).
void Fres() {
  g_case = "fres";
  const struct {
    uint64_t b;
    uint32_t rn, fprf;
  } kCases[] = {
      {0x4000000000000000ull, 0, 0x04},  // 2: 0.5, +normal
      {0xC010000000000000ull, 0, 0x08},  // -4: -0.25, -normal
      {0, 0, 0x05},                      // +0: +infinity (ZX)
      {1ull << 63, 0, 0x09},             // -0: -infinity (ZX)
      {0x7FF0000000000000ull, 0, 0x02},  // +infinity: +0
      {0xFFF0000000000000ull, 0, 0x12},  // -infinity: -0
      {0x37D0000000000000ull, 0, 0x05},  // 2^-130: 2^130 overflows single, +infinity
      {0x37D0000000000000ull, 1, 0x04},  // the same toward zero: single's largest, +normal
  };
  for (const auto& k : kCases)
    for (uint32_t prf = 0; prf < 32; prf++) {
      Ctx c{};
      c.fpscr = prf << 12 | k.rn;
      uint32_t in = c.fpscr;
      FpArith(c, kFRes, true, 0, k.b, 0);
      Check((c.fpscr & kFprf) == k.fprf << 12, "FPRF is not the result's class", in, c.fpscr);
    }
}

// The fixture's opcode 59/63 record forms, lifted: CR1 = FPSCR[FX FEX VX
// OX] after the instruction, and FX by the 0 to 1 rule except where the
// instruction writes FPSCR bits itself (MTFSF MTFSFI MTFSB0 MTFSB1).
void RecordForms() {
  int forms = 0;
  for (int k = 0; k < kFormCount; k++) {
    uint32_t raw = kForms[k].raw, op = raw >> 26, xo = (raw >> 1) & 0x3FF;
    if ((op != 59 && op != 63) || !(raw & 1)) continue;
    bool writes_fpscr = op == 63 && (xo == 711 || xo == 134 || xo == 70 || xo == 38);
    g_case = kForms[k].name;
    forms++;
    for (int t = 0; t < 2000; t++) {
      Ctx c{};
      for (uint64_t& f : c.f) f = RandomDouble(-8, 8);
      c.cr = uint32_t(Next());
      c.fpscr = RandomFpscr();
      uint32_t in = c.fpscr;
      kForms[k].fn(c);
      uint32_t out = c.fpscr;
      Check(Field(c.cr, 1) == out >> 28, "CR1 is not FPSCR[FX FEX VX OX]", in, out);
      if (writes_fpscr) continue;
      bool fx = (in & kFx) || (out & ~in & kExceptions);
      Check(((out & kFx) != 0) == fx, "FX is not 'an exception bit went from 0 to 1'", in, out);
    }
  }
  if (!forms) {
    std::printf("FAIL: the fixture has no floating-point record forms\n");
    failures++;
  }
}

}  // namespace

int main() {
  Arithmetic();
  Conversions();
  Compares();
  Fres();
  RecordForms();
  std::printf("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
  return failures ? 1 : 0;
}
