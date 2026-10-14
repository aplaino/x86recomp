#pragma once
// What game patches may ask about the host screen, for resolution and
// widescreen fixes (ARCHITECTURE.md, "Display modes, fullscreen and
// presentation"). Where the last frame was shown (its rectangle, the
// output's size and so its aspect): gfx::LastPresentation().

#include "runtime/gfx/device.h"

namespace recomp {

// The host desktop's size, in the units of the display modes games see
// (screen coordinates: points on macOS and on scaled Wayland outputs).
// Does not change when the game switches modes. Main thread; brings up
// SDL video if needed.
void DesktopSize(int* w, int* h);

}  // namespace recomp
