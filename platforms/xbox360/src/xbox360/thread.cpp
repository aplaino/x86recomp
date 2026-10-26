// Guest threads (kernel.h): each runs on its own host thread with its own
// context, guest stack, KTHREAD, KPCR (r13) and TLS.
//
// The structures' layouts are Xenia's and ReXGlue's (X_KTHREAD, X_KPCR),
// checked against what the system's own code and titles read through r13:
// KPCR+0x100 the current KTHREAD, +0x10C the processor number, +0x150 the
// DPC-active flag; KTHREAD+0x14C the thread id, +0x160 the last error
// (read and written), +0x58 (read; its meaning is unknown, here 0). Every
// guest thread has a KPCR of its own (the console has one per hardware
// thread and switches KPCR+0x100), so r13 never changes under a thread.

#include <pthread.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "ppc/dispatch.h"
#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {

struct Thread {
  ppc::Ctx c{};
  ThreadParams p{};
  void (*system_body)(void* arg) = nullptr;  // a system thread's
  void* system_arg = nullptr;
  uint32_t kthread = 0, pcr = 0, tls = 0;
  uint32_t stack_alloc = 0, stack_base = 0;
  uint32_t id = 0;
  std::mutex lock;
  std::condition_variable resumed;
  uint32_t suspend = 0;
  bool started = false;
};

namespace {

using ppc::Fatal;

constexpr uint32_t kKThreadSize = 0xAB0, kPcrSize = 0x2D8;
// KTHREAD fields.
constexpr uint32_t kKtStackBase = 0x5C, kKtStackLimit = 0x60, kKtTls = 0x68,
                   kKtProcessTypeDup = 0x72, kKtProcessType = 0x73, kKtCpu = 0xBF,
                   kKtPrcb = 0xC0, kKtPrcb2 = 0xC4, kKtStackAlloc = 0xD0, kKtCreateTime = 0x130,
                   kKtExitStatus = 0x140, kKtTimerList = 0x144, kKtId = 0x14C, kKtStart = 0x150,
                   kKtList154 = 0x154, kKtCreationFlags = 0x16C;
// KPCR fields.
constexpr uint32_t kPcrTls = 0x0, kPcrSelf = 0x30, kPcrStackBase = 0x70, kPcrStackEnd = 0x74,
                   kPcrPrcb = 0x100, kPcrCurrentThread = 0x100, kPcrCpu = 0x10C, kPcrDpcActive = 0x150,
                   kPcrPrcbPtr = 0x2A8;
// A system thread's guest stack, for the guest code it calls.
constexpr uint32_t kSystemStack = 64 << 10;
// The host stack lifted code runs on: its frames are the guest's calls.
constexpr size_t kHostStack = 16 << 20;
// Below the top of a new guest stack, room for the parameter area of the
// call into the start routine, which the routine may write.
constexpr uint32_t kStackTopReserve = 0x100;
// VSCR[NJ], set for new threads as the operating systems for this VMX set
// it (a console check is pending).
constexpr uint32_t kVscrNj = 0x00010000;

struct Exit {
  uint32_t status;
};

thread_local Thread* t_current;
std::mutex g_threads;
std::unordered_map<uint32_t, Thread*> g_by_kthread;
uint32_t g_next_id = 1;
uint32_t g_live = 0;
ProcessInfo g_process;

uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

void Destroy(uint32_t kthread) {
  Thread* t;
  {
    std::lock_guard<std::mutex> g(g_threads);
    t = g_by_kthread.at(kthread);
    g_by_kthread.erase(kthread);
  }
  Release(t->stack_alloc);
  SystemFree(t->tls);
  SystemFree(t->pcr);
  SystemFree(t->kthread);
  delete t;
}

// The thread's end: its exit status, its KTHREAD signalled, the process's
// end with its last thread.
void Finish(Thread* t, uint32_t status) {
  Wr32(t->kthread + kKtExitStatus, status);
  SetSignal(t->kthread, 1);
  bool last;
  {
    std::lock_guard<std::mutex> g(g_threads);
    last = --g_live == 0;
  }
  if (last) ExitProcess(int(status));
  Dereference(t->kthread);
}

void* Run(void* arg) {
  Thread* t = static_cast<Thread*>(arg);
  t_current = t;
  if (t->system_body) {
    t->system_body(t->system_arg);
    return nullptr;
  }
  {
    std::unique_lock<std::mutex> l(t->lock);
    t->resumed.wait(l, [t] { return t->suspend == 0; });
    t->started = true;
  }
  uint32_t status;
  try {
    ppc::Ctx& c = t->c;
    uint32_t target = t->p.start;
    if (t->p.startup) {
      c.r[3] = t->p.start;
      c.r[4] = t->p.context;
      target = t->p.startup;
    } else {
      c.r[3] = t->p.context;
    }
    ppc::Dispatch(c, target, 0);
    status = uint32_t(c.r[3]);
    if (t->p.main) ExitProcess(int(status));
  } catch (const Exit& e) {
    status = e.status;
  }
  Finish(t, status);
  return nullptr;
}

}  // namespace

