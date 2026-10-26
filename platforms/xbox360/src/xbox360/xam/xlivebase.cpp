// xam: the Live app (XLiveBase, app 0xFC). xam's own handler for it only
// forwards to the object that xlivebase.xex registers; that module is in
// the console's flash, not in the system update, so none of this comes
// from the system's code: what is here is Xenia's, awaiting the console
// check (docs/xam.md).

#include "xbox360/hle.h"
#include "xbox360/xam/msg.h"

namespace recomp::xbox360 {
namespace {

constexpr uint32_t kEFail = 0x80004005;

}  // namespace

// 0x58004 (p1: a word): sent on a title's online startup, which passes the
// word on to its next requests. Xenia writes 1 and succeeds.
XMSG(kXLiveBaseApp, 0x58004, XLiveBaseGetLogonId) {
  Wr32(p1, 1);
  return 0;
}

// 0x58020 (p1: 0x58004's word, p2: the request): the friends list, which a
// title enumerates; Xenia fails it (E_FAIL), which a title takes for no
// list.
XMSG(kXLiveBaseApp, 0x58020, XLiveBaseEnumerateFriends) { return kEFail; }

}  // namespace recomp::xbox360
