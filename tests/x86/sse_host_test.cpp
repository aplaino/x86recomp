// sse_host_test: SSE arithmetic's host path (sse.cpp, HostArith) against
// the model alone (ArithModel): ADD/SUB/MUL/DIV on 1 or 4 floats and 1 or
// 2 doubles, with MXCSR as games keep it, with and without FTZ and DAZ,
// and in modes the host path must leave to the model (another rounding
// control, PE clear; unmasked exceptions are fatal in the model, #XM not
// being modelled, and refused by the same check). Lanes come from edge values
// (zeros, denormals, the normal range's ends, infinities, NaNs), random
// values near the bottom of the range (denormal results, products and
// quotients that underflow, sums that round into or out of it) and random
// bits. The register and MXCSR must match bit for bit. Built with a
// planted bug of the host path (RECOMP_SSE_MUTATION) it must fail.
// Portable.
//
// Usage: sse_host_test [cases] [seed]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "x86/sse.h"

namespace {

using namespace recomp;

std::mt19937_64 g_rng;

template <bool D>
using U = std::conditional_t<D, uint64_t, uint32_t>;

template <bool D>
std::vector<U<D>> Edges() {
  if constexpr (D) {
    return {0, 1, 2, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x0010000000000001ull,
            0x0020000000000000ull, 0x3FF0000000000000ull, 0x3FF0000000000001ull,
            0x3FEFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull, 0x7FE0000000000000ull,
            0x7FF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull,
            0x1FF0000000000000ull, 0x2000000000000000ull, 0x5FF0000000000000ull};
  } else {
    return {0, 1, 2, 0x007FFFFFu, 0x00800000u, 0x00800001u, 0x01000000u, 0x3F800000u,
            0x3F800001u, 0x3F7FFFFFu, 0x7F7FFFFFu, 0x7F000000u, 0x7F800000u, 0x7FC00000u,
            0x7F800001u, 0x1F800000u, 0x20000000u, 0x5F800000u};
  }
}

// A lane: an edge, a value near the bottom of the range or around its
// square root (so products and quotients land there), or random bits;
// either sign.
template <bool D>
U<D> Lane() {
  using T = U<D>;
  constexpr int kFracBits = D ? 52 : 23, kBias = D ? 1023 : 127;
  static const std::vector<T> edges = Edges<D>();
  const T sign = T(g_rng() & 1) << (sizeof(T) * 8 - 1);
  const T frac = T(g_rng()) & ((T(1) << kFracBits) - 1);
  switch (g_rng() % 6) {
    case 0:
      return edges[g_rng() % edges.size()] | sign;
    case 1:  // exponent 0..3: denormals and the smallest normals
      return sign | T(g_rng() % 4) << kFracBits | frac;
    case 2: {  // around the square root of the smallest normal
      const int e = kBias / 2 + int(g_rng() % 8) - 4;
      return sign | T(e) << kFracBits | frac;
    }
    case 3: {  // around 1 and 2^kFracBits (sums that cancel or round)
      const int e = kBias + int(g_rng() % 4) - 2;
      return sign | T(e) << kFracBits | frac;
    }
    case 4: {  // near the top (overflow)
      const int e = 2 * kBias - int(g_rng() % 4);
      return sign | T(e) << kFracBits | frac;
    }
    default:
      return T(g_rng());
  }
}

int g_failures = 0;

// A pair whose product or quotient lands at the smallest normal: one
// near it, the other within a few ulps of 1.
template <bool D>
void Boundary(U<D>* a, U<D>* b) {
  using T = U<D>;
  constexpr int kFracBits = D ? 52 : 23;
  constexpr T kOne = D ? 0x3FF0000000000000ull : 0x3F800000u;
  *a = T(1) << kFracBits | T(g_rng() % 8);
  *b = g_rng() & 1 ? kOne - 1 - T(g_rng() % 8) : kOne + T(g_rng() % 8);
  *a |= T(g_rng() & 1) << (sizeof(T) * 8 - 1);
}

template <bool D>
void Case(uint32_t mxcsr, sse::Op op, int lanes) {
  Xmm d{}, s{};
  for (int i = 0; i < 16 / int(sizeof(U<D>)); i++) {
    // Pairs from one generator often: an edge against itself, a value
    // against a nearby one, the smallest normal's boundary.
    U<D> a = Lane<D>(), b = g_rng() % 4 ? Lane<D>() : a ^ (U<D>(g_rng()) & 0xF);
    if (g_rng() % 8 == 0) Boundary<D>(&a, &b);
    std::memcpy(d.b + i * sizeof a, &a, sizeof a);
    std::memcpy(s.b + i * sizeof b, &b, sizeof b);
  }
  Ctx host{}, model{};
  host.mxcsr = model.mxcsr = mxcsr;
  Xmm got = d, want = d;
  sse::Arith(host, op, D, lanes, got, s, 0);
  sse::ArithModel(model, op, D, lanes, want, s, 0);
  if (std::memcmp(got.b, want.b, 16) || host.mxcsr != model.mxcsr) {
    if (g_failures++ < 10) {
      std::printf("FAIL %s op %d lanes %d mxcsr %04X: got mxcsr %04X, want %04X\n",
                  D ? "double" : "float", int(op), lanes, mxcsr, host.mxcsr, model.mxcsr);
      for (int i = 0; i < lanes; i++) {
        U<D> x, y, g, w;
        std::memcpy(&x, d.b + i * sizeof x, sizeof x);
        std::memcpy(&y, s.b + i * sizeof y, sizeof y);
        std::memcpy(&g, got.b + i * sizeof g, sizeof g);
        std::memcpy(&w, want.b + i * sizeof w, sizeof w);
        std::printf("  lane %d: %llX op %llX = %llX, want %llX\n", i, (unsigned long long)x,
                    (unsigned long long)y, (unsigned long long)g, (unsigned long long)w);
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const long cases = argc > 1 ? std::atol(argv[1]) : 2000000;
  g_rng.seed(argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1);
  // The host path's modes (PE raised, every exception masked, round to
  // nearest; FTZ and DAZ either way; other flags raised or not), then
  // modes it must leave to the model.
  static const uint32_t kModes[] = {0x1FA0, 0x9FA0, 0x1FE0, 0x9FE0, 0x9FBF, 0x1FA2,
                                    0x1F80, 0x9FC0, 0x3FA0, 0x5FA0, 0x7FA0, 0x9FE1};
  static const sse::Op kOps[] = {sse::kAdd, sse::kSub, sse::kMul, sse::kDiv};
  for (long i = 0; i < cases; i++) {
    const uint32_t mxcsr = kModes[g_rng() % (sizeof kModes / sizeof *kModes)];
    const sse::Op op = kOps[g_rng() % 4];
    if (g_rng() & 1)
      Case<true>(mxcsr, op, g_rng() & 1 ? 1 : 2);
    else
      Case<false>(mxcsr, op, g_rng() & 1 ? 1 : 4);
  }
  std::printf("sse_host_test: %ld cases, %d failures\n", cases, g_failures);
#if RECOMP_SSE_MUTATION
  // A planted bug must be caught.
  return g_failures ? 0 : 1;
#else
  return g_failures ? 1 : 0;
#endif
}
