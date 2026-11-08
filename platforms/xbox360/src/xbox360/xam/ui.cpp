// xam: the system UI titles show (XamShow*UI). xam loads its UI app and
// sends it (0xFE) a message per UI. There is no UI here: one that shows
// closes at once, raising XN_SYS_UI 1 and 0 as xam's UI does (sign-in in
// user.cpp, the device selector in content.cpp). The UIs that need Live
// (friends, messages, players, invitations, others' gamer cards, player
// review) fail as xam fails them for a user not on Live.

#include <cstring>

#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/xam/msg.h"
#include "xbox360/xam/notify.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorAccessDenied = 5, kErrorOutOfMemory = 14, kErrorInvalidParameter = 87,
                   kErrorIoPending = 997, kErrorFunctionFailed = 1627;
constexpr uint32_t kEOutOfMemory = 0x8007000E;
constexpr uint32_t kAnyUser = 0xFF;

// What xam's exports return for their UI app's result: `ok` when it took
// the message, ERROR_OUTOFMEMORY or ERROR_FUNCTION_FAILED (and the UI app
// unloaded) when not.
uint32_t UiResult(uint32_t hr, uint32_t ok) {
  if (int32_t(hr) >= 0) return ok;
  return hr == kEOutOfMemory ? kErrorOutOfMemory : kErrorFunctionFailed;
}

// The UIs a user on Live sees (friends, messages, players, invitations,
// friend requests, others' gamer cards, player review: xam's message
// 0x21002 or 0x21009): for a user, or with 0xFF any, not signed in to Live
// xam shows nothing and returns ERROR_ACCESS_DENIED, as for every profile
// here (xam's code).
uint32_t LiveUi() { return kErrorAccessDenied; }

// A UI shown and closed.
void ShowUi() {
  Broadcast(kXnSysUi, 1);
  Broadcast(kXnSysUi, 0);
}

}  // namespace

HLE(kXam, 703, XamShowFriendsUI) { return LiveUi(); }
HLE(kXam, 704, XamShowMessagesUI) { return LiveUi(); }
HLE(kXam, 712, XamShowPlayersUI) { return LiveUi(); }
// XamShowGameInviteUI(slot, *XUIDs, count, text).
HLE(kXam, 717, XamShowGameInviteUI) { return LiveUi(); }
// XamShowFriendRequestUI(slot, XUID).
HLE(kXam, 718, XamShowFriendRequestUI) { return LiveUi(); }
// XamShowPlayerReviewUI(slot, XUID): ERROR_INVALID_PARAMETER for a slot
// past 3 but 0xFD.
HLE(kXam, 710, XamShowPlayerReviewUI) {
  uint32_t slot = Arg(c, 0);
  return slot < kUserSlots || slot == 0xFD ? LiveUi() : kErrorInvalidParameter;
}

// The profile UI (0x21008): a user's achievements or gamer card. xam asks
// for the user's XUIDs (none for a slot without a profile) and sends it in
// process.
XMSG(kXamApp, 0x21008, ShowProfileUi) {
  ShowUi();
  return 0;
}
static uint32_t ProfileUi(ppc::Ctx& c) {
  uint32_t req = SystemAlloc(0x220);
  std::memset(Mem() + req, 0, 0x220);
  uint32_t hr = CallXMsg(c, kXamApp, 0x21008, req, 0x220);
  SystemFree(req);
  return UiResult(hr, 0);
}

// XamShowAchievementsUI(slot, title): XamShowAchievementsUIEx's.
HLE(kXam, 709, XamShowAchievementsUI) { return ProfileUi(c); }

// XamShowGamerCardUIForXUID(slot, XUID, ?): the user's own card for XUID 0
// or one of the user's XUIDs; another online XUID (0x0009 at the top) is a
// Live UI; ERROR_INVALID_PARAMETER for any other XUID (xam's code).
HLE(kXam, 725, XamShowGamerCardUIForXUID) {
  uint64_t xuid = Arg64(c, 1);
  const Profile* p = SignedInProfile(Arg(c, 0));
  if (!xuid || (p && xuid == p->xuid)) return ProfileUi(c);
  return xuid >> 48 == 9 ? LiveUi() : kErrorInvalidParameter;
}

// The marketplace (0x2100A {..., slot at 0xC, ...}).
XMSG(kXamApp, 0x2100A, ShowMarketplace) {
  ShowUi();
  return 0;
}

// XamShowMarketplaceUI(slot, type, offer id (8 bytes), content
// categories): XamShowMarketplaceUIEx's. ERROR_INVALID_PARAMETER for a
// slot past 3 or a type past 0x18; the user's XUIDs' error (no profile:
// ERROR_NO_SUCH_USER) (xam's code).
HLE(kXam, 711, XamShowMarketplaceUI) {
  constexpr uint32_t kErrorNoSuchUser = 1317;
  uint32_t slot = Arg(c, 0);
  if (slot >= kUserSlots || Arg(c, 1) >= 0x19) return kErrorInvalidParameter;
  if (!SignedInProfile(slot)) return kErrorNoSuchUser;
  uint32_t req = SystemAlloc(0xD8);
  std::memset(Mem() + req, 0, 0xD8);
  Wr32(req + 0xC, slot);
  uint32_t hr = CallXMsg(c, kXamApp, 0x2100A, req, 0xD8);
  SystemFree(req);
  return UiResult(hr, 0);
}

