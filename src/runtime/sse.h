#pragma once
// SSE/SSE2 for lifted code. The XMM registers and MXCSR live in Ctx and
// are used in place. Floating-point lanes are computed with SoftFloat,
// with the SSE NaN rules applied here (the shared SoftFloat build uses the
// x87 ones), so results and MXCSR flags match the hardware bit for bit on
// any host. Integer and data-movement operations are plain portable C++.
//
// Not modelled, fatal if reached: unmasked SIMD exceptions (#XM), and FTZ
// or DAZ actually changing a result (a tiny result or a denormal operand
// while they are set).
//
// RSQRTPS has no architectural result, only an error bound (|rel. error|
// <= 1.5 * 2^-12), and CPUs differ: here it is the correctly rounded
// 1/sqrt(x), within the bound and the same on every host (and what a
// native replacement computes). tests/sse_rsqrt_test checks it
// exhaustively; the bit-for-bit oracle against the host skips it.
//
// Only the forms the supported games use are implemented; the lifter
// reports the rest as unimplemented.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "runtime/ctx.h"

namespace recomp::sse {

inline constexpr uint32_t kMxcsrDefault = 0x1F80;  // all masked, RN

template <typename T>
inline T Lane(const Xmm& x, int i) {
  T v;
  std::memcpy(&v, x.data() + i * sizeof(T), sizeof(T));
  return v;
}
template <typename T>
inline void SetLane(Xmm& x, int i, T v) {
  std::memcpy(x.data() + i * sizeof(T), &v, sizeof(T));
}

// `bytes` of guest memory at a, zero-extended to a register.
inline Xmm Ld(const uint8_t* B, uint64_t a, int bytes) {
  Xmm x{};
  std::memcpy(x.data(), B + a, bytes);
  return x;
}
inline void St(uint8_t* B, uint64_t a, const Xmm& x, int bytes) {
  std::memcpy(B + a, x.data(), bytes);
}
// Legacy-SSE m128 operands other than movups/movupd/movdqu must be 16-byte
// aligned (#GP otherwise): fatal, naming the instruction.
[[noreturn]] void Misaligned(uint64_t a, uint64_t va, unsigned align);
inline void CheckAligned(Ctx& c, uint64_t a, uint64_t va) {
  (void)c;
  if (a & 15) Misaligned(a, va, 16);
}
// The same for VEX moves that require `align` (16 or 32) bytes.
inline void CheckAligned(Ctx& c, uint64_t a, uint64_t va, unsigned align) {
  (void)c;
  if (a & (align - 1)) Misaligned(a, va, align);
}

// ---- floating point ---------------------------------------------------
enum Op : uint8_t { kAdd, kSub, kMul, kDiv };

// Fast paths, inline in lifted code. Every SSE instruction of a game runs
// through these, and nearly all see MXCSR as usual (exceptions masked,
// round to nearest, no FTZ or DAZ, the precision flag already raised) and
// operands and results that are normal or zero: then the host's IEEE
// arithmetic gives the same bits and no new flag, and the work is a few
// instructions at the call site (whose operation, precision and lane
// count are constants there). Anything else goes to the full model in
// sse.cpp (ArithModel, ComiModel).
//
// Whether MXCSR is as usual (`usual` below) is the lifted function's local
// MU (lifted.h: LIFT_ENTER, LIFT_RELOAD), not a load of c.mxcsr at every
// instruction: inside a function only LDMXCSR changes MXCSR's control bits
// (the lifter recomputes MU after it), and the models only raise sticky
// flags, which cannot make a usual MXCSR unusual. A stale false only
// sends lanes to the exact model.
//
// The models are out of line, cold, and take and return registers by
// value: a reference would take the address of the function's XMM locals
// (X0..X7), which then live in memory at every instruction instead of in
// host registers.
namespace fast {

template <bool D>
struct Bits;
template <>
struct Bits<false> {
  using U = uint32_t;
  using F = float;
  static constexpr U kExp = 0x7F800000u, kFrac = 0x007FFFFFu;
};
template <>
struct Bits<true> {
  using U = uint64_t;
  using F = double;
  static constexpr U kExp = 0x7FF0000000000000ull, kFrac = 0x000FFFFFFFFFFFFFull;
};

[[gnu::always_inline]] inline bool UsualMxcsr(uint32_t mxcsr) {
  return (mxcsr & 0xFFE0u) == 0x1FA0u;
}

template <bool D>
inline bool NormalOrZero(typename Bits<D>::U v) {
  const typename Bits<D>::U e = v & Bits<D>::kExp;
  return e != Bits<D>::kExp && (e || !(v & Bits<D>::kFrac));
}

// One lane of d op s, if the fast path covers it.
template <bool D>
inline bool ArithLane(Op op, typename Bits<D>::U a, typename Bits<D>::U b,
                      typename Bits<D>::U* out) {
  using U = typename Bits<D>::U;
  using F = typename Bits<D>::F;
  constexpr U kSign = U(1) << (sizeof(U) * 8 - 1);
  if (!NormalOrZero<D>(a) || !NormalOrZero<D>(b)) return false;
  const F x = std::bit_cast<F>(a), y = std::bit_cast<F>(b);
  F r;
  switch (op) {
    case kAdd: r = x + y; break;
    case kSub: r = x - y; break;
    case kMul: r = x * y; break;
    default: r = x / y; break;
  }
  const U v = std::bit_cast<U>(r);
  const U e = v & Bits<D>::kExp;
  if (e == Bits<D>::kExp) return false;  // overflow, a quotient by zero
  if (!e) {
    if (v & Bits<D>::kFrac) return false;  // denormal: underflow
    // A zero product or quotient of non-zero operands underflowed.
    if ((op == kMul || op == kDiv) && (a & ~kSign) && (b & ~kSign)) return false;
  }
  *out = v;
  return true;
}

// RECOMP_SSE_HOST (a game's opt-in, for its lifted code): lanes computed
// by the host without the checks. The same bits for every operand that is
// not a NaN, denormals and infinities included (host FPCR: no flush to
// zero, no default NaN); what differs is the sign and payload of the NaN
// an invalid operation makes (x86 0xFFC00000, the host 0x7FC00000) and
// MXCSR's exception flags, which nothing reads then. Requires MXCSR as
// usual, as the checked path does.
template <bool D>
inline void HostArith(Op op, int lanes, Xmm& d, const Xmm& s) {
  using F = typename Bits<D>::F;
  Xmm r = d;
  for (int i = 0; i < lanes; i++) {
    const F x = Lane<F>(d, i), y = Lane<F>(s, i);
    F v;
    switch (op) {
      case kAdd: v = x + y; break;
      case kSub: v = x - y; break;
      case kMul: v = x * y; break;
      default: v = x / y; break;
    }
    SetLane<F>(r, i, v);
  }
  d = r;
}

// RECOMP_SSE_HOST: CMPPS/PD/SS/SD by the host's compares (pred < 8).
template <bool D>
inline void HostCmp(int lanes, Xmm& d, const Xmm& s, uint8_t pred) {
  using U = typename Bits<D>::U;
  using F = typename Bits<D>::F;
  Xmm r = d;
  for (int i = 0; i < lanes; i++) {
    const F x = Lane<F>(d, i), y = Lane<F>(s, i);
    bool v;
    switch (pred) {
      case 0: v = x == y; break;
      case 1: v = x < y; break;
      case 2: v = x <= y; break;
      case 3: v = __builtin_isunordered(x, y); break;
      case 4: v = !(x == y); break;
      case 5: v = !(x < y); break;
      case 6: v = !(x <= y); break;
      default: v = !__builtin_isunordered(x, y); break;
    }
    SetLane<U>(r, i, v ? U(~U(0)) : U(0));
  }
  d = r;
}

// RECOMP_SSE_HOST: MINxx/MAXxx, d op s ? d : s on the lanes' bits.
template <bool D>
inline void HostMinMax(bool max, int lanes, Xmm& d, const Xmm& s) {
  using U = typename Bits<D>::U;
  using F = typename Bits<D>::F;
  Xmm r = d;
  for (int i = 0; i < lanes; i++) {
    const F x = Lane<F>(d, i), y = Lane<F>(s, i);
    const bool pick_d = max ? x > y : x < y;
    SetLane<U>(r, i, pick_d ? Lane<U>(d, i) : Lane<U>(s, i));
  }
  d = r;
}

// RECOMP_SSE_HOST: CVT(T)PS2DQ / CVT(T)PD2DQ (MXCSR as usual). A lane is
// converted by the host when its integer fits in int32; otherwise (NaN,
// out of range) it is the indefinite 0x80000000. True if any lane was
// (IE).
template <bool D>
inline bool HostCvtToDq(bool truncate, Xmm& d, const Xmm& s) {
  using F = typename Bits<D>::F;
  constexpr int kLanes = D ? 2 : 4;
  Xmm r{};
  bool bad = false;
  for (int i = 0; i < kLanes; i++) {
    F x = Lane<F>(s, i);
    bool ok;
    if (truncate) {
      // Truncation keeps values above -2^31 - 1 (only a double has one
      // between that and -2^31).
      ok = D ? x > F(-2147483649.0) : x >= F(-2147483648.0);
      ok = ok && x < F(2147483648.0);
    } else {
      x = D ? F(__builtin_roundeven(double(x))) : F(__builtin_roundevenf(float(x)));
      ok = x >= F(-2147483648.0) && x < F(2147483648.0);
    }
    const int32_t v = int32_t(ok ? x : F(0));
    SetLane<int32_t>(r, i, ok ? v : INT32_MIN);
    bad |= !ok;
  }
  d = r;
  return bad;
}

template <bool D>
inline bool Arith(Op op, int lanes, Xmm& d, const Xmm& s) {
  using U = typename Bits<D>::U;
  Xmm r = d;
  for (int i = 0; i < lanes; i++) {
    U v;
    if (!ArithLane<D>(op, Lane<U>(d, i), Lane<U>(s, i), &v)) return false;
    SetLane<U>(r, i, v);
  }
  d = r;
  return true;
}

}  // namespace fast

// d = d op s, lane-wise: `lanes` lanes of double (dbl) or float. Scalar
// forms pass lanes = 1 and keep the upper lanes of d.
[[gnu::cold]] Xmm ArithModel(Ctx& c, Op op, bool dbl, int lanes, Xmm d, Xmm s, uint64_t va);
// Always inlined: op, dbl and lanes are constants at every call site, so
// the host path folds to the one instruction (out of line, a call and a
// switch per SSE instruction).
[[gnu::always_inline]] inline void Arith(Ctx& c, bool usual, Op op, bool dbl, int lanes,
                                         Xmm& d, const Xmm& s, uint64_t va) {
#ifdef RECOMP_SSE_HOST
  if (usual) [[likely]] {
    if (dbl)
      fast::HostArith<true>(op, lanes, d, s);
    else
      fast::HostArith<false>(op, lanes, d, s);
    return;
  }
#endif
  if (usual &&
      (dbl ? fast::Arith<true>(op, lanes, d, s) : fast::Arith<false>(op, lanes, d, s)))
    [[likely]] return;
  d = ArithModel(c, op, dbl, lanes, d, s, va);
}

// CMPPS/PD/SS/SD: each lane of d becomes all ones if `d pred s` holds,
// else zero. pred is the imm8 (0 EQ, 1 LT, 2 LE, 3 UNORD, 4 NEQ, 5 NLT,
// 6 NLE, 7 ORD).
[[gnu::cold]] Xmm CmpModel(Ctx& c, bool dbl, int lanes, Xmm d, Xmm s, uint8_t pred,
                           uint64_t va);
[[gnu::always_inline]] inline void Cmp(Ctx& c, bool usual, bool dbl, int lanes, Xmm& d,
                                       const Xmm& s, uint8_t pred, uint64_t va) {
#ifdef RECOMP_SSE_HOST
  // The host's compares give every predicate's result, NaNs, zeros of
  // either sign and denormals included; what differs is only MXCSR's
  // IE/DE (see HostArith).
  if (usual && pred < 8) [[likely]] {
    if (dbl)
      fast::HostCmp<true>(lanes, d, s, pred);
    else
      fast::HostCmp<false>(lanes, d, s, pred);
    return;
  }
#endif
  d = CmpModel(c, dbl, lanes, d, s, pred, va);
}

// (U)COMISS/SD of the low lanes: 0 greater, 1 less, 2 equal, 3 unordered
// (the caller sets ZF/PF/CF). `ordered` = COMIS (invalid on QNaN too).
// Normal or zero operands raise nothing: compared in place.
[[gnu::cold]] int ComiModel(Ctx& c, bool dbl, bool ordered, Xmm a, Xmm b, uint64_t va);
// Whether the low lanes of a and b compare in place: then the host's
// ordered compare of them is Comi's result and the instruction changes no
// flag. Normal or zero operands; with RECOMP_SSE_HOST every operand but a
// NaN (unordered, maybe invalid), denormals' DE not kept (see HostArith).
// The lifter tests this before a jump on the flags of a (U)COMIS, then
// branches on the compare itself.
[[gnu::always_inline]] inline bool ComiDirect(bool dbl, const Xmm& a, const Xmm& b) {
#ifdef RECOMP_SSE_HOST
  if (dbl) return !__builtin_isunordered(Lane<double>(a, 0), Lane<double>(b, 0));
  return !__builtin_isunordered(Lane<float>(a, 0), Lane<float>(b, 0));
#else
  if (dbl)
    return fast::NormalOrZero<true>(Lane<uint64_t>(a, 0)) &&
           fast::NormalOrZero<true>(Lane<uint64_t>(b, 0));
  return fast::NormalOrZero<false>(Lane<uint32_t>(a, 0)) &&
         fast::NormalOrZero<false>(Lane<uint32_t>(b, 0));
#endif
}
[[gnu::always_inline]] inline int Comi(Ctx& c, bool dbl, bool ordered, const Xmm& a,
                                       const Xmm& b, uint64_t va) {
  if (ComiDirect(dbl, a, b)) {
    if (dbl) {
      const double p = Lane<double>(a, 0), q = Lane<double>(b, 0);
      return p > q ? 0 : p < q ? 1 : 2;
    }
    const float p = Lane<float>(a, 0), q = Lane<float>(b, 0);
    return p > q ? 0 : p < q ? 1 : 2;
  }
  return ComiModel(c, dbl, ordered, a, b, va);
}

// SQRTSD/SQRTSS/SQRTPD/SQRTPS: d = sqrt(s), lane-wise; scalar forms keep
// the upper lanes of d.
[[gnu::cold]] Xmm SqrtModel(Ctx& c, bool dbl, int lanes, Xmm d, Xmm s, uint64_t va);
[[gnu::always_inline]] inline void Sqrt(Ctx& c, bool usual, bool dbl, int lanes, Xmm& d,
                                        const Xmm& s, uint64_t va) {
#ifdef RECOMP_SSE_HOST
  // The host's square root is correctly rounded too: the same bits but for
  // the NaN of a negative operand (see HostArith).
  if (usual) [[likely]] {
    Xmm r = d;
    for (int i = 0; i < lanes; i++) {
      if (dbl)
        SetLane<double>(r, i, std::sqrt(Lane<double>(s, i)));
      else
        SetLane<float>(r, i, std::sqrt(Lane<float>(s, i)));
    }
    d = r;
    return;
  }
#endif
  d = SqrtModel(c, dbl, lanes, d, s, va);
}

// RSQRTPS: d = 1/sqrt(s) in all four float lanes (see above). No flags:
// ±0 and denormals give ±inf, negative values the default NaN, +inf +0,
// NaNs themselves quieted.
uint32_t RsqrtLane(uint32_t x);
inline void Rsqrt(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 4; i++) SetLane<uint32_t>(r, i, RsqrtLane(Lane<uint32_t>(s, i)));
  d = r;
}
// RCPPS: d = 1/s in all four float lanes, by the same rule as RSQRTPS (no
// architectural result): the correctly rounded quotient. No flags.
uint32_t RcpLane(uint32_t x);
inline void Rcp(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 4; i++) SetLane<uint32_t>(r, i, RcpLane(Lane<uint32_t>(s, i)));
  d = r;
}

