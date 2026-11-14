#pragma once
// One lifted integer instruction form (from recomp_lift --int-test).
#include <cstdint>

#include "lift_form_mem.h"
#include "x86/ctx.h"

struct IntForm {
  void (*fn)(recomp::Ctx&);
  uint8_t bytes[16];  // run natively (fn: lifted from the same instruction)
  uint8_t len;
  FormMem m;
  uint32_t undef;     // EFLAGS bits the instruction leaves undefined
  const char* text;   // disassembly of the original instruction
  uint64_t example_va;
};
extern const IntForm kIntForms[];
extern const int kIntFormCount;
