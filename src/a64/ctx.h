#pragma once
// Guest CPU context: one per guest thread. Lifted functions take it by
// reference, cache the general registers, SP and NZCV in locals and spill
// them back around anything that can observe them (calls, supervisor
// calls, returns).

#include <cstdint>

namespace recomp::a64 {

struct Ctx {
  uint64_t x[31];
  uint64_t sp;
  uint32_t nzcv;       // N Z C V in bits 31-28, as MRS NZCV reads it
  uint64_t tpidr_el0;  // the thread pointer, guest-writable
  // The local exclusive monitor (LDXR/STXR). A store-exclusive succeeds
  // only while the monitor holds the address it stores to and memory
  // still holds the value the load-exclusive saw: an atomic
  // compare-and-swap, as QEMU and Unicorn model it.
  uint64_t excl_addr;
  uint64_t excl_value;
  bool excl_valid;
};

}  // namespace recomp::a64
