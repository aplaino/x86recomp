#pragma once
// Programs lifted with and without the flag analysis (from recomp_lift
// --flags-test; see tools/lifter/flagstest.cpp).
#include <cstdint>

#include "runtime/ctx.h"

struct FlagsFn {
  uint32_t va;
  void (*fn)(recomp::Ctx&);
};
// What the analysis found for one function: flags read at entry, flags
// kept at its returns (without the part every return keeps for indirect
// call sites).
struct FlagsMask {
  const char* name;
  uint32_t va, reads, ret;
};
struct FlagsProgram {
  const char* name;
  const FlagsFn* opt;  // with the analysis
  const FlagsFn* ref;  // every flag spilled
  int fn_count;
  const FlagsMask* masks;
  int mask_count;
  uint32_t any, indirect;
};
extern const FlagsProgram kFlagsPrograms[];
extern const int kFlagsProgramCount;
