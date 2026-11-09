// kernel32: waitable objects (events, mutexes, semaphores, threads),
// waits, guest threads, and file mappings.
//
// All waitable state is guarded by one global mutex with one condition
// variable: any signal wakes every waiter, which re-checks its objects.
// Simple and obviously correct for a game with a handful of threads; it
// also makes WaitForMultipleObjects(wait-all) atomic for free.
//
// Memory ordering: g_wait_mu gives signal -> wait acquire/release (what a
// thread wrote before SetEvent/ReleaseMutex/exiting is visible to the
// thread whose wait that satisfies), and every handler here that
// synchronises opens a GuestSyncPoint, which makes the call a full
// barrier as on Windows (see kernel32_state.h).

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/win32.h"
#include "runtime/perf.h"
#include "runtime/platform.h"
#include "runtime/log.h"

namespace recomp {

namespace {

std::mutex g_wait_mu;

// A thread blocked in WaitObjects: woken through its own condition
// variable by the objects it waits on (not every waiting thread by every
// signal: games keep dozens of threads waiting on their own events).
struct Sleeper {
  std::condition_variable cv;
};
thread_local Sleeper t_sleeper;
Sleeper* g_main_sleeper = nullptr;  // the main thread's, once it has waited

struct Waitable : KObject {
  using KObject::KObject;
  // Under g_wait_mu: the threads blocked on it.
  std::vector<Sleeper*> sleepers;
  // Under g_wait_mu: is the object signalled for thread `tid`?
  virtual bool Signalled(uint32_t tid) const = 0;
  // Under g_wait_mu: a wait on it was satisfied; consume (auto-reset,
  // mutex ownership, semaphore count).
  virtual void Acquire(uint32_t tid) { (void)tid; }
  // Under g_wait_mu: thread `tid` starts (on) or stops waiting on it.
  virtual void Waiting(uint32_t tid, bool on) { (void)tid, (void)on; }
};

struct EventObject : Waitable {
  EventObject(bool manual, bool initial)
      : Waitable(Type::Event), manual_reset(manual), signalled(initial) {}
  bool Signalled(uint32_t tid) const override { return signalled || pulsed.count(tid); }
  void Acquire(uint32_t tid) override {
    if (pulsed.erase(tid)) return;
    if (!manual_reset) signalled = false;
  }
  void Waiting(uint32_t tid, bool on) override {
    if (on) {
      waiters.insert(tid);
    } else {
      waiters.erase(tid);
      pulsed.erase(tid);
    }
  }
  // PulseEvent: satisfies the threads waiting now (one of them if
  // auto-reset) and leaves the event reset.
  void Pulse() {
    signalled = false;
    if (manual_reset)
      pulsed = waiters;
    else if (!waiters.empty())
      pulsed.insert(*waiters.begin());
  }
  bool manual_reset;
  bool signalled;
  std::set<uint32_t> waiters, pulsed;  // threads waiting; those a pulse released
  // MarkEventOrdered: when a wait-any finds several such events set, it
  // takes the one set first (see WaitObjects).
  bool ordered = false;
  uint64_t set_seq = 0;  // when it was last set
};

uint64_t g_set_seq = 0;  // under g_wait_mu

struct MutexObject : Waitable {
  MutexObject() : Waitable(Type::Mutex) {}
  bool Signalled(uint32_t tid) const override {
    return owner == 0 || owner == tid;
  }
  void Acquire(uint32_t tid) override {
    owner = tid;
    count++;
  }
  uint32_t owner = 0;
  uint32_t count = 0;
};

struct SemaphoreObject : Waitable {
  SemaphoreObject(int32_t initial, int32_t maximum)
      : Waitable(Type::Semaphore), count(initial), max(maximum) {}
  bool Signalled(uint32_t) const override { return count > 0; }
  void Acquire(uint32_t) override { count--; }
  int32_t count, max;
};

struct ThreadObject : Waitable {
  ThreadObject() : Waitable(Type::Thread) {}
  bool Signalled(uint32_t) const override { return finished; }
  Thread* thread = nullptr;
  gaddr entry = 0;
  uint64_t param = 0;
  std::atomic<bool> started{false};  // CreateThread vs ResumeThread races
  bool finished = false;  // under g_wait_mu
  uint32_t exit_code = 0;  // once finished
};

std::map<std::string, std::shared_ptr<KObject>> g_named;  // under g_wait_mu
std::map<uint32_t, std::shared_ptr<ThreadObject>> g_thread_objs;  // by tid

constexpr uint32_t kWaitObject0 = 0, kWaitTimeout = 0x102,
                   kWaitFailed = 0xFFFFFFFF, kInfinite = 0xFFFFFFFF;

std::shared_ptr<Waitable> AsWaitable(uint32_t h) {
  if (h == 0xFFFFFFFE) {  // GetCurrentThread() pseudo handle
    std::lock_guard<std::mutex> l(g_wait_mu);
    auto it = g_thread_objs.find(CurrentThread()->tid);
    return it == g_thread_objs.end() ? nullptr : it->second;
  }
  return std::dynamic_pointer_cast<Waitable>(GetHandle(h));
}

// Waits for one (wait_all=false) or all of `objs`. Returns WAIT_OBJECT_0+i
// or WAIT_TIMEOUT.
uint32_t WaitObjects(const std::vector<std::shared_ptr<Waitable>>& objs,
                     bool wait_all, uint32_t timeout_ms) {
  uint32_t tid = CurrentThread()->tid;
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  std::unique_lock<std::mutex> l(g_wait_mu);
  // Registered as a waiter for the whole wait (PulseEvent), under the lock.
  Sleeper* me = &t_sleeper;
  struct Registration {
    const std::vector<std::shared_ptr<Waitable>>& objs;
    uint32_t tid;
    Sleeper* me;
    ~Registration() {
      for (const auto& o : objs) {
        o->Waiting(tid, false);
        std::erase(o->sleepers, me);
      }
    }
  } reg{objs, tid, me};
  for (const auto& o : objs) {
    o->Waiting(tid, true);
    o->sleepers.push_back(me);
  }
  if (OnMainThread()) g_main_sleeper = me;
  for (;;) {
    if (wait_all) {
      bool all = true;
      for (const auto& o : objs) all = all && o->Signalled(tid);
      if (all) {
        for (const auto& o : objs) o->Acquire(tid);
        return kWaitObject0;
      }
    } else {
      for (size_t i = 0; i < objs.size(); i++)
        if (objs[i]->Signalled(tid)) {
          // Windows takes the lowest index. Ordered events (DirectSound
          // position notifications) set together are taken in the order
          // they were set instead: on Windows they come one at a time, as
          // the play cursor passes each, and a game refills the part of
          // its ring each one names in the order it gets them; lowest
          // index first, a ring's wrap (the last and the first set at
          // once) had it write its parts out of order (heard as clicks).
          if (const auto* e = dynamic_cast<const EventObject*>(objs[i].get()); e && e->ordered) {
            for (size_t j = i + 1; j < objs.size(); j++) {
              const auto* f = dynamic_cast<const EventObject*>(objs[j].get());
              if (f && f->ordered && f->Signalled(tid) &&
                  f->set_seq < static_cast<const EventObject*>(objs[i].get())->set_seq)
                i = j;
            }
          }
          objs[i]->Acquire(tid);
          return kWaitObject0 + uint32_t(i);
        }
    }
    if (timeout_ms == 0) return kWaitTimeout;
    if (OnMainThread() && PumpInWaits()) {
      // Another thread drives a window: run its window calls and post its
      // events from here, in slices, until the wait is satisfied.
      auto slice = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
      if (timeout_ms != kInfinite && deadline < slice) slice = deadline;
      me->cv.wait_until(l, slice);
      l.unlock();
      PumpHostEvents();
      l.lock();
      if (timeout_ms != kInfinite && std::chrono::steady_clock::now() >= deadline)
        return kWaitTimeout;
      continue;
    }
    if (timeout_ms == kInfinite) {
      me->cv.wait(l);
    } else if (me->cv.wait_until(l, deadline) == std::cv_status::timeout) {
      return kWaitTimeout;  // deadline passed: no re-check
    }
  }
}

// Under g_wait_mu: `o` may have become signalled; wakes the threads
// blocked on it.
void SignalChanged(const Waitable& o) {
  for (Sleeper* s : o.sleepers) s->cv.notify_one();
}

}  // namespace

void WakeMainThreadWait() {
  std::lock_guard<std::mutex> l(g_wait_mu);
  if (g_main_sleeper) g_main_sleeper->cv.notify_one();
}

bool SignalEventHandle(uint32_t h, bool pulse) {
  auto e = std::dynamic_pointer_cast<EventObject>(GetHandle(h));
  if (!e) return false;
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    if (pulse) {
      e->Pulse();
    } else {
      if (!e->signalled) e->set_seq = ++g_set_seq;
      e->signalled = true;
    }
    SignalChanged(*e);
  }
  return true;
}

