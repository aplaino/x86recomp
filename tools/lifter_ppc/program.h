#pragma once
// The module being lifted and its functions. Function starts come from the
// image reader (for the Xbox 360's XEX: the entry point, .pdata and the
// import thunks), from branch targets and from addresses held as constants
// that land in code no function reaches (PointerStarts); each function's
// instructions are those reachable from its start within its range, so
// data after a return is never decoded.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ppc.h"

namespace lifter::ppc {

// An imported function: the lifter emits a call to the platform's Import()
// in place of the thunk the image has for it.
struct ImportRef {
  std::string module;
  uint32_t ordinal = 0;
};

// A module as its image reader hands it over: the bytes as loaded at
// `base`, its code ranges and what the format says about code.
struct Image {
  uint64_t base = 0;
  std::vector<uint8_t> bytes;
  std::vector<std::pair<uint64_t, uint64_t>> code;  // guest VAs, [lo, hi)
  // Function starts and their sizes (0: up to the next start).
  std::map<uint64_t, uint64_t> starts;
  std::map<uint64_t, std::string> names;
  std::map<uint64_t, ImportRef> imports;  // thunk VA -> export it calls
  Cpu cpu = Cpu::kGeneric;  // the CPU its code is for
  // The length in bytes of the prologues the format describes, by function
  // start: the lifter reads each into a frame description (LiftedFrame).
  std::map<uint64_t, uint64_t> prologues;
  // Functions an exception's unwind may continue in (those with an
  // exception handler), and addresses in them it may continue at that the
  // format names (an __except block's start); see Program::ResumeStarts.
  std::set<uint64_t> resumable;
  std::set<uint64_t> resume_targets;

  // A start given more than once keeps the first size that is not 0.
  void AddStart(uint64_t va, uint64_t size = 0) {
    auto [it, added] = starts.emplace(va, size);
    if (!added && !it->second) it->second = size;
  }
};

// An executable format, read by a console platform's module (ARCHITECTURE.md,
// "Layout: base, CPU cores, platform modules"), which registers it from a
// static initialiser in the source it adds to recomp_lift_ppc. Match says
// whether a file is in its format; Load fills the image.
struct ImageReader {
  bool (*Match)(const std::string& path);
  bool (*Load)(Image& img, const std::string& path, std::string& err);
};
void RegisterImageReader(const ImageReader& r);

struct Function {
  uint64_t va = 0;
  uint64_t end = 0;  // exclusive end of its range
  const ImportRef* import = nullptr;  // an import thunk: no instructions
  std::map<uint64_t, ppc::Insn> insns;
  std::set<uint64_t> labels;  // targets of branches inside the function
  // BCCTR instructions recognised as jump tables: their possible targets.
  std::map<uint64_t, std::vector<uint64_t>> tables;
  // An unwind may continue in it (Image::resumable): GuestResume.
  bool resumable = false;
  // BCLR instructions after the stack pointer was loaded from memory:
  // jumps to another frame (ppc::Jump), not returns.
  std::set<uint64_t> jumps;
};

// A prologue as the lifter read it (recomp::ppc::LiftedFrame).
struct Frame {
  uint64_t va = 0, end = 0;
  int32_t sp = 0, fp_offset = 0;
  uint8_t fp = 0;
  bool unreadable = false;
  std::vector<recomp::ppc::LiftedSave> saves;
};

class Program {
 public:
  bool Load(const std::string& path, std::string& err);
  void Discover();

  bool InText(uint64_t va) const;
  uint32_t Word(uint64_t va) const;
  // The image's bytes at a guest address, big-endian (0 outside the image).
  uint64_t Read(uint64_t va, int size, bool sign) const;
  const std::string* NameOf(uint64_t va) const;

  uint64_t base() const { return img_.base; }
  Cpu cpu() const { return img_.cpu; }
  // Overrides the CPU the image reader gave (before Discover).
  void set_cpu(Cpu cpu) { img_.cpu = cpu; }
  // TextHash() of the code ranges in address order, for the runtime's check.
  uint64_t text_hash() const;
  const std::map<uint64_t, Function>& functions() const { return funcs_; }
  // The frames of the functions whose prologue the image describes.
  std::vector<Frame> Frames() const;
  const std::vector<std::string>& warnings() const { return warnings_; }

 private:
  Function Build(uint64_t start, uint64_t end, std::set<uint64_t>& found);
  bool JumpTable(const Function& f, uint64_t bcctr_va, std::vector<uint64_t>& targets);
  bool ConstOf(uint64_t va, uint64_t lo, int reg, uint64_t& out, int depth = 0) const;
  // Addresses the module holds as constants: LIS with ADDI or ORI in the
  // code reached, and with `data` the words in its data.
  std::set<uint64_t> Constants(bool data) const;
  // Starts that only a pointer reaches: see Discover.
  std::set<uint64_t> PointerStarts(const std::set<uint64_t>& starts) const;
  // Starts where an unwind may continue a resumable function: see Discover.
  std::set<uint64_t> ResumeStarts(const std::set<uint64_t>& starts) const;
  Frame ReadPrologue(uint64_t va, uint64_t len) const;
  // A function that saves r1 and its return address into the buffer its
  // first argument points at (setjmp): a jump may come back to its return.
  bool SavesContext(const Function& f) const;
  // Calls to such functions: their callers become resumable and their
  // return addresses resume targets. True if that added any.
  bool SetjmpReturns();
  // The end of the code range holding `va`.
  uint64_t CodeEnd(uint64_t va) const;
  void Warn(const std::string& w);

  Image img_;
  std::map<uint64_t, uint64_t> sized_;  // start -> size, from the image
  std::map<uint64_t, Function> funcs_;
  std::vector<std::string> warnings_;
};

// True if `i` writes general register `r`.
bool WritesReg(const ppc::Insn& i, int r);

}  // namespace lifter::ppc
