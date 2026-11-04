// orbis/gpu/tiling: every read tile mode maps a padded surface's elements
// onto its bytes one to one, and a few elements land where the Sea Islands
// pipe and bank equations (as the firmware's addressing has them) put them;
// texture levels and slices lie where the firmware's (11.00) GL library
// puts them.

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

// Level `level` of t starts at `offset` and its slices are `bytes` long.
void ExpectLevel(const TextureLayout& t, uint32_t level, uint64_t offset, uint64_t bytes,
                 const char* what) {
  Level l;
  if (!LevelOf(t, level, &l) || l.offset != offset || l.slice_bytes * l.depth != bytes) {
    std::printf("FAIL: %s level %u at %llu, %llu bytes; want %llu, %llu\n", what, level,
                (unsigned long long)l.offset, (unsigned long long)(l.slice_bytes * l.depth),
                (unsigned long long)offset, (unsigned long long)bytes);
    failures++;
  }
}

// Element (x, y) of slice `slice` of t's level 0 (4-byte elements) is at
// byte `byte` from the level's start.
void ExpectSliceAt(const TextureLayout& t, uint32_t slice, uint32_t x, uint32_t y, uint64_t byte) {
  Level l;
  LevelOf(t, 0, &l);
  std::vector<uint32_t> src(l.slice_bytes * t.slices / 4), dst(uint64_t(l.w) * l.h);
  for (uint32_t i = 0; i < src.size(); i++) src[i] = i;
  DetileSlice(l, 4, slice, reinterpret_cast<const uint8_t*>(src.data()),
              reinterpret_cast<uint8_t*>(dst.data()), l.w * 4);
  const uint64_t got = uint64_t(dst[uint64_t(y) * l.w + x]) * 4;
  if (got != byte) {
    std::printf("FAIL: slice %u (%u,%u) at byte %llu, want %llu\n", slice, x, y,
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
  // Levels one after another, each padded on its own: 256x256, 4 bytes,
  // 2D displayable (10), its macro tiles 128 wide; from 64x64 down 1D
  // displayable (9), from 8x8 down micro tiles of 256 bytes.
  TextureLayout t;
  t.index = 10;
  t.w = t.h = t.pitch = 256;
  const uint64_t chain[9][2] = {{0, 262144},      {262144, 65536}, {327680, 16384},
                                {344064, 4096},   {348160, 1024},  {349184, 256},
                                {349440, 256},    {349696, 256},   {349952, 256}};
  for (uint32_t i = 0; i < 9; i++) ExpectLevel(t, i, chain[i][0], chain[i][1], "2D chain");
  Level l, l2;
  LevelOf(t, 1, &l);
  LevelOf(t, 2, &l2);
  if (l.index != 10 || l2.index != 9) {
    std::printf("FAIL: levels 1 and 2 in modes %u and %u, want 10 and 9\n", l.index, l2.index);
    failures++;
  }
  // 1D thin (13), 100x60: rows padded to micro tiles.
  t = {};
  t.index = 13;
  t.w = t.pitch = 100;
  t.h = 60;
  ExpectLevel(t, 0, 0, 26624, "1D");
  ExpectLevel(t, 1, 26624, 7168, "1D");
  ExpectLevel(t, 3, 35840, 512, "1D");
  // Past level 0 a level is as wide as the level-0 pitch halved, not the
  // width: 100 texels in a pitch of 160.
  t.pitch = 160;
  ExpectLevel(t, 1, 40960, 10240, "1D, pitch 160");
  ExpectLevel(t, 3, 53760, 768, "1D, pitch 160");
  // POW2_PAD: 20x20 levels as 32x32, three slices as four.
  t = {};
  t.index = 13;
  t.w = t.h = t.pitch = 20;
  t.slices = 3;
  t.pow2 = true;
  ExpectLevel(t, 0, 0, 4096, "pow2");
  ExpectLevel(t, 1, 16384, 1024, "pow2");
  ExpectLevel(t, 2, 20480, 256, "pow2");
  // A cube's six faces after one another in each level.
  t = {};
  t.index = 10;
  t.w = t.h = t.pitch = 64;
  t.slices = 6;
  ExpectLevel(t, 1, 196608, 4096, "cube");
  ExpectLevel(t, 2, 221184, 1024, "cube");
  // A 32-bit format in depth micro tiles pads its 1D levels to 256
  // elements; an 8-bit one to 256 bytes.
  t = {};
  t.index = 5;
  t.w = t.h = t.pitch = 8;
  t.depth_format = true;
  ExpectLevel(t, 0, 0, 1024, "depth format");
  t.depth_format = false;
  t.bpe = 1;
  ExpectLevel(t, 0, 0, 256, "8-bit");
  // An array's slices, 2D thin (14), 128x128, 4 bytes: each a slice's
  // bytes after the one before, its banks rotated.
  t = {};
  t.index = 14;
  t.w = t.h = t.pitch = 128;
  t.slices = 4;
  ExpectSliceAt(t, 0, 9, 9, 16396);
  ExpectSliceAt(t, 1, 0, 0, 79872);
  ExpectSliceAt(t, 1, 8, 0, 80128);
  ExpectSliceAt(t, 2, 0, 0, 159744);
  ExpectSliceAt(t, 3, 9, 9, 223244);
  if (failures) return 1;
  std::printf("orbis_tiling: ok\n");
  return 0;
}
