// sse_ftz_test: SSE arithmetic and CVTPD2PS under FTZ and DAZ against
// the host, from operands near the bottom of the range (denormal results,
// exact or not, and ones that round into or out of it), every rounding
// mode, FTZ and DAZ set and clear; and the other forms that read
// floating-point operands (square roots, minimum and maximum, compares,
// conversions, rounding) on denormal operands under DAZ set and clear;
// F16C's conversions: every half to single, and singles around the half
// range to half under each rounding control, DAZ and FTZ set and clear.
// x86-64 hosts with F16C only.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include "x86/sse.h"

namespace {

using namespace recomp;

int failures = 0;

typedef float V4 __attribute__((vector_size(16)));

// The host's result and MXCSR for one form, run with mxcsr loaded.
#define HOST_FORM(name, insn)                                                      \
  void name(uint32_t& mxcsr, Xmm& d, const Xmm& s) {                               \
    asm volatile("ldmxcsr %0\n\t" insn " %2, %1\n\tstmxcsr %0"                     \
                 : "+m"(mxcsr), "+x"(*reinterpret_cast<V4*>(d.b))              \
                 : "x"(*reinterpret_cast<const V4*>(s.b)));                    \
  }
HOST_FORM(HostAddps, "addps")
HOST_FORM(HostSubps, "subps")
HOST_FORM(HostMulps, "mulps")
HOST_FORM(HostDivps, "divps")
HOST_FORM(HostAddpd, "addpd")
HOST_FORM(HostSubpd, "subpd")
HOST_FORM(HostMulpd, "mulpd")
HOST_FORM(HostDivpd, "divpd")
HOST_FORM(HostCvtpd2ps, "cvtpd2ps")
HOST_FORM(HostSqrtps, "sqrtps")
HOST_FORM(HostSqrtpd, "sqrtpd")
HOST_FORM(HostMinps, "minps")
HOST_FORM(HostMaxpd, "maxpd")
HOST_FORM(HostCmpltps, "cmpltps")
HOST_FORM(HostCmplepd, "cmplepd")
HOST_FORM(HostCvtps2pd, "cvtps2pd")
HOST_FORM(HostCvttps2dq, "cvttps2dq")
HOST_FORM(HostCvtpd2dq, "cvtpd2dq")
HOST_FORM(HostRoundps, "roundps $4,")
HOST_FORM(HostCvtph2ps, "vcvtph2ps")

// vcvtps2ph with each rounding control (imm 0-4).
template <int Imm>
void HostCvtps2ph(uint32_t& mxcsr, Xmm& d, const Xmm& s) {
  asm volatile("ldmxcsr %0\n\tvcvtps2ph %2, %3, %1\n\tstmxcsr %0"
               : "+m"(mxcsr), "+x"(*reinterpret_cast<V4*>(d.b))
               : "i"(Imm), "x"(*reinterpret_cast<const V4*>(s.b)));
}
void (*const kHostCvtps2ph[5])(uint32_t&, Xmm&, const Xmm&) = {
    HostCvtps2ph<0>, HostCvtps2ph<1>, HostCvtps2ph<2>, HostCvtps2ph<3>, HostCvtps2ph<4>};

// Singles around the half range: its denormals and their bottom (float
// exponents 101-112), its normals, its top and past it (to 143), float
// denormals, infinities and NaNs.
uint32_t HalfRangeFloat(std::mt19937_64& g) {
  static const uint32_t kExp[] = {0, 101, 102, 103, 105, 108, 110, 111, 112, 113, 127, 140, 142, 143, 144, 255};
  const uint32_t e = kExp[g() % 16], frac = g() % 4 ? uint32_t(g()) & 0x7FFFFF : g() & 7;
  return uint32_t(g() & 1) << 31 | e << 23 | frac;
}

// Biased exponents that reach the denormal range: denormal and near-min
// operands (add, sub), about half the range down (mul), the top (div).
uint32_t Float(std::mt19937_64& g) {
  static const uint32_t kExp[] = {0, 1, 2, 3, 60, 63, 66, 190, 200};
  const uint32_t e = kExp[g() % 9], frac = g() % 4 ? uint32_t(g()) & 0x7FFFFF : g() & 7;
  return uint32_t(g() & 1) << 31 | e << 23 | frac;
}
uint64_t Double(std::mt19937_64& g) {
  static const uint64_t kExp[] = {0, 1, 2, 3, 500, 511, 520, 1500, 2000};
  const uint64_t e = kExp[g() % 9], frac = g() % 4 ? g() & 0xFFFFFFFFFFFFFull : g() & 7;
  return (g() & 1) << 63 | e << 52 | frac;
}
// Doubles that convert to the bottom of the float range.
uint64_t SmallDouble(std::mt19937_64& g) {
  const uint64_t e = 1023 - 150 + g() % 30;
  return (g() & 1) << 63 | e << 52 | (g() & 0xFFFFFFFFFFFFFull);
}

void Check(const char* form, uint32_t mxcsr, const Xmm& d, const Xmm& s, const Xmm& got,
           uint32_t got_mxcsr, const Xmm& want, uint32_t want_mxcsr) {
  if (!std::memcmp(got.b, want.b, 16) && got_mxcsr == want_mxcsr) return;
  if (++failures > 10) return;
  auto hex = [](const Xmm& x) {
    static char s[2][40];
    static int k;
    k ^= 1;
    for (int i = 0; i < 16; i++) std::snprintf(s[k] + 2 * i, 3, "%02X", x.b[15 - i]);
    return s[k];
  };
  std::printf("FAIL %s mxcsr=%04X d=%s s=%s\n", form, mxcsr, hex(d), hex(s));
  std::printf("   host %s mxcsr=%04X\n", hex(want), want_mxcsr);
  std::printf("   lift %s mxcsr=%04X\n", hex(got), got_mxcsr);
}

}  // namespace

