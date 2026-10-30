// dinput8: DirectInput8Create, IDirectInput8A and IDirectInputDevice8A for
// the system keyboard and mouse, and the SDL gamepads as joysticks.
//
// Input comes from the event pump's raw input (input_state.h): the
// keyboard by DIK code, the mouse in mickeys. Each acquired device keeps
// its own state and, with DIPROP_BUFFERSIZE set, a buffer of events.
//
// Game controllers are the XInput pads (xinput.h), read when the game
// polls: a joystick with six axes, a POV hat and 16 buttons in the layout
// of the PlayStation 2 USB adapters PC games of the time were written for
// (see kPadButtons), so a modern pad (Xbox, DualSense, Switch Pro, ...)
// works with a game's own default bindings. The first one is listed even
// before a pad connects, so one connected later still works. Force
// feedback effects rumble the pad.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/xinput.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using guest::GUID;

constexpr GUID kGUID_SysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID kIID_IDirectInputDevice8A = {0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};

constexpr uint32_t DI_OK = 0;
constexpr uint32_t DI_BUFFEROVERFLOW = 1;
constexpr uint32_t DI_NOEFFECT = 1;
constexpr uint32_t DIERR_INVALIDPARAM = 0x80070057;
constexpr uint32_t DIERR_NOINTERFACE = 0x80004002;
constexpr uint32_t DIERR_UNSUPPORTED = 0x80004001;
constexpr uint32_t DIERR_DEVICENOTREG = 0x80040154;
constexpr uint32_t DIERR_NOTACQUIRED = 0x8007000C;
constexpr uint32_t DIERR_ACQUIRED = 0x800700AA;

// Device classes (EnumDevices) and types.
constexpr uint32_t DI8DEVCLASS_ALL = 0, DI8DEVCLASS_DEVICE = 1,
                   DI8DEVCLASS_POINTER = 2, DI8DEVCLASS_KEYBOARD = 3,
                   DI8DEVCLASS_GAMECTRL = 4;
constexpr uint32_t DI8DEVTYPE_MOUSE = 0x12, DI8DEVTYPE_KEYBOARD = 0x13,
                   DI8DEVTYPE_GAMEPAD = 0x15;
constexpr uint32_t kMouseType = DI8DEVTYPE_MOUSE | (2 << 8);        // traditional
constexpr uint32_t kKeyboardType = DI8DEVTYPE_KEYBOARD | (4 << 8);  // PC enhanced
constexpr uint32_t kGamepadType = DI8DEVTYPE_GAMEPAD | (2 << 8) | 0x10000;  // standard, HID

// Joystick object types (DIDFT_*) and properties (DIPROP_* are small
// integers in place of GUID pointers).
constexpr uint32_t DIDFT_RELAXIS = 0x1, DIDFT_ABSAXIS = 0x2, DIDFT_AXIS = 0x3,
                   DIDFT_PSHBUTTON = 0x4, DIDFT_BUTTON = 0xC, DIDFT_POV = 0x10,
                   DIDFT_FFACTUATOR = 0x01000000;
constexpr uint32_t DIPROP_RANGE = 4, DIPROP_DEADZONE = 5, DIPROP_SATURATION = 6;
constexpr uint32_t DIPH_DEVICE = 0, DIPH_BYOFFSET = 1, DIPH_BYID = 2;
constexpr uint32_t DIDC_FORCEFEEDBACK = 0x100;
constexpr uint32_t DIERR_NOTFOUND = 0x80070002;

