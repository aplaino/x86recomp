// xbox360_elf2xex: builds a XEX2 from a big-endian PowerPC ELF linked at its
// load address, for the test fixtures (platforms/xbox360/tests).
//   xbox360_elf2xex <in.elf> <out.xex> [--import <module>]...
// The XEX is unencrypted and stored uncompressed: the image holds a PE
// header (one page at the load address), the ELF's allocated sections at
// their addresses, and a .pdata built from its sized function symbols,
// which is where the lifter finds function starts. Imports are the words
// the program labels with symbols named __xex_import*: each is an import
// record as a title's are (type << 24 | library << 16 | ordinal, type 0 a
// variable's slot, 1 a function's thunk), the library being the index of
// a --import module in the order given.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Elf {
  std::vector<uint8_t> d;
  uint64_t U(uint64_t off, int n) const {
    uint64_t v = 0;
    for (int k = 0; k < n; k++) v = v << 8 | d.at(off + k);
    return v;
  }
};

void PutBe(std::vector<uint8_t>& d, size_t off, uint64_t v, int n) {
  if (d.size() < off + n) d.resize(off + n);
  for (int k = 0; k < n; k++) d[off + k] = uint8_t(v >> (8 * (n - 1 - k)));
}
void PutLe(std::vector<uint8_t>& d, size_t off, uint64_t v, int n) {
  if (d.size() < off + n) d.resize(off + n);
  for (int k = 0; k < n; k++) d[off + k] = uint8_t(v >> (8 * k));
}

uint32_t Be32At(const std::vector<uint8_t>& d, size_t off) {
  return uint32_t(d.at(off)) << 24 | uint32_t(d.at(off + 1)) << 16 | uint32_t(d.at(off + 2)) << 8 | d.at(off + 3);
}

struct Section {
  std::string name;
  uint64_t addr, size, off;
  bool exec, nobits;
};

