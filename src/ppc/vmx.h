#pragma once
// VMX (AltiVec) semantics for lifted code: the element-wise integer
// operations inline here, the floating-point ones in ppc/vmx.cpp. Vectors
// are in the register's big-endian element order (ppc/ctx.h, Vec);
// element 0 is the most significant. Saturating operations set VSCR[SAT]
// (sticky). Included by ppc/lifted.h.

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <type_traits>

namespace recomp::ppc {

// Every VMX operation, shared with the decoder (as FpOp is).
enum VmxOp : uint8_t {
  // Integer arithmetic: modulo, saturating, carry/borrow, average, extremes.
  kVaddubm, kVadduhm, kVadduwm, kVaddubs, kVadduhs, kVadduws, kVaddsbs, kVaddshs, kVaddsws,
  kVaddcuw,
  kVsububm, kVsubuhm, kVsubuwm, kVsububs, kVsubuhs, kVsubuws, kVsubsbs, kVsubshs, kVsubsws,
  kVsubcuw,
  kVavgub, kVavguh, kVavguw, kVavgsb, kVavgsh, kVavgsw,
  kVmaxub, kVmaxuh, kVmaxuw, kVmaxsb, kVmaxsh, kVmaxsw,
  kVminub, kVminuh, kVminuw, kVminsb, kVminsh, kVminsw,
  // Multiplies (even and odd elements, to double width), multiply-adds.
  kVmuleub, kVmuleuh, kVmulesb, kVmulesh, kVmuloub, kVmulouh, kVmulosb, kVmulosh,
  kVmhaddshs, kVmhraddshs, kVmladduhm,
  kVmsumubm, kVmsummbm, kVmsumuhm, kVmsumuhs, kVmsumshm, kVmsumshs,
  kVsum4ubs, kVsum4sbs, kVsum4shs, kVsum2sws, kVsumsws,
  // Logical.
  kVand, kVandc, kVor, kVnor, kVxor, kVsel,
  // Rotates and shifts per element; whole-register shifts by bits / octets.
  kVrlb, kVrlh, kVrlw, kVslb, kVslh, kVslw, kVsrb, kVsrh, kVsrw, kVsrab, kVsrah, kVsraw,
  kVsl, kVsr, kVslo, kVsro,
  // Permutes, merges, splats, packs and unpacks.
  kVperm, kVsldoi,
  kVmrghb, kVmrghh, kVmrghw, kVmrglb, kVmrglh, kVmrglw,
  kVspltb, kVsplth, kVspltw, kVspltisb, kVspltish, kVspltisw,
  kVpkuhum, kVpkuwum, kVpkuhus, kVpkuwus, kVpkshus, kVpkswus, kVpkshss, kVpkswss, kVpkpx,
  kVupkhsb, kVupkhsh, kVupklsb, kVupklsh, kVupkhpx, kVupklpx,
  // Integer compares (all set CR6 in their record form).
  kVcmpequb, kVcmpequh, kVcmpequw, kVcmpgtub, kVcmpgtuh, kVcmpgtuw, kVcmpgtsb, kVcmpgtsh,
  kVcmpgtsw,
  // Floating point (ppc/vmx.cpp).
  kVaddfp, kVsubfp, kVmaddfp, kVnmsubfp, kVmaxfp, kVminfp,
  kVrefp, kVrsqrtefp, kVexptefp, kVlogefp,
  kVrfin, kVrfiz, kVrfip, kVrfim,
  kVcfux, kVcfsx, kVctuxs, kVctsxs,
  kVcmpeqfp, kVcmpgefp, kVcmpgtfp, kVcmpbfp,
  kVmxOpCount,
};

namespace lift {

constexpr uint32_t kVscrNj = 1u << 16, kVscrSat = 1u;

template <typename T>
[[gnu::always_inline]] inline T VGet(const Vec& v, int k) {
  T x;
  std::memcpy(&x, v.b + k * sizeof(T), sizeof(T));
  if constexpr (sizeof(T) == 2) return T(__builtin_bswap16(uint16_t(x)));
  else if constexpr (sizeof(T) == 4) return T(__builtin_bswap32(uint32_t(x)));
  else return x;
}
template <typename T>
[[gnu::always_inline]] inline void VPut(Vec& v, int k, T x) {
  if constexpr (sizeof(T) == 2) x = T(__builtin_bswap16(uint16_t(x)));
  else if constexpr (sizeof(T) == 4) x = T(__builtin_bswap32(uint32_t(x)));
  std::memcpy(v.b + k * sizeof(T), &x, sizeof(T));
}

// Saturates to T's range, setting VSCR[SAT] when it clamps.
template <typename T>
[[gnu::always_inline]] inline T VSat(int64_t x, uint32_t& vscr) {
  constexpr int64_t lo = int64_t(std::numeric_limits<T>::min()), hi = int64_t(std::numeric_limits<T>::max());
  if (x > hi) {
    if constexpr (!Mutated(7)) vscr |= kVscrSat;
    return T(hi);
  }
  if (x < lo) {
    if constexpr (!Mutated(7)) vscr |= kVscrSat;
    return T(lo);
  }
  return T(x);
}

// Element-wise over two vectors with elements of type T.
template <typename T, typename F>
[[gnu::always_inline]] inline Vec VMap(const Vec& a, const Vec& b, F f) {
  Vec r;
  for (int k = 0; k < int(16 / sizeof(T)); k++) VPut<T>(r, k, f(VGet<T>(a, k), VGet<T>(b, k)));
  return r;
}

// Floating-point operations (ppc/vmx.cpp), lane by lane: round to nearest,
// VMX's NaN rules (the first NaN of the operands, quieted; 0x7FC00000 for
// invalid ones), denormals flushed to zero when VSCR[NJ] is set.
Vec VmxFloat(Ctx& c, unsigned op, const Vec& a, const Vec& b, const Vec& cc, uint32_t imm);

// One VMX operation (VmxOp). `imm` is UIMM, SIMM or SHB as the operation
// takes; `cc` is the third source of the VA forms.
[[gnu::always_inline]] inline Vec Vmx(Ctx& c, unsigned op, const Vec& a, const Vec& b,
                                      const Vec& cc, uint32_t imm) {
  using i8 = int8_t;
  using i16 = int16_t;
  using i32 = int32_t;
  using u8 = uint8_t;
  using u16 = uint16_t;
  using u32 = uint32_t;
  uint32_t& vscr = c.vscr;
  Vec r{};
  switch (op) {
    case kVaddubm: return VMap<u8>(a, b, [](u8 x, u8 y) { return u8(x + y); });
    case kVadduhm: return VMap<u16>(a, b, [](u16 x, u16 y) { return u16(x + y); });
    case kVadduwm: return VMap<u32>(a, b, [](u32 x, u32 y) { return u32(x + y); });
    case kVaddubs: return VMap<u8>(a, b, [&](u8 x, u8 y) { return VSat<u8>(int64_t(x) + y, vscr); });
    case kVadduhs: return VMap<u16>(a, b, [&](u16 x, u16 y) { return VSat<u16>(int64_t(x) + y, vscr); });
    case kVadduws: return VMap<u32>(a, b, [&](u32 x, u32 y) { return VSat<u32>(int64_t(x) + y, vscr); });
    case kVaddsbs: return VMap<i8>(a, b, [&](i8 x, i8 y) { return VSat<i8>(int64_t(x) + y, vscr); });
    case kVaddshs: return VMap<i16>(a, b, [&](i16 x, i16 y) { return VSat<i16>(int64_t(x) + y, vscr); });
    case kVaddsws: return VMap<i32>(a, b, [&](i32 x, i32 y) { return VSat<i32>(int64_t(x) + y, vscr); });
    case kVaddcuw: return VMap<u32>(a, b, [](u32 x, u32 y) { return u32((uint64_t(x) + y) >> 32); });
    case kVsububm: return VMap<u8>(a, b, [](u8 x, u8 y) { return u8(x - y); });
    case kVsubuhm: return VMap<u16>(a, b, [](u16 x, u16 y) { return u16(x - y); });
    case kVsubuwm: return VMap<u32>(a, b, [](u32 x, u32 y) { return u32(x - y); });
    case kVsububs: return VMap<u8>(a, b, [&](u8 x, u8 y) { return VSat<u8>(int64_t(x) - y, vscr); });
    case kVsubuhs: return VMap<u16>(a, b, [&](u16 x, u16 y) { return VSat<u16>(int64_t(x) - y, vscr); });
    case kVsubuws: return VMap<u32>(a, b, [&](u32 x, u32 y) { return VSat<u32>(int64_t(x) - y, vscr); });
    case kVsubsbs: return VMap<i8>(a, b, [&](i8 x, i8 y) { return VSat<i8>(int64_t(x) - y, vscr); });
    case kVsubshs: return VMap<i16>(a, b, [&](i16 x, i16 y) { return VSat<i16>(int64_t(x) - y, vscr); });
    case kVsubsws: return VMap<i32>(a, b, [&](i32 x, i32 y) { return VSat<i32>(int64_t(x) - y, vscr); });
    // The complement of the borrow: 1 when no borrow.
    case kVsubcuw: return VMap<u32>(a, b, [](u32 x, u32 y) { return u32(x >= y); });
    case kVavgub: return VMap<u8>(a, b, [](u8 x, u8 y) { return u8((int64_t(x) + y + 1) >> 1); });
    case kVavguh: return VMap<u16>(a, b, [](u16 x, u16 y) { return u16((int64_t(x) + y + 1) >> 1); });
    case kVavguw: return VMap<u32>(a, b, [](u32 x, u32 y) { return u32((int64_t(x) + y + 1) >> 1); });
    case kVavgsb: return VMap<i8>(a, b, [](i8 x, i8 y) { return i8((int64_t(x) + y + 1) >> 1); });
    case kVavgsh: return VMap<i16>(a, b, [](i16 x, i16 y) { return i16((int64_t(x) + y + 1) >> 1); });
    case kVavgsw: return VMap<i32>(a, b, [](i32 x, i32 y) { return i32((int64_t(x) + y + 1) >> 1); });
    case kVmaxub: return VMap<u8>(a, b, [](u8 x, u8 y) { return x > y ? x : y; });
    case kVmaxuh: return VMap<u16>(a, b, [](u16 x, u16 y) { return x > y ? x : y; });
    case kVmaxuw: return VMap<u32>(a, b, [](u32 x, u32 y) { return x > y ? x : y; });
    case kVmaxsb: return VMap<i8>(a, b, [](i8 x, i8 y) { return x > y ? x : y; });
    case kVmaxsh: return VMap<i16>(a, b, [](i16 x, i16 y) { return x > y ? x : y; });
    case kVmaxsw: return VMap<i32>(a, b, [](i32 x, i32 y) { return x > y ? x : y; });
    case kVminub: return VMap<u8>(a, b, [](u8 x, u8 y) { return x < y ? x : y; });
    case kVminuh: return VMap<u16>(a, b, [](u16 x, u16 y) { return x < y ? x : y; });
    case kVminuw: return VMap<u32>(a, b, [](u32 x, u32 y) { return x < y ? x : y; });
    case kVminsb: return VMap<i8>(a, b, [](i8 x, i8 y) { return x < y ? x : y; });
    case kVminsh: return VMap<i16>(a, b, [](i16 x, i16 y) { return x < y ? x : y; });
    case kVminsw: return VMap<i32>(a, b, [](i32 x, i32 y) { return x < y ? x : y; });

    // Even (element 2k) and odd (2k + 1) elements, multiplied to double width.
    case kVmuleub: case kVmuloub:
      for (int k = 0; k < 8; k++) VPut<u16>(r, k, u16(VGet<u8>(a, 2 * k + (op == kVmuloub)) * VGet<u8>(b, 2 * k + (op == kVmuloub))));
      return r;
    case kVmulesb: case kVmulosb:
      for (int k = 0; k < 8; k++) VPut<i16>(r, k, i16(VGet<i8>(a, 2 * k + (op == kVmulosb)) * VGet<i8>(b, 2 * k + (op == kVmulosb))));
      return r;
    case kVmuleuh: case kVmulouh:
      for (int k = 0; k < 4; k++) VPut<u32>(r, k, u32(VGet<u16>(a, 2 * k + (op == kVmulouh))) * VGet<u16>(b, 2 * k + (op == kVmulouh)));
      return r;
    case kVmulesh: case kVmulosh:
      for (int k = 0; k < 4; k++) VPut<i32>(r, k, i32(VGet<i16>(a, 2 * k + (op == kVmulosh))) * VGet<i16>(b, 2 * k + (op == kVmulosh)));
      return r;
    case kVmhaddshs: case kVmhraddshs:
      for (int k = 0; k < 8; k++) {
        int64_t p = int64_t(int32_t(VGet<i16>(a, k)) * VGet<i16>(b, k)) + (op == kVmhraddshs ? 0x4000 : 0);
        VPut<i16>(r, k, VSat<i16>((p >> 15) + VGet<i16>(cc, k), vscr));
      }
      return r;
    case kVmladduhm:
      for (int k = 0; k < 8; k++) VPut<u16>(r, k, u16(uint32_t(VGet<u16>(a, k)) * VGet<u16>(b, k) + VGet<u16>(cc, k)));
      return r;
    // Multiply-sums: each word of `cc` plus the products of the bytes
    // (halfwords) of a and b in that word.
    case kVmsumubm: case kVmsummbm:
      for (int k = 0; k < 4; k++) {
        uint32_t s = VGet<u32>(cc, k);
        for (int j = 0; j < 4; j++) {
          int32_t x = op == kVmsummbm ? int32_t(VGet<i8>(a, 4 * k + j)) : int32_t(VGet<u8>(a, 4 * k + j));
          s += uint32_t(x * int32_t(VGet<u8>(b, 4 * k + j)));
        }
        VPut<u32>(r, k, s);
      }
      return r;
    case kVmsumuhm: case kVmsumuhs:
      for (int k = 0; k < 4; k++) {
        int64_t s = VGet<u32>(cc, k);
        for (int j = 0; j < 2; j++) s += int64_t(VGet<u16>(a, 2 * k + j)) * VGet<u16>(b, 2 * k + j);
        VPut<u32>(r, k, op == kVmsumuhs ? VSat<u32>(s, vscr) : u32(s));
      }
      return r;
    case kVmsumshm: case kVmsumshs:
      for (int k = 0; k < 4; k++) {
        int64_t s = VGet<i32>(cc, k);
        for (int j = 0; j < 2; j++) s += int64_t(VGet<i16>(a, 2 * k + j)) * VGet<i16>(b, 2 * k + j);
        VPut<i32>(r, k, op == kVmsumshs ? VSat<i32>(s, vscr) : i32(uint32_t(s)));
      }
      return r;
    case kVsum4ubs:
      for (int k = 0; k < 4; k++) {
        int64_t s = VGet<u32>(b, k);
        for (int j = 0; j < 4; j++) s += VGet<u8>(a, 4 * k + j);
        VPut<u32>(r, k, VSat<u32>(s, vscr));
      }
      return r;
    case kVsum4sbs:
      for (int k = 0; k < 4; k++) {
        int64_t s = VGet<i32>(b, k);
        for (int j = 0; j < 4; j++) s += VGet<i8>(a, 4 * k + j);
        VPut<i32>(r, k, VSat<i32>(s, vscr));
      }
      return r;
    case kVsum4shs:
      for (int k = 0; k < 4; k++) {
        int64_t s = int64_t(VGet<i32>(b, k)) + VGet<i16>(a, 2 * k) + VGet<i16>(a, 2 * k + 1);
        VPut<i32>(r, k, VSat<i32>(s, vscr));
      }
      return r;
    case kVsum2sws:
      for (int k = 0; k < 2; k++) {
        int64_t s = int64_t(VGet<i32>(b, 2 * k + 1)) + VGet<i32>(a, 2 * k) + VGet<i32>(a, 2 * k + 1);
        VPut<i32>(r, 2 * k + 1, VSat<i32>(s, vscr));
      }
      return r;
    case kVsumsws: {
      int64_t s = VGet<i32>(b, 3);
      for (int k = 0; k < 4; k++) s += VGet<i32>(a, k);
      VPut<i32>(r, 3, VSat<i32>(s, vscr));
      return r;
    }

    case kVand: return VMap<u32>(a, b, [](u32 x, u32 y) { return x & y; });
    case kVandc: return VMap<u32>(a, b, [](u32 x, u32 y) { return x & ~y; });
    case kVor: return VMap<u32>(a, b, [](u32 x, u32 y) { return x | y; });
    case kVnor: return VMap<u32>(a, b, [](u32 x, u32 y) { return ~(x | y); });
    case kVxor: return VMap<u32>(a, b, [](u32 x, u32 y) { return x ^ y; });
    case kVsel:
      for (int k = 0; k < 16; k++) r.b[k] = u8((a.b[k] & ~cc.b[k]) | (b.b[k] & cc.b[k]));
      return r;

    // Per-element rotates and shifts, by the low bits of b's element.
    case kVrlb: return VMap<u8>(a, b, [](u8 x, u8 y) { unsigned n = y & 7; return u8(n ? (x << n) | (x >> (8 - n)) : x); });
    case kVrlh: return VMap<u16>(a, b, [](u16 x, u16 y) { unsigned n = y & 15; return u16(n ? (x << n) | (x >> (16 - n)) : x); });
    case kVrlw: return VMap<u32>(a, b, [](u32 x, u32 y) { unsigned n = y & 31; return u32(n ? (x << n) | (x >> (32 - n)) : x); });
    case kVslb: return VMap<u8>(a, b, [](u8 x, u8 y) { return u8(x << (y & 7)); });
    case kVslh: return VMap<u16>(a, b, [](u16 x, u16 y) { return u16(x << (y & 15)); });
    case kVslw: return VMap<u32>(a, b, [](u32 x, u32 y) { return u32(x << (y & 31)); });
    case kVsrb: return VMap<u8>(a, b, [](u8 x, u8 y) { return u8(x >> (y & 7)); });
    case kVsrh: return VMap<u16>(a, b, [](u16 x, u16 y) { return u16(x >> (y & 15)); });
    case kVsrw: return VMap<u32>(a, b, [](u32 x, u32 y) { return u32(x >> (y & 31)); });
    case kVsrab: return VMap<i8>(a, b, [](i8 x, i8 y) { return i8(x >> (uint8_t(y) & 7)); });
    case kVsrah: return VMap<i16>(a, b, [](i16 x, i16 y) { return i16(x >> (uint16_t(y) & 15)); });
    case kVsraw: return VMap<i32>(a, b, [](i32 x, i32 y) { return i32(x >> (uint32_t(y) & 31)); });
    // Whole-register shifts: by bits (the low 3 bits of b's last byte; the
    // ISA wants all 16 equal) and by octets (bits 121-124).
    case kVsl: case kVsr: {
      unsigned n = b.b[15] & 7;
      for (int k = 0; k < 16; k++) {
        if (op == kVsl) r.b[k] = u8((a.b[k] << n) | (k < 15 && n ? a.b[k + 1] >> (8 - n) : 0));
        else r.b[k] = u8((a.b[k] >> n) | (k > 0 && n ? a.b[k - 1] << (8 - n) : 0));
      }
      return r;
    }
    case kVslo: case kVsro: {
      int n = (b.b[15] >> 3) & 15;
      for (int k = 0; k < 16; k++) {
        int src = op == kVslo ? k + n : k - n;
        r.b[k] = src >= 0 && src < 16 ? a.b[src] : 0;
      }
      return r;
    }

    case kVperm:
      for (int k = 0; k < 16; k++) {
        unsigned i = cc.b[k] & (Mutated(9) ? 15 : 31);
        r.b[k] = i < 16 ? a.b[i] : b.b[i - 16];
      }
      return r;
    case kVsldoi:
      for (int k = 0; k < 16; k++) {
        unsigned i = k + (imm & 15);
        r.b[k] = i < 16 ? a.b[i] : b.b[i - 16];
      }
      return r;
    case kVmrghb: case kVmrglb:
      for (int k = 0; k < 8; k++) {
        int s = k + (op == kVmrglb ? 8 : 0);
        r.b[2 * k] = a.b[s];
        r.b[2 * k + 1] = b.b[s];
      }
      return r;
    case kVmrghh: case kVmrglh:
      for (int k = 0; k < 4; k++) {
        int s = k + (op == kVmrglh ? 4 : 0);
        VPut<u16>(r, 2 * k, VGet<u16>(a, s));
        VPut<u16>(r, 2 * k + 1, VGet<u16>(b, s));
      }
      return r;
    case kVmrghw: case kVmrglw:
      for (int k = 0; k < 2; k++) {
        int s = k + (op == kVmrglw ? 2 : 0);
        VPut<u32>(r, 2 * k, VGet<u32>(a, s));
        VPut<u32>(r, 2 * k + 1, VGet<u32>(b, s));
      }
      return r;
    case kVspltb:
      for (int k = 0; k < 16; k++) r.b[k] = b.b[imm & 15];
      return r;
    case kVsplth:
      for (int k = 0; k < 8; k++) VPut<u16>(r, k, VGet<u16>(b, imm & 7));
      return r;
    case kVspltw:
      for (int k = 0; k < 4; k++) VPut<u32>(r, k, VGet<u32>(b, imm & 3));
      return r;
    // SIMM: 5 bits, sign-extended.
    case kVspltisb:
      for (int k = 0; k < 16; k++) r.b[k] = u8(int8_t(int32_t(imm << 27) >> 27));
      return r;
    case kVspltish:
      for (int k = 0; k < 8; k++) VPut<i16>(r, k, i16(int32_t(imm << 27) >> 27));
      return r;
    case kVspltisw:
      for (int k = 0; k < 4; k++) VPut<i32>(r, k, int32_t(imm << 27) >> 27);
      return r;
    // Packs: a's elements then b's, narrowed (modulo or saturating).
    case kVpkuhum:
      for (int k = 0; k < 16; k++) r.b[k] = u8(VGet<u16>(k < 8 ? a : b, k & 7));
      return r;
    case kVpkuwum:
      for (int k = 0; k < 8; k++) VPut<u16>(r, k, u16(VGet<u32>(k < 4 ? a : b, k & 3)));
      return r;
    case kVpkuhus:
      for (int k = 0; k < 16; k++) r.b[k] = VSat<u8>(VGet<u16>(k < 8 ? a : b, k & 7), vscr);
      return r;
    case kVpkuwus:
      for (int k = 0; k < 8; k++) VPut<u16>(r, k, VSat<u16>(VGet<u32>(k < 4 ? a : b, k & 3), vscr));
      return r;
    case kVpkshus:
      for (int k = 0; k < 16; k++) r.b[k] = VSat<u8>(VGet<i16>(k < 8 ? a : b, k & 7), vscr);
      return r;
    case kVpkswus:
      for (int k = 0; k < 8; k++) VPut<u16>(r, k, VSat<u16>(VGet<i32>(k < 4 ? a : b, k & 3), vscr));
      return r;
    case kVpkshss:
      for (int k = 0; k < 16; k++) r.b[k] = u8(VSat<i8>(VGet<i16>(k < 8 ? a : b, k & 7), vscr));
      return r;
    case kVpkswss:
      for (int k = 0; k < 8; k++) VPut<i16>(r, k, VSat<i16>(VGet<i32>(k < 4 ? a : b, k & 3), vscr));
      return r;
    // Pixels: 8:8:8:8 words to 1:5:5:5 halfwords (the top bit of the first
    // byte's low bit, the top 5 bits of the other three).
    case kVpkpx:
      for (int k = 0; k < 8; k++) {
        uint32_t w = VGet<u32>(k < 4 ? a : b, k & 3);
        u16 p = u16(((w >> 24) & 1) << 15 | ((w >> 19) & 0x1F) << 10 | ((w >> 11) & 0x1F) << 5 | ((w >> 3) & 0x1F));
        VPut<u16>(r, k, p);
      }
      return r;
    // Unpacks: the high or low half of b, sign-extended (pixels: 1:5:5:5 to
    // the sign-extended bit and three zero-extended 5-bit fields).
    case kVupkhsb: case kVupklsb:
      for (int k = 0; k < 8; k++) VPut<i16>(r, k, VGet<i8>(b, k + (op == kVupklsb ? 8 : 0)));
      return r;
    case kVupkhsh: case kVupklsh:
      for (int k = 0; k < 4; k++) VPut<i32>(r, k, VGet<i16>(b, k + (op == kVupklsh ? 4 : 0)));
      return r;
    case kVupkhpx: case kVupklpx:
      for (int k = 0; k < 4; k++) {
        u16 p = VGet<u16>(b, k + (op == kVupklpx ? 4 : 0));
        uint32_t w = (p & 0x8000 ? 0xFFu : 0u) << 24 | uint32_t((p >> 10) & 0x1F) << 16 |
                     uint32_t((p >> 5) & 0x1F) << 8 | uint32_t(p & 0x1F);
        VPut<u32>(r, k, w);
      }
      return r;

    case kVcmpequb: return VMap<u8>(a, b, [](u8 x, u8 y) { return u8(x == y ? 0xFF : 0); });
    case kVcmpequh: return VMap<u16>(a, b, [](u16 x, u16 y) { return u16(x == y ? 0xFFFF : 0); });
    case kVcmpequw: return VMap<u32>(a, b, [](u32 x, u32 y) { return x == y ? ~0u : 0u; });
    case kVcmpgtub: return VMap<u8>(a, b, [](u8 x, u8 y) { return u8(x > y ? 0xFF : 0); });
    case kVcmpgtuh: return VMap<u16>(a, b, [](u16 x, u16 y) { return u16(x > y ? 0xFFFF : 0); });
    case kVcmpgtuw: return VMap<u32>(a, b, [](u32 x, u32 y) { return x > y ? ~0u : 0u; });
    case kVcmpgtsb: return VMap<i8>(a, b, [](i8 x, i8 y) { return i8(x > y ? -1 : 0); });
    case kVcmpgtsh: return VMap<i16>(a, b, [](i16 x, i16 y) { return i16(x > y ? -1 : 0); });
    case kVcmpgtsw: return VMap<i32>(a, b, [](i32 x, i32 y) { return x > y ? -1 : 0; });
    default:
      return VmxFloat(c, op, a, b, cc, imm);
  }
}

// CR6 of a compare's record form: all elements true (LT bit), none true (EQ
// bit). VCMPBFP: only the EQ bit, when every element is within bounds.
[[gnu::always_inline]] inline uint32_t VCr6(const Vec& r, bool bounds) {
  bool all = true, none = true;
  for (int k = 0; k < 16; k++) {
    if (r.b[k] != 0xFF) all = false;
    if (r.b[k]) none = false;
  }
  return bounds ? (none ? 2u : 0u) : (all ? 8u : 0u) | (none ? 2u : 0u);
}

// VMX loads and stores. Quadwords at the address with its low 4 bits
// cleared (LVX STVX and their LRU-hint forms); one element at its own
// position (LVEBX ... STVEWX; the ISA leaves the other elements of a load
// undefined, and this leaves them as they were).
[[gnu::always_inline]] inline Vec Lvx(const uint8_t* m, uint32_t ea) {
  Vec v;
  std::memcpy(v.b, m + (ea & ~15u), 16);
  return v;
}
[[gnu::always_inline]] inline void Stvx(uint8_t* m, uint32_t ea, const Vec& v) {
  std::memcpy(m + (ea & ~15u), v.b, 16);
}
[[gnu::always_inline]] inline void Lve(const uint8_t* m, uint32_t ea, Vec& v, unsigned size) {
  ea &= ~(size - 1);
  std::memcpy(v.b + (ea & 15), m + ea, size);
}
[[gnu::always_inline]] inline void Stve(uint8_t* m, uint32_t ea, const Vec& v, unsigned size) {
  ea &= ~(size - 1);
  std::memcpy(m + ea, v.b + (ea & 15), size);
}
// LVSL / LVSR: the permute control for the address's low 4 bits.
[[gnu::always_inline]] inline Vec Lvsl(uint32_t ea, bool right) {
  Vec v;
  unsigned sh = ea & 15;
  for (int k = 0; k < 16; k++) v.b[k] = uint8_t(right ? 16 - sh + k : sh + k);
  return v;
}
// The Xenon's (and Cell's) unaligned pair, with their LRU-hint forms:
// LVLX loads the bytes from the address to the end of its quadword into the
// left of the register, zeros after; LVRX the bytes from the quadword's
// start up to the address into the right, zeros before. STVLX and STVRX
// store the same parts back.
[[gnu::always_inline]] inline Vec Lvlx(const uint8_t* m, uint32_t ea) {
  Vec v{};
  unsigned n = 16 - (ea & 15);
  std::memcpy(v.b, m + ea, n);
  return v;
}
[[gnu::always_inline]] inline Vec Lvrx(const uint8_t* m, uint32_t ea) {
  Vec v{};
  unsigned n = ea & 15;
  std::memcpy(v.b + 16 - n, m + (ea & ~15u), n);
  return v;
}
[[gnu::always_inline]] inline void Stvlx(uint8_t* m, uint32_t ea, const Vec& v) {
  std::memcpy(m + ea, v.b, 16 - (ea & 15));
}
[[gnu::always_inline]] inline void Stvrx(uint8_t* m, uint32_t ea, const Vec& v) {
  unsigned n = ea & 15;
  std::memcpy(m + (ea & ~15u), v.b + 16 - n, n);
}
// MFVSCR: VSCR in the last word, the rest zero.
[[gnu::always_inline]] inline Vec Mfvscr(uint32_t vscr) {
  Vec v{};
  VPut<uint32_t>(v, 3, vscr);
  return v;
}

}  // namespace lift
}  // namespace recomp::ppc
