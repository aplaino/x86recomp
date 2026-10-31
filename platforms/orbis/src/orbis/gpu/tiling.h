#pragma once
// Surface layouts: a T#'s tile mode index (into the GPU's tile mode table)
// read as linear rows. Level 0, one sample, slice 0, in elements (pixels,
// or 4x4 blocks of a compressed format) of bpe bytes.

#include <cstdint>

namespace recomp::orbis::gpu {

// The bytes a surface of pitch elements a row and h rows spans in tile
// mode `index`: its tiles padded out. 0 for a layout not read yet.
uint64_t TiledBytes(uint32_t index, uint32_t pitch, uint32_t h, uint32_t bpe);

// Copies its w x h elements to dst, rows of dst_row bytes. src spans
// TiledBytes; false (and nothing copied) for a layout not read yet.
bool Detile(uint32_t index, const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h,
            uint32_t pitch, uint32_t bpe, uint32_t dst_row);

}  // namespace recomp::orbis::gpu
