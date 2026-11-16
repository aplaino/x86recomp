// Checks the SSE model's flush-to-zero (FTZ) and denormals-are-zero (DAZ)
// modes against results worked out from the SDM (vol. 1, 10.2.3.3 and
// 10.2.3.4): under FTZ with underflow masked a tiny result is a zero of its
// sign with UE and PE; under DAZ a denormal operand is a zero of its sign and
// raises nothing. Each case also runs without the mode, where the IEEE
// result stands. Then fuzzes the RECOMP_SSE_HOST paths for FTZ with DAZ
// (sse::ArithFtzDaz, CmpDaz, MinMaxDaz, SqrtDaz, CvtFloatFtzDaz) against
// the model: where a path takes an operation it must give the model's bits
// (NaNs aside, as HostArith's contract has it) and its UE. Portable.

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <random>

#include "runtime/sse.h"

namespace {

namespace sse = recomp::sse;
using recomp::Ctx;
using recomp::Xmm;

constexpr uint32_t kMasked = 0x1F80, kFTZ = 0x8000, kDAZ = 0x40, kRoundUp = 0x4000;
constexpr uint32_t kFlags = 0x3F;

int g_fails = 0;

Xmm F(uint32_t v) {
  Xmm x{};
  sse::SetLane<uint32_t>(x, 0, v);
  return x;
}

void Expect(const char* what, uint32_t mxcsr, uint32_t got, uint32_t want, const Ctx& c,
            uint32_t want_flags) {
  const uint32_t flags = c.mxcsr & kFlags;
  if (got == want && flags == want_flags) return;
  std::printf("FAIL %s (mxcsr %04X): %08X flags %02X, want %08X flags %02X\n", what, mxcsr, got,
              flags, want, want_flags);
  g_fails++;
}

void Arith(const char* what, uint32_t mxcsr, sse::Op op, uint32_t a, uint32_t b, uint32_t want,
           uint32_t want_flags) {
  Ctx c{};
  c.mxcsr = mxcsr;
  Xmm r = sse::ArithModel(c, op, false, 1, F(a), F(b), 0);
  Expect(what, mxcsr, sse::Lane<uint32_t>(r, 0), want, c, want_flags);
}

void Sqrt(const char* what, uint32_t mxcsr, uint32_t a, uint32_t want, uint32_t want_flags) {
  Ctx c{};
  c.mxcsr = mxcsr;
  Xmm r = sse::SqrtModel(c, false, 1, F(0), F(a), 0);
  Expect(what, mxcsr, sse::Lane<uint32_t>(r, 0), want, c, want_flags);
}

void ToInt(const char* what, uint32_t mxcsr, uint32_t a, uint32_t want, uint32_t want_flags) {
  Ctx c{};
  c.mxcsr = mxcsr;
  Expect(what, mxcsr, sse::CvtToI32Model(c, false, F(a), false, 0), want, c, want_flags);
}

void Min(const char* what, uint32_t mxcsr, uint32_t a, uint32_t b, uint32_t want,
         uint32_t want_flags) {
  Ctx c{};
  c.mxcsr = mxcsr;
  Xmm r = sse::MinMaxModel(c, false, false, 1, F(a), F(b), 0);
  Expect(what, mxcsr, sse::Lane<uint32_t>(r, 0), want, c, want_flags);
}

// Operands that reach the edges: tiny and near-normal magnitudes (the
// region FTZ and DAZ act on), ordinary ones, zeros, infinities and NaNs.
std::mt19937_64 rng(1234);
uint32_t RandF32() {
  const uint32_t sign = uint32_t(rng() & 1) << 31, frac = uint32_t(rng()) & 0x7FFFFFu;
  switch (rng() % 8) {
    case 0: return sign | frac;                                  // denormal
    case 1: return sign | (uint32_t(1 + rng() % 3) << 23) | frac;  // just above
    case 2: return sign | 0x00800000u | (rng() % 2 ? 0x7FFFFFu & uint32_t(rng() % 4) : 0);
    case 3: return sign | (uint32_t(rng() % 4) ? 0 : 0x7F800000u);  // zero, inf
    case 4: return sign | (uint32_t(60 + rng() % 8) << 23) | frac;   // 2^-67..: tiny products
    case 5: return sign | (rng() % 2 ? 0x3F7FFF00u : 0x3F800000u) | uint32_t(rng() & 0xFF);  // ~1
    default: return sign | (uint32_t(1 + rng() % 254) << 23) | frac;
  }
}
uint64_t RandF64() {
  const uint64_t sign = (rng() & 1) << 63, frac = rng() & 0xFFFFFFFFFFFFFull;
  switch (rng() % 6) {
    case 0: return sign | frac;
    case 1: return sign | (uint64_t(1 + rng() % 3) << 52) | frac;
    // Around the float range's edge, for narrowing.
    case 2: return sign | (uint64_t(1023 - 126 - int(rng() % 30)) << 52) | frac;
    case 3: return sign | (uint64_t(1023 - 126 + int(rng() % 3) - 1) << 52) |
                   (rng() % 2 ? 0xFFFFFFF000000ull | (frac & 0xFFFFFFull) : frac);
    case 4: return sign | (rng() % 4 ? 0 : 0x7FF0000000000000ull);
    default: return sign | (uint64_t(1 + rng() % 2046) << 52) | frac;
  }
}

bool IsNan32(uint32_t v) { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu); }
bool IsNan64(uint64_t v) {
  return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0xFFFFFFFFFFFFFull);
}

