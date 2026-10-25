// libkernel HLE: threads, mutexes, condition variables, read-write locks,
// thread-specific data and once (scePthread*, and libScePosix's pthread_*
// over the same objects). FreeBSD's semantics: mutexes are error-checking
// by default; the scePthread functions return SCE error codes, the POSIX
// ones errno values.
//
// Every object is an opaque pointer in guest memory (ScePthreadMutex and
// friends are pointers): the HLE hands out small guest blocks as handles
// and keeps the host state for each by its address. A null handle is the
// static initialiser and gets its object on first use (FreeBSD's 1 and 2
// for mutexes: recursive and adaptive).
//
// Guest threads are host threads (StartServiceThread); every call that
// synchronises is a full barrier for guest memory (GuestSyncPoint).

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::orbis::kernel {
namespace {

using Clock = std::chrono::steady_clock;

enum MutexType : int { kErrorCheck = 1, kRecursive = 2, kNormal = 3, kAdaptive = 4 };

struct MutexAttr {
  int type = kErrorCheck;
};

struct Mutex {
  std::mutex mu;
  std::condition_variable cv;
  uint32_t owner = 0;  // guest tid, 0: unlocked
  uint32_t count = 0;
  int type = kErrorCheck;
  std::string name;
};

struct Cond {
  std::mutex mu;
  std::condition_variable cv;
  uint64_t seq = 0;  // bumped by every signal and broadcast
};

struct RwLock {
  std::mutex mu;
  std::condition_variable cv;
  uint32_t writer = 0;  // guest tid
  uint32_t readers = 0;
};

// FreeBSD's default thread stack (not measured on hardware) and libkernel's
// default priority.
constexpr uint64_t kDefaultStack = 2u << 20;
constexpr int kDefaultPrio = 700;

struct ThreadAttr {
  uint64_t stack = kDefaultStack;
  bool detached = false;
  int prio = kDefaultPrio, policy = 1, inherit = 4;
  uint64_t affinity = 0x7F;
};

struct PThread {
  gaddr handle = 0;
  Thread* t = nullptr;
  std::string name;
  std::mutex mu;
  std::condition_variable cv;
  bool done = false, detached = false;
  uint64_t ret = 0;
  int prio = kDefaultPrio;
  uint64_t affinity = 0x7F;
  std::map<uint32_t, uint64_t> specific;  // key -> value
};

// The objects behind the handles (kernel.h, NewHandle).
std::mutex g_mu;
std::map<gaddr, std::unique_ptr<MutexAttr>> g_mutex_attrs;
std::map<gaddr, std::unique_ptr<Mutex>> g_mutexes;
std::map<gaddr, std::unique_ptr<Cond>> g_conds;
std::map<gaddr, std::unique_ptr<RwLock>> g_rwlocks;
std::map<gaddr, std::unique_ptr<ThreadAttr>> g_thread_attrs;
std::map<gaddr, std::shared_ptr<PThread>> g_threads;  // by handle
std::map<Thread*, std::shared_ptr<PThread>> g_by_thread;
std::map<uint32_t, gaddr> g_key_destructors;  // key -> destructor (0: none)
uint32_t g_next_key = 1;

template <typename T>
T* Find(std::map<gaddr, std::unique_ptr<T>>& m, gaddr h) {
  auto it = m.find(h);
  return it == m.end() ? nullptr : it->second.get();
}

uint32_t Tid() { return CurrentThread()->tid; }

// The handle the guest variable at `slot` holds, made for a static
// initialiser (null, or for mutexes 1 and 2).
Mutex* MutexAt(gaddr slot) {
  std::lock_guard<std::mutex> l(g_mu);
  gaddr h = Rd64(slot);
  if (h > 2) return Find(g_mutexes, h);
  auto m = std::make_unique<Mutex>();
  m->type = h == 1 ? kRecursive : h == 2 ? kAdaptive : kErrorCheck;
  gaddr nh = NewHandle();
  Mutex* raw = m.get();
  g_mutexes[nh] = std::move(m);
  Wr64(slot, nh);
  return raw;
}

template <typename T>
T* LazyAt(std::map<gaddr, std::unique_ptr<T>>& objs, gaddr slot) {
  std::lock_guard<std::mutex> l(g_mu);
  gaddr h = Rd64(slot);
  if (h) return Find(objs, h);
  gaddr nh = NewHandle();
  T* raw = (objs[nh] = std::make_unique<T>()).get();
  Wr64(slot, nh);
  return raw;
}

// ---- mutexes ----------------------------------------------------------

int MutexInit(gaddr slot, gaddr attr_slot, const char* name) {
  std::lock_guard<std::mutex> l(g_mu);
  auto m = std::make_unique<Mutex>();
  if (attr_slot) {
    MutexAttr* a = Find(g_mutex_attrs, Rd64(attr_slot));
    if (!a) return kEINVAL;
    m->type = a->type;
  }
  if (name) m->name = name;
  gaddr h = NewHandle();
  g_mutexes[h] = std::move(m);
  Wr64(slot, h);
  return 0;
}

// timeout_us < 0: wait for ever; 0 with try: do not wait.
int MutexLock(gaddr slot, bool try_only, int64_t timeout_us) {
  GuestSyncPoint sync;
  Mutex* m = MutexAt(slot);
  if (!m) return kEINVAL;
  const uint32_t me = Tid();
  std::unique_lock<std::mutex> l(m->mu);
  if (m->owner == me) {
    if (m->type == kRecursive) {
      m->count++;
      return 0;
    }
    if (m->type == kErrorCheck) return try_only ? kEBUSY : kEDEADLK;
    if (try_only) return kEBUSY;
    FatalF("thread %04X locks mutex %08llX (%s) it holds: a self-deadlock", me,
           (unsigned long long)Rd64(slot), m->name.c_str());
  }
  if (m->owner) {
    if (try_only) return kEBUSY;
    if (timeout_us < 0) {
      m->cv.wait(l, [&] { return !m->owner; });
    } else if (!m->cv.wait_for(l, std::chrono::microseconds(timeout_us),
                               [&] { return !m->owner; })) {
      return kETIMEDOUT;
    }
  }
  m->owner = me;
  m->count = 1;
  return 0;
}

int MutexUnlock(gaddr slot) {
  GuestSyncPoint sync;
  Mutex* m = MutexAt(slot);
  if (!m) return kEINVAL;
  std::lock_guard<std::mutex> l(m->mu);
  if (m->owner != Tid()) return kEPERM;
  if (--m->count == 0) {
    m->owner = 0;
    m->cv.notify_one();
  }
  return 0;
}

int MutexDestroy(gaddr slot) {
  std::lock_guard<std::mutex> l(g_mu);
  gaddr h = Rd64(slot);
  auto it = g_mutexes.find(h);
  if (it == g_mutexes.end()) return h <= 2 ? 0 : kEINVAL;
  if (it->second->owner) return kEBUSY;
  g_mutexes.erase(it);
  FreeHandle(h);
  Wr64(slot, 0);
  return 0;
}

int MutexattrInit(gaddr slot) {
  std::lock_guard<std::mutex> l(g_mu);
  gaddr h = NewHandle();
  g_mutex_attrs[h] = std::make_unique<MutexAttr>();
  Wr64(slot, h);
  return 0;
}

int MutexattrSettype(gaddr slot, int type) {
  std::lock_guard<std::mutex> l(g_mu);
  MutexAttr* a = Find(g_mutex_attrs, Rd64(slot));
  if (!a || type < kErrorCheck || type > kAdaptive) return kEINVAL;
  a->type = type;
  return 0;
}

template <typename T>
int DestroyObject(std::map<gaddr, std::unique_ptr<T>>& objs, gaddr slot) {
  std::lock_guard<std::mutex> l(g_mu);
  gaddr h = Rd64(slot);
  if (!objs.erase(h)) return kEINVAL;
  FreeHandle(h);
  Wr64(slot, 0);
  return 0;
}

// ---- condition variables ---------------------------------------------

// Releases the mutex the caller holds (all its recursion levels), waits
// for a signal, then takes the mutex back. The cond's lock is taken before
// the mutex is released, so a signal in between is not lost.
int CondWait(gaddr cond_slot, gaddr mutex_slot, int64_t timeout_us) {
  GuestSyncPoint sync;
  Cond* cv = LazyAt(g_conds, cond_slot);
  Mutex* m = MutexAt(mutex_slot);
  if (!cv || !m) return kEINVAL;
  const uint32_t me = Tid();
  std::unique_lock<std::mutex> cl(cv->mu);
  uint32_t count;
  {
    std::lock_guard<std::mutex> ml(m->mu);
    if (m->owner != me) return kEPERM;
    count = m->count;
    m->owner = 0;
    m->count = 0;
    m->cv.notify_one();
  }
  const uint64_t seq = cv->seq;
  bool signalled = true;
  if (timeout_us < 0)
    cv->cv.wait(cl, [&] { return cv->seq != seq; });
  else
    signalled = cv->cv.wait_for(cl, std::chrono::microseconds(timeout_us),
                                [&] { return cv->seq != seq; });
  cl.unlock();
  std::unique_lock<std::mutex> ml(m->mu);
  m->cv.wait(ml, [&] { return !m->owner; });
  m->owner = me;
  m->count = count;
  return signalled ? 0 : kETIMEDOUT;
}

int CondSignal(gaddr slot, bool all) {
  GuestSyncPoint sync;
  Cond* cv = LazyAt(g_conds, slot);
  if (!cv) return kEINVAL;
  std::lock_guard<std::mutex> l(cv->mu);
  cv->seq++;
  if (all)
    cv->cv.notify_all();
  else
    cv->cv.notify_one();
  return 0;
}

int CondInit(gaddr slot) {
  std::lock_guard<std::mutex> l(g_mu);
  gaddr h = NewHandle();
  g_conds[h] = std::make_unique<Cond>();
  Wr64(slot, h);
  return 0;
}

// ---- read-write locks -------------------------------------------------

int RwAcquire(gaddr slot, bool write, bool try_only) {
  GuestSyncPoint sync;
  RwLock* rw = LazyAt(g_rwlocks, slot);
  if (!rw) return kEINVAL;
  const uint32_t me = Tid();
  std::unique_lock<std::mutex> l(rw->mu);
  if (rw->writer == me) return kEDEADLK;
  auto free = [&] { return !rw->writer && (!write || !rw->readers); };
  if (!free()) {
    if (try_only) return kEBUSY;
    rw->cv.wait(l, free);
  }
  if (write)
    rw->writer = me;
  else
    rw->readers++;
  return 0;
}

int RwUnlock(gaddr slot) {
  GuestSyncPoint sync;
  RwLock* rw = LazyAt(g_rwlocks, slot);
  if (!rw) return kEINVAL;
  std::lock_guard<std::mutex> l(rw->mu);
  if (rw->writer == Tid())
    rw->writer = 0;
  else if (rw->readers)
    rw->readers--;
  else
    return kEPERM;
  rw->cv.notify_all();
  return 0;
}

int RwInit(gaddr slot) {
  std::lock_guard<std::mutex> l(g_mu);
  gaddr h = NewHandle();
  g_rwlocks[h] = std::make_unique<RwLock>();
  Wr64(slot, h);
  return 0;
}

// ---- threads ----------------------------------------------------------

// The calling thread's PThread (the main thread's made on first use).
std::shared_ptr<PThread> Self() {
  std::lock_guard<std::mutex> l(g_mu);
  Thread* t = CurrentThread();
  auto it = g_by_thread.find(t);
  if (it != g_by_thread.end()) return it->second;
  auto pt = std::make_shared<PThread>();
  pt->t = t;
  pt->handle = NewHandle();
  pt->name = "main";
  g_threads[pt->handle] = pt;
  g_by_thread[t] = pt;
  return pt;
}

std::shared_ptr<PThread> ThreadOf(gaddr h) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_threads.find(h);
  return it == g_threads.end() ? nullptr : it->second;
}

// POSIX: values with a destructor are cleared and passed to it, for up to
// four rounds while destructors set values again.
void RunKeyDestructors(Ctx& c, PThread& pt) {
  for (int round = 0; round < 4; round++) {
    std::vector<std::pair<gaddr, uint64_t>> calls;
    {
      std::lock_guard<std::mutex> l(g_mu);
      for (auto& [key, value] : pt.specific) {
        auto d = g_key_destructors.find(key);
        if (value && d != g_key_destructors.end() && d->second) {
          calls.push_back({d->second, value});
          value = 0;
        }
      }
    }
    if (calls.empty()) return;
    for (auto [fn, value] : calls) CallGuest(c, fn, {value});
  }
}

int Create(gaddr out, gaddr attr_slot, gaddr entry, uint64_t arg, gaddr name) {
  ThreadAttr attr;
  if (attr_slot) {
    std::lock_guard<std::mutex> l(g_mu);
    ThreadAttr* a = Find(g_thread_attrs, Rd64(attr_slot));
    if (!a) return kEINVAL;
    attr = *a;
  }
  Thread* t = CreateGuestThread(attr.stack);
  auto pt = std::make_shared<PThread>();
  pt->t = t;
  pt->detached = attr.detached;
  pt->prio = attr.prio;
  pt->affinity = attr.affinity;
  if (name) pt->name = GStr(name);
  {
    std::lock_guard<std::mutex> l(g_mu);
    pt->handle = NewHandle();
    g_threads[pt->handle] = pt;
    g_by_thread[t] = pt;
  }
  Wr64(out, pt->handle);
  GuestSyncFence();
  StartServiceThread(t, [pt, entry, arg] {
    Ctx& c = pt->t->ctx;
    uint64_t r;
    try {
      r = CallGuest(c, entry, {arg});
    } catch (const GuestThreadExit&) {
      r = pt->ret;  // scePthreadExit set it
    }
    RunKeyDestructors(c, *pt);
    std::lock_guard<std::mutex> l(pt->mu);
    pt->ret = r;
    pt->done = true;
    pt->cv.notify_all();
  });
  return 0;
}

int Join(gaddr h, gaddr ret_out) {
  std::shared_ptr<PThread> pt = ThreadOf(h);
  if (!pt) return kESRCH;
  if (pt->t == CurrentThread()) return kEDEADLK;
  {
    std::unique_lock<std::mutex> l(pt->mu);
    if (pt->detached) return kEINVAL;
    pt->cv.wait(l, [&] { return pt->done; });
  }
  GuestSyncFence();
  if (ret_out) Wr64(ret_out, pt->ret);
  return 0;
}

int ThreadAttrInit(gaddr slot) {
  std::lock_guard<std::mutex> l(g_mu);
  gaddr h = NewHandle();
  g_thread_attrs[h] = std::make_unique<ThreadAttr>();
  Wr64(slot, h);
  return 0;
}

ThreadAttr* AttrAt(gaddr slot) {
  std::lock_guard<std::mutex> l(g_mu);
  return Find(g_thread_attrs, Rd64(slot));
}

// ---- once -------------------------------------------------------------

// pthread_once_t: an int state (0: not run, 1: done, 2: running), then a
// mutex FreeBSD leaves unused.
std::mutex g_once_mu;
std::condition_variable g_once_cv;

int Once(Ctx& c, gaddr once, gaddr fn) {
  GuestSyncPoint sync;
  std::unique_lock<std::mutex> l(g_once_mu);
  for (;;) {
    const uint32_t state = Rd32(once);
    if (state == 1) return 0;
    if (state == 0) break;
    g_once_cv.wait(l);
  }
  Wr32(once, 2);
  l.unlock();
  CallGuest(c, fn, {});
  l.lock();
  Wr32(once, 1);
  g_once_cv.notify_all();
  return 0;
}

uint64_t Sce(int e) { return e ? SceError(e) : 0; }

}  // namespace
}  // namespace recomp::orbis::kernel

