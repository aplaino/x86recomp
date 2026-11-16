// steam_api64.dll (Steamworks, the SteamInternal_* accessors of SDKs 1.37
// and later) for x64 guests, answering as a Steam client that is running
// but offline: SteamAPI_Init succeeds and no callback is ever posted.
// Interfaces come from ISteamClient's GetISteam* methods, by the version
// string the game asks for, as guest objects whose vtables are thunks named
// "<version>::#<slot>": a method gets a handler (by its slot in that
// version) once a game is seen to call it, and calling one without is
// fatal and names it, as an unimplemented import is.

#include <strings.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "steam_api64.dll";
constexpr int kSlots = 128;  // more than any Steamworks interface has
constexpr uint32_t kUser = 1, kPipe = 1;  // HSteamUser, HSteamPipe

// The guest object for interface `name`: {vtable} with kSlots thunks.
gaddr Interface(const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, gaddr> objs;
  std::lock_guard<std::mutex> l(mu);
  gaddr& o = objs[name];
  if (o) return o;
  gaddr vt = HeapAllocProcess(8 * kSlots, true);
  for (int i = 0; i < kSlots; i++)
    Wr64(vt + 8 * i, ThunkFor(kDll, name + "::#" + std::to_string(i)));
  o = HeapAllocProcess(16, true);
  Wr64(o, vt);
  RECOMP_TRACE("  steam: interface %s at %08llX\n", name.c_str(), (unsigned long long)o);
  return o;
}

// Incremented by SteamAPI_Init: contexts set up before it are set up again
// (SteamInternal_ContextInit).
uint64_t g_context_counter = 0;

// A Steamworks interface version string ("SteamUser019",
// "STEAMHTTP_INTERFACE_VERSION002"), or empty.
std::string VersionAt(gaddr p) {
  if (p < 0x10000 || !g_proc->mem.IsCommitted(p & ~gaddr(0xFFF))) return {};
  std::string s = GStr(p);
  return s.size() > 5 && !strncasecmp(s.c_str(), "steam", 5) ? s : std::string();
}

// ISteamClient's GetISteam*(... , version): the version string is the last
// argument, the fourth (hUser, hPipe, version) or, for GetISteamUtils, the
// third (hPipe, version).
uint64_t GetInterface(Ctx& c) {
  std::string v = VersionAt(Arg(c, 3));
  if (v.empty()) v = VersionAt(Arg(c, 2));
  if (v.empty()) FatalF("steam_api64: an ISteamClient getter without a version string");
  return Interface(v);
}

}  // namespace

HLE64(kDll, SteamAPI_Init, kCdecl) {
  RECOMP_TRACE("  steam: Init (offline)\n");
  g_context_counter++;
  return 1;
}
HLE64(kDll, SteamAPI_Shutdown, kCdecl) { return 0; }
HLE64(kDll, SteamAPI_RunCallbacks, kCdecl) { return 0; }
HLE64(kDll, SteamAPI_GetHSteamUser, kCdecl) { return kUser; }
HLE64(kDll, SteamAPI_GetHSteamPipe, kCdecl) { return kPipe; }

// CCallbackBase: {vtable, uint8 m_nCallbackFlags, int m_iCallback}. The
// SDK's CCallback destructor unregisters only while the registered flag is
// set, so registering sets it as Steam's own steam_api64.dll does.
constexpr gaddr kCallbackFlags = 8, kCallbackId = 12;
constexpr uint8_t kCallbackRegistered = 0x01;

// (CCallbackBase*, int iCallback): nothing is ever posted, so the callback
// is only marked registered.
HLE64(kDll, SteamAPI_RegisterCallback, kCdecl) {
  gaddr cb = Arg(c, 0);
  Wr8(cb + kCallbackFlags, Rd8(cb + kCallbackFlags) | kCallbackRegistered);
  Wr32(cb + kCallbackId, uint32_t(Arg(c, 1)));
  return 0;
}
HLE64(kDll, SteamAPI_UnregisterCallback, kCdecl) {
  gaddr cb = Arg(c, 0);
  Wr8(cb + kCallbackFlags, Rd8(cb + kCallbackFlags) & ~kCallbackRegistered);
  return 0;
}
// (CCallbackBase*, SteamAPICall_t): no call is ever made, so no result.
HLE64(kDll, SteamAPI_RegisterCallResult, kCdecl) { return 0; }
HLE64(kDll, SteamAPI_UnregisterCallResult, kCdecl) { return 0; }

// SteamInternal_CreateInterface(version): ISteamClient.
HLE64(kDll, SteamInternal_CreateInterface, kCdecl) {
  std::string v = VersionAt(Arg(c, 0));
  if (v.empty()) FatalF("steam_api64: CreateInterface without a version string");
  return Interface(v);
}

// SteamInternal_ContextInit(CSteamAPIContextInitData*): {void (*init)(void*
// context), uintp counter, context}. The context is (re)built by init when
// its counter is behind SteamAPI_Init's; returns the context.
HLE64(kDll, SteamInternal_ContextInit, kCdecl) {
  const gaddr data = Arg(c, 0), ctx = data + 16;
  if (Rd64(data + 8) != g_context_counter) {
    Wr64(data + 8, g_context_counter);
    CallGuest(c, Rd64(data), {ctx});
  }
  return ctx;
}

// ISteamClient (SteamClient017): the interface getters by slot.
#define RECOMP_STEAM_GETTER(slot)                                                     \
  HLE_NAMED_X(kDll, "SteamClient017::#" #slot, SteamClientGet##slot, kCdecl, true) { \
    return GetInterface(c);                                                           \
  }
RECOMP_STEAM_GETTER(5)   // GetISteamUser
RECOMP_STEAM_GETTER(6)   // GetISteamGameServer
RECOMP_STEAM_GETTER(8)   // GetISteamFriends
RECOMP_STEAM_GETTER(9)   // GetISteamUtils
RECOMP_STEAM_GETTER(10)  // GetISteamMatchmaking
RECOMP_STEAM_GETTER(11)  // GetISteamMatchmakingServers
RECOMP_STEAM_GETTER(12)  // GetISteamGenericInterface
RECOMP_STEAM_GETTER(13)  // GetISteamUserStats
RECOMP_STEAM_GETTER(14)  // GetISteamGameServerStats
RECOMP_STEAM_GETTER(15)  // GetISteamApps
RECOMP_STEAM_GETTER(16)  // GetISteamNetworking
RECOMP_STEAM_GETTER(17)  // GetISteamRemoteStorage
RECOMP_STEAM_GETTER(18)  // GetISteamScreenshots
RECOMP_STEAM_GETTER(23)  // GetISteamHTTP
RECOMP_STEAM_GETTER(25)  // GetISteamController
RECOMP_STEAM_GETTER(26)  // GetISteamUGC
RECOMP_STEAM_GETTER(27)  // GetISteamAppList
RECOMP_STEAM_GETTER(28)  // GetISteamMusic
RECOMP_STEAM_GETTER(29)  // GetISteamMusicRemote
RECOMP_STEAM_GETTER(30)  // GetISteamHTMLSurface
RECOMP_STEAM_GETTER(35)  // GetISteamInventory
RECOMP_STEAM_GETTER(36)  // GetISteamVideo
#undef RECOMP_STEAM_GETTER

}  // namespace recomp::hle
