// Dispatcher objects, waits, handles and time (kernel.h).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

using Clock = std::chrono::steady_clock;

// The dispatcher lock: every change of a signal state and every check of
// a waiter is made under it, and a change wakes every waiter to check
// again.
std::mutex g_dispatch;
std::condition_variable g_wake;

// Each thread's queued APCs, kernel and user, by KTHREAD; under the
// dispatcher lock. A thread that has ended keeps an entry, closed, until
// its KTHREAD goes.
struct QueuedApc {
  uint32_t apc;
  bool owned;
};
struct ApcQueues {
  std::deque<QueuedApc> q[2];  // [kKernelMode], [kUserMode]
  bool closed = false;
};
std::unordered_map<uint32_t, ApcQueues> g_apcs;

// Whether a wait of the current thread has an APC to run: a kernel one
// outside a critical region, a user one in an alertable wait.
bool ApcDue(uint32_t kthread, bool alertable) {
  auto it = g_apcs.find(kthread);
  if (it == g_apcs.end()) return false;
  if (!it->second.q[kKernelMode].empty() && int32_t(Rd32(kthread + kKtKernelApcDisable)) >= 0) return true;
  return alertable && !it->second.q[kUserMode].empty();
}

int32_t State(uint32_t o) { return int32_t(Rd32(o + 4)); }
void SetState(uint32_t o, int32_t v) { Wr32(o + 4, uint32_t(v)); }

bool Signaled(uint32_t o, uint32_t kthread) {
  if (Rd8(o) == kMutantObject) return State(o) > 0 || Rd32(o + 0x18) == kthread;
  return State(o) > 0;
}
// Takes what a satisfied wait takes from the object; true if it was an
// abandoned mutant.
bool Acquire(uint32_t o, uint32_t kthread) {
  switch (Rd8(o)) {
    case kSynchronizationEvent:
    case kSynchronizationTimer: SetState(o, 0); break;
    case kSemaphoreObject: SetState(o, State(o) - 1); break;
    case kMutantObject:
      SetState(o, State(o) - 1);
      Wr32(o + 0x18, kthread);
      if (Rd8(o + 0x1C)) {
        Wr8(o + 0x1C, 0);
        return true;
      }
      break;
    default: break;
  }
  return false;
}

// The steady clock's time for an NT timeout.
// A wait longer than the host clock can count (XAPI's INFINITE sleep passes
// a relative 0x8000000000000000) waits forever.
Clock::time_point Deadline(int64_t timeout) {
  if (timeout == INT64_MIN) return Clock::time_point::max();
  int64_t ticks = timeout < 0 ? -timeout : timeout - int64_t(SystemTime());
  if (ticks < 0) ticks = 0;
  Clock::time_point now = Clock::now();
  if (uint64_t(ticks) > uint64_t((Clock::time_point::max() - now).count()) / 100)
    return Clock::time_point::max();
  return now + std::chrono::nanoseconds(ticks) * 100;
}

const Clock::time_point g_start = Clock::now();

}  // namespace

void InitObject(uint32_t obj, ObjectType type, int32_t signal_state) {
  uint8_t size = type == kSemaphoreObject ? kSemaphoreSize / 4
                 : type == kMutantObject  ? kMutantSize / 4
                 : type == kThreadObject  ? 0
                                          : kDispatcherHeaderSize / 4;
  Wr8(obj, type);
  Wr8(obj + 1, 0);
  Wr8(obj + 2, size);
  Wr8(obj + 3, 0);
  SetState(obj, signal_state);
  Wr32(obj + 8, obj + 8);
  Wr32(obj + 12, obj + 8);
}

void InitCriticalSection(uint32_t cs, uint32_t spin_count) {
  InitObject(cs, kSynchronizationEvent, 0);
  Wr8(cs + 1, uint8_t(std::min<uint32_t>((spin_count + 255) / 256, 255)));
  Wr32(cs + 0x10, 0xFFFFFFFF);
  Wr32(cs + 0x14, 0);
  Wr32(cs + 0x18, 0);
}

