// AES-NI, PCLMULQDQ, the SSE4.2 string compares and FXSAVE/FXRSTOR (see
// sse.h). Plain integer code: the same bits on every host.
//
// AES: FIPS-197's round steps on the x86 state layout (byte i of the
// register is row i % 4, column i / 4). The S-box is computed once from
// its definition (the GF(2^8) inverse followed by the affine map).
//
// PCMPxSTRx: the SDM's imm8 control (vol. 2, "Imm8 Control Byte Operation
// for PCMPESTRI / PCMPESTRM / PCMPISTRI / PCMPISTRM"): element format,
// aggregation, polarity and output selection, with its table of forced
// results for invalid elements.

#include <cstdint>
#include <cstring>

#include "runtime/sse.h"
#include "runtime/x87.h"

namespace recomp::sse {
namespace {

uint8_t Xtime(uint8_t a) { return uint8_t(a << 1) ^ (a & 0x80 ? 0x1B : 0); }

uint8_t GfMul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  for (; b; b >>= 1, a = Xtime(a))
    if (b & 1) r ^= a;
  return r;
}

struct Sbox {
  uint8_t fwd[256], inv[256];
  Sbox() {
    for (int x = 0; x < 256; x++) {
      uint8_t v = 0;  // the inverse of x (0 for 0)
      for (int y = 1; y < 256 && x; y++)
        if (GfMul(uint8_t(x), uint8_t(y)) == 1) v = uint8_t(y);
      uint8_t s = v;
      for (int k = 1; k <= 4; k++) s ^= uint8_t(v << k | v >> (8 - k));
      fwd[x] = s ^ 0x63;
    }
    for (int x = 0; x < 256; x++) inv[fwd[x]] = uint8_t(x);
  }
};

const Sbox& Box() {
  static const Sbox b;
  return b;
}

Xmm ShiftRows(const Xmm& s, bool inverse) {
  Xmm r;
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++)
      r.b[row + 4 * c] = s.b[row + 4 * ((inverse ? c - row + 4 : c + row) % 4)];
  return r;
}

void SubBytes(Xmm& s, bool inverse) {
  const Sbox& b = Box();
  for (int i = 0; i < 16; i++) s.b[i] = inverse ? b.inv[s.b[i]] : b.fwd[s.b[i]];
}

void MixColumns(Xmm& s, bool inverse) {
  static const uint8_t kFwd[4] = {2, 3, 1, 1}, kInv[4] = {14, 11, 13, 9};
  const uint8_t* k = inverse ? kInv : kFwd;
  for (int c = 0; c < 4; c++) {
    uint8_t a[4] = {s.b[4 * c], s.b[4 * c + 1], s.b[4 * c + 2], s.b[4 * c + 3]};
    for (int row = 0; row < 4; row++) {
      uint8_t v = 0;
      for (int i = 0; i < 4; i++) v ^= GfMul(a[i], k[(i - row + 4) % 4]);
      s.b[4 * c + row] = v;
    }
  }
}

uint32_t SubWord(uint32_t w) {
  const Sbox& b = Box();
  return uint32_t(b.fwd[w & 0xFF]) | uint32_t(b.fwd[w >> 8 & 0xFF]) << 8 |
         uint32_t(b.fwd[w >> 16 & 0xFF]) << 16 | uint32_t(b.fwd[w >> 24]) << 24;
}

}  // namespace

void AesEnc(Xmm& d, const Xmm& s, bool last) {
  Xmm t = ShiftRows(d, false);
  SubBytes(t, false);
  if (!last) MixColumns(t, false);
  Xor(t, s);
  d = t;
}

void AesDec(Xmm& d, const Xmm& s, bool last) {
  Xmm t = ShiftRows(d, true);
  SubBytes(t, true);
  if (!last) MixColumns(t, true);
  Xor(t, s);
  d = t;
}

void AesImc(Xmm& d, const Xmm& s) {
  Xmm t = s;
  MixColumns(t, true);
  d = t;
}

void AesKeygenAssist(Xmm& d, const Xmm& s, uint8_t rcon) {
  auto rot = [](uint32_t w) { return w >> 8 | w << 24; };
  const uint32_t x1 = SubWord(Lane<uint32_t>(s, 1)), x3 = SubWord(Lane<uint32_t>(s, 3));
  Xmm r;
  SetLane<uint32_t>(r, 0, x1);
  SetLane<uint32_t>(r, 1, rot(x1) ^ rcon);
  SetLane<uint32_t>(r, 2, x3);
  SetLane<uint32_t>(r, 3, rot(x3) ^ rcon);
  d = r;
}

void Clmul(Xmm& d, const Xmm& s, uint8_t imm) {
  const uint64_t a = Lane<uint64_t>(d, imm & 1), b = Lane<uint64_t>(s, (imm >> 4) & 1);
  uint64_t lo = 0, hi = 0;
  for (int i = 0; i < 64; i++)
    if (b >> i & 1) {
      lo ^= a << i;
      if (i) hi ^= a >> (64 - i);
    }
  SetLane<uint64_t>(d, 0, lo);
  SetLane<uint64_t>(d, 1, hi);
}