using namespace recomp::orbis::kernel;
using recomp::hle::Arg;

// ---- scePthread* (SCE error codes) -------------------------------------

ORBIS_HLE("libkernel", scePthreadMutexattrInit, 1) { return Sce(MutexattrInit(Arg(c, 0))); }
ORBIS_HLE("libkernel", scePthreadMutexattrDestroy, 1) {
  return Sce(DestroyObject(g_mutex_attrs, Arg(c, 0)));
}
ORBIS_HLE("libkernel", scePthreadMutexattrSettype, 2) {
  return Sce(MutexattrSettype(Arg(c, 0), int(Arg(c, 1))));
}
// Priority protocols do nothing on a host scheduler.
ORBIS_HLE("libkernel", scePthreadMutexattrSetprotocol, 2) { return 0; }
ORBIS_HLE("libkernel", scePthreadMutexInit, 3) {
  const recomp::gaddr name = Arg(c, 2);
  return Sce(MutexInit(Arg(c, 0), Arg(c, 1), name ? recomp::GStr(name).c_str() : nullptr));
}
ORBIS_HLE("libkernel", scePthreadMutexDestroy, 1) { return Sce(MutexDestroy(Arg(c, 0))); }
ORBIS_HLE("libkernel", scePthreadMutexLock, 1) { return Sce(MutexLock(Arg(c, 0), false, -1)); }
ORBIS_HLE("libkernel", scePthreadMutexTrylock, 1) { return Sce(MutexLock(Arg(c, 0), true, 0)); }
// The timeout is in microseconds from now.
ORBIS_HLE("libkernel", scePthreadMutexTimedlock, 2) {
  return Sce(MutexLock(Arg(c, 0), false, int64_t(Arg(c, 1))));
}
ORBIS_HLE("libkernel", scePthreadMutexUnlock, 1) { return Sce(MutexUnlock(Arg(c, 0))); }

