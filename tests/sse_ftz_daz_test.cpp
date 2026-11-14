// Checks the SSE model's flush-to-zero (FTZ) and denormals-are-zero (DAZ)
// modes against results worked out from the SDM (vol. 1, 10.2.3.3 and
// 10.2.3.4): under FTZ with underflow masked a tiny result is a zero of its
// sign with UE and PE; under DAZ a denormal operand is a zero of its sign and
// raises nothing. Each case also runs without the mode, where the IEEE
// result stands. Portable.

#include <cinttypes>
#include <cstdio>

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

  std::printf("%d failures\n", g_fails);
  return g_fails ? 1 : 0;
}
