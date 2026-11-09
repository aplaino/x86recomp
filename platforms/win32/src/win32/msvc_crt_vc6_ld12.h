// VC6's 12-byte extended arithmetic (_LDBL12), which its floating-point
// conversions share (msvc_crt_vc6_output.cpp). Internal to them.

#pragma once

#include <cstdint>
#include <cstring>

namespace recomp::native::msvc_crt::vc6 {

// Little-endian: 16 guard bits, a 64-bit significand (explicit integer
// bit), then sign and biased exponent.
struct Ld12 {
  uint8_t b[12];
  uint16_t W(int o) const {
    uint16_t v;
    std::memcpy(&v, b + o, 2);
    return v;
  }
  uint32_t D(int o) const {
    uint32_t v;
    std::memcpy(&v, b + o, 4);
    return v;
  }
  void SetW(int o, uint16_t v) { std::memcpy(b + o, &v, 2); }
  void SetD(int o, uint32_t v) { std::memcpy(b + o, &v, 4); }
};

// __shl_12, __shr_12: the 96 bits shifted by one.
void Shl12(Ld12& x);
void Shr12(Ld12& x);
// __addl: a 32-bit add, the carry returned.
bool Addl(uint32_t a, uint32_t b, uint32_t* out);
// __add_12: x += y.
void Add12(Ld12& x, const Ld12& y);
// __ld12mul: x *= y.
void Ld12Mul(Ld12& x, const Ld12& y);
// __multtenpow12(x, n, mult12): x *= 10^n, n's octal digits from the
// lowest; without mult12 the guard word is cleared first (when n is not
// 0).
void MultTenPow12(Ld12& x, int n, bool mult12);

}  // namespace recomp::native::msvc_crt::vc6
