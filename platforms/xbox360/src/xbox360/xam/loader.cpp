// xam: the title loader.

#include "xbox360/hle.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorNotFound = 0x490;

}  // namespace

// Launch data is what a title hands the next one it launches; a title the
// system starts from its disc has none. xam then writes a size of 0 and
// returns ERROR_NOT_FOUND from both.
HLE(kXam, 423, XamLoaderGetLaunchDataSize) {
  Wr32(Arg(c, 0), 0);
  return kErrorNotFound;
}
HLE(kXam, 424, XamLoaderGetLaunchData) { return kErrorNotFound; }

}  // namespace recomp::xbox360
