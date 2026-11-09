#pragma once
// The console's video output on the host: a window and the GPU device on
// it, which the EDRAM shares. Each swap shows its front buffer (the
// texture VdSwap's fetch constant describes) through the base's
// presentation and its dev aids (gfx/present.h).

#include <mutex>

#include "runtime/gfx/device.h"

namespace recomp::xbox360 {

// At power-on, on the process's main thread: SDL video, the window and
// the device.
void StartDisplay();
// The device, and the lock every use of it holds: the command
// processor's resolves and the main thread's frames.
gfx::Device& HostDevice();
std::mutex& HostDeviceLock();

// A swap packet, from the command processor: the front buffer's texture
// fetch constant (its address physical), and the part of it to show.
void QueueFrame(const uint32_t fetch[6], uint32_t width, uint32_t height);

// The main thread's work once the title runs: the window's events and the
// frames swaps queue. The title ends the process.
[[noreturn]] void RunDisplay();

}  // namespace recomp::xbox360
