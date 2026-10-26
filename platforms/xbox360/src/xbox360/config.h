#pragma once
// The console's settings (XConfig, which ExGetXConfigSetting reads) and
// what the system derives from them: the AV pack and the video mode
// (docs/xboxkrnl.md, "Settings and the display"). Only the settings here
// exist; reading any other is fatal and names it.

#include <cstdint>

namespace recomp::xbox360 {

// XConfig categories.
constexpr uint16_t kXConfigSecured = 2, kXConfigUser = 3;

// Setting `setting` of `category`: its bytes (big-endian, as the console
// stores them) and size; false if there is no such setting here.
bool XConfigSetting(uint16_t category, uint16_t setting, const uint8_t*& data, uint16_t& size);

// XGetGameRegion: the console's game region, the games it plays (the
// XEX's region mask's bits for it).
uint16_t GameRegion();

// HalGetCurrentAVPack: the cable the console's display is on.
uint32_t AvPack();
// The display's mode; VdQueryVideoMode writes it as an X_VIDEO_MODE (0x30
// bytes) at `va`.
struct VideoMode {
  uint32_t width, height;
  bool interlaced, widescreen, high_definition;
  uint32_t refresh_hz, standard;
};
VideoMode CurrentVideoMode();
constexpr uint32_t kVideoModeSize = 0x30;
void WriteVideoMode(uint32_t va);

}  // namespace recomp::xbox360