bool MarkEventOrdered(uint32_t h) {
  auto e = std::dynamic_pointer_cast<EventObject>(GetHandle(h));
  if (!e) return false;
  std::lock_guard<std::mutex> l(g_wait_mu);
  e->ordered = true;
  return true;
}

uint32_t NewEventHandle(bool manual_reset, bool initial) {
  return NewHandle(std::make_shared<EventObject>(manual_reset, initial));
}

bool ResetEventHandle(uint32_t h) {
  auto e = std::dynamic_pointer_cast<EventObject>(GetHandle(h));
  if (!e) return false;
  std::lock_guard<std::mutex> l(g_wait_mu);
  e->signalled = false;
  return true;
}

namespace {

std::string NameOf(gaddr va, bool wide = false) {
  if (!va) return std::string();
  return wide ? win32::Narrow(GWStr(va)) : GStr(va);
}

// Registers a named object or returns the existing one (ERROR_ALREADY_EXISTS).
template <typename Make>
uint32_t CreateNamed(const std::string& name, Make make) {
  std::shared_ptr<KObject> obj;
  bool existed = false;
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    if (!name.empty()) {
      auto it = g_named.find(name);
      if (it != g_named.end()) {
        obj = it->second;
        existed = true;
      }
    }
    if (!obj) {
      obj = make();
      if (!name.empty()) g_named[name] = obj;
    }
  }
  hle::SetLastError(existed ? win32::kErrorAlreadyExists : 0);
  return NewHandle(obj);
}

}  // namespace