// The keyboard (0x21005 {slot, characters, flags, default text, title,
// description, buffer, *overlapped}): with no UI the default text comes
// back, as Xenia's headless keyboard has it (chosen; console check),
// truncated to the buffer's characters with its terminator.
XMSG(kXamApp, 0x21005, ShowKeyboard) {
  uint32_t chars = Rd32(p1 + 4), text = Rd32(p1 + 0xC), buffer = Rd32(p1 + 0x18);
  ShowUi();
  if (!chars) return 0;
  uint32_t n = 0;
  if (text)
    for (; n + 1 < chars && Rd16(text + 2 * n); n++) Wr16(buffer + 2 * n, Rd16(text + 2 * n));
  Wr16(buffer + 2 * n, 0);
  return 0;
}

// XamShowKeyboardUI(slot, flags, default text, title, description,
// buffer, characters, *overlapped): ERROR_IO_PENDING once the request is
// sent, with or without an XOVERLAPPED (xam's code).
HLE(kXam, 705, XamShowKeyboardUI) {
  uint32_t req = SystemAlloc(0x20);
  const uint32_t fields[8] = {Arg(c, 0), Arg(c, 6), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5), Arg(c, 7)};
  for (uint32_t k = 0; k < 8; k++) Wr32(req + 4 * k, fields[k]);
  uint32_t hr = StartXMsgRequest(c, kXamApp, 0x21005, Arg(c, 7), req, 0x20);
  SystemFree(req);
  return UiResult(hr, kErrorIoPending);
}

// The message box (0x2100D, 0x47C bytes: user at 4, its buttons' count at
// 0x364, the focused button at 0x46C, flags, *result, *overlapped): with
// no UI the focused button is pressed, as Xenia's headless message box has
// it (chosen; console check).
XMSG(kXamApp, 0x2100D, ShowMessageBox) {
  ShowUi();
  if (uint32_t result = Rd32(p1 + 0x474)) Wr32(result, Rd32(p1 + 0x46C));
  return 0;
}

// XamShowMessageBoxUI(Ex)(slot, title, text, button count, *buttons,
// focused button, flags, *result, *overlapped), as xam checks it: a slot
// past 3 but 0xFF, unknown flags, 1-3 buttons with the focus on one (or
// none with a passcode mode, 0x370000), 0x100000 (one button at most,
// focus 0) with 0x200000 (two at most, focus 0 or 1), and a result without
// an XOVERLAPPED or none with one are ERROR_INVALID_PARAMETER, the
// XOVERLAPPED's extended error too. The result is cleared unless a
// passcode mode; ERROR_IO_PENDING with an XOVERLAPPED, otherwise 0 (xam's
// code).
static uint32_t MessageBox(ppc::Ctx& c) {
  uint32_t slot = Arg(c, 0), count = Arg(c, 3), buttons = Arg(c, 4), focus = Arg(c, 5), flags = Arg(c, 6),
           result = Arg(c, 7), ov = Arg(c, 8);
  constexpr uint32_t kOneButton = 0x100000, kTwoButtons = 0x200000, kPasscode = 0x370000;
  bool valid = (slot == kAnyUser || slot < kUserSlots) && !(flags & 0xFFC00EF8) &&
               ((count - 1 <= 2 && focus <= count - 1) || (!count && flags & kPasscode)) &&
               !(flags & kOneButton && flags & kTwoButtons) &&
               (!(flags & kOneButton) || (count <= 1 && !focus)) &&
               (!(flags & kTwoButtons) || (count <= 2 && focus <= 1)) && (ov ? result != 0 : result == 0);
  if (!valid) {
    if (ov) Wr32(ov + 0x18, kErrorInvalidParameter);
    return kErrorInvalidParameter;
  }
  uint32_t req = SystemAlloc(0x47C);
  std::memset(Mem() + req, 0, 0x47C);
  Wr32(req + 4, slot);
  Wr32(req + 8, 1);
  auto copy = [](uint32_t to, uint32_t from, uint32_t chars) {
    for (uint32_t k = 0; from && k + 1 < chars && Rd16(from + 2 * k); k++) Wr16(to + 2 * k, Rd16(from + 2 * k));
  };
  copy(req + 0xC, Arg(c, 1), 0x2B);
  copy(req + 0x62, Arg(c, 2), 0x180);
  Wr32(req + 0x364, count);
  for (uint32_t k = 0; k < count; k++) copy(req + 0x368 + 0x56 * k, Rd32(buttons + 4 * k), 0x2B);
  Wr32(req + 0x46C, focus);
  Wr32(req + 0x470, flags);
  Wr32(req + 0x474, result);
  Wr32(req + 0x478, ov);
  if (!(flags & 0x70000) && result) {
    Wr32(result, 0);
    Wr32(result + 4, 0);
  }
  uint32_t hr = ov ? StartXMsgRequest(c, kXamApp, 0x2100D, ov, req, 0x47C)
                   : CallXMsg(c, kXamApp, 0x2100D, req, 0x47C);
  SystemFree(req);
  return UiResult(hr, ov ? kErrorIoPending : 0);
}
HLE(kXam, 732, XamShowMessageBoxUIEx) { return MessageBox(c); }

// XamShowDirtyDiscErrorUI: xam's message box with its own strings and one
// button, which it waits for; its result (here the button pressed: 0).
HLE(kXam, 729, XamShowDirtyDiscErrorUI) {
  ShowUi();
  return 0;
}

}  // namespace recomp::xbox360
