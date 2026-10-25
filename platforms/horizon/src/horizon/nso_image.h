#pragma once
// Executable modules as the console's loader sees them: the memory image
// (segments at their offsets, .bss zeroed), MOD0 and what its .dynamic
// points at. Shared by the lifter and the runtime loader so both see the
// same bytes at the same addresses. NRO (homebrew) only for now; NSO
// (LZ4 segments) joins it with the first game.

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::horizon {

struct Segment {
  uint32_t off = 0, size = 0;  // image offset and size
};

struct Symbol {
  std::string name;
  uint64_t value = 0;  // image offset (0 for an import)
  uint64_t size = 0;
  bool func = false;
  bool defined = false;
};

struct Reloc {
  uint64_t off = 0;  // image offset of the patched word
  uint32_t type = 0;
  uint32_t sym = 0;  // index into Module::symbols
  int64_t addend = 0;
};

constexpr uint32_t kRelocAarch64Relative = 1027;

struct Module {
  std::string path;
  std::vector<uint8_t> image;  // text..bss, rounded up to 4 KiB
  Segment text, ro, data, bss;
  uint32_t mod0 = 0;           // image offset of the MOD0 header
  uint32_t dynamic = 0;        // image offsets (0: absent)
  uint32_t eh_frame_hdr = 0, eh_frame_hdr_size = 0;
  std::vector<Symbol> symbols;
  std::vector<Reloc> relocs;   // DT_RELA, then DT_JMPREL
};

// Reads an NRO file. On failure returns false with a message in `err`.
bool LoadNro(const std::string& path, Module& m, std::string& err);

// Applies the module's relocations to its image for a load at `base`.
// Only relative relocations exist so far; any other type fails with the
// offset and type, since imports are bound by the runtime, not here.
bool Relocate(Module& m, uint64_t base, std::string& err);

// FNV-1a over the text segment: identifies the code a lifted build was made
// from, so the runtime refuses to run it against a different module.
uint64_t TextHash(const Module& m);

// Function ranges from .eh_frame_hdr's search table and the FDEs it points
// at: one per FDE, as image offsets. Empty without .eh_frame_hdr.
struct FdeRange {
  uint64_t start = 0, size = 0;
};
std::vector<FdeRange> EhFrameFunctions(const Module& m);

}  // namespace recomp::horizon
