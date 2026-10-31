// Addressing as AMD's addressing library describes it for Sea Islands
// (single sample, array slice 0, no pipe or bank swizzle), checked against the
// firmware's (11.00) own tiled-surface addressing in its GL library: the
// same pipe and bank equations, tile split and macro tile mode choice, and
// its fixed 8 pipes, 256-byte pipe interleave and 1 KiB rows. The tile mode
// table is the console's: GB_TILE_MODE0-31 and GB_MACROTILE_MODE0-15 read
// on a console (firmware 11.00) through the kernel's GPU register reads.

#include "orbis/gpu/tiling.h"

#include <cstring>

namespace recomp::orbis::gpu {
namespace {

constexpr uint32_t kTileModes[32] = {
    0x00800310, 0x00800B10, 0x00801310, 0x00801B10, 0x00802310, 0x00800308, 0x00801318,
    0x00802318, 0x00000304, 0x00000308, 0x02000310, 0x02000294, 0x02000318, 0x00400308,
    0x02400310, 0x024002B0, 0x02400294, 0x02400318, 0x0240032C, 0x0100030C, 0x0100031C,
    0x010002B4, 0x010002A4, 0x01000328, 0x010002BC, 0x01000320, 0x010002B8, 0x00C00308,
    0x02C00310, 0x02C00294, 0x02C00318, 0x00000000};
constexpr uint32_t kMacroTileModes[16] = {0xE8, 0xD4, 0xD0, 0xD0, 0x80, 0x40, 0x00, 0x00,
                                          0xEC, 0xE8, 0xD4, 0xD0, 0x80, 0x40, 0x00, 0x00};

constexpr uint32_t kPipeInterleaveBits = 8;  // 256 bytes

// GB_TILE_MODE's ARRAY_MODE and MICRO_TILE_MODE_NEW values read here.
enum : uint32_t { kLinearGeneral = 0, kLinearAligned = 1, k1dThin = 2, k2dThin = 4 };
enum : uint32_t { kMicroDisplay = 0, kMicroThin = 1, kMicroDepth = 2 };

uint32_t Log2(uint32_t v) {
  uint32_t n = 0;
  while (v > 1) v >>= 1, n++;
  return n;
}
uint32_t Bit(uint32_t v, uint32_t i) { return (v >> i) & 1; }

// A 2D-thin mode's macro tiles: pipes and banks, the
// micro tiles a bank holds (bank_w x bank_h) and the macro tile's size in
// elements; and the tile split, the bytes of a micro tile kept together
// (the rest of it goes to further slices).
struct Macro {
  uint32_t pipes, banks, bank_w, bank_h, pitch, height, split;
};

// A tile mode's layout: its array mode and, for 2D, its macro tiles. The
// tile split is 64 << TILE_SPLIT bytes for depth; a colour one is never
// below one sample's micro tile. The macro tile mode follows from the
// micro tile's bytes as far as the split and a row (1 KiB) let them go,
// counted in 64-byte steps.
struct Mode {
  uint32_t array, micro;
  Macro m;
};
bool Lookup(uint32_t index, uint32_t bpe, Mode* out) {
  if (index >= 32) return false;
  const uint32_t r = kTileModes[index];
  Mode md = {};
  md.array = (r >> 2) & 0xF;
  md.micro = (r >> 22) & 7;
  if (md.array == kLinearGeneral || md.array == kLinearAligned) {
    *out = md;
    return true;
  }
  if (md.micro == kMicroDisplay ? bpe > 8 : md.micro != kMicroThin && md.micro != kMicroDepth)
    return false;
  if (md.array == k1dThin) {
    *out = md;
    return true;
  }
  if (md.array != k2dThin) return false;
  const uint32_t tile_bytes = 64 * bpe;
  Macro& m = md.m;
  m.split = md.micro == kMicroDepth ? 64u << ((r >> 11) & 7) : tile_bytes;
  uint32_t bytes = m.split < tile_bytes ? m.split : tile_bytes;
  if (bytes > 1024) bytes = 1024;
  const uint32_t mm = kMacroTileModes[Log2(bytes / 64)];
  // P8_32x32_16x16 is the one this table uses for 2D-thin modes.
  if (((r >> 6) & 0x1F) != 12) return false;
  m.pipes = 8;
  m.bank_w = 1u << (mm & 3);
  m.bank_h = 1u << ((mm >> 2) & 3);
  const uint32_t aspect = 1u << ((mm >> 4) & 3);
  m.banks = 2u << ((mm >> 6) & 3);
  m.pitch = 8 * m.bank_w * m.pipes * aspect;
  m.height = 8 * m.bank_h * m.banks / aspect;
  *out = md;
  return true;
}

// An element's index inside its 8x8 micro tile, from its low bit up: thin
// (and depth) order x0 y0 x1 y1 x2 y2; displayable order by element size,
// 1 byte x0 x1 x2 y1 y0 y2, 2 bytes x0 x1 x2 y0 y1 y2, 4 bytes x0 x1 y0 x2
// y1 y2, 8 bytes x0 y0 x1 x2 y1 y2.
uint32_t MicroIndex(uint32_t micro, uint32_t bpe, uint32_t x, uint32_t y) {
  const uint32_t x0 = Bit(x, 0), x1 = Bit(x, 1), x2 = Bit(x, 2);
  const uint32_t y0 = Bit(y, 0), y1 = Bit(y, 1), y2 = Bit(y, 2);
  if (micro != kMicroDisplay)
    return x0 | y0 << 1 | x1 << 2 | y1 << 3 | x2 << 4 | y2 << 5;
  switch (bpe) {
    case 1:
      return x0 | x1 << 1 | x2 << 2 | y1 << 3 | y0 << 4 | y2 << 5;
    case 2:
      return x0 | x1 << 1 | x2 << 2 | y0 << 3 | y1 << 4 | y2 << 5;
    case 4:
      return x0 | x1 << 1 | y0 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    default:
      return x0 | y0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
  }
}

uint32_t PaddedPitch(const Mode& md, uint32_t pitch) {
  const uint32_t a = md.array == k2dThin ? md.m.pitch : md.array == k1dThin ? 8 : 1;
  return (pitch + a - 1) / a * a;
}
uint32_t PaddedHeight(const Mode& md, uint32_t h) {
  const uint32_t a = md.array == k2dThin ? md.m.height : md.array == k1dThin ? 8 : 1;
  return (h + a - 1) / a * a;
}

// The pipe and bank of element (x, y), P8_32x32_16x16 and 2 to 16 banks;
// a tile split's further slices rotate the bank.
uint32_t Pipe(uint32_t x, uint32_t y) {
  return (Bit(x, 3) ^ Bit(x, 4) ^ Bit(y, 3)) | (Bit(x, 4) ^ Bit(y, 4)) << 1 |
         (Bit(x, 5) ^ Bit(y, 5)) << 2;
}
uint32_t Bank(const Macro& m, uint32_t x, uint32_t y, uint32_t slice) {
  const uint32_t tx = x / 8 / (m.bank_w * m.pipes), ty = y / 8 / m.bank_h;
  const uint32_t x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2), x6 = Bit(tx, 3);
  const uint32_t y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2), y6 = Bit(ty, 3);
  uint32_t bank;
  switch (m.banks) {
    case 16:
      bank = (x3 ^ y6) | (x4 ^ y5 ^ y6) << 1 | (x5 ^ y4) << 2 | (x6 ^ y3) << 3;
      break;
    case 8:
      bank = (x3 ^ y5) | (x4 ^ y4 ^ y5) << 1 | (x5 ^ y3) << 2;
      break;
    case 4:
      bank = (x3 ^ y4) | (x4 ^ y3) << 1;
      break;
    default:
      bank = x3 ^ y3;
  }
  return (bank ^ (m.banks / 2 + 1) * slice) & (m.banks - 1);
}

