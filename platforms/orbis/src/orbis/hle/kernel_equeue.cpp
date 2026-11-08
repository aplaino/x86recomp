// libkernel HLE: event queues (sceKernel*Equeue), on which other system
// libraries deliver their events (kernel.h). A returned event is struct
// kevent: ident (8), filter (2), flags (2), fflags (4), data (8), udata
// (8).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "x86/runtime.h"

namespace recomp::orbis::kernel {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint16_t kEvClear = 0x20;  // FreeBSD EV_CLEAR
constexpr int16_t kEvfiltUser = -11;

struct Event {
  uint64_t ident;
  int16_t filter;
  uint64_t udata;
  bool triggered = false;
  int64_t data = 0;
  // EV_CLEAR: a wait that returns the event resets it. Only user events
  // can be without it (level: returned by every wait once triggered).
  bool clear = true;
};

struct Equeue {
  std::vector<Event> events;
};

// One lock for every queue: triggers look an event up in all of them.
// Never destroyed: threads may still wait at exit.
std::mutex& g_mu = *new std::mutex;
std::condition_variable& g_cv = *new std::condition_variable;
std::map<uint32_t, std::unique_ptr<Equeue>>& g_queues = *new std::map<uint32_t, std::unique_ptr<Equeue>>;
uint32_t g_next = 3;  // after stdin, stdout and stderr, as a descriptor

// The user event id in eq (g_mu held), or null; *bad_queue tells why.
Event* FindUser(uint32_t eq, uint64_t id, bool* bad_queue) {
  auto it = g_queues.find(eq);
  *bad_queue = it == g_queues.end();
  if (*bad_queue) return nullptr;
  for (Event& e : it->second->events)
    if (e.ident == id && e.filter == kEvfiltUser) return &e;
  return nullptr;
}

// sceKernelAddUserEvent(Edge): in 11.00 a kevent EV_ADD of EVFILT_USER
// (Edge: EV_ADD | EV_CLEAR), udata 0; adding an existing one keeps its
// state and mode and sets its udata, as FreeBSD's kqueue_register does.
uint64_t AddUser(uint32_t eq, uint64_t id, bool clear) {
  std::lock_guard<std::mutex> l(g_mu);
  bool bad;
  if (Event* e = FindUser(eq, id, &bad)) {
    e->udata = 0;
    return 0;
  }
  if (bad) return SceError(kEBADF);
  Event e{id, kEvfiltUser, 0};
  e.clear = clear;
  g_queues[eq]->events.push_back(e);
  return 0;
}

}  // namespace

bool AddEvent(uint32_t eq, uint64_t ident, int16_t filter, uint64_t udata) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_queues.find(eq);
  if (it == g_queues.end()) return false;
  auto& evs = it->second->events;
  auto e = std::find_if(evs.begin(), evs.end(),
                        [&](const Event& e) { return e.ident == ident && e.filter == filter; });
  if (e != evs.end())
    e->udata = udata;  // EV_ADD on an existing event changes it
  else
    evs.push_back({ident, filter, udata});
  return true;
}

bool DeleteEvent(uint32_t eq, uint64_t ident, int16_t filter) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_queues.find(eq);
  if (it == g_queues.end()) return false;
  auto& evs = it->second->events;
  auto e = std::find_if(evs.begin(), evs.end(),
                        [&](const Event& e) { return e.ident == ident && e.filter == filter; });
  if (e == evs.end()) return false;
  evs.erase(e);
  return true;
}

void TriggerEvent(uint64_t ident, int16_t filter, int64_t data) {
  std::lock_guard<std::mutex> l(g_mu);
  for (auto& [id, q] : g_queues)
    for (Event& e : q->events)
      if (e.ident == ident && e.filter == filter) {
        e.triggered = true;
        e.data = data;
      }
  g_cv.notify_all();
}

}  // namespace recomp::orbis::kernel

using namespace recomp::orbis::kernel;
using recomp::hle::Arg;

// int sceKernelCreateEqueue(SceKernelEqueue* out, const char* name)
ORBIS_HLE("libkernel", sceKernelCreateEqueue, 2) {
  if (!Arg(c, 0)) return SceError(kEINVAL);
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t id = g_next++;
  g_queues[id] = std::make_unique<Equeue>();
  recomp::Wr64(Arg(c, 0), id);
  return 0;
}

ORBIS_HLE("libkernel", sceKernelDeleteEqueue, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_queues.erase(uint32_t(Arg(c, 0)))) return SceError(kEBADF);
  g_cv.notify_all();
  return 0;
}

