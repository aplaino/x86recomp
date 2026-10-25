// The Horizon kernel's supervisor calls that lifted code makes, by number
// (ARCHITECTURE.md, "HLE boundary"): the platform's definition of
// a64/lifted.h's Svc. Results go in x0; an unhandled call is fatal by
// design.

#include <cstdio>
#include <cstdlib>

#include "a64/fatal.h"
#include "a64/lifted.h"

namespace recomp::a64 {

void Svc(Ctx& c, uint32_t imm, uint64_t va) {
  switch (imm) {
    case 0x07:  // void svcExitProcess()
      std::fflush(stdout);
      std::exit(0);
    case 0x26:  // void svcBreak(u32 reason, uintptr_t arg, size_t size)
      Fatal(&c, "svcBreak(%#llx, %#llx, %#llx) at %llx", static_cast<unsigned long long>(c.x[0]),
            static_cast<unsigned long long>(c.x[1]), static_cast<unsigned long long>(c.x[2]),
            static_cast<unsigned long long>(va));
    case 0x27:  // Result svcOutputDebugString(const char* str, u64 size)
      std::fwrite(reinterpret_cast<const char*>(c.x[0]), 1, c.x[1], stdout);
      c.x[0] = 0;
      return;
    default:
      Fatal(&c, "unhandled svc %#x at %llx", imm, static_cast<unsigned long long>(va));
  }
}

}  // namespace recomp::a64
