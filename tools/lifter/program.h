#pragma once
// Everything the lifter knows about the program: the mapped images (the
// EXE and any DLLs lifted with it), function entries, names, no-return
// functions and switch tables. Inputs are the images themselves plus the
// TSVs exported from Ghidra into each one's symbols directory. Images are
// mapped side by side in one guest address space, so a VA (and with it a
// lifted function's name) identifies its module.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "runtime/guest_memory.h"
#include "runtime/pe_image.h"

namespace lifter {

struct Module {
  std::string name;  // file name without extension, e.g. "gamex86"
  recomp::PeImage img;
  uint32_t text_lo = 0, text_hi = 0;  // [lo, hi) of .text
};

struct Program {
  recomp::GuestMemory mem;
  std::vector<Module> modules;  // [0] is the EXE

  std::set<uint32_t> entries;                 // function entry points
  // Entries known only from pointer scans (a dword in data or an
  // immediate that happens to point into .text). They get their own
  // lifted function for indirect calls, but do not split other functions:
  // code running into one continues inline. Many are not function starts
  // at all, and splitting a loop at one turned every iteration into a
  // nested host call (a "freeze" that was really deepening recursion).
  std::set<uint32_t> weak;

  // True if control reaching t from another function leaves that function
  // (a tail call) rather than continuing inline.
  bool Splits(uint32_t t) const { return entries.count(t) && !weak.count(t); }
  std::map<uint32_t, std::string> names;      // demangled or Ghidra names
  std::set<uint32_t> noreturn;                // functions that never return
  std::map<uint32_t, std::vector<uint32_t>> jumptables;  // jmp va -> targets
  std::set<uint32_t> noreturn_iat;            // IAT slots of ExitProcess etc.
  std::map<uint32_t, std::string> probes;     // manifest: probe on entry
  std::set<uint32_t> resume_sites;            // manifest: calls/jmps that
                                              // abandon the stack (SEH)

  // Maps an image (at `base`, 0 for its preferred base) and reads its
  // symbols. The first one is the EXE.
  bool Load(const std::string& path, const std::string& symdir, uint32_t base,
            std::string* err);

  // Applies the manifest of the module loaded last (entry/name/probe/
  // resume lines, VAs at its preferred base).
  bool LoadManifest(const std::string& path, std::string* err);

  // The module whose .text contains va, or null.
  const Module* TextOf(uint32_t va) const {
    for (const auto& m : modules)
      if (va >= m.text_lo && va < m.text_hi) return &m;
    return nullptr;
  }
  bool InText(uint32_t va) const { return TextOf(va) != nullptr; }
  const uint8_t* Bytes(uint32_t va) const { return mem.ptr<uint8_t>(va); }
  size_t BytesLeft(uint32_t va) const { return TextOf(va)->text_hi - va; }
};

}  // namespace lifter