namespace {

Thread* NewThread(const ThreadParams& p, void (*system_body)(void*), void* system_arg) {
  auto* t = new Thread;
  t->p = p;
  t->system_body = system_body;
  t->system_arg = system_arg;
  uint32_t size = std::max(0x4000u, RoundUp(p.stack_size ? p.stack_size : g_process.stack_size, 0x1000));
  uint32_t reserved = RoundUp(size, HeapPageSize(Heap::kVirtual64k));
  t->stack_alloc = Reserve(Heap::kVirtual64k, 0, reserved, 0, true);
  if (!t->stack_alloc || !Commit(t->stack_alloc, reserved, kPageReadWrite))
    Fatal(nullptr, "out of guest memory for a %u-byte thread stack", size);
  t->stack_base = t->stack_alloc + reserved;
  t->kthread = SystemAlloc(kKThreadSize);
  t->pcr = SystemAlloc(kPcrSize);
  uint32_t tls_size = g_process.tls_data_size + 4 * g_process.tls_slots;
  t->tls = SystemAlloc(std::max(tls_size, 16u));
  if (g_process.tls_raw_size)
    std::memcpy(Mem() + t->tls, Mem() + g_process.tls_raw_data, g_process.tls_raw_size);
  {
    std::lock_guard<std::mutex> g(g_threads);
    t->id = g_next_id++;
    g_by_kthread[t->kthread] = t;
    if (!system_body) g_live++;
  }

  uint32_t k = t->kthread, pcr = t->pcr;
  InitObject(k, kThreadObject, 0);
  Wr32(k + kKtStackBase, t->stack_base);
  Wr32(k + kKtStackLimit, t->stack_base - size);
  Wr32(k + kKtTls, t->tls);
  Wr8(k + kKtProcessTypeDup, kTitleProcess);
  Wr8(k + kKtProcessType, kTitleProcess);
  Wr32(k + kKtPrcb, pcr + kPcrPrcb);
  Wr32(k + kKtPrcb2, pcr + kPcrPrcb);
  Wr32(k + kKtStackAlloc, t->stack_alloc);
  Wr64(k + kKtCreateTime, SystemTime());
  for (uint32_t list : {kKtTimerList, kKtList154}) {
    Wr32(k + list, k + list);
    Wr32(k + list + 4, k + list);
  }
  Wr32(k + kKtId, t->id);
  Wr32(k + kKtStart, p.start);
  Wr32(k + kKtCreationFlags, p.flags);
  Wr32(pcr + kPcrTls, t->tls);
  Wr64(pcr + kPcrSelf, pcr);
  Wr32(pcr + kPcrStackBase, t->stack_base);
  Wr32(pcr + kPcrStackEnd, t->stack_base - size);
  Wr32(pcr + kPcrCurrentThread, k);
  Wr32(pcr + kPcrPrcbPtr, pcr + kPcrPrcb);

  t->c.mem = Mem();
  t->c.r[1] = t->stack_base - kStackTopReserve;
  t->c.r[13] = pcr;
  t->c.vscr = kVscrNj;
  t->suspend = p.flags & kCreateSuspended ? 1 : 0;
  // The thread's own reference, dropped when it ends.
  Own(k, Destroy);
  Reference(k);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kHostStack);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t host;
  int err = pthread_create(&host, &attr, Run, t);
  pthread_attr_destroy(&attr);
  if (err) Fatal(nullptr, "cannot start a host thread: %s", std::strerror(err));
  return t;
}

}  // namespace

Thread* CreateThread(const ThreadParams& p) { return NewThread(p, nullptr, nullptr); }

Thread* CreateSystemThread(void (*body)(void* arg), void* arg) {
  return NewThread({kSystemStack, 0, 0, 0, 0}, body, arg);
}

uint32_t CallGuest(uint32_t fn, uint32_t a0, uint32_t a1) {
  ppc::Ctx& c = t_current->c;
  c.r[1] = t_current->stack_base - kStackTopReserve;
  c.r[3] = a0;
  c.r[4] = a1;
  ppc::Dispatch(c, fn, 0);
  return uint32_t(c.r[3]);
}

uint32_t CallDpc(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t cpu) {
  Thread* t = t_current;
  SetProcessor(t, cpu);
  uint8_t irql = Rd8(t->pcr + kPcrIrql);
  Wr8(t->pcr + kPcrIrql, kDispatchLevel);
  Wr32(t->pcr + kPcrDpcActive, 1);
  uint32_t r = CallGuest(fn, a0, a1);
  Wr32(t->pcr + kPcrDpcActive, 0);
  Wr8(t->pcr + kPcrIrql, irql);
  return r;
}

Thread* CurrentThread() { return t_current; }
uint32_t KThread(const Thread* t) { return t ? t->kthread : 0; }
uint32_t ThreadId(const Thread* t) { return t->id; }
uint32_t TlsSlots(const Thread* t) { return t->tls + g_process.tls_data_size; }

Thread* ThreadOf(uint32_t kthread) {
  std::lock_guard<std::mutex> g(g_threads);
  auto it = g_by_kthread.find(kthread);
  return it == g_by_kthread.end() ? nullptr : it->second;
}

void SetProcessor(Thread* t, uint32_t cpu) {
  Wr8(t->kthread + kKtCpu, uint8_t(cpu));
  Wr8(t->pcr + kPcrCpu, uint8_t(cpu));
}

uint32_t SuspendThread(Thread* t) {
  std::lock_guard<std::mutex> l(t->lock);
  // A running host thread cannot be stopped where it is; only a thread
  // that has not started yet can be held.
  if (t->started) Fatal(&t->c, "suspending a running thread is not supported yet");
  return t->suspend++;
}

uint32_t ResumeThread(Thread* t) {
  std::lock_guard<std::mutex> l(t->lock);
  uint32_t old = t->suspend;
  if (t->suspend && --t->suspend == 0) t->resumed.notify_all();
  return old;
}

void ExitThread(uint32_t status) { throw Exit{status}; }

void SetProcessInfo(const ProcessInfo& info) { g_process = info; }
const ProcessInfo& Process() { return g_process; }

void ExitProcess(int status) {
  std::fflush(nullptr);
  std::_Exit(status);
}

}  // namespace recomp::xbox360