// All masked, RN, FTZ, PE; DAZ set or not (g_daz).
constexpr uint32_t kFtz = 0x9FA0, kDazBit = 0x40;
uint32_t g_mxcsr = kFtz | kDazBit;
constexpr uint32_t kUE = 0x10;
int g_taken = 0, g_declined = 0;

// The fast lanes against the model's: same bits but where both are NaN.
template <typename U>
bool SameLanes(const Xmm& a, const Xmm& b, int n, bool (*nan)(U)) {
  for (int i = 0; i < n; i++) {
    const U x = sse::Lane<U>(a, i), y = sse::Lane<U>(b, i);
    if (x != y && !(nan(x) && nan(y))) return false;
  }
  return true;
}

void FuzzArith(bool dbl, sse::Op op) {
  for (int k = 0; k < 200000; k++) {
    Xmm d{}, s{};
    for (int i = 0; i < 4; i++) {
      if (dbl) {
        if (i < 2) sse::SetLane<uint64_t>(d, i, RandF64()), sse::SetLane<uint64_t>(s, i, RandF64());
      } else {
        sse::SetLane<uint32_t>(d, i, RandF32());
        sse::SetLane<uint32_t>(s, i, RandF32());
      }
    }
    const int lanes = dbl ? 1 + int(rng() % 2) : (rng() % 2 ? 1 : 4);
    Ctx cm{}, cf{};
    cm.mxcsr = cf.mxcsr = g_mxcsr;
    const Xmm want = sse::ArithModel(cm, op, dbl, lanes, d, s, 0);
    Xmm got = d;
    if (!sse::ArithFtzDaz(cf, op, dbl, lanes, got, s)) {
      if (!SameLanes<uint64_t>(got, d, 2, IsNan64)) {
        std::printf("FAIL arith %d/%d: declined but changed d\n", int(op), dbl);
        g_fails++;
      }
      g_declined++;
      continue;
    }
    g_taken++;
    const bool same = dbl ? SameLanes<uint64_t>(got, want, 2, IsNan64)
                          : SameLanes<uint32_t>(got, want, 4, IsNan32);
    if (!same || (cf.mxcsr & kUE) != (cm.mxcsr & kUE)) {
      std::printf("FAIL arith op %d dbl %d lanes %d: d %016" PRIX64 "%016" PRIX64
                  " s %016" PRIX64 "%016" PRIX64 " got %016" PRIX64 "%016" PRIX64
                  " want %016" PRIX64 "%016" PRIX64 " UE %X/%X\n",
                  int(op), dbl, lanes, sse::Lane<uint64_t>(d, 1), sse::Lane<uint64_t>(d, 0),
                  sse::Lane<uint64_t>(s, 1), sse::Lane<uint64_t>(s, 0),
                  sse::Lane<uint64_t>(got, 1), sse::Lane<uint64_t>(got, 0),
                  sse::Lane<uint64_t>(want, 1), sse::Lane<uint64_t>(want, 0),
                  cf.mxcsr & kUE, cm.mxcsr & kUE);
      if (++g_fails > 20) return;
    }
  }
}

