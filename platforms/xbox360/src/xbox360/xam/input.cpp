// xam: controllers (XamInput*) on the host's SDL gamepads. Up to four pads
// take the users in the order they connect and keep them until they
// disconnect, as a console gives the first controller player 1. SDL's
// layout is the controller's (south = A, east = B, west = X, north = Y);
// stick Y is up on the console and down in SDL. Automated runs
// (<ID>_BACKGROUND) see no pads, as they take no host input.
//
// The pads are read on a host thread of their own, which starts SDL's
// gamepads and updates them (SDL wants both on one thread) every few
// milliseconds, and raises XN_SYS_INPUTDEVICESCHANGED when the users with
// a pad change. It starts with the first call here or the first
// notification listener; the pads it finds then were there before the
// title, and raise nothing.

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "runtime/log.h"
#include "xbox360/hle.h"
#include "xbox360/process.h"
#include "xbox360/xam/input.h"
#include "xbox360/xam/notify.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorDeviceNotConnected = 1167, kErrorFunctionFailed = 1627;
constexpr uint32_t kUsers = 4;
constexpr auto kUpdatePeriod = std::chrono::milliseconds(4);

// XINPUT_FLAG_*: the gamepad (1), the keyboard (2); 8 asks for the
// gamepad's other device context (type 3), 0x10 and 0x20 for devices of
// types 1 and 4 (xam's code).
constexpr uint32_t kFlagGamepad = 0x01, kFlagKeyboard = 0x02, kFlagContext3 = 0x08, kFlagType1 = 0x10,
                   kFlagType4 = 0x20;

struct Pad {
  SDL_Gamepad* gp = nullptr;
  uint32_t packet = 0;
  uint8_t last[12] = {};  // XINPUT_GAMEPAD last reported
};

std::mutex g_lock;
Pad g_pads[kUsers];
std::once_flag g_start;

// Frees the pads that disconnected and gives new ones the free users; the
// mask of users with a pad. Under g_lock.
uint32_t Refresh() {
  SDL_UpdateGamepads();
  for (uint32_t u = 0; u < kUsers; u++)
    if (g_pads[u].gp && !SDL_GamepadConnected(g_pads[u].gp)) {
      SDL_CloseGamepad(g_pads[u].gp);
      g_pads[u] = Pad{};
    }
  int n = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&n);
  for (int i = 0; ids && i < n; i++) {
    bool open = false;
    for (const Pad& p : g_pads)
      if (p.gp && SDL_GetGamepadID(p.gp) == ids[i]) open = true;
    for (uint32_t u = 0; u < kUsers && !open; u++)
      if (!g_pads[u].gp) {
        g_pads[u].gp = SDL_OpenGamepad(ids[i]);
        if (g_pads[u].gp) SDL_SetGamepadPlayerIndex(g_pads[u].gp, int(u));
        open = true;
      }
  }
  SDL_free(ids);
  uint32_t mask = 0;
  for (uint32_t u = 0; u < kUsers; u++)
    if (g_pads[u].gp) mask |= 1u << u;
  return mask;
}

void Reader(std::mutex& m, std::condition_variable& cv, bool& ready) {
  auto started = [&] {
    std::lock_guard<std::mutex> g(m);
    ready = true;
    cv.notify_one();
  };
  // As for audio: SDL's SIGINT and SIGTERM handlers would only queue a
  // quit event nothing reads.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    WarnF("xbox360 input: SDL gamepad init failed: %s; no controllers\n", SDL_GetError());
    started();
    return;
  }
  // Nothing reads SDL's event queue: the pads are updated here.
  SDL_SetJoystickEventsEnabled(false);
  SDL_SetGamepadEventsEnabled(false);
  uint32_t mask;
  {
    std::lock_guard<std::mutex> g(g_lock);
    mask = Refresh();
  }
  started();
  for (;;) {
    std::this_thread::sleep_for(kUpdatePeriod);
    uint32_t now;
    {
      std::lock_guard<std::mutex> g(g_lock);
      now = Refresh();
    }
    // xam raises it with 0 whenever the mask of users with a gamepad
    // changes (xam's code).
    if (now != mask) Broadcast(kXnSysInputDevicesChanged, 0);
    mask = now;
  }
}

// The pad of `user`'s gamepad context (xam's type 0; with `flags` asking
// for type 3, none: a console check); nullptr if none. Under g_lock.
Pad* PadOf(uint32_t user, uint32_t flags) {
  if (user >= kUsers || flags & kFlagContext3) return nullptr;
  return g_pads[user].gp ? &g_pads[user] : nullptr;
}

void Wr16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

int16_t Stick(SDL_Gamepad* gp, SDL_GamepadAxis a, bool flip) {
  int v = SDL_GetGamepadAxis(gp, a);
  return int16_t(std::clamp(flip ? -v : v, -32768, 32767));
}

