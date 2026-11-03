// libScePad HLE: the user's controller from the host's first SDL gamepad,
// else the keyboard (arrows, Z/X/A/S for cross/circle/square/triangle,
// Q/W L1/R1, 1/2 L2/R2, Enter options). It is always connected, as a
// DualShock 4 is to its user; <ID>_BACKGROUND=1 runs see it at rest, or
// pressed as <ID>_TEST_PAD's file says (Script below). The motion
// sensors report the controller lying still. While a system dialog has
// it (pad.h), the game reads it intercepted. ScePadData's layout,
// the argument checks and codes are libScePad 11.00's; one state is read
// per call (the newest).

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "orbis/hle/pad.h"

#include "orbis/hle.h"
#include "orbis/host.h"
#include "x86/runtime.h"

namespace {

using recomp::gaddr;
using namespace recomp::orbis::pad;

constexpr uint32_t kErrInvalidArg = 0x80920001, kErrInvalidHandle = 0x80920003,
                   kErrAlreadyOpened = 0x80920004, kErrNotInitialized = 0x80920005;
constexpr int32_t kHandle = 1;  // the user's standard controller
constexpr int32_t kUser = 0x10000000;  // the one user (libSceUserService)
constexpr size_t kDataSize = 0x80;

std::mutex g_mu;
bool g_init = false, g_open = false;
bool g_intercepted = false;
SDL_Gamepad* g_pad = nullptr;  // host thread
const auto g_start = std::chrono::steady_clock::now();

void Sink(const SDL_Event& e) {
  if (e.type == SDL_EVENT_GAMEPAD_ADDED && !g_pad) g_pad = SDL_OpenGamepad(e.gdevice.which);
  if (e.type == SDL_EVENT_GAMEPAD_REMOVED && g_pad &&
      SDL_GetGamepadID(g_pad) == e.gdevice.which) {
    SDL_CloseGamepad(g_pad);
    g_pad = nullptr;
  }
}

struct State {
  uint32_t buttons = 0;
  uint8_t stick[4] = {0x80, 0x80, 0x80, 0x80};
  uint8_t trigger[2] = {0, 0};
};

// <ID>_TEST_PAD=<file>: background runs' presses, one per line:
// "<seconds> <button>...", each held 0.2 s from that many seconds after
// scePadInit; buttons cross, circle, square, triangle, up, down, left,
// right, l1, r1, options, touchpad; '#' starts a comment. Also on a line:
// lx=, ly=, rx=, ry= a stick's axis, -1 (left, up) to 1 (right, down);
// for=<seconds> holds the line that long. The file is read as it grows:
// "+<seconds>" times a line appended during the run from when it is read.
struct Press {
  double t, hold = 0.2;
  uint32_t buttons = 0;
  int8_t axis[4] = {};  // which sticks' axes the line sets
  uint8_t stick[4] = {};
};

Press ParseLine(char* line, double now) {
  char* save = nullptr;
  char* tok = strtok_r(line, " \t\r\n", &save);
  Press p{tok[0] == '+' ? now + std::atof(tok + 1) : std::atof(tok)};
  static const struct {
    const char* name;
    uint32_t bit;
  } kNames[] = {
      {"cross", kCross}, {"circle", kCircle}, {"square", kSquare}, {"triangle", kTriangle},
      {"up", kUp},       {"down", kDown},     {"left", kLeft},     {"right", kRight},
      {"l1", kL1},       {"r1", kR1},         {"options", kOptions}, {"touchpad", kTouchPad},
  };
  static const char* const kAxes[4] = {"lx=", "ly=", "rx=", "ry="};
  while ((tok = strtok_r(nullptr, " \t\r\n", &save))) {
    bool known = false;
    for (int i = 0; i < 4; i++)
      if (!std::strncmp(tok, kAxes[i], 3)) {
        const double v = std::atof(tok + 3);
        if (v < -1 || v > 1) recomp::FatalF("TEST_PAD: %s is outside -1 to 1", tok);
        p.axis[i] = 1;
        p.stick[i] = uint8_t(std::lround(127.5 + v * 127.5));
        known = true;
      }
    if (!std::strncmp(tok, "for=", 4)) {
      p.hold = std::atof(tok + 4);
      known = true;
    }
    for (const auto& n : kNames)
      if (!std::strcmp(tok, n.name)) {
        p.buttons |= n.bit;
        known = true;
      }
    if (!known) recomp::FatalF("TEST_PAD: unknown button %s", tok);
  }
  return p;
}

State Script() {
  static const auto t0 = std::chrono::steady_clock::now();
  static FILE* f = [] {
    const char* path = recomp::Env("TEST_PAD");
    FILE* f = path ? std::fopen(path, "r") : nullptr;
    if (path && !f) recomp::FatalF("TEST_PAD: cannot open %s", path);
    return f;
  }();
  static std::vector<Press> presses;
  static std::string partial;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  char buf[256];
  while (f && std::fgets(buf, sizeof buf, f)) {
    partial += buf;
    if (partial.back() != '\n') continue;  // the rest is still being written
    if (size_t h = partial.find('#'); h != std::string::npos) partial.resize(h);
    if (partial.find_first_not_of(" \t\r\n") != std::string::npos)
      presses.push_back(ParseLine(partial.data(), t));
    partial.clear();
  }
  if (f) std::clearerr(f);  // past the end: read what is appended later
  State s;
  for (const Press& p : presses)
    if (t >= p.t && t < p.t + p.hold) {
      s.buttons |= p.buttons;
      for (int i = 0; i < 4; i++)
        if (p.axis[i]) s.stick[i] = p.stick[i];
    }
  return s;
}

// The controller now (host thread).
State Poll() {
  State s;
  if (recomp::Background()) return Script();
  if (g_pad) {
    static const struct {
      SDL_GamepadButton b;
      uint32_t bit;
    } kButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, kCross},        {SDL_GAMEPAD_BUTTON_EAST, kCircle},
        {SDL_GAMEPAD_BUTTON_WEST, kSquare},        {SDL_GAMEPAD_BUTTON_NORTH, kTriangle},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL1},   {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR1},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, kL3},      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kR3},
        {SDL_GAMEPAD_BUTTON_START, kOptions},      {SDL_GAMEPAD_BUTTON_TOUCHPAD, kTouchPad},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},         {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},     {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
    };
    for (const auto& k : kButtons)
      if (SDL_GetGamepadButton(g_pad, k.b)) s.buttons |= k.bit;
    const SDL_GamepadAxis axes[4] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY,
                                     SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY};
    for (int i = 0; i < 4; i++) s.stick[i] = uint8_t((SDL_GetGamepadAxis(g_pad, axes[i]) + 32768) >> 8);
    s.trigger[0] = uint8_t(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    s.trigger[1] = uint8_t(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    if (s.trigger[0]) s.buttons |= kL2;
    if (s.trigger[1]) s.buttons |= kR2;
    return s;
  }
  if (!SDL_WasInit(SDL_INIT_VIDEO)) return s;  // no window, no keyboard
  const bool* k = SDL_GetKeyboardState(nullptr);
  static const struct {
    SDL_Scancode key;
    uint32_t bit;
  } kKeys[] = {
      {SDL_SCANCODE_Z, kCross},      {SDL_SCANCODE_X, kCircle}, {SDL_SCANCODE_A, kSquare},
      {SDL_SCANCODE_S, kTriangle},   {SDL_SCANCODE_Q, kL1},     {SDL_SCANCODE_W, kR1},
      {SDL_SCANCODE_1, kL2},         {SDL_SCANCODE_2, kR2},     {SDL_SCANCODE_RETURN, kOptions},
      {SDL_SCANCODE_UP, kUp},        {SDL_SCANCODE_DOWN, kDown}, {SDL_SCANCODE_LEFT, kLeft},
      {SDL_SCANCODE_RIGHT, kRight},
  };
  for (const auto& m : kKeys)
    if (k[m.key]) s.buttons |= m.bit;
  if (s.buttons & kL2) s.trigger[0] = 0xFF;
  if (s.buttons & kR2) s.trigger[1] = 0xFF;
  return s;
}