// CVTSI2SD: the low lane of d = (double)v (always exact).
uint64_t Cvtsi2sdLane(int32_t v);
inline void Cvtsi2sd(Xmm& d, int32_t v) { SetLane<uint64_t>(d, 0, Cvtsi2sdLane(v)); }

// CVT(T)SS2SI / CVT(T)SD2SI of the low lane of s.
[[gnu::cold]] uint32_t CvtToI32Model(Ctx& c, bool dbl, Xmm s, bool truncate, uint64_t va);
[[gnu::always_inline]] inline uint32_t CvtToI32(Ctx& c, bool usual, bool dbl, const Xmm& s,
                                                bool truncate, uint64_t va) {
#ifdef RECOMP_SSE_HOST
  // Truncated, or rounded to nearest even (MXCSR as usual); NaN and out
  // of range give 0x80000000, as x86 does.
  if (usual) [[likely]] {
    double x = dbl ? Lane<double>(s, 0) : double(Lane<float>(s, 0));
    if (truncate) {
      if (x > -2147483649.0 && x < 2147483648.0) return uint32_t(int32_t(x));
    } else {
      x = std::nearbyint(x);
      if (x >= -2147483648.0 && x < 2147483648.0) return uint32_t(int32_t(x));
    }
    return 0x80000000u;
  }
#endif
  return CvtToI32Model(c, dbl, s, truncate, va);
}

