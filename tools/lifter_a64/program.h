#pragma once
// The module being lifted and its functions. Function starts come from the
// image reader (for a64/image.h's modules: the entry point, .eh_frame FDEs,
// sized function symbols and relocation targets inside .text) and from
// branch targets (src/a64/ARCHITECTURE.md, "Modules and function
// discovery"); each function's instructions are those reachable from its
// start within its range, so data after a return (literal pools) is never
// decoded.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "a64.h"
#include "a64/image.h"

namespace lifter::a64 {

// A module as its image reader hands it over: the bytes as loaded at
// `base` (relocated), the code range and what the format says about code.
struct Image {
  uint64_t base = 0;
  std::vector<uint8_t> bytes;
  uint64_t text_lo = 0, text_hi = 0;  // guest VAs, [lo, hi)
  // Function starts and their sizes (0: up to the next start).
  std::map<uint64_t, uint64_t> starts;
  std::map<uint64_t, std::string> names;

  // A start given more than once keeps the first size that is not 0.
  void AddStart(uint64_t va, uint64_t size = 0) {
    auto [it, added] = starts.emplace(va, size);
    if (!added && !it->second) it->second = size;
  }
};

// An executable format: static-PIE ELF is the core's own (elf_reader.cpp);
// a platform module (ARCHITECTURE.md, "Layout: base, CPU cores, platform
// modules") registers its own from a static initialiser in the source it
// adds to recomp_lift_a64. Match says whether a file is in its format;
// Load fills the image.
struct ImageReader {
  bool (*Match)(const std::string& path);
  bool (*Load)(Image& img, const std::string& path, std::string& err);
};
void RegisterImageReader(const ImageReader& r);

// Fills img from a module loaded and relocated at `base`: the entry point
// (the start of .text), .eh_frame FDEs, sized function symbols and
// relative relocations that point into .text.
void ImageFromModule(Image& img, recomp::a64::Module&& m, uint64_t base);

struct Function {
  uint64_t va = 0;
  uint64_t end = 0;  // exclusive end of its range
  std::map<uint64_t, a64::Insn> insns;
  std::set<uint64_t> labels;  // targets of branches inside the function
  // BR instructions recognised as jump tables: their possible targets.
  std::map<uint64_t, std::vector<uint64_t>> tables;
};

class Program {
 public:
  bool Load(const std::string& path, std::string& err);
  void Discover();

  bool InText(uint64_t va) const { return va >= img_.text_lo && va < img_.text_hi; }
  uint32_t Word(uint64_t va) const;
  // The relocated image's bytes at a guest address (0 outside the image).
  uint64_t Read(uint64_t va, int size, bool sign) const;
  const std::string* NameOf(uint64_t va) const;

  uint64_t base() const { return img_.base; }
  // TextHash() of the code range, for the runtime's check.
  uint64_t text_hash() const;
  const std::map<uint64_t, Function>& functions() const { return funcs_; }
  const std::vector<std::string>& warnings() const { return warnings_; }

 private:
  Function Build(uint64_t start, uint64_t end, std::set<uint64_t>& found);
  bool JumpTable(const Function& f, uint64_t br_va, std::vector<uint64_t>& targets);
  bool ConstOf(uint64_t va, uint64_t lo, int reg, uint64_t& out, int depth = 0) const;

  Image img_;
  std::map<uint64_t, uint64_t> sized_;  // start -> size, from FDEs and symbols
  std::map<uint64_t, Function> funcs_;
  std::vector<std::string> warnings_;
};

// True if `i` writes general register `r` (0-30; 31 means SP here).
bool WritesReg(const a64::Insn& i, int r);

}  // namespace lifter::a64
