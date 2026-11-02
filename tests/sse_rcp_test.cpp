// sse::RcpLane (RCPPS) is the correctly rounded 1/x (see sse.h).
//
// - Every significand at 1 <= x < 2 (the rest follows from
//   rcp(2x) = rcp(x) / 2): the result is checked against the two midpoints
//   around it with exact integer arithmetic.
// - The scaling identity at every exponent whose result is normal, for a
//   sample of significands; results below the normal range are ±0.
// - Special operands: ±0, denormals, ±inf, NaNs, signs.

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

#include "runtime/sse.h"

namespace recomp {
void FatalF(const char* fmt, ...) {  // runtime/sse.cpp's #XM
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::abort();
}
}  // namespace recomp

namespace {

int g_fail = 0;

void Fail(const char* what, uint32_t x, uint32_t got) {
  if (g_fail++ < 20) std::printf("FAIL %s: x=%08X got=%08X\n", what, x, got);
}

}  // namespace

int main() {
  using recomp::sse::RcpLane;

  // Exhaustive over 1 <= x < 2. x = mx * 2^-23; for x > 1 the result lies
  // in (0.5, 1): r = mr * 2^-24, its midpoints (2 mr -+ 1) * 2^-25, and r
  // is the nearest float iff lo * x < 1 < hi * x, i.e.
  // (2 mr - 1) * mx < 2^48 < (2 mr + 1) * mx. No ties: 1/x is a midpoint
  // only if both significands are powers of two.
  uint64_t n = 0;
  if (RcpLane(0x3F800000) != 0x3F800000) Fail("1", 0x3F800000, RcpLane(0x3F800000));
  for (uint32_t m = 1; m < 0x800000; m++) {
    uint32_t x = (127u << 23) | m;
    uint32_t r = RcpLane(x);
    if ((r >> 23) != 126) {
      Fail("range", x, r);
      continue;
    }
    const uint64_t mx = 0x800000u | m, mr = 0x800000u | (r & 0x7FFFFF);
    const uint64_t one = uint64_t(1) << 48;
    if (!((2 * mr - 1) * mx < one && one < (2 * mr + 1) * mx)) Fail("nearest", x, r);
    n++;
  }

  // Scaling: rcp(x * 2^k) = rcp(x) * 2^-k while the result is normal, and
  // ±0 below that. The sign carries through.
  for (uint32_t m = 0; m < 0x800000; m += 4093) {
    const uint32_t base = RcpLane((127u << 23) | m);
    const int ebase = int(base >> 23);
    for (int e = 1; e <= 254; e++) {
      for (uint32_t sign = 0; sign <= 1; sign++) {
        uint32_t x = sign << 31 | uint32_t(e) << 23 | m;
        int er = ebase - (e - 127);
        uint32_t want = er >= 1 ? sign << 31 | uint32_t(er) << 23 | (base & 0x7FFFFF)
                                : sign << 31;
        uint32_t got = RcpLane(x);
        if (got != want) Fail("scaling", x, got);
      }
    }
  }

  // Special operands.
  struct {
    uint32_t x, want;
  } special[] = {
      {0x00000000, 0x7F800000}, {0x80000000, 0xFF800000},  // ±0 -> ±inf
      {0x00000001, 0x7F800000}, {0x807FFFFF, 0xFF800000},  // denormals as ±0
      {0x40000000, 0x3F000000}, {0xC0000000, 0xBF000000},  // ±2 -> ±0.5
      {0x7F800000, 0x00000000}, {0xFF800000, 0x80000000},  // ±inf -> ±0
      {0x7F7FFFFF, 0x00000000},                            // max -> below normal: +0
      {0x7FC00001, 0x7FC00001}, {0x7F800001, 0x7FC00001},  // QNaN, SNaN quieted
      {0xFF800001, 0xFFC00001},
  };
  for (const auto& s : special) {
    uint32_t got = RcpLane(s.x);
    if (got != s.want) Fail("special", s.x, got);
  }

  std::printf("rcp: %llu significands, %d failures\n", (unsigned long long)n, g_fail);
  return g_fail ? 1 : 0;
}
