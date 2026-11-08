// libSceSystemService, libSceUserService, libSceSysmodule and
// libSceScreenShot HLE: the console's settings (a fixed set: English (US),
// 24-hour clock, UTC, the cross button to confirm), one signed-in user,
// system modules, which are all HLE here (loading one only records it),
// and the system's screenshots, which it never takes.

#include <cstring>
#include <mutex>
#include <set>

#include "orbis/hle.h"
#include "x86/runtime.h"

namespace {

// sceSystemServiceParamGetInt's ids (SCE_SYSTEM_SERVICE_PARAM_ID_*).
enum : int {
  kLang = 1, kDateFormat = 2, kTimeFormat = 3, kTimeZone = 4, kSummertime = 5,
  kParentalLevel = 7, kEnterButton = 1000,
};
constexpr uint64_t kSystemServiceErrorParameter = 0x80A10003;
constexpr uint64_t kSystemServiceErrorNoEvent = 0x80A10004;

// The system's events for the game: on 11.00 a game finds one event
// pending at its start, of type 0x10000003 with no data (measured; what
// raises it is not known). The service keeps pending events as bits;
// libSceSystemService maps each bit to its type.
constexpr uint32_t kStartEvent = 0x10000003;
bool g_start_event = true;

constexpr int kUserId = 0x10000000;  // the one signed-in user, as the console numbers its first
constexpr uint64_t kUserServiceErrorAlreadyInitialized = 0x80960002;
constexpr uint64_t kUserServiceErrorNoSuchUser = 0x80960009;

constexpr uint64_t kSysmoduleErrorUnloaded = 0x80A80001;
std::mutex g_mu;
std::set<uint32_t> g_modules;  // loaded system module ids
bool g_user_service = false;

}  // namespace

using recomp::hle::Arg;

// int sceSystemServiceParamGetInt(int id, int* value)
ORBIS_HLE("libSceSystemService", sceSystemServiceParamGetInt, 2) {
  int v;
  switch (int(Arg(c, 0))) {
    case kLang: v = 1; break;          // English (United States)
    case kDateFormat: v = 0; break;    // YYYY/MM/DD
    case kTimeFormat: v = 1; break;    // 24-hour
    case kTimeZone: v = 0; break;      // minutes from UTC
    case kSummertime: v = 0; break;
    case kParentalLevel: v = 0; break; // off
    case kEnterButton: v = 1; break;   // cross
    default: return kSystemServiceErrorParameter;
  }
  recomp::Wr32(Arg(c, 1), uint32_t(v));
  return 0;
}

// int sceSystemServiceGetStatus(SceSystemServiceStatus* status): the
// library clears the 0x88 bytes, then writes the pending events' count and
// the flags (system UI overlaid, in background, CPU mode 7, live
// streaming, out of the VR play area), which are all clear for a game in
// the foreground (measured). It does not check the pointer.
ORBIS_HLE("libSceSystemService", sceSystemServiceGetStatus, 1) {
  const recomp::gaddr s = Arg(c, 0);
  if (!s) recomp::FatalF("sceSystemServiceGetStatus(NULL): not modelled");
  std::memset(recomp::GPtr(s), 0, 0x88);
  std::lock_guard<std::mutex> l(g_mu);
  recomp::Wr32(s, g_start_event ? 1 : 0);
  return 0;
}

// int sceSystemServiceReceiveEvent(SceSystemServiceEvent* event): the
// oldest pending event; the library clears the 0x2004 bytes and writes its
// type (and, for some types, data; the start event has none). None
// pending: 0x80A10004.
ORBIS_HLE("libSceSystemService", sceSystemServiceReceiveEvent, 1) {
  const recomp::gaddr e = Arg(c, 0);
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_start_event) return kSystemServiceErrorNoEvent;
  if (!e) recomp::FatalF("sceSystemServiceReceiveEvent(NULL): not modelled");
  g_start_event = false;
  std::memset(recomp::GPtr(e), 0, 0x2004);
  recomp::Wr32(e, kStartEvent);
  return 0;
}

// There is no splash screen or background music player to hide.
ORBIS_HLE("libSceSystemService", sceSystemServiceHideSplashScreen, 0) { return 0; }
ORBIS_HLE("libSceSystemService", sceSystemServiceDisableMusicPlayer, 0) { return 0; }

ORBIS_HLE("libSceUserService", sceUserServiceInitialize, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_user_service) return kUserServiceErrorAlreadyInitialized;
  g_user_service = true;
  return 0;
}
ORBIS_HLE("libSceUserService", sceUserServiceTerminate, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  g_user_service = false;
  return 0;
}
ORBIS_HLE("libSceUserService", sceUserServiceGetInitialUser, 1) {
  recomp::Wr32(Arg(c, 0), kUserId);
  return 0;
}
// int sceUserServiceGetUserName(SceUserServiceUserId id, char* name,
// size_t size)
ORBIS_HLE("libSceUserService", sceUserServiceGetUserName, 3) {
  if (int(Arg(c, 0)) != kUserId) return kUserServiceErrorNoSuchUser;
  static const char kName[] = "Player";
  if (Arg(c, 2) < sizeof kName) return 0x80960007;  // SCE_USER_SERVICE_ERROR_BUFFER_TOO_SHORT
  std::memcpy(recomp::GPtr(Arg(c, 1)), kName, sizeof kName);
  return 0;
}

ORBIS_HLE("libSceSysmodule", sceSysmoduleLoadModule, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  g_modules.insert(uint32_t(Arg(c, 0) & 0xFFFF));
  return 0;
}
ORBIS_HLE("libSceSysmodule", sceSysmoduleUnloadModule, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  g_modules.erase(uint32_t(Arg(c, 0) & 0xFFFF));
  return 0;
}
ORBIS_HLE("libSceSysmodule", sceSysmoduleIsLoaded, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_modules.count(uint32_t(Arg(c, 0) & 0xFFFF)) ? 0 : kSysmoduleErrorUnloaded;
}

// The system takes no screenshots: allowing them, forbidding them and
// their parameters and overlay change nothing.
ORBIS_HLE("libSceScreenShot", sceScreenShotDisable, 0) { return 0; }
ORBIS_HLE("libSceScreenShot", sceScreenShotEnable, 0) { return 0; }
ORBIS_HLE("libSceScreenShot", sceScreenShotSetParam, 1) { return 0; }
ORBIS_HLE("libSceScreenShot", sceScreenShotSetOverlayImage, 3) { return 0; }
