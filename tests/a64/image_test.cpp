// The static-PIE ELF reader (a64/image.h) on images whose headers do not
// describe a module: each must be refused (or, for .eh_frame_hdr, read no
// further than the image), never read or written past its buffers. Built
// with the address sanitizer where the toolchain has it, with image.cpp
// compiled in, so a read past a buffer fails the test as well.
//   a64_image_test <scratch file>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "a64/image.h"

namespace {

using recomp::a64::Module;

int failures = 0;

void Check(bool ok, const char* what) {
  if (ok) return;
  std::printf("FAIL %s\n", what);
  failures++;
}

template <typename T>
void Put(std::vector<uint8_t>& b, uint64_t off, T v) {
  std::memcpy(b.data() + off, &v, sizeof(T));
}

struct Phdr {
  uint32_t type, flags;
  uint64_t off, va, filesz, memsz;
};

// An ELF64 AArch64 ET_DYN of `size` bytes with these program headers.
std::vector<uint8_t> Elf(const std::vector<Phdr>& ph, size_t size = 0x3000) {
  std::vector<uint8_t> f(size, 0);
  std::memcpy(f.data(), "\x7f" "ELF\x02\x01\x01", 7);
  Put<uint16_t>(f, 16, 3);
  Put<uint16_t>(f, 18, 183);
  Put<uint32_t>(f, 20, 1);
  Put<uint64_t>(f, 32, 64);
  Put<uint16_t>(f, 52, 64);
  Put<uint16_t>(f, 54, 56);
  Put<uint16_t>(f, 56, uint16_t(ph.size()));
  for (size_t i = 0; i < ph.size(); i++) {
    uint64_t p = 64 + 56 * i;
    Put(f, p, ph[i].type);
    Put(f, p + 4, ph[i].flags);
    Put(f, p + 8, ph[i].off);
    Put(f, p + 16, ph[i].va);
    Put(f, p + 32, ph[i].filesz);
    Put(f, p + 40, ph[i].memsz);
  }
  return f;
}

// Text, read-only data and data (with 0x1000 of .bss), .dynamic (empty)
// at the start of data.
std::vector<Phdr> Good() {
  return {{1, 5, 0, 0, 0x1000, 0x1000},
          {1, 4, 0x1000, 0x1000, 0x1000, 0x1000},
          {1, 6, 0x2000, 0x2000, 0x1000, 0x2000},
          {2, 6, 0x2000, 0x2000, 0x10, 0x10}};
}

bool Load(const char* path, const std::vector<uint8_t>& f) {
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
  }
  Module m;
  std::string err;
  return recomp::a64::LoadElf(path, m, err);
}

void Dyn(Module& m, uint64_t& at, int64_t tag, uint64_t v) {
  Put(m.image, at, tag);
  Put(m.image, at + 8, v);
  at += 16;
}

// A module with .dynamic at 0x100: a symbol table of `nchain` entries at
// 0x400 (DT_HASH at 0x300), names at `strtab`, and DT_RELA.
Module Dynamic(uint32_t nchain, uint64_t strtab, uint64_t strsz, uint64_t rela, uint64_t relasz) {
  Module m;
  m.image.assign(0x2000, 0);
  m.dynamic = 0x100;
  uint64_t at = 0x100;
  Dyn(m, at, 6, 0x400);  // DT_SYMTAB
  Dyn(m, at, 4, 0x300);  // DT_HASH
  Dyn(m, at, 5, strtab);  // DT_STRTAB
  Dyn(m, at, 10, strsz);  // DT_STRSZ
  Dyn(m, at, 7, rela);  // DT_RELA
  Dyn(m, at, 8, relasz);  // DT_RELASZ
  Put<uint32_t>(m.image, 0x304, nchain);
  Put<uint32_t>(m.image, 0x400, 8);  // the first symbol's name
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: a64_image_test scratch-file\n");
    return 2;
  }
  const char* path = argv[1];

  // LoadElf: the segments in order, inside the file and the 32-bit image.
  Check(Load(path, Elf(Good())), "a well-formed image is read");
  std::vector<Phdr> ph = Good();
  ph[1].va = 0x3000;  // read-only data after data, whose end sizes the image
  ph[2] = {1, 6, 0x2000, 0x1000, 0x10, 0x10};
  ph[3].va = 0x1000;
  Check(!Load(path, Elf(ph)), "segments out of address order are refused");
  ph = Good();
  ph[1].va = 0x0;
  Check(!Load(path, Elf(ph)), "overlapping segments are refused");
  ph = Good();
  ph[2].filesz = 0x3000, ph[2].memsz = 0x1000;
  Check(!Load(path, Elf(ph, 0x5000)), "a segment with more file than memory is refused");
  ph = Good();
  ph[0].off = ~0ull - 0xFF;
  Check(!Load(path, Elf(ph)), "a file offset that wraps is refused");
  ph = Good();
  ph[2].memsz = 0xFFFFFFFFFFFFF000ull;
  Check(!Load(path, Elf(ph)), "a memory size that wraps is refused");

  // ReadDynamic: the tables inside the image.
  std::string err;
  Module m = Dynamic(1, 0x800, 0x10, 0x900, 24);
  Check(recomp::a64::ReadDynamic(m, err) && m.symbols.size() == 1 && m.relocs.size() == 1,
        "well-formed .dynamic is read");
  m = Dynamic(1, 0x1FF8, 0x100, 0x900, 24);
  Check(!recomp::a64::ReadDynamic(m, err), ".dynstr past the image is refused");
  m = Dynamic(0x10000000, 0x800, 0x10, 0x900, 24);
  Check(!recomp::a64::ReadDynamic(m, err), "a symbol count past the image is refused");
  m = Dynamic(1, 0x800, 0x10, 0x900, 1ull << 40);
  Check(!recomp::a64::ReadDynamic(m, err), "relocations past the image are refused");

  // Relocate: the patched word inside the image.
  m = Module{};
  m.image.assign(0x1000, 0);
  m.relocs.push_back({~0ull - 3, recomp::a64::kRelocAarch64Relative, 0, 0});
  Check(!recomp::a64::Relocate(m, 0, err), "a relocation whose end wraps is refused");

  // EhFrameFunctions: reads stay in the image, the walk ends with it.
  m = Module{};
  m.image.assign(0x1000, 0);
  m.eh_frame_hdr = 0xFFF;
  m.image[0xFFF] = 1;
  Check(recomp::a64::EhFrameFunctions(m).empty(), ".eh_frame_hdr at the image's end has no table");
  // A count far past the image (udata4 encodings).
  m.eh_frame_hdr = 0x100;
  std::memcpy(m.image.data() + 0x100, "\x01\x03\x03\x03", 4);
  Put<uint32_t>(m.image, 0x108, 0xFFFFFFFF);
  Check(recomp::a64::EhFrameFunctions(m).size() < 0x1000, "the FDE table ends with the image");
  // One FDE whose CIE pointer points below 0.
  Put<uint32_t>(m.image, 0x108, 1);
  Put<uint32_t>(m.image, 0x110, 0x200);
  Put<uint32_t>(m.image, 0x200, 0x10);
  Put<uint32_t>(m.image, 0x204, 0x300);
  recomp::a64::EhFrameFunctions(m);

  std::printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
