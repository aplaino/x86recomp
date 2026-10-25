#pragma once
// The lifted function table: guest VA -> lifted function.

#include <cstdint>

#include "a64/lifted.h"

namespace recomp::a64 {

GuestFn LookupLifted(uint64_t va);
// TextHash() of the module the registered table was lifted from.
uint64_t LiftedTextHash();

}  // namespace recomp::a64
