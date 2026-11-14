// audio_source_test: raw sample decoding (audio/source.h). Checks that an
// IMA ADPCM final block cut short is decoded from the bytes it has, as the
// first frames of the whole block, without reading past it (the data ends
// at a page that faults).

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/audio/source.h"

using namespace recomp;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

}  // namespace

int main() {
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  auto* map = static_cast<uint8_t*>(
      mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (map == MAP_FAILED || mprotect(map + page, page, PROT_NONE)) return 1;

  for (int ch = 1; ch <= 2; ch++) {
    audio::RawFormat f;
    f.channels = ch;
    f.adpcm = true;
    f.block_size = uint32_t(4 * ch + 4 * ch * 2);  // two groups
    std::vector<uint8_t> block(f.block_size);
    for (size_t i = 0; i < block.size(); i++) block[i] = uint8_t(i * 37 + 11);
    for (int c = 0; c < ch; c++) block[size_t(4 * c + 2)] = uint8_t(20 + c);  // step index
    std::vector<float> whole;
    std::string err;
    CHECK(audio::DecodeRaw(block.data(), block.size(), f, &whole, &err));
    CHECK(whole.size() == size_t(17 * ch));

    // Every cut of the second group; the frames are those every channel
    // has bytes for.
    for (size_t cut = 1; cut < size_t(4 * ch); cut++) {
      const size_t len = size_t(4 * ch + 4 * ch) + cut;
      uint8_t* at = map + page - len;
      std::memcpy(at, block.data(), len);
      std::vector<float> part;
      CHECK(audio::DecodeRaw(at, len, f, &part, &err));
      const size_t last = size_t(4 * (ch - 1));
      const size_t frames = 1 + 8 + (cut > last ? (cut - last) * 2 : 0);
      CHECK(part.size() == frames * size_t(ch));
      CHECK(std::memcmp(part.data(), whole.data(), part.size() * sizeof(float)) == 0);
    }
  }

  munmap(map, 2 * page);
  if (g_failures) return 1;
  std::printf("audio sources: ok\n");
  return 0;
}
