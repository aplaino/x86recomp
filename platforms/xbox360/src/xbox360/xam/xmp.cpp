// xam: the music player app (XMP, app 0xFA): who controls playback, the
// title or the user's own music. There is no user music here, so the title
// always does.

#include "xbox360/hle.h"
#include "xbox360/xam/msg.h"

namespace recomp::xbox360 {
namespace {

constexpr uint32_t kEInvalidArg = 0x80070057;
// XMP clients and playback controllers (Xenia's names).
constexpr uint32_t kClientGame = 2, kControllerGame = 0, kControllerRestore = 4;

}  // namespace

// XMPSetPlaybackController {client, controller, locked}: a title asks for
// playback (controller 0) or gives it back (4); xam refuses any other
// controller from a title. Accepted here.
XMSG(kXmpApp, 0x7001A, XMPSetPlaybackController) {
  if (!p1) return kEInvalidArg;
  uint32_t client = Rd32(p1), controller = Rd32(p1 + 4);
  if (client == kClientGame && controller != kControllerGame && controller != kControllerRestore)
    return kEInvalidArg;
  return 0;
}

// XMPGetPlaybackController {client, *controller, *locked}: the title's.
XMSG(kXmpApp, 0x7001B, XMPGetPlaybackController) {
  if (!p1) return kEInvalidArg;
  Wr32(Rd32(p1 + 4), kControllerGame);
  Wr32(Rd32(p1 + 8), 0);
  return 0;
}

}  // namespace recomp::xbox360
