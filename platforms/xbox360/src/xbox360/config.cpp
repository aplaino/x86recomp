#include "xbox360/config.h"

#include <bit>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "xbox360/hle.h"

namespace recomp::xbox360 {
namespace {

struct Setting {
  uint16_t category, setting;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> Be32(uint32_t v) { return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }

// The secured AV region: NTSC-M (60 Hz; the video standard in bits 8-15,
// 50 Hz in bit 23).
constexpr uint32_t kAvRegionNtscM = 0x00400100;
// HDMI, in the numbering Xenia uses.
constexpr uint32_t kAvPackHdmi = 8;

// The console as one set up in English for an HD display, with Xenia's
// defaults; the time zone is the host's, at its current offset and
// without daylight saving rules.
const std::vector<Setting>& Settings() {
  static const std::vector<Setting> settings = [] {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    // NT's bias: UTC minus local time, in minutes.
    int32_t bias = int32_t(-local.tm_gmtoff / 60);
    std::string zone = local.tm_zone ? local.tm_zone : "UTC";
    std::vector<uint8_t> name(4, 0);
    for (size_t k = 0; k < zone.size() && k < 4; k++) name[k] = uint8_t(zone[k]);
    return std::vector<Setting>{
        {kXConfigSecured, 2, Be32(kAvRegionNtscM)},  // AV region
        {kXConfigUser, 1, Be32(uint32_t(bias))},     // time zone bias
        {kXConfigUser, 2, name},                     // standard time's name
        {kXConfigUser, 3, name},                     // daylight time's name
        {kXConfigUser, 4, Be32(0)},                  // standard time's start: none
        {kXConfigUser, 5, Be32(0)},                  // daylight time's start: none
        {kXConfigUser, 6, Be32(0)},                  // standard time's bias
        {kXConfigUser, 7, Be32(0)},                  // daylight time's bias
        {kXConfigUser, 9, Be32(1)},                  // language: English
        {kXConfigUser, 10, Be32(0x00010000)},        // video flags: widescreen
        {kXConfigUser, 12, Be32(0x40)},              // retail flags: the dashboard is set up
    };
  }();
  return settings;
}

uint32_t Value(uint16_t category, uint16_t setting) {
  const uint8_t* d;
  uint16_t size;
  if (!XConfigSetting(category, setting, d, size) || size != 4) return 0;
  return uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3];
}

}  // namespace

bool XConfigSetting(uint16_t category, uint16_t setting, const uint8_t*& data, uint16_t& size) {
  for (const Setting& s : Settings())
    if (s.category == category && s.setting == setting) {
      data = s.data.data();
      size = uint16_t(s.data.size());
      return true;
    }
  return false;
}

uint32_t AvPack() { return kAvPackHdmi; }

// X_VIDEO_MODE: width, height, interlaced, widescreen, high definition,
// refresh rate (a float), video standard, then two words Xenia fills with
// 0x8A and 1 (widescreen; 3 otherwise). xam reads the size, the refresh
// rate and the standard. 1280x720 progressive on the HDMI pack, at the
// AV region's rate and standard.
void WriteVideoMode(uint32_t va) {
  uint32_t region = Value(kXConfigSecured, 2);
  bool widescreen = Value(kXConfigUser, 10) & 0x00010000;
  std::memset(Mem() + va, 0, kVideoModeSize);
  Wr32(va, 1280);
  Wr32(va + 4, 720);
  Wr32(va + 8, 0);
  Wr32(va + 12, widescreen);
  Wr32(va + 16, 1);
  Wr32(va + 20, std::bit_cast<uint32_t>(region >> 23 & 1 ? 50.0f : 60.0f));
  Wr32(va + 24, region >> 8 & 0xFF);
  Wr32(va + 28, 0x8A);
  Wr32(va + 32, widescreen ? 1 : 3);
}

}  // namespace recomp::xbox360
