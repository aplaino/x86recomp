// libSceSystemService, libSceUserService, libSceSysmodule and
// libSceScreenShot HLE: the console's settings (a fixed set: English (US),
// 24-hour clock, UTC, the cross button to confirm), one signed-in user,
// system modules, which are all HLE here (loading one only records it),
// and the system's screenshots, which it never takes.

#include <cstring>
#include <mutex>
#include <set>

#include "orbis/hle.h"
#include "runtime/runtime.h"

namespace {

// sceSystemServiceParamGetInt's ids (SCE_SYSTEM_SERVICE_PARAM_ID_*).
enum : int {
  kLang = 1, kDateFormat = 2, kTimeFormat = 3, kTimeZone = 4, kSummertime = 5,
  kParentalLevel = 7, kEnterButton = 1000,
};
constexpr uint64_t kSystemServiceErrorParameter = 0x80A10003;

constexpr int kUserId = 1;  // the one signed-in user
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
