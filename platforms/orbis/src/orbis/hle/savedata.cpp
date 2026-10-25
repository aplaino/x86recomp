// libSceSaveData HLE: the user's save data directories below
// DataDir()/savedata/<user>/, each with a <dir>.meta beside it (its block
// count and SceSaveDataParam) and its icon, <dir>.png. A mount maps
// /savedataN (the lowest free N) onto the directory through libkernel's
// files. The checks of the arguments and their error codes are
// libSceSaveData 11.00's for the game's SDK; the system service's answers
// (not found, exists, busy, ...) are the SDK's codes for them. Save data
// of other titles is not implemented yet.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kErrParameter = 0x809F0000, kErrNotInitialized = 0x809F0001,
                   kErrBusy = 0x809F0003, kErrNotMounted = 0x809F0004,
                   kErrExists = 0x809F0007, kErrNotFound = 0x809F0008,
                   kErrMountFull = 0x809F000C, kErrInvalidLoginUser = 0x809F0011;
constexpr uint32_t kModeRdOnly = 1, kModeRdWr = 2, kModeCreate = 4, kModeCopyIcon = 0x10;
constexpr uint32_t kMountCreated = 1;
constexpr int kMaxMounts = 16;
constexpr uint64_t kMinBlocks = 96;
constexpr size_t kParamSize = 0x530;  // SceSaveDataParam
constexpr uint32_t kUser = 1;         // the one user (libSceUserService)

struct Mounted {
  std::string dir, guest;
  bool writable;
};

std::mutex g_mu;
bool g_init = false;
Mounted* g_mounts[kMaxMounts] = {};

std::string Root(uint32_t user) { return recomp::DataDir() + "/savedata/" + std::to_string(user); }
std::string Root() { return Root(kUser); }

bool Zero(recomp::gaddr p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (recomp::GPtr(p)[i]) return false;
  return true;
}

// A directory name: 1-31 of A-Z, a-z, 0-9, '-' and '_' (a search may also
// use the wildcard '%').
bool ValidDirName(const std::string& s, bool pattern) {
  if (s.empty() || s.size() > 31) return false;
  for (char ch : s)
    if (!std::isalnum(uint8_t(ch)) && ch != '-' && ch != '_' && !(pattern && ch == '%'))
      return false;
  return true;
}

// The common checks: a signed-in user, the title id (null: the game's
// own; else 9 characters and zeros up to 16) and the directory name.
uint32_t CheckUserTitleDir(recomp::gaddr p, bool dir_required, bool pattern) {
  if (recomp::Rd32(p) != kUser) return kErrInvalidLoginUser;
  if (const recomp::gaddr title = recomp::Rd64(p + 8)) {
    if (!Zero(title + 10, 6)) return kErrParameter;
    recomp::FatalF("save data of title %s not implemented yet", recomp::GStr(title).c_str());
  }
  const recomp::gaddr dir = recomp::Rd64(p + 16);
  if (!dir) return dir_required ? kErrParameter : 0;
  return ValidDirName(recomp::GStr(dir), pattern) ? 0 : kErrParameter;
}

struct Meta {
  uint64_t blocks = 0;
  uint8_t param[kParamSize] = {};
};

bool ReadMeta(const std::string& dir, Meta* m) {
  std::ifstream f(Root() + "/" + dir + ".meta", std::ios::binary);
  return f.read(reinterpret_cast<char*>(m), sizeof *m).good();
}
void WriteMeta(const std::string& dir, const Meta& m) {
  std::ofstream(Root() + "/" + dir + ".meta", std::ios::binary)
      .write(reinterpret_cast<const char*>(&m), sizeof m);
}

// 32 KiB blocks the directory's files take.
uint64_t UsedBlocks(const std::string& dir) {
  uint64_t n = 0;
  for (const auto& e : fs::recursive_directory_iterator(Root() + "/" + dir))
    if (e.is_regular_file()) n += (e.file_size() + 0x7FFF) / 0x8000;
  return n;
}

Mounted* FindMount(recomp::gaddr mount_point) {
  const std::string mp = recomp::GStr(mount_point);
  for (Mounted* m : g_mounts)
    if (m && m->guest == mp) return m;
  return nullptr;
}

// A mount point argument: a string shorter than 16 bytes.
bool ValidMountPoint(recomp::gaddr p) {
  if (!p) return false;
  for (int i = 0; i < 16; i++)
    if (!recomp::GPtr(p)[i]) return i > 0;
  return false;
}

}  // namespace