namespace hle {
namespace {

using namespace win32;

// ---- events -----------------------------------------------------------------

uint64_t CreateEvent(const Ctx& c, bool wide) {
  bool manual = uint32_t(Arg(c, 1)), initial = uint32_t(Arg(c, 2));
  return HandleRet(CreateNamed(NameOf(Arg(c, 3), wide), [&] {
    return std::make_shared<EventObject>(manual, initial);
  }));
}
HLE64("kernel32.dll", CreateEventA, 4) { return CreateEvent(c, false); }
HLE64("kernel32.dll", CreateEventW, 4) { return CreateEvent(c, true); }

HLE64("kernel32.dll", SetEvent, 1) {
  GuestSyncPoint sync;
  return SignalEventHandle(uint32_t(Arg(c, 0)), false) ? 1 : 0;
}

HLE64("kernel32.dll", PulseEvent, 1) {
  GuestSyncPoint sync;
  return SignalEventHandle(uint32_t(Arg(c, 0)), true) ? 1 : 0;
}

HLE64("kernel32.dll", ResetEvent, 1) {
  GuestSyncPoint sync;
  auto e = std::dynamic_pointer_cast<EventObject>(GetHandle(Arg(c, 0)));
  if (!e) return 0;
  std::lock_guard<std::mutex> l(g_wait_mu);
  e->signalled = false;
  return 1;
}

// ---- mutexes ----------------------------------------------------------------

uint32_t CreateMutex(const Ctx& c, bool wide) {
  GuestSyncPoint sync;
  bool own = Arg(c, 1);
  uint32_t tid = CurrentThread()->tid;
  uint32_t h = CreateNamed(NameOf(Arg(c, 2), wide), [] {
    return std::make_shared<MutexObject>();
  });
  if (own) {
    auto m = std::dynamic_pointer_cast<MutexObject>(GetHandle(h));
    std::lock_guard<std::mutex> l(g_wait_mu);
    if (m->owner == 0 || m->owner == tid) m->Acquire(tid);
  }
  return h;
}

HLE("kernel32.dll", CreateMutexA, 3) { return CreateMutex(c, false); }
HLE("kernel32.dll", CreateMutexW, 3) { return CreateMutex(c, true); }

// OpenMutexA/OpenFileMappingA(access, inherit, name): a new handle to an
// existing named object of that type.
uint32_t OpenNamed(gaddr name_va, KObject::Type type, bool wide = false) {
  std::string name = NameOf(name_va, wide);
  std::lock_guard<std::mutex> l(g_wait_mu);
  auto it = g_named.find(name);
  if (it == g_named.end() || it->second->type != type) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  return NewHandle(it->second);
}

HLE("kernel32.dll", OpenMutexA, 3) { return OpenNamed(Arg(c, 2), KObject::Type::Mutex); }
HLE("kernel32.dll", OpenEventA, 3) { return OpenNamed(Arg(c, 2), KObject::Type::Event); }

HLE("kernel32.dll", ReleaseMutex, 1) {
  GuestSyncPoint sync;
  auto m = std::dynamic_pointer_cast<MutexObject>(GetHandle(Arg(c, 0)));
  if (!m) return 0;
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    if (m->owner != CurrentThread()->tid) return 0;
    if (--m->count == 0) m->owner = 0;
    if (!m->count) SignalChanged(*m);
  }
  return 1;
}

// ---- semaphores -------------------------------------------------------------

uint64_t CreateSemaphore(const Ctx& c, bool wide) {
  int32_t initial = int32_t(Arg(c, 1)), maximum = int32_t(Arg(c, 2));
  if (maximum <= 0 || initial < 0 || initial > maximum) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  return HandleRet(CreateNamed(NameOf(Arg(c, 3), wide), [&] {
    return std::make_shared<SemaphoreObject>(initial, maximum);
  }));
}
HLE64("kernel32.dll", CreateSemaphoreA, 4) { return CreateSemaphore(c, false); }
HLE64("kernel32.dll", CreateSemaphoreW, 4) { return CreateSemaphore(c, true); }

HLE64("kernel32.dll", ReleaseSemaphore, 3) {
  GuestSyncPoint sync;
  auto s = std::dynamic_pointer_cast<SemaphoreObject>(GetHandle(uint32_t(Arg(c, 0))));
  int32_t n = int32_t(Arg(c, 1));
  if (!s) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    if (n <= 0 || n > s->max - s->count) {
      SetLastError(kErrorTooManyPosts);
      return 0;
    }
    if (gaddr prev = Arg(c, 2)) Wr32(prev, uint32_t(s->count));
    s->count += n;
    SignalChanged(*s);
  }
  return 1;
}

