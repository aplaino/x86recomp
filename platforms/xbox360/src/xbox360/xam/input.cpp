// xam: controllers (XamInput*). No controller is connected yet: the host's
// input devices are not read, so every user's device is missing, as on a
// console with none plugged in.

#include <cstring>

#include "xbox360/hle.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorDeviceNotConnected = 1167;

}  // namespace

// XamInputGetCapabilities(user, flags, *capabilities): XINPUT_CAPABILITIES
// (0x14 bytes) zeroed, then filled from the device; with none, for a
// gamepad and a keyboard alike, ERROR_DEVICE_NOT_CONNECTED (xam's code).
HLE(kXam, 400, XamInputGetCapabilities) {
  std::memset(Mem() + Arg(c, 2), 0, 0x14);
  return kErrorDeviceNotConnected;
}

// XamInputGetState(user, flags, *state): XINPUT_STATE (packet number, then
// the gamepad: 16 bytes) zeroed first; ERROR_DEVICE_NOT_CONNECTED when the
// user has no device (xam's code).
HLE(kXam, 401, XamInputGetState) {
  std::memset(Mem() + Arg(c, 2), 0, 0x10);
  return kErrorDeviceNotConnected;
}

// XamInputSetState(user, flags, *vibration): ERROR_DEVICE_NOT_CONNECTED
// when the user has no device (xam's code).
HLE(kXam, 402, XamInputSetState) { return kErrorDeviceNotConnected; }

}  // namespace recomp::xbox360
