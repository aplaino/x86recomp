#pragma once
// Which arithmetic flags (CF PF AF ZF SF OF) are live where control leaves
// a lifted function, so that calls, returns and tail calls spill only
// those to Ctx::eflags (see "Flags across calls" in ARCHITECTURE.md).
// 32-bit images only.
//
// Inside a function the flags are locals and the compiler drops the dead
// ones itself; what it cannot see is the other side of a call. So, as in
// box64's and FEX's flag elimination but across functions, each function
// gets two masks, solved together over the whole program:
//   reads(f): flags f may read before writing them (live at its entry),
//   ret(f):   flags some caller may read after f returns, before writing
//             them (live at f's returns).
// A direct call to g spills reads(g); everything live after it is in
// ret(g), and g passes on what it does not write, so that is in reads(g)
// too. A return spills ret(f) and a tail call to g reads(g), with
// ret(g) including ret(f). A function the game replaces counts as its
// lifted body.
//
// What the lifter cannot follow is assumed to follow the calling
// convention, which no compiler breaks: a function entered through a
// pointer (indirect call or jump, Dispatch) reads no flag at entry, a
// manifest hook and an import read none (but RaiseException and
// RtlUnwind, which put EFLAGS in a CONTEXT for SEH handlers: all), and
// code reached only through a pointer scan (Program::weak, often data
// that decodes as code) puts no constraint on the functions it calls.
// The flags read after an indirect call are kept by every return
// (`indirect`), and flags live after a hook or an import pass through
// it. lift_report.txt lists the address-taken functions that read flags
// at entry (`any`: what the assumption leaves out); --flags-check checks
// all of it while the game runs.
//
// DF, AC and ID are not part of this: in 32-bit lifted code they live in
// Ctx::eflags only (cld/std, popfd write them there), so a spill never has
// to carry them.

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "function.h"

namespace lifter {

inline constexpr uint32_t kArithFlags = 0x8D5u;  // CF PF AF ZF SF OF

// The arithmetic flags an instruction reads (use) and those it writes on
// every execution (kill). Undefined results and writes that depend on a
// count (shifts by CL, rep cmps/scas) do not kill.
struct FlagEffect {
  uint32_t use = 0, kill = 0;
};
FlagEffect InsnFlags(const Insn& in);

class FlagLiveness {
 public:
  // Analyses every entry of p (BuildFunction on each). Appends a summary
  // to *report.
  void Run(const Program& p, const Decoder& dec, std::string* report,
           const std::set<uint64_t>& dump = {});
  // After Run: each instruction of the function at entry f with the
  // flags live before it (for --flags-dump).
  std::string Dump(uint64_t f) const;

  // Flags to spill before entering the function at entry t (a direct call
  // or tail call): reads(t), all for an unknown t.
  uint32_t Reads(uint64_t t) const;
  // Flags to spill at a return of the function at entry f.
  uint32_t Ret(uint64_t f) const;
  // At the indirect or import call at va in the function at entry f.
  uint32_t CallSite(uint64_t f, uint64_t va) const;
  // At an indirect jump leaving the function at entry f.
  uint32_t IndirectTail(uint64_t f) const { return Ret(f); }
  // What address-taken functions read at entry (not spilled for them:
  // see above; for review), and what any return keeps for indirect call
  // sites (part of every Ret).
  uint32_t Any() const { return any_; }
  uint32_t Indirect() const { return indirect_; }

 private:
  struct Fn {
    uint32_t reads = 0, ret = 0;
  };
  std::unordered_map<uint64_t, Fn> fns_;
  std::unordered_map<uint64_t, uint32_t> sites_;  // entry << 32 | va
  std::unordered_map<uint64_t, std::string> dumps_;
  uint32_t any_ = kArithFlags, indirect_ = kArithFlags;
  bool done_ = false;
  const Program* p_ = nullptr;
};

}  // namespace lifter