// ---- waits ------------------------------------------------------------------
// Alertable waits (the Ex forms) are never alerted: no APCs are queued.

uint64_t WaitOne(const Ctx& c) {
  GuestSyncPoint sync;
  WaitLog log(c, "WaitForSingleObject", Arg(c, 0));
  if (perf::On() && Arg(c, 1) == 0) perf::CountPoll(perf::Poll::kZeroWait, Caller(c));
  auto w = AsWaitable(uint32_t(Arg(c, 0)));
  if (!w) {
    SetLastError(kErrorInvalidHandle);
    return kWaitFailed;
  }
  return WaitObjects({w}, false, uint32_t(Arg(c, 1)));
}
HLE64("kernel32.dll", WaitForSingleObject, 2) { return WaitOne(c); }
HLE64("kernel32.dll", WaitForSingleObjectEx, 3) { return WaitOne(c); }

HLE64("kernel32.dll", WaitForMultipleObjects, 4) {
  GuestSyncPoint sync;
  WaitLog log(c, "WaitForMultipleObjects", Arg(c, 1));
  if (perf::On() && Arg(c, 3) == 0) perf::CountPoll(perf::Poll::kZeroWait, Caller(c));
  uint32_t n = uint32_t(Arg(c, 0));
  gaddr arr = Arg(c, 1);
  const uint32_t ptr = g_proc->x64 ? 8 : 4;  // HANDLE[]
  std::vector<std::shared_ptr<Waitable>> objs;
  for (uint32_t i = 0; i < n; i++) {
    auto w = AsWaitable(uint32_t(RdPtr(arr + ptr * i)));
    if (!w) {
      SetLastError(kErrorInvalidHandle);
      return kWaitFailed;
    }
    objs.push_back(w);
  }
  return WaitObjects(objs, Arg(c, 2) != 0, Arg(c, 3));
}

// ---- slim reader/writer locks and condition variables ------------------------
// Host objects keyed by the guest SRWLOCK / CONDITION_VARIABLE address (the
// guest's pointer-sized word stays as the guest initialised it, zero).
// Every call is a full barrier like the other waits (GuestSyncPoint).
// Acquisition is not fair, as on Windows; condition variables may wake
// spuriously, as there.

