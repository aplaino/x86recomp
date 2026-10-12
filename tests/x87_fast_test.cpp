// Checks the x87 model's integer fast paths (add/sub/mul/div of normal
// operands, fst m32/m64, fsin/fcos/fsincos) against the SoftFloat paths
// they bypass (x87::g_softfloat_only).
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
    if (Rand() % 4 == 0) {  // the game's case: PC=24 (RC=chop there), 24-bit operands
      cw = uint16_t(0x007F | (Rand() % 4) << 10);
      a.sig = (a.sig | 0x8000000000000000ull) & ~((1ull << 40) - 1);
      b.sig = (b.sig | 0x8000000000000000ull) & ~((1ull << 40) - 1);
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
  return fails ? 1 : 0;
}
