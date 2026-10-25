#pragma once
// One lifted instruction form (from recomp_lift_ppc --forms).
#include <cstdint>

#include "ppc/ctx.h"

struct Form {
  void (*fn)(recomp::ppc::Ctx&);
  uint64_t va;  // where the example instruction is in the fixture
  uint32_t raw;
  const char* name;
};
extern const Form kForms[];
extern const int kFormCount;
