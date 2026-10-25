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
// RSQRTPS and RCPPS have no architectural result, only an error bound
// (|rel. error| <= 1.5 * 2^-12), and CPUs differ: here they are the
// correctly rounded 1/sqrt(x) and 1/x, within the bound and the same on
// every host (and what a native replacement computes). tests/
// sse_rsqrt_test checks them; the bit-for-bit oracle against the host
// skips them.
//
// Only the forms the supported games use are implemented; the lifter
// reports the rest as unimplemented.

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
  std::memcpy(&v, x.b + i * sizeof(T), sizeof(T));
  return v;
}
template <typename T>
inline void SetLane(Xmm& x, int i, T v) {
  std::memcpy(x.b + i * sizeof(T), &v, sizeof(T));
}

// `bytes` of guest memory at a, zero-extended to a register.
inline Xmm Ld(const uint8_t* B, uint64_t a, int bytes) {
  Xmm x{};
  std::memcpy(x.b, B + a, bytes);
  return x;
}
inline void St(uint8_t* B, uint64_t a, const Xmm& x, int bytes) {
  std::memcpy(B + a, x.b, bytes);
}
// Legacy-SSE m128 operands other than movups/movupd/movdqu must be 16-byte
// aligned (#GP otherwise): fatal, naming the instruction.
void CheckAligned(Ctx& c, uint64_t a, uint64_t va);
// The same for VEX moves that require `align` (16 or 32) bytes.
void CheckAligned(Ctx& c, uint64_t a, uint64_t va, unsigned align);

// ---- floating point ---------------------------------------------------
enum Op : uint8_t { kAdd, kSub, kMul, kDiv };

// d = d op s, lane-wise: `lanes` lanes of double (dbl) or float. Scalar
// forms pass lanes = 1 and keep the upper lanes of d.
void Arith(Ctx& c, Op op, bool dbl, int lanes, Xmm& d, const Xmm& s,
           uint64_t va);

// CMPPS/PD/SS/SD: each lane of d becomes all ones if `d pred s` holds,
// else zero. pred is AVX's 5-bit predicate (0 EQ_OQ, 1 LT_OS, 2 LE_OS,
// 3 UNORD_Q, 4 NEQ_UQ, 5 NLT_US, 6 NLE_US, 7 ORD_Q, 8 EQ_UQ, ...
// 15 TRUE_UQ; 16-31 the same with signalling flipped); legacy SSE passes
// imm8 & 7.
void Cmp(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint8_t pred,
         uint64_t va);

// (U)COMISS/SD of the low lanes: 0 greater, 1 less, 2 equal, 3 unordered
// (the caller sets ZF/PF/CF). `ordered` = COMIS (invalid on QNaN too).
int Comi(Ctx& c, bool dbl, bool ordered, const Xmm& a, const Xmm& b,
         uint64_t va);

// SQRTSD/SQRTSS/SQRTPD/SQRTPS: d = sqrt(s), lane-wise; scalar forms keep
// the upper lanes of d.
void Sqrt(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va);

// RSQRTPS: d = 1/sqrt(s) in all four float lanes (see above). No flags:
// ±0 and denormals give ±inf, negative values the default NaN, +inf +0,
// NaNs themselves quieted.
void Rsqrt(Xmm& d, const Xmm& s);
uint32_t RsqrtLane(uint32_t x);

// CVTSI2SD: the low lane of d = (double)v (always exact).
void Cvtsi2sd(Xmm& d, int32_t v);

// CVT(T)SS2SI / CVT(T)SD2SI of the low lane of s.
uint32_t CvtToI32(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint64_t va);

// CVT(T)SS2SI / CVT(T)SD2SI to a 64-bit register.
uint64_t CvtToI64(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint64_t va);

// MINSS/SD/PS/PD (max = false) and MAXSS/SD/PS/PD, lane-wise: d op s ? d : s.
// A NaN in either operand (invalid, quiet ones too) or equal values,
// zeros of either sign included, give s. Scalar forms keep d's upper lanes.
void MinMax(Ctx& c, bool max, bool dbl, int lanes, Xmm& d, const Xmm& s,
            uint64_t va);

// CVTSS2SD / CVTPS2PD (to_dbl, 1 or 2 lanes) and CVTSD2SS / CVTPD2PS.
// Scalar forms keep d's other lanes; CVTPD2PS zeroes its upper half.
void CvtFloat(Ctx& c, bool to_dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va);

// CVTSI2SS / CVTSI2SD of a 32-bit or 64-bit (src64) integer into the low
// lane, rounded by MXCSR (only int64 -> double and int -> float can be
// inexact).
void CvtIntToF(Ctx& c, bool dbl, bool src64, uint64_t v, Xmm& d, uint64_t va);

