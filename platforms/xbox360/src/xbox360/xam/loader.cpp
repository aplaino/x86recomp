// xam: the title loader.

#include <string>

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorNotFound = 0x490;
// XamLoaderLaunchTitleEx's flags: the dashboard whatever the name (2), and
// no names that are device paths (4, which XamLoaderLaunchTitle adds).
constexpr uint32_t kLaunchDashboard = 2, kLaunchNoDevicePaths = 4;

}  // namespace

// Launch data is what a title hands the next one it launches; a title the
// system starts from its disc has none. xam then writes a size of 0 and
// returns ERROR_NOT_FOUND from both.
HLE(kXam, 423, XamLoaderGetLaunchDataSize) {
  Wr32(Arg(c, 0), 0);
  return kErrorNotFound;
}
HLE(kXam, 424, XamLoaderGetLaunchData) { return kErrorNotFound; }

// XamLoaderLaunchTitle(name, flags): ends the title and starts the
// executable named, through XamLoaderLaunchTitleEx with flag 4. No name,
// an empty one, flag 2 or (with flag 4) a device path (\\...) start the
// dashboard (xam's code), which ends the process here, as
// HalReturnToFirmware does. Another executable cannot run: the program is
// the one title it was lifted from.
HLE(kXam, 420, XamLoaderLaunchTitle) {
  uint32_t name_ptr = Arg(c, 0), flags = Arg(c, 1) | kLaunchNoDevicePaths;
  std::string name = name_ptr ? ReadString(name_ptr) : "";
  if (name.empty() || flags & kLaunchDashboard || name[0] == '\\') ExitProcess(0);
  ppc::Fatal(&c, "XamLoaderLaunchTitle: %s is another executable, which this program cannot start",
             name.c_str());
}

// XamLoaderTerminateTitle: unloads the title and starts the dashboard
// (xam's code), which ends the process here.
HLE(kXam, 425, XamLoaderTerminateTitle) { ExitProcess(0); }

}  // namespace recomp::xbox360
