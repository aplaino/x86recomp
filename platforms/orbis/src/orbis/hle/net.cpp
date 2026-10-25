// libSceNet HLE: the network library on a console with no network
// connection. Errors are 0x80410100 + the errno (FreeBSD's, and the
// library's own from 200 on), also left in the calling thread's net errno
// (sceNetErrnoLoc). Pools are flexible memory the library would carve
// its buffers from; ids from 3000 as libSceNet 11.00 numbers them.

#include <cstring>
#include <map>
#include <mutex>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/runtime.h"

namespace {

constexpr int kENOTINIT = 200;
constexpr int kMaxPools = 32;

std::mutex g_mu;
bool g_init = false;
struct Pool {
  recomp::gaddr mem = 0;
  uint64_t size = 0;
};
std::map<int, Pool> g_pools;
std::map<uint32_t, recomp::gaddr> g_errno;  // by guest thread

recomp::gaddr ErrnoLoc() {
  const uint32_t tid = recomp::CurrentThread()->tid;
  std::lock_guard<std::mutex> l(g_mu);
  recomp::gaddr& a = g_errno[tid];
  if (!a) {
    a = recomp::g_proc->vm.Reserve(0, recomp::GuestMemory::kPageSize);
    if (!a || !recomp::g_proc->vm.Commit(a, recomp::GuestMemory::kPageSize, recomp::Prot::RW))
      recomp::FatalF("libSceNet: out of guest memory");
  }
  return a;
}

uint64_t Error(int e) {
  recomp::Wr32(ErrnoLoc(), uint32_t(e));
  return 0x80410100 + uint32_t(e);
}

}  // namespace

using recomp::hle::Arg;

ORBIS_HLE("libSceNet", sceNetInit, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  g_init = true;
  return 0;
}
ORBIS_HLE("libSceNet", sceNetTerm, 0) { return 0; }

ORBIS_HLE("libSceNet", sceNetErrnoLoc, 0) { return ErrnoLoc(); }

// int sceNetPoolCreate(const char* name, int size, int flags): at least
// 4 KiB (rounded up to 16 KiB), no flags, a name of up to 31 bytes.
ORBIS_HLE("libSceNet", sceNetPoolCreate, 3) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_init) return Error(kENOTINIT);
  }
  using namespace recomp::orbis::kernel;
  const int size = int(Arg(c, 1));
  if (!Arg(c, 0) || size < 0x1000 || int(Arg(c, 2))) return Error(kEINVAL);
  if (recomp::GStr(Arg(c, 0)).size() > 31) return Error(kENAMETOOLONG);
  const uint64_t bytes = (uint64_t(size) + 0x3FFF) & ~uint64_t(0x3FFF);
  std::lock_guard<std::mutex> l(g_mu);
  int id = 0;
  while (id < kMaxPools && g_pools.count(3000 + id)) id++;
  if (id == kMaxPools) return Error(kENOMEM);
  Pool p{recomp::g_proc->vm.Reserve(0, bytes), bytes};
  if (!p.mem || !recomp::g_proc->vm.Commit(p.mem, bytes, recomp::Prot::RW)) return Error(kENOMEM);
  g_pools[3000 + id] = p;
  return uint64_t(3000 + id);
}

ORBIS_HLE("libSceNet", sceNetPoolDestroy, 1) {
  using namespace recomp::orbis::kernel;
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return Error(kENOTINIT);
  auto it = g_pools.find(int(Arg(c, 0)));
  if (it == g_pools.end()) return Error(kEBADF);
  recomp::g_proc->vm.Release(it->second.mem);
  g_pools.erase(it);
  return 0;
}

ORBIS_HLE("libSceNet", sceNetHtons, 1) {
  const uint16_t v = uint16_t(Arg(c, 0));
  return uint16_t(v >> 8 | v << 8);
}
ORBIS_HLE("libSceNet", sceNetNtohs, 1) {
  const uint16_t v = uint16_t(Arg(c, 0));
  return uint16_t(v >> 8 | v << 8);
}

// int sceNetInetPton(int af, const char* src, void* dst): IPv4 dotted
// quads (AF_INET 2); 1 if parsed, 0 if not.
ORBIS_HLE("libSceNet", sceNetInetPton, 3) {
  if (int(Arg(c, 0)) != 2) recomp::FatalF("sceNetInetPton: address family %d", int(Arg(c, 0)));
  const std::string s = recomp::GStr(Arg(c, 1));
  uint32_t parts[4], n = 0, v = 0;
  bool digit = false;
  for (size_t i = 0; i <= s.size(); i++) {
    const char ch = i < s.size() ? s[i] : '.';
    if (ch >= '0' && ch <= '9') {
      v = v * 10 + uint32_t(ch - '0');
      if (v > 255) return 0;
      digit = true;
    } else if (ch == '.' && digit && n < 4) {
      parts[n++] = v;
      v = 0;
      digit = false;
    } else {
      return 0;
    }
  }
  if (n != 4) return 0;
  for (int i = 0; i < 4; i++) recomp::GPtr(Arg(c, 2))[i] = uint8_t(parts[i]);
  return 1;
}

// ---- libSceNetCtl: the connection is down (state 0, disconnected) and
// never changes, so callbacks never run; what needs a connection answers
// SCE_NET_CTL_ERROR_NOT_CONNECTED. Checks and codes are libSceNetCtl
// 11.00's.

namespace {

constexpr uint32_t kCtlNotInitialized = 0x80412101, kCtlCallbackMax = 0x80412103,
                   kCtlIdNotFound = 0x80412104, kCtlInvalidId = 0x80412105,
                   kCtlInvalidAddr = 0x80412107, kCtlNotConnected = 0x80412108,
                   kCtlInvalidSize = 0x80412111;
constexpr int kCtlCallbacks = 8;

bool g_ctl_init = false;
recomp::gaddr g_ctl_callbacks[kCtlCallbacks] = {};

}  // namespace

