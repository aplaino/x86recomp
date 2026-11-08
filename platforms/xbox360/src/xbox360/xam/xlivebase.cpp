// xam: the Live app (XLiveBase, app 0xFC). xam's own handler for it only
// forwards to the object that xlivebase.xex registers; that module is in
// the console's flash, not in the system update, so none of this comes
// from the system's code: what is here is Xenia's or chosen for a console
// with no Live connection, awaiting the console check (docs/xam.md).

#include "xbox360/hle.h"
#include "xbox360/xam/msg.h"

namespace recomp::xbox360 {
namespace {

constexpr uint32_t kNoMoreFiles = 0x80070012;
// What Live's requests give with no connection (chosen; console check).
constexpr uint32_t kNotLoggedOn = 0x80151802;

}  // namespace

// 0x58004 (p1: a word): sent on a title's online startup, which passes the
// word on to its next requests. Xenia writes 1 and succeeds.
XMSG(kXLiveBaseApp, 0x58004, XLiveBaseGetLogonId) {
  Wr32(p1, 1);
  return 0;
}

// 0x58006 (p1: *NAT type): the NAT type Live sees; with no connection it
// fails, and titles' XOnlineGetNatType then reports open (1) itself.
XMSG(kXLiveBaseApp, 0x58006, XLiveBaseGetNatType) { return kNotLoggedOn; }

// 0x58009 {slot, title, ?, *counts}: the marketplace's offer counts, which
// titles ask only for a user signed in to Live.
XMSG(kXLiveBaseApp, 0x58009, XLiveBaseGetMarketplaceCounts) { return kNotLoggedOn; }

// 0x5800E {slot, ?, XUID, muted}: whether the user muted that XUID. The
// mute list is Live's: empty here.
XMSG(kXLiveBaseApp, 0x5800E, XLiveBaseMuteListQuery) {
  Wr32(p1 + 0x10, 0);
  return 0;
}

// 0x58020 (p1: 0x58004's word, p2: the request): the friends list, which a
// title enumerates. A profile signed in only locally has no friends: the
// enumeration's end at once, HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES), which
// titles take for an empty list (Xenia fails it with E_FAIL, which they
// take for a failure).
XMSG(kXLiveBaseApp, 0x58020, XLiveBaseEnumerateFriends) { return kNoMoreFiles; }

// 0x58023 (p1: a service handle, p2: the request): a request to a Live
// service; none is reachable.
XMSG(kXLiveBaseApp, 0x58023, XLiveBaseServiceRequest) { return kNotLoggedOn; }

}  // namespace recomp::xbox360
