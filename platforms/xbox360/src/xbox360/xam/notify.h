#pragma once
// xam's notifications (notify.cpp), for xam's own code that raises them.

#include <cstdint>

namespace recomp::xbox360 {

// Notification ids xam raises: the system UI shown (1) or closed (0), the
// signed-in slots' mask, the users with a controller changed (0).
constexpr uint32_t kXnSysUi = 0x00000009, kXnSysSigninChanged = 0x0000000A,
                   kXnSysInputDevicesChanged = 0x00000012;

// Posts a notification to every listener that takes it.
void Broadcast(uint32_t id, uint32_t value);

}  // namespace recomp::xbox360
