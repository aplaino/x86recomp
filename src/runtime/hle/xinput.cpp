// XInput 1.3 (xinput1_3.dll, by ordinal) on SDL gamepads. Up to four pads
// take the user indices in the order they connect and keep them until
// they disconnect. SDL's gamepad layout is the Xbox controller's (south =
// A, east = B, west = X, north = Y); stick Y is up in XInput and down in
// SDL. Automated runs (<ID>_BACKGROUND) see no pads, as they take no host
// input, unless they ask for the virtual test pad (<ID>_TEST_PAD). The gamepad subsystem starts on the main thread, where SDL's
// events are pumped (callers on other threads see no pads until then).
// DirectInput and WMI list the same pads (xinput.h).

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <cstring>
#include <mutex>

#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/xinput.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

constexpr uint32_t ERROR_SUCCESS = 0, ERROR_BAD_ARGUMENTS = 160,
                   ERROR_DEVICE_NOT_CONNECTED = 1167;

struct Pad {
  SDL_Gamepad* gp = nullptr;
  uint32_t packet = 0;
  uint8_t last[12] = {};  // XINPUT_GAMEPAD last reported
};

std::mutex g_mu;
bool g_up = false;
Pad g_pads[4];
SDL_Joystick* g_test_pad = nullptr;  // <ID>_TEST_PAD's virtual gamepad

bool TestPadWanted() {
  static const bool on = [] {
    const char* e = Env("TEST_PAD");
    return e && *e && *e != '0';
  }();
  return on;
}

// Opens newly connected gamepads into free slots, frees disconnected ones.
void Refresh() {
  if (!g_up) {
    if (!OnMainThread() || (Background() && !TestPadWanted())) return;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
      WarnF("xinput: SDL gamepad init failed: %s\n", SDL_GetError());
      return;
    }
    g_up = true;
#if defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV)
    // GameController finds paired controllers only after launch: a moment
    // for it (pumping its run loop), so that the game's first enumeration
    // already sees a pad that is on.
    for (int i = 0; i < 50 && !SDL_HasGamepad(); i++) {
      SDL_PumpEvents();
      SDL_Delay(20);
    }
#endif
    if (TestPadWanted()) {
      SDL_VirtualJoystickDesc d;
      SDL_INIT_INTERFACE(&d);
      d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
      d.naxes = SDL_GAMEPAD_AXIS_COUNT;
      d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
      d.name = "Test Pad";
      const SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
      g_test_pad = id ? SDL_OpenJoystick(id) : nullptr;
      if (!g_test_pad) WarnF("xinput: test pad: %s\n", SDL_GetError());
      else std::fprintf(stderr, "xinput: test pad attached\n");
    }
  }
  for (Pad& p : g_pads) {
    if (p.gp && !SDL_GamepadConnected(p.gp)) {
      std::fprintf(stderr, "xinput: pad %d disconnected\n", int(&p - g_pads));
      SDL_CloseGamepad(p.gp);
      p = Pad{};
    }
  }
  // The pads move down into a freed slot: a game can read only the first
  // (an iPad's touch controls leave it when a real pad connects).
  int to = 0;
  for (Pad& p : g_pads) {
    if (!p.gp) continue;
    if (&p != &g_pads[to]) {
      g_pads[to] = p;
      p = Pad{};
      std::fprintf(stderr, "xinput: pad %d: %s\n", to, SDL_GetGamepadName(g_pads[to].gp));
    }
    to++;
  }
  int n = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&n);
  {  // Joysticks SDL has no gamepad mapping for are not pads here: say so.
    static int last_j = -1, last_g = -1;
    int nj = 0;
    SDL_free(SDL_GetJoysticks(&nj));
    if (nj != last_j || n != last_g)
      std::fprintf(stderr, "xinput: %d joystick(s), %d of them gamepads\n", nj, n);
    last_j = nj;
    last_g = n;
  }
  for (int i = 0; ids && i < n; i++) {
    // Automated runs take no host input: only the test pad.
    if (Background() && !(g_test_pad && SDL_GetJoystickID(g_test_pad) == ids[i])) continue;
    bool open = false;
    for (const Pad& p : g_pads)
      if (p.gp && SDL_GetGamepadID(p.gp) == ids[i]) open = true;
    if (open) continue;
    for (Pad& p : g_pads) {
      if (p.gp) continue;
      p.gp = SDL_OpenGamepad(ids[i]);
      if (p.gp) {
        const char* name = SDL_GetGamepadName(p.gp);
        std::fprintf(stderr, "xinput: pad %d: %s\n", int(&p - g_pads), name ? name : "?");
      } else {
        WarnF("xinput: opening gamepad %u: %s\n", unsigned(ids[i]), SDL_GetError());
      }
      break;
    }
  }
  SDL_free(ids);
}

