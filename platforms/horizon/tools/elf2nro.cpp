// horizon_elf2nro: converts a static-PIE AArch64 ELF linked at 0 into an NRO.
//   horizon_elf2nro in.elf out.nro
// The ELF has three PT_LOADs in order: text (R X), read-only (R), data
// (RW, .bss at its end). Its first 0x80 bytes are crt0's NRO header
// reservation (branch, MOD0 offset, then zeros), which this fills in.
// Build-time only: the test fixtures' build uses it.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

template <typename T>
T Get(const std::vector<uint8_t>& b, uint64_t off) {
  T v{};
  if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
  return v;
}
template <typename T>
void Put(std::vector<uint8_t>& b, uint64_t off, T v) {
  std::memcpy(b.data() + off, &v, sizeof(T));
}

constexpr uint64_t kPage = 0x1000;
uint64_t PageUp(uint64_t v) { return (v + kPage - 1) & ~(kPage - 1); }

int Fail(const char* what) {
  std::fprintf(stderr, "horizon_elf2nro: %s\n", what);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return Fail("usage: horizon_elf2nro in.elf out.nro");
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(in)), {});
  if (elf.size() < 64 || std::memcmp(elf.data(), "\x7F" "ELF\x02\x01", 6) ||
      Get<uint16_t>(elf, 18) != 183 /* EM_AARCH64 */)
    return Fail("not a little-endian ELF64 for AArch64");
  uint64_t phoff = Get<uint64_t>(elf, 32);
  uint16_t phentsize = Get<uint16_t>(elf, 54), phnum = Get<uint16_t>(elf, 56);

  struct Load {
    uint64_t off, vaddr, filesz, memsz;
    uint32_t flags;
  };
  std::vector<Load> loads;
  for (uint16_t i = 0; i < phnum; i++) {
    uint64_t p = phoff + uint64_t(i) * phentsize;
    if (Get<uint32_t>(elf, p) != 1) continue;  // PT_LOAD
    loads.push_back({Get<uint64_t>(elf, p + 8), Get<uint64_t>(elf, p + 16),
                     Get<uint64_t>(elf, p + 32), Get<uint64_t>(elf, p + 40),
                     Get<uint32_t>(elf, p + 4)});
  }
  static const uint32_t kFlags[3] = {5, 4, 6};  // R X, R, RW
  if (loads.size() != 3) return Fail("expected three PT_LOAD segments");
  for (int i = 0; i < 3; i++) {
    if (loads[i].flags != kFlags[i]) return Fail("PT_LOADs are not text, rodata, data");
    if (loads[i].vaddr % kPage) return Fail("a PT_LOAD is not page-aligned");
    if (loads[i].off + loads[i].filesz > elf.size()) return Fail("a PT_LOAD is past the file");
    if (i < 2 && loads[i].memsz != loads[i].filesz) return Fail("bss outside the data segment");
  }
  if (loads[0].vaddr != 0) return Fail("text is not linked at 0");

  uint64_t sizes[3];
  for (int i = 0; i < 3; i++) sizes[i] = PageUp(loads[i].filesz);
  for (int i = 0; i < 2; i++)
    if (loads[i].vaddr + sizes[i] > loads[i + 1].vaddr) return Fail("segments overlap");
  uint64_t total = loads[2].vaddr + sizes[2];
  std::vector<uint8_t> nro(total, 0);
  for (const Load& l : loads) std::memcpy(nro.data() + l.vaddr, elf.data() + l.off, l.filesz);

  for (uint64_t i = 0x10; i < 0x80; i++)
    if (nro[i]) return Fail("bytes 0x10-0x7F of text are not crt0's zeroed NRO header");
  std::memcpy(nro.data() + 0x10, "NRO0", 4);
  Put<uint32_t>(nro, 0x14, 0);  // version
  Put<uint32_t>(nro, 0x18, uint32_t(total));
  Put<uint32_t>(nro, 0x1C, 0);  // flags
  for (int i = 0; i < 3; i++) {
    Put<uint32_t>(nro, 0x20 + i * 8, uint32_t(loads[i].vaddr));
    Put<uint32_t>(nro, 0x24 + i * 8, uint32_t(sizes[i]));
  }
  uint64_t bss = PageUp(loads[2].vaddr + loads[2].memsz) - (loads[2].vaddr + sizes[2]);
  Put<uint32_t>(nro, 0x38, uint32_t(bss));

  std::ofstream out(argv[2], std::ios::binary);
  out.write(reinterpret_cast<const char*>(nro.data()), std::streamsize(nro.size()));
  return out ? 0 : Fail("cannot write the NRO");
}
