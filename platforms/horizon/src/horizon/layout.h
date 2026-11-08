#pragma once
// The guest address space, fixed for every game so that addresses in crash
// logs and comments mean the same thing everywhere (ARCHITECTURE.md, "What
// 64-bit means here"). tests/oracle/*.py repeat these values.
//
// Everything guest-visible is in one window below 1<<39 that both hosts
// leave free at startup: on macOS arm64 the kernel keeps
// [0xfc0000000, 0x7000000000) unmappable and malloc reserves its regions
// at random above 0x7180000000.

#include <cstdint>

namespace recomp::horizon {

constexpr uint64_t kGuestBase = 0x7000000000ull;
constexpr uint64_t kGuestEnd = 0x7180000000ull;

// The main module (an NRO's only one), as the console's loader places it.
// Modules fill [kMainBase, kGuestEnd); stacks and heap are below kMainBase.
constexpr uint64_t kMainBase = 0x7100000000ull;
// The main thread's stack.
constexpr uint64_t kMainStackBase = kGuestBase;
constexpr uint64_t kMainStackSize = 0x100000;

}  // namespace recomp::horizon