void PutF(gaddr p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  recomp::Wr32(p, v);
}

void PutData(gaddr d, State s) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_intercepted) s = State{kIntercepted};
  }
  std::memset(recomp::GPtr(d), 0, kDataSize);
  recomp::Wr32(d, s.buttons);
  std::memcpy(recomp::GPtr(d + 4), s.stick, 4);
  std::memcpy(recomp::GPtr(d + 8), s.trigger, 2);
  PutF(d + 0x18, 1.0f);  // the orientation quaternion's w
  recomp::GPtr(d + 0x4C)[0] = 1;  // connected
  recomp::Wr64(d + 0x50, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - g_start)
                                      .count()));
  recomp::GPtr(d + 0x68)[0] = 1;  // times connected
}

uint32_t Check(int32_t h) {
  if (!g_init) return kErrNotInitialized;
  return h == kHandle && g_open ? 0 : kErrInvalidHandle;
}

}  // namespace

namespace recomp::orbis::pad {

void SetIntercepted(bool on) {
  std::lock_guard<std::mutex> l(g_mu);
  g_intercepted = on;
}

uint32_t Buttons() {
  State s;
  RunOnHost([&] { s = Poll(); });
  return s.buttons;
}

}  // namespace recomp::orbis::pad

using recomp::hle::Arg;

