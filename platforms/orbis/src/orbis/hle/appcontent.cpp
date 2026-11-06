// libSceAppContent HLE. The library (11.00) checks its arguments, then
// asks a system service; the checks, their order, the SDK versions they
// depend on and the filter the service's codes go through are the
// library's. The service's side is modelled as a game installed in full
// with no add-on content: the full SKU, param.sfo's USER_DEFINED_PARAM_n,
// an empty add-on list, and temporary data at /temp0 (kernel_file.cpp's
// host directory). docs/hle.md has what is not measured.

#include <filesystem>
#include <mutex>
#include <string>

#include "orbis/hle.h"
#include "orbis/process.h"
#include "runtime/log.h"
#include "x86/runtime.h"

namespace {

using recomp::gaddr;
using recomp::orbis::SdkVersion;

constexpr uint32_t kErrNotInitialized = 0x80D90001, kErrParameter = 0x80D90002,
                   kErrBusy = 0x80D90003, kErrNotFound = 0x80D90005;

std::mutex g_mu;
bool g_initialized = false;

bool Zero(gaddr p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (recomp::Rd8(p + i)) return false;
  return true;
}

// The calls' common check: initialised.
bool Initialized() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_initialized;
}

// An entitlement label ({char data[17]; char padding[3]}): from SDK 1.50
// (3.50 for the download progress) its padding must be zero.
bool LabelOk(gaddr label, uint32_t from_sdk = 0x01500000) {
  return SdkVersion() < from_sdk || Zero(label + 17, 3);
}

void PutMountPoint(gaddr p, const char* name) {
  for (int i = 0; i < 16; i++) recomp::Wr8(p + i, 0);
  recomp::PutStr(p, name);
}

}  // namespace

using recomp::hle::Arg;

// int sceAppContentInitialize(const SceAppContentInitParam* init (32
// zero bytes), SceAppContentBootParam* boot (40 bytes: attr at 4)): boot
// gets attr 0, not a trial. Twice: busy (from SDK 1.50).
ORBIS_HLE("libSceAppContent", sceAppContentInitialize, 2) {
  const gaddr init = Arg(c, 0), boot = Arg(c, 1);
  const bool checked = SdkVersion() >= 0x01500000;
  std::lock_guard<std::mutex> l(g_mu);
  if (checked && g_initialized) return kErrBusy;
  if (checked && (!init || !boot || !Zero(init, 32) || !Zero(boot, 4) || !Zero(boot + 8, 32)))
    return kErrParameter;
  if (!init || !boot)
    recomp::FatalF("sceAppContentInitialize: null parameters (SDK before 1.50) not implemented");
  for (int i = 0; i < 40; i++) recomp::Wr8(boot + i, 0);
  g_initialized = true;
  return 0;
}

// int sceAppContentAppParamGetInt(uint32_t id, int32_t* value): 0 the SKU
// (3, full), 1-4 param.sfo's USER_DEFINED_PARAM_1-4. On 11.00, id 1 with no
// USER_DEFINED_PARAM_1 in param.sfo returns 0 and value 0 (measured); the
// other ids' answer without their parameter is not measured.
ORBIS_HLE("libSceAppContent", sceAppContentAppParamGetInt, 2) {
  if (!Initialized()) return kErrNotInitialized;
  if (!Arg(c, 1)) return kErrParameter;
  const uint32_t id = uint32_t(Arg(c, 0));
  int32_t v;
  if (id == 0) {
    v = 3;
  } else if (id >= 1 && id <= 4) {
    const auto& ints = recomp::orbis::AppParam().ints;
    auto it = ints.find("USER_DEFINED_PARAM_" + std::to_string(id));
    if (it != ints.end()) {
      v = int32_t(it->second);
    } else if (id == 1) {
      v = 0;
    } else {
      recomp::FatalF("sceAppContentAppParamGetInt(%u): param.sfo has no USER_DEFINED_PARAM_%u "
                     "(the system's answer not implemented)",
                     id, id);
    }
  } else {
    recomp::FatalF("sceAppContentAppParamGetInt(%u) not implemented", id);
  }
  recomp::Wr32(Arg(c, 1), uint32_t(v));
  return 0;
}

// int sceAppContentGetAddcontInfoList(uint32_t service_label,
// SceAppContentAddcontInfo* list, uint32_t list_num, uint32_t* hit_num):
// no add-on content.
ORBIS_HLE("libSceAppContent", sceAppContentGetAddcontInfoList, 4) {
  if (!Initialized()) return kErrNotInitialized;
  if (!Arg(c, 3)) return kErrParameter;
  recomp::Wr32(Arg(c, 3), 0);
  return 0;
}
// int sceAppContentGetAddcontInfo(uint32_t service_label, const
// SceNpUnifiedEntitlementLabel* label, SceAppContentAddcontInfo* info):
// none is installed.
ORBIS_HLE("libSceAppContent", sceAppContentGetAddcontInfo, 3) {
  if (!Initialized()) return kErrNotInitialized;
  if (!Arg(c, 1) || !Arg(c, 2) || !LabelOk(Arg(c, 1))) return kErrParameter;
  return kErrNotFound;
}

// int sceAppContentTemporaryDataMount2(uint32_t option,
// SceAppContentMountPoint* mount_point (16 bytes)): /temp0; option bit 0
// (format) clears it first. sceAppContentTemporaryDataMount formats.
static uint64_t MountTemp(gaddr mp, uint32_t option) {
  if (!Initialized()) return kErrNotInitialized;
  if (!mp) return kErrParameter;
  if (option & 1) {
    std::error_code ec;
    std::filesystem::remove_all(recomp::DataDir() + "/temp0", ec);
  }
  PutMountPoint(mp, "/temp0");
  return 0;
}
ORBIS_HLE("libSceAppContent", sceAppContentTemporaryDataMount2, 2) {
  return MountTemp(Arg(c, 1), uint32_t(Arg(c, 0)));
}
ORBIS_HLE("libSceAppContent", sceAppContentTemporaryDataMount, 1) { return MountTemp(Arg(c, 0), 1); }
// Unmounting leaves /temp0 as it is (kernel_file.cpp maps it always).
ORBIS_HLE("libSceAppContent", sceAppContentTemporaryDataUnmount, 1) {
  if (!Initialized()) return kErrNotInitialized;
  return Arg(c, 0) ? 0 : kErrParameter;
}
ORBIS_HLE("libSceAppContent", sceAppContentTemporaryDataFormat, 1) {
  if (!Initialized()) return kErrNotInitialized;
  if (!Arg(c, 0)) return kErrParameter;
  std::error_code ec;
  std::filesystem::remove_all(recomp::DataDir() + "/temp0", ec);
  return 0;
}
