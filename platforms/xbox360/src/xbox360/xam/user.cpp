// xam: the users signed in (config.h's profiles), as xam's own code
// reports them: 4 slots, a profile signed in locally (sign-in state 1, an
// offline XUID) or not at all; none is signed in to Live.

#include <algorithm>

#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/xam/msg.h"
#include "xbox360/xam/notify.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kEInvalidArg = 0x80070057, kNoSuchUser = 0x80070525;
constexpr uint32_t kErrorInvalidParameter = 87, kErrorNotLoggedOn = 1245, kErrorNoSuchUser = 1317,
                   kErrorFunctionFailed = 1627;
constexpr uint32_t kAnyUser = 0xFF;
// XUID types XamUserGetXUID asks for: offline (1), online (2, 4).
constexpr uint32_t kXuidOffline = 1;

}  // namespace

// XamUserGetSigninState(slot): 0 not signed in, 1 locally, 2 to Live.
HLE(kXam, 528, XamUserGetSigninState) { return SignedInProfile(Arg(c, 0)) ? 1 : 0; }

// XamUserGetXUID(slot, types, *xuid): an HRESULT; the offline XUID when the
// types include it, ERROR_NO_SUCH_USER (and 0) when the slot has no
// profile or no XUID of those types.
HLE(kXam, 522, XamUserGetXUID) {
  uint32_t slot = Arg(c, 0), types = Arg(c, 1), out = Arg(c, 2);
  const Profile* p = SignedInProfile(slot);
  uint32_t hr = slot >= kUserSlots ? kEInvalidArg : !p || !(types & kXuidOffline) ? kNoSuchUser : 0;
  Wr64(out, hr ? 0 : p->xuid);
  return hr;
}

// XamUserGetName(slot, buffer, size): the gamertag, truncated to size - 1
// and NUL-terminated; a Win32 code (ERROR_INVALID_PARAMETER,
// ERROR_NO_SUCH_USER with an empty name).
HLE(kXam, 526, XamUserGetName) {
  uint32_t slot = Arg(c, 0), buf = Arg(c, 1), size = Arg(c, 2);
  if (slot >= kUserSlots) return kErrorInvalidParameter;
  const Profile* p = SignedInProfile(slot);
  if (!p) {
    Wr8(buf, 0);
    return kErrorNoSuchUser;
  }
  uint32_t n = std::min<uint32_t>(uint32_t(p->gamertag.size()), (size - 1) & 0xFFFF);
  for (uint32_t k = 0; k < n; k++) Wr8(buf + k, uint8_t(p->gamertag[k]));
  Wr8(buf + n, 0);
  return 0;
}

// XamUserCheckPrivilege(slot or 0xFF for any, privilege, *result): a Win32
// code and whether the user holds the privilege. Privileges come with a
// Live account (xam asks Live, XLiveBase 0x58008); a profile signed in
// only locally holds none: ERROR_NOT_LOGGED_ON and FALSE.
// ERROR_NO_SUCH_USER for a slot with no profile (or no profile at all for
// 0xFF), ERROR_INVALID_PARAMETER for a slot past 3. All from xam's code.
HLE(kXam, 530, XamUserCheckPrivilege) {
  uint32_t slot = Arg(c, 0);
  bool any = false;
  for (uint32_t s = 0; s < kUserSlots; s++) any |= SignedInProfile(s) != nullptr;
  Wr32(Arg(c, 2), 0);
  if (slot == kAnyUser) return any ? kErrorNotLoggedOn : kErrorNoSuchUser;
  if (slot >= kUserSlots) return kErrorInvalidParameter;
  return SignedInProfile(slot) ? kErrorNotLoggedOn : kErrorNoSuchUser;
}

// XamShowSigninUI(panes, flags): xam's sign-in UI for any user. With
// XSSUI_FLAGS_SHOWONLYONLINEENABLED (0x400000) it needs a signed-in user
// that holds Live's privilege 0xFE, or fails with ERROR_FUNCTION_FAILED,
// as it does here, where none has Live. Otherwise xam posts its UI app
// (0xFE) message 0x21001 {0, 0xFF, panes, flags, 0} and returns
// ERROR_SUCCESS; the UI raises XN_SYS_UI 1 while it shows and 0 when it
// closes, then XN_SYS_SIGNINCHANGED if the profiles changed. With no UI
// here it closes at once with nothing changed (chosen), so a listener
// holds XN_SYS_UI 0 (a listener keeps an id's latest value): the sign-in
// was cancelled.
HLE(kXam, 700, XamShowSigninUI) {
  constexpr uint32_t kOnlineEnabledOnly = 0x400000;
  if (Arg(c, 1) & kOnlineEnabledOnly) return kErrorFunctionFailed;
  Broadcast(kXnSysUi, 1);
  Broadcast(kXnSysUi, 0);
  return 0;
}

}  // namespace recomp::xbox360