// CVT(T)SS2SI / CVT(T)SD2SI to a 64-bit register.
uint64_t CvtToI64(Ctx& c, bool dbl, Xmm s, bool truncate, uint64_t va);

// MINSS/SD/PS/PD (max = false) and MAXSS/SD/PS/PD, lane-wise: d op s ? d : s.
// A NaN in either operand (invalid, quiet ones too) or equal values,
// zeros of either sign included, give s. Scalar forms keep d's upper lanes.
[[gnu::cold]] Xmm MinMaxModel(Ctx& c, bool max, bool dbl, int lanes, Xmm d, Xmm s,
                              uint64_t va);
[[gnu::always_inline]] inline void MinMax(Ctx& c, bool usual, bool max, bool dbl, int lanes,
                                          Xmm& d, const Xmm& s, uint64_t va) {
#ifdef RECOMP_SSE_HOST
  // The same rule on the host's compare picks the same lane bits (a NaN
  // or an equal pair fails it and gives s); only IE/DE differ (see
  // HostArith).
  if (usual) [[likely]] {
    if (dbl)
      fast::HostMinMax<true>(max, lanes, d, s);
    else
      fast::HostMinMax<false>(max, lanes, d, s);
    return;
  }
#endif
  d = MinMaxModel(c, max, dbl, lanes, d, s, va);
}

