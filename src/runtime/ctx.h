#pragma once
// Guest CPU context: one per guest thread. Lifted functions take it by
// reference, cache registers and flags in locals, and spill them back
// around anything that can observe them (calls, HLE, returns).

#include <cstdint>

#include "runtime/x87.h"

namespace recomp {

struct Flags {
  bool cf, pf, af, zf, sf, of, df;
  // AC (bit 18) and ID (bit 21) are just storage, but user code can flip
  // them on every CPU since the 486 (AC) and every one with CPUID (ID),
  // and that is how it detects them.
  bool ac, id;
};

struct Thread;

// An SSE register: 16 bytes, lanes little-endian (as in guest memory).
struct Xmm {
  uint8_t b[16];
};

struct Ctx {
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
  Flags f;
  uint32_t fs_base;  // guest VA of this thread's TEB
  x87::State fpu;
  // SSE state. Unlike the integer registers these are never cached in
  // locals: lifted code operates on them in place (runtime/sse.h).
  Xmm xmm[8];
  uint32_t mxcsr;
  Thread* thread;    // runtime-owned per-thread data
};

// EFLAGS packing for pushfd/popfd/lahf/sahf. Bit 1 always reads as 1 and
// IF (bit 9) as set, like user mode on Windows.
inline uint32_t PackEFlags(const Flags& f) {
  return (f.cf ? 0x1u : 0) | 0x2u | (f.pf ? 0x4u : 0) | (f.af ? 0x10u : 0) |
         (f.zf ? 0x40u : 0) | (f.sf ? 0x80u : 0) | 0x200u |
         (f.df ? 0x400u : 0) | (f.of ? 0x800u : 0) | (f.ac ? 0x40000u : 0) |
         (f.id ? 0x200000u : 0);
}
inline void UnpackEFlags(Flags& f, uint32_t v) {
  f.cf = v & 0x1;
  f.pf = v & 0x4;
  f.af = v & 0x10;
  f.zf = v & 0x40;
  f.sf = v & 0x80;
  f.df = v & 0x400;
  f.of = v & 0x800;
  f.ac = v & 0x40000;
  f.id = v & 0x200000;
}

}  // namespace recomp
