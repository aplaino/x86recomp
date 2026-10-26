// Dispatcher objects, waits, handles and time (kernel.h).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
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
Clock::time_point Deadline(int64_t timeout) {
  int64_t ticks = timeout < 0 ? -timeout : timeout - int64_t(SystemTime());
  if (ticks < 0) ticks = 0;
  return Clock::now() + std::chrono::nanoseconds(ticks) * 100;
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

uint32_t Wait(const uint32_t* objs, uint32_t count, bool wait_all, const int64_t* timeout,
              bool /*alertable*/) {
  uint32_t me = KThread(CurrentThread());
  Clock::time_point deadline = timeout ? Deadline(*timeout) : Clock::time_point::max();
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
    if (last) return kStatusTimeout;
    if (deadline == Clock::time_point::max()) g_wake.wait(l);
    else g_wake.wait_until(l, deadline);
  }
}

uint32_t Delay(int64_t timeout, bool /*alertable*/) {
  if (timeout == 0) std::this_thread::yield();
  else std::this_thread::sleep_until(Deadline(timeout));
  return kStatusSuccess;
}

// ---- handles ------------------------------------------------------------
namespace {
struct Owned {
  uint32_t refs;
  void (*destroy)(uint32_t);
};
std::mutex g_handles;
std::unordered_map<uint32_t, uint32_t> g_table;  // handle -> object
std::unordered_map<uint32_t, Owned> g_owned;     // object -> references
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