int32_t SetSignal(uint32_t obj, int32_t state) {
  std::lock_guard<std::mutex> g(g_dispatch);
  int32_t old = State(obj);
  SetState(obj, state);
  if (state > 0) g_wake.notify_all();
  return old;
}

int32_t PulseEvent(uint32_t obj) {
  int32_t old;
  {
    std::lock_guard<std::mutex> g(g_dispatch);
    old = State(obj);
    SetState(obj, 1);
    g_wake.notify_all();
  }
  // Waiters check again once the lock is free; give them the chance
  // before the event is cleared.
  std::this_thread::yield();
  std::lock_guard<std::mutex> g(g_dispatch);
  SetState(obj, 0);
  return old;
}

bool ReleaseSemaphore(uint32_t sem, int32_t count, int32_t& previous) {
  std::lock_guard<std::mutex> g(g_dispatch);
  previous = State(sem);
  int32_t limit = int32_t(Rd32(sem + 0x10));
  if (count <= 0 || int64_t(previous) + count > limit) return false;
  SetState(sem, previous + count);
  g_wake.notify_all();
  return true;
}

uint32_t ReleaseMutant(uint32_t mutant, int32_t& previous) {
  uint32_t me = KThread(CurrentThread());
  std::lock_guard<std::mutex> g(g_dispatch);
  previous = State(mutant);
  if (Rd32(mutant + 0x18) != me || previous > 0) return kStatusMutantNotOwned;
  SetState(mutant, previous + 1);
  if (previous + 1 == 1) {
    Wr32(mutant + 0x18, 0);
    g_wake.notify_all();
  }
  return kStatusSuccess;
}

namespace {
// What WaitObjects returns when the wait has an APC to run first.
constexpr uint32_t kApcDue = 0xFFFFFFFF;

uint32_t WaitObjects(const uint32_t* objs, uint32_t count, bool wait_all, Clock::time_point deadline,
                     bool alertable) {
  uint32_t me = KThread(CurrentThread());
  std::unique_lock<std::mutex> l(g_dispatch);
  for (bool last = false;; last = Clock::now() >= deadline) {
    if (wait_all) {
      bool all = true;
      for (uint32_t i = 0; i < count && all; i++) all = Signaled(objs[i], me);
      if (all) {
        bool abandoned = false;
        for (uint32_t i = 0; i < count; i++) abandoned |= Acquire(objs[i], me);
        return abandoned ? kStatusAbandoned : kStatusSuccess;
      }
    } else {
      for (uint32_t i = 0; i < count; i++)
        if (Signaled(objs[i], me)) return (Acquire(objs[i], me) ? kStatusAbandoned : 0) + i;
    }
    if (ApcDue(me, alertable)) return kApcDue;
    if (last) return kStatusTimeout;
    if (deadline == Clock::time_point::max()) g_wake.wait(l);
    else g_wake.wait_until(l, deadline);
  }
}

// Runs the current thread's due APCs, kernel ones first; true if a user
// one ran.
bool RunApcs(bool alertable) {
  uint32_t me = KThread(CurrentThread());
  bool user = false;
  for (;;) {
    QueuedApc a;
    uint32_t kernel_routine, values[4];  // normal routine, context, arguments
    {
      std::lock_guard<std::mutex> g(g_dispatch);
      if (!ApcDue(me, alertable)) return user;
      auto& queues = g_apcs[me].q;
      bool kernel = !queues[kKernelMode].empty() && int32_t(Rd32(me + kKtKernelApcDisable)) >= 0;
      auto& q = queues[kernel ? kKernelMode : kUserMode];
      a = q.front();
      q.pop_front();
      Wr8(a.apc + 3, 0);
      user |= !kernel;
      kernel_routine = Rd32(a.apc + 0x10);
      for (uint32_t k = 0; k < 4; k++) values[k] = Rd32(a.apc + 0x18 + 4 * k);
    }
    // The routines' arguments, which the kernel routine may change (and
    // the APC may be freed by it).
    uint32_t args = SystemAlloc(16);
    for (uint32_t k = 0; k < 4; k++) Wr32(args + 4 * k, values[k]);
    if (kernel_routine) CallGuestNested(kernel_routine, {a.apc, args, args + 4, args + 8, args + 12});
    uint32_t normal = Rd32(args), context = Rd32(args + 4), arg1 = Rd32(args + 8), arg2 = Rd32(args + 12);
    SystemFree(args);
    if (a.owned) SystemFree(a.apc);
    if (normal) CallGuestNested(normal, {context, arg1, arg2});
  }
}

uint32_t WaitWithApcs(const uint32_t* objs, uint32_t count, bool wait_all, Clock::time_point deadline,
                      bool alertable) {
  HoldIfSuspended();
  uint32_t status;
  while ((status = WaitObjects(objs, count, wait_all, deadline, alertable)) == kApcDue)
    if (RunApcs(alertable)) {
      status = kStatusUserApc;
      break;
    }
  HoldIfSuspended();
  return status;
}

}  // namespace

