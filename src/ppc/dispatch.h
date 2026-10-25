#pragma once
// The lifted function table: guest VA -> lifted function, over every
// lifted module loaded (each registers its table as its library loads).

#include <cstdint>

#include "ppc/lifted.h"

namespace recomp::ppc {

GuestFn LookupLifted(uint32_t va);
// True if a lifted module made from code with this TextHash() is loaded.
bool HasLiftedModule(uint64_t text_hash);

}  // namespace recomp::ppc
