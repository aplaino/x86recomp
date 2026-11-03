#pragma once
// libScePad's HLE (recomp_orbis_pad): the controller for the system's own
// use.

#include <cstdint>

namespace recomp::orbis::pad {

// Button bits of ScePadData's buttons.
enum : uint32_t {
  kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80,
  kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000,
  kCross = 0x4000, kSquare = 0x8000, kTouchPad = 0x100000, kIntercepted = 0x80000000,
};

// While the system has the controller (a system dialog is open), the
// game reads it intercepted: kIntercepted alone, sticks centred.
void SetIntercepted(bool on);

// The controller's buttons now, for the system (any thread).
uint32_t Buttons();

}  // namespace recomp::orbis::pad
