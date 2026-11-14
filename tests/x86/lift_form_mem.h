#pragma once
// The memory operand of a lifted form (recomp_lift --*-test, KeepAddress
// in tools/lifter_x86/x87test.cpp) and how the tests aim it at their
// buffer, which they map at the same low address natively and in the
// guest arena: plain operands address kFormTarget, fs/gs ones
// kFormSegOffset past the segment base (kFormTarget - kFormSegOffset; gs
// natively, as fs belongs to the C library), RIP-relative ones (x64)
// kFormRipTarget when lifted and a buffer of the test's own natively.
#include <cstdint>

struct FormMem {
  bool mem;   // has a memory operand (lea's is none)
  int base;   // GPRs set so that base + index * scale + disp is the
  int index;  // target (-1: none; neither: absolute, the target itself)
  int scale;
  int64_t disp;
  bool seg;   // fs:/gs: operand
  int rip;    // offset of a RIP-relative operand's disp32 in the bytes
};
extern const uint64_t kFormTarget, kFormSegOffset, kFormRipTarget;

// Sets r[m.base] and r[m.index] (to `idx` when there is a base too) so
// that the operand addresses its target, or `skew` bytes past it where
// registers can move it (an index alone moves it by multiples of its
// scale). Returns how far past the target it is.
inline uint32_t AimForm(const FormMem& m, uint64_t* r, uint64_t idx, uint32_t skew = 0) {
  if (m.base < 0 && m.index < 0) return 0;
  if (m.base < 0) skew -= skew % uint32_t(m.scale);
  const int64_t t = int64_t(m.seg ? kFormSegOffset : kFormTarget) + skew - m.disp;
  if (m.index < 0)
    idx = 0;
  else if (m.base < 0)
    idx = uint64_t(t / m.scale);  // the generator made it exact
  if (m.index >= 0) r[m.index] = idx;
  if (m.base >= 0) r[m.base] = uint64_t(t) - idx * uint64_t(m.scale);
  return skew;
}