uint32_t Wait(const uint32_t* objs, uint32_t count, bool wait_all, const int64_t* timeout, bool alertable) {
  return WaitWithApcs(objs, count, wait_all, timeout ? Deadline(*timeout) : Clock::time_point::max(), alertable);
}

uint32_t Delay(int64_t timeout, bool alertable) {
  if (timeout == 0) std::this_thread::yield();
  uint32_t status = WaitWithApcs(nullptr, 0, false, Deadline(timeout), alertable);
  return status == kStatusTimeout ? kStatusSuccess : status;
}

// ---- APCs ---------------------------------------------------------------
void InitApc(uint32_t apc, uint32_t kthread, uint32_t kernel_routine, uint32_t rundown_routine,
             uint32_t normal_routine, uint8_t mode, uint32_t normal_context) {
  std::memset(Mem() + apc, 0, kApcSize);
  Wr8(apc, kApcObject);
  Wr8(apc + 2, normal_routine ? mode : kKernelMode);
  Wr32(apc + 4, kthread);
  Wr32(apc + 0x10, kernel_routine);
  Wr32(apc + 0x14, rundown_routine);
  Wr32(apc + 0x18, normal_routine);
  Wr32(apc + 0x1C, normal_routine ? normal_context : 0);
}

bool InsertApc(uint32_t apc, uint32_t arg1, uint32_t arg2, bool owned) {
  std::lock_guard<std::mutex> g(g_dispatch);
  uint32_t kthread = Rd32(apc + 4);
  ApcQueues& queues = g_apcs[kthread];
  if (Rd8(apc + 3) || queues.closed) return false;
  Wr32(apc + 0x20, arg1);
  Wr32(apc + 0x24, arg2);
  Wr8(apc + 3, 1);
  queues.q[Rd8(apc + 2) == kUserMode ? kUserMode : kKernelMode].push_back({apc, owned});
  g_wake.notify_all();
  return true;
}

uint32_t QueueApc(uint32_t kthread, uint32_t normal_routine, uint8_t mode, uint32_t normal_context,
                  uint32_t arg1, uint32_t arg2) {
  uint32_t apc = SystemAlloc(kApcSize);
  InitApc(apc, kthread, 0, 0, normal_routine, mode, normal_context);
  if (InsertApc(apc, arg1, arg2, true)) return apc;
  SystemFree(apc);
  return 0;
}

bool RemoveApc(uint32_t apc) {
  bool owned = false;
  {
    std::lock_guard<std::mutex> g(g_dispatch);
    auto it = g_apcs.find(Rd32(apc + 4));
    if (it == g_apcs.end() || !Rd8(apc + 3)) return false;
    for (auto& q : it->second.q)
      for (auto a = q.begin(); a != q.end(); ++a)
        if (a->apc == apc) {
          owned = a->owned;
          q.erase(a);
          Wr8(apc + 3, 0);
          goto removed;
        }
    return false;
  }
removed:
  if (owned) SystemFree(apc);
  return true;
}

