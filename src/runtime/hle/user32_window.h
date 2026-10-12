#pragma once
// Window state shared between user32_window.cpp and other HLE modules
// (ddraw needs the SDL window behind an HWND).

#include <SDL3/SDL.h>

#include <cstdint>

namespace recomp {

// <ID>_BACKGROUND=1 (used for automated test runs): never move or hide the
// host pointer, don't take focus when the window appears, and answer
// message boxes with their default button.
bool Background();

// The SDL window behind a top-level HWND, or nullptr.
SDL_Window* SdlWindowFor(uint32_t hwnd);

// HDCs: GetDC(hwnd) returns DcForWindow(hwnd); WindowForDc maps it back
// (0 for the screen DC or anything else).
uint32_t DcForWindow(uint32_t hwnd);
uint32_t WindowForDc(uint32_t hdc);

// Ensures SDL video is initialised (main thread only).
void EnsureSdlVideo();

// Pumps SDL events into the guest message queues. Main thread only;
// called from PeekMessage/GetMessage and long waits.
void PumpHostEvents();

// DirectDraw exclusive mode on hwnd: the SDL window's client area is the
// display mode (w x h) and the game's own window placement (MoveWindow,
// minimize/restore on deactivation) is bookkeeping only, as it would be
// behind a fullscreen exclusive surface on Windows. w = 0 ends it.
void SetExclusiveMode(uint32_t hwnd, int w, int h);

}  // namespace recomp
