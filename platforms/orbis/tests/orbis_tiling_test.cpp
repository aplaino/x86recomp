// orbis/gpu/tiling: every read tile mode maps a padded surface's elements
// onto its bytes one to one, and a few elements land where the Sea Islands
// pipe and bank equations (as the firmware's addressing has them) put them.

#include <cstdio>
#include <cstring>
#include <vector>

#include "orbis/gpu/tiling.h"

namespace {

using namespace recomp::orbis::gpu;

int failures = 0;

// Detiles a surface whose every byte holds its own offset (in elements,
// spread over bpe bytes) and returns the offsets, element by element.
std::vector<uint64_t> Offsets(uint32_t mode, uint32_t w, uint32_t h, uint32_t pitch,
                              uint32_t bpe) {
  const uint64_t bytes = TiledBytes(mode, pitch, h, bpe);
  std::vector<uint8_t> src(bytes), dst(uint64_t(w) * h * bpe);
  for (uint64_t i = 0; i < bytes / bpe; i++) std::memcpy(&src[i * bpe], &i, bpe < 8 ? bpe : 8);
  if (!Detile(mode, src.data(), dst.data(), w, h, pitch, bpe, w * bpe)) return {};
  std::vector<uint64_t> out(uint64_t(w) * h);
  for (uint64_t i = 0; i < out.size(); i++) {
    out[i] = 0;
    std::memcpy(&out[i], &dst[i * bpe], bpe < 8 ? bpe : 8);
  }
  return out;
}

void ExpectBijective(uint32_t mode, uint32_t pitch, uint32_t h, uint32_t bpe) {
  const uint64_t bytes = TiledBytes(mode, pitch, h, bpe);
  if (bytes != uint64_t(pitch) * h * bpe) {
    std::printf("FAIL: mode %u %ux%u bpe %u: %llu bytes, want %llu (padded already)\n", mode,
                pitch, h, bpe, (unsigned long long)bytes,
                (unsigned long long)(uint64_t(pitch) * h * bpe));
    failures++;
    return;
  }
  const std::vector<uint64_t> off = Offsets(mode, pitch, h, pitch, bpe);
  std::vector<bool> seen(off.size());
  for (uint64_t o : off) {
    if (o >= seen.size() || seen[o]) {
      std::printf("FAIL: mode %u bpe %u: element offset %llu repeats or is out of range\n", mode,
                  bpe, (unsigned long long)o);
      failures++;
      return;
    }
    seen[o] = true;
  }
}

void ExpectAt(uint32_t mode, uint32_t pitch, uint32_t h, uint32_t bpe, uint32_t x, uint32_t y,
              uint64_t byte) {
  const std::vector<uint64_t> off = Offsets(mode, pitch, h, pitch, bpe);
  const uint64_t got = off.empty() ? ~0ull : off[uint64_t(y) * pitch + x] * bpe;
  if (got != byte) {
    std::printf("FAIL: mode %u bpe %u (%u,%u) at byte %llu, want %llu\n", mode, bpe, x, y,
                (unsigned long long)got, (unsigned long long)byte);
    failures++;
  }
}

}  // namespace

int main() {
  // Linear (8), 1D thin (13), 2D depth (0, 2) and 2D thin (14), over a
  // padded surface two macro tiles each way for every element size.
  for (uint32_t bpe : {2u, 4u, 8u, 16u})
    for (uint32_t mode : {8u, 13u, 0u, 2u, 14u}) ExpectBijective(mode, 256, 256, bpe);
  // Displayable 1D (9) and 2D (10), up to 8 bytes an element.
  for (uint32_t bpe : {2u, 4u, 8u})
    for (uint32_t mode : {9u, 10u}) ExpectBijective(mode, 256, 256, bpe);
  // 2D thin, 4 bytes an element: 16 banks of one micro tile, macro tiles of
  // 128x64 elements, 256-byte pipe interleave.
  ExpectAt(14, 256, 128, 4, 1, 0, 4);                            // inside the micro tile
  ExpectAt(14, 256, 128, 4, 8, 0, 1 << 8);                       // x3: pipe 1
  ExpectAt(14, 256, 128, 4, 16, 0, 3 << 8);                      // x4: pipe 3
  ExpectAt(14, 256, 128, 4, 0, 8, (1 << 8) | (8 << 11));         // y3: pipe 1, bank 8
  ExpectAt(14, 256, 128, 4, 128, 0, (2 << 11) | (1 << 15));     // next macro tile: bank 2
  // Depth with a 64-byte tile split, 4 bytes an element: micro tiles cut
  // in 4 slices after one another (macro tiles of 256x128 elements, two
  // rows of them a slice), each further slice's bank rotated by 9.
  ExpectAt(0, 256, 256, 4, 2, 0, 16);
  ExpectAt(0, 256, 256, 4, 4, 0, (9 << 11) | (2 << 15));
  // 1D thin: micro tiles row by row.
  ExpectAt(13, 64, 16, 4, 9, 1, (1 * 64 + 3) * 4);
  ExpectAt(13, 64, 16, 4, 0, 8, 8 * 64 * 4);
  // 1D displayable, 4 bytes an element: x0 x1 y0 inside the micro tile.
  ExpectAt(9, 64, 16, 4, 1, 1, 5 * 4);
  // Layouts not read yet: rotated (28), PRT (6), thick (19), displayable
  // with 16 bytes an element.
  for (uint32_t mode : {28u, 6u, 19u})
    if (TiledBytes(mode, 256, 256, 4)) {
      std::printf("FAIL: mode %u reads\n", mode);
      failures++;
    }
  if (TiledBytes(10, 256, 256, 16)) {
    std::printf("FAIL: mode 10 reads 16-byte elements\n");
    failures++;
  }
  if (failures) return 1;
  std::printf("orbis_tiling: ok\n");
  return 0;
}
