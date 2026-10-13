// Checks the x87 model's integer fast paths (add/sub/mul/div of normal
// operands, fst m32/m64, fsin/fcos/fsincos) against the SoftFloat paths
// they bypass (x87::g_softfloat_only), then the inline forms lifted runs
// call (x87::run::*Fast: arithmetic with register, m32 and m64 operands,
// m32/m64 loads and stores, compares) against the out-of-line run
// functions on the SoftFloat path.
// Portable (no host FPU), so it also runs on arm64. Every case runs one
// instruction on identical states both ways and compares the registers,
// the status word (flags, C1) and the tag word bit for bit. The SoftFloat
// path itself is checked against the hardware by x87_test.
//
// Usage: x87_fast_test [cases] [seed]

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "runtime/x87.h"

namespace {

using recomp::x87::F80;
using recomp::x87::Op;
using recomp::x87::State;

std::mt19937_64 g_rng;

uint64_t Rand() { return g_rng(); }

// A normal significand: random, or with only the top 24/53 bits set
// (values that came from m32/m64), or close to a rounding tie.
uint64_t Sig() {
  uint64_t s = Rand() | 0x8000000000000000ull;
  switch (Rand() % 8) {
    case 0: return s & ~((1ull << 40) - 1);           // float-exact
    case 1: return s & ~((1ull << 11) - 1);           // double-exact
    case 2: return (s & ~((1ull << 40) - 1)) | (1ull << 39);  // PC24 tie
    case 3: return (s & ~((1ull << 11) - 1)) | (1ull << 10);  // PC53 tie
    case 4: return 0x8000000000000000ull;             // power of two
    case 5: return ~0ull;                             // all ones
    case 6: return s | ((1ull << 40) - 1);            // just below a step
    default: return s;
  }
}

// Trig operands: mostly everyday angles, some needing reduction, some
// close to multiples of pi/2.
F80 Angle() {
  F80 v{Sig(), uint16_t(0x3FFF - 30 + int(Rand() % 52))};
  if (Rand() % 4 == 0) v.se = uint16_t(0x3FFF - 2 + int(Rand() % 6));
  if (Rand() % 8 == 0) {  // k * pi/2 nudged: 0xC90FDAA22168C235 * 2^-63 = pi/2 * ...
    static const uint64_t kHalfPi = 0xC90FDAA22168C235ull;
    int k = 1 + int(Rand() % 16);
    unsigned __int128 p = (unsigned __int128)kHalfPi * k;
    int sh = 0;
    while (p >> (64 + sh)) ++sh;
    v = {uint64_t(p >> sh) ^ (Rand() % 1024), uint16_t(0x3FFF + sh)};
  }
  if (Rand() & 1) v.se |= 0x8000;
  return v;
}

int Exp(int near) {
  switch (Rand() % 7) {
    case 0: return 1 + int(Rand() % 0x7FFE);                  // anywhere
    case 1: return 1 + int(Rand() % 80);                      // near underflow
    case 2: return 0x7FFE - int(Rand() % 80);                 // near overflow
    case 3: return 0x3FFF - 200 + int(Rand() % 400);          // everyday
    case 4: {  // the m32/m64 range edges (stores)
      static const int kEdges[] = {0x3FFF + 127, 0x3FFF - 126, 0x3FFF + 1023,
                                   0x3FFF - 1022};
      return kEdges[Rand() % 4] + int(Rand() % 5) - 2;
    }
    default: {
      int e = near + int(Rand() % 141) - 70;                  // close to the other
      return e < 1 ? 1 : e > 0x7FFE ? 0x7FFE : e;
    }
  }
}

F80 Value(int near) {
  F80 v{Sig(), uint16_t(Exp(near))};
  if (Rand() % 12 == 0) v = {0, 0};  // zero
  if (Rand() & 1) v.se |= 0x8000;
  return v;
}

struct Snap {
  F80 r[8];
  uint16_t sw, tw;
};

// k < 6: arithmetic; 6: fst m32; 7: fst m64 (result in r[0].sig);
// 8: fsin, 9: fcos, 10: fsincos.
Snap Run(bool softfloat_only, uint16_t cw, F80 a, F80 b, int k, bool st0_first) {
  static const Op kOps[] = {Op::Add, Op::Mul, Op::Sub, Op::SubR, Op::Div, Op::DivR};
  recomp::x87::g_softfloat_only = softfloat_only;
  State s;
  recomp::x87::Init(s);
  recomp::x87::SetControlWord(s, cw);
  recomp::x87::LoadF80(s, b);
  recomp::x87::LoadF80(s, a);  // st0 = a, st1 = b
  uint64_t stored = 0;
  if (k == 6)
    stored = recomp::x87::StoreF32(s);
  else if (k == 7)
    stored = recomp::x87::StoreF64(s);
  else if (k == 8)
    recomp::x87::Sin(s);
  else if (k == 9)
    recomp::x87::Cos(s);
  else if (k == 10)
    recomp::x87::SinCos(s);
  else if (st0_first)
    recomp::x87::ArithSt0Sti(s, kOps[k], 1);
  else
    recomp::x87::ArithStiSt0(s, kOps[k], 1);
  Snap r{};
  for (int i = 0; i < 8; i++) r.r[i] = s.r[i];
  r.r[0].sig ^= stored;  // empty slot after two pushes; carries the store
  r.sw = recomp::x87::StatusWord(s);
  r.tw = recomp::x87::TagWord(s);
  return r;
}

bool Same(const Snap& x, const Snap& y) {
  if (x.sw != y.sw || x.tw != y.tw) return false;
  for (int i = 0; i < 8; i++)
    if (x.r[i].sig != y.r[i].sig || x.r[i].se != y.r[i].se) return false;
  return true;
}

// ---- inline run forms ----------------------------------------------------

namespace run = recomp::x87::run;

// A register value: mostly what Value() gives, with 24/53-bit significands
// often, and every special class now and then.
run::V RunValue(int near) {
  F80 v = Value(near);
  switch (Rand() % 24) {
    case 0: v = {0x8000000000000000ull, 0x7FFF}; break;                // inf
    case 1: v = {0xC000000000000000ull | (Rand() >> 2), 0x7FFF}; break; // QNaN
    case 2: v = {0x8000000000000001ull | (Rand() >> 2 & ~(1ull << 62)), 0x7FFF}; break;  // SNaN
    case 3: v = {Rand() >> 1, 0}; break;                                // denormal
    case 4: v = {Rand() | 0x8000000000000000ull, 0}; break;            // pseudo-denormal
    case 5: v = {Rand() >> 1, uint16_t(1 + Rand() % 0x7FFE)}; break;   // unsupported
    case 6: case 7: case 8: v.sig &= ~((1ull << 40) - 1); break;
    case 9: case 10: case 11: v.sig &= ~((1ull << 11) - 1); break;
    case 12: case 13: case 14:  // sparse 53-bit: exact results, ties, carries
      v.sig = 0x8000000000000000ull | (Rand() & Rand() & Rand() & ~((1ull << 11) - 1));
      break;
    case 15: case 16:  // leading ones, then sparse: sums that carry out
      v.sig = (~0ull << (11 + Rand() % 53)) | (Rand() & Rand() & Rand() & ~((1ull << 11) - 1));
      break;
    default: break;
  }
  if (Rand() & 1) v.se ^= 0x8000;
  return run::ToV(v);
}

// m32/m64 operands: normal (often near the other operand), zero,
// denormal, inf, NaN, raw bits.
uint32_t RandF32(int near) {
  uint32_t sign = uint32_t(Rand() & 1) << 31;
  switch (Rand() % 12) {
    case 0: return sign;
    case 1: return sign | uint32_t(Rand() & 0x7FFFFF);
    case 2: return sign | 0x7F800000u;
    case 3: return sign | 0x7F800000u | uint32_t(1 + Rand() % 0x7FFFFF);
    case 4: return uint32_t(Rand());
    default: {
      int e = near - 0x3FFF + 127 + int(Rand() % 61) - 30;
      if (Rand() % 4 == 0) e = 1 + int(Rand() % 254);
      e = e < 1 ? 1 : e > 254 ? 254 : e;
      return sign | uint32_t(e) << 23 | uint32_t(Rand() & 0x7FFFFF);
    }
  }
}
uint64_t RandF64(int near) {
  uint64_t sign = (Rand() & 1) << 63;
  switch (Rand() % 12) {
    case 0: return sign;
    case 1: return sign | (Rand() & 0xFFFFFFFFFFFFFull);
    case 2: return sign | 0x7FF0000000000000ull;
    case 3: return sign | 0x7FF0000000000000ull | (1 + Rand() % 0xFFFFFFFFFFFFFull);
    case 4: return Rand();
    default: {
      int e = near - 0x3FFF + 1023 + int(Rand() % 141) - 70;
      if (Rand() % 4 == 0) e = 1 + int(Rand() % 2046);
      e = e < 1 ? 1 : e > 2046 ? 2046 : e;
      uint64_t f = Rand() & 0xFFFFFFFFFFFFFull;
      if (Rand() % 3 == 0) f &= ~((1ull << 29) - 1);  // float-exact
      return sign | uint64_t(e) << 52 | f;
    }
  }
}

bool SameRV(run::RV x, run::RV y) { return x.sig == y.sig && x.meta == y.meta; }
bool SameSV(run::SV x, run::SV y) { return x.bits == y.bits && x.meta == y.meta; }

// One random case of every inline form; the count of mismatches.
long RunForms(long n) {
  static const Op kOps[] = {Op::Add, Op::Mul, Op::Sub, Op::SubR, Op::Div, Op::DivR};
  static const uint16_t kPc[] = {0x0000, 0x0200, 0x0300};
  run::V a = RunValue(0x3FFF);
  run::V b = RunValue(int(a.se & 0x7FFF));
  if (Rand() % 4 == 0) {  // close to a: cancellation, equal compares
    b = a;
    b.se = (b.se & 0x8000) | ((a.se + Rand() % 3 - 1) & 0x7FFF);
    b.sig ^= (Rand() % 4) << ((Rand() & 1) ? 11 : 40);
    if (Rand() & 1) b.se ^= 0x8000;
  }
  int near = int(a.se & 0x7FFF);
  uint32_t m32 = RandF32(near);
  uint64_t m64 = RandF64(near);
  uint16_t cw = uint16_t(0x007F | kPc[Rand() % 3] | (Rand() % 4) << 10);
  if (Rand() % 2) cw = Rand() & 1 ? 0x027F : uint16_t(0x007F | (Rand() % 4) << 10);
  Op op = kOps[Rand() % 6];
  long fails = 0;
  auto report = [&](const char* what, uint64_t f1, uint64_t f2, uint64_t s1, uint64_t s2) {
    if (++fails, n < 20)
      std::printf("FAIL %s cw=%04X a=%04X:%016" PRIX64 " b=%04X:%016" PRIX64 " m32=%08X"
                  " m64=%016" PRIX64 "\n  fast %016" PRIX64 " %016" PRIX64
                  "\n  soft %016" PRIX64 " %016" PRIX64 "\n",
                  what, cw, uint32_t(a.se), a.sig, uint32_t(b.se), b.sig, m32, m64, f1, f2, s1,
                  s2);
  };
  auto arith = [&](const char* what, run::RV fast, auto&& ref) {
    recomp::x87::g_softfloat_only = true;
    run::RV soft = ref();
    recomp::x87::g_softfloat_only = false;
    if (!SameRV(fast, soft)) report(what, fast.sig, fast.meta, soft.sig, soft.meta);
  };
  auto store = [&](const char* what, run::SV fast, auto&& ref) {
    recomp::x87::g_softfloat_only = true;
    run::SV soft = ref();
    recomp::x87::g_softfloat_only = false;
    if (!SameSV(fast, soft)) report(what, fast.bits, fast.meta, soft.bits, soft.meta);
  };
  auto cmp = [&](const char* what, uint32_t fast, uint32_t soft) {
    if (fast != soft) report(what, fast, 0, soft, 0);
  };
  arith("ArithFast", run::ArithFast(cw, op, a, b), [&] { return run::Arith(cw, op, a, b); });
  arith("ArithF32Fast", run::ArithF32Fast(cw, op, a, m32),
        [&] { return run::ArithF32(cw, op, a, m32); });
  arith("ArithF64Fast", run::ArithF64Fast(cw, op, a, m64),
        [&] { return run::ArithF64(cw, op, a, m64); });
  arith("LoadF32Fast", run::LoadF32Fast(m32), [&] { return run::LoadF32(m32); });
  arith("LoadF64Fast", run::LoadF64Fast(m64), [&] { return run::LoadF64(m64); });
  store("StoreF32Fast", run::StoreF32Fast(cw, a), [&] { return run::StoreF32(cw, a); });
  store("StoreF64Fast", run::StoreF64Fast(cw, a), [&] { return run::StoreF64(cw, a); });
  bool quiet = Rand() & 1;
  cmp("CompareFast", run::CompareFast(a, b, quiet), run::Compare(a, b, quiet));
  cmp("CompareF32Fast", run::CompareF32Fast(a, m32), run::CompareF32(a, m32));
  cmp("CompareF64Fast", run::CompareF64Fast(a, m64), run::CompareF64(a, m64));
  return fails;
}

}  // namespace