ORBIS_HLE("libkernel", scePthreadCondattrInit, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  recomp::Wr64(Arg(c, 0), NewHandle());  // no condattr state is kept
  return 0;
}
ORBIS_HLE("libkernel", scePthreadCondattrDestroy, 1) { return 0; }
ORBIS_HLE("libkernel", scePthreadCondInit, 3) { return Sce(CondInit(Arg(c, 0))); }
ORBIS_HLE("libkernel", scePthreadCondDestroy, 1) { return Sce(DestroyObject(g_conds, Arg(c, 0))); }
ORBIS_HLE("libkernel", scePthreadCondWait, 2) { return Sce(CondWait(Arg(c, 0), Arg(c, 1), -1)); }
// The timeout is in microseconds from now.
ORBIS_HLE("libkernel", scePthreadCondTimedwait, 3) {
  return Sce(CondWait(Arg(c, 0), Arg(c, 1), int64_t(Arg(c, 2))));
}
ORBIS_HLE("libkernel", scePthreadCondSignal, 1) { return Sce(CondSignal(Arg(c, 0), false)); }
ORBIS_HLE("libkernel", scePthreadCondBroadcast, 1) { return Sce(CondSignal(Arg(c, 0), true)); }

ORBIS_HLE("libkernel", scePthreadRwlockattrInit, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  recomp::Wr64(Arg(c, 0), NewHandle());  // no rwlockattr state is kept
  return 0;
}
ORBIS_HLE("libkernel", scePthreadRwlockattrDestroy, 1) { return 0; }
ORBIS_HLE("libkernel", scePthreadRwlockInit, 3) { return Sce(RwInit(Arg(c, 0))); }
ORBIS_HLE("libkernel", scePthreadRwlockDestroy, 1) {
  return Sce(DestroyObject(g_rwlocks, Arg(c, 0)));
}
ORBIS_HLE("libkernel", scePthreadRwlockRdlock, 1) { return Sce(RwAcquire(Arg(c, 0), false, false)); }
ORBIS_HLE("libkernel", scePthreadRwlockWrlock, 1) { return Sce(RwAcquire(Arg(c, 0), true, false)); }
ORBIS_HLE("libkernel", scePthreadRwlockTryrdlock, 1) { return Sce(RwAcquire(Arg(c, 0), false, true)); }
ORBIS_HLE("libkernel", scePthreadRwlockTrywrlock, 1) { return Sce(RwAcquire(Arg(c, 0), true, true)); }
ORBIS_HLE("libkernel", scePthreadRwlockUnlock, 1) { return Sce(RwUnlock(Arg(c, 0))); }

