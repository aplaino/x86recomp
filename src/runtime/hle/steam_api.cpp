// steam_api.dll (Steamworks, the accessor API of SDKs before 1.37) for
// 32-bit guests, answering as a Steam client that is running but offline:
// SteamAPI_Init succeeds, RestartAppIfNecessary never restarts, and no
// callback is ever posted. The accessors (SteamUser(), SteamApps()...)
// return guest objects whose vtables are thunks named "<Interface>::#<slot>":
// the interface versions are those of the steam_api.dll a game shipped with,
// so a method gets a handler (by its slot in that version) once a game is
// seen to call it, and calling one without is fatal and names it, as an
// unimplemented import is. Methods are __thiscall: `this` in ecx, the
// arguments on the stack, popped by the callee.

#include <map>
#include <mutex>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "steam_api.dll";
constexpr int kSlots = 128;  // more than any Steamworks interface has

// The guest object for interface `name`: {vtable} with kSlots thunks.
gaddr Interface(const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, gaddr> objs;
  std::lock_guard<std::mutex> l(mu);
  gaddr& o = objs[name];
  if (o) return o;
  gaddr vt = HeapAllocProcess(4 * kSlots, true);
  for (int i = 0; i < kSlots; i++) Wr32(vt + 4 * i, ThunkFor(kDll, name + "::#" + std::to_string(i)));
  o = HeapAllocProcess(16, true);
  Wr32(o, vt);
  return o;
}

// CCallbackBase: {vtable, uint8 m_nCallbackFlags, int m_iCallback}. The
// SDK's CCallback destructor unregisters only while the registered flag is
// set, so registering sets it as Steam's own steam_api.dll does.
constexpr gaddr kCallbackFlags = 4, kCallbackId = 8;
constexpr uint8_t kCallbackRegistered = 0x01;

}  // namespace

HLE(kDll, SteamAPI_RestartAppIfNecessary, kCdecl) {
  RECOMP_TRACE("  steam: RestartAppIfNecessary(%u): no\n", unsigned(Arg(c, 0)));
  return 0;
}
HLE(kDll, SteamAPI_Init, kCdecl) {
  RECOMP_TRACE("  steam: Init (offline)\n");
  return 1;
}
HLE(kDll, SteamAPI_Shutdown, kCdecl) { return 0; }
HLE(kDll, SteamAPI_RunCallbacks, kCdecl) { return 0; }

// (CCallbackBase*, int iCallback): nothing is ever posted, so the callback
// is only marked registered.
HLE(kDll, SteamAPI_RegisterCallback, kCdecl) {
  gaddr cb = Arg(c, 0);
  Wr8(cb + kCallbackFlags, Rd8(cb + kCallbackFlags) | kCallbackRegistered);
  Wr32(cb + kCallbackId, Arg(c, 1));
  return 0;
}
HLE(kDll, SteamAPI_UnregisterCallback, kCdecl) {
  gaddr cb = Arg(c, 0);
  Wr8(cb + kCallbackFlags, Rd8(cb + kCallbackFlags) & ~kCallbackRegistered);
  return 0;
}
// (CCallbackBase*, SteamAPICall_t): no call is ever made, so no result.
HLE(kDll, SteamAPI_RegisterCallResult, kCdecl) { return 0; }
HLE(kDll, SteamAPI_UnregisterCallResult, kCdecl) { return 0; }

// ISteamClient [21] SetWarningMessageHook(SteamAPIWarningMessageHook_t)
// (ISteamClient012 and later): nothing ever warns.
HLE_NAMED(kDll, "ISteamClient::#21", SteamClientSetWarningMessageHook, 1) { return 0; }

// ISteamUtils [10] SetOverlayNotificationPosition(ENotificationPosition)
// (ISteamUtils005 and later): there is no overlay.
HLE_NAMED(kDll, "ISteamUtils::#10", SteamUtilsSetOverlayNotificationPosition, 1) { return 0; }

#define RECOMP_STEAM_ACCESSOR(fn, iface) \
  HLE(kDll, fn, kCdecl) { return Interface(#iface); }
RECOMP_STEAM_ACCESSOR(SteamClient, ISteamClient)
RECOMP_STEAM_ACCESSOR(SteamUser, ISteamUser)
RECOMP_STEAM_ACCESSOR(SteamFriends, ISteamFriends)
RECOMP_STEAM_ACCESSOR(SteamUtils, ISteamUtils)
RECOMP_STEAM_ACCESSOR(SteamUserStats, ISteamUserStats)
RECOMP_STEAM_ACCESSOR(SteamApps, ISteamApps)
RECOMP_STEAM_ACCESSOR(SteamNetworking, ISteamNetworking)
RECOMP_STEAM_ACCESSOR(SteamMatchmaking, ISteamMatchmaking)
#undef RECOMP_STEAM_ACCESSOR

}  // namespace recomp::hle
