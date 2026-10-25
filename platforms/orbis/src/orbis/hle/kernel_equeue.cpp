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
#include "runtime/hle/kernel32_state.h"
#include "runtime/runtime.h"

namespace recomp::orbis::kernel {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint16_t kEvClear = 0x20;  // FreeBSD EV_CLEAR

struct Event {
  uint64_t ident;
  int16_t filter;
  uint64_t udata;
  bool triggered = false;
  int64_t data = 0;
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
    recomp::Wr16(k + 10, kEvClear);
    recomp::Wr32(k + 12, 0);
    recomp::Wr64(k + 16, uint64_t(e.data));
    recomp::Wr64(k + 24, e.udata);
    e.triggered = false;
    e.data = 0;
  }
  if (out) recomp::Wr32(out, uint32_t(n));
  return n ? 0 : SceError(kETIMEDOUT);
}
