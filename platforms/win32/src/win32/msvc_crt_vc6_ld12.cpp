// VC6's 12-byte extended arithmetic; msvc_crt_vc6_ld12.h.

#include "win32/msvc_crt_vc6_ld12.h"

namespace recomp::native::msvc_crt::vc6 {

void Shl12(Ld12& x) {
  const uint32_t a = x.D(0), b = x.D(4);
  x.SetD(0, a << 1);
  x.SetD(4, b << 1 | a >> 31);
  x.SetD(8, x.D(8) << 1 | b >> 31);
}

void Shr12(Ld12& x) {
  const uint32_t b = x.D(4);
  x.SetD(4, b >> 1 | x.D(8) << 31);
  x.SetD(8, x.D(8) >> 1);
  x.SetD(0, x.D(0) >> 1 | b << 31);
}

bool Addl(uint32_t a, uint32_t b, uint32_t* out) {
  const uint32_t s = a + b;
  *out = s;
  return s < a || s < b;
}

void Add12(Ld12& x, const Ld12& y) {
  uint32_t v;
  if (Addl(x.D(0), y.D(0), &v)) {
    x.SetD(0, v);
    uint32_t m;
    if (Addl(x.D(4), 1, &m)) x.SetD(8, x.D(8) + 1);
    x.SetD(4, m);
  } else {
    x.SetD(0, v);
  }
  if (Addl(x.D(4), y.D(4), &v)) x.SetD(8, x.D(8) + 1);
  x.SetD(4, v);
  Addl(x.D(8), y.D(8), &v);
  x.SetD(8, v);
}

// The product keeps the partial products of the top words only (word i of
// x with word j of y, i + j >= 4), each carry out of a 32-bit sum
// incrementing the 16-bit word above it; then it normalizes (sticky bits
// when it shifts right), rounds the 16 guard bits to nearest even and
// saturates to infinity.
void Ld12Mul(Ld12& x, const Ld12& y) {
  const uint16_t ex = x.W(10), ey = y.W(10);
  const uint16_t sign = (ex ^ ey) & 0x8000;
  const uint16_t e1 = ex & 0x7FFF, e2 = ey & 0x7FFF;
  uint32_t e = uint32_t(e1) + e2;
  auto infinity = [&] {
    x.SetD(4, 0);
    x.SetD(0, 0);
    x.SetD(8, sign ? 0xFFFF8000u : 0x7FFF8000u);
  };
  auto zero = [&] {
    x.SetD(8, 0);
    x.SetD(4, 0);
    x.SetD(0, 0);
  };
  if (e1 >= 0x7FFF || e2 >= 0x7FFF || uint16_t(e) > 0xBFFD) return infinity();
  if (uint16_t(e) <= 0x3FBF) return zero();
  if (e1 == 0) {
    e++;
    if (!(x.D(8) & 0x7FFFFFFF) && !x.D(4) && !x.D(0)) return x.SetW(10, 0);
  }
  if (e2 == 0) {
    e++;
    if (!(y.D(8) & 0x7FFFFFFF) && !y.D(4) && !y.D(0)) return zero();
  }
  Ld12 p{};
  for (int i = 0; i < 5; i++)
    for (int k = 0; k < 5 - i; k++) {
      uint32_t s;
      if (Addl(p.D(2 * i), uint32_t(x.W(2 * (i + k))) * y.W(2 * (4 - k)), &s) && i < 4)
        p.SetW(4 + 2 * i, uint16_t(p.W(4 + 2 * i) + 1));
      p.SetD(2 * i, s);
    }
  e += 0xC002;
  bool round = false;
  if (int16_t(e) > 0) {
    while (!(p.b[11] & 0x80)) {
      Shl12(p);
      e += 0xFFFF;
      if (int16_t(e) <= 0) break;
    }
    round = int16_t(e) > 0;
  }
  if (!round) {
    e += 0xFFFF;
    if (int16_t(e) < 0) {
      int n = -int16_t(e);
      e += uint32_t(n);
      uint32_t sticky = 0;
      do {
        if (p.b[0] & 1) sticky++;
        Shr12(p);
      } while (--n);
      if (sticky) p.b[0] |= 1;
    }
  }
  if (p.W(0) > 0x8000 || (p.D(0) & 0x1FFFF) == 0x18000) {
    if (p.D(2) == 0xFFFFFFFF) {
      p.SetD(2, 0);
      if (p.D(6) == 0xFFFFFFFF) {
        p.SetD(6, 0);
        if (p.W(10) == 0xFFFF) {
          e++;
          p.SetW(10, 0x8000);
        } else {
          p.SetW(10, uint16_t(p.W(10) + 1));
        }
      } else {
        p.SetD(6, p.D(6) + 1);
      }
    } else {
      p.SetD(2, p.D(2) + 1);
    }
  }
  if (uint16_t(e) >= 0x7FFF) return infinity();
  x.SetW(0, p.W(2));
  x.SetD(2, p.D(4));
  x.SetD(6, p.D(8));
  x.SetW(10, uint16_t(e) | sign);
}

// VC6's tables of 10^(j * 8^g) and 10^-(j * 8^g), j 1..7 (g 0..3) and
// 10^4096: the significand rounded to nearest at 64 bits, the guard word
// the low 16 bits of the value rounded to nearest at 80 bits.
// MultTenPow12 takes one less in the significand's low bits where the
// guard word is 0x8000 or more (undoing the 64-bit rounding up).
// clang-format off
constexpr uint16_t kPow10[2][29][6] = {
    {
        {0x0000, 0x0000, 0x0000, 0x0000, 0xA000, 0x4002},  // 10^1
        {0x0000, 0x0000, 0x0000, 0x0000, 0xC800, 0x4005},  // 10^2
        {0x0000, 0x0000, 0x0000, 0x0000, 0xFA00, 0x4008},  // 10^3
        {0x0000, 0x0000, 0x0000, 0x0000, 0x9C40, 0x400C},  // 10^4
        {0x0000, 0x0000, 0x0000, 0x0000, 0xC350, 0x400F},  // 10^5
        {0x0000, 0x0000, 0x0000, 0x0000, 0xF424, 0x4012},  // 10^6
        {0x0000, 0x0000, 0x0000, 0x8000, 0x9896, 0x4016},  // 10^7
        {0x0000, 0x0000, 0x0000, 0x2000, 0xBEBC, 0x4019},  // 10^8
        {0x0000, 0x0000, 0x0400, 0xC9BF, 0x8E1B, 0x4034},  // 10^16
        {0x0000, 0xA100, 0xCCED, 0x1BCE, 0xD3C2, 0x404E},  // 10^24
        {0xF020, 0xB59E, 0x2B70, 0xADA8, 0x9DC5, 0x4069},  // 10^32
        {0x5DD0, 0x25FD, 0x1AE5, 0x4F8E, 0xEB19, 0x4083},  // 10^40
        {0x9671, 0x95D7, 0x0E43, 0x8D05, 0xAF29, 0x409E},  // 10^48
        {0xBFF9, 0x44A0, 0x81ED, 0x8F12, 0x8281, 0x40B9},  // 10^56
        {0x3CBF, 0xA6D5, 0xFFCF, 0x1F49, 0xC278, 0x40D3},  // 10^64
        {0xC66F, 0x8CE0, 0x80E9, 0x47C9, 0x93BA, 0x41A8},  // 10^128
        {0x85BC, 0x556B, 0x3927, 0xF78D, 0xE070, 0x427C},  // 10^192
        {0xDDBC, 0xDE8E, 0x9DF9, 0xEBFB, 0xAA7E, 0x4351},  // 10^256
        {0xE6A1, 0xE376, 0xF2CC, 0x2F29, 0x8184, 0x4426},  // 10^320
        {0x1028, 0xAA17, 0xAEF8, 0xE310, 0xC4C5, 0x44FA},  // 10^384
        {0xA7EB, 0xF3D4, 0xEBF7, 0x4AE1, 0x957A, 0x45CF},  // 10^448
        {0xCC65, 0x91C7, 0xA60E, 0xA0AE, 0xE319, 0x46A3},  // 10^512
        {0x650D, 0x0C17, 0x8175, 0x7586, 0xC976, 0x4D48},  // 10^1024
        {0x4258, 0xA7E4, 0x3993, 0x353B, 0xB2B8, 0x53ED},  // 10^1536
        {0xA74D, 0x5DE5, 0xC53D, 0x3B5D, 0x9E8B, 0x5A92},  // 10^2048
        {0x5DFF, 0xF0A6, 0x20A1, 0x54C0, 0x8CA5, 0x6137},  // 10^2560
        {0xFDD1, 0x5A8B, 0xD88B, 0x5D25, 0xF989, 0x67DB},  // 10^3072
        {0x95AA, 0xF3F8, 0xBF27, 0xC8A2, 0xDD5D, 0x6E80},  // 10^3584
        {0xC94C, 0x979B, 0x8A20, 0x5202, 0xC460, 0x7525},  // 10^4096
    },
    {
        {0xCCCD, 0xCCCD, 0xCCCC, 0xCCCC, 0xCCCC, 0x3FFB},  // 10^-1
        {0x3D71, 0xD70A, 0x70A3, 0x0A3D, 0xA3D7, 0x3FF8},  // 10^-2
        {0x645A, 0xDF3B, 0x8D4F, 0x6E97, 0x8312, 0x3FF5},  // 10^-3
        {0xD3C3, 0x652C, 0xE219, 0x1758, 0xD1B7, 0x3FF1},  // 10^-4
        {0x0FD0, 0x8423, 0x1B47, 0xAC47, 0xA7C5, 0x3FEE},  // 10^-5
        {0xA640, 0x69B6, 0xAF6C, 0xBD05, 0x8637, 0x3FEB},  // 10^-6
        {0x3D33, 0x42BC, 0xE57A, 0x94D5, 0xD6BF, 0x3FE7},  // 10^-7
        {0xFDC2, 0xCEFD, 0x8461, 0x7711, 0xABCC, 0x3FE4},  // 10^-8
        {0x4C2F, 0xE15B, 0xC44D, 0x94BE, 0xE695, 0x3FC9},  // 10^-16
        {0xC492, 0x3B53, 0x4475, 0x14CD, 0x9ABE, 0x3FAF},  // 10^-24
        {0x67DE, 0x94BA, 0x4539, 0x1EAD, 0xCFB1, 0x3F94},  // 10^-32
        {0x2324, 0xE2C6, 0xBABC, 0x313B, 0x8B61, 0x3F7A},  // 10^-40
        {0x5561, 0xC159, 0xB17E, 0x7C53, 0xBB12, 0x3F5F},  // 10^-48
        {0xEED7, 0x8D2F, 0xBE06, 0x8592, 0xFB15, 0x3F44},  // 10^-56
        {0x3F24, 0xE9A5, 0xA539, 0xEA27, 0xA87F, 0x3F2A},  // 10^-64
        {0xAC7D, 0xE4A1, 0x64BC, 0x467C, 0xDDD0, 0x3E55},  // 10^-128
        {0x7B63, 0xCC06, 0x5423, 0x8377, 0x91FF, 0x3D81},  // 10^-192
        {0xFA91, 0x193A, 0x637A, 0x4325, 0xC031, 0x3CAC},  // 10^-256
        {0x8921, 0x38D1, 0x4782, 0xB897, 0xFD00, 0x3BD7},  // 10^-320
        {0x88DC, 0x0858, 0xB11B, 0xE3E8, 0xA686, 0x3B03},  // 10^-384
        {0x84C6, 0x4245, 0xB607, 0x7599, 0xDB37, 0x3A2E},  // 10^-448
        {0x7133, 0xD21C, 0xDB23, 0xEE32, 0x9049, 0x395A},  // 10^-512
        {0x87A6, 0xC0BE, 0xDA57, 0x82A5, 0xA2A6, 0x32B5},  // 10^-1024
        {0x68E2, 0x11B2, 0x52A7, 0x449F, 0xB759, 0x2C10},  // 10^-1536
        {0x4925, 0x2DE4, 0x3436, 0x534F, 0xCEAE, 0x256B},  // 10^-2048
        {0x598F, 0xA404, 0xDEC0, 0x7DC2, 0xE8FB, 0x1EC6},  // 10^-2560
        {0xE79E, 0x5A88, 0x9157, 0xBF3C, 0x8350, 0x1822},  // 10^-3072
        {0x4B4E, 0x6265, 0x83FD, 0xAF8F, 0x9406, 0x117D},  // 10^-3584
        {0x2DE4, 0x9FDE, 0xD2CE, 0x04C8, 0xA6DD, 0x0AD8},  // 10^-4096
    },
};
// clang-format on

void MultTenPow12(Ld12& x, int n, bool mult12) {
  if (!n) return;
  const bool neg = n < 0;
  if (neg) n = -n;
  if (!mult12) x.SetW(0, 0);
  for (int g = 0; n; g++, n >>= 3) {
    const int j = n & 7;
    if (!j) continue;
    Ld12 e;
    for (int w = 0; w < 6; w++) e.SetW(2 * w, kPow10[neg][7 * g + j - 1][w]);
    if (e.W(0) >= 0x8000) e.SetD(2, e.D(2) - 1);
    Ld12Mul(x, e);
  }
}

}  // namespace recomp::native::msvc_crt::vc6