int main(int argc, char** argv) {
  long cases = argc > 1 ? std::atol(argv[1]) : 2000000;
  g_rng.seed(argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1);
  static const char* kNames[] = {"add", "mul", "sub", "subr", "div", "divr",
                                 "fst m32", "fst m64", "fsin", "fcos", "fsincos"};
  static const uint16_t kPc[] = {0x0000, 0x0200, 0x0300};
  long fails = 0;
  for (long n = 0; n < cases; n++) {
    F80 a = Value(0x3FFF);
    F80 b = Value(a.se & 0x7FFF);
    if (Rand() % 16 == 0) b = {a.sig ^ (Rand() % 4), a.se};  // cancellation
    uint16_t cw = uint16_t(0x007F | kPc[Rand() % 3] | (Rand() % 4) << 10);
    if (Rand() % 4 == 0) {  // games' common modes: PC=24 (any RC), 24-bit operands,
      // or PC=53 with RC=nearest, 53-bit operands
      bool pc53 = Rand() & 1;
      cw = pc53 ? 0x027F : uint16_t(0x007F | (Rand() % 4) << 10);
      const uint64_t low = pc53 ? (1ull << 11) - 1 : (1ull << 40) - 1;
      a.sig = (a.sig | 0x8000000000000000ull) & ~low;
      b.sig = (b.sig | 0x8000000000000000ull) & ~low;
      if (!(a.se & 0x7FFF)) a.se |= 0x3FFF;
      if (!(b.se & 0x7FFF)) b.se |= 0x3FFF;
    }
    int k = int(Rand() % 11);
    if (k >= 8) a = Angle();
    bool st0_first = Rand() & 1;
    Snap fast = Run(false, cw, a, b, k, st0_first);
    Snap ref = Run(true, cw, a, b, k, st0_first);
    if (Same(fast, ref)) continue;
    if (++fails <= 20) {
      int dst = st0_first ? 0 : 1;  // physical reg after the two pushes
      int p = k == 6 || k == 7 ? 0 : k >= 8 ? 6 : (6 + dst) & 7;
      std::printf("FAIL %s%s cw=%04X a=%04X:%016" PRIX64 " b=%04X:%016" PRIX64
                  "\n  fast %04X:%016" PRIX64 " sw=%04X\n  soft %04X:%016" PRIX64
                  " sw=%04X\n",
                  kNames[k], st0_first ? "" : " (st1 dst)", cw, a.se, a.sig, b.se,
                  b.sig, fast.r[p].se, fast.r[p].sig, fast.sw, ref.r[p].se,
                  ref.r[p].sig, ref.sw);
    }
  }
  std::printf("%ld cases, %ld mismatches\n", cases, fails);
  long run_fails = 0;
  for (long n = 0; n < cases; n++) run_fails += RunForms(run_fails);
  std::printf("%ld cases of the inline run forms, %ld mismatches\n", cases, run_fails);
  return fails || run_fails ? 1 : 0;
}
