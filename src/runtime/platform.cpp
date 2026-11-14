// Host platform specifics; see platform.h.

#if defined(__APPLE__)
// Darwin hides the (deprecated, still working) ucontext routines unless
// _XOPEN_SOURCE is set, and _XOPEN_SOURCE hides the BSD API unless
// _DARWIN_C_SOURCE is set too.
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#endif

#include "runtime/platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#else
#include <sys/prctl.h>
#endif

namespace recomp::platform {

namespace {
ucontext_t g_caller_ctx, g_stack_ctx;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
void RunOnStack(void* stack, size_t size, void (*fn)()) {
  getcontext(&g_stack_ctx);
  g_stack_ctx.uc_stack.ss_sp = stack;
  g_stack_ctx.uc_stack.ss_size = size;
  g_stack_ctx.uc_link = &g_caller_ctx;  // resume here on return
  makecontext(&g_stack_ctx, fn, 0);
  swapcontext(&g_caller_ctx, &g_stack_ctx);
}

struct Coroutine {
  ucontext_t uc;
  void (*fn)(void*) = nullptr;
  void* arg = nullptr;
  bool started = true;
};

namespace {
thread_local Coroutine* t_starting = nullptr;

void CoroutineStart() {
  Coroutine* c = t_starting;
  c->fn(c->arg);
  abort();  // fn must not return
}
}  // namespace

Coroutine* NewCoroutine(void* stack, size_t size, void (*fn)(void*), void* arg) {
  Coroutine* c = new Coroutine;
  getcontext(&c->uc);
  c->uc.uc_stack.ss_sp = stack;
  c->uc.uc_stack.ss_size = size;
  c->uc.uc_link = nullptr;
  makecontext(&c->uc, CoroutineStart, 0);
  c->fn = fn;
  c->arg = arg;
  c->started = false;
  return c;
}

Coroutine* ThreadCoroutine() { return new Coroutine; }

void SwitchCoroutine(Coroutine* from, Coroutine* to) {
  if (!to->started) {
    to->started = true;
    t_starting = to;
  }
  swapcontext(&from->uc, &to->uc);
}

void FreeCoroutine(Coroutine* c) { delete c; }
#pragma clang diagnostic pop

FaultContext ReadFaultContext(const void* uc) {
  FaultContext f;
  const auto* u = static_cast<const ucontext_t*>(uc);
#if defined(__linux__) && defined(__x86_64__)
  const greg_t* g = u->uc_mcontext.gregs;
  f.write_known = true;
  f.write = (g[REG_ERR] & 2) != 0;  // page-fault error code: W/R bit
  f.pc = uintptr_t(g[REG_RIP]);
  f.caller = *reinterpret_cast<const uintptr_t*>(g[REG_RSP]);
#elif defined(__APPLE__) && defined(__aarch64__)
  const auto& es = u->uc_mcontext->__es;
  const auto& ss = u->uc_mcontext->__ss;
  // ESR_EL1 of a data abort: ISS bit 6 is WnR (write, not read).
  f.write_known = true;
  f.write = (es.__esr >> 6) & 1;
  f.pc = uintptr_t(ss.__pc);
  f.caller = uintptr_t(ss.__lr);
#elif defined(__linux__) && defined(__aarch64__)
  const mcontext_t& m = u->uc_mcontext;
  f.pc = uintptr_t(m.pc);
  f.caller = uintptr_t(m.regs[30]);
  // The kernel adds the fault's ESR as a record (esr_context, magic
  // ESR_MAGIC) to the list in __reserved, which ends with a zero magic.
  // A data abort's (EC 0x24 lower EL, 0x25 same EL) ISS bit 6 is WnR.
  constexpr uint32_t kEsrMagic = 0x45535201;
  for (size_t at = 0; at + 8 <= sizeof m.__reserved;) {
    uint32_t magic, size;
    memcpy(&magic, m.__reserved + at, 4);
    memcpy(&size, m.__reserved + at + 4, 4);
    if (!magic || size < 8) break;
    if (magic == kEsrMagic && size >= 16 && at + 16 <= sizeof m.__reserved) {
      uint64_t esr;
      memcpy(&esr, m.__reserved + at + 8, 8);
      const uint64_t ec = esr >> 26;
      if (ec == 0x24 || ec == 0x25) {
        f.write_known = true;
        f.write = (esr >> 6) & 1;
      }
      break;
    }
    at += size;
  }
#elif defined(__APPLE__) && defined(__x86_64__)
  const auto& es = u->uc_mcontext->__es;
  const auto& ss = u->uc_mcontext->__ss;
  f.write_known = true;
  f.write = (es.__err & 2) != 0;
  f.pc = uintptr_t(ss.__rip);
  f.caller = *reinterpret_cast<const uintptr_t*>(ss.__rsp);
#else
// The x86 core needs the fault's kind and where it was (guest exception
// dispatch): without them guest faults would read as reads at pc 0.
#error "no fault context for this host (Linux and macOS, x86-64 and arm64, have one)"
#endif
  return f;
}

#if defined(__APPLE__)
uint64_t MonotonicNs() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

void SleepUntilNs(uint64_t t) {
  // CLOCK_UPTIME_RAW is mach_absolute_time in nanoseconds.
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t i;
    mach_timebase_info(&i);
    return i;
  }();
  auto ticks = [](uint64_t ns) {
    return uint64_t((unsigned __int128)ns * tb.denom / tb.numer);
  };
  // A timeshare thread's timer gets coalescing leeway: it woke 1-3.5 ms
  // late (no QoS class, latency tier or task role brought it under 1 ms).
  // A time-constraint (real-time) thread's timer has none and wakes
  // within some tens of microseconds. The thread is real-time only while
  // it waits: past its computation budget the kernel would demote it.
  const thread_act_t self = pthread_mach_thread_np(pthread_self());
  thread_time_constraint_policy_data_t rt = {};
  rt.computation = uint32_t(ticks(500000));  // 0.5 ms after waking
  rt.constraint = uint32_t(ticks(1000000));
  rt.preemptible = 1;
  const bool set = thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY,
                                     reinterpret_cast<thread_policy_t>(&rt),
                                     THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
  while (MonotonicNs() < t) mach_wait_until(ticks(t));
  if (set) {
    thread_standard_policy_data_t ts = {};
    thread_policy_set(self, THREAD_STANDARD_POLICY, reinterpret_cast<thread_policy_t>(&ts),
                      THREAD_STANDARD_POLICY_COUNT);
  }
}
#else
uint64_t MonotonicNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec);
}

