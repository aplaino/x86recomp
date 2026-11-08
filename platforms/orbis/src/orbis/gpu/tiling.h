#pragma once
// Surface layouts: a T#'s tile mode index (into the GPU's tile mode table)
// read as linear rows, in elements (pixels, or 4x4 blocks of a compressed
// format) of bpe bytes, one sample.

#include <cstdint>

namespace recomp::orbis::gpu {

// The bytes a surface of pitch elements a row and h rows spans in tile
// mode `index`: its tiles padded out. 0 for a layout not read yet.
uint64_t TiledBytes(uint32_t index, uint32_t pitch, uint32_t h, uint32_t bpe);

// Copies its w x h elements to dst, rows of dst_row bytes. src spans
// TiledBytes; false (and nothing copied) for a layout not read yet.
bool Detile(uint32_t index, const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h,
            uint32_t pitch, uint32_t bpe, uint32_t dst_row);

// A texture as its T# describes it, for its levels' layout.
struct TextureLayout {
  uint32_t index = 0;  // TILING_INDEX
  uint32_t bpe = 4;    // bytes per element
  bool blocks = false;  // elements are 4x4 blocks
  bool pow2 = false;    // POW2_PAD
  bool depth_format = false;  // a 16 or 32-bit data format (not 3D): pads as depth
  uint32_t w = 1, h = 1, pitch = 1;  // level 0, in texels (PITCH)
  uint32_t depth = 1;   // a 3D texture's (else 1)
  bool volume = false;  // a 3D texture (thick modes tile its depth)
  uint32_t slices = 1;  // array slices, 6 per cube (1 for 3D)
};

// One level: where its first slice starts (bytes from the texture's base),
// a slice's bytes (a 3D level's slices are its depth), its tile mode (a 2D
// mode drops to the 1D one of its micro tiles for a level smaller than a
// macro tile), its padded pitch and height and its size, in elements.
struct Level {
  uint64_t offset = 0, slice_bytes = 0;
  uint32_t index = 0, pitch = 0, height = 0;
  uint32_t w = 0, h = 0, depth = 1;
};

// Level `level` of t; false for a layout not read yet.
bool LevelOf(const TextureLayout& t, uint32_t level, Level* out);

// Copies slice `slice` (a 3D level's z) of a level to dst, rows of
// dst_row bytes. src is the level's first slice.
bool DetileSlice(const Level& l, uint32_t bpe, uint32_t slice, const uint8_t* src, uint8_t* dst,
                 uint32_t dst_row);

}  // namespace recomp::orbis::gpu
