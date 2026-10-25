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
#elif defined(__APPLE__) && defined(__x86_64__)
  const auto& es = u->uc_mcontext->__es;
  const auto& ss = u->uc_mcontext->__ss;
  f.write_known = true;
  f.write = (es.__err & 2) != 0;
  f.pc = uintptr_t(ss.__rip);
  f.caller = *reinterpret_cast<const uintptr_t*>(ss.__rsp);
#else
  (void)u;
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

bool RedirectToCall(void* uc, void (*fn)()) {
#if defined(__APPLE__) && defined(__aarch64__)
  auto& ss = static_cast<ucontext_t*>(uc)->uc_mcontext->__ss;
  // The unwinder looks up a return address minus one, which lands inside
  // the faulting instruction (arm64 instructions are 4 bytes).
  ss.__lr = ss.__pc + 4;
  ss.__pc = reinterpret_cast<uint64_t>(fn);
  return true;
#else
  (void)uc;
  (void)fn;
  return false;
#endif
}

}  // namespace recomp::platform