// The pad at `index`, refreshed; null if none.
Pad* PadAt(uint32_t index) {
  if (index >= 4) return nullptr;
  Refresh();
  return g_pads[index].gp ? &g_pads[index] : nullptr;
}

int16_t Stick(SDL_Gamepad* gp, SDL_GamepadAxis a, bool flip) {
  int v = SDL_GetGamepadAxis(gp, a);
  if (flip) v = -v;
  return int16_t(std::clamp(v, -32768, 32767));
}

uint8_t Trigger(SDL_Gamepad* gp, SDL_GamepadAxis a) {
  int v = SDL_GetGamepadAxis(gp, a);
  return uint8_t(std::clamp(v, 0, 32767) * 255 / 32767);
}

// XINPUT_GAMEPAD: wButtons, bLeftTrigger, bRightTrigger, sThumbLX/LY/RX/RY.
void ReadGamepad(SDL_Gamepad* gp, uint8_t out[12]) {
  static const std::pair<SDL_GamepadButton, uint16_t> kButtons[] = {
      {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
      {SDL_GAMEPAD_BUTTON_START, 0x0010},       {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
      {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},       {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
      {SDL_GAMEPAD_BUTTON_WEST, 0x4000},        {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
  };
  uint16_t buttons = 0;
  for (const auto& [b, bit] : kButtons)
    if (SDL_GetGamepadButton(gp, b)) buttons |= bit;
  int16_t sticks[4] = {Stick(gp, SDL_GAMEPAD_AXIS_LEFTX, false),
                       Stick(gp, SDL_GAMEPAD_AXIS_LEFTY, true),
                       Stick(gp, SDL_GAMEPAD_AXIS_RIGHTX, false),
                       Stick(gp, SDL_GAMEPAD_AXIS_RIGHTY, true)};
  std::memcpy(out, &buttons, 2);
  out[2] = Trigger(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
  out[3] = Trigger(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
  std::memcpy(out + 4, sticks, 8);
}

// XInputGetState(dwUserIndex, pState): XINPUT_STATE {dwPacketNumber,
// XINPUT_GAMEPAD}; the packet number changes when the state does.
HLE_NAMED("xinput1_3.dll", "#2", XInputGetState, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  Pad* p = PadAt(Arg(c, 0));
  if (!p) return ERROR_DEVICE_NOT_CONNECTED;
  if (!Arg(c, 1)) return ERROR_BAD_ARGUMENTS;
  uint8_t g[12];
  ReadGamepad(p->gp, g);
  if (std::memcmp(g, p->last, sizeof g) != 0) {
    std::memcpy(p->last, g, sizeof g);
    p->packet++;
  }
  Wr32(Arg(c, 1), p->packet);
  std::memcpy(GPtr(Arg(c, 1) + 4), g, sizeof g);
  return ERROR_SUCCESS;
}

// XInputSetState(dwUserIndex, pVibration): XINPUT_VIBRATION
// {wLeftMotorSpeed, wRightMotorSpeed}; held until changed.
HLE_NAMED("xinput1_3.dll", "#3", XInputSetState, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  Pad* p = PadAt(Arg(c, 0));
  if (!p) return ERROR_DEVICE_NOT_CONNECTED;
  if (!Arg(c, 1)) return ERROR_BAD_ARGUMENTS;
  uint32_t v = Rd32(Arg(c, 1));
  SDL_RumbleGamepad(p->gp, uint16_t(v), uint16_t(v >> 16), 0xFFFFFFFF);
  return ERROR_SUCCESS;
}

// XInputGetCapabilities(dwUserIndex, dwFlags, pCapabilities):
// XINPUT_CAPABILITIES {Type, SubType, Flags, Gamepad, Vibration}: a
// gamepad with every control and both motors.
HLE_NAMED("xinput1_3.dll", "#4", XInputGetCapabilities, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  Pad* p = PadAt(Arg(c, 0));
  if (!p) return ERROR_DEVICE_NOT_CONNECTED;
  uint32_t out = Arg(c, 2);
  if (!out || Arg(c, 1) > 1) return ERROR_BAD_ARGUMENTS;
  uint8_t caps[20] = {};
  caps[0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
  caps[1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
  const uint16_t buttons = 0xF3FF;
  std::memcpy(caps + 4, &buttons, 2);
  caps[6] = caps[7] = 0xFF;
  std::memset(caps + 8, 0xFF, 8);    // stick ranges
  std::memset(caps + 16, 0xFF, 4);   // motors
  std::memcpy(GPtr(out), caps, sizeof caps);
  return ERROR_SUCCESS;
}

}  // namespace

bool ReadPad(uint32_t index, PadState* out) {
  std::lock_guard<std::mutex> l(g_mu);
  Pad* p = PadAt(index);
  if (!p) return false;
  for (int a = 0; a < 6; a++)
    out->axis[a] = SDL_GetGamepadAxis(p->gp, SDL_GamepadAxis(a));
  for (int b = 0; b < 32; b++)
    out->button[b] = b < SDL_GAMEPAD_BUTTON_COUNT && SDL_GetGamepadButton(p->gp, SDL_GamepadButton(b));
  return true;
}

void RumblePad(uint32_t index, uint16_t low, uint16_t high, uint32_t ms) {
  std::lock_guard<std::mutex> l(g_mu);
  if (Pad* p = PadAt(index)) SDL_RumbleGamepad(p->gp, low, high, ms);
}

void TestPadButton(int button, bool down) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_test_pad) SDL_SetJoystickVirtualButton(g_test_pad, button, down);
}

void TestPadAxis(int axis, int16_t value) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_test_pad) SDL_SetJoystickVirtualAxis(g_test_pad, axis, value);
}

// The runtime's live input drives the test pad through these.
const bool g_test_pad_hooks = [] {
  SetTestPadHooks({&TestPadButton, &TestPadAxis});
  return true;
}();

std::vector<XInputPad> XInputPads() {
  std::lock_guard<std::mutex> l(g_mu);
  Refresh();
  std::vector<XInputPad> pads;
  for (uint32_t i = 0; i < 4; i++) {
    SDL_Gamepad* gp = g_pads[i].gp;
    if (!gp) continue;
    const char* name = SDL_GetGamepadName(gp);
    const SDL_GamepadType type = SDL_GetGamepadType(gp);
    pads.push_back({i, SDL_GetGamepadVendor(gp), SDL_GetGamepadProduct(gp),
                    name ? name : "Controller",
                    type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 ||
                        type == SDL_GAMEPAD_TYPE_PS5});
  }
  return pads;
}

// The same handlers by name, for GetProcAddress(xinput1_3.dll, "XInputGetState").
static Registrar name_XInputGetState{"xinput1_3.dll", "XInputGetState", 2, &hle_XInputGetState};
static Registrar name_XInputSetState{"xinput1_3.dll", "XInputSetState", 2, &hle_XInputSetState};
static Registrar name_XInputGetCapabilities{"xinput1_3.dll", "XInputGetCapabilities", 3, &hle_XInputGetCapabilities};

}  // namespace recomp::hle
