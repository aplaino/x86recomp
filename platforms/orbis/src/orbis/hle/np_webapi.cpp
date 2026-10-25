// libSceNpWebApi HLE: PSN's web API for a user not signed in. Library
// contexts count up from 1, user contexts are (library << 16) | a counter
// shared by all libraries (1-0xFFFF), as libSceNpWebApi 11.00 numbers
// them; a user context can be made for any online id, and what would
// reach the server fails as for a signed-out user.

#include <map>
#include <mutex>
#include <set>

#include "orbis/hle.h"
#include "runtime/runtime.h"

namespace {

constexpr uint32_t kErrInvalidArgument = 0x80552902, kErrInvalidLibContextId = 0x80552903,
                   kErrLibContextNotFound = 0x80552904, kErrUserContextNotFound = 0x80552905;

std::mutex g_mu;
uint32_t g_last_lib = 0, g_last_user = 0;
std::set<uint32_t> g_libs, g_users;

}  // namespace

using recomp::hle::Arg;

// int sceNpWebApiInitialize(int http_ctx, size_t pool_size): a library
// context id.
ORBIS_HLE("libSceNpWebApi", sceNpWebApiInitialize, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  do {
    g_last_lib = g_last_lib >= 0x7FFF ? 1 : g_last_lib + 1;
  } while (g_libs.count(g_last_lib));
  g_libs.insert(g_last_lib);
  return g_last_lib;
}

ORBIS_HLE("libSceNpWebApi", sceNpWebApiTerminate, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_libs.erase(uint32_t(Arg(c, 0)))) return kErrLibContextNotFound;
  return 0;
}

// int sceNpWebApiCreateContext(int lib_ctx, const SceNpOnlineId* id): a
// user context id.
ORBIS_HLE("libSceNpWebApi", sceNpWebApiCreateContext, 2) {
  const uint32_t lib = uint32_t(Arg(c, 0));
  if (lib - 1 >= 0x7FFF) return kErrInvalidLibContextId;
  if (!Arg(c, 1)) return kErrInvalidArgument;
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_libs.count(lib)) return kErrLibContextNotFound;
  g_last_user = g_last_user >= 0xFFFF ? 1 : g_last_user + 1;
  const uint32_t id = lib << 16 | g_last_user;
  g_users.insert(id);
  return id;
}

ORBIS_HLE("libSceNpWebApi", sceNpWebApiDeleteContext, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_users.erase(uint32_t(Arg(c, 0))) ? 0 : kErrUserContextNotFound;
}