uint8_t Trigger(SDL_Gamepad* gp, SDL_GamepadAxis a) {
  return uint8_t(std::clamp<int>(SDL_GetGamepadAxis(gp, a), 0, 32767) * 255 / 32767);
}

// XINPUT_GAMEPAD (big-endian): buttons, left and right trigger, left
// stick X and Y, right stick X and Y. The guide button (0x0400) stays
// clear: xam clears it for titles.
void ReadGamepad(SDL_Gamepad* gp, uint8_t out[12]) {
  static const std::pair<SDL_GamepadButton, uint16_t> kButtons[] = {
      {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},     {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
      {SDL_GAMEPAD_BUTTON_START, 0x0010},         {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
      {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},         {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
      {SDL_GAMEPAD_BUTTON_WEST, 0x4000},          {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
  };
  uint16_t buttons = 0;
  for (const auto& [b, bit] : kButtons)
    if (SDL_GetGamepadButton(gp, b)) buttons |= bit;
  Wr16(out, buttons);
  out[2] = Trigger(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
  out[3] = Trigger(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
  Wr16(out + 4, uint16_t(Stick(gp, SDL_GAMEPAD_AXIS_LEFTX, false)));
  Wr16(out + 6, uint16_t(Stick(gp, SDL_GAMEPAD_AXIS_LEFTY, true)));
  Wr16(out + 8, uint16_t(Stick(gp, SDL_GAMEPAD_AXIS_RIGHTX, false)));
  Wr16(out + 10, uint16_t(Stick(gp, SDL_GAMEPAD_AXIS_RIGHTY, true)));
}

}  // namespace

void StartControllers() {
  std::call_once(g_start, [] {
    if (Background()) return;
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    std::thread(Reader, std::ref(m), std::ref(cv), std::ref(ready)).detach();
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return ready; });
  });
}

// XamInputGetCapabilities(user, flags, *capabilities): XINPUT_CAPABILITIES
// (0x14 bytes) zeroed, then filled from the device. Flags 0 mean the
// gamepad and the keyboard. With none of the flags' devices,
// ERROR_DEVICE_NOT_CONNECTED; with flags that name no device type,
// ERROR_FUNCTION_FAILED (xam's code). There is no keyboard (xam asks the
// HID driver for one). A gamepad reports every control and both motors,
// as Xenia has it (console check).
HLE(kXam, 400, XamInputGetCapabilities) {
  uint32_t user = Arg(c, 0), flags = Arg(c, 1) ? Arg(c, 1) : kFlagGamepad | kFlagKeyboard;
  uint8_t* out = Mem() + Arg(c, 2);
  std::memset(out, 0, 0x14);
  StartControllers();
  std::lock_guard<std::mutex> g(g_lock);
  if (flags & (kFlagGamepad | kFlagContext3) && PadOf(user, flags)) {
    out[0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
    out[1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    Wr16(out + 4, 0xF3FF);
    std::memset(out + 6, 0xFF, 14);  // triggers, sticks, motors
    return 0;
  }
  constexpr uint32_t kDevices = kFlagGamepad | kFlagKeyboard | kFlagContext3 | kFlagType1 | kFlagType4;
  return flags & kDevices ? kErrorDeviceNotConnected : kErrorFunctionFailed;
}

// XamInputGetState(user, flags, *state): XINPUT_STATE (packet number, then
// the gamepad: 16 bytes) zeroed first; ERROR_DEVICE_NOT_CONNECTED when the
// user has no gamepad (xam's code). The packet number changes when the
// state does, as Xenia has it (console check).
HLE(kXam, 401, XamInputGetState) {
  uint32_t user = Arg(c, 0), flags = Arg(c, 1), state = Arg(c, 2);
  std::memset(Mem() + state, 0, 0x10);
  StartControllers();
  std::lock_guard<std::mutex> g(g_lock);
  Pad* p = PadOf(user, flags);
  if (!p) return kErrorDeviceNotConnected;
  uint8_t pad[12];
  ReadGamepad(p->gp, pad);
  if (std::memcmp(pad, p->last, sizeof pad) != 0) {
    std::memcpy(p->last, pad, sizeof pad);
    p->packet++;
  }
  Wr32(state, p->packet);
  std::memcpy(Mem() + state + 4, pad, sizeof pad);
  return 0;
}

// XamInputSetState(user, flags, *vibration): XINPUT_VIBRATION (the left,
// low-frequency motor's speed, then the right's), held until changed; the
// flags are not used. ERROR_DEVICE_NOT_CONNECTED when the user has no
// gamepad (xam's code).
HLE(kXam, 402, XamInputSetState) {
  uint32_t user = Arg(c, 0), vibration = Arg(c, 2);
  StartControllers();
  std::lock_guard<std::mutex> g(g_lock);
  Pad* p = PadOf(user, 0);
  if (!p) return kErrorDeviceNotConnected;
  SDL_RumbleGamepad(p->gp, Rd16(vibration), Rd16(vibration + 2), 0xFFFFFFFF);
  return 0;
}

}  // namespace recomp::xbox360