struct SrwLock {
  std::mutex m;
  std::condition_variable cv;
  int readers = 0;
  bool writer = false;
};
struct CondVar {
  std::mutex m;
  std::condition_variable cv;
  uint32_t waiting = 0, released = 0;  // sleepers; wakes not yet taken
};

template <typename T>
T& ObjectAt(gaddr va) {
  static std::mutex mu;
  static std::map<gaddr, std::unique_ptr<T>> objs;
  std::lock_guard<std::mutex> l(mu);
  auto& o = objs[va];
  if (!o) o = std::make_unique<T>();
  return *o;
}

HLE64("kernel32.dll", InitializeSRWLock, 1) {
  WrPtr(Arg(c, 0), 0);  // SRWLOCK_INIT
  return 0;
}
HLE64("kernel32.dll", AcquireSRWLockExclusive, 1) {
  GuestSyncPoint sync;
  SrwLock& s = ObjectAt<SrwLock>(Arg(c, 0));
  std::unique_lock<std::mutex> l(s.m);
  s.cv.wait(l, [&] { return !s.writer && !s.readers; });
  s.writer = true;
  return 0;
}
HLE64("kernel32.dll", TryAcquireSRWLockExclusive, 1) {
  GuestSyncPoint sync;
  SrwLock& s = ObjectAt<SrwLock>(Arg(c, 0));
  std::lock_guard<std::mutex> l(s.m);
  if (s.writer || s.readers) return 0;
  s.writer = true;
  return 1;
}
HLE64("kernel32.dll", AcquireSRWLockShared, 1) {
  GuestSyncPoint sync;
  SrwLock& s = ObjectAt<SrwLock>(Arg(c, 0));
  std::unique_lock<std::mutex> l(s.m);
  s.cv.wait(l, [&] { return !s.writer; });
  s.readers++;
  return 0;
}
HLE64("kernel32.dll", ReleaseSRWLockExclusive, 1) {
  GuestSyncPoint sync;
  SrwLock& s = ObjectAt<SrwLock>(Arg(c, 0));
  {
    std::lock_guard<std::mutex> l(s.m);
    s.writer = false;
  }
  s.cv.notify_all();
  return 0;
}
HLE64("kernel32.dll", ReleaseSRWLockShared, 1) {
  GuestSyncPoint sync;
  SrwLock& s = ObjectAt<SrwLock>(Arg(c, 0));
  bool last;
  {
    std::lock_guard<std::mutex> l(s.m);
    last = --s.readers == 0;
  }
  if (last) s.cv.notify_all();
  return 0;
}

HLE64("kernel32.dll", InitializeConditionVariable, 1) {
  WrPtr(Arg(c, 0), 0);  // CONDITION_VARIABLE_INIT
  return 0;
}

// Releases the critical section, sleeps until a Wake (or the timeout) and
// takes it again. The CondVar's mutex is held across the Leave, so a Wake
// right after it still finds this thread counted as waiting.
HLE64("kernel32.dll", SleepConditionVariableCS, 3) {
  GuestSyncPoint sync;
  CondVar& v = ObjectAt<CondVar>(Arg(c, 0));
  gaddr cs = Arg(c, 1);
  uint32_t ms = uint32_t(Arg(c, 2));
  bool woken;
  {
    std::unique_lock<std::mutex> l(v.m);
    v.waiting++;
    LeaveCs(cs);
    auto pred = [&] { return v.released > 0; };
    if (ms == kInfinite) {
      v.cv.wait(l, pred);
      woken = true;
    } else {
      woken = v.cv.wait_for(l, std::chrono::milliseconds(ms), pred);
    }
    v.waiting--;
    if (woken) v.released--;
  }
  EnterCs(cs);
  if (!woken) {
    SetLastError(kErrorTimeout);
    return 0;
  }
  return 1;
}

HLE64("kernel32.dll", WakeConditionVariable, 1) {
  GuestSyncPoint sync;
  CondVar& v = ObjectAt<CondVar>(Arg(c, 0));
  {
    std::lock_guard<std::mutex> l(v.m);
    if (v.released < v.waiting) v.released++;
  }
  v.cv.notify_one();
  return 0;
}

HLE64("kernel32.dll", WakeAllConditionVariable, 1) {
  GuestSyncPoint sync;
  CondVar& v = ObjectAt<CondVar>(Arg(c, 0));
  {
    std::lock_guard<std::mutex> l(v.m);
    v.released = v.waiting;
  }
  v.cv.notify_all();
  return 0;
}