constexpr GUID kGUID_XAxis = {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGUID_Button = {0xA36D02F0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGUID_POV = {0xA36D02F2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGUID_Sine = {0x13541C23, 0x8E33, 0x11D0, {0x9A, 0xD0, 0x00, 0xA0, 0xC9, 0xA0, 0x6E, 0x35}};
constexpr GUID kIID_IDirectInputEffect = {0xE7E1F7C0, 0x88D2, 0x11D0, {0x9A, 0xD0, 0x00, 0xA0, 0xC9, 0xA0, 0x6E, 0x35}};

// The pads' instance GUIDs (FillPadInstance): Data1 is this plus the
// XInput user index.
constexpr uint32_t kPadGuidBase = 0x9A6F1D20;
constexpr int kPadButtonCount = 16;

// DIJOYSTATE axes, in order: lX, lY, lZ, lRx, lRy, lRz (GUID_XAxis + i).
constexpr int kPadAxes = 6;

constexpr uint32_t DISCL_EXCLUSIVE = 0x1;
constexpr uint32_t DIGDD_PEEK = 0x1;
constexpr uint32_t DIPROP_BUFFERSIZE = 1, DIPROP_AXISMODE = 2;
constexpr uint32_t DIPROPAXISMODE_ABS = 0;
constexpr uint32_t DIDC_ATTACHED = 0x1;

// Mouse data offsets (DIMOUSESTATE / DIMOUSESTATE2).
constexpr uint32_t DIMOFS_X = 0, DIMOFS_Y = 4, DIMOFS_Z = 8, DIMOFS_BUTTON0 = 12;

struct DIDEVCAPS {
  uint32_t dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs,
      dwFFSamplePeriod, dwFFMinTimeResolution, dwFirmwareRevision,
      dwHardwareRevision, dwFFDriverVersion;
};
static_assert(sizeof(DIDEVCAPS) == 44);

struct DIDEVICEINSTANCEA {
  uint32_t dwSize;
  GUID guidInstance, guidProduct;
  uint32_t dwDevType;
  char tszInstanceName[260], tszProductName[260];
  GUID guidFFDriver;
  uint16_t wUsagePage, wUsage;
};
static_assert(sizeof(DIDEVICEINSTANCEA) == 580);

// DIDEVICEOBJECTDATA; DirectX 3 apps pass the 16-byte form without uAppData.
struct DIDEVICEOBJECTDATA {
  uint32_t dwOfs, dwData, dwTimeStamp, dwSequence, uAppData;
};

struct Direct8 : com::Object {};

// A pad axis' reporting (DIPROP_RANGE, _DEADZONE, _SATURATION).
struct AxisProps {
  int32_t min = 0, max = 65535;
  uint32_t dead = 0, sat = 10000;
};

// A force feedback effect: Start rumbles its pad for the effect's
// duration at its gain.
struct Effect : com::Object {
  uint32_t pad = 0;
  GUID guid{};
  uint32_t duration_us = 200000;  // DIEFFECT dwDuration (INFINITE: until Stop)
  uint32_t gain = 10000;          // DIEFFECT dwGain
};

struct InputDevice : com::Object {
  bool keyboard = false;
  uint32_t data_size = 0;  // the data format's state size; 0 = not set
  uint32_t coop = 0;
  uint32_t hwnd = 0;
  bool acquired = false;
  bool grabbed = false;  // exclusive mouse: host pointer in relative mode
  uint32_t buffer_size = 0;
  bool overflow = false;
  std::deque<DIDEVICEOBJECTDATA> buffer;
  // Mouse: absolute position (DIPROPAXISMODE_ABS) or motion since the last
  // GetDeviceState, wheel, buttons.
  bool absolute = false;
  int32_t axis[3] = {};
  uint8_t buttons[8] = {};
  // Keyboard: DIK state.
  uint8_t keys[256] = {};
  // Joystick: the XInput user index of the pad it reads, or -1.
  int pad = -1;
  AxisProps axes[kPadAxes];
  ~InputDevice() override;
};

std::mutex g_mu;
std::set<InputDevice*> g_acquired;  // devices that get raw input
uint32_t g_sequence = 0;

void Record(InputDevice& d, uint32_t ofs, uint32_t data, uint32_t time) {
  if (!d.buffer_size) return;
  if (d.buffer.size() >= d.buffer_size) {
    d.buffer.pop_front();
    d.overflow = true;
  }
  d.buffer.push_back({ofs, data, time, ++g_sequence, 0});
}

void OnRawInput(const RawInput& in) {
  std::lock_guard<std::mutex> l(g_mu);
  for (InputDevice* d : g_acquired) {
    if (d->keyboard) {
      if (in.kind != RawInput::Key) continue;
      uint8_t v = in.value ? 0x80 : 0;
      if (d->keys[in.code] == v) continue;
      d->keys[in.code] = v;
      Record(*d, in.code, v, in.time_ms);
      continue;
    }
    switch (in.kind) {
      case RawInput::MouseX:
      case RawInput::MouseY:
      case RawInput::Wheel: {
        int a = in.kind == RawInput::MouseX ? 0 : in.kind == RawInput::MouseY ? 1 : 2;
        d->axis[a] += in.value;
        // Buffered axis data is the motion (relative) or the position.
        uint32_t data = uint32_t(d->absolute ? d->axis[a] : in.value);
        Record(*d, a == 0 ? DIMOFS_X : a == 1 ? DIMOFS_Y : DIMOFS_Z, data, in.time_ms);
        break;
      }
      case RawInput::MouseButton:
        if (in.code < 8) {
          d->buttons[in.code] = in.value ? 0x80 : 0;
          Record(*d, DIMOFS_BUTTON0 + in.code, d->buttons[in.code], in.time_ms);
        }
        break;
      default:
        break;
    }
  }
}

// The exclusive mouse hides the pointer and reports motion without
// bounds, as on Windows: SDL's relative mode on the device's window.
void Grab(InputDevice& d, bool on) {
  if (d.grabbed == on || d.keyboard || !(d.coop & DISCL_EXCLUSIVE) || Background())
    return;
  if (SDL_Window* w = SdlWindowFor(d.hwnd)) SDL_SetWindowRelativeMouseMode(w, on);
  d.grabbed = on;
}

void Unacquire(InputDevice& d) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_acquired.erase(&d);
  }
  if (d.acquired) Grab(d, false);
  d.acquired = false;
}

InputDevice::~InputDevice() { Unacquire(*this); }

uint32_t DirectInputVtable() {
  return com::Vtable("dinput8.dll", "IDirectInput8A",
                     {"QueryInterface", "AddRef", "Release", "CreateDevice",
                      "EnumDevices", "GetDeviceStatus", "RunControlPanel",
                      "Initialize", "FindDevice", "EnumDevicesBySemantics",
                      "ConfigureDevices"});
}

uint32_t DeviceVtable() {
  return com::Vtable(
      "dinput8.dll", "IDirectInputDevice8A",
      {"QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
       "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState",
       "GetDeviceData", "SetDataFormat", "SetEventNotification",
       "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo",
       "RunControlPanel", "Initialize", "CreateEffect", "EnumEffects",
       "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand",
       "EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData",
       "EnumEffectsInFile", "WriteEffectToFile", "BuildActionMap",
       "SetActionMap", "GetImageInfo"});
}

InputDevice* Dev(Ctx& c) {
  InputDevice* d = com::As<InputDevice>(Arg(c, 0));
  if (!d) FatalF("IDirectInputDevice8A call on unknown object %08X", uint32_t(Arg(c, 0)));
  return d;
}

void FillInstance(DIDEVICEINSTANCEA* di, bool keyboard) {
  std::memset(di, 0, sizeof *di);
  di->dwSize = sizeof *di;
  di->guidInstance = di->guidProduct = keyboard ? kGUID_SysKeyboard : kGUID_SysMouse;
  di->dwDevType = keyboard ? kKeyboardType : kMouseType;
  std::snprintf(di->tszInstanceName, sizeof di->tszInstanceName, keyboard ? "Keyboard" : "Mouse");
  std::snprintf(di->tszProductName, sizeof di->tszProductName, keyboard ? "Keyboard" : "Mouse");
  di->wUsagePage = 1;              // generic desktop
  di->wUsage = keyboard ? 6 : 2;   // keyboard / mouse
}

void FillPadInstance(DIDEVICEINSTANCEA* di, const XInputPad& pad) {
  std::memset(di, 0, sizeof *di);
  di->dwSize = sizeof *di;
  di->guidInstance = {kPadGuidBase + pad.index, 0xD5A0, 0x11CF, {'X', 'I', 'N', 'P', 'U', 'T', 0, 0}};
  di->guidProduct = {pad.vid | uint32_t(pad.pid) << 16, 0, 0, {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'}};
  di->dwDevType = kGamepadType;
  std::snprintf(di->tszInstanceName, sizeof di->tszInstanceName, "%s", pad.name.c_str());
  std::snprintf(di->tszProductName, sizeof di->tszProductName, "%s", pad.name.c_str());
  di->wUsagePage = 1;  // generic desktop
  di->wUsage = 5;      // game pad
}

// ---- pads

// DIJOYSTATE buttons from SDL's gamepad buttons, in the PS2 USB adapter
// order: Triangle, Circle, Cross, Square, L2, R2, L1, R1, Select, Start,
// R3, L3, d-pad up, down, left, right. -1 = a trigger (L2/R2) as a button.
constexpr int kPadButtons[kPadButtonCount] = {
    SDL_GAMEPAD_BUTTON_NORTH,         SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_SOUTH,         SDL_GAMEPAD_BUTTON_WEST,
    -1,                               -2,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_BACK,          SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,   SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP,       SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,     SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};

// Raw stick value (-32768..32767) to the axis' range, through its dead
// zone and saturation (ten-thousandths of the half range around centre).
int32_t ScaleAxis(int v, const AxisProps& p) {
  double x = std::clamp(v / 32767.0, -1.0, 1.0);
  const double dead = p.dead / 10000.0, sat = std::max(p.sat / 10000.0, dead + 1e-6);
  const double m = std::fabs(x);
  x = m <= dead ? 0.0 : std::copysign(std::min((m - dead) / (sat - dead), 1.0), x);
  return int32_t(std::lround(p.min + (x + 1.0) / 2.0 * (double(p.max) - p.min)));
}

// DIJOYSTATE (80 bytes) or DIJOYSTATE2 (272): lX, lY (left stick), lZ (the
// triggers, L2 up and R2 down from centre), lRx and lRz (right stick X:
// games of the time read it from either), lRy (right stick Y), sliders,
// four POVs (the d-pad on the first), 32 or 128 buttons.
void ReadPadState(const InputDevice& d, uint8_t* out, uint32_t size) {
  std::memset(out, 0, size);
  PadState p{};
  const bool on = d.pad >= 0 && ReadPad(uint32_t(d.pad), &p);
  const int trig = (p.axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] - p.axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER]) / 2;
  const int raw[kPadAxes] = {p.axis[SDL_GAMEPAD_AXIS_LEFTX],  p.axis[SDL_GAMEPAD_AXIS_LEFTY],
                             trig,                            p.axis[SDL_GAMEPAD_AXIS_RIGHTX],
                             p.axis[SDL_GAMEPAD_AXIS_RIGHTY], p.axis[SDL_GAMEPAD_AXIS_RIGHTX]};
  for (int a = 0; a < kPadAxes; a++) {
    const int32_t v = ScaleAxis(on ? raw[a] : 0, d.axes[a]);
    std::memcpy(out + 4 * a, &v, 4);
  }
  // POV: hundredths of a degree clockwise from up, -1 centred.
  const bool up = p.button[SDL_GAMEPAD_BUTTON_DPAD_UP], down = p.button[SDL_GAMEPAD_BUTTON_DPAD_DOWN];
  const bool left = p.button[SDL_GAMEPAD_BUTTON_DPAD_LEFT], right = p.button[SDL_GAMEPAD_BUTTON_DPAD_RIGHT];
  const int dx = (right ? 1 : 0) - (left ? 1 : 0), dy = (down ? 1 : 0) - (up ? 1 : 0);
  static const int32_t kPov[3][3] = {{31500, 0, 4500}, {27000, -1, 9000}, {22500, 18000, 13500}};
  const int32_t pov = on ? kPov[dy + 1][dx + 1] : -1;
  for (int i = 0; i < 4; i++) {
    const int32_t v = i == 0 ? pov : -1;
    std::memcpy(out + 32 + 4 * i, &v, 4);
  }
  if (!on) return;
  constexpr int kTriggerDown = 32767 / 4;
  for (int b = 0; b < kPadButtonCount; b++) {
    const int src = kPadButtons[b];
    const bool down = src == -1   ? p.axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] > kTriggerDown
                      : src == -2 ? p.axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] > kTriggerDown
                                  : p.button[src];
    out[48 + b] = down ? 0x80 : 0;
  }
}