void FuzzOthers() {
  for (int k = 0; k < 200000; k++) {
    Xmm d{}, s{};
    for (int i = 0; i < 4; i++) {
      sse::SetLane<uint32_t>(d, i, RandF32());
      sse::SetLane<uint32_t>(s, i, RandF32());
    }
    const int lanes = rng() % 2 ? 1 : 4;
    Ctx c{};
    c.mxcsr = g_mxcsr;
    // Compares (predicates 0-7) and MIN/MAX.
    const uint8_t pred = uint8_t(rng() % 8);
    Xmm got = d;
    sse::CmpDaz(c, false, lanes, got, s, pred);
    if (!SameLanes<uint32_t>(got, sse::CmpModel(c, false, lanes, d, s, pred, 0), 4, IsNan32)) {
      std::printf("FAIL cmp pred %d: %08X vs %08X\n", pred, sse::Lane<uint32_t>(d, 0),
                  sse::Lane<uint32_t>(s, 0));
      g_fails++;
    }
    const bool max = rng() % 2;
    got = d;
    sse::MinMaxDaz(c, max, false, lanes, got, s);
    if (!SameLanes<uint32_t>(got, sse::MinMaxModel(c, max, false, lanes, d, s, 0), 4, IsNan32)) {
      std::printf("FAIL %s: %08X, %08X\n", max ? "max" : "min", sse::Lane<uint32_t>(d, 0),
                  sse::Lane<uint32_t>(s, 0));
      g_fails++;
    }
    got = d;
    sse::SqrtDaz(c, false, lanes, got, s);
    if (!SameLanes<uint32_t>(got, sse::SqrtModel(c, false, lanes, d, s, 0), 4, IsNan32)) {
      std::printf("FAIL sqrt %08X\n", sse::Lane<uint32_t>(s, 0));
      g_fails++;
    }
    // Narrowing and widening.
    Xmm wide{};
    sse::SetLane<uint64_t>(wide, 0, RandF64());
    sse::SetLane<uint64_t>(wide, 1, RandF64());
    for (bool to_dbl : {false, true}) {
      const Xmm src = to_dbl ? s : wide;
      const int n = 1 + int(rng() % 2);
      Ctx cm{}, cf{};
      cm.mxcsr = cf.mxcsr = g_mxcsr;
      const Xmm want = sse::CvtFloatModel(cm, to_dbl, n, d, src, 0);
      got = d;
      if (!sse::CvtFloatFtzDaz(cf, to_dbl, n, got, src)) {
        g_declined++;
        continue;
      }
      g_taken++;
      const bool same = to_dbl ? SameLanes<uint64_t>(got, want, 2, IsNan64)
                               : SameLanes<uint32_t>(got, want, 4, IsNan32);
      if (!same || (cf.mxcsr & kUE) != (cm.mxcsr & kUE)) {
        std::printf("FAIL cvt to_dbl %d: %016" PRIX64 " got %08X want %08X\n", to_dbl,
                    sse::Lane<uint64_t>(src, 0), sse::Lane<uint32_t>(got, 0),
                    sse::Lane<uint32_t>(want, 0));
        if (++g_fails > 40) return;
      }
    }
  }
}

}  // namespace