// CVTTPS2DQ / CVTPS2DQ (truncate false) and CVTTPD2DQ / CVTPD2DQ (dbl,
// two lanes into the low half, the upper one zeroed): float lanes to
// int32, 0x80000000 for NaN and out of range (invalid).
void CvtToDq(Ctx& c, bool dbl, bool truncate, Xmm& d, const Xmm& s, uint64_t va);

// HADDPS/PD, HSUBPS/PD (SSE3): pairs of adjacent lanes of d, then of s,
// added (subtracted: the higher lane from the lower). ADDSUBPS/PD: even
// lanes d - s, odd ones d + s.
void HorizOp(Ctx& c, bool dbl, bool sub, Xmm& d, const Xmm& s, uint64_t va);
void AddSub(Ctx& c, bool dbl, Xmm& d, const Xmm& s, uint64_t va);

// ROUNDPS/PD/SS/SD (SSE4.1): lanes rounded to integers, imm bits 1:0 the
// mode (bit 2: MXCSR's instead), bit 3 suppressing PE. Scalar forms
// (lanes = 1) keep d's upper lanes.
void Round(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint8_t imm, uint64_t va);

// DPPS/DPPD (SSE4.1): the products of the lanes imm selects (bits 7:4,
// 5:4 for DPPD; others +0) summed pairwise, written to the lanes imm
// bits 3:0 (1:0) select, the others zeroed.
void Dp(Ctx& c, bool dbl, Xmm& d, const Xmm& s, uint8_t imm, uint64_t va);

// RCPPS: d = 1/s in all four float lanes, correctly rounded (see above).
// No flags: ±0 and denormals give ±inf, results below the normal range
// ±0, ±inf ±0, NaNs themselves quieted.
void Rcp(Xmm& d, const Xmm& s);
uint32_t RcpLane(uint32_t x);
inline void RcpSs(Xmm& d, const Xmm& s) {
  SetLane<uint32_t>(d, 0, RcpLane(Lane<uint32_t>(s, 0)));
}

// RSQRTSS: the low lane of d = 1/sqrt(the low lane of s), as RsqrtLane.
inline void RsqrtSs(Xmm& d, const Xmm& s) {
  SetLane<uint32_t>(d, 0, RsqrtLane(Lane<uint32_t>(s, 0)));
}

// CVTDQ2PS: four int32 lanes to float, rounded by MXCSR.
void Cvtdq2ps(Ctx& c, Xmm& d, const Xmm& s, uint64_t va);

// CVTDQ2PD: the two low int32 lanes of s as doubles (always exact).
inline Xmm Cvtdq2pd(const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 2; i++) {
    double v = double(Lane<int32_t>(s, i));
    SetLane<double>(r, i, v);
  }
  return r;
}

// FXSAVE / FXRSTOR (both forms) of the 512-byte area at p (16-byte
// aligned): the x87 part (x87::FxSave), MXCSR and MXCSR_MASK (0xFFFF:
// the bits LDMXCSR takes), XMM0-15 from byte 160; bytes 416-511 are left
// alone. FXRSTOR with reserved MXCSR bits set would #GP: fatal.
void Fxsave(Ctx& c, uint8_t* p);
void Fxrstor(Ctx& c, const uint8_t* p, uint64_t va);

// LDMXCSR: reserved bits set would #GP: fatal.
void Ldmxcsr(Ctx& c, uint32_t v, uint64_t va);

// ---- bitwise and integer ----------------------------------------------
inline void And(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 16; i++) d.b[i] &= s.b[i];
}
inline void AndNot(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 16; i++) d.b[i] = uint8_t(~d.b[i] & s.b[i]);
}
inline void Or(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 16; i++) d.b[i] |= s.b[i];
}
inline void Xor(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 16; i++) d.b[i] ^= s.b[i];
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
  std::memcpy(t, s.b, 16);
  std::memcpy(t + 16, d.b, 16);
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