// ---- threads ----------------------------------------------------------------

// Starts the thread once. pthread_create synchronises memory: what the
// creator wrote before it is visible to the new thread.
bool RunThreadObject(const std::shared_ptr<ThreadObject>& to) {
  if (to->started.exchange(true)) return false;
  StartHostThread(to->thread, to->entry, to->param);
  return true;
}

}  // namespace

uint32_t NewThread(uint64_t stack, gaddr entry, uint64_t param, bool suspended, uint32_t* tid) {
  GuestSyncPoint sync;
  auto to = std::make_shared<ThreadObject>();
  to->thread = CreateGuestThread(stack ? stack : g_proc->img.stack_reserve);
  to->entry = entry;
  to->param = param;
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    g_thread_objs[to->thread->tid] = to;
  }
  *tid = to->thread->tid;
  uint32_t h = NewHandle(to);
  RECOMP_TRACE("  new thread entry=%08llX param=%08llX -> tid %04X%s\n",
               (unsigned long long)entry, (unsigned long long)param, to->thread->tid,
               suspended ? " (suspended)" : "");
  if (!suspended) RunThreadObject(to);
  return h;
}

bool JoinThread(uint32_t h, uint32_t* exit_code) {
  auto to = std::dynamic_pointer_cast<ThreadObject>(GetHandle(h));
  if (!to) return false;
  WaitObjects({to}, false, kInfinite);
  std::lock_guard<std::mutex> l(g_wait_mu);
  *exit_code = to->exit_code;
  return true;
}

namespace {

HLE("kernel32.dll", CreateThread, 6) {
  uint32_t tid;
  uint32_t h = NewThread(Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4) & 4, &tid);
  if (Arg(c, 5)) Wr32(Arg(c, 5), tid);
  return h;
}

HLE("kernel32.dll", ResumeThread, 1) {
  GuestSyncPoint sync;
  auto to = std::dynamic_pointer_cast<ThreadObject>(GetHandle(Arg(c, 0)));
  if (!to) return 0xFFFFFFFF;
  return RunThreadObject(to) ? 1 : 0;
}

HLE("kernel32.dll", SuspendThread, 1) {
  WarnF("SuspendThread on a running thread is not supported; ignored\n");
  return 0;
}

HLE("kernel32.dll", TerminateThread, 2) {
  WarnF("TerminateThread is not supported; ignored\n");
  return 0;
}

HLE("kernel32.dll", ExitThread, 1) {
  GuestSyncFence();  // OnGuestThreadFinished fences again after unwinding
  throw GuestThreadExit{uint32_t(Arg(c, 0))};
}

HLE("kernel32.dll", GetThreadPriority, 1) {
  const uint32_t h = Arg(c, 0);
  if (h == 0xFFFFFFFE) return uint32_t(CurrentThread()->priority);
  auto to = std::dynamic_pointer_cast<ThreadObject>(GetHandle(h));
  return to ? uint32_t(to->thread->priority) : 0x7FFFFFFF;  // THREAD_PRIORITY_ERROR_RETURN
}
// SetThreadPriority(thread, priority): applied as the host scheduling class
// (platform::SetThreadPriorityClass) on the thread itself, at once for the
// caller, at its start for one not started yet (games set it between
// CreateThread(CREATE_SUSPENDED) and ResumeThread). A running thread other
// than the caller keeps its class, and so does one whose class the game
// chose (GameConfig::thread_qos).
void ApplyOwnPriority(Thread* t, int p) {
  if (!t->qos_fixed) platform::SetThreadPriorityClass(p);
  if (perf::On()) perf::NoteThread(t->tid, p, "SetThreadPriority");
}
HLE("kernel32.dll", SetThreadPriority, 2) {
  const uint32_t h = Arg(c, 0);
  const int p = int32_t(Arg(c, 1));
  if (h == 0xFFFFFFFE) {  // GetCurrentThread()
    CurrentThread()->priority = p;
    ApplyOwnPriority(CurrentThread(), p);
    return 1;
  }
  auto to = std::dynamic_pointer_cast<ThreadObject>(GetHandle(h));
  if (!to) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  to->thread->priority = p;
  if (to->thread == CurrentThread()) ApplyOwnPriority(CurrentThread(), p);
  return 1;
}
HLE("kernel32.dll", SetThreadPriorityBoost, 2) { return 1; }

