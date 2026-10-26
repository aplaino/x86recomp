// xam: the users signed in (config.h's profiles), as xam's own code
// reports them: 4 slots, a profile signed in locally (sign-in state 1, an
// offline XUID) or not at all; none is signed in to Live.

#include <algorithm>

#include "xbox360/config.h"
#include "xbox360/hle.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kEInvalidArg = 0x80070057, kNotLoggedOn = 0x80070525;
// XUID types XamUserGetXUID asks for: offline (1), online (2, 4).
constexpr uint32_t kXuidOffline = 1;

}  // namespace

// XamUserGetSigninState(slot): 0 not signed in, 1 locally, 2 to Live.
HLE(kXam, 528, XamUserGetSigninState) { return SignedInProfile(Arg(c, 0)) ? 1 : 0; }

// XamUserGetXUID(slot, types, *xuid): an HRESULT; the offline XUID when the
// types include it, ERROR_NOT_LOGGED_ON (and 0) when the slot has no
// profile or no XUID of those types.
HLE(kXam, 522, XamUserGetXUID) {
  uint32_t slot = Arg(c, 0), types = Arg(c, 1), out = Arg(c, 2);
  const Profile* p = SignedInProfile(slot);
  uint32_t hr = slot >= kUserSlots ? kEInvalidArg : !p || !(types & kXuidOffline) ? kNotLoggedOn : 0;
  Wr64(out, hr ? 0 : p->xuid);
  return hr;
}

// XamUserGetName(slot, buffer, size): the gamertag, truncated to size - 1
// and NUL-terminated; a Win32 code (ERROR_INVALID_PARAMETER,
// ERROR_NOT_LOGGED_ON with an empty name).
HLE(kXam, 526, XamUserGetName) {
  uint32_t slot = Arg(c, 0), buf = Arg(c, 1), size = Arg(c, 2);
  if (slot >= kUserSlots) return kEInvalidArg & 0xFFFF;
  const Profile* p = SignedInProfile(slot);
  if (!p) {
    Wr8(buf, 0);
    return kNotLoggedOn & 0xFFFF;
  }
  uint32_t n = std::min<uint32_t>(uint32_t(p->gamertag.size()), (size - 1) & 0xFFFF);
  for (uint32_t k = 0; k < n; k++) Wr8(buf + k, uint8_t(p->gamertag[k]));
  Wr8(buf + n, 0);
  return 0;
}

}  // namespace recomp::xbox360
