#pragma once
// Host-side keyboard/mouse state shared between the SDL event pump and the
// user32 HLE (GetKeyState, GetAsyncKeyState, GetCursorPos).

#include <atomic>
#include <cstdint>

namespace recomp {

extern std::atomic<uint8_t> g_key_down[256];
extern std::atomic<uint8_t> g_key_toggled[256];
extern std::atomic<uint8_t> g_key_pressed_since[256];

// Desktop size reported to the game (GetSystemMetrics, EnumDisplaySettings).
// Set from the primary display once SDL is up; the default is a safe
// 2002-era desktop.
inline int g_display_w = 1024;
inline int g_display_h = 768;

// Set-1 scan code for a virtual key (US layout); *ext = E0-prefixed.
uint8_t ScanCodeForVk(uint8_t vk, bool* ext);

}  // namespace recomp