// int scePthreadCreate(ScePthread* out, const ScePthreadAttr* attr,
// void* (*entry)(void*), void* arg, const char* name)
ORBIS_HLE("libkernel", scePthreadCreate, 5) {
  return Sce(Create(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4)));
}
ORBIS_HLE("libkernel", scePthreadJoin, 2) { return Sce(Join(Arg(c, 0), Arg(c, 1))); }
ORBIS_HLE("libkernel", scePthreadDetach, 1) {
  std::shared_ptr<PThread> pt = ThreadOf(Arg(c, 0));
  if (!pt) return SceError(kESRCH);
  std::lock_guard<std::mutex> l(pt->mu);
  pt->detached = true;
  return 0;
}
ORBIS_HLE("libkernel", scePthreadExit, 1) {
  std::shared_ptr<PThread> pt = Self();
  pt->ret = Arg(c, 0);
  throw recomp::GuestThreadExit{uint32_t(pt->ret)};
}
ORBIS_HLE("libkernel", scePthreadSelf, 0) { return Self()->handle; }
ORBIS_HLE("libkernel", scePthreadEqual, 2) { return Arg(c, 0) == Arg(c, 1); }
ORBIS_HLE("libkernel", scePthreadGetthreadid, 0) { return recomp::CurrentThread()->tid; }
ORBIS_HLE("libkernel", scePthreadYield, 0) {
  std::this_thread::yield();
  return 0;
}
ORBIS_HLE("libkernel", scePthreadGetprio, 2) {
  std::shared_ptr<PThread> pt = ThreadOf(Arg(c, 0));
  if (!pt) return SceError(kESRCH);
  recomp::Wr32(Arg(c, 1), uint32_t(pt->prio));
  return 0;
}
// Priorities and affinity are kept for the getters; the host schedules.
ORBIS_HLE("libkernel", scePthreadSetprio, 2) {
  std::shared_ptr<PThread> pt = ThreadOf(Arg(c, 0));
  if (!pt) return SceError(kESRCH);
  pt->prio = int(Arg(c, 1));
  return 0;
}
ORBIS_HLE("libkernel", scePthreadGetaffinity, 2) {
  std::shared_ptr<PThread> pt = ThreadOf(Arg(c, 0));
  if (!pt) return SceError(kESRCH);
  recomp::Wr64(Arg(c, 1), pt->affinity);
  return 0;
}
ORBIS_HLE("libkernel", scePthreadSetaffinity, 2) {
  std::shared_ptr<PThread> pt = ThreadOf(Arg(c, 0));
  if (!pt) return SceError(kESRCH);
  pt->affinity = Arg(c, 1);
  return 0;
}

