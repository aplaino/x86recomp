// libSceNpTrophy HLE: trophies work without PSN, as on a console that is
// offline. Contexts and handles are numbered from 1 here (the trophy
// service numbers contexts on the console); unlocked trophies are kept
// in DataDir()/trophies/<service label>.txt, one id per line. What needs
// the game's trophy pack (names, grades, icons: TROPHY.TRP, which lives
// outside the game's files) is not implemented yet. Argument checks and
// codes are libSceNpTrophy 11.00's.

#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "orbis/hle.h"
#include "runtime/runtime.h"

namespace {

constexpr uint32_t kErrInvalidArgument = 0x80551604, kErrInvalidHandle = 0x80551608,
                   kErrInvalidContext = 0x80551609, kErrInvalidServiceLabel = 0x80551621,
                   kErrInvalidUserId = 0x80551625, kErrInvalidTrophyId = 0x8055160A,
                   kErrTrophyAlreadyUnlocked = 0x8055160C;  // the service's (the SDK's code)
constexpr int32_t kInvalidTrophyId = -1;

struct Context {
  int32_t user;
  uint32_t label;
  bool registered = false;
  std::set<int32_t> unlocked;
};

std::mutex g_mu;
std::map<int32_t, Context> g_contexts;
std::set<int32_t> g_handles;
int32_t g_next_context = 1, g_next_handle = 1;

std::string File(uint32_t label) {
  return recomp::DataDir() + "/trophies/" + std::to_string(label) + ".txt";
}

}  // namespace

using recomp::hle::Arg;

// int sceNpTrophyCreateContext(SceNpTrophyContext* out, int user, uint32_t
// service_label, uint64_t options)
ORBIS_HLE("libSceNpTrophy", sceNpTrophyCreateContext, 4) {
  const recomp::gaddr out = Arg(c, 0);
  if (!out || Arg(c, 3)) return kErrInvalidArgument;
  if (int32_t(Arg(c, 1)) == -1) return kErrInvalidUserId;
  if (uint32_t(Arg(c, 2)) >= 100) return kErrInvalidServiceLabel;
  std::lock_guard<std::mutex> l(g_mu);
  recomp::Wr32(out, uint32_t(-1));
  const int32_t id = g_next_context++;
  Context& ctx = g_contexts[id];
  ctx.user = int32_t(Arg(c, 1));
  ctx.label = uint32_t(Arg(c, 2));
  std::ifstream f(File(ctx.label));
  for (int32_t t; f >> t;) ctx.unlocked.insert(t);
  recomp::Wr32(out, uint32_t(id));
  return 0;
}

ORBIS_HLE("libSceNpTrophy", sceNpTrophyDestroyContext, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_contexts.erase(int32_t(Arg(c, 0))) ? 0 : kErrInvalidContext;
}

ORBIS_HLE("libSceNpTrophy", sceNpTrophyCreateHandle, 1) {
  if (!Arg(c, 0)) return kErrInvalidArgument;
  std::lock_guard<std::mutex> l(g_mu);
  const int32_t h = g_next_handle++;
  g_handles.insert(h);
  recomp::Wr32(Arg(c, 0), uint32_t(h));
  return 0;
}

ORBIS_HLE("libSceNpTrophy", sceNpTrophyDestroyHandle, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_handles.erase(int32_t(Arg(c, 0))) ? 0 : kErrInvalidHandle;
}

// Nothing waits on the trophy service to abort.
ORBIS_HLE("libSceNpTrophy", sceNpTrophyAbortHandle, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_handles.count(int32_t(Arg(c, 0))) ? 0 : kErrInvalidHandle;
}

// int sceNpTrophyRegisterContext(SceNpTrophyContext ctx, SceNpTrophyHandle
// h, uint64_t options)
ORBIS_HLE("libSceNpTrophy", sceNpTrophyRegisterContext, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  if (int32_t(Arg(c, 1)) == -1 || !g_handles.count(int32_t(Arg(c, 1)))) return kErrInvalidHandle;
  if (Arg(c, 2)) return kErrInvalidArgument;
  auto it = g_contexts.find(int32_t(Arg(c, 0)));
  if (it == g_contexts.end()) return kErrInvalidContext;
  it->second.registered = true;
  return 0;
}

// int sceNpTrophyUnlockTrophy(SceNpTrophyContext ctx, SceNpTrophyHandle h,
// SceNpTrophyId id, SceNpTrophyId* platinum): the platinum trophy it
// would unlock in turn is not known without the trophy pack.
ORBIS_HLE("libSceNpTrophy", sceNpTrophyUnlockTrophy, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  if (int32_t(Arg(c, 0)) == -1) return kErrInvalidContext;
  if (int32_t(Arg(c, 1)) == -1) return kErrInvalidHandle;
  const int32_t id = int32_t(Arg(c, 2));
  if (uint32_t(id) > 0x7F) return kErrInvalidTrophyId;
  if (!Arg(c, 3)) return kErrInvalidArgument;
  if (!g_handles.count(int32_t(Arg(c, 1)))) return kErrInvalidHandle;
  auto it = g_contexts.find(int32_t(Arg(c, 0)));
  if (it == g_contexts.end() || !it->second.registered) return kErrInvalidContext;
  recomp::Wr32(Arg(c, 3), uint32_t(kInvalidTrophyId));
  if (!it->second.unlocked.insert(id).second) return kErrTrophyAlreadyUnlocked;
  recomp::MakeDirs(recomp::DataDir() + "/trophies");
  std::ofstream(File(it->second.label), std::ios::app) << id << "\n";
  recomp::WarnF("trophy %d unlocked\n", id);
  return 0;
}