// CVTSS2SD / CVTPS2PD (to_dbl, 1 or 2 lanes) and CVTSD2SS / CVTPD2PS.
// Scalar forms keep d's other lanes; CVTPD2PS zeroes its upper half.
[[gnu::cold]] Xmm CvtFloatModel(Ctx& c, bool to_dbl, int lanes, Xmm d, Xmm s, uint64_t va);
[[gnu::always_inline]] inline void CvtFloat(Ctx& c, bool usual, bool to_dbl, int lanes, Xmm& d,
                                            const Xmm& s, uint64_t va) {
#ifdef RECOMP_SSE_HOST
  // Widening is exact; narrowing rounds to nearest even on both (MXCSR as
  // usual); NaNs keep their payload, quieted, on both.
  if (usual) [[likely]] {
    Xmm r = d;
    if (to_dbl) {
      const float a = Lane<float>(s, 0), b = Lane<float>(s, 1);
      SetLane<double>(r, 0, double(a));
      if (lanes == 2) SetLane<double>(r, 1, double(b));
    } else {
      const double a = Lane<double>(s, 0), b = Lane<double>(s, 1);
      SetLane<float>(r, 0, float(a));
      if (lanes == 2) {
        SetLane<float>(r, 1, float(b));
        SetLane<uint64_t>(r, 1, 0);
      }
    }
    d = r;
    return;
  }
#endif
  d = CvtFloatModel(c, to_dbl, lanes, d, s, va);
}