ORBIS_HLE("libkernel", scePthreadAttrInit, 1) { return Sce(ThreadAttrInit(Arg(c, 0))); }
ORBIS_HLE("libkernel", scePthreadAttrDestroy, 1) {
  return Sce(DestroyObject(g_thread_attrs, Arg(c, 0)));
}
#define ATTR_SET(name, field, type)                    \
  ORBIS_HLE("libkernel", name, 2) {                     \
    ThreadAttr* a = AttrAt(Arg(c, 0));                  \
    if (!a) return SceError(kEINVAL);                   \
    a->field = type(Arg(c, 1));                         \
    return 0;                                           \
  }
ATTR_SET(scePthreadAttrSetstacksize, stack, uint64_t)
ATTR_SET(scePthreadAttrSetdetachstate, detached, bool)
ATTR_SET(scePthreadAttrSetinheritsched, inherit, int)
ATTR_SET(scePthreadAttrSetaffinity, affinity, uint64_t)
// SceKernelSchedParam: the priority as an int.
ORBIS_HLE("libkernel", scePthreadAttrSetschedparam, 2) {
  ThreadAttr* a = AttrAt(Arg(c, 0));
  if (!a) return SceError(kEINVAL);
  a->prio = int(recomp::Rd32(Arg(c, 1)));
  return 0;
}
ORBIS_HLE("libkernel", scePthreadAttrGetaffinity, 2) {
  ThreadAttr* a = AttrAt(Arg(c, 0));
  if (!a) return SceError(kEINVAL);
  recomp::Wr64(Arg(c, 1), a->affinity);
  return 0;
}
// int scePthreadAttrGet(ScePthread thread, ScePthreadAttr* attr): attr
// (made with scePthreadAttrInit) describes the thread.
ORBIS_HLE("libkernel", scePthreadAttrGet, 2) {
  std::shared_ptr<PThread> pt = ThreadOf(Arg(c, 0));
  ThreadAttr* a = AttrAt(Arg(c, 1));
  if (!pt || !a) return SceError(pt ? kEINVAL : kESRCH);
  a->stack = pt->t->stack_hi - pt->t->stack_lo;
  a->detached = pt->detached;
  a->prio = pt->prio;
  a->affinity = pt->affinity;
  return 0;
}