// PADDSB/SW, PADDUSB/USW, PSUBSB/SW, PSUBUSB/USW: lanes of T, saturated.
template <typename T>
inline void AddSat(Xmm& d, const Xmm& s, bool sub) {
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    int32_t v = sub ? int32_t(Lane<T>(d, i)) - int32_t(Lane<T>(s, i))
                    : int32_t(Lane<T>(d, i)) + int32_t(Lane<T>(s, i));
    constexpr int32_t lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    SetLane<T>(d, i, T(v < lo ? lo : v > hi ? hi : v));
  }
}
// PMULHW / PMULHUW: the high half of each word product.
template <typename T>
inline void MulHi16(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 8; i++)
    SetLane<uint16_t>(d, i, uint16_t((int32_t(Lane<T>(d, i)) * int32_t(Lane<T>(s, i))) >> 16));
}
// PMULHRSW (SSSE3): each word product scaled by 2^-15 with rounding.
inline void MulHrs(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 8; i++) {
    int32_t p = int32_t(Lane<int16_t>(d, i)) * int32_t(Lane<int16_t>(s, i));
    SetLane<uint16_t>(d, i, uint16_t(((p >> 14) + 1) >> 1));
  }
}
// PMADDWD: adjacent signed word products summed into dwords (wrapping).
inline void MAddWd(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 4; i++) {
    uint32_t a = uint32_t(int32_t(Lane<int16_t>(d, 2 * i)) * Lane<int16_t>(s, 2 * i));
    uint32_t b = uint32_t(int32_t(Lane<int16_t>(d, 2 * i + 1)) * Lane<int16_t>(s, 2 * i + 1));
    SetLane<uint32_t>(r, i, a + b);
  }
  d = r;
}
// PMADDUBSW (SSSE3): unsigned bytes of d times signed bytes of s, adjacent
// products summed into words with signed saturation.
inline void MAddUbsw(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 8; i++) {
    int32_t v = int32_t(d.b[2 * i]) * int8_t(s.b[2 * i]) +
                int32_t(d.b[2 * i + 1]) * int8_t(s.b[2 * i + 1]);
    SetLane<int16_t>(r, i, int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v));
  }
  d = r;
}
// PHADDW/D, PHSUBW/D (SSSE3): adjacent lanes of d, then of s, added
// (subtracted: the higher from the lower), wrapping.
template <typename T>
inline void HorizInt(Xmm& d, const Xmm& s, bool sub) {
  constexpr int n = int(16 / sizeof(T));
  Xmm r;
  for (int i = 0; i < n; i++) {
    const Xmm& x = i < n / 2 ? d : s;
    int k = 2 * (i % (n / 2));
    T a = Lane<T>(x, k), b = Lane<T>(x, k + 1);
    SetLane<T>(r, i, T(sub ? a - b : a + b));
  }
  d = r;
}
// PHADDSW / PHSUBSW (SSSE3): HorizInt on words with signed saturation.
inline void HorizSatW(Xmm& d, const Xmm& s, bool sub) {
  Xmm r;
  for (int i = 0; i < 8; i++) {
    const Xmm& x = i < 4 ? d : s;
    int k = 2 * (i % 4);
    int32_t a = Lane<int16_t>(x, k), b = Lane<int16_t>(x, k + 1);
    int32_t v = sub ? a - b : a + b;
    SetLane<int16_t>(r, i, int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v));
  }
  d = r;
}
// PSIGNB/W/D (SSSE3): each lane of d negated (wrapping), zeroed or kept
// as the same lane of s is negative, zero or positive.
template <typename T>
inline void Sign(Xmm& d, const Xmm& s) {
  using U = std::make_unsigned_t<T>;
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T v = Lane<T>(s, i);
    U x = Lane<U>(d, i);
    SetLane<U>(d, i, v < 0 ? U(0 - x) : v == 0 ? U(0) : x);
  }
}
// MPSADBW (SSE4.1): word i = the sum of |d byte (o1 + i + k) - s byte
// (o2 + k)| over k < 4, with o1 = imm[2] * 4 and o2 = imm[1:0] * 4.
inline void MpSadBw(Xmm& d, const Xmm& s, uint8_t imm) {
  const int o1 = (imm >> 2 & 1) * 4, o2 = (imm & 3) * 4;
  Xmm r;
  for (int i = 0; i < 8; i++) {
    uint32_t sum = 0;
    for (int k = 0; k < 4; k++) {
      int a = d.b[o1 + i + k], b = s.b[o2 + k];
      sum += uint32_t(a > b ? a - b : b - a);
    }
    SetLane<uint16_t>(r, i, uint16_t(sum));
  }
  d = r;
}
// CRC32 (SSE4.2): crc updated with the low `bytes` of v (least significant
// first), CRC-32C (Castagnoli), bit-reflected, no inversion.
inline uint32_t Crc32c(uint32_t crc, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; i++) {
    crc ^= uint8_t(v >> (8 * i));
    for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (crc & 1 ? 0x82F63B78u : 0u);
  }
  return crc;
}

