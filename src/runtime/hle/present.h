#pragma once
// Presents a finished frame of a w x h back buffer on `dev`, with the dev
// aids every graphics API shares (<ID>_SHOT_EVERY / _SHOT_FROM / _SHOT_DIR
// screenshots, <ID>_HITCH frame-time reports), then pumps window events
// and ticks the watchdog.

#include "runtime/gfx/device.h"

namespace recomp::hle {

void PresentFrame(gfx::Device& dev, int w, int h);

}  // namespace recomp::hle