ORBIS_HLE("libSceNetCtl", sceNetCtlInit, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  g_ctl_init = true;
  return 0;
}
ORBIS_HLE("libSceNetCtl", sceNetCtlTerm, 0) { return 0; }

ORBIS_HLE("libSceNetCtl", sceNetCtlGetState, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_ctl_init) return kCtlNotInitialized;
  if (!Arg(c, 0)) return kCtlInvalidAddr;
  recomp::Wr32(Arg(c, 0), 0);
  return 0;
}

// int sceNetCtlRegisterCallback(SceNetCtlCallback cb, void* arg, int* cid)
ORBIS_HLE("libSceNetCtl", sceNetCtlRegisterCallback, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_ctl_init) return kCtlNotInitialized;
  if (!Arg(c, 0) || !Arg(c, 2)) return kCtlInvalidAddr;
  for (int i = 0; i < kCtlCallbacks; i++)
    if (!g_ctl_callbacks[i]) {
      g_ctl_callbacks[i] = Arg(c, 0);
      recomp::Wr32(Arg(c, 2), uint32_t(i));
      return 0;
    }
  return kCtlCallbackMax;
}

ORBIS_HLE("libSceNetCtl", sceNetCtlUnregisterCallback, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_ctl_init) return kCtlNotInitialized;
  const uint32_t id = uint32_t(Arg(c, 0));
  if (id >= kCtlCallbacks) return kCtlInvalidId;
  if (!g_ctl_callbacks[id]) return kCtlIdNotFound;
  g_ctl_callbacks[id] = 0;
  return 0;
}

ORBIS_HLE("libSceNetCtl", sceNetCtlCheckCallback, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_ctl_init ? 0 : kCtlNotInitialized;
}

// int sceNetCtlGetResult(int event, int* error): no event has failed.
ORBIS_HLE("libSceNetCtl", sceNetCtlGetResult, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_ctl_init) return kCtlNotInitialized;
  if (!Arg(c, 1)) return kCtlInvalidAddr;
  recomp::Wr32(Arg(c, 1), 0);
  return 0;
}

// int sceNetCtlGetInfo(int code, SceNetCtlInfo* info): the device (1:
// wired) and the link (4: down) are known without a connection.
ORBIS_HLE("libSceNetCtl", sceNetCtlGetInfo, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_ctl_init) return kCtlNotInitialized;
  if (!Arg(c, 1)) return kCtlInvalidAddr;
  switch (int(Arg(c, 0))) {
    case 1: recomp::Wr32(Arg(c, 1), 0); return 0;
    case 4: recomp::Wr32(Arg(c, 1), 0); return 0;
  }
  return kCtlNotConnected;
}

// int sceNetCtlGetNatInfo(SceNetCtlNatInfo* info): size 16.
ORBIS_HLE("libSceNetCtl", sceNetCtlGetNatInfo, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_ctl_init) return kCtlNotInitialized;
  if (!Arg(c, 0)) return kCtlInvalidAddr;
  if (recomp::Rd32(Arg(c, 0)) != 16) return kCtlInvalidSize;
  return kCtlNotConnected;
}

// ---- libSceSsl and libSceHttp: contexts as their 11.00 libraries number
// them, 128 slots taken round-robin after the last one, id = slot + 1
// (libSceHttp, for SDK 2.00 and later, adds a generation in bits 8-11).

namespace {

struct Slots {
  bool used[128] = {};
  int last = 127;
  uint32_t ids[128];
  Slots() {
    for (int i = 0; i < 128; i++) ids[i] = uint32_t(i + 1);
  }
  // A free slot after the last one taken, or -1.
  int Take() {
    for (int n = 1; n <= 128; n++) {
      const int s = (last + n) % 128;
      if (!used[s]) {
        used[s] = true;
        last = s;
        return s;
      }
    }
    return -1;
  }
  // The slot of id, or -1.
  int Find(uint32_t id) const {
    const uint32_t s = (id & 0xFF) - 1;
    return s < 128 && used[s] && ids[s] == id ? int(s) : -1;
  }
};

Slots g_ssl, g_http;

}  // namespace

// int sceSslInit(size_t pool_size): a context id.
ORBIS_HLE("libSceSsl", sceSslInit, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  const int s = g_ssl.Take();
  if (s < 0) recomp::FatalF("sceSslInit: no free context");
  return g_ssl.ids[s];
}

ORBIS_HLE("libSceSsl", sceSslTerm, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t id = uint32_t(Arg(c, 0));
  if (id - 1 >= 128) return 0x8095F006;
  const int s = g_ssl.Find(id);
  if (s < 0) return 0x8095F009;
  g_ssl.used[s] = false;
  return 0;
}

// int sceHttpInit(int net_pool, int ssl_ctx, size_t pool_size): a context
// id.
ORBIS_HLE("libSceHttp", sceHttpInit, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  const int s = g_http.Take();
  if (s < 0) recomp::FatalF("sceHttpInit: no free context");
  uint32_t& id = g_http.ids[s];
  id = (id & 0xFF) | ((id + 0x100) & 0xF00);
  return id;
}

ORBIS_HLE("libSceHttp", sceHttpTerm, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  const int s = g_http.Find(uint32_t(Arg(c, 0)));
  if (s < 0) recomp::FatalF("sceHttpTerm: context %X", uint32_t(Arg(c, 0)));
  g_http.used[s] = false;
  return 0;
}
