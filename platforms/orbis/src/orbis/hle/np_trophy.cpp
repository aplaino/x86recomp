// libSceNpTrophy HLE: trophies work without PSN, as on a console that is
// offline. Contexts and handles are numbered from 1 here (the trophy
// service numbers contexts on the console); unlocked trophies are kept
// in DataDir()/trophies/<service label>.txt, one id per line. The trophy
// pack of a service label is app0's sce_sys/trophy/trophyNN.trp, NN the
// index of the label's entry in sce_sys/npbind.dat (a package's layout);
// only its table of contents is read (TrophyCount), as what it describes
// (names, grades, the platinum trophy) is encrypted. Argument checks and
// codes are libSceNpTrophy 11.00's.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "orbis/hle.h"
#include "runtime/log.h"
#include "x86/runtime.h"

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
  int32_t count = -1;  // the pack's trophies, -1 until read
  std::set<int32_t> unlocked;
};

std::mutex g_mu;
std::map<int32_t, Context> g_contexts;
std::set<int32_t> g_handles;
int32_t g_next_context = 1, g_next_handle = 1;

std::string File(uint32_t label) {
  return recomp::DataDir() + "/trophies/" + std::to_string(label) + ".txt";
}

std::vector<uint8_t> ReadAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) recomp::FatalF("libSceNpTrophy: cannot read %s", path.c_str());
  return {std::istreambuf_iterator<char>(f), {}};
}

uint64_t Be(const std::vector<uint8_t>& b, size_t at, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v = v << 8 | b.at(at + i);
  return v;
}

// The number of trophies in the pack of a service label: the pack's
// TROPnnn.PNG entries (every trophy has its icon; a console's count for a
// pack matches). npbind.dat (big-endian): magic 0xD294A018, the entry
// size at 0x10, the entry count at 0x18, the entries from 0x80, each a
// run of tags (u16 type, u16 size, data); tag 0x10 is the NP
// communication id, 0x11 the service label in decimal. The pack (TRP,
// big-endian): magic 0xDCA24D00, version, file size, the entry count at
// 0x10 and entry size at 0x14; in version 3 the entries follow at 0x60,
// each starting with its 32-byte name.
int32_t TrophyCount(uint32_t label) {
  const std::string sys = recomp::g_proc->game_dir + "/sce_sys/";
  const std::vector<uint8_t> bind = ReadAll(sys + "npbind.dat");
  if (bind.size() < 0x80 || Be(bind, 0, 4) != 0xD294A018)
    recomp::FatalF("libSceNpTrophy: %snpbind.dat is not an npbind file", sys.c_str());
  const uint64_t esize = Be(bind, 0x10, 8), n = Be(bind, 0x18, 8);
  int index = -1;
  for (uint64_t i = 0; i < n && index < 0; i++) {
    const size_t e = 0x80 + i * esize;
    for (size_t t = e; t + 4 <= e + esize;) {
      const uint32_t type = uint32_t(Be(bind, t, 2)), size = uint32_t(Be(bind, t + 2, 2));
      if (!type) break;
      if (type == 0x11 &&
          std::strtoul(std::string(bind.begin() + t + 4, bind.begin() + t + 4 + size).c_str(),
                       nullptr, 10) == label)
        index = int(i);
      t += 4 + size;
    }
  }
  if (index < 0) recomp::FatalF("libSceNpTrophy: npbind.dat has no service label %u", label);
  char name[32];
  std::snprintf(name, sizeof name, "trophy/trophy%02d.trp", index);
  const std::vector<uint8_t> trp = ReadAll(sys + name);
  if (trp.size() < 0x60 || Be(trp, 0, 4) != 0xDCA24D00)
    recomp::FatalF("libSceNpTrophy: %s%s is not a trophy pack", sys.c_str(), name);
  if (Be(trp, 4, 4) != 3)
    recomp::FatalF("libSceNpTrophy: trophy pack version %u not implemented yet",
                   unsigned(Be(trp, 4, 4)));
  const uint32_t entries = uint32_t(Be(trp, 0x10, 4)), entry_size = uint32_t(Be(trp, 0x14, 4));
  int32_t count = 0;
  for (uint32_t i = 0; i < entries; i++) {
    const size_t e = 0x60 + size_t(i) * entry_size;
    if (e + 32 > trp.size()) recomp::FatalF("libSceNpTrophy: %s%s is cut short", sys.c_str(), name);
    const char* s = reinterpret_cast<const char*>(&trp[e]);
    if (strnlen(s, 32) == 11 && !std::memcmp(s, "TROP", 4) && !std::memcmp(s + 7, ".PNG", 4) &&
        std::isdigit(uint8_t(s[4])) && std::isdigit(uint8_t(s[5])) && std::isdigit(uint8_t(s[6])))
      count++;
  }
  return count;
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

// int sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext ctx,
// SceNpTrophyHandle h, SceNpTrophyFlagArray* flags, uint32_t* count):
// flags (128 bits, trophy n at bit n % 32 of word n / 32) and the pack's
// trophy count, each written only when given.
ORBIS_HLE("libSceNpTrophy", sceNpTrophyGetTrophyUnlockState, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  if (int32_t(Arg(c, 0)) == -1) return kErrInvalidContext;
  if (int32_t(Arg(c, 1)) == -1) return kErrInvalidHandle;
  const recomp::gaddr flags = Arg(c, 2), count = Arg(c, 3);
  if (!flags && !count) return kErrInvalidArgument;
  if (!g_handles.count(int32_t(Arg(c, 1)))) return kErrInvalidHandle;
  auto it = g_contexts.find(int32_t(Arg(c, 0)));
  if (it == g_contexts.end() || !it->second.registered) return kErrInvalidContext;
  Context& ctx = it->second;
  if (ctx.count < 0) ctx.count = TrophyCount(ctx.label);
  if (flags) {
    uint32_t words[4] = {};
    for (int32_t id : ctx.unlocked) words[id >> 5] |= 1u << (id & 31);
    for (int i = 0; i < 4; i++) recomp::Wr32(flags + 4 * i, words[i]);
  }
  if (count) recomp::Wr32(count, uint32_t(ctx.count));
  return 0;
}
