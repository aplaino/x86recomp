#pragma once
// Presents a finished frame (gfx::PresentWithDevAids), then services
// the window system for the guest and ticks the watchdog.

#include "runtime/gfx/device.h"

namespace recomp::hle {

void PresentFrame(gfx::Device& dev, int w, int h);

}  // namespace recomp::hle
