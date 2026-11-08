// xam: what titles ask about the console.

#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
// XC_LANGUAGE_*: English, Japanese, Korean; languages run 1-9.
constexpr uint32_t kEnglish = 1, kJapanese = 2, kKorean = 7, kLanguages = 10;
// Game regions: Asia's (0x01xx), Japan, China.
constexpr uint32_t kRegionAsia = 0x100, kRegionJapan = 0x101, kRegionChina = 0x102;
// The execution info's XEX header key.
constexpr uint32_t kXexExecutionInfo = 0x00040006;

}  // namespace

// xam's own code for these two is a jump to the kernel's
// HalGetCurrentAVPack and VdQueryVideoMode.
HLE(kXam, 971, XGetAVPack) { return AvPack(); }

// XGetVideoMode(*mode).
HLE(kXam, 977, XGetVideoMode) {
  WriteVideoMode(Arg(c, 0));
  return 0;
}

// XGetGameRegion: xam returns a halfword the system keeps in memory.
HLE(kXam, 972, XGetGameRegion) { return GameRegion(); }

// XamGetSystemVersion: XboxKrnlVersion's major and minor (4 bits each),
// build and QFE (its low byte), packed as xam packs them.
HLE(kXam, 642, XamGetSystemVersion) {
  uint32_t v = VariableAddress("xboxkrnl.exe", 344);
  if (!v) ppc::Fatal(&c, "XamGetSystemVersion: xboxkrnl's HLE module is not loaded");
  return (Rd16(v) & 0xF) << 28 | (Rd16(v + 2) & 0xF) << 24 | uint32_t(Rd16(v + 4)) << 8 | Rd8(v + 7);
}

// XGetLanguage: the user's language setting when it names one; otherwise
// the region's: Japanese in Japan, Korean in the rest of Asia when the
// country setting (0xE, one byte) is 0x38, English elsewhere (xam's code).
HLE(kXam, 973, XGetLanguage) {
  const uint8_t* data;
  uint16_t size;
  if (XConfigSetting(kXConfigUser, 9, data, size) && size == 4) {
    uint32_t language = uint32_t(data[0]) << 24 | data[1] << 16 | data[2] << 8 | data[3];
    if (language && language < kLanguages) return language;
  }
  uint32_t region = GameRegion();
  if ((region & 0xFF00) != kRegionAsia || region == kRegionChina) return kEnglish;
  if (region == kRegionJapan) return kJapanese;
  return XConfigSetting(kXConfigUser, 0xE, data, size) && size >= 1 && data[0] == 0x38 ? kKorean : kEnglish;
}

// XamGetExecutionId(*info): the title's execution info (its XEX header's
// address), as xam finds it through XexExecutableModuleHandle;
// STATUS_NOT_FOUND (and 0) without a title or without the header.
HLE(kXam, 640, XamGetExecutionId) {
  uint32_t out = Arg(c, 0);
  Wr32(out, 0);
  uint32_t module = ExecutableModule(), headers = module ? Rd32(module + kLdrXexHeader) : 0;
  if (!headers) return kStatusNotFound;
  uint32_t info = XexHeaderField(headers, kXexExecutionInfo);
  Wr32(out, info);
  return info ? kStatusSuccess : kStatusNotFound;
}

}  // namespace recomp::xbox360
