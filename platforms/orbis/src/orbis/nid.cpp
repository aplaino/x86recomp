#include "orbis/nid.h"

#include <cstdint>
#include <vector>

namespace recomp::orbis {

namespace {

constexpr uint8_t kSuffix[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                                 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};

uint32_t Rol(uint32_t x, int n) { return x << n | x >> (32 - n); }

// FIPS 180-4 SHA-1; only the first 8 bytes of the digest are needed.
void Sha1(const std::vector<uint8_t>& msg, uint8_t out[20]) {
  std::vector<uint8_t> m = msg;
  const uint64_t bits = uint64_t(msg.size()) * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; i--) m.push_back(uint8_t(bits >> (8 * i)));
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  for (size_t blk = 0; blk < m.size(); blk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = uint32_t(m[blk + 4 * i]) << 24 | uint32_t(m[blk + 4 * i + 1]) << 16 |
             uint32_t(m[blk + 4 * i + 2]) << 8 | m[blk + 4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      const uint32_t t = Rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = Rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  for (int i = 0; i < 20; i++) out[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
}

}  // namespace

std::string Nid(std::string_view name) {
  std::vector<uint8_t> msg(name.begin(), name.end());
  msg.insert(msg.end(), kSuffix, kSuffix + 16);
  uint8_t digest[20];
  Sha1(msg, digest);
  uint8_t v[9] = {};  // 8 bytes, reversed, and a zero pad to 3-byte groups
  for (int i = 0; i < 8; i++) v[i] = digest[7 - i];
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
  std::string s;
  for (int i = 0; i < 9; i += 3) {
    const uint32_t g = uint32_t(v[i]) << 16 | uint32_t(v[i + 1]) << 8 | v[i + 2];
    for (int k = 18; k >= 0; k -= 6) s += kAlphabet[g >> k & 63];
  }
  s.resize(11);
  return s;
}

}  // namespace recomp::orbis