void RunKernelApcs() { RunApcs(false); }

void RundownApcs() {
  std::deque<QueuedApc> left;
  {
    std::lock_guard<std::mutex> g(g_dispatch);
    ApcQueues& queues = g_apcs[KThread(CurrentThread())];
    queues.closed = true;
    for (auto& q : queues.q) {
      left.insert(left.end(), q.begin(), q.end());
      q.clear();
    }
  }
  for (const QueuedApc& a : left) {
    Wr8(a.apc + 3, 0);
    uint32_t rundown = Rd32(a.apc + 0x14);
    if (a.owned) SystemFree(a.apc);
    else if (rundown) CallGuestNested(rundown, {a.apc});
  }
}

void ForgetApcs(uint32_t kthread) {
  std::lock_guard<std::mutex> g(g_dispatch);
  g_apcs.erase(kthread);
}

// ---- timers -------------------------------------------------------------
// One host thread signals the timers that are due.
namespace {
struct Due {
  Clock::time_point at;
  uint32_t period_ms;
};
std::mutex g_timers;
std::condition_variable g_timers_changed;
std::unordered_map<uint32_t, Due> g_due;  // timer -> when
std::unordered_map<uint32_t, uint32_t> g_timer_apcs;  // timer -> its KAPC, if it has a routine
bool g_timer_thread = false;

void Timers() {
  std::unique_lock<std::mutex> l(g_timers);
  for (;;) {
    auto next = g_due.end();
    for (auto it = g_due.begin(); it != g_due.end(); ++it)
      if (next == g_due.end() || it->second.at < next->second.at) next = it;
    if (next == g_due.end()) {
      g_timers_changed.wait(l);
      continue;
    }
    if (Clock::now() < next->second.at) {
      g_timers_changed.wait_until(l, next->second.at);
      continue;
    }
    uint32_t timer = next->first;
    if (next->second.period_ms)
      next->second.at += std::chrono::milliseconds(next->second.period_ms);
    else
      g_due.erase(next);
    SetSignal(timer, 1);
    // Its APC, with the system time it expired at; one still queued from
    // an earlier period keeps its place (that period's APC is lost).
    auto apc = g_timer_apcs.find(timer);
    if (apc != g_timer_apcs.end()) {
      uint64_t now = SystemTime();
      InsertApc(apc->second, uint32_t(now), uint32_t(now >> 32));
    }
  }
}

// Takes a timer's APC off its thread's queue if it is still there.
void Unqueue(uint32_t timer) {
  auto apc = g_timer_apcs.find(timer);
  if (apc != g_timer_apcs.end()) RemoveApc(apc->second);
}
}  // namespace

bool SetTimer(uint32_t timer, int64_t due, uint32_t period_ms, uint32_t apc_routine, uint8_t apc_mode,
              uint32_t apc_context) {
  SetSignal(timer, 0);
  std::lock_guard<std::mutex> g(g_timers);
  if (!g_timer_thread) {
    std::thread(Timers).detach();
    g_timer_thread = true;
  }
  Unqueue(timer);
  if (apc_routine) {
    uint32_t& apc = g_timer_apcs[timer];
    if (!apc) apc = SystemAlloc(kApcSize);
    InitApc(apc, KThread(CurrentThread()), 0, 0, apc_routine, apc_mode, apc_context);
  } else if (auto apc = g_timer_apcs.find(timer); apc != g_timer_apcs.end()) {
    SystemFree(apc->second);
    g_timer_apcs.erase(apc);
  }
  bool was = g_due.count(timer) != 0;
  g_due[timer] = {Deadline(due), period_ms};
  g_timers_changed.notify_all();
  return was;
}

bool CancelTimer(uint32_t timer) {
  std::lock_guard<std::mutex> g(g_timers);
  Unqueue(timer);
  return g_due.erase(timer) != 0;
}

