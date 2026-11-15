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

// The game's app id, as it passes it to SteamAPI_RestartAppIfNecessary
// (Steam's own client knows it from the launch; 0 until then).
uint32_t g_app_id = 0;

}  // namespace

HLE(kDll, SteamAPI_RestartAppIfNecessary, kCdecl) {
  g_app_id = uint32_t(Arg(c, 0));
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

// ISteamUtils [9] GetAppID() (ISteamUtils005 and later).
HLE_NAMED(kDll, "ISteamUtils::#9", SteamUtilsGetAppID, 0) {
  if (!g_app_id) FatalF("steam_api: GetAppID before SteamAPI_RestartAppIfNecessary named the app");
  return g_app_id;
}

// ISteamUtils [10] SetOverlayNotificationPosition(ENotificationPosition)
// (ISteamUtils005 and later): there is no overlay.
HLE_NAMED(kDll, "ISteamUtils::#10", SteamUtilsSetOverlayNotificationPosition, 1) { return 0; }

// ISteamUser [1] BLoggedOn() (every version): not connected to the Steam
// servers, as with Steam in offline mode.
HLE_NAMED(kDll, "ISteamUser::#1", SteamUserBLoggedOn, 0) { return 0; }

// ISteamUser [2] GetSteamID() (ISteamUser010 and later): MSVC returns the
// CSteamID class through a hidden pointer, the only stack argument. Always
// the same individual account of the public universe, so what a game keys
// by it (save folders) stays put.
constexpr uint64_t kSteamId = 0x0110000100000001ull;
HLE_NAMED(kDll, "ISteamUser::#2", SteamUserGetSteamID, 1) {
  const gaddr out = Arg(c, 0);
  Wr32(out, uint32_t(kSteamId));
  Wr32(out + 4, uint32_t(kSteamId >> 32));
  return out;
}

// ISteamApps [4] GetCurrentGameLanguage() -> const char* (ISteamApps003
// and later): the language Steam would launch the game in.
HLE_NAMED(kDll, "ISteamApps::#4", SteamAppsGetCurrentGameLanguage, 0) {
  static const gaddr lang = [] {
    const gaddr s = HeapAllocProcess(8, true);
    PutStr(s, "english");
    return s;
  }();
  return lang;
}

// ISteamApps [6] BIsSubscribedApp(AppId_t), [7] BIsDlcInstalled(AppId_t)
// (ISteamApps003 and later), asked about DLC: GameConfig::steam_owns_dlc.
HLE_NAMED(kDll, "ISteamApps::#6", SteamAppsBIsSubscribedApp, 1) {
  RECOMP_TRACE("  steam: BIsSubscribedApp(%u)\n", unsigned(Arg(c, 0)));
  return g_proc->game.steam_owns_dlc ? 1 : 0;
}
HLE_NAMED(kDll, "ISteamApps::#7", SteamAppsBIsDlcInstalled, 1) {
  RECOMP_TRACE("  steam: BIsDlcInstalled(%u)\n", unsigned(Arg(c, 0)));
  return g_proc->game.steam_owns_dlc ? 1 : 0;
}

// ISteamFriends [0] GetPersonaName() -> const char* (every version): a
// string the client owns, the same name as the GFWL profile's.
HLE_NAMED(kDll, "ISteamFriends::#0", SteamFriendsGetPersonaName, 0) {
  static const gaddr name = [] {
    const gaddr s = HeapAllocProcess(8, true);
    PutStr(s, "Player");
    return s;
  }();
  return name;
}

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
