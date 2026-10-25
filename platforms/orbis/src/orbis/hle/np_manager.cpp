// libSceNpManager HLE: the user is not signed in to PSN (the console has
// no account linked), so the sign-in state is "signed out", it never
// changes (no state callback runs), and what the PSN service answers
// fails as it does for a signed-out user (SCE_NP_ERROR_SIGNED_OUT).
// Argument checks, error codes and request ids are libSceNpManager
// 11.00's.

#include <mutex>
#include <set>

#include "orbis/hle.h"
#include "runtime/runtime.h"

namespace {

constexpr uint32_t kErrInvalidArgument = 0x80550003, kErrSignedOut = 0x80550006,
                   kErrCallbackRegistered = 0x80550008, kErrCallbackNotRegistered = 0x80550009,
                   kErrInvalidSize = 0x80550011, kErrRequestMax = 0x80550013,
                   kErrRequestNotFound = 0x80550014;
constexpr uint32_t kStateSignedOut = 1;
constexpr int32_t kUserInvalid = -1;
constexpr uint32_t kRequestBase = 0x20000000, kMaxRequests = 32;

std::mutex g_mu;
recomp::gaddr g_state_callback = 0;
std::set<uint32_t> g_requests;

}  // namespace

using recomp::hle::Arg;

// int sceNpSetNpTitleId(const SceNpTitleId* id, const SceNpTitleSecret*
// secret)
ORBIS_HLE("libSceNpManager", sceNpSetNpTitleId, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrInvalidArgument;
  return 0;
}

// int sceNpGetState(SceUserServiceUserId user, SceNpState* state)
ORBIS_HLE("libSceNpManager", sceNpGetState, 2) {
  if (int32_t(Arg(c, 0)) == kUserInvalid || !Arg(c, 1)) return kErrInvalidArgument;
  recomp::Wr32(Arg(c, 1), kStateSignedOut);
  return 0;
}

// int sceNpRegisterStateCallback(SceNpStateCallback cb, void* arg): one
// at a time; it would run from sceNpCheckCallback on a change.
ORBIS_HLE("libSceNpManager", sceNpRegisterStateCallback, 2) {
  if (!Arg(c, 0)) return kErrInvalidArgument;
  std::lock_guard<std::mutex> l(g_mu);
  if (g_state_callback) return kErrCallbackRegistered;
  g_state_callback = Arg(c, 0);
  return 0;
}

ORBIS_HLE("libSceNpManager", sceNpUnregisterStateCallback, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_state_callback) return kErrCallbackNotRegistered;
  g_state_callback = 0;
  return 0;
}

// Delivers pending callbacks: the state never changes.
ORBIS_HLE("libSceNpManager", sceNpCheckCallback, 0) { return 0; }

// int sceNpGetNpId(SceUserServiceUserId user, SceNpId* id), and the online
// id the NpId begins with.
ORBIS_HLE("libSceNpManager", sceNpGetNpId, 2) {
  if (int32_t(Arg(c, 0)) == kUserInvalid || !Arg(c, 1)) return kErrInvalidArgument;
  return kErrSignedOut;
}
ORBIS_HLE("libSceNpManager", sceNpGetOnlineId, 2) {
  if (int32_t(Arg(c, 0)) == kUserInvalid || !Arg(c, 1)) return kErrInvalidArgument;
  return kErrSignedOut;
}

// int sceNpCreateRequest(void): the smallest free id above 0x20000000.
ORBIS_HLE("libSceNpManager", sceNpCreateRequest, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_requests.size() >= kMaxRequests) return kErrRequestMax;
  uint32_t id = kRequestBase + 1;
  while (g_requests.count(id)) id++;
  g_requests.insert(id);
  return id;
}

ORBIS_HLE("libSceNpManager", sceNpDeleteRequest, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_requests.erase(uint32_t(Arg(c, 0))) ? 0 : kErrRequestNotFound;
}

// int sceNpCheckNpAvailability(int request, const SceNpOnlineId* id, bool*
// available): there is no such signed-in user.
ORBIS_HLE("libSceNpManager", sceNpCheckNpAvailability, 3) {
  if (!Arg(c, 0) || !Arg(c, 1)) return kErrInvalidArgument;
  if (Arg(c, 2)) recomp::GPtr(Arg(c, 2))[0] = 0;
  return kErrSignedOut;
}

// int sceNpSetContentRestriction(const SceNpContentRestriction* r): size
// 0x18, a default age (signed byte) and up to 256 per-country ages.
ORBIS_HLE("libSceNpManager", sceNpSetContentRestriction, 1) {
  const recomp::gaddr r = Arg(c, 0);
  if (!r) return kErrInvalidArgument;
  if (recomp::Rd64(r) != 0x18) return kErrInvalidSize;
  const uint32_t ages = recomp::Rd32(r + 12);
  if (int8_t(recomp::GPtr(r + 8)[0]) < 0 || ages > 256) return kErrInvalidSize;
  if (ages && !recomp::Rd64(r + 16)) return kErrInvalidArgument;
  return 0;
}