int main() {
  constexpr uint32_t kMinNormal = 0x00800000, kHalf = 0x3F000000, kOne = 0x3F800000;
  constexpr uint32_t kUE = 0x10, kPE = 0x20, kDE = 0x02, kIE = 0x01, kZE = 0x04;

  // An exact denormal product: no flag without FTZ, flushed with it.
  Arith("min normal * 0.5", kMasked, sse::kMul, kMinNormal, kHalf, 0x00400000, 0);
  Arith("min normal * 0.5", kMasked | kFTZ, sse::kMul, kMinNormal, kHalf, 0, kUE | kPE);
  Arith("-min normal * 0.5", kMasked | kFTZ, sse::kMul, 0x80800000, kHalf, 0x80000000,
        kUE | kPE);
  // An inexact tiny product: rounded to a denormal, or flushed.
  Arith("(min normal + 1 ulp) * 0.5", kMasked, sse::kMul, 0x00800001, kHalf, 0x00400000,
        kUE | kPE);
  Arith("(min normal + 1 ulp) * 0.5", kMasked | kFTZ, sse::kMul, 0x00800001, kHalf, 0,
        kUE | kPE);
  // Normal results are untouched by FTZ.
  Arith("1 * 0.5", kMasked | kFTZ, sse::kMul, kOne, kHalf, kHalf, 0);

  // A denormal operand: DE without DAZ, a zero with it.
  Arith("denormal + 0", kMasked, sse::kAdd, 1, 0, 1, kDE);
  Arith("denormal + 0", kMasked | kDAZ, sse::kAdd, 1, 0, 0, 0);
  Arith("-denormal + -0", kMasked | kDAZ, sse::kAdd, 0x80000001, 0x80000000, 0x80000000, 0);
  // A denormal divisor divides by zero under DAZ.
  Arith("1 / denormal", kMasked | kDAZ, sse::kDiv, kOne, 1, 0x7F800000, kZE);
  // The square root of a negative denormal: invalid, or that of -0.
  Sqrt("sqrt(-denormal)", kMasked, 0x80000001, 0xFFC00000, kIE);
  Sqrt("sqrt(-denormal)", kMasked | kDAZ, 0x80000001, 0x80000000, 0);
  // Rounded up to an integer: 1 and inexact, or exactly 0.
  ToInt("cvtss2si(denormal), round up", kMasked | kRoundUp, 1, 1, kPE);
  ToInt("cvtss2si(denormal), round up", kMasked | kRoundUp | kDAZ, 1, 0, 0);
  // MIN of two denormals of opposite sign: equal zeros give the source.
  Min("min(-denormal, denormal)", kMasked, 0x80000001, 1, 0x80000001, kDE);
  Min("min(-denormal, denormal)", kMasked | kDAZ, 0x80000001, 1, 0, 0);

  // Tiny before rounding, the smallest normal after: x86 flushes it (it
  // detects tininess after rounding to unbounded range, where the value
  // stays below 2^-126); the host's result is the smallest normal. The
  // host path must leave it to the model.
  {
    Ctx cf{};
    cf.mxcsr = kFtz | kDazBit;
    Xmm d = F(kMinNormal), s = F(0x3F7FFFFF);  // * (1 - 2^-24)
    const bool taken = sse::ArithFtzDaz(cf, sse::kMul, false, 1, d, s);
    if (taken && !(sse::Lane<uint32_t>(d, 0) == 0 && (cf.mxcsr & kUE))) {
      std::printf("FAIL host path: min normal * (1 - 2^-24) gave %08X\n",
                  sse::Lane<uint32_t>(d, 0));
      g_fails++;
    }
  }
  for (uint32_t m : {kFtz | kDazBit, kFtz}) {
    g_mxcsr = m;
    for (bool dbl : {false, true})
      for (sse::Op op : {sse::kAdd, sse::kSub, sse::kMul, sse::kDiv}) FuzzArith(dbl, op);
    FuzzOthers();
  }
  std::printf("host FTZ/DAZ paths: %d taken, %d left to the model\n", g_taken, g_declined);

  std::printf("%d failures\n", g_fails);
  return g_fails ? 1 : 0;
}
