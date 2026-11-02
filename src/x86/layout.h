#pragma once
// The guest addresses the x86 core reserves in every process, whatever
// its platform: the HLE thunk region (x86/hle.h) and the return address
// of calls from the host into guest code (CallGuest). A platform lays out
// the rest (Win32: win32/process.h).

#include <cstdint>

#include "runtime/guest_memory.h"

namespace recomp {

// 32-bit guests: everything stays below 0x80000000, because games that are
// not large-address-aware may treat pointers as signed.
inline constexpr gaddr kThunkBase = 0x7F000000;
inline constexpr uint32_t kThunkStride = 0x10;
inline constexpr uint32_t kThunkRegionSize = 0x00100000;
// x64 guests get nothing below 4 GiB (ARCHITECTURE.md, "64-bit guests"):
// their thunks sit near the top of the default 64 GiB arena.
inline constexpr gaddr kThunkBase64 = 0xFFF000000;
inline constexpr unsigned kArenaBits64 = 36;
inline constexpr gaddr kUserLo64 = 0x100000000;

// Return address pushed when the host calls into guest code; reaching it
// means the guest function returned to us.
inline constexpr uint32_t kReturnToHost = kThunkBase + kThunkRegionSize - 0x10;
inline constexpr gaddr kReturnToHost64 = kThunkBase64 + kThunkRegionSize - 0x10;

}  // namespace recomp