// int sceKernelWaitEqueue(SceKernelEqueue eq, SceKernelEvent* ev, int num,
// int* out, SceKernelUseconds* timeout): waits for at least one triggered
// event (null timeout: for ever; *timeout 0: polls) and returns up to num.
ORBIS_HLE("libkernel", sceKernelWaitEqueue, 5) {
  recomp::GuestSyncPoint sync;
  const uint32_t eq = uint32_t(Arg(c, 0));
  const int num = int(Arg(c, 2));
  const recomp::gaddr ev = Arg(c, 1), out = Arg(c, 3), timeout = Arg(c, 4);
  if (!ev || num < 1) return SceError(kEINVAL);
  std::unique_lock<std::mutex> l(g_mu);
  auto ready = [&] {
    auto it = g_queues.find(eq);
    if (it == g_queues.end()) return true;
    for (const Event& e : it->second->events)
      if (e.triggered) return true;
    return false;
  };
  if (g_queues.find(eq) == g_queues.end()) return SceError(kEBADF);
  if (!timeout) {
    g_cv.wait(l, ready);
  } else {
    const auto deadline = Clock::now() + std::chrono::microseconds(recomp::Rd32(timeout));
    g_cv.wait_until(l, deadline, ready);
  }
  auto it = g_queues.find(eq);
  if (it == g_queues.end()) return SceError(kEBADF);
  int n = 0;
  for (Event& e : it->second->events) {
    if (!e.triggered || n == num) continue;
    const recomp::gaddr k = ev + 32 * uint64_t(n++);
    recomp::Wr64(k, e.ident);
    recomp::Wr16(k + 8, uint16_t(e.filter));
    recomp::Wr16(k + 10, e.clear ? kEvClear : 0);
    recomp::Wr32(k + 12, 0);
    recomp::Wr64(k + 16, uint64_t(e.data));
    recomp::Wr64(k + 24, e.udata);
    if (e.clear) {
      e.triggered = false;
      e.data = 0;
    }
  }
  if (out) recomp::Wr32(out, uint32_t(n));
  return n ? 0 : SceError(kETIMEDOUT);
}

// User events: ids the guest picks, scoped to their queue. A trigger is a
// kevent of NOTE_TRIGGER (fflags 0x1000000), data 0, carrying the udata the
// event returns; an event not added is ENOENT, as kevent answers.
ORBIS_HLE("libkernel", sceKernelAddUserEvent, 2) {
  return AddUser(uint32_t(Arg(c, 0)), uint64_t(int64_t(int32_t(Arg(c, 1)))), false);
}
ORBIS_HLE("libkernel", sceKernelAddUserEventEdge, 2) {
  return AddUser(uint32_t(Arg(c, 0)), uint64_t(int64_t(int32_t(Arg(c, 1)))), true);
}
// int sceKernelTriggerUserEvent(SceKernelEqueue eq, int id, void* udata)
ORBIS_HLE("libkernel", sceKernelTriggerUserEvent, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  bool bad;
  Event* e = FindUser(uint32_t(Arg(c, 0)), uint64_t(int64_t(int32_t(Arg(c, 1)))), &bad);
  if (!e) return SceError(bad ? kEBADF : kENOENT);
  e->triggered = true;
  e->data = 0;
  e->udata = Arg(c, 2);
  g_cv.notify_all();
  return 0;
}
ORBIS_HLE("libkernel", sceKernelDeleteUserEvent, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  bool bad;
  Event* e = FindUser(uint32_t(Arg(c, 0)), uint64_t(int64_t(int32_t(Arg(c, 1)))), &bad);
  if (!e) return SceError(bad ? kEBADF : kENOENT);
  auto& evs = g_queues[uint32_t(Arg(c, 0))]->events;
  evs.erase(evs.begin() + (e - evs.data()));
  return 0;
}

// The fields of a returned SceKernelEvent (struct kevent).
ORBIS_HLE("libkernel", sceKernelGetEventId, 1) { return recomp::Rd64(Arg(c, 0)); }
ORBIS_HLE("libkernel", sceKernelGetEventFilter, 1) {
  return uint64_t(int64_t(int16_t(recomp::Rd16(Arg(c, 0) + 8))));
}
ORBIS_HLE("libkernel", sceKernelGetEventData, 1) { return recomp::Rd64(Arg(c, 0) + 16); }
ORBIS_HLE("libkernel", sceKernelGetEventUserData, 1) { return recomp::Rd64(Arg(c, 0) + 24); }