// The axis a DIPROPHEADER names (dwObj, dwHow), or -1; DIPH_DEVICE is -2.
int PadAxisFor(uint32_t obj, uint32_t how) {
  if (how == DIPH_DEVICE) return -2;
  for (int a = 0; a < kPadAxes; a++) {
    if (how == DIPH_BYOFFSET && obj == uint32_t(4 * a)) return a;
    if (how == DIPH_BYID && (obj & 0xFFFFFF) == ((uint32_t(a) << 8) | DIDFT_ABSAXIS)) return a;
  }
  return -1;
}

// DIDEVICEOBJECTINSTANCEA (DirectX 8: 316 bytes) for object `i` of a pad:
// the axes, then the POV, then the buttons.
constexpr uint32_t kObjectInstanceSize = 316;
constexpr int kPadObjects = kPadAxes + 1 + kPadButtonCount;

void FillPadObject(uint8_t* o, int i) {
  static const char* kAxisNames[kPadAxes] = {"X Axis", "Y Axis", "Z Axis",
                                             "X Rotation", "Y Rotation", "Z Rotation"};
  std::memset(o, 0, kObjectInstanceSize);
  const uint32_t size = kObjectInstanceSize;
  std::memcpy(o, &size, 4);
  GUID type;
  uint32_t ofs, dtype, usage;
  char name[32];
  if (i < kPadAxes) {
    type = kGUID_XAxis;
    type.Data1 += uint32_t(i);
    ofs = 4 * uint32_t(i);
    dtype = (uint32_t(i) << 8) | DIDFT_ABSAXIS | (i < 2 ? DIDFT_FFACTUATOR : 0);
    usage = 0x30 + uint32_t(i);  // HID X, Y, Z, Rx, Ry, Rz
    std::snprintf(name, sizeof name, "%s", kAxisNames[i]);
  } else if (i == kPadAxes) {
    type = kGUID_POV;
    ofs = 32;
    dtype = DIDFT_POV;
    usage = 0x39;  // hat switch
    std::snprintf(name, sizeof name, "Hat Switch");
  } else {
    const int b = i - kPadAxes - 1;
    type = kGUID_Button;
    ofs = 48 + uint32_t(b);
    dtype = (uint32_t(b) << 8) | DIDFT_PSHBUTTON;
    usage = uint32_t(b) + 1;
    std::snprintf(name, sizeof name, "Button %d", b + 1);
  }
  std::memcpy(o + 4, &type, 16);
  std::memcpy(o + 0x14, &ofs, 4);
  std::memcpy(o + 0x18, &dtype, 4);
  std::snprintf(reinterpret_cast<char*>(o + 0x20), 260, "%s", name);
  const uint16_t page = i <= kPadAxes ? 1 : 9;  // generic desktop / button
  std::memcpy(o + 0x134, &page, 2);
  const uint16_t u = uint16_t(usage);
  std::memcpy(o + 0x136, &u, 2);
}

