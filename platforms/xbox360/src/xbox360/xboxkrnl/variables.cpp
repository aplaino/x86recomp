// xboxkrnl: variable exports that describe the system.

#include <chrono>
#include <thread>

#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";

// XboxKrnlVersion: major, minor, build, QFE (be16 each), the version of the
// kernel the system modules here were built against (xam 2.0.17559.0
// imports xboxkrnl 2.0.17559.0).
void InitVersion(uint32_t va) {
  Wr16(va, 2);
  Wr16(va + 2, 0);
  Wr16(va + 4, 17559);
  Wr16(va + 6, 0);
}

// KeTimeStampBundle: interrupt time and system time (100 ns), tick count
// (ms), which guest code reads inline (GetTickCount); kept current every
// millisecond.
void Stamp(uint32_t va) {
  uint64_t interrupt = InterruptTime();
  Wr64(va, interrupt);
  Wr64(va + 8, SystemTime());
  Wr32(va + 16, uint32_t(interrupt / 10000));
}
void InitTimeStampBundle(uint32_t va) {
  Stamp(va);
  std::thread([va] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      Stamp(va);
    }
  }).detach();
}

}  // namespace

HLE_VARIABLE(kKrnl, 344, XboxKrnlVersion, 8, InitVersion);
HLE_VARIABLE(kKrnl, 173, KeTimeStampBundle, 0x18, InitTimeStampBundle);
// No debugger and no certification monitor: zero.
HLE_VARIABLE(kKrnl, 89, KeDebugMonitorData, 0x100, nullptr);
HLE_VARIABLE(kKrnl, 614, KeCertMonitorData, 0x100, nullptr);

}  // namespace recomp::xbox360
