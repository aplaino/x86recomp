// What lifted code calls the platform for (ppc/lifted.h): the system call
// instruction, traps and the time base. Imports are hle.cpp's.

#include "ppc/fatal.h"
#include "ppc/lifted.h"
#include "xbox360/kernel.h"

namespace recomp::ppc {

// Titles call the kernel through imports; `sc` is the kernel's own way
// into the hypervisor.
void Sc(Ctx& c, uint64_t va) { Fatal(&c, "sc at %llx: hypervisor calls are not modelled", (unsigned long long)va); }

// A trap that fires: TWI 31, r0, 22 is a debug break (__debugbreak); no
// debugger is attached, so it stops the title, as any other trap does.
void Trap(Ctx& c, uint32_t raw, uint64_t va) {
  if (raw == 0x0FE00016) Fatal(&c, "debug break at %llx", (unsigned long long)va);
  Fatal(&c, "trap %08x at %llx", raw, (unsigned long long)va);
}

uint64_t TimeBase(Ctx&) { return xbox360::TimeBaseTicks(); }

}  // namespace recomp::ppc