bool PadObjectMatches(int i, uint32_t flags) {
  const uint32_t kinds = flags & 0xFF;
  if (kinds == 0) return true;  // DIDFT_ALL
  if (i < kPadAxes) return kinds & DIDFT_AXIS;
  if (i == kPadAxes) return kinds & DIDFT_POV;
  return kinds & DIDFT_BUTTON;
}

bool IsPadGuid(uint32_t guid, int* index) {
  const uint32_t d1 = Rd32(guid);
  if (d1 < kPadGuidBase || d1 >= kPadGuidBase + 4 || Rd16(guid + 4) != 0xD5A0) return false;
  *index = int(d1 - kPadGuidBase);
  return true;
}

uint32_t QueryInterfaceFor(Ctx& c, const GUID& iid) {
  if (com::GuidEq(Arg(c, 1), iid) || com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return DI_OK;
  }
  Wr32(Arg(c, 2), 0);
  return DIERR_NOINTERFACE;
}

// (hinst, dwVersion, riidltf, ppvOut, punkOuter)
HLE("dinput8.dll", DirectInput8Create, 5) {
  uint32_t out = Arg(c, 3);
  if (!com::GuidEq(Arg(c, 2), kIID_IDirectInput8A)) {
    Wr32(out, 0);
    return DIERR_NOINTERFACE;
  }
  RECOMP_TRACE("  DirectInput8Create version %X\n", uint32_t(Arg(c, 1)));
  Wr32(out, com::Publish(std::make_shared<Direct8>(), DirectInputVtable()));
  return DI_OK;
}

// ---- IDirectInput8A ----------------------------------------------------------------

