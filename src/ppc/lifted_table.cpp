// Registers the lifter's function table (generated/table.cpp) with the
// runtime before main(). Compiled into each game's lifted code by
// recomp_add_lifted_ppc (CMakeLists.txt), not into the runtime library.

#include "ppc/lifted.h"

namespace recomp::ppc {
namespace {
const bool kRegistered =
    (RegisterLiftedFunctions(kLiftedFunctions, kLiftedFunctionCount, kLiftedTextHash), true);
}  // namespace
}  // namespace recomp::ppc
