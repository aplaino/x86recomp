// libSceDiscMap HLE: where the data of a game made for disc lies, from
// the game's own /app0/sce_discmap.plt (and _patch.plt, which wins). Each
// is a table, big-endian: "PLT ", the block size, the entry count, 12
// bytes, then sorted 24-byte entries: the MD5 of the file's path below
// /app0/, a 24-bit block index (the file offset over the block size), a
// 24-bit location and a 16-bit length. A request is on the HDD where the
// system's bitmap of installed blocks says so; the game here is
// installed in full, so all of it is. Checks and error codes are
// libSceDiscMap 11.00's.

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "orbis/hle.h"
#include "runtime/runtime.h"

namespace {

constexpr uint32_t kErrInvalidArgument = 0x81100001, kErrNotFound = 0x81100002,
                   kErrNoEntry = 0x81100003;

// MD5 (RFC 1321).
void Md5(const std::string& msg, uint8_t out[16]) {
  static const uint32_t k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const uint8_t r[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
  std::vector<uint8_t> m(msg.begin(), msg.end());
  const uint64_t bits = uint64_t(m.size()) * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 0; i < 8; i++) m.push_back(uint8_t(bits >> (8 * i)));
  uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
      w[i] = m[off + 4 * i] | m[off + 4 * i + 1] << 8 | m[off + 4 * i + 2] << 16 |
             uint32_t(m[off + 4 * i + 3]) << 24;
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
      uint32_t f;
      int g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const uint32_t t = d, s = r[i / 16 * 4 + i % 4];
      d = c;
      c = b;
      const uint32_t x = a + f + k[i] + w[g];
      b += x << s | x >> (32 - s);
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
  }
  for (int i = 0; i < 16; i++) out[i] = uint8_t(h[i / 4] >> (8 * (i % 4)));
}

uint64_t Be(const uint8_t* p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v = v << 8 | p[i];
  return v;
}

struct Table {
  std::vector<uint8_t> data;  // empty: no such file
  uint64_t block = 0, entries = 0;
  const uint8_t* entry(uint64_t i) const { return &data[0x18 + 0x18 * i]; }
};

std::mutex g_mu;
bool g_loaded = false;
Table g_tables[2];  // the patch's, then the game's

void Load() {
  if (g_loaded) return;
  g_loaded = true;
  const char* names[2] = {"/sce_discmap_patch.plt", "/sce_discmap.plt"};
  for (int i = 0; i < 2; i++) {
    std::FILE* f = std::fopen((recomp::g_proc->game_dir + names[i]).c_str(), "rb");
    if (!f) continue;
    Table& t = g_tables[i];
    uint8_t buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) t.data.insert(t.data.end(), buf, buf + n);
    std::fclose(f);
    if (t.data.size() < 0x18 || std::memcmp(t.data.data(), "PLT ", 4) != 0)
      recomp::FatalF("disc map %s: not a PLT table", names[i] + 1);
    t.block = Be(&t.data[4], 4);
    t.entries = Be(&t.data[8], 4);
    if (!t.block || 0x18 + 0x18 * t.entries > t.data.size())
      recomp::FatalF("disc map %s: bad header", names[i] + 1);
  }
}

// The entries of path (below /app0/) for bytes offset .. offset + size - 1:
// the first block's location and the length of it and the blocks after it
// that follow on in the table.
bool Find(const Table& t, const std::string& path, int64_t offset, int64_t size, uint64_t* where,
          uint64_t* length) {
  if (t.data.empty() || !t.entries) return false;
  uint8_t key[19];
  Md5(path, key);
  uint64_t first = uint64_t(offset) / t.block;
  const uint64_t last = uint64_t(offset + size - 1) / t.block;
  key[16] = uint8_t(first >> 16);
  key[17] = uint8_t(first >> 8);
  key[18] = uint8_t(first);
  int64_t lo = 0, hi = int64_t(t.entries) - 1;
  while (lo <= hi) {
    const int64_t mid = (lo + hi) / 2;
    const int cmp = std::memcmp(t.entry(uint64_t(mid)), key, 19);
    if (cmp < 0) {
      lo = mid + 1;
    } else if (cmp > 0) {
      hi = mid - 1;
    } else {
      const uint8_t* e = t.entry(uint64_t(mid));
      *where = Be(e + 19, 3);
      *length = Be(e + 22, 2);
      for (uint64_t i = uint64_t(mid) + 1; first < last && i < t.entries; i++) {
        first++;
        key[16] = uint8_t(first >> 16);
        key[17] = uint8_t(first >> 8);
        key[18] = uint8_t(first);
        if (std::memcmp(t.entry(i), key, 19) != 0) break;
        *length += Be(t.entry(i) + 22, 2);
      }
      return true;
    }
  }
  return false;
}

// ioKMruft1ek's lookup: *flags is (the mount: 0 /app0, 1 /app1) << 8, | 1
// if the patch's table has it; all three are -1 where nothing is found.
uint32_t Locate(const std::string& p, int64_t offset, int64_t size, uint64_t* flags,
                uint64_t* where, uint64_t* length) {
  *flags = *where = *length = ~0ull;
  const bool app1 = p.compare(0, 6, "/app1/") == 0;
  if (p.compare(0, 6, "/app0/") != 0 && !app1) return kErrNotFound;
  std::lock_guard<std::mutex> l(g_mu);
  Load();
  if (app1) return kErrNoEntry;  // only /app0 has tables
  for (int i = 0; i < 2; i++)
    if (Find(g_tables[i], p.substr(6), offset, size, where, length)) {
      *flags = i == 0 ? 1 : 0;
      return 0;
    }
  *where = *length = ~0ull;
  return kErrNoEntry;
}

}  // namespace

using recomp::hle::Arg;

// int ioKMruft1ek(const char* path, int64_t offset, int64_t size, int64_t*
// flags, int64_t* where, int64_t* length) (exported by NID only)
ORBIS_HLE_NID("libSceDiscMap", ioKMruft1ek, 6) {
  const recomp::gaddr path = Arg(c, 0), flags = Arg(c, 3), where = Arg(c, 4), length = Arg(c, 5);
  const int64_t offset = int64_t(Arg(c, 1)), size = int64_t(Arg(c, 2));
  if (!path || !flags || !where || !length || offset < 0 || size <= 0) return kErrInvalidArgument;
  uint64_t f, w, n;
  const uint32_t r = Locate(recomp::GStr(path), offset, size, &f, &w, &n);
  recomp::Wr64(flags, f);
  recomp::Wr64(where, w);
  recomp::Wr64(length, n);
  return r;
}

// int sceDiscMapIsRequestOnHDD(const char* path, int64_t offset, int64_t
// size, int* on_hdd)
ORBIS_HLE("libSceDiscMap", sceDiscMapIsRequestOnHDD, 4) {
  const recomp::gaddr path = Arg(c, 0), out = Arg(c, 3);
  if (!path || !out) return kErrInvalidArgument;
  recomp::Wr32(out, 1);
  const int64_t offset = int64_t(Arg(c, 1)), size = int64_t(Arg(c, 2));
  if (offset < 0 || size <= 0) return kErrInvalidArgument;
  uint64_t f, w, n;
  return Locate(recomp::GStr(path), offset, size, &f, &w, &n);
}
