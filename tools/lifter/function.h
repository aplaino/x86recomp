#pragma once
// Per-function control-flow discovery by recursive descent from the entry.
// Only reachable instructions are decoded, so data and padding inside .text
// are never mistaken for code. Code shared between functions (jumps into
// another function's body that is not an entry) is simply lifted into each
// function that reaches it.

#include <Zydis/Zydis.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "program.h"

namespace lifter {

struct Insn {
  uint32_t va;
  ZydisDecodedInstruction d;
  ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
  bool ok;  // false: undecodable bytes, lifted as a fatal trap
  uint32_t next() const { return va + (ok ? d.length : 1); }
};

enum class Flow : uint8_t {
  Fall,        // continues at next()
  Branch,      // jcc: next() and target
  Jump,        // direct jmp inside the function
  TailCall,    // direct jmp/fallthrough to another function's entry
  JumpTable,   // indirect jmp resolved to targets
  TailDispatch,// indirect jmp with unknown target
  Return,      // ret
  Stop,        // noreturn call, int3, hlt, decode failure
};

struct Function {
  uint32_t entry = 0;
  std::map<uint32_t, Insn> insns;
  std::set<uint32_t> labels;            // addresses needing a goto label
  std::set<uint32_t> direct_calls;      // call targets (for discovery)
  std::set<uint32_t> tail_calls;        // jmp/fallthrough to other entries
  std::vector<std::string> warnings;
};

class Decoder {
 public:
  Decoder();
  bool Decode(const Program& p, uint32_t va, Insn* out) const;
  std::string Format(const Insn& in) const;

 private:
  ZydisDecoder dec_;
  ZydisFormatter fmt_;
};

// Classifies an instruction's control flow; `target` gets the direct branch
// target where there is one.
Flow Classify(const Program& p, const Function& f, const Insn& in,
              uint32_t* target);

void BuildFunction(const Program& p, const Decoder& dec, uint32_t entry,
                   Function* out);

// A direct branch target that is another (strong) function entry: reached
// by a tail call, not lifted into f. See Program::weak.
bool IsOtherEntry(const Program& p, const Function& f, uint32_t t);

// Absolute target of a relative branch/call operand.
uint32_t RelTarget(const Insn& in);

}  // namespace lifter
