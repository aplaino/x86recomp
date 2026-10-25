// The system dialogs' HLE: libSceCommonDialog (what every dialog library
// needs initialised first), then each dialog library. Codes are 11.00's.

#include <mutex>

#include "orbis/hle.h"
#include "runtime/runtime.h"

namespace {

constexpr uint32_t kErrAlreadySystemInitialized = 0x80B80002;

std::mutex g_mu;
bool g_common = false;

}  // namespace

ORBIS_HLE("libSceCommonDialog", sceCommonDialogInitialize, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_common) return kErrAlreadySystemInitialized;
  g_common = true;
  return 0;
}