HLE_NAMED("dinput8.dll", "IDirectInput8A::QueryInterface", DI8_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IDirectInput8A);
}
HLE_NAMED("dinput8.dll", "IDirectInput8A::AddRef", DI8_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dinput8.dll", "IDirectInput8A::Release", DI8_Release, 1) { return com::Release(Arg(c, 0)); }

// (rguid, lplpDirectInputDevice, pUnkOuter)
HLE_NAMED("dinput8.dll", "IDirectInput8A::CreateDevice", DI8_CreateDevice, 4) {
  uint32_t out = Arg(c, 2);
  bool keyboard = com::GuidEq(Arg(c, 1), kGUID_SysKeyboard);
  int pad = -1;
  if (!keyboard && !com::GuidEq(Arg(c, 1), kGUID_SysMouse) && !IsPadGuid(Arg(c, 1), &pad)) {
    Wr32(out, 0);
    return DIERR_DEVICENOTREG;
  }
  auto d = std::make_shared<InputDevice>();
  d->keyboard = keyboard;
  d->pad = pad;
  Wr32(out, com::Publish(d, DeviceVtable()));
  SetRawInputSink(&OnRawInput);
  return DI_OK;
}

// (dwDevType, lpCallback, pvRef, dwFlags): the keyboard, the mouse and the
// XInput pads.
HLE_NAMED("dinput8.dll", "IDirectInput8A::EnumDevices", DI8_EnumDevices, 5) {
  uint32_t type = Arg(c, 1), cb = Arg(c, 2), ref = Arg(c, 3);
  uint32_t cls = type & 0xFF;
  bool mouse = cls == DI8DEVCLASS_ALL || cls == DI8DEVCLASS_POINTER || cls == DI8DEVTYPE_MOUSE;
  bool keyboard = cls == DI8DEVCLASS_ALL || cls == DI8DEVCLASS_KEYBOARD || cls == DI8DEVTYPE_KEYBOARD;
  bool pads = cls == DI8DEVCLASS_ALL || cls == DI8DEVCLASS_GAMECTRL || cls == DI8DEVTYPE_GAMEPAD;
  if (cls != DI8DEVCLASS_ALL && cls != DI8DEVCLASS_DEVICE && cls != DI8DEVCLASS_POINTER &&
      cls != DI8DEVCLASS_KEYBOARD && cls != DI8DEVCLASS_GAMECTRL && cls < 0x11)
    return DIERR_INVALIDPARAM;
  uint32_t inst = HeapAllocProcess(sizeof(DIDEVICEINSTANCEA), true);
  auto* di = reinterpret_cast<DIDEVICEINSTANCEA*>(GPtr(inst));
  bool more = true;
  for (bool kb : {true, false}) {
    if (!more || (kb ? !keyboard : !mouse)) continue;
    FillInstance(di, kb);
    more = CallGuest(c, cb, {inst, ref});  // 0 = DIENUM_STOP
  }
  if (pads) {
    std::vector<XInputPad> list = XInputPads();
    // The first pad's slot even before one connects (pads that connect
    // later, over Bluetooth for one, then still work). Not in automated
    // runs, which see the host's pads only through the test pad.
    if (list.empty() && !Background()) list.push_back({0, 0, 0, "Controller"});
    for (const XInputPad& pad : list) {
      if (!more) break;
      std::fprintf(stderr, "dinput8: game controller %u: %s\n", pad.index, pad.name.c_str());
      FillPadInstance(di, pad);
      more = CallGuest(c, cb, {inst, ref});
    }
  }
  HeapFreeProcess(inst);
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInput8A::GetDeviceStatus", DI8_GetDeviceStatus, 2) {
  int pad;
  return com::GuidEq(Arg(c, 1), kGUID_SysKeyboard) || com::GuidEq(Arg(c, 1), kGUID_SysMouse) ||
                 IsPadGuid(Arg(c, 1), &pad)
             ? DI_OK
             : DIERR_DEVICENOTREG;
}

// ---- IDirectInputDevice8A ----------------------------------------------------------

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::QueryInterface", DID8_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IDirectInputDevice8A);
}
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::AddRef", DID8_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Release", DID8_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetCapabilities", DID8_GetCapabilities, 2) {
  InputDevice* d = Dev(c);
  DIDEVCAPS* caps = reinterpret_cast<DIDEVCAPS*>(GPtr(Arg(c, 1)));
  if (caps->dwSize != sizeof(DIDEVCAPS)) return DIERR_INVALIDPARAM;
  std::memset(reinterpret_cast<uint8_t*>(caps) + 4, 0, sizeof *caps - 4);
  caps->dwFlags = DIDC_ATTACHED;
  if (d->pad >= 0) {
    caps->dwFlags |= DIDC_FORCEFEEDBACK;
    caps->dwDevType = kGamepadType;
    caps->dwAxes = kPadAxes;
    caps->dwButtons = kPadButtonCount;
    caps->dwPOVs = 1;
    caps->dwFFSamplePeriod = caps->dwFFMinTimeResolution = 1000;
    return DI_OK;
  }
  caps->dwDevType = d->keyboard ? kKeyboardType : kMouseType;
  caps->dwAxes = d->keyboard ? 0 : 3;
  caps->dwButtons = d->keyboard ? 128 : 5;
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetDeviceInfo", DID8_GetDeviceInfo, 2) {
  InputDevice* d = Dev(c);
  DIDEVICEINSTANCEA* di = reinterpret_cast<DIDEVICEINSTANCEA*>(GPtr(Arg(c, 1)));
  if (di->dwSize != sizeof(DIDEVICEINSTANCEA)) return DIERR_INVALIDPARAM;
  if (d->pad >= 0) {
    std::string name = "Controller";
    for (const XInputPad& p : XInputPads())
      if (int(p.index) == d->pad) {
        FillPadInstance(di, p);
        return DI_OK;
      }
    FillPadInstance(di, {uint32_t(d->pad), 0, 0, name});
    return DI_OK;
  }
  FillInstance(di, d->keyboard);
  return DI_OK;
}

