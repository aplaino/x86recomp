// xam: what titles ask about the console.

#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/hle.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";

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

}  // namespace recomp::xbox360
