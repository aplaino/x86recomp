#pragma once
// One lifted instruction form (from recomp_lift_a64 --forms).
#include <cstdint>

#include "a64/ctx.h"

// What a native stub (forms_native.S) runs from and leaves behind; its
// offsets are fixed in recomp_lift_a64's NativeStub.
struct alignas(16) NativeState {
  uint64_t x[31];   // 0
  uint64_t unused;  // 248
  uint64_t nzcv;    // 256
  uint64_t host_sp; // 264, the stub's own
};
static_assert(sizeof(NativeState) == 272);

struct Form {
  void (*fn)(recomp::a64::Ctx&);
  // The original instruction in a stub for this CPU (arm64 builds only;
  // null where running elsewhere than its own address, or with the host's
  // SP and thread pointer, would change its result).
  void (*native)(NativeState*);
  uint64_t va;   // where the example instruction is in the fixture
  uint32_t raw;
  const char* name;
  bool uses_x18;  // reserved by macOS, which may change it at any time
};
extern const Form kForms[];
extern const int kFormCount;
