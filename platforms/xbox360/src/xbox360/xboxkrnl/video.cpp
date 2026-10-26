// xboxkrnl: the display (Vd*) and the AV pack.

#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";

// The Xenos runs at 500 MHz.
void InitGpuClock(uint32_t va) { Wr32(va, 500); }
// A critical section, with Xenia's spin count.
void InitHsioCalibrationLock(uint32_t va) { InitCriticalSection(va, 10000); }

}  // namespace

// The title's D3D device: titles store it here and read it back.
HLE_VARIABLE(kKrnl, 446, VdGlobalDevice, 4, nullptr);
HLE_VARIABLE(kKrnl, 448, VdGpuClockInMHz, 4, InitGpuClock);
HLE_VARIABLE(kKrnl, 449, VdHSIOCalibrationLock, kCriticalSectionSize, InitHsioCalibrationLock);

HLE(kKrnl, 34, HalGetCurrentAVPack) { return AvPack(); }

// VdQueryVideoMode(*mode).
HLE(kKrnl, 458, VdQueryVideoMode) {
  WriteVideoMode(Arg(c, 0));
  return 0;
}

}  // namespace recomp::xbox360