// CVTSI2SS / CVTSI2SD of a 32-bit or 64-bit (src64) integer into the low
// lane, rounded by MXCSR (only int64 -> double and int -> float can be
// inexact).
[[gnu::cold]] Xmm CvtIntToFModel(Ctx& c, bool dbl, bool src64, uint64_t v, Xmm d, uint64_t va);
[[gnu::always_inline]] inline void CvtIntToF(Ctx& c, bool usual, bool dbl, bool src64, uint64_t v,
                                             Xmm& d, uint64_t va) {
#ifdef RECOMP_SSE_HOST
  if (usual) [[likely]] {  // round to nearest even on both
    if (dbl)
      SetLane<double>(d, 0, src64 ? double(int64_t(v)) : double(int32_t(v)));
    else
      SetLane<float>(d, 0, src64 ? float(int64_t(v)) : float(int32_t(v)));
    return;
  }
#endif
  d = CvtIntToFModel(c, dbl, src64, v, d, va);
}

// CVTTPS2DQ / CVTPS2DQ (truncate false) and CVTTPD2DQ / CVTPD2DQ (dbl,
// two lanes into the low half, the upper one zeroed): float lanes to
// int32, 0x80000000 for NaN and out of range (invalid).
[[gnu::cold]] Xmm CvtToDqModel(Ctx& c, bool dbl, bool truncate, Xmm s, uint64_t va);
[[gnu::always_inline]] inline void CvtToDq(Ctx& c, bool usual, bool dbl, bool truncate, Xmm& d,
                                           const Xmm& s, uint64_t va) {
#ifdef RECOMP_SSE_HOST
  // Exact, MXCSR included: the host converts in-range lanes (truncated,
  // or rounded to nearest even: MXCSR as usual); the others are set to
  // the indefinite here, since the host saturates (and gives 0 for NaN),
  // and raise IE. PE is set already.
  if (usual) [[likely]] {
    const bool bad = dbl ? fast::HostCvtToDq<true>(truncate, d, s)
                         : fast::HostCvtToDq<false>(truncate, d, s);
    if (bad) c.mxcsr |= 1u;  // IE
    return;
  }
#endif
  d = CvtToDqModel(c, dbl, truncate, s, va);
}

