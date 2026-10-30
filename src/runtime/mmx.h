#pragma once
// MMX for lifted code: 64-bit integer SIMD on the x87 register file.
//
// As on the CPU (Intel SDM vol. 1, 9.5), the eight MMX registers are the
// significands of the x87 physical registers: MM<i> is R<i>. Writing an
// MMX register sets that register's sign and exponent to all ones; every
// MMX instruction but EMMS sets TOP to 0 and tags all registers valid;
// EMMS tags them all empty. Code that mixes the two without EMMS therefore
// sees exactly what the hardware would.
//
// Lanes are read and written little-endian, which both hosts are.

#include <cstdint>
#include <cstring>

#include "runtime/ctx.h"

namespace recomp::mmx {

inline uint64_t Get(const Ctx& c, int i) { return c.fpu.r[i].sig; }
inline void Set(Ctx& c, int i, uint64_t v) {
  c.fpu.r[i].sig = v;
  c.fpu.r[i].se = 0xFFFF;
}
// The x87-to-MMX transition at the start of every MMX instruction but EMMS.
inline void Enter(Ctx& c) {
  c.fpu.top = 0;
  c.fpu.empty = 0;
}
inline void Emms(Ctx& c) { c.fpu.empty = 0xFF; }

template <class T>
inline T Lane(uint64_t v, int i) {
  T x;
  std::memcpy(&x, reinterpret_cast<const char*>(&v) + i * sizeof(T), sizeof(T));
  return x;
}
template <class T>
inline void SetLane(uint64_t& v, int i, T x) {
  std::memcpy(reinterpret_cast<char*>(&v) + i * sizeof(T), &x, sizeof(T));
}

// r.lane[i] = f(a.lane[i], b.lane[i]) for every lane of type T.
template <class T, class F>
inline uint64_t Map(uint64_t a, uint64_t b, F f) {
  uint64_t r = 0;
  for (int i = 0; i < int(8 / sizeof(T)); i++) SetLane<T>(r, i, T(f(Lane<T>(a, i), Lane<T>(b, i))));
  return r;
}

// PAND, PANDN, POR, PXOR: the 64-bit result as it is.
inline uint64_t Bits(uint64_t v) { return v; }

template <class T>
inline T Sat(int64_t v) {
  constexpr int64_t lo = T(~T(0)) < T(0) ? -(int64_t(1) << (8 * sizeof(T) - 1)) : 0;
  constexpr int64_t hi = T(~T(0)) < T(0) ? (int64_t(1) << (8 * sizeof(T) - 1)) - 1
                                         : (int64_t(1) << (8 * sizeof(T))) - 1;
  return T(v < lo ? lo : v > hi ? hi : v);
}

// PADD*/PSUB*: wrapping (T unsigned), signed saturating (T signed, sat) or
// unsigned saturating (T unsigned, sat).
template <class T>
inline uint64_t Add(uint64_t a, uint64_t b, bool sat) {
  return Map<T>(a, b, [sat](T x, T y) {
    return sat ? Sat<T>(int64_t(x) + int64_t(y)) : T(x + y);
  });
}
template <class T>
inline uint64_t Sub(uint64_t a, uint64_t b, bool sat) {
  return Map<T>(a, b, [sat](T x, T y) {
    return sat ? Sat<T>(int64_t(x) - int64_t(y)) : T(x - y);
  });
}

inline uint64_t Mullw(uint64_t a, uint64_t b) {
  return Map<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t(uint32_t(x) * y); });
}
inline uint64_t Mulhw(uint64_t a, uint64_t b) {
  return Map<int16_t>(a, b, [](int16_t x, int16_t y) { return int16_t((int32_t(x) * y) >> 16); });
}
inline uint64_t Mulhuw(uint64_t a, uint64_t b) {
  return Map<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t((uint32_t(x) * y) >> 16); });
}
// PMADDWD: each dword is the sum of two signed word products. The only
// sum that leaves int32 (both pairs 0x8000 * 0x8000) wraps to 0x80000000.
inline uint64_t Maddwd(uint64_t a, uint64_t b) {
  uint64_t r = 0;
  for (int i = 0; i < 2; i++) {
    int64_t s = int64_t(Lane<int16_t>(a, 2 * i)) * Lane<int16_t>(b, 2 * i) +
                int64_t(Lane<int16_t>(a, 2 * i + 1)) * Lane<int16_t>(b, 2 * i + 1);
    SetLane<uint32_t>(r, i, uint32_t(s));
  }
  return r;
}

template <class T>
inline uint64_t CmpEq(uint64_t a, uint64_t b) {
  return Map<T>(a, b, [](T x, T y) { return x == y ? T(~T(0)) : T(0); });
}
template <class T>  // signed T
inline uint64_t CmpGt(uint64_t a, uint64_t b) {
  return Map<T>(a, b, [](T x, T y) { return x > y ? T(-1) : T(0); });
}

// Shifts by a 64-bit count (an imm8, or an MMX register or m64). A count
// past the lane width gives 0 (logical) or the sign in every bit
// (arithmetic).
template <class T>
inline uint64_t Shl(uint64_t a, uint64_t n) {
  if (n >= 8 * sizeof(T)) return 0;
  return Map<T>(a, 0, [n](T x, T) { return T(x << n); });
}
template <class T>
inline uint64_t Shr(uint64_t a, uint64_t n) {
  if (n >= 8 * sizeof(T)) return 0;
  return Map<T>(a, 0, [n](T x, T) { return T(x >> n); });
}
template <class T>  // signed T
inline uint64_t Sar(uint64_t a, uint64_t n) {
  if (n >= 8 * sizeof(T)) n = 8 * sizeof(T) - 1;
  return Map<T>(a, 0, [n](T x, T) { return T(x >> n); });
}

// PACKSSWB / PACKSSDW / PACKUSWB: a's lanes narrowed with saturation into
// the low half, b's into the high half. From is the signed source lane, To
// the destination lane (signed or unsigned saturation).
template <class From, class To>
inline uint64_t Pack(uint64_t a, uint64_t b) {
  constexpr int n = 8 / sizeof(From);
  uint64_t r = 0;
  for (int i = 0; i < n; i++) {
    SetLane<To>(r, i, Sat<To>(Lane<From>(a, i)));
    SetLane<To>(r, n + i, Sat<To>(Lane<From>(b, i)));
  }
  return r;
}

// PUNPCKL* / PUNPCKH*: interleave the low (high) halves' lanes of a and b,
// a's first.
template <class T>
inline uint64_t Unpack(uint64_t a, uint64_t b, bool high) {
  constexpr int n = 4 / sizeof(T);
  uint64_t r = 0;
  for (int i = 0; i < n; i++) {
    SetLane<T>(r, 2 * i, Lane<T>(a, (high ? n : 0) + i));
    SetLane<T>(r, 2 * i + 1, Lane<T>(b, (high ? n : 0) + i));
  }
  return r;
}

}  // namespace recomp::mmx