ORBIS_HLE("libkernel", scePthreadKeyCreate, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t key = g_next_key++;
  g_key_destructors[key] = Arg(c, 1);
  recomp::Wr32(Arg(c, 0), key);
  return 0;
}
ORBIS_HLE("libkernel", scePthreadKeyDelete, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_key_destructors.erase(uint32_t(Arg(c, 0))) ? 0 : SceError(kEINVAL);
}
ORBIS_HLE("libkernel", scePthreadSetspecific, 2) {
  std::shared_ptr<PThread> pt = Self();
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_key_destructors.count(uint32_t(Arg(c, 0)))) return SceError(kEINVAL);
  pt->specific[uint32_t(Arg(c, 0))] = Arg(c, 1);
  return 0;
}
ORBIS_HLE("libkernel", scePthreadGetspecific, 1) {
  std::shared_ptr<PThread> pt = Self();
  std::lock_guard<std::mutex> l(g_mu);
  auto it = pt->specific.find(uint32_t(Arg(c, 0)));
  return it == pt->specific.end() ? 0 : it->second;
}
ORBIS_HLE("libkernel", scePthreadOnce, 2) { return Sce(Once(c, Arg(c, 0), Arg(c, 1))); }

ORBIS_HLE("libkernel", sched_yield, 0) {
  std::this_thread::yield();
  return 0;
}