// (lpdf): the formats are recognised by their state size: c_dfDIKeyboard
// (256), c_dfDIMouse (16) and c_dfDIMouse2 (20).
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetDataFormat", DID8_SetDataFormat, 2) {
  InputDevice* d = Dev(c);
  if (d->acquired) return DIERR_ACQUIRED;
  uint32_t size = Rd32(Arg(c, 1) + 12);  // DIDATAFORMAT.dwDataSize
  bool ok = d->pad >= 0 ? (size == 80 || size == 272)
                        : d->keyboard ? size == 256 : (size == 16 || size == 20);
  if (!ok) FatalF("dinput8: SetDataFormat with a %u-byte %s format", size,
                  d->pad >= 0 ? "joystick" : d->keyboard ? "keyboard" : "mouse");
  d->data_size = size;
  return DI_OK;
}

// (hwnd, dwFlags)
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetCooperativeLevel", DID8_SetCooperativeLevel, 3) {
  InputDevice* d = Dev(c);
  d->hwnd = Arg(c, 1);
  d->coop = Arg(c, 2);
  return DI_OK;
}

// (rguidProp, pdiph): DIPROPDWORD for the buffer size and axis mode.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SetProperty", DID8_SetProperty, 3) {
  InputDevice* d = Dev(c);
  uint32_t prop = Arg(c, 1), ph = Arg(c, 2);
  uint32_t value = Rd32(ph + 16);  // DIPROPDWORD.dwData
  if (d->pad >= 0 && (prop == DIPROP_RANGE || prop == DIPROP_DEADZONE || prop == DIPROP_SATURATION)) {
    const int a = PadAxisFor(Rd32(ph + 8), Rd32(ph + 12));
    if (a == -1) return DIERR_NOTFOUND;
    for (int i = 0; i < kPadAxes; i++) {
      if (a != -2 && a != i) continue;
      AxisProps& p = d->axes[i];
      if (prop == DIPROP_RANGE) {
        p.min = int32_t(Rd32(ph + 16));
        p.max = int32_t(Rd32(ph + 20));
      } else if (prop == DIPROP_DEADZONE) {
        p.dead = std::min<uint32_t>(value, 10000);
      } else {
        p.sat = std::min<uint32_t>(value, 10000);
      }
    }
    return DI_OK;
  }
  switch (prop) {
    case DIPROP_BUFFERSIZE: {
      std::lock_guard<std::mutex> l(g_mu);
      d->buffer_size = value;
      while (d->buffer.size() > value) d->buffer.pop_front();
      return DI_OK;
    }
    case DIPROP_AXISMODE:
      if (d->keyboard) return DIERR_UNSUPPORTED;
      d->absolute = value == DIPROPAXISMODE_ABS;
      return DI_OK;
    default:
      RECOMP_TRACE("  SetProperty %X: unsupported\n", prop);
      return DIERR_UNSUPPORTED;
  }
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetProperty", DID8_GetProperty, 3) {
  InputDevice* d = Dev(c);
  uint32_t prop = Arg(c, 1), ph = Arg(c, 2);
  if (d->pad >= 0 && (prop == DIPROP_RANGE || prop == DIPROP_DEADZONE || prop == DIPROP_SATURATION)) {
    int a = PadAxisFor(Rd32(ph + 8), Rd32(ph + 12));
    if (a == -1) return DIERR_NOTFOUND;
    const AxisProps& p = d->axes[a < 0 ? 0 : a];
    if (prop == DIPROP_RANGE) {
      Wr32(ph + 16, uint32_t(p.min));
      Wr32(ph + 20, uint32_t(p.max));
    } else {
      Wr32(ph + 16, prop == DIPROP_DEADZONE ? p.dead : p.sat);
    }
    return DI_OK;
  }
  switch (prop) {
    case DIPROP_BUFFERSIZE:
      Wr32(ph + 16, d->buffer_size);
      return DI_OK;
    case DIPROP_AXISMODE:
      if (d->keyboard) return DIERR_UNSUPPORTED;
      Wr32(ph + 16, d->absolute ? 0 : 1);
      return DI_OK;
    default:
      RECOMP_TRACE("  GetProperty %X: unsupported\n", prop);
      return DIERR_UNSUPPORTED;
  }
}

// Acquiring starts from the current state: the keys that are down now,
// a mouse at rest (relative mode) or where it was left (absolute mode).
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Acquire", DID8_Acquire, 1) {
  InputDevice* d = Dev(c);
  if (!d->data_size) return DIERR_INVALIDPARAM;
  if (d->acquired) return DI_NOEFFECT;
  if (d->pad >= 0) {
    d->acquired = true;
    return DI_OK;
  }
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (d->keyboard) {
      std::memset(d->keys, 0, sizeof d->keys);
      for (int vk = 1; vk < 256; vk++) {
        if (!g_key_down[vk]) continue;
        bool ext;
        uint8_t sc = ScanCodeForVk(uint8_t(vk), &ext);
        if (sc) d->keys[sc | (ext ? 0x80 : 0)] = 0x80;
      }
    }
    g_acquired.insert(d);
  }
  d->acquired = true;
  Grab(*d, true);
  return DI_OK;
}

HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Unacquire", DID8_Unacquire, 1) {
  InputDevice* d = Dev(c);
  if (!d->acquired) return DI_NOEFFECT;
  Unacquire(*d);
  return DI_OK;
}