using recomp::hle::Arg;
using recomp::gaddr;

// int sceSaveDataInitialize(const void* reserved): again is no error.
ORBIS_HLE("libSceSaveData", sceSaveDataInitialize, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  g_init = true;
  fs::create_directories(Root());
  return 0;
}

ORBIS_HLE("libSceSaveData", sceSaveDataTerminate, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  g_init = false;
  return 0;
}

// int sceSaveDataMount(const SceSaveDataMount* m, SceSaveDataMountResult*
// r): m is {user, title id*, directory name*, fingerprint*, blocks, mode,
// 32 zero bytes}; r {mount point[16], required blocks, unused, status, 28
// zero bytes}.
ORBIS_HLE("libSceSaveData", sceSaveDataMount, 2) {
  const gaddr m = Arg(c, 0), r = Arg(c, 1);
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  if (!m) return kErrParameter;
  if (uint32_t e = CheckUserTitleDir(m, true, false)) return e;
  const uint64_t blocks = recomp::Rd64(m + 0x20);
  const uint32_t mode = recomp::Rd32(m + 0x28);
  if (!mode || (mode & 3) == 3 || (mode & 5) == 5 || (mode & 0x14) == 0x10 ||
      (mode & kModeCreate && blocks < kMinBlocks) || !Zero(m + 0x2C, 32))
    return kErrParameter;
  if (!!recomp::Rd64(m + 8) != !!recomp::Rd64(m + 0x18)) return kErrParameter;
  if (!r || !Zero(r + 0x20, 28)) return kErrParameter;
  if (mode & ~0x1Fu) recomp::FatalF("sceSaveDataMount: mode %X not implemented yet", mode);
  const std::string dir = recomp::GStr(recomp::Rd64(m + 16));
  const std::string host = Root() + "/" + dir;
  for (Mounted* mt : g_mounts)
    if (mt && mt->dir == dir) return kErrBusy;
  int slot = 0;
  while (slot < kMaxMounts && g_mounts[slot]) slot++;
  if (slot == kMaxMounts) return kErrMountFull;
  const bool exists = fs::is_directory(host);
  uint32_t status = 0;
  if (mode & kModeCreate) {
    if (exists) return kErrExists;
    fs::create_directories(host);
    Meta meta;
    meta.blocks = blocks;
    WriteMeta(dir, meta);
    if (mode & kModeCopyIcon) {
      const fs::path icon = recomp::g_proc->game_dir + "/sce_sys/save_data.png";
      if (fs::exists(icon)) fs::copy_file(icon, Root() + "/" + dir + ".png",
                                          fs::copy_options::overwrite_existing);
    }
    status = kMountCreated;
  } else if (!exists) {
    return kErrNotFound;
  }
  const std::string guest = "/savedata" + std::to_string(slot);
  g_mounts[slot] = new Mounted{dir, guest, !(mode & kModeRdOnly)};
  recomp::orbis::kernel::Mount(guest, host, mode & kModeRdOnly);
  std::memset(recomp::GPtr(r), 0, 16);
  recomp::PutStr(r, guest);
  recomp::Wr64(r + 0x10, 0);
  recomp::Wr32(r + 0x18, 0);
  recomp::Wr32(r + 0x1C, status);
  return 0;
}

// int sceSaveDataUmount(const SceSaveDataMountPoint* mp)
ORBIS_HLE("libSceSaveData", sceSaveDataUmount, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  if (!ValidMountPoint(Arg(c, 0))) return kErrParameter;
  for (Mounted*& m : g_mounts)
    if (m && m->guest == recomp::GStr(Arg(c, 0))) {
      recomp::orbis::kernel::Unmount(m->guest);
      delete m;
      m = nullptr;
      return 0;
    }
  return kErrNotMounted;
}

// int sceSaveDataSetParam(const SceSaveDataMountPoint* mp, int type, const
// void* buf, size_t size): 0 all (SceSaveDataParam), 1 title (128 bytes),
// 2 subtitle (128), 3 detail (1024), 4 the user parameter (u32).
ORBIS_HLE("libSceSaveData", sceSaveDataSetParam, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  const uint32_t type = uint32_t(Arg(c, 1));
  const gaddr buf = Arg(c, 2);
  const uint64_t size = Arg(c, 3);
  if (!ValidMountPoint(Arg(c, 0)) || type > 4 || !buf || !size || size > 0x1C800)
    return kErrParameter;
  Mounted* m = FindMount(Arg(c, 0));
  if (!m) return kErrNotMounted;
  Meta meta;
  ReadMeta(m->dir, &meta);
  static const size_t kAt[5] = {0, 0, 0x80, 0x100, 0x500}, kSize[5] = {kParamSize, 0x80, 0x80,
                                                                        0x400, 4};
  const size_t n = std::min<size_t>(size, kSize[type]);
  std::memcpy(meta.param + kAt[type], recomp::GPtr(buf), n);
  WriteMeta(m->dir, meta);
  return 0;
}

