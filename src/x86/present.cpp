// Frame presentation shared by the graphics APIs (Win32's ddraw, opengl32,
// a console platform's video output).

#include "x86/present.h"

#include "runtime/gfx/present.h"
#include "x86/runtime.h"
#include "x86/watchdog.h"

namespace recomp::hle {

void PresentFrame(gfx::Device& dev, int w, int h) {
  gfx::PresentWithDevAids(dev, w, h);
  // Loading screens present without pumping messages; keep answering the
  // window system (compositor pings) so the window does not look hung.
  // Events are only queued for the game, as Windows would.
  if (g_proc->pump_events) g_proc->pump_events();
  WatchdogFrame();
}

}  // namespace recomp::hle
