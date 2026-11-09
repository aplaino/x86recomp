#pragma once
// The Xenos's EDRAM and its resolves (docs/xenos.md, "EDRAM and
// resolves"): the 10 MiB the render back end draws into, kept on the host
// GPU as a buffer in the console's layout, so a target that one format
// wrote reads back as another the way it does there. A resolve copies a
// rectangle of a colour or depth target to a tiled texture in memory and
// clears the target, as compute work on the device.

#include <cstdint>

namespace recomp::xbox360 {

// 2048 tiles of 80x16 32-bit samples.
constexpr uint32_t kEdramTiles = 2048, kEdramTileWords = 80 * 16,
                   kEdramBytes = kEdramTiles * kEdramTileWords * 4;

// At power-on, on the process's main thread: the host GPU device and the
// EDRAM on it.
void StartEdram();

// A draw packet with RB_MODECONTROL in copy mode: the resolve the copy
// registers describe, from the command processor. Its results are in
// memory when it returns.
void Resolve();

// Byte offset of block (x, y) in a 2D tiled surface `pitch` blocks wide
// (a multiple of 32), blocks of 1 << bpb_log2 bytes; negative coordinates
// give the offsets before the surface that resolves use.
int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpb_log2);

}  // namespace recomp::xbox360