void SleepUntilNs(uint64_t t) {
  // The default slack (50 us) would delay every wake-up by about that.
  static thread_local bool slack_set = false;
  if (!slack_set) {
    prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
    slack_set = true;
  }
  timespec ts;
  ts.tv_sec = time_t(t / 1000000000u);
  ts.tv_nsec = long(t % 1000000000u);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}
#endif

#if defined(__x86_64__)
namespace {
thread_local void (*t_redirected)() = nullptr;

// What RedirectToCall makes the faulting instruction call on x86-64: the
// stack there need not be aligned for a call, so this realigns it (its
// frame is unwound through rbp) and calls the function asked for.
[[noreturn]] __attribute__((force_align_arg_pointer, noinline)) void CallRedirected() {
  void (*fn)() = t_redirected;
  t_redirected = nullptr;
  fn();
  abort();  // fn must not return
}
}  // namespace
#endif

bool RedirectToCall(void* uc, void (*fn)()) {
  // The unwinder looks up a return address minus one: one past the start
  // of the faulting instruction lands inside it on x86-64, pc + 4 (its
  // end) on arm64, whose instructions are 4 bytes.
#if defined(__aarch64__)
#if defined(__APPLE__)
  auto& ss = static_cast<ucontext_t*>(uc)->uc_mcontext->__ss;
  auto &pc = ss.__pc, &lr = ss.__lr;
#else
  mcontext_t& m = static_cast<ucontext_t*>(uc)->uc_mcontext;
  auto &pc = m.pc, &lr = m.regs[30];
#endif
  lr = pc + 4;
  pc = reinterpret_cast<uintptr_t>(fn);
  return true;
#elif defined(__x86_64__)
#if defined(__APPLE__)
  auto& ss = static_cast<ucontext_t*>(uc)->uc_mcontext->__ss;
  auto &rip = ss.__rip, &rsp = ss.__rsp;
#else
  greg_t* g = static_cast<ucontext_t*>(uc)->uc_mcontext.gregs;
  uint64_t &rip = reinterpret_cast<uint64_t&>(g[REG_RIP]),
           &rsp = reinterpret_cast<uint64_t&>(g[REG_RSP]);
#endif
  // A call's push: it lands where the faulting function's red zone would
  // be, so code that can fault is built without one (RECOMP_FAULT_FRAME_FLAGS).
  rsp -= 8;
  *reinterpret_cast<uint64_t*>(rsp) = rip + 1;
  rip = reinterpret_cast<uintptr_t>(&CallRedirected);
  t_redirected = fn;
  return true;
#else
  (void)uc;
  (void)fn;
  return false;
#endif
}

}  // namespace recomp::platform
