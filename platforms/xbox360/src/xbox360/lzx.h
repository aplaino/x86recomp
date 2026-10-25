#pragma once
// LZX decompression (the format of Microsoft's CAB LZX and XMemCompress,
// written from its specification): verbatim, aligned offset and
// uncompressed blocks, 2^15 to 2^21 byte windows, the E8 call
// translation, and the 16-bit realignment of the input after every 32 KiB
// frame of output. XEX images in "normal" compression are one such stream.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recomp::xbox360 {

// Decompresses `in` into exactly `out_size` bytes with a window of
// `window_size` bytes (a power of two). Fails on malformed input.
bool LzxDecompress(const uint8_t* in, size_t in_size, uint32_t window_size, size_t out_size,
                   std::vector<uint8_t>& out, std::string& err);

}  // namespace recomp::xbox360
