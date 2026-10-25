#pragma once
// The guest address space, fixed for every game so that addresses in crash
// logs and comments mean the same thing everywhere (ARCHITECTURE.md, "What
// 64-bit means here"). tests/oracle/*.py repeat these values.

#include <cstdint>

namespace recomp::horizon {

// The main module (an NRO's only one), as the console's loader places it.
constexpr uint64_t kMainBase = 0x7100000000ull;
// The main thread's stack.
constexpr uint64_t kMainStackBase = 0x5000000000ull;
constexpr uint64_t kMainStackSize = 0x100000;

}  // namespace recomp::horizon
