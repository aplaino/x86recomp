#pragma once
// The lifted function table: guest VA -> lifted function.

#include <cstdint>

#include "ppc/lifted.h"

namespace recomp::ppc {

GuestFn LookupLifted(uint32_t va);
// TextHash() of the module the registered table was lifted from.
uint64_t LiftedTextHash();

}  // namespace recomp::ppc
