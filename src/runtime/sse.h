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
// Only the forms the supported games use are implemented; the lifter
// reports the rest as unimplemented.

#include <cstdint>
#include <cstring>

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
inline Xmm Ld(const uint8_t* B, uint32_t a, int bytes) {
  Xmm x{};
  std::memcpy(x.b, B + a, bytes);
  return x;
}
inline void St(uint8_t* B, uint32_t a, const Xmm& x, int bytes) {
  std::memcpy(B + a, x.b, bytes);
}
// Legacy-SSE m128 operands other than movups/movupd/movdqu must be 16-byte
// aligned (#GP otherwise): fatal, naming the instruction.
void CheckAligned(Ctx& c, uint32_t a, uint32_t va);

// ---- floating point ---------------------------------------------------
enum Op : uint8_t { kAdd, kSub, kMul, kDiv };

// d = d op s, lane-wise: `lanes` lanes of double (dbl) or float. Scalar
// forms pass lanes = 1 and keep the upper lanes of d.
void Arith(Ctx& c, Op op, bool dbl, int lanes, Xmm& d, const Xmm& s,
           uint32_t va);

// CMPPS/PD/SS/SD: each lane of d becomes all ones if `d pred s` holds,
// else zero. pred is the imm8 (0 EQ, 1 LT, 2 LE, 3 UNORD, 4 NEQ, 5 NLT,
// 6 NLE, 7 ORD).
void Cmp(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint8_t pred,
         uint32_t va);

// (U)COMISS/SD of the low lanes: 0 greater, 1 less, 2 equal, 3 unordered
// (the caller sets ZF/PF/CF). `ordered` = COMIS (invalid on QNaN too).
int Comi(Ctx& c, bool dbl, bool ordered, const Xmm& a, const Xmm& b,
         uint32_t va);

// CVT(T)SS2SI / CVT(T)SD2SI of the low lane of s.
uint32_t CvtToI32(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint32_t va);

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
void Ldmxcsr(Ctx& c, uint32_t v, uint32_t va);

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
// PSUBB/W/D/Q (wrapping).
template <typename T>
inline void Sub(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(Lane<T>(d, i) - Lane<T>(s, i)));
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

}  // namespace recomp::sse
