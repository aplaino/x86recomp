// Checks the RECOMP_SSE_HOST forms in runtime/sse.h (built here with it
// defined) against the SoftFloat models in sse.cpp: CMPxx, MINxx/MAXxx,
// CVT(T)PS2DQ / CVT(T)PD2DQ, CVTDQ2PS and (U)COMIS with ComiDirect, plus
// the jumps the lifter compiles to a compare after a (U)COMIS. Lanes come
// from a list of edge values (zeros, denormals, the normal range's ends,
// infinities, QNaN and SNaN payloads, the int32 limits and ties) and
// random bits. Results must match bit for bit; for the conversions MXCSR
// too (they are exact), for the others only IE/DE may differ (see
// HostArith). MXCSR not as usual (another rounding mode, PE clear) must
// take the model. Portable.
//
// Usage: sse_host_test [cases] [seed]

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "runtime/sse.h"

namespace {

namespace sse = recomp::sse;
using recomp::Ctx;
using recomp::Xmm;

std::mt19937_64 g_rng;
uint64_t Rand() { return g_rng(); }

uint32_t Bits(float x) { return std::bit_cast<uint32_t>(x); }
uint64_t Bits(double x) { return std::bit_cast<uint64_t>(x); }

std::vector<uint32_t> g_f32;
std::vector<uint64_t> g_f64;

void MakeEdges() {
  const double kInt[] = {0.0,          0.25,         0.5,          0.75,         1.0,
                         1.5,          2.5,          3.5,          1e-30,        0x1p23,
                         0x1p23 + 0.5, 0x1p24,       0x1p52,       0x1p52 + 0.5, 2147483520.0,
                         2147483584.0, 2147483647.0, 2147483647.5, 0x1p31,       0x1p31 + 1,
                         0x1p31 + 256, 2147483648.5, 2147483649.0, 0x1p32,       0x1p63,
                         1e10,         1e300};
  for (double sign : {1.0, -1.0}) {
    for (double v : kInt) {
      g_f64.push_back(Bits(sign * v));
      g_f64.push_back(Bits(std::nextafter(sign * v, 0.0)));
      g_f64.push_back(Bits(std::nextafter(sign * v, sign * 1e308)));
      const float f = float(sign * v);
      g_f32.push_back(Bits(f));
      g_f32.push_back(Bits(std::nextafter(f, 0.0f)));
      g_f32.push_back(Bits(std::nextafter(f, sign * 3e38f)));
    }
    // -2^31 - 1 + ulp etc.: truncation boundaries of the doubles.
    g_f64.push_back(Bits(sign * 2147483648.999));
    g_f64.push_back(Bits(sign * 2147483647.999));
  }
  const uint32_t k32[] = {0x00000001u, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu,
                          0x7F800000u, 0x7FC00000u, 0x7FC12345u, 0x7FA00000u,
                          0x7F800001u, 0x7FFFFFFFu, 0x3F000000u, 0x4F000000u};
  for (uint32_t b : k32) {
    g_f32.push_back(b);
    g_f32.push_back(b | 0x80000000u);
  }
  const uint64_t k64[] = {0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull,
                          0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0x7FF8000000000000ull,
                          0x7FF8000000012345ull, 0x7FF4000000000000ull, 0x7FF0000000000001ull,
                          0x7FFFFFFFFFFFFFFFull};
  for (uint64_t b : k64) {
    g_f64.push_back(b);
    g_f64.push_back(b | 0x8000000000000000ull);
  }
}

uint32_t RandF32() {
  switch (Rand() % 4) {
    case 0: return uint32_t(Rand());
    case 1: {  // an everyday value
      const float f = float(int64_t(Rand() % 2000001) - 1000000) / float(1 + Rand() % 64);
      return Bits(f);
    }
    default: return g_f32[Rand() % g_f32.size()];
  }
}
uint64_t RandF64() {
  switch (Rand() % 4) {
    case 0: return Rand();
    case 1: return Bits(double(int64_t(Rand() % 2000001) - 1000000) / double(1 + Rand() % 64));
    default: return g_f64[Rand() % g_f64.size()];
  }
}
Xmm RandXmm(bool dbl) {
  Xmm x;
  for (int i = 0; i < (dbl ? 2 : 4); i++)
    if (dbl)
      sse::SetLane<uint64_t>(x, i, RandF64());
    else
      sse::SetLane<uint32_t>(x, i, RandF32());
  return x;
}
// int32 lanes for CVTDQ2PS: around powers of two (rounding), the limits.
Xmm RandInts() {
  Xmm x;
  for (int i = 0; i < 4; i++) {
    int32_t v;
    switch (Rand() % 4) {
      case 0: v = int32_t(Rand()); break;
      case 1: v = int32_t((1u << (24 + Rand() % 7)) + (Rand() % 9) - 4); break;
      case 2: v = Rand() & 1 ? INT32_MIN + int32_t(Rand() % 300) : INT32_MAX - int32_t(Rand() % 300); break;
      default: v = int32_t(Rand() % 2001) - 1000; break;
    }
    if (Rand() & 1) v = -v;
    sse::SetLane<int32_t>(x, i, v);
  }
  return x;
}

bool Same(const Xmm& a, const Xmm& b) { return std::memcmp(a.data(), b.data(), 16) == 0; }

long g_fails = 0;
void Fail(const char* what, uint32_t mxcsr, const Xmm& d, const Xmm& s, const Xmm& f,
          const Xmm& m, uint32_t fm, uint32_t mm) {
  if (++g_fails > 20) return;
  auto hex = [](const Xmm& x) {
    static char buf[4][40];
    static int k;
    char* p = buf[k++ & 3];
    for (int i = 15; i >= 0; i--) std::snprintf(p + 2 * (15 - i), 3, "%02X", x.b[i]);
    return p;
  };
  std::printf("FAIL %s mxcsr=%04X d=%s s=%s\n  host %s %04X\n  model %s %04X\n", what, mxcsr,
              hex(d), hex(s), hex(f), fm, hex(m), mm);
}

// MXCSR for a case: as usual (PE set) mostly, else another state the
// host forms must leave to the model.
uint32_t RandMxcsr() {
  if (Rand() % 4) return 0x1FA0u | uint32_t(Rand() % 2);  // IE set or not
  static const uint32_t kOther[] = {0x1F80u, 0x3FA0u, 0x5FA0u, 0x7FA0u, 0x1FA2u};
  return kOther[Rand() % 5];
}
// Flags compared: all for the exact forms, all but IE/DE for the others
// (while MXCSR is as usual; the model's own otherwise).
uint32_t Mask(bool exact, uint32_t mxcsr) {
  return exact || !sse::fast::UsualMxcsr(mxcsr) ? 0xFFFFu : 0xFFFCu;
}

void One() {
  Ctx c{};
  // Lifted code passes its MU (lifted.h): whether MXCSR is as usual.
  auto U = [&] { return sse::fast::UsualMxcsr(c.mxcsr); };
  const bool dbl = Rand() & 1;
  const uint32_t mx = RandMxcsr();
  const Xmm d0 = RandXmm(dbl), s0 = RandXmm(dbl);
  Xmm d0b = d0;
  if (Rand() % 4 == 0) std::memcpy(d0b.data(), s0.data(), 16);  // equal lanes
  auto check = [&](const char* what, bool exact, auto&& host, auto&& model, const Xmm& d,
                   const Xmm& s) {
    Xmm hd = d, md = d;
    c.mxcsr = mx;
    host(hd, s);
    const uint32_t hm = c.mxcsr;
    c.mxcsr = mx;
    model(md, s);
    const uint32_t mm = c.mxcsr;
    const uint32_t k = Mask(exact, mx);
    if (!Same(hd, md) || (hm & k) != (mm & k)) Fail(what, mx, d, s, hd, md, hm, mm);
  };
  // CMPxx: every predicate, packed and scalar.
  const uint8_t pred = uint8_t(Rand() % 8);
  const int lanes = Rand() & 1 ? 1 : dbl ? 2 : 4;
  check("Cmp", false, [&](Xmm& d, const Xmm& s) { sse::Cmp(c, U(), dbl, lanes, d, s, pred, 0); },
        [&](Xmm& d, const Xmm& s) { d = sse::CmpModel(c, dbl, lanes, d, s, pred, 0); }, d0b, s0);
  // MINxx/MAXxx.
  const bool max = Rand() & 1;
  check("MinMax", false,
        [&](Xmm& d, const Xmm& s) { sse::MinMax(c, U(), max, dbl, lanes, d, s, 0); },
        [&](Xmm& d, const Xmm& s) { d = sse::MinMaxModel(c, max, dbl, lanes, d, s, 0); }, d0b, s0);
  // CVT(T)PS2DQ / CVT(T)PD2DQ.
  const bool trunc = Rand() & 1;
  check("CvtToDq", true, [&](Xmm& d, const Xmm& s) { sse::CvtToDq(c, U(), dbl, trunc, d, s, 0); },
        [&](Xmm& d, const Xmm& s) { d = sse::CvtToDqModel(c, dbl, trunc, s, 0); }, d0, s0);
  // CVTDQ2PS.
  const Xmm ints = RandInts();
  check("Cvtdq2ps", true, [&](Xmm& d, const Xmm& s) { sse::Cvtdq2ps(c, U(), d, s, 0); },
        [&](Xmm& d, const Xmm& s) { d = sse::Cvtdq2psModel(c, s, 0); }, d0, ints);
  // (U)COMIS: Comi's result, ComiDirect exactly when neither is a NaN, and
  // every jump the lifter fuses, on the compare (ComiDirect) and on the
  // model's flags.
  const bool ordered = Rand() & 1;
  c.mxcsr = mx;
  const int host = sse::Comi(c, dbl, ordered, d0b, s0, 0);
  c.mxcsr = mx;
  const int model = sse::ComiModel(c, dbl, ordered, d0b, s0, 0);
  const bool direct = sse::ComiDirect(dbl, d0b, s0);
  const bool nan = model == 3;
  if (host != model || direct == nan) {
    Xmm h{}, m{};
    h.b[0] = uint8_t(host);
    m.b[0] = uint8_t(model);
    Fail("Comi", mx, d0b, s0, h, m, direct, 0);
  }
  if (direct) {
    const bool zf = model >= 2, pf = model == 3, cf = model == 1 || model == 3;
    bool want[8] = {cf, !cf, zf, !zf, cf || zf, !(cf || zf), pf, !pf};
    bool got[8];
    auto jumps = [&](auto p, auto q) {
      bool g[8] = {p < q, p >= q, p == q, p != q, p <= q, p > q, false, true};
      std::memcpy(got, g, sizeof g);
    };
    if (dbl)
      jumps(sse::Lane<double>(d0b, 0), sse::Lane<double>(s0, 0));
    else
      jumps(sse::Lane<float>(d0b, 0), sse::Lane<float>(s0, 0));
    for (int j = 0; j < 8; j++)
      if (got[j] != want[j]) {
        Xmm h{}, m{};
        h.b[0] = uint8_t(j);
        Fail("Comi jcc", mx, d0b, s0, h, m, got[j], want[j]);
      }
  }
}

}  // namespace

int main(int argc, char** argv) {
  long cases = argc > 1 ? std::atol(argv[1]) : 2000000;
  g_rng.seed(argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1);
  MakeEdges();
  for (long n = 0; n < cases; n++) One();
  std::printf("%ld cases, %ld mismatches\n", cases, g_fails);
  return g_fails ? 1 : 0;
}