// Devices with input of their own are not polled; pads are read in
// GetDeviceState.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::Poll", DID8_Poll, 1) {
  InputDevice* d = Dev(c);
  if (!d->acquired) return DIERR_NOTACQUIRED;
  return d->pad >= 0 ? DI_OK : DI_NOEFFECT;
}

// (cbData, lpvData)
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetDeviceState", DID8_GetDeviceState, 3) {
  InputDevice* d = Dev(c);
  uint32_t size = Arg(c, 1), out = Arg(c, 2);
  if (!d->acquired) return DIERR_NOTACQUIRED;
  if (size != d->data_size) return DIERR_INVALIDPARAM;
  if (d->pad >= 0) {
    ReadPadState(*d, GPtr(out), size);
    return DI_OK;
  }
  std::lock_guard<std::mutex> l(g_mu);
  if (d->keyboard) {
    std::memcpy(GPtr(out), d->keys, 256);
    return DI_OK;
  }
  for (int a = 0; a < 3; a++) Wr32(out + 4 * uint32_t(a), uint32_t(d->axis[a]));
  if (!d->absolute) std::memset(d->axis, 0, sizeof d->axis);
  std::memcpy(GPtr(out + 12), d->buttons, size - 12);
  return DI_OK;
}

// (cbObjectData, rgdod, pdwInOut, dwFlags): the oldest buffered events;
// a null rgdod flushes them.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetDeviceData", DID8_GetDeviceData, 5) {
  InputDevice* d = Dev(c);
  uint32_t obj_size = Arg(c, 1), out = Arg(c, 2), inout = Arg(c, 3), flags = Arg(c, 4);
  if (!d->acquired) return DIERR_NOTACQUIRED;
  if (!d->buffer_size) return DIERR_INVALIDPARAM;  // DIERR_NOTBUFFERED
  if (obj_size != 16 && obj_size != 20) return DIERR_INVALIDPARAM;
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t n = std::min<uint32_t>(Rd32(inout), uint32_t(d->buffer.size()));
  for (uint32_t i = 0; out && i < n; i++)
    std::memcpy(GPtr(out + i * obj_size), &d->buffer[i], obj_size);
  if (!(flags & DIGDD_PEEK)) d->buffer.erase(d->buffer.begin(), d->buffer.begin() + n);
  Wr32(inout, n);
  bool overflow = d->overflow;
  if (!(flags & DIGDD_PEEK)) d->overflow = false;
  return overflow ? DI_BUFFEROVERFLOW : DI_OK;
}

// (lpCallback, pvRef, dwFlags): a pad's objects of the DIDFT_ kinds in
// dwFlags; the mouse and keyboard list none.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::EnumObjects", DID8_EnumObjects, 4) {
  InputDevice* d = Dev(c);
  uint32_t cb = Arg(c, 1), ref = Arg(c, 2), flags = Arg(c, 3);
  if (d->pad < 0) return DI_OK;
  uint32_t inst = HeapAllocProcess(kObjectInstanceSize, true);
  for (int i = 0; i < kPadObjects; i++) {
    if (!PadObjectMatches(i, flags)) continue;
    FillPadObject(GPtr(inst), i);
    if (!CallGuest(c, cb, {inst, ref})) break;  // DIENUM_STOP
  }
  HeapFreeProcess(inst);
  return DI_OK;
}

// (pdidoi, dwObj, dwHow)
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::GetObjectInfo", DID8_GetObjectInfo, 4) {
  InputDevice* d = Dev(c);
  uint32_t out = Arg(c, 1), obj = Arg(c, 2), how = Arg(c, 3);
  if (d->pad < 0) return DIERR_NOTFOUND;
  uint8_t o[kObjectInstanceSize];
  for (int i = 0; i < kPadObjects; i++) {
    FillPadObject(o, i);
    uint32_t ofs, type;
    std::memcpy(&ofs, o + 0x14, 4);
    std::memcpy(&type, o + 0x18, 4);
    if ((how == DIPH_BYOFFSET && ofs == obj) || (how == DIPH_BYID && (type & 0xFFFFFF) == (obj & 0xFFFFFF))) {
      const uint32_t size = std::min<uint32_t>(Rd32(out), kObjectInstanceSize);
      std::memcpy(GPtr(out + 4), o + 4, size - 4);
      return DI_OK;
    }
  }
  return DIERR_NOTFOUND;
}

uint32_t EffectVtable() {
  return com::Vtable("dinput8.dll", "IDirectInputEffect",
                     {"QueryInterface", "AddRef", "Release", "Initialize", "GetEffectGuid",
                      "GetParameters", "SetParameters", "Start", "Stop", "GetEffectStatus",
                      "Download", "Unload", "Escape"});
}

// DIEFFECT: dwDuration (+8, microseconds) and dwGain (+16), as its
// dwFlags-independent fields; DIEP_DURATION 0x1, DIEP_GAIN 0x4 on SetParameters.
void ReadEffectParams(Effect& e, uint32_t eff, uint32_t mask) {
  if (!eff) return;
  if (mask & 0x1) e.duration_us = Rd32(eff + 8);
  if (mask & 0x4) e.gain = std::min<uint32_t>(Rd32(eff + 16), 10000);
}

