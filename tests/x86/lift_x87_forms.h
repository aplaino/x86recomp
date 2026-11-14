#pragma once
// One lifted x87 instruction form, or a sequence of forms lifted as one
// block (from recomp_lift --x87-test).
#include <cstdint>

#include "lift_form_mem.h"
#include "x86/ctx.h"

struct X87Form {
  void (*fn)(recomp::Ctx&);
  uint8_t bytes[128];  // one instruction, or a sequence of them, as run natively
  uint8_t len;
  int mem_bytes;      // memory operand size, 0 if none
  FormMem m;
  const char* text;   // disassembly of the original instruction
  uint32_t example_va;
};
extern const X87Form kX87Forms[];
extern const int kX87FormCount;
