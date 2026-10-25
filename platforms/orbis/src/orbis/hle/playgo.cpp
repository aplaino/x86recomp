// libScePlayGo HLE: the game is installed in full, so every chunk is
// local (fast), nothing is left to download and every ETA is 0. The
// argument checks and error codes are those of libScePlayGo 11.00's
// client side; the chunk count (the package's, which the PlayGo service
// knows) is not known here, so every chunk id below 1000 is accepted.

#include <mutex>

#include "orbis/hle.h"
#include "runtime/runtime.h"

namespace {

constexpr uint32_t kErrInvalidArgument = 0x80B20004, kErrNotInitialized = 0x80B20005,
                   kErrAlreadyInitialized = 0x80B20006, kErrBadHandle = 0x80B20009,
                   kErrBadPointer = 0x80B2000A, kErrBadSize = 0x80B2000B,
                   kErrBadSpeed = 0x80B2000D;
constexpr uint32_t kHandle = 1;
constexpr uint32_t kMaxEntries = 999;
constexpr int8_t kLocusLocalFast = 3;

std::mutex g_mu;
bool g_init = false, g_open = false;
uint32_t g_speed = 2;  // full speed

uint32_t CheckHandle(uint32_t h) {
  if (!g_init) return kErrNotInitialized;
  return h == kHandle && g_open ? 0 : kErrBadHandle;
}

}  // namespace

using recomp::hle::Arg;

// int scePlayGoInitialize(const ScePlayGoInitParams* p): p is a buffer of
// at least 2 MiB, 16-byte aligned, a multiple of 512 bytes, then a zero.
ORBIS_HLE("libScePlayGo", scePlayGoInitialize, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_init) return kErrAlreadyInitialized;
  const recomp::gaddr p = Arg(c, 0);
  if (!p) return kErrInvalidArgument;
  const uint64_t buf = recomp::Rd64(p);
  const uint32_t size = recomp::Rd32(p + 8);
  if (!buf || buf & 0xF || size < 0x200000 || size & 0x1FF || recomp::Rd32(p + 12))
    return kErrInvalidArgument;
  g_init = true;
  return 0;
}

// int scePlayGoOpen(ScePlayGoHandle* out, const void* param)
ORBIS_HLE("libScePlayGo", scePlayGoOpen, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  if (!Arg(c, 0)) return kErrBadPointer;
  if (Arg(c, 1)) return kErrInvalidArgument;
  g_open = true;
  recomp::Wr32(Arg(c, 0), kHandle);
  return 0;
}

// int scePlayGoGetLocus(ScePlayGoHandle h, const ScePlayGoChunkId* ids,
// uint32_t n, ScePlayGoLocus* out)
ORBIS_HLE("libScePlayGo", scePlayGoGetLocus, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!Arg(c, 1) || !Arg(c, 3)) return kErrBadPointer;
  const uint32_t n = uint32_t(Arg(c, 2));
  if (!n || n > kMaxEntries) return kErrBadSize;
  if (uint32_t e = CheckHandle(uint32_t(Arg(c, 0)))) return e;
  for (uint32_t i = 0; i < n; i++) recomp::GPtr(Arg(c, 3) + i)[0] = uint8_t(kLocusLocalFast);
  return 0;
}

// int scePlayGoGetToDoList(ScePlayGoHandle h, ScePlayGoToDo* out, uint32_t
// n, uint32_t* entries): nothing to do.
ORBIS_HLE("libScePlayGo", scePlayGoGetToDoList, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!Arg(c, 1) || !Arg(c, 3)) return kErrBadPointer;
  if (!Arg(c, 2) || uint32_t(Arg(c, 2)) > kMaxEntries) return kErrBadSize;
  if (uint32_t e = CheckHandle(uint32_t(Arg(c, 0)))) return e;
  recomp::Wr32(Arg(c, 3), 0);
  return 0;
}

// int scePlayGoGetEta(ScePlayGoHandle h, const ScePlayGoChunkId* ids,
// uint32_t n, ScePlayGoEta* out)
ORBIS_HLE("libScePlayGo", scePlayGoGetEta, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!Arg(c, 1) || !Arg(c, 3)) return kErrBadPointer;
  if (!Arg(c, 2) || uint32_t(Arg(c, 2)) > kMaxEntries) return kErrBadSize;
  if (uint32_t e = CheckHandle(uint32_t(Arg(c, 0)))) return e;
  recomp::Wr64(Arg(c, 3), 0);
  return 0;
}

// int scePlayGoSetInstallSpeed(ScePlayGoHandle h, int speed): 0 suspended,
// 1 trickle, 2 full.
ORBIS_HLE("libScePlayGo", scePlayGoSetInstallSpeed, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (uint32_t e = CheckHandle(uint32_t(Arg(c, 0)))) return e;
  if (uint32_t(Arg(c, 1)) > 2) return kErrBadSpeed;
  g_speed = uint32_t(Arg(c, 1));
  return 0;
}

ORBIS_HLE("libScePlayGo", scePlayGoGetInstallSpeed, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (uint32_t e = CheckHandle(uint32_t(Arg(c, 0)))) return e;
  if (!Arg(c, 1)) return kErrBadPointer;
  recomp::Wr32(Arg(c, 1), g_speed);
  return 0;
}
