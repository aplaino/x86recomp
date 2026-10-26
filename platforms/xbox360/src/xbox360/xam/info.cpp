// xam: what titles ask about the console.

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

}  // namespace recomp::xbox360