// int sceSaveDataSaveIcon(const SceSaveDataMountPoint* mp, const
// SceSaveDataIcon* icon): icon is {buf*, buf size, data size, 32 zero
// bytes}; data size 1 to 0x1C800 and no more than the buffer.
ORBIS_HLE("libSceSaveData", sceSaveDataSaveIcon, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  const gaddr icon = Arg(c, 1);
  if (!ValidMountPoint(Arg(c, 0)) || !icon) return kErrParameter;
  const gaddr buf = recomp::Rd64(icon);
  const uint64_t cap = recomp::Rd64(icon + 8), size = recomp::Rd64(icon + 16);
  if (!buf || !cap || size - 1 >= 0x1C800 || size > cap || !Zero(icon + 0x18, 32))
    return kErrParameter;
  Mounted* m = FindMount(Arg(c, 0));
  if (!m) return kErrNotMounted;
  std::ofstream(Root() + "/" + m->dir + ".png", std::ios::binary)
      .write(reinterpret_cast<const char*>(recomp::GPtr(buf)), std::streamsize(size));
  return 0;
}

// int sceSaveDataDelete(const SceSaveDataDelete* d): {user, title id*,
// directory name*, unused, 32 zero bytes}.
ORBIS_HLE("libSceSaveData", sceSaveDataDelete, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  const gaddr d = Arg(c, 0);
  if (!d) return kErrParameter;
  if (uint32_t e = CheckUserTitleDir(d, true, false)) return e;
  if (!Zero(d + 0x1C, 32)) return kErrParameter;
  const std::string dir = recomp::GStr(recomp::Rd64(d + 16));
  for (Mounted* m : g_mounts)
    if (m && m->dir == dir) return kErrBusy;
  if (!fs::is_directory(Root() + "/" + dir)) return kErrNotFound;
  fs::remove_all(Root() + "/" + dir);
  fs::remove(Root() + "/" + dir + ".meta");
  fs::remove(Root() + "/" + dir + ".png");
  return 0;
}