ORBIS_HLE("libScePad", scePadInit, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_init) return 0;
  g_init = true;
  recomp::orbis::RunOnHost([] {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  // as host.cpp
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
      if (n) g_pad = SDL_OpenGamepad(ids[0]);
      SDL_free(ids);
    }
  });
  recomp::orbis::AddHostEventSink(Sink);
  if (recomp::Background()) recomp::orbis::RunOnHost([] { Script(); });  // starts its clock
  return 0;
}

// int scePadOpen(SceUserServiceUserId user, int type, int index, const
// ScePadOpenParam* param): the standard controller (type 0) of the user.
ORBIS_HLE("libScePad", scePadOpen, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  const uint32_t type = uint32_t(Arg(c, 1));
  if (type > 0x10 || !((0x10005u >> type) & 1) || (type == 0x10 && int32_t(Arg(c, 0)) != 0xFF))
    return kErrInvalidArg;
  if (type != 0 || int32_t(Arg(c, 2)) != 0 || int32_t(Arg(c, 0)) != kUser)
    recomp::FatalF("scePadOpen: user %d type %u index %d not implemented yet", int32_t(Arg(c, 0)),
                   type, int32_t(Arg(c, 2)));
  if (g_open) return kErrAlreadyOpened;
  g_open = true;
  return kHandle;
}

ORBIS_HLE("libScePad", scePadClose, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (uint32_t e = Check(int32_t(Arg(c, 0)))) return e;
  g_open = false;
  return 0;
}

// int scePadRead(int handle, ScePadData* data, int n): n 1-64; 1 state.
ORBIS_HLE("libScePad", scePadRead, 3) {
  const int n = int(Arg(c, 2));
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_init) return kErrNotInitialized;
    if (!Arg(c, 1) || n < 1 || n > 64) return kErrInvalidArg;
    if (uint32_t e = Check(int32_t(Arg(c, 0)))) return e;
  }
  State s;
  recomp::orbis::RunOnHost([&] { s = Poll(); });
  PutData(Arg(c, 1), s);
  return 1;
}

ORBIS_HLE("libScePad", scePadReadState, 2) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_init) return kErrNotInitialized;
    if (!Arg(c, 1)) return kErrInvalidArg;
    if (uint32_t e = Check(int32_t(Arg(c, 0)))) return e;
  }
  State s;
  recomp::orbis::RunOnHost([&] { s = Poll(); });
  PutData(Arg(c, 1), s);
  return 0;
}

// int scePadSetVibration(int handle, const ScePadVibrationParam* p): large
// and small motors (0-255).
ORBIS_HLE("libScePad", scePadSetVibration, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  if (!Arg(c, 1)) return kErrInvalidArg;
  if (uint32_t e = Check(int32_t(Arg(c, 0)))) return e;
  const uint8_t large = recomp::GPtr(Arg(c, 1))[0], small = recomp::GPtr(Arg(c, 1))[1];
  recomp::orbis::PostToHost([large, small] {
    if (g_pad && !recomp::Background())
      SDL_RumbleGamepad(g_pad, uint16_t(large * 257), uint16_t(small * 257), 1000);
  });
  return 0;
}

// int scePadSetLightBar(int handle, const ScePadLightBarParam* p): r, g, b.
ORBIS_HLE("libScePad", scePadSetLightBar, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  if (!Arg(c, 1)) return kErrInvalidArg;
  if (uint32_t e = Check(int32_t(Arg(c, 0)))) return e;
  const uint8_t* p = recomp::GPtr(Arg(c, 1));
  const uint8_t r = p[0], g = p[1], b = p[2];
  recomp::orbis::PostToHost([r, g, b] {
    if (g_pad) SDL_SetGamepadLED(g_pad, r, g, b);
  });
  return 0;
}

// The motion sensors report the controller lying still: nothing to reset
// or correct.
ORBIS_HLE("libScePad", scePadResetOrientation, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return Check(int32_t(Arg(c, 0)));
}
ORBIS_HLE("libScePad", scePadSetTiltCorrectionState, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  return Check(int32_t(Arg(c, 0)));
}

// int scePadGetControllerInformation(int handle,
// ScePadControllerInformation* info): a DualShock 4's touch pad (44.86
// dots per mm, 1920 x 942) and stick dead zones, connected locally, as a
// console reports them.
ORBIS_HLE("libScePad", scePadGetControllerInformation, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  const gaddr p = Arg(c, 1);
  if (!p) return kErrInvalidArg;
  if (uint32_t e = Check(int32_t(Arg(c, 0)))) return e;
  std::memset(recomp::GPtr(p), 0, 0x1C);
  PutF(p, 44.86f);
  recomp::Wr16(p + 4, 1920);
  recomp::Wr16(p + 6, 942);
  recomp::GPtr(p)[8] = 13;  // dead zones
  recomp::GPtr(p)[9] = 13;
  recomp::GPtr(p)[10] = 0;   // local connection
  recomp::GPtr(p)[11] = 1;   // connected once
  recomp::GPtr(p)[12] = 1;   // connected
  return 0;
}
