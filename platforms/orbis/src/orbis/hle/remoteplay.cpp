// libSceRemoteplay HLE: Remote Play, for a console nobody streams from.
// sceRemoteplayInitialize's checks and error codes are libSceRemoteplay
// 11.00's; its last step asks the Remote Play service, whose answer is
// modelled as success (docs/hle.md).

#include <mutex>

#include "orbis/hle.h"

namespace {

constexpr uint32_t kErrInvalidArgument = 0x80FC0001, kErrAlreadyInitialized = 0x80FC0003;
constexpr uint64_t kMinHeap = 0x1800;  // a heap the game passes, or the library's own

std::mutex g_mu;
bool g_initialized = false;

}  // namespace

using recomp::hle::Arg;

// int sceRemoteplayInitialize(void* heap, size_t heap_size): without a
// heap the library allocates its own 0x1800 bytes.
ORBIS_HLE("libSceRemoteplay", sceRemoteplayInitialize, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_initialized) return kErrAlreadyInitialized;
  if (Arg(c, 0) && Arg(c, 1) < kMinHeap) return kErrInvalidArgument;
  g_initialized = true;
  return 0;
}