HLE("kernel32.dll", SwitchToThread, 0) {
  if (perf::On()) perf::CountPoll(perf::Poll::kSwitchToThread, Caller(c));
  std::this_thread::yield();
  return 1;
}

// STILL_ACTIVE (259) until the thread has finished.
HLE("kernel32.dll", GetExitCodeThread, 2) {
  GuestSyncPoint sync;
  auto to = std::dynamic_pointer_cast<ThreadObject>(GetHandle(Arg(c, 0)));
  if (!to) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  std::lock_guard<std::mutex> l(g_wait_mu);
  Wr32(Arg(c, 1), to->finished ? to->exit_code : 259);
  return 1;
}

// OpenThread(access, inherit, tid): a new handle to a thread of this process.
HLE("kernel32.dll", OpenThread, 3) {
  std::lock_guard<std::mutex> l(g_wait_mu);
  auto it = g_thread_objs.find(uint32_t(Arg(c, 2)));
  if (it == g_thread_objs.end()) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  return NewHandle(it->second);
}
HLE("kernel32.dll", GetPriorityClass, 1) { return 0x20; }  // NORMAL
HLE("kernel32.dll", SetPriorityClass, 2) { return 1; }

// Affinity is bookkeeping only: guest threads are never pinned on the host
// (games pin a thread to one CPU around QueryPerformanceCounter reads, for
// processors whose counters differ, which host clocks do not). The masks
// are over the processors GetSystemInfo reports.
uint32_t SystemAffinityMask() { return (1u << GuestProcessorCount()) - 1; }
std::atomic<uint32_t> g_process_affinity{0};  // 0: all processors

uint32_t ProcessAffinityMask() {
  uint32_t m = g_process_affinity.load();
  return m ? m : SystemAffinityMask();
}

HLE("kernel32.dll", GetProcessAffinityMask, 3) {
  Wr32(Arg(c, 1), ProcessAffinityMask());
  Wr32(Arg(c, 2), SystemAffinityMask());
  return 1;
}

HLE("kernel32.dll", SetProcessAffinityMask, 2) {
  uint32_t mask = Arg(c, 1);
  if (!mask || (mask & ~SystemAffinityMask())) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  g_process_affinity = mask;
  return 1;
}

// Returns the previous mask; threads are not tracked, so that is the
// process mask, as for a thread that was never restricted.
HLE("kernel32.dll", SetThreadAffinityMask, 2) {
  uint32_t mask = Arg(c, 1);
  if (!mask || (mask & ~ProcessAffinityMask())) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  return ProcessAffinityMask();
}

HLE("kernel32.dll", GetThreadContext, 2) {
  // Only used by the crash reporter to sample other threads.
  SetLastError(kErrorAccessDenied);
  return 0;
}

HLE("kernel32.dll", OpenProcess, 3) {
  // Only the guest and its parent (the processes snapshots report) open.
  uint32_t pid = uint32_t(Arg(c, 2));
  if (pid != kGuestPid && pid != kGuestParentPid) {
    SetLastError(kErrorAccessDenied);
    return 0;
  }
  return NewHandle(std::make_shared<ProcessObject>(pid));
}

// Process snapshots list only the guest itself (see Process32FirstW).
HLE("kernel32.dll", CreateToolhelp32Snapshot, 2) {
  if (!(Arg(c, 0) & 2 /*TH32CS_SNAPPROCESS*/)) {
    SetLastError(kErrorAccessDenied);
    return 0xFFFFFFFF;
  }
  return NewHandle(std::make_shared<KObject>(KObject::Type::Snapshot));
}
HLE("kernel32.dll", Thread32First, 2) { return 0; }
HLE("kernel32.dll", Thread32Next, 2) { return 0; }
HLE("kernel32.dll", Process32First, 2) { return 0; }
HLE("kernel32.dll", Process32Next, 2) { return 0; }

// ---- file mappings ------------------------------------------------------------
// Views are mapped straight into the guest arena with mmap(MAP_FIXED), so a
// view of a large archive costs address space, not memory.

struct MappingObject : KObject {
  MappingObject() : KObject(Type::Mapping) {}
  int fd = -1;          // -1: pagefile-backed (anonymous, shared)
  uint64_t size = 0;
  bool writable = false;
  uint32_t anon_base = 0;  // for pagefile-backed: one shared guest block
  ~MappingObject() override {
    if (fd >= 0) close(fd);
  }
};

