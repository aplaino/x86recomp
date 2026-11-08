#pragma once
// Position-independent AArch64 modules as a loader sees them: the memory
// image (segments at their offsets from 0, .bss zeroed) and what its
// .dynamic points at. Shared by the lifter and the runtime loaders so both
// see the same bytes at the same addresses. The core reads static-PIE ELF
// itself (its tests' fixture); a platform's formats fill the same Module
// and read .dynamic with ReadDynamic.

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::a64 {

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
  uint32_t dynamic = 0;        // image offsets (0: absent)
  uint32_t eh_frame_hdr = 0, eh_frame_hdr_size = 0;
  std::vector<Symbol> symbols;
  std::vector<Reloc> relocs;   // DT_RELA, then DT_JMPREL
};

// Where the core loads a static-PIE ELF (the lifter's own reader and the
// core's tests): in the window below 1<<39 that both hosts leave free at
// startup (macOS arm64 keeps [0xfc0000000, 0x7000000000) unmappable and
// reserves malloc regions at random above 0x7180000000).
constexpr uint64_t kElfBase = 0x7100000000ull;

// Reads a static-PIE AArch64 ELF (ET_DYN linked at 0: text, read-only and
// data segments, PT_DYNAMIC, PT_GNU_EH_FRAME). On failure returns false
// with a message in `err`.
bool LoadElf(const std::string& path, Module& m, std::string& err);

// Reads m.dynamic's symbols and relocations into m (a loader that has
// filled m.image and m.dynamic calls it).
bool ReadDynamic(Module& m, std::string& err);

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

}  // namespace recomp::a64
