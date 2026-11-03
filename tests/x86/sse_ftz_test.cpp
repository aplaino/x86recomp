// sse_ftz_test: SSE arithmetic and CVTPD2PS under FTZ against the host,
// from operands near the bottom of the range (denormal results, exact or
// not, and ones that round into or out of it), every rounding mode, FTZ
// set and clear. x86-64 hosts only.

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
  long flushed = 0;
  for (int t = 0; t < 200000; t++) {
    const uint32_t mxcsr = sse::kMxcsrDefault | uint32_t(g() % 4) << 13 | (g() % 4 ? 0x8000 : 0);
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
  }
  // The operands must reach the case: results flushed on the host.
  if (flushed < 10000) {
    std::printf("FAIL only %ld results underflowed under FTZ\n", flushed);
    failures++;
  }
  if (failures) return 1;
  std::printf("sse ftz: ok (%ld underflowed under FTZ)\n", flushed);
  return 0;
}
