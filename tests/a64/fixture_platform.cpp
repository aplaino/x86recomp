// The test platform's supervisor calls (fixture/crt0.S), its definition of
// a64/lifted.h's Svc: 0 exits, 1 writes x1 bytes at x0 to stdout.
// oracle/oracle.py answers the same two.

#include <cstdio>
#include <cstdlib>

#include "a64/fatal.h"
#include "a64/lifted.h"

namespace recomp::a64 {

void Svc(Ctx& c, uint32_t imm, uint64_t va) {
  switch (imm) {
    case 0:  // exit
      std::fflush(stdout);
      std::exit(0);
    case 1:  // u32 fixture_write(const char* str, u64 size)
      std::fwrite(reinterpret_cast<const char*>(c.x[0]), 1, c.x[1], stdout);
      c.x[0] = 0;
      return;
    default:
      Fatal(&c, "unhandled svc %#x at %llx", imm, static_cast<unsigned long long>(va));
  }
}

}  // namespace recomp::a64
