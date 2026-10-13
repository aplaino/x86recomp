// Backend selection for gfx::Device.

#include "runtime/gfx/device.h"

namespace recomp::gfx {

std::unique_ptr<Device> CreateDevice(Backend b, SDL_Window* window) {
  switch (b) {
    case Backend::D3D9:
#ifdef RECOMP_HAVE_D3D9
      return CreateD3D9Device(window);
#else
      return nullptr;
#endif
    case Backend::Gpu:
      return CreateGpuDevice(window);
  }
  return nullptr;
}

}  // namespace recomp::gfx
