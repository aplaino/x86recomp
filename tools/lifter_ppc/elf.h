#pragma once
// Big-endian PowerPC64 ELF executables, statically linked at their load
// address: recomp_lift_ppc's own image format (elf_reader.cpp), which
// needs no platform module. The test programs built by clang and LLD are
// these; the tests that run what was lifted from them load them too.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lifter::ppc {

struct ElfImage {
  uint64_t base = 0;  // the lowest allocated section's address
  uint64_t entry = 0;
  std::vector<uint8_t> bytes;  // the allocated sections from `base`, zeros between and for .bss
  std::vector<std::pair<uint64_t, uint64_t>> code;  // executable sections, [lo, hi)
  std::vector<std::pair<uint64_t, uint64_t>> functions;  // sized function symbols: start, size
};

// TextHash() of the executable sections in address order: what
// recomp_lift_ppc stores as kLiftedTextHash for this image.
uint64_t TextHash(const ElfImage& e);

// True if the file starts with the ELF magic.
bool IsElf(const std::string& path);
bool LoadElf(const std::string& path, ElfImage& e, std::string& err);

}  // namespace lifter::ppc
