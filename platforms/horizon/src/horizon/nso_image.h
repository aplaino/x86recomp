#pragma once
// Executable modules as the console's loader sees them (a64/image.h's
// Module): the segments at their offsets, .bss zeroed, and what MOD0's
// .dynamic points at. Shared by the lifter's reader and the runtime loader
// so both see the same bytes at the same addresses. NRO (homebrew) only for
// now; NSO (LZ4 segments) joins it with the first game.

#include <string>

#include "a64/image.h"

namespace recomp::horizon {

// Reads an NRO file. On failure returns false with a message in `err`.
bool LoadNro(const std::string& path, a64::Module& m, std::string& err);

}  // namespace recomp::horizon