void DeleteTimer(uint32_t timer) {
  CancelTimer(timer);
  std::lock_guard<std::mutex> g(g_timers);
  if (auto apc = g_timer_apcs.find(timer); apc != g_timer_apcs.end()) {
    SystemFree(apc->second);
    g_timer_apcs.erase(apc);
  }
}

// ---- handles ------------------------------------------------------------
namespace {
struct Owned {
  uint32_t refs;
  void (*destroy)(uint32_t);
  std::string name;  // empty: none
};
std::mutex g_handles;
std::unordered_map<uint32_t, uint32_t> g_table;     // handle -> object
std::unordered_map<uint32_t, Owned> g_owned;        // object -> references
std::unordered_map<std::string, uint32_t> g_names;  // name -> object

bool SameName(const std::string& a, const std::string& b, bool case_insensitive) {
  if (!case_insensitive) return a == b;
  return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                    [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}
// As the kernel's: multiples of 4, never 0 or a pseudo handle.
uint32_t g_next_handle = 0xF8000000;
}  // namespace

uint32_t NewHandle(uint32_t obj) {
  Reference(obj);
  std::lock_guard<std::mutex> g(g_handles);
  g_next_handle += 4;
  g_table[g_next_handle] = obj;
  return g_next_handle;
}

uint32_t HandleObject(uint32_t handle) {
  if (handle == kCurrentThreadHandle) return KThread(CurrentThread());
  std::lock_guard<std::mutex> g(g_handles);
  auto it = g_table.find(handle);
  return it == g_table.end() ? 0 : it->second;
}

bool CloseHandle(uint32_t handle) {
  uint32_t obj;
  {
    std::lock_guard<std::mutex> g(g_handles);
    auto it = g_table.find(handle);
    if (it == g_table.end()) return false;
    obj = it->second;
    g_table.erase(it);
  }
  Dereference(obj);
  return true;
}

void Own(uint32_t obj, void (*destroy)(uint32_t obj)) {
  std::lock_guard<std::mutex> g(g_handles);
  g_owned[obj] = {0, destroy};
}

uint32_t NameObject(uint32_t obj, const std::string& name, bool case_insensitive) {
  std::lock_guard<std::mutex> g(g_handles);
  for (const auto& [n, existing] : g_names)
    if (SameName(n, name, case_insensitive)) {
      g_owned.at(existing).refs++;
      return existing;
    }
  g_owned.at(obj).name = name;
  g_names[name] = obj;
  return obj;
}

void Reference(uint32_t obj) {
  std::lock_guard<std::mutex> g(g_handles);
  auto it = g_owned.find(obj);
  if (it != g_owned.end()) it->second.refs++;
}

void Dereference(uint32_t obj) {
  void (*destroy)(uint32_t) = nullptr;
  {
    std::lock_guard<std::mutex> g(g_handles);
    auto it = g_owned.find(obj);
    if (it == g_owned.end()) return;
    if (!it->second.refs) ppc::Fatal(nullptr, "object %x dereferenced more often than referenced", obj);
    if (--it->second.refs) return;
    destroy = it->second.destroy;
    if (!it->second.name.empty()) g_names.erase(it->second.name);
    g_owned.erase(it);
  }
  if (destroy) destroy(obj);
}

// ---- time ---------------------------------------------------------------
uint64_t TimeBaseTicks() {
  uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - g_start).count());
  return ns * (kTimeBaseFrequency / 125000) / 8000;  // ns * 49875000 / 10^9
}

uint64_t SystemTime() {
  // FILETIME: 100 ns units since 1601-01-01, 11644473600 s before 1970.
  auto since_1970 = std::chrono::system_clock::now().time_since_epoch();
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(since_1970).count() / 100) +
         116444736000000000ull;
}

uint64_t InterruptTime() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - g_start).count() / 100);
}

}  // namespace recomp::xbox360
