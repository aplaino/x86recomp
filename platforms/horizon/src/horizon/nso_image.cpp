#include "horizon/nso_image.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace recomp::horizon {
namespace {

template <typename T>
T Get(const std::vector<uint8_t>& b, uint64_t off) {
  T v{};
  if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
  return v;
}

std::string Hex(uint64_t v) {
  char s[24];
  std::snprintf(s, sizeof s, "0x%llx", static_cast<unsigned long long>(v));
  return s;
}

constexpr uint32_t kPage = 0x1000;
uint64_t PageUp(uint64_t v) { return (v + kPage - 1) & ~uint64_t(kPage - 1); }

}  // namespace

bool LoadNro(const std::string& path, a64::Module& m, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = path + ": cannot open";
    return false;
  }
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
  if (file.size() < 0x80 || std::memcmp(file.data() + 0x10, "NRO0", 4)) {
    err = path + ": not an NRO";
    return false;
  }
  m = a64::Module{};
  m.path = path;
  a64::Segment* segs[3] = {&m.text, &m.ro, &m.data};
  for (int i = 0; i < 3; i++) {
    segs[i]->off = Get<uint32_t>(file, 0x20 + i * 8);
    segs[i]->size = Get<uint32_t>(file, 0x24 + i * 8);
    if (uint64_t(segs[i]->off) + segs[i]->size > file.size() || segs[i]->off % kPage) {
      err = path + ": segment " + std::to_string(i) + " is outside the file or unaligned";
      return false;
    }
  }
  m.bss.off = m.data.off + m.data.size;
  m.bss.size = Get<uint32_t>(file, 0x38);
  m.image.assign(PageUp(uint64_t(m.bss.off) + m.bss.size), 0);
  for (a64::Segment* s : segs) std::memcpy(m.image.data() + s->off, file.data() + s->off, s->size);

  const uint32_t mod0 = Get<uint32_t>(file, 0x04);
  if (mod0 + 0x1C > m.image.size() || std::memcmp(m.image.data() + mod0, "MOD0", 4)) {
    err = path + ": no MOD0 header at " + Hex(mod0);
    return false;
  }
  auto mod0_field = [&](int i) { return mod0 + Get<int32_t>(m.image, mod0 + 4 + i * 4); };
  m.dynamic = mod0_field(0);
  uint32_t hdr_start = mod0_field(3), hdr_end = mod0_field(4);
  if (hdr_end > hdr_start) {
    m.eh_frame_hdr = hdr_start;
    m.eh_frame_hdr_size = hdr_end - hdr_start;
  }
  if (!a64::ReadDynamic(m, err)) {
    err = path + ": " + err;
    return false;
  }
  return true;
}

}  // namespace recomp::horizon