constexpr uint32_t kPage = 0x1000;

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> libraries;
  for (int k = 3; k + 1 < argc && !std::strcmp(argv[k], "--import"); k += 2) libraries.push_back(argv[k + 1]);
  if (argc < 3 || argc != 3 + 2 * int(libraries.size())) {
    std::fprintf(stderr, "usage: xbox360_elf2xex in.elf out.xex [--import module]...\n");
    return 2;
  }
  Elf e;
  {
    std::ifstream f(argv[1], std::ios::binary);
    e.d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  if (e.d.size() < 64 || std::memcmp(e.d.data(), "\x7f" "ELF\x02\x02", 6) || e.U(18, 2) != 21) {
    std::fprintf(stderr, "xbox360_elf2xex: %s is not a big-endian PowerPC64 ELF\n", argv[1]);
    return 1;
  }
  uint64_t entry = e.U(24, 8), shoff = e.U(40, 8);
  uint32_t shentsize = uint32_t(e.U(58, 2)), shnum = uint32_t(e.U(60, 2)), shstrndx = uint32_t(e.U(62, 2));
  auto sh = [&](uint32_t k, int field, int n) { return e.U(shoff + uint64_t(k) * shentsize + field, n); };
  uint64_t strtab = sh(shstrndx, 24, 8);
  std::vector<Section> secs;
  std::vector<std::pair<uint32_t, uint32_t>> funcs;  // start, size
  std::vector<uint32_t> records;                      // import records' addresses
  for (uint32_t k = 0; k < shnum; k++) {
    uint32_t type = uint32_t(sh(k, 4, 4));
    uint64_t flags = sh(k, 8, 8), addr = sh(k, 16, 8), off = sh(k, 24, 8), size = sh(k, 32, 8);
    if (type == 2) {  // SHT_SYMTAB: sized STT_FUNC symbols, import records
      uint64_t entsize = sh(k, 56, 8), names = sh(uint32_t(sh(k, 40, 4)), 24, 8);
      for (uint64_t s = off; s + entsize <= off + size; s += entsize) {
        uint8_t info = e.d[s + 4];
        uint64_t value = e.U(s + 8, 8), sz = e.U(s + 16, 8);
        if ((info & 0xF) == 2 && sz && value) funcs.emplace_back(uint32_t(value), uint32_t(sz));
        const char* name = reinterpret_cast<const char*>(&e.d[names + e.U(s, 4)]);
        if (!std::strncmp(name, "__xex_import", 12)) records.push_back(uint32_t(value));
      }
    }
    if (!(flags & 2) || !size) continue;  // SHF_ALLOC
    std::string name(reinterpret_cast<const char*>(&e.d[strtab + sh(k, 0, 4)]));
    secs.push_back({name, addr, size, off, (flags & 4) != 0, type == 8});
  }
  if (secs.empty()) {
    std::fprintf(stderr, "xbox360_elf2xex: no allocated sections\n");
    return 1;
  }
  std::sort(secs.begin(), secs.end(), [](const Section& a, const Section& b) { return a.addr < b.addr; });
  std::sort(funcs.begin(), funcs.end());
  funcs.erase(std::unique(funcs.begin(), funcs.end()), funcs.end());
  uint64_t base = secs.front().addr - kPage;
  if (base & 0xFFFF || base >> 32) {
    std::fprintf(stderr, "xbox360_elf2xex: link the first section one page after a 64 KiB aligned base below 4 GiB\n");
    return 1;
  }

  // The image: PE header page, the sections, then .pdata.
  std::vector<uint8_t> img;
  for (const Section& s : secs) {
    size_t at = s.addr - base;
    img.resize(std::max(img.size(), size_t(at + s.size)));
    if (!s.nobits) std::memcpy(&img[at], &e.d[s.off], s.size);
  }
  img.resize((img.size() + kPage - 1) & ~size_t(kPage - 1));
  uint32_t pdata_rva = uint32_t(img.size());
  for (size_t k = 0; k < funcs.size(); k++) {
    PutBe(img, pdata_rva + 8 * k, funcs[k].first, 4);
    PutBe(img, pdata_rva + 8 * k + 4, uint64_t(funcs[k].second / 4) << 8, 4);
  }
  uint32_t pdata_size = uint32_t(8 * funcs.size());
  img.resize((img.size() + kPage - 1) & ~size_t(kPage - 1));

  // PE header: DOS header, NT signature, file header (PowerPC big-endian),
  // PE32 optional header with the exception directory, section table.
  img[0] = 'M';
  img[1] = 'Z';
  const uint32_t nt = 0x40, opt = nt + 24, opt_size = 0xE0, sect = opt + opt_size;
  PutLe(img, 0x3C, nt, 4);
  PutLe(img, nt, 0x00004550, 4);
  PutLe(img, nt + 4, 0x1F2, 2);
  PutLe(img, nt + 6, secs.size() + 1, 2);
  PutLe(img, nt + 20, opt_size, 2);
  PutLe(img, opt, 0x10B, 2);
  PutLe(img, opt + 16, uint32_t(entry - base), 4);
  PutLe(img, opt + 28, base, 4);
  PutLe(img, opt + 56, img.size(), 4);
  PutLe(img, opt + 92, 16, 4);
  PutLe(img, opt + 96 + 8 * 3, pdata_rva, 4);
  PutLe(img, opt + 100 + 8 * 3, pdata_size, 4);
  auto section = [&](size_t k, const std::string& name, uint32_t rva, uint32_t size, uint32_t chars) {
    size_t s = sect + 40 * k;
    std::memcpy(&img[s], name.c_str(), std::min<size_t>(8, name.size()));
    PutLe(img, s + 8, size, 4);
    PutLe(img, s + 12, rva, 4);
    PutLe(img, s + 36, chars, 4);
  };
  for (size_t k = 0; k < secs.size(); k++)
    section(k, secs[k].name, uint32_t(secs[k].addr - base), uint32_t(secs[k].size),
            secs[k].exec ? 0x60000020u : secs[k].nobits ? 0xC0000080u : 0xC0000040u);
  section(secs.size(), ".pdata", pdata_rva, pdata_size, 0x40000040u);
  if (sect + 40 * (secs.size() + 1) > kPage) {
    std::fprintf(stderr, "xbox360_elf2xex: too many sections\n");
    return 1;
  }

  // XEX2: header, the optional headers (file format, entry point, image
  // base, import libraries), the file format info (unencrypted, stored),
  // the security info, the import libraries.
  std::vector<uint8_t> x;
  const uint32_t ffi = 0x40, security = 0x100, imports = 0x400, header_size = 0x1000;
  x.resize(header_size);
  std::memcpy(&x[0], "XEX2", 4);
  PutBe(x, 4, 1, 4);  // a title
  PutBe(x, 8, header_size, 4);
  PutBe(x, 16, security, 4);
  PutBe(x, 20, libraries.empty() ? 3 : 4, 4);
  PutBe(x, 24, 0x000003FF, 4);
  PutBe(x, 28, ffi, 4);
  PutBe(x, 32, 0x00010100, 4);
  PutBe(x, 36, entry, 4);
  PutBe(x, 40, 0x00010201, 4);
  PutBe(x, 44, base, 4);
  PutBe(x, 48, 0x000103FF, 4);
  PutBe(x, 52, imports, 4);
  PutBe(x, ffi, 8, 4);  // info size; encryption 0, compression 0 follow
  // The import libraries: the size, the names' size and count, the names
  // (NUL-terminated, padded to words), then per library its size, digest,
  // id and versions (zero), name index, record count and records.
  if (!libraries.empty()) {
    uint32_t at = imports + 12;
    for (const std::string& n : libraries) {
      std::memcpy(&x[at], n.c_str(), n.size() + 1);
      at += uint32_t(n.size()) + 1;
    }
    at = (at + 3) & ~3u;
    PutBe(x, imports + 4, at - imports - 12, 4);
    PutBe(x, imports + 8, libraries.size(), 4);
    for (size_t lib = 0; lib < libraries.size(); lib++) {
      std::vector<uint32_t> mine;
      for (uint32_t va : records)
        if (((Be32At(img, va - base)) >> 16 & 0xFF) == lib) mine.push_back(va);
      uint32_t size = 0x28 + 4 * uint32_t(mine.size());
      PutBe(x, at, size, 4);
      PutBe(x, at + 0x24, lib, 2);
      PutBe(x, at + 0x26, mine.size(), 2);
      for (size_t r = 0; r < mine.size(); r++) PutBe(x, at + 0x28 + 4 * r, mine[r], 4);
      at += size;
    }
    PutBe(x, imports, at - imports, 4);
    if (at > header_size) {
      std::fprintf(stderr, "xbox360_elf2xex: too many imports\n");
      return 1;
    }
  }
  PutBe(x, security, 0x184, 4);
  PutBe(x, security + 4, img.size(), 4);
  PutBe(x, security + 0x110, base, 4);
  x.insert(x.end(), img.begin(), img.end());
  std::ofstream f(argv[2], std::ios::binary);
  f.write(reinterpret_cast<const char*>(x.data()), std::streamsize(x.size()));
  if (!f) {
    std::fprintf(stderr, "xbox360_elf2xex: cannot write %s\n", argv[2]);
    return 1;
  }
  return 0;
}
