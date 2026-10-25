// Registers the lifter's function table (generated/table.cpp) with the
// runtime before main(). Compiled into each game's lifted code by
// recomp_add_lifted_a64 (CMakeLists.txt), not into the runtime library.

#include "a64/lifted.h"

namespace recomp::a64 {
namespace {
const bool kRegistered =
    (RegisterLiftedFunctions(kLiftedFunctions, kLiftedFunctionCount, kLiftedTextHash), true);
}  // namespace
}  // namespace recomp::a64