// The byte offset of element (x, y) in a 2D-thin surface of `height`
// (padded) rows. A micro tile larger than the tile split is cut into
// slices of split bytes, each slice a whole surface's worth of macro tile
// rows after the one before. Within a slice, the macro tile's share of each
// pipe and bank, the micro tile inside that share and the element inside
// the micro tile make an offset whose low bits (the pipe interleave) stay
// low and the rest go above the pipe and bank bits.
uint64_t Offset2d(const Mode& md, uint32_t x, uint32_t y, uint32_t pitch, uint32_t height,
                  uint32_t bpe) {
  const Macro& m = md.m;
  const uint32_t pipe_bits = Log2(m.pipes), bank_bits = Log2(m.banks);
  const uint32_t tile_bytes = 64 * bpe < m.split ? 64 * bpe : m.split;
  uint32_t elem = MicroIndex(md.micro, bpe, x, y) * bpe;
  const uint32_t slice = elem / tile_bytes;
  elem %= tile_bytes;
  const uint64_t share =
      uint64_t(m.pitch / 8) * (m.height / 8) * tile_bytes >> (pipe_bits + bank_bits);
  const uint64_t macro =
      (uint64_t(slice) * (height / m.height) + y / m.height) * (pitch / m.pitch) + x / m.pitch;
  const uint32_t tile = ((y / 8) % m.bank_h) * m.bank_w + ((x / 8) / m.pipes) % m.bank_w;
  const uint64_t total = macro * share + uint64_t(tile) * tile_bytes + elem;
  const uint64_t low = total & ((1u << kPipeInterleaveBits) - 1);
  return low | uint64_t(Pipe(x, y)) << kPipeInterleaveBits |
         uint64_t(Bank(m, x, y, slice)) << (kPipeInterleaveBits + pipe_bits) |
         (total >> kPipeInterleaveBits) << (kPipeInterleaveBits + pipe_bits + bank_bits);
}

}  // namespace

uint64_t TiledBytes(uint32_t index, uint32_t pitch, uint32_t h, uint32_t bpe) {
  Mode md;
  if (!Lookup(index, bpe, &md)) return 0;
  return uint64_t(PaddedPitch(md, pitch)) * PaddedHeight(md, h) * bpe;
}

bool Detile(uint32_t index, const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h,
            uint32_t pitch, uint32_t bpe, uint32_t dst_row) {
  Mode md;
  if (!Lookup(index, bpe, &md)) return false;
  pitch = PaddedPitch(md, pitch);
  const uint32_t height = PaddedHeight(md, h);
  for (uint32_t y = 0; y < h; y++) {
    uint8_t* row = dst + uint64_t(y) * dst_row;
    if (md.array == kLinearGeneral || md.array == kLinearAligned) {
      std::memcpy(row, src + uint64_t(y) * pitch * bpe, uint64_t(w) * bpe);
      continue;
    }
    for (uint32_t x = 0; x < w; x++) {
      const uint64_t at = md.array == k1dThin
                              ? uint64_t(64 * bpe) * ((y / 8) * (pitch / 8) + x / 8) +
                                    MicroIndex(md.micro, bpe, x, y) * bpe
                              : Offset2d(md, x, y, pitch, height, bpe);
      std::memcpy(row + uint64_t(x) * bpe, src + at, bpe);
    }
  }
  return true;
}

}  // namespace recomp::orbis::gpu