std::mutex g_views_mu;
std::set<uint32_t> g_views;  // guest bases of file views

HLE("kernel32.dll", CreateFileMappingA, 6) {
  uint32_t file = Arg(c, 0), prot = Arg(c, 2), size_hi = Arg(c, 3),
           size_lo = Arg(c, 4), name = Arg(c, 5);
  auto m = std::make_shared<MappingObject>();
  m->writable = prot == kPageReadWrite || prot == kPageWriteCopy ||
                prot == kPageExecuteReadWrite;
  m->size = (uint64_t(size_hi) << 32) | size_lo;
  if (file != 0xFFFFFFFF) {
    auto o = GetHandle(file);
    if (!o || o->type != KObject::Type::File) {
      SetLastError(kErrorInvalidHandle);
      return 0;
    }
    // Duplicate the fd so the mapping outlives CloseHandle(file).
    m->fd = dup(FileFd(o.get()));
    struct stat st;
    fstat(m->fd, &st);
    if (!m->size) m->size = uint64_t(st.st_size);
  } else {
    // Pagefile-backed shared memory: one guest block shared by all views.
    uint32_t base = g_proc->vm.Reserve(0, uint32_t(m->size));
    if (!base) {
      SetLastError(kErrorNotEnoughMemory);
      return 0;
    }
    g_proc->vm.Commit(base, uint32_t(m->size), Prot::RW);
    m->anon_base = base;
  }
  return CreateNamed(NameOf(name), [&] { return m; });
}

HLE("kernel32.dll", OpenFileMappingW, 3) {
  return OpenNamed(Arg(c, 2), KObject::Type::Mapping, true);
}

HLE("kernel32.dll", OpenFileMappingA, 3) {
  return OpenNamed(Arg(c, 2), KObject::Type::Mapping);
}

uint32_t MapView(uint32_t h, uint32_t access, uint32_t off_hi, uint32_t off_lo,
                 uint32_t size, uint32_t want_base) {
  auto m = std::dynamic_pointer_cast<MappingObject>(GetHandle(h));
  if (!m) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  uint64_t off = (uint64_t(off_hi) << 32) | off_lo;
  if (m->anon_base) {
    if (want_base) return 0;
    return m->anon_base + uint32_t(off);
  }
  if (!size) size = uint32_t(m->size - off);
  uint32_t base = g_proc->vm.Reserve(want_base, size);
  if (!base) {
    SetLastError(kErrorNotEnoughMemory);
    return 0;
  }
  bool write = (access & 0x2) && m->writable;  // FILE_MAP_WRITE
  // Copy-on-write (FILE_MAP_COPY) and read views are private; write views
  // are shared with the file.
  int flags = MAP_FIXED | (write ? MAP_SHARED : MAP_PRIVATE);
  void* p = mmap(GPtr(base), size, PROT_READ | PROT_WRITE, flags, m->fd, off_t(off));
  if (p == MAP_FAILED) {
    g_proc->vm.Release(base);
    SetLastError(kErrorAccessDenied);
    return 0;
  }
  // Tell the page table these guest pages are committed (the host pages
  // are already accessible through the file mapping).
  g_proc->mem.Commit(base, size, write ? Prot::RW : Prot::R);
  std::lock_guard<std::mutex> l(g_views_mu);
  g_views.insert(base);
  return base;
}

HLE("kernel32.dll", MapViewOfFile, 5) {
  return MapView(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), 0);
}

HLE("kernel32.dll", MapViewOfFileEx, 6) {
  return MapView(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4),
                 Arg(c, 5));
}

HLE("kernel32.dll", UnmapViewOfFile, 1) {
  uint32_t base = Arg(c, 0);
  {
    std::lock_guard<std::mutex> l(g_views_mu);
    if (!g_views.erase(base)) return 1;  // pagefile views: nothing to do
  }
  // Release replaces the file mapping with an inaccessible anonymous one.
  // Don't zero first: the pages are discarded and may be read-only.
  g_proc->vm.Release(base, false);
  return 1;
}

}  // namespace
}  // namespace hle

void OnGuestThreadFinished(uint32_t tid, uint32_t exit_code) {
  GuestSyncPoint sync;
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    auto it = g_thread_objs.find(tid);
    if (it != g_thread_objs.end()) {
      it->second->exit_code = exit_code;
      it->second->finished = true;
      SignalChanged(*it->second);
    }
  }
}

}  // namespace recomp
