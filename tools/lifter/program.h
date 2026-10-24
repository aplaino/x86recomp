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
  std::string name;  // file name without extension, e.g. "game"
  // Where it is mapped, and the base its symbol files and manifest use.
  uint64_t base = 0, preferred_base = 0;
  uint64_t text_lo = 0, text_hi = 0;  // [lo, hi) of .text
  // Initialised data scanned for code pointers, as [lo, hi) ranges.
  std::vector<std::pair<uint64_t, uint64_t>> data;
  // Read-only data outside .text that may hold switch tables (an ELF's
  // .rodata; MSVC puts its tables in .text).
  std::vector<std::pair<uint64_t, uint64_t>> rodata;
  recomp::PeImage pe;  // PE modules only; empty for an ImageReader's
};

struct Program;

// A non-PE executable format, read by a console platform's module
// (ARCHITECTURE.md, "Console platforms"), which registers it from a static
// initialiser in the source it adds to recomp_lift. Match says whether a
// file is in its format (and whether it is x64 code); Load maps the image
// into p.mem, fills m (all but name) and adds what the format itself says
// about code to p: function entries, names, noreturn import slots.
struct ImageReader {
  bool (*Match)(const std::string& path, bool* x64);
  bool (*Load)(Program& p, Module& m, const std::string& path, uint64_t base,
               std::string* err);
};
void RegisterImageReader(const ImageReader& r);

struct Program {
  recomp::GuestMemory mem;
  std::vector<Module> modules;  // [0] is the EXE
  bool x64 = false;  // PE32+ AMD64 images (all modules are one or the other)

  std::set<uint64_t> entries;                 // function entry points
  // Entries known only from pointer scans (a dword in data or an
  // immediate that happens to point into .text). They get their own
  // lifted function for indirect calls, but do not split other functions:
  // code running into one continues inline. Many are not function starts
  // at all, and splitting a loop at one turned every iteration into a
  // nested host call (a "freeze" that was really deepening recursion).
  std::set<uint64_t> weak;

  // True if control reaching t from another function leaves that function
  // (a tail call) rather than continuing inline.
  bool Splits(uint64_t t) const { return entries.count(t) && !weak.count(t); }
  std::map<uint64_t, std::string> names;      // demangled or Ghidra names
  std::set<uint64_t> noreturn;                // functions that never return
  std::map<uint64_t, std::vector<uint64_t>> jumptables;  // jmp va -> targets
  std::set<uint64_t> noreturn_iat;            // IAT slots of ExitProcess etc.
  std::map<uint64_t, std::string> probes;     // manifest: probe on entry
  std::set<uint64_t> replaced;                // manifest: the game project
                                              // defines f_X; the lifted body
                                              // is emitted as f_X_lifted
  std::set<uint64_t> resume_sites;            // manifest: calls/jmps that
                                              // abandon the stack (SEH,
                                              // longjmp)
  std::set<uint64_t> setjmps;                 // manifest: setjmp functions
                                              // (their callers can be
                                              // resumed by longjmp)
  // x64 functions with a language handler (their exceptions' unwinds end
  // in them): where the prologue ends and the frame register (0: rsp) and
  // its offset, which give the establisher frame there.
  struct EhFrame {
    uint64_t prolog_end;
    uint8_t frame_reg, frame_off;
  };
  std::map<uint64_t, EhFrame> eh_frames;      // by function entry

  // Maps an image (at `base`, 0 for its preferred base) and reads its
  // symbols (none if symdir is empty; PE images need them). The first one
  // is the EXE.
  bool Load(const std::string& path, const std::string& symdir, uint64_t base,
            std::string* err);

  // Applies the manifest of the module loaded last (entry/name/probe/
  // resume/replace/setjmp lines, VAs at its preferred base).
  bool LoadManifest(const std::string& path, std::string* err);

  // The module whose .text contains va, or null.
  const Module* TextOf(uint64_t va) const {
    for (const auto& m : modules)
      if (va >= m.text_lo && va < m.text_hi) return &m;
    return nullptr;
  }
  bool InText(uint64_t va) const { return TextOf(va) != nullptr; }
  const uint8_t* Bytes(uint64_t va) const { return mem.ptr<uint8_t>(va); }
  size_t BytesLeft(uint64_t va) const { return TextOf(va)->text_hi - va; }
};

}  // namespace lifter