// RCPSS: the low lane of d = 1/(the low lane of s), as RcpLane.
inline void RcpSs(Xmm& d, const Xmm& s) {
  SetLane<uint32_t>(d, 0, RcpLane(Lane<uint32_t>(s, 0)));
}
// RSQRTSS: the low lane of d = 1/sqrt(the low lane of s), as RsqrtLane.
inline void RsqrtSs(Xmm& d, const Xmm& s) {
  SetLane<uint32_t>(d, 0, RsqrtLane(Lane<uint32_t>(s, 0)));
}

// CVTDQ2PS: four int32 lanes to float, rounded by MXCSR.
[[gnu::cold]] Xmm Cvtdq2psModel(Ctx& c, Xmm s, uint64_t va);
[[gnu::always_inline]] inline void Cvtdq2ps(Ctx& c, bool usual, Xmm& d, const Xmm& s,
                                            uint64_t va) {
#ifdef RECOMP_SSE_HOST
  // Exact, MXCSR included: rounded to nearest even on both, and PE (the
  // only flag it can raise) is set already.
  if (usual) [[likely]] {
    Xmm r;
    for (int i = 0; i < 4; i++) SetLane<float>(r, i, float(Lane<int32_t>(s, i)));
    d = r;
    return;
  }
#endif
  d = Cvtdq2psModel(c, s, va);
}

// CVTDQ2PD: the two low int32 lanes of s as doubles (always exact).
inline Xmm Cvtdq2pd(const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 2; i++) {
    double v = double(Lane<int32_t>(s, i));
    SetLane<double>(r, i, v);
  }
  return r;
}

// LDMXCSR: reserved bits set would #GP: fatal.
void Ldmxcsr(Ctx& c, uint32_t v, uint64_t va);
// Called with each value LDMXCSR loads and its guest address when set
// (the runtime's <ID>_PERF counters).
extern void (*g_ldmxcsr_note)(uint32_t v, uint64_t va);

// ---- bitwise and integer ----------------------------------------------
inline void And(Xmm& d, const Xmm& s) {
  d.b &= s.b;
}
inline void AndNot(Xmm& d, const Xmm& s) {
  d.b = ~d.b & s.b;
}
inline void Or(Xmm& d, const Xmm& s) {
  d.b |= s.b;
}
inline void Xor(Xmm& d, const Xmm& s) {
  d.b ^= s.b;
}

