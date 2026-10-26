// xboxkrnl: threads, TLS, time, the title's end and its notifications.

#include <bit>
#include <mutex>
#include <vector>

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";
constexpr uint32_t kKtApcDisable = 0xB0;

Thread* ThreadArg(ppc::Ctx& c, uint32_t kthread) {
  Thread* t = ThreadOf(kthread);
  if (!t) ppc::Fatal(&c, "%x is not a thread", kthread);
  return t;
}

// Dynamic TLS slots in use, process wide.
std::mutex g_tls;
std::vector<bool> g_tls_used;
// ExRegisterTitleTerminateNotification's registrations.
std::mutex g_terminate;
std::vector<uint32_t> g_terminate_routines;

}  // namespace

// ExCreateThread(*handle, stack_size, *thread_id, xapi_startup, start,
// context, flags).
HLE(kKrnl, 13, ExCreateThread) {
  uint32_t handle_ptr = Arg(c, 0), id_ptr = Arg(c, 2);
  Thread* t = CreateThread({Arg(c, 1), Arg(c, 3), Arg(c, 4), Arg(c, 5), Arg(c, 6)});
  if (handle_ptr) Wr32(handle_ptr, NewHandle(KThread(t)));
  if (id_ptr) Wr32(id_ptr, ThreadId(t));
  return kStatusSuccess;
}

HLE(kKrnl, 25, ExTerminateThread) { ExitThread(Arg(c, 0)); }

HLE(kKrnl, 102, KeGetCurrentProcessType) { return kTitleProcess; }

HLE(kKrnl, 338, KeTlsAlloc) {
  std::lock_guard<std::mutex> g(g_tls);
  g_tls_used.resize(Process().tls_slots);
  for (uint32_t i = 0; i < g_tls_used.size(); i++)
    if (!g_tls_used[i]) {
      g_tls_used[i] = true;
      return i;
    }
  return 0xFFFFFFFF;
}

HLE(kKrnl, 339, KeTlsFree) {
  std::lock_guard<std::mutex> g(g_tls);
  uint32_t i = Arg(c, 0);
  if (i >= g_tls_used.size() || !g_tls_used[i]) return 0;
  g_tls_used[i] = false;
  return 1;
}

HLE(kKrnl, 340, KeTlsGetValue) {
  uint32_t i = Arg(c, 0);
  return i < Process().tls_slots ? Rd32(TlsSlots(CurrentThread()) + 4 * i) : 0;
}

HLE(kKrnl, 341, KeTlsSetValue) {
  uint32_t i = Arg(c, 0);
  if (i >= Process().tls_slots) return 0;
  Wr32(TlsSlots(CurrentThread()) + 4 * i, Arg(c, 1));
  return 1;
}

// KeSetAffinityThread(kthread, affinity, *previous): the thread runs where
// the host schedules it; the processor it reports is the mask's lowest.
HLE(kKrnl, 151, KeSetAffinityThread) {
  Thread* t = ThreadArg(c, Arg(c, 0));
  uint32_t mask = Arg(c, 1), prev_ptr = Arg(c, 2);
  if (!mask) return kStatusInvalidParameter;
  if (prev_ptr) Wr32(prev_ptr, 1u << Rd8(Arg(c, 0) + 0xBF));
  SetProcessor(t, uint32_t(std::countr_zero(mask)));
  return kStatusSuccess;
}

// KeSetBasePriorityThread(kthread, increment): priorities are the host's.
HLE(kKrnl, 153, KeSetBasePriorityThread) {
  ThreadArg(c, Arg(c, 0));
  return 0;
}

HLE(kKrnl, 146, KeResumeThread) { return ResumeThread(ThreadArg(c, Arg(c, 0))); }

// NtResumeThread(handle, *previous count), NtSuspendThread(handle, *previous count).
HLE(kKrnl, 245, NtResumeThread) {
  uint32_t obj = HandleObject(Arg(c, 0));
  Thread* t = ThreadOf(obj);
  if (!t) return kStatusInvalidHandle;
  uint32_t old = ResumeThread(t);
  if (Arg(c, 1)) Wr32(Arg(c, 1), old);
  return kStatusSuccess;
}

HLE(kKrnl, 252, NtSuspendThread) {
  Thread* t = ThreadOf(HandleObject(Arg(c, 0)));
  if (!t) return kStatusInvalidHandle;
  uint32_t old = SuspendThread(t);
  if (Arg(c, 1)) Wr32(Arg(c, 1), old);
  return kStatusSuccess;
}

// KeDelayExecutionThread(mode, alertable, *interval).
HLE(kKrnl, 90, KeDelayExecutionThread) {
  return Delay(int64_t(Rd64(Arg(c, 2))), Arg(c, 1) != 0);
}

// Critical regions disable kernel APCs (none are delivered yet): the
// thread's count goes negative while inside one.
HLE(kKrnl, 95, KeEnterCriticalRegion) {
  uint32_t k = KThread(CurrentThread()) + kKtApcDisable;
  Wr32(k, Rd32(k) - 1);
  return 0;
}
HLE(kKrnl, 125, KeLeaveCriticalRegion) {
  uint32_t k = KThread(CurrentThread()) + kKtApcDisable;
  Wr32(k, Rd32(k) + 1);
  return 0;
}

HLE(kKrnl, 131, KeQueryPerformanceFrequency) { return kTimeBaseFrequency; }

HLE(kKrnl, 132, KeQuerySystemTime) {
  Wr64(Arg(c, 0), SystemTime());
  return 0;
}

// HalReturnToFirmware(routine): the title's end, whatever the routine.
HLE(kKrnl, 40, HalReturnToFirmware) { ExitProcess(0); }

// ExRegisterTitleTerminateNotification(registration, create): adds (or,
// create 0, removes) a routine for the title's end; the registration is
// the routine, its priority and a list entry (0x10 bytes). The routines
// are kept, not run: the title's end here is the host process's, with no
// system left to tidy for another title (as in Xenia).
HLE(kKrnl, 21, ExRegisterTitleTerminateNotification) {
  std::lock_guard<std::mutex> g(g_terminate);
  uint32_t reg = Arg(c, 0);
  std::erase(g_terminate_routines, reg);
  if (Arg(c, 1)) g_terminate_routines.push_back(reg);
  return 0;
}

}  // namespace recomp::xbox360