// ---- libScePosix pthread_* (errno values) -------------------------------

ORBIS_HLE("libScePosix", pthread_mutexattr_init, 1) { return MutexattrInit(Arg(c, 0)); }
ORBIS_HLE("libScePosix", pthread_mutexattr_destroy, 1) {
  return DestroyObject(g_mutex_attrs, Arg(c, 0));
}
ORBIS_HLE("libScePosix", pthread_mutexattr_settype, 2) {
  return MutexattrSettype(Arg(c, 0), int(Arg(c, 1)));
}
ORBIS_HLE("libScePosix", pthread_mutex_init, 2) { return MutexInit(Arg(c, 0), Arg(c, 1), nullptr); }
ORBIS_HLE("libScePosix", pthread_mutex_destroy, 1) { return MutexDestroy(Arg(c, 0)); }
ORBIS_HLE("libScePosix", pthread_mutex_lock, 1) { return MutexLock(Arg(c, 0), false, -1); }
ORBIS_HLE("libScePosix", pthread_mutex_trylock, 1) { return MutexLock(Arg(c, 0), true, 0); }
ORBIS_HLE("libScePosix", pthread_mutex_unlock, 1) { return MutexUnlock(Arg(c, 0)); }
ORBIS_HLE("libScePosix", pthread_attr_init, 1) { return ThreadAttrInit(Arg(c, 0)); }
ORBIS_HLE("libScePosix", pthread_attr_destroy, 1) {
  return DestroyObject(g_thread_attrs, Arg(c, 0));
}
ORBIS_HLE("libScePosix", pthread_attr_setstacksize, 2) {
  ThreadAttr* a = AttrAt(Arg(c, 0));
  if (!a) return kEINVAL;
  a->stack = Arg(c, 1);
  return 0;
}
// PTHREAD_CREATE_DETACHED is 1, JOINABLE 0.
ORBIS_HLE("libScePosix", pthread_attr_setdetachstate, 2) {
  ThreadAttr* a = AttrAt(Arg(c, 0));
  if (!a) return kEINVAL;
  a->detached = Arg(c, 1) == 1;
  return 0;
}
// int pthread_create_name_np(pthread_t* out, const pthread_attr_t* attr,
// void* (*entry)(void*), void* arg, const char* name)
ORBIS_HLE("libScePosix", pthread_create_name_np, 5) {
  return Create(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4));
}
ORBIS_HLE("libScePosix", pthread_join, 2) { return Join(Arg(c, 0), Arg(c, 1)); }
ORBIS_HLE("libScePosix", pthread_self, 0) { return Self()->handle; }
