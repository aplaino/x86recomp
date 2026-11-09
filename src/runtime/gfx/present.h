#pragma once
// What every graphics front end does with a finished frame and with its
// device: presents a w x h back buffer on `dev` with the dev aids they
// all share (<ID>_SHOT_EVERY / _SHOT_FROM / _SHOT_DIR screenshots,
// <ID>_HITCH long-frame and <ID>_FPS_REPORT frame-rate reports), and
// picks the backend (<ID>_GFX). The caller then services the window
// system and its core's watchdog (x86/present.h).

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>

#include "runtime/gfx/device.h"

namespace recomp::gfx {

void PresentWithDevAids(Device& dev, int w, int h);

// The backend every graphics front end uses: <ID>_GFX=d3d9 or gpu; by
// default SDL_GPU.
Backend ChosenBackend();
// SDL_CreateWindow flags the backend needs on top-level windows.
uint64_t ChosenWindowFlags();
// Creates the device for `w` on ChosenBackend(); fatal if that fails.
std::unique_ptr<Device> CreateChosenDevice(SDL_Window* w);

}  // namespace recomp::gfx