// PCMPEQB/W/D: lanes of type T equal -> all ones.
template <typename T>
inline void CmpEq(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, Lane<T>(d, i) == Lane<T>(s, i) ? T(~T(0)) : T(0));
}
// PADDB/W/D/Q (wrapping).
template <typename T>
inline void Add(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(Lane<T>(d, i) + Lane<T>(s, i)));
}
// PSUBB/W/D/Q (wrapping).
template <typename T>
inline void Sub(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(Lane<T>(d, i) - Lane<T>(s, i)));
}
// PCMPGTB/W/D (signed lanes of type T).
template <typename T>
inline void CmpGt(Xmm& d, const Xmm& s) {
  using U = std::make_unsigned_t<T>;
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<U>(d, i, Lane<T>(d, i) > Lane<T>(s, i) ? U(~U(0)) : U(0));
}
// PMAXUB/UW/UD, PMAXSB/SW/SD, PMINxx: lane-wise, by T's order.
template <typename T>
inline void Max(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T a = Lane<T>(d, i), b = Lane<T>(s, i);
    SetLane<T>(d, i, a > b ? a : b);
  }
}
template <typename T>
inline void Min(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T a = Lane<T>(d, i), b = Lane<T>(s, i);
    SetLane<T>(d, i, a < b ? a : b);
  }
}
// PABSB/W/D (SSSE3): the minimum value stays itself (as unsigned, its
// absolute value).
template <typename T>
inline void Abs(Xmm& d, const Xmm& s) {
  using U = std::make_unsigned_t<T>;
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T v = Lane<T>(s, i);
    SetLane<U>(d, i, v < 0 ? U(-U(v)) : U(v));
  }
}
// PSLLW/D/Q, PSRLW/D/Q, PSRAW/D: every lane of type T by `count` (the
// immediate, or the low quadword of the register/memory operand). Counts
// past the lane width clear the lane (logical) or fill it with its sign.
enum ShiftKind : uint8_t { kShl, kShr, kSar };
template <typename T>
inline void Shift(Xmm& d, uint64_t count, ShiftKind k) {
  using S = std::make_signed_t<T>;
  constexpr uint64_t kBits = sizeof(T) * 8;
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T v = Lane<T>(d, i);
    if (k == kSar)
      v = T(S(v) >> (count >= kBits ? kBits - 1 : count));
    else if (count >= kBits)
      v = 0;
    else
      v = k == kShl ? T(v << count) : T(v >> count);
    SetLane<T>(d, i, v);
  }
}
// PUNPCKLBW/WD/DQ/QDQ (high: PUNPCKH..): the low (high) halves of d and s
// interleaved, d's lane first.
template <typename T>
inline void Unpck(Xmm& d, const Xmm& s, bool high) {
  constexpr int n = int(16 / sizeof(T));
  Xmm r;
  int b = high ? n / 2 : 0;
  for (int i = 0; i < n / 2; i++) {
    SetLane<T>(r, 2 * i, Lane<T>(d, b + i));
    SetLane<T>(r, 2 * i + 1, Lane<T>(s, b + i));
  }
  d = r;
}
// PSHUFLW / PSHUFHW: the four words of the low (high) quadword of s
// picked by imm; the other quadword copied.
inline void ShufW(Xmm& d, const Xmm& s, uint8_t imm, bool high) {
  Xmm r = s;
  int b = high ? 4 : 0;
  for (int i = 0; i < 4; i++)
    SetLane<uint16_t>(r, b + i, Lane<uint16_t>(s, b + ((imm >> (2 * i)) & 3)));
  d = r;
}
// PMULUDQ / PMULDQ (SSE4.1): the even 32-bit lanes multiplied into
// 64-bit products.
inline void MulUDq(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++)
    SetLane<uint64_t>(d, i, uint64_t(Lane<uint32_t>(d, 2 * i)) * Lane<uint32_t>(s, 2 * i));
}
inline void MulDq(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++)
    SetLane<int64_t>(d, i, int64_t(Lane<int32_t>(d, 2 * i)) * Lane<int32_t>(s, 2 * i));
}
// PMULLW / PMULLD (SSE4.1): the low half of each lane's product.
template <typename T>
inline void MulLo(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(uint64_t(Lane<T>(d, i)) * Lane<T>(s, i)));
}
// PACKSSDW / PACKSSWB / PACKUSWB / PACKUSDW: d's lanes then s's,
// narrowed with saturation (signed From to To).
template <typename From, typename To>
inline void Pack(Xmm& d, const Xmm& s) {
  constexpr int n = int(16 / sizeof(From));
  auto sat = [](From v) {
    constexpr From lo = From(std::numeric_limits<To>::min()),
                   hi = From(std::numeric_limits<To>::max());
    return To(v < lo ? lo : v > hi ? hi : v);
  };
  Xmm r;
  for (int i = 0; i < n; i++) SetLane<To>(r, i, sat(Lane<From>(d, i)));
  for (int i = 0; i < n; i++) SetLane<To>(r, n + i, sat(Lane<From>(s, i)));
  d = r;
}
// PMOVSX/PMOVZX (SSE4.1): the low lanes of s widened from From to To.
template <typename From, typename To>
inline void Extend(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < int(16 / sizeof(To)); i++) SetLane<To>(r, i, To(Lane<From>(s, i)));
  d = r;
}
// PALIGNR (SSSE3): bytes n.. of the 32-byte d:s (d high), zero past it.
inline void Alignr(Xmm& d, const Xmm& s, uint8_t n) {
  uint8_t t[32];
  std::memcpy(t, s.data(), 16);
  std::memcpy(t + 16, d.data(), 16);
  Xmm r;
  for (int i = 0; i < 16; i++) r.b[i] = n + i < 32 ? t[n + i] : 0;
  d = r;
}
// MOVMSKPS / MOVMSKPD: the sign bits of the float (double) lanes.
inline uint32_t MovMskPs(const Xmm& s) {
  uint32_t m = 0;
  for (int i = 0; i < 4; i++) m |= (Lane<uint32_t>(s, i) >> 31) << i;
  return m;
}
inline uint32_t MovMskPd(const Xmm& s) {
  return uint32_t(Lane<uint64_t>(s, 0) >> 63) | uint32_t(Lane<uint64_t>(s, 1) >> 63) << 1;
}
// BLENDVPS/PD (SSE4.1): lanes of s where the mask lane's sign bit is set.
template <typename T>
inline void BlendV(Xmm& d, const Xmm& s, const Xmm& mask) {
  constexpr int kTop = int(sizeof(T) * 8 - 1);
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    if (Lane<T>(mask, i) >> kTop) SetLane<T>(d, i, Lane<T>(s, i));
}

