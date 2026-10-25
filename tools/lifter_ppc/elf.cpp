#include "elf.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "text_hash.h"

namespace lifter::ppc {

uint64_t TextHash(const ElfImage& e) {
  std::vector<std::pair<uint64_t, uint64_t>> code = e.code;
  std::sort(code.begin(), code.end());
  std::vector<uint8_t> text;
  for (const auto& [lo, hi] : code)
    text.insert(text.end(), e.bytes.begin() + (lo - e.base), e.bytes.begin() + (hi - e.base));
  return recomp::TextHash(text.data(), text.size());
}

bool IsElf(const std::string& path) {
  char head[4] = {};
  std::ifstream f(path, std::ios::binary);
  return f.read(head, sizeof head) && !std::memcmp(head, "\x7f" "ELF", 4);
}

bool LoadElf(const std::string& path, ElfImage& e, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto u = [&](uint64_t off, int n) {
    uint64_t v = 0;
    for (int k = 0; k < n; k++) v = v << 8 | (off + k < d.size() ? d[off + k] : 0);
    return v;
  };
  // ELFCLASS64, ELFDATA2MSB, EM_PPC64.
  if (d.size() < 64 || std::memcmp(d.data(), "\x7f" "ELF\x02\x02", 6) || u(18, 2) != 21) {
    err = path + ": not a big-endian PowerPC64 ELF";
    return false;
  }
  e.entry = u(24, 8);
  uint64_t shoff = u(40, 8), shentsize = u(58, 2), shnum = u(60, 2);
  auto sh = [&](uint64_t k, int field, int n) { return u(shoff + k * shentsize + field, n); };
  struct Section {
    uint64_t addr, size, off;
    bool nobits;
  };
  std::vector<Section> secs;
  for (uint64_t k = 0; k < shnum; k++) {
    uint64_t type = sh(k, 4, 4), flags = sh(k, 8, 8), addr = sh(k, 16, 8), off = sh(k, 24, 8),
             size = sh(k, 32, 8);
    if (type == 2) {  // SHT_SYMTAB: the sized STT_FUNC symbols
      uint64_t entsize = sh(k, 56, 8);
      for (uint64_t s = off; entsize && s + entsize <= off + size; s += entsize) {
        uint64_t value = u(s + 8, 8), sz = u(s + 16, 8);
        if ((u(s + 4, 1) & 0xF) == 2 && sz && value) e.functions.emplace_back(value, sz);
      }
    }
    if (!(flags & 2) || !size) continue;  // SHF_ALLOC
    secs.push_back({addr, size, off, type == 8});  // SHT_NOBITS
    if (flags & 4) e.code.emplace_back(addr, addr + size);  // SHF_EXECINSTR
  }
  if (secs.empty()) {
    err = path + ": no allocated sections";
    return false;
  }
  std::sort(e.functions.begin(), e.functions.end());
  e.functions.erase(std::unique(e.functions.begin(), e.functions.end()), e.functions.end());
  e.base = secs.front().addr;
  uint64_t end = 0;
  for (const Section& s : secs) {
    e.base = std::min(e.base, s.addr);
    end = std::max(end, s.addr + s.size);
  }
  if (end > 1ull << 32) {
    err = path + ": sections above 4 GiB";
    return false;
  }
  e.bytes.assign(end - e.base, 0);
  for (const Section& s : secs) {
    if (s.nobits) continue;
    if (s.off + s.size > d.size()) {
      err = path + ": a section runs past the end of the file";
      return false;
    }
    std::memcpy(e.bytes.data() + (s.addr - e.base), d.data() + s.off, s.size);
  }
  return true;
}

}  // namespace lifter::ppc