// int sceSaveDataDirNameSearch(const SceSaveDataDirNameSearchCond* cond,
// SceSaveDataDirNameSearchResult* r): cond is {user, title id*, name
// pattern* ('%' matches any run; null: all), key (0 name, 1 user
// parameter, 2 blocks, 3 time, 4 free blocks, 5), order (0 ascending,
// 1 descending), 32 zero bytes}; r {hits, names* (32 bytes each),
// names' room (1-1024), names set, params*, infos* ({blocks, free
// blocks, 32 bytes}), 12 zero bytes}.
ORBIS_HLE("libSceSaveData", sceSaveDataDirNameSearch, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  const gaddr cond = Arg(c, 0), r = Arg(c, 1);
  if (!cond) return kErrParameter;
  if (uint32_t e = CheckUserTitleDir(cond, false, true)) return e;
  const uint32_t key = recomp::Rd32(cond + 0x18), order = recomp::Rd32(cond + 0x1C);
  if (key > 5 || order > 1 || !Zero(cond + 0x20, 32)) return kErrParameter;
  const uint32_t room = r ? recomp::Rd32(r + 0x10) : 0;
  if (!r || !recomp::Rd64(r + 8) || room - 1 >= 0x400 || !Zero(r + 0x28, 12))
    return kErrParameter;
  const gaddr pat = recomp::Rd64(cond + 16);
  const std::string pattern = pat ? recomp::GStr(pat) : "%";
  // '%' matches any run of characters.
  auto match = [](const std::string& p, const std::string& s) {
    std::vector<std::vector<bool>> t(p.size() + 1, std::vector<bool>(s.size() + 1));
    t[0][0] = true;
    for (size_t i = 1; i <= p.size(); i++)
      for (size_t j = 0; j <= s.size(); j++)
        t[i][j] = p[i - 1] == '%' ? t[i - 1][j] || (j && t[i][j - 1])
                                  : j && t[i - 1][j - 1] && p[i - 1] == s[j - 1];
    return t[p.size()][s.size()];
  };
  struct Hit {
    std::string name;
    Meta meta;
    uint64_t mtime;
  };
  std::vector<Hit> hits;
  for (const auto& e : fs::directory_iterator(Root())) {
    if (!e.is_directory()) continue;
    const std::string name = e.path().filename().string();
    if (!match(pattern, name)) continue;
    Hit h{name, {}, uint64_t(fs::last_write_time(e.path()).time_since_epoch().count())};
    ReadMeta(name, &h.meta);
    hits.push_back(h);
  }
  auto user_param = [](const Hit& h) {
    uint32_t v;
    std::memcpy(&v, h.meta.param + 0x500, 4);
    return v;
  };
  std::stable_sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) {
    switch (key) {
      case 1: return user_param(a) < user_param(b);
      case 2: case 4: return a.meta.blocks < b.meta.blocks;
      case 3: return a.mtime < b.mtime;
      default: return a.name < b.name;
    }
  });
  if (order == 1) std::reverse(hits.begin(), hits.end());
  const uint32_t set = std::min<uint32_t>(uint32_t(hits.size()), room);
  const gaddr names = recomp::Rd64(r + 8), params = recomp::Rd64(r + 0x18),
              infos = recomp::Rd64(r + 0x20);
  for (uint32_t i = 0; i < set; i++) {
    std::memset(recomp::GPtr(names + 32 * uint64_t(i)), 0, 32);
    recomp::PutStr(names + 32 * uint64_t(i), hits[i].name);
    if (params) std::memcpy(recomp::GPtr(params + kParamSize * i), hits[i].meta.param, kParamSize);
    if (infos) {
      std::memset(recomp::GPtr(infos + 48 * uint64_t(i)), 0, 48);
      recomp::Wr64(infos + 48 * uint64_t(i), hits[i].meta.blocks);
      recomp::Wr64(infos + 48 * uint64_t(i) + 8, hits[i].meta.blocks - UsedBlocks(hits[i].name));
    }
  }
  recomp::Wr32(r, uint32_t(hits.size()));
  recomp::Wr32(r + 0x14, set);
  return 0;
}

// Save data memory: one block of up to 8 MiB per user that the system
// keeps (here: DataDir()/savedata/<user>/memory.dat, zeros when new).

// int sceSaveDataSetupSaveDataMemory(SceUserServiceUserId user, size_t
// size, const SceSaveDataParam* param)
ORBIS_HLE("libSceSaveData", sceSaveDataSetupSaveDataMemory, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  if (uint32_t(Arg(c, 0)) != kUser) return kErrInvalidLoginUser;
  const uint64_t size = Arg(c, 1);
  if (size - 1 >= 0x800000) return kErrParameter;
  const std::string path = Root() + "/memory.dat";
  if (!fs::exists(path) || fs::file_size(path) < size) {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    f.close();
    fs::resize_file(path, size);
  }
  if (Arg(c, 2)) {
    std::ofstream(Root() + "/memory.param", std::ios::binary)
        .write(reinterpret_cast<const char*>(recomp::GPtr(Arg(c, 2))), kParamSize);
  }
  return 0;
}

// int sceSaveDataGetSaveDataMemory(user, void* buf, size_t size, int64_t
// offset) and Set: bytes offset .. offset + size of the block.
static uint64_t SaveDataMemory(recomp::Ctx& c, bool write) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInitialized;
  if (uint32_t(Arg(c, 0)) != kUser) return kErrInvalidLoginUser;
  const gaddr buf = Arg(c, 1);
  const uint64_t size = Arg(c, 2), off = Arg(c, 3);
  const std::string path = Root() + "/memory.dat";
  if (!buf || !size) return kErrParameter;
  if (!fs::exists(path) || off + size > fs::file_size(path)) return kErrParameter;
  std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
  f.seekg(std::streamoff(off));
  if (write)
    f.write(reinterpret_cast<const char*>(recomp::GPtr(buf)), std::streamsize(size));
  else
    f.read(reinterpret_cast<char*>(recomp::GPtr(buf)), std::streamsize(size));
  return 0;
}
ORBIS_HLE("libSceSaveData", sceSaveDataGetSaveDataMemory, 4) { return SaveDataMemory(c, false); }
ORBIS_HLE("libSceSaveData", sceSaveDataSetSaveDataMemory, 4) { return SaveDataMemory(c, true); }