int main() {
  std::mt19937_64 g(1130);
  struct {
    const char* name;
    sse::Op op;
    void (*host)(uint32_t&, Xmm&, const Xmm&);
  } const kFloat[] = {{"addps", sse::kAdd, HostAddps}, {"subps", sse::kSub, HostSubps},
                      {"mulps", sse::kMul, HostMulps}, {"divps", sse::kDiv, HostDivps}},
          kDouble[] = {{"addpd", sse::kAdd, HostAddpd}, {"subpd", sse::kSub, HostSubpd},
                       {"mulpd", sse::kMul, HostMulpd}, {"divpd", sse::kDiv, HostDivpd}};
  long flushed = 0, zeroed = 0;
  for (int t = 0; t < 200000; t++) {
    const uint32_t mxcsr = sse::kMxcsrDefault | uint32_t(g() % 4) << 13 | (g() % 4 ? 0x8000 : 0) |
                           (g() % 2 ? 0x40 : 0);
    Xmm d, s, sd;
    for (int i = 0; i < 4; i++) {
      sse::SetLane<uint32_t>(d, i, Float(g));
      sse::SetLane<uint32_t>(s, i, Float(g));
    }
    for (int i = 0; i < 2; i++) sse::SetLane<uint64_t>(sd, i, SmallDouble(g));
    for (bool dbl : {false, true}) {
      Xmm dd = d, ss = s;
      if (dbl)
        for (int i = 0; i < 2; i++) {
          sse::SetLane<uint64_t>(dd, i, Double(g));
          sse::SetLane<uint64_t>(ss, i, Double(g));
        }
      for (const auto& f : dbl ? kDouble : kFloat) {
        Xmm want = dd, got = dd;
        uint32_t want_mxcsr = mxcsr;
        f.host(want_mxcsr, want, ss);
        Ctx c{};
        c.mxcsr = mxcsr;
        sse::Arith(c, f.op, dbl, dbl ? 2 : 4, got, ss, 0);
        Check(f.name, mxcsr, dd, ss, got, c.mxcsr, want, want_mxcsr);
        if ((mxcsr & 0x8000) && (want_mxcsr & 0x10)) flushed++;
      }
    }
    Xmm want = d, got = d;
    uint32_t want_mxcsr = mxcsr;
    HostCvtpd2ps(want_mxcsr, want, sd);
    Ctx c{};
    c.mxcsr = mxcsr;
    sse::CvtFloat(c, false, 2, got, sd, 0);
    Check("cvtpd2ps", mxcsr, d, sd, got, c.mxcsr, want, want_mxcsr);

    // The other forms, on the same float operands and doubles from them.
    Xmm dd, ss;
    for (int i = 0; i < 2; i++) {
      sse::SetLane<uint64_t>(dd, i, Double(g));
      sse::SetLane<uint64_t>(ss, i, Double(g));
    }
    bool den = false;
    for (int i = 0; i < 4; i++) den |= !(sse::Lane<uint32_t>(s, i) & 0x7F800000u);
    if ((mxcsr & 0x40) && den) zeroed++;
    struct {
      const char* name;
      void (*host)(uint32_t&, Xmm&, const Xmm&);
      bool dbl;
      void (*lift)(Ctx&, Xmm&, const Xmm&);
    } const kForms[] = {
        {"sqrtps", HostSqrtps, false, [](Ctx& c, Xmm& d, const Xmm& s) { sse::Sqrt(c, false, 4, d, s, 0); }},
        {"sqrtpd", HostSqrtpd, true, [](Ctx& c, Xmm& d, const Xmm& s) { sse::Sqrt(c, true, 2, d, s, 0); }},
        {"minps", HostMinps, false, [](Ctx& c, Xmm& d, const Xmm& s) { sse::MinMax(c, false, false, 4, d, s, 0); }},
        {"maxpd", HostMaxpd, true, [](Ctx& c, Xmm& d, const Xmm& s) { sse::MinMax(c, true, true, 2, d, s, 0); }},
        {"cmpltps", HostCmpltps, false, [](Ctx& c, Xmm& d, const Xmm& s) { sse::Cmp(c, false, 4, d, s, 1, 0); }},
        {"cmplepd", HostCmplepd, true, [](Ctx& c, Xmm& d, const Xmm& s) { sse::Cmp(c, true, 2, d, s, 2, 0); }},
        {"cvtps2pd", HostCvtps2pd, false, [](Ctx& c, Xmm& d, const Xmm& s) { sse::CvtFloat(c, true, 2, d, s, 0); }},
        {"cvttps2dq", HostCvttps2dq, false, [](Ctx& c, Xmm& d, const Xmm& s) { sse::CvtToDq(c, false, true, d, s, 0); }},
        {"cvtpd2dq", HostCvtpd2dq, true, [](Ctx& c, Xmm& d, const Xmm& s) { sse::CvtToDq(c, true, false, d, s, 0); }},
        {"roundps", HostRoundps, false, [](Ctx& c, Xmm& d, const Xmm& s) { sse::Round(c, false, 4, d, s, 4, 0); }},
    };
    for (const auto& f : kForms) {
      const Xmm& a = f.dbl ? dd : d;
      const Xmm& b = f.dbl ? ss : s;
      Xmm want = a, got = a;
      uint32_t want_mxcsr = mxcsr;
      f.host(want_mxcsr, want, b);
      Ctx c{};
      c.mxcsr = mxcsr;
      f.lift(c, got, b);
      Check(f.name, mxcsr, a, b, got, c.mxcsr, want, want_mxcsr);
    }
  }
  // F16C: every half to single, DAZ and FTZ set and clear.
  long half_denormals = 0;
  for (uint32_t m : {0u, 0x40u, 0x8000u, 0x8040u}) {
    for (uint32_t h = 0; h < 0x10000; h += 4) {
      const uint32_t mxcsr = sse::kMxcsrDefault | m;
      Xmm s{}, want{}, got{};
      for (int i = 0; i < 4; i++) sse::SetLane<uint16_t>(s, i, uint16_t(h + i));
      for (int i = 4; i < 8; i++) sse::SetLane<uint16_t>(s, i, uint16_t(g()));
      uint32_t want_mxcsr = mxcsr;
      HostCvtph2ps(want_mxcsr, want, s);
      Ctx c{};
      c.mxcsr = mxcsr;
      sse::Cvtph2ps(c, got, s, 0, 0);
      Check("vcvtph2ps", mxcsr, Xmm{}, s, got, c.mxcsr, want, want_mxcsr);
      if ((m & 0x40) && !(h & 0x7C00) && (h & 0x3FF)) half_denormals++;
    }
  }
  // Singles to half: each rounding control, MXCSR's modes, DAZ and FTZ.
  long half_tiny = 0;
  for (int t = 0; t < 200000; t++) {
    const uint32_t mxcsr = sse::kMxcsrDefault | uint32_t(g() % 4) << 13 | (g() % 2 ? 0x8000 : 0) |
                           (g() % 2 ? 0x40 : 0);
    const int imm = int(g() % 5);
    Xmm s, d;
    for (int i = 0; i < 4; i++) {
      sse::SetLane<uint32_t>(s, i, HalfRangeFloat(g));
      sse::SetLane<uint32_t>(d, i, uint32_t(g()));
    }
    Xmm want = d, got{};
    uint32_t want_mxcsr = mxcsr;
    kHostCvtps2ph[imm](want_mxcsr, want, s);
    Ctx c{};
    c.mxcsr = mxcsr;
    sse::Cvtps2ph(c, got, 0, s, uint8_t(imm), 0);
    char name[24];
    std::snprintf(name, sizeof name, "vcvtps2ph %d", imm);
    Check(name, mxcsr, d, s, got, c.mxcsr, want, want_mxcsr);
    for (int i = 0; i < 4; i++) {
      const uint16_t r = sse::Lane<uint16_t>(want, i);
      if (!(r & 0x7C00) && (r & 0x3FF)) half_tiny++;
    }
  }
  if (half_denormals < 1000 || half_tiny < 10000) {
    std::printf("FAIL F16C cases not reached (%ld denormal halves under DAZ, %ld denormal results)\n",
                half_denormals, half_tiny);
    failures++;
  }

  // The operands must reach the case: results flushed on the host.
  if (flushed < 10000) {
    std::printf("FAIL only %ld results underflowed under FTZ\n", flushed);
    failures++;
  }
  if (zeroed < 10000) {
    std::printf("FAIL only %ld runs with denormal operands under DAZ\n", zeroed);
    failures++;
  }
  if (failures) return 1;
  std::printf("sse ftz: ok (%ld underflowed under FTZ, %ld runs with denormal operands under DAZ)\n",
              flushed, zeroed);
  return 0;
}