// PAVGB/W: unsigned (a + b + 1) / 2.
template <typename T>
inline void Avg(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T((uint32_t(Lane<T>(d, i)) + Lane<T>(s, i) + 1) >> 1));
}
// PSADBW: per quadword, the sum of the absolute byte differences in its
// low word, the rest zero.
inline void SadBw(Xmm& d, const Xmm& s) {
  Xmm r{};
  for (int q = 0; q < 2; q++) {
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
      int a = d.b[8 * q + i], b = s.b[8 * q + i];
      sum += uint32_t(a > b ? a - b : b - a);
    }
    SetLane<uint64_t>(r, q, sum);
  }
  d = r;
}
// PSHUFB (SSSE3): byte i of d = the byte of d that s's byte i indexes (low
// four bits), or zero if its top bit is set.
inline void ShufB(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 16; i++) r.b[i] = (s.b[i] & 0x80) ? 0 : d.b[s.b[i] & 15];
  d = r;
}
// PBLENDW (SSE4.1): words of s where imm's bit is set.
inline void BlendW(Xmm& d, const Xmm& s, uint8_t imm) {
  for (int i = 0; i < 8; i++)
    if (imm >> i & 1) SetLane<uint16_t>(d, i, Lane<uint16_t>(s, i));
}
// BLENDPS/PD (SSE4.1): lanes of s where imm's bit is set.
template <typename T>
inline void BlendImm(Xmm& d, const Xmm& s, uint8_t imm) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    if (imm >> i & 1) SetLane<T>(d, i, Lane<T>(s, i));
}
// INSERTPS (SSE4.1): float lane imm[7:6] of s (a register; a memory
// operand passes its m32 as lane 0 with imm[7:6] ignored) into lane
// imm[5:4] of d, then the lanes in imm[3:0] zeroed.
inline void InsertPs(Xmm& d, const Xmm& s, uint8_t imm, bool mem) {
  SetLane<uint32_t>(d, (imm >> 4) & 3, Lane<uint32_t>(s, mem ? 0 : imm >> 6));
  for (int i = 0; i < 4; i++)
    if (imm >> i & 1) SetLane<uint32_t>(d, i, 0);
}
// PTEST (SSE4.1): ZF = (d & s) == 0, CF = (~d & s) == 0; the other flags
// cleared. Returns ZF | CF << 1.
inline uint32_t Ptest(const Xmm& d, const Xmm& s) {
  bool z = true, cf = true;
  for (int i = 0; i < 16; i++) {
    if (d.b[i] & s.b[i]) z = false;
    if (~d.b[i] & s.b[i]) cf = false;
  }
  return uint32_t(z) | uint32_t(cf) << 1;
}
// PHMINPOSUW (SSE4.1): the least unsigned word of s (the first of equal
// ones) and its index, in the low two words; the rest zero.
inline void MinPosUw(Xmm& d, const Xmm& s) {
  int k = 0;
  for (int i = 1; i < 8; i++)
    if (Lane<uint16_t>(s, i) < Lane<uint16_t>(s, k)) k = i;
  Xmm r{};
  SetLane<uint16_t>(r, 0, Lane<uint16_t>(s, k));
  SetLane<uint16_t>(r, 1, uint16_t(k));
  d = r;
}
// MOVSLDUP / MOVSHDUP (SSE3): the even (odd) float lanes of s duplicated.
// MOVDDUP: the low quadword of s twice.
inline void MovSDup(Xmm& d, const Xmm& s, bool high) {
  Xmm r;
  for (int i = 0; i < 4; i++) SetLane<uint32_t>(r, i, Lane<uint32_t>(s, (i & ~1) + high));
  d = r;
}
inline void MovDDup(Xmm& d, const Xmm& s) {
  uint64_t q = Lane<uint64_t>(s, 0);
  SetLane<uint64_t>(d, 0, q);
  SetLane<uint64_t>(d, 1, q);
}

// ---- AES-NI, PCLMULQDQ, SSE4.2 string compares (sse_ext.cpp) -----------
// AESENC/AESENCLAST (last: no MixColumns), AESDEC/AESDECLAST: one round of
// d with round key s. AESIMC: InvMixColumns of s. AESKEYGENASSIST: the
// key expansion's SubWord/RotWord of s's dwords 1 and 3 with rcon.
void AesEnc(Xmm& d, const Xmm& s, bool last);
void AesDec(Xmm& d, const Xmm& s, bool last);
void AesImc(Xmm& d, const Xmm& s);
void AesKeygenAssist(Xmm& d, const Xmm& s, uint8_t rcon);
// PCLMULQDQ: the carry-less product of quadword imm[0] of d and
// quadword imm[4] of s.
void Clmul(Xmm& d, const Xmm& s, uint8_t imm);
// PCMPESTRI (explicit lengths la, lb: rax/eax, rdx/edx, signed) and
// PCMPISTRI (implicit: up to the first zero element): the index into ECX;
// flags CF, ZF, SF, OF as the SDM defines them (AF, PF cleared), returned
// as CF | ZF << 1 | SF << 2 | OF << 3.
uint32_t PcmpStrI(const Xmm& a, const Xmm& b, uint8_t imm, bool expl, int64_t la, int64_t lb,
                  uint32_t* index);
// PCMPESTRM / PCMPISTRM: the same, the result into *mask (XMM0): bits
// (imm[6] clear) or whole elements (set).
uint32_t PcmpStrM(const Xmm& a, const Xmm& b, uint8_t imm, bool expl, int64_t la, int64_t lb,
                  Xmm* mask);

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
