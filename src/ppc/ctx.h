#pragma once
// Guest CPU context: one per guest thread. Lifted functions take it by
// reference, cache the registers they use in locals and spill them back
// around anything that can observe them (calls, system calls, traps,
// returns). The CPU is in 32-bit mode (ppc/lifted.h).

#include <cstdint>

namespace recomp::ppc {

struct Ctx {
  uint64_t r[32];
  uint64_t lr, ctr;
  uint32_t cr;     // CR0 in bits 31-28 (LT GT EQ SO) .. CR7 in bits 3-0, as MFCR reads it
  uint32_t xer;    // SO OV CA in bits 31, 30, 29 and the byte count in bits 6-0, as MFXER reads it
  uint64_t msr;    // what MFMSR reads back: whatever MTMSR(D) wrote
  uint32_t vrsave;
  // Floating point: FPRs as their bit patterns, FPSCR as MFFS reads it.
  // Lifted code leaves them here (every floating-point operation is a
  // call into ppc/fp.cpp).
  uint64_t f[32];
  uint32_t fpscr;
  // The guest arena: guest address a is at mem + a, big-endian.
  uint8_t* mem;
  // The reservation (LWARX/LDARX, STWCX./STDCX.). A store-conditional
  // succeeds only while the reservation holds the address it stores to and
  // memory still holds the value the load saw: an atomic compare-and-swap,
  // as QEMU models it.
  uint32_t resv_addr;
  uint64_t resv_value;
  bool resv_valid;
};

}  // namespace recomp::ppc