// (rguid, lpeff, ppdeff, punkOuter): any effect type; it rumbles.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::CreateEffect", DID8_CreateEffect, 5) {
  InputDevice* d = Dev(c);
  uint32_t guid = Arg(c, 1), eff = Arg(c, 2), out = Arg(c, 3);
  if (d->pad < 0) return DIERR_UNSUPPORTED;
  auto e = std::make_shared<Effect>();
  e->pad = uint32_t(d->pad);
  std::memcpy(&e->guid, GPtr(guid), sizeof e->guid);
  ReadEffectParams(*e, eff, 0x5);
  Wr32(out, com::Publish(e, EffectVtable()));
  return DI_OK;
}

// (lpszFileName, pec, pvRef, dwFlags): the effects saved in a file
// (DIFILEEFFECT: dwSize, GuidEffect, lpDiEffect, szFriendlyName). Games
// ship their rumbles that way; where the file is missing (Getting Up's
// Steam build has none) one short rumble stands in for them.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::EnumEffectsInFile", DID8_EnumEffectsInFile, 5) {
  InputDevice* d = Dev(c);
  uint32_t cb = Arg(c, 2), ref = Arg(c, 3);
  if (d->pad < 0) return DI_OK;
  constexpr uint32_t kEffectSize = 0x44, kFileEffectSize = 4 + 16 + 4 + 260;
  uint32_t mem = HeapAllocProcess(kFileEffectSize + kEffectSize, true);
  const uint32_t eff = mem + kFileEffectSize;
  Wr32(mem, kFileEffectSize);
  std::memcpy(GPtr(mem + 4), &kGUID_Sine, 16);
  Wr32(mem + 20, eff);
  std::snprintf(reinterpret_cast<char*>(GPtr(mem + 24)), 260, "Rumble");
  Wr32(eff, kEffectSize);
  Wr32(eff + 8, 180000);  // dwDuration
  Wr32(eff + 16, 10000);  // dwGain
  CallGuest(c, cb, {mem, ref});
  HeapFreeProcess(mem);
  return DI_OK;
}

// (dwFlags): DISFFC_RESET 1, DISFFC_STOPALL 2 stop the rumble.
HLE_NAMED("dinput8.dll", "IDirectInputDevice8A::SendForceFeedbackCommand", DID8_SendFFCommand, 2) {
  InputDevice* d = Dev(c);
  if (d->pad < 0) return DIERR_UNSUPPORTED;
  if (Arg(c, 1) & 3) RumblePad(uint32_t(d->pad), 0, 0, 0);
  return DI_OK;
}

Effect* Eff(Ctx& c) {
  Effect* e = com::As<Effect>(Arg(c, 0));
  if (!e) FatalF("IDirectInputEffect call on unknown object %08X", uint32_t(Arg(c, 0)));
  return e;
}

HLE_NAMED("dinput8.dll", "IDirectInputEffect::QueryInterface", DIE_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IDirectInputEffect);
}
HLE_NAMED("dinput8.dll", "IDirectInputEffect::AddRef", DIE_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dinput8.dll", "IDirectInputEffect::Release", DIE_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("dinput8.dll", "IDirectInputEffect::GetEffectGuid", DIE_GetEffectGuid, 2) {
  std::memcpy(GPtr(Arg(c, 1)), &Eff(c)->guid, 16);
  return DI_OK;
}
// (peff, dwFlags)
HLE_NAMED("dinput8.dll", "IDirectInputEffect::SetParameters", DIE_SetParameters, 3) {
  ReadEffectParams(*Eff(c), Arg(c, 1), Arg(c, 2));
  return DI_OK;
}
HLE_NAMED("dinput8.dll", "IDirectInputEffect::GetParameters", DIE_GetParameters, 3) {
  Effect* e = Eff(c);
  uint32_t eff = Arg(c, 1), mask = Arg(c, 2);
  if (mask & 0x1) Wr32(eff + 8, e->duration_us);
  if (mask & 0x4) Wr32(eff + 16, e->gain);
  return DI_OK;
}
// (dwIterations, dwFlags): both motors at the effect's gain, for its
// duration times the iterations (INFINITE either way: until Stop).
HLE_NAMED("dinput8.dll", "IDirectInputEffect::Start", DIE_Start, 3) {
  Effect* e = Eff(c);
  const uint32_t iterations = Arg(c, 1);
  uint64_t ms = e->duration_us == 0xFFFFFFFF || iterations == 0xFFFFFFFF
                    ? 0xFFFFFFFFull
                    : uint64_t(e->duration_us) / 1000 * std::max<uint32_t>(iterations, 1);
  const uint16_t level = uint16_t(uint32_t(e->gain) * 0xFFFF / 10000);
  RumblePad(e->pad, level, level, uint32_t(std::min<uint64_t>(ms, 0xFFFFFFFF)));
  return DI_OK;
}
HLE_NAMED("dinput8.dll", "IDirectInputEffect::Stop", DIE_Stop, 1) {
  RumblePad(Eff(c)->pad, 0, 0, 0);
  return DI_OK;
}
HLE_NAMED("dinput8.dll", "IDirectInputEffect::GetEffectStatus", DIE_GetEffectStatus, 2) {
  Wr32(Arg(c, 1), 0);
  return DI_OK;
}
HLE_NAMED("dinput8.dll", "IDirectInputEffect::Download", DIE_Download, 1) { return DI_OK; }
HLE_NAMED("dinput8.dll", "IDirectInputEffect::Unload", DIE_Unload, 1) { return DI_OK; }

}  // namespace
}  // namespace recomp::hle