// PMAXSW.
inline void MaxS16(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 8; i++) {
    int16_t a = Lane<int16_t>(d, i), b = Lane<int16_t>(s, i);
    SetLane<int16_t>(d, i, a > b ? a : b);
  }
}
// PMOVMSKB: the sign bit of every byte.
inline uint32_t MovMskB(const Xmm& s) {
  uint32_t m = 0;
  for (int i = 0; i < 16; i++) m |= uint32_t(s.b[i] >> 7) << i;
  return m;
}
// PSLLQ/PSRLQ: each quadword shifted by `count` (>= 64 clears it). The
// register/memory form passes the low quadword of its operand.
inline void ShlQ(Xmm& d, uint64_t count) {
  for (int i = 0; i < 2; i++)
    SetLane<uint64_t>(d, i, count > 63 ? 0 : Lane<uint64_t>(d, i) << count);
}
inline void ShrQ(Xmm& d, uint64_t count) {
  for (int i = 0; i < 2; i++)
    SetLane<uint64_t>(d, i, count > 63 ? 0 : Lane<uint64_t>(d, i) >> count);
}
// PSRLDQ / PSLLDQ: the whole register shifted by `n` bytes (more than 15
// clears it).
inline void ShrDq(Xmm& d, uint8_t n) {
  Xmm r{};
  for (int i = 0; i + n < 16; i++) r.b[i] = d.b[i + n];
  d = r;
}
inline void ShlDq(Xmm& d, uint8_t n) {
  Xmm r{};
  for (int i = n; i < 16; i++) r.b[i] = d.b[i - n];
  d = r;
}
// PSHUFD.
inline void ShufD(Xmm& d, const Xmm& s, uint8_t imm) {
  Xmm r;
  for (int i = 0; i < 4; i++)
    SetLane<uint32_t>(r, i, Lane<uint32_t>(s, (imm >> (2 * i)) & 3));
  d = r;
}
// SHUFPS: low two lanes from d, high two from s.
inline void ShufPs(Xmm& d, const Xmm& s, uint8_t imm) {
  Xmm r;
  SetLane<uint32_t>(r, 0, Lane<uint32_t>(d, imm & 3));
  SetLane<uint32_t>(r, 1, Lane<uint32_t>(d, (imm >> 2) & 3));
  SetLane<uint32_t>(r, 2, Lane<uint32_t>(s, (imm >> 4) & 3));
  SetLane<uint32_t>(r, 3, Lane<uint32_t>(s, (imm >> 6) & 3));
  d = r;
}
// UNPCKLPD: high quadword of d = low quadword of s.
inline void UnpckLPd(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 1, Lane<uint64_t>(s, 0));
}
// UNPCKHPD: low quadword of d = its high one, high quadword = s's high one.
inline void UnpckHPd(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 0, Lane<uint64_t>(d, 1));
  SetLane<uint64_t>(d, 1, Lane<uint64_t>(s, 1));
}
// UNPCKLPS / UNPCKHPS: the low / high two lanes of d and s interleaved.
inline void UnpckPs(Xmm& d, const Xmm& s, bool high) {
  Xmm r;
  int b = high ? 2 : 0;
  SetLane<uint32_t>(r, 0, Lane<uint32_t>(d, b));
  SetLane<uint32_t>(r, 1, Lane<uint32_t>(s, b));
  SetLane<uint32_t>(r, 2, Lane<uint32_t>(d, b + 1));
  SetLane<uint32_t>(r, 3, Lane<uint32_t>(s, b + 1));
  d = r;
}
// MOVLHPS: high quadword of d = low quadword of s. MOVHLPS: low quadword
// of d = high quadword of s.
inline void MovLHPs(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 1, Lane<uint64_t>(s, 0));
}
inline void MovHLPs(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 0, Lane<uint64_t>(s, 1));
}
// SHUFPD: low quadword from d, high from s, picked by imm bits 0 and 1.
inline void ShufPd(Xmm& d, const Xmm& s, uint8_t imm) {
  Xmm r;
  SetLane<uint64_t>(r, 0, Lane<uint64_t>(d, imm & 1));
  SetLane<uint64_t>(r, 1, Lane<uint64_t>(s, (imm >> 1) & 1));
  d = r;
}

}  // namespace recomp::sse