namespace {

// IntRes2 into *res2; returns the flags as PcmpStrI does.
uint32_t PcmpStr(const Xmm& a, const Xmm& b, uint8_t imm, bool expl, int64_t la, int64_t lb,
                 uint32_t* res2_out) {
  const bool words = imm & 1, sign = imm & 2;
  const int n = words ? 8 : 16;
  auto elem = [&](const Xmm& x, int i) -> int32_t {
    if (words) return sign ? int32_t(Lane<int16_t>(x, i)) : int32_t(Lane<uint16_t>(x, i));
    return sign ? int32_t(int8_t(x.b[i])) : int32_t(x.b[i]);
  };
  auto length = [&](const Xmm& x, int64_t l) -> int {
    if (expl) {
      uint64_t m = l < 0 ? uint64_t(0) - uint64_t(l) : uint64_t(l);
      return m > uint64_t(n) ? n : int(m);
    }
    for (int i = 0; i < n; i++)
      if (elem(x, i) == 0) return i;
    return n;
  };
  const int len1 = length(a, la), len2 = length(b, lb);
  const int agg = (imm >> 2) & 3;
  // BoolRes[i][j]: element i of a against element j of b, forced for
  // invalid elements as the SDM's table says.
  auto bool_res = [&](int i, int j) -> bool {
    const bool va = i < len1, vb = j < len2;
    if (!va || !vb) {
      if (agg == 2) return !va && !vb;  // equal each: true only if both invalid
      if (agg == 3) return !va;         // equal ordered: true if a's is invalid
      return false;
    }
    const int32_t x = elem(a, i), y = elem(b, j);
    if (agg == 1) return i & 1 ? y <= x : y >= x;  // ranges: [a[2k], a[2k+1]]
    return x == y;
  };
  uint32_t res1 = 0;
  for (int j = 0; j < n; j++) {
    bool r = false;
    switch (agg) {
      case 0:  // equal any
        for (int i = 0; i < n; i++) r = r || bool_res(i, j);
        break;
      case 1:  // ranges
        for (int i = 0; i + 1 < n; i += 2) r = r || (bool_res(i, j) && bool_res(i + 1, j));
        break;
      case 2:  // equal each
        r = bool_res(j, j);
        break;
      default:  // equal ordered: a found at j
        r = true;
        for (int k = 0; j + k < n; k++) r = r && bool_res(k, j + k);
        break;
    }
    if (r) res1 |= 1u << j;
  }
  const uint32_t all = (1u << n) - 1;
  uint32_t res2 = res1;
  switch ((imm >> 4) & 3) {
    case 1: res2 = ~res1 & all; break;                                // negative
    case 3: res2 = (res1 ^ ((1u << len2) - 1)) & all; break;          // masked negative
    default: break;
  }
  *res2_out = res2;
  return uint32_t(res2 != 0) | uint32_t(len2 < n) << 1 | uint32_t(len1 < n) << 2 |
         (res2 & 1) << 3;
}

}  // namespace

uint32_t PcmpStrI(const Xmm& a, const Xmm& b, uint8_t imm, bool expl, int64_t la, int64_t lb,
                  uint32_t* index) {
  uint32_t res2;
  const uint32_t f = PcmpStr(a, b, imm, expl, la, lb, &res2);
  if (!res2)
    *index = imm & 1 ? 8 : 16;
  else
    *index = imm & 0x40 ? uint32_t(31 - __builtin_clz(res2)) : uint32_t(__builtin_ctz(res2));
  return f;
}

uint32_t PcmpStrM(const Xmm& a, const Xmm& b, uint8_t imm, bool expl, int64_t la, int64_t lb,
                  Xmm* mask) {
  uint32_t res2;
  const uint32_t f = PcmpStr(a, b, imm, expl, la, lb, &res2);
  Xmm r{};
  if (!(imm & 0x40)) {
    SetLane<uint16_t>(r, 0, uint16_t(res2));
  } else {
    const int n = imm & 1 ? 8 : 16, size = 16 / n;
    for (int i = 0; i < n; i++)
      if (res2 >> i & 1)
        for (int k = 0; k < size; k++) r.b[i * size + k] = 0xFF;
  }
  *mask = r;
  return f;
}

void Fxsave(Ctx& c, uint8_t* p) {
  x87::FxSave(c.fpu, p);
  std::memcpy(p + 24, &c.mxcsr, 4);
  const uint32_t mask = 0xFFFF;
  std::memcpy(p + 28, &mask, 4);
  for (int i = 0; i < 16; i++) std::memcpy(p + 160 + 16 * i, c.xmm[i].b, 16);
}

void Fxrstor(Ctx& c, const uint8_t* p, uint64_t va) {
  uint32_t mxcsr;
  std::memcpy(&mxcsr, p + 24, 4);
  Ldmxcsr(c, mxcsr, va);
  x87::FxRestore(c.fpu, p);
  for (int i = 0; i < 16; i++) std::memcpy(c.xmm[i].b, p + 160 + 16 * i, 16);
}

}  // namespace recomp::sse
