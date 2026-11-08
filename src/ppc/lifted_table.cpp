// Registers the lifter's function and frame tables (generated/table.cpp)
// with the runtime when the lifted module's library loads. Compiled into
// each lifted module by recomp_add_lifted_ppc (CMakeLists.txt), not into
// the runtime library.

#include "ppc/lifted.h"

namespace recomp::ppc {

extern const LiftedFrame kLiftedFrames[];
extern const uint32_t kLiftedFrameCount;
extern const LiftedSave kLiftedSaves[];

namespace {
const bool kRegistered =
    (RegisterLiftedFunctions(kLiftedFunctions, kLiftedFunctionCount, kLiftedTextHash),
     RegisterLiftedFrames(kLiftedFrames, kLiftedFrameCount, kLiftedSaves), true);
}  // namespace
}  // namespace recomp::ppc
