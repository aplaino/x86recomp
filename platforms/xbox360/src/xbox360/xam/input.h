#pragma once
// xam's controllers (input.cpp), for xam's own code that needs them read.

namespace recomp::xbox360 {

// Starts reading the host's controllers, once, unless the run is
// automated (Background()); returns when the pads already connected are
// known.
void StartControllers();

}  // namespace recomp::xbox360
