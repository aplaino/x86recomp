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
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp {

namespace {

std::mutex g_wait_mu;
std::condition_variable g_wait_cv;

struct Waitable : KObject {
  using KObject::KObject;
  // Under g_wait_mu: is the object signalled for thread `tid`?
  virtual bool Signalled(uint32_t tid) const = 0;
  // Under g_wait_mu: a wait on it was satisfied; consume (auto-reset,
  // mutex ownership, semaphore count).
  virtual void Acquire(uint32_t tid) { (void)tid; }
};

struct EventObject : Waitable {
  EventObject(bool manual, bool initial)
      : Waitable(Type::Event), manual_reset(manual), signalled(initial) {}
  bool Signalled(uint32_t) const override { return signalled; }
  void Acquire(uint32_t) override {
    if (!manual_reset) signalled = false;
  }
  bool manual_reset;
  bool signalled;
};

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

struct ThreadObject : Waitable {
  ThreadObject() : Waitable(Type::Thread) {}
  bool Signalled(uint32_t) const override { return finished; }
  Thread* thread = nullptr;
  uint32_t entry = 0, param = 0;
  std::atomic<bool> started{false};  // CreateThread vs ResumeThread races
  bool finished = false;  // under g_wait_mu
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
          objs[i]->Acquire(tid);
          return kWaitObject0 + uint32_t(i);
        }
    }
    if (timeout_ms == 0) return kWaitTimeout;
    if (timeout_ms == kInfinite) {
      g_wait_cv.wait(l);
    } else if (g_wait_cv.wait_until(l, deadline) == std::cv_status::timeout) {
      return kWaitTimeout;  // deadline passed: no re-check
    }
  }
}

void SignalChanged() { g_wait_cv.notify_all(); }

std::string NameOf(uint32_t va) { return va ? GStr(va) : std::string(); }

// Registers a named object or returns the existing one (ERROR_ALREADY_EXISTS).
template <typename Make>
uint32_t CreateNamed(uint32_t name_va, Make make) {
  std::string name = NameOf(name_va);
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

HLE("kernel32.dll", CreateEventA, 4) {
  bool manual = Arg(c, 1), initial = Arg(c, 2);
  return CreateNamed(Arg(c, 3), [&] {
    return std::make_shared<EventObject>(manual, initial);
  });
}

HLE("kernel32.dll", SetEvent, 1) {
  GuestSyncPoint sync;
  auto e = std::dynamic_pointer_cast<EventObject>(GetHandle(Arg(c, 0)));
  if (!e) return 0;
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    e->signalled = true;
  }
  SignalChanged();
  return 1;
}

HLE("kernel32.dll", ResetEvent, 1) {
  GuestSyncPoint sync;
  auto e = std::dynamic_pointer_cast<EventObject>(GetHandle(Arg(c, 0)));
  if (!e) return 0;
  std::lock_guard<std::mutex> l(g_wait_mu);
  e->signalled = false;
  return 1;
}

// ---- mutexes ----------------------------------------------------------------

HLE("kernel32.dll", CreateMutexA, 3) {
  GuestSyncPoint sync;
  bool own = Arg(c, 1);
  uint32_t tid = CurrentThread()->tid;
  uint32_t h = CreateNamed(Arg(c, 2), [] {
    return std::make_shared<MutexObject>();
  });
  if (own) {
    auto m = std::dynamic_pointer_cast<MutexObject>(GetHandle(h));
    std::lock_guard<std::mutex> l(g_wait_mu);
    if (m->owner == 0 || m->owner == tid) m->Acquire(tid);
  }
  return h;
}

// OpenMutexA/OpenFileMappingA(access, inherit, name): a new handle to an
// existing named object of that type.
uint32_t OpenNamed(uint32_t name_va, KObject::Type type) {
  std::string name = NameOf(name_va);
  std::lock_guard<std::mutex> l(g_wait_mu);
  auto it = g_named.find(name);
  if (it == g_named.end() || it->second->type != type) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  return NewHandle(it->second);
}

HLE("kernel32.dll", OpenMutexA, 3) { return OpenNamed(Arg(c, 2), KObject::Type::Mutex); }

HLE("kernel32.dll", ReleaseMutex, 1) {
  GuestSyncPoint sync;
  auto m = std::dynamic_pointer_cast<MutexObject>(GetHandle(Arg(c, 0)));
  if (!m) return 0;
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    if (m->owner != CurrentThread()->tid) return 0;
    if (--m->count == 0) m->owner = 0;
  }
  SignalChanged();
  return 1;
}

// ---- waits ------------------------------------------------------------------

HLE("kernel32.dll", WaitForSingleObject, 2) {
  GuestSyncPoint sync;
  auto w = AsWaitable(Arg(c, 0));
  if (!w) {
    SetLastError(kErrorInvalidHandle);
    return kWaitFailed;
  }
  return WaitObjects({w}, false, Arg(c, 1));
}

HLE("kernel32.dll", WaitForMultipleObjects, 4) {
  GuestSyncPoint sync;
  uint32_t n = Arg(c, 0), arr = Arg(c, 1);
  std::vector<std::shared_ptr<Waitable>> objs;
  for (uint32_t i = 0; i < n; i++) {
    auto w = AsWaitable(Rd32(arr + 4 * i));
    if (!w) {
      SetLastError(kErrorInvalidHandle);
      return kWaitFailed;
    }
    objs.push_back(w);
  }
  return WaitObjects(objs, Arg(c, 2) != 0, Arg(c, 3));
}

// ---- threads ----------------------------------------------------------------

// Starts the thread once. pthread_create synchronises memory: what the
// creator wrote before it is visible to the new thread.
bool RunThreadObject(const std::shared_ptr<ThreadObject>& to) {
  if (to->started.exchange(true)) return false;
  StartHostThread(to->thread, to->entry, to->param);
  return true;
}

HLE("kernel32.dll", CreateThread, 6) {
  GuestSyncPoint sync;
  uint32_t stack = Arg(c, 1), entry = Arg(c, 2), param = Arg(c, 3),
           flags = Arg(c, 4), tid_out = Arg(c, 5);
  auto to = std::make_shared<ThreadObject>();
  to->thread = CreateGuestThread(stack ? stack : g_proc->img.stack_reserve);
  to->entry = entry;
  to->param = param;
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    g_thread_objs[to->thread->tid] = to;
  }
  if (tid_out) Wr32(tid_out, to->thread->tid);
  uint32_t h = NewHandle(to);
  RECOMP_TRACE("  CreateThread entry=%08X param=%08X -> tid %04X%s\n", entry,
            param, to->thread->tid, (flags & 4) ? " (suspended)" : "");
  if (!(flags & 4)) RunThreadObject(to);
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
  throw GuestThreadExit{Arg(c, 0)};
}

HLE("kernel32.dll", GetThreadPriority, 1) { return 0; }
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
  SetLastError(kErrorAccessDenied);
  return 0;
}

HLE("kernel32.dll", CreateToolhelp32Snapshot, 2) {
  SetLastError(kErrorAccessDenied);
  return 0xFFFFFFFF;
}
HLE("kernel32.dll", Thread32First, 2) { return 0; }
HLE("kernel32.dll", Thread32Next, 2) { return 0; }

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
  return CreateNamed(name, [&] { return m; });
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

void OnGuestThreadFinished(uint32_t tid) {
  GuestSyncPoint sync;
  {
    std::lock_guard<std::mutex> l(g_wait_mu);
    auto it = g_thread_objs.find(tid);
    if (it != g_thread_objs.end()) it->second->finished = true;
  }
  SignalChanged();
}

}  // namespace recomp
