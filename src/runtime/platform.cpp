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
#include <pthread.h>
#include <sys/resource.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>

#include <cstdio>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#else
#include <sys/prctl.h>
#endif

namespace recomp::platform {

namespace {
ucontext_t g_caller_ctx, g_stack_ctx;
}

#if defined(__APPLE__) && defined(__aarch64__) && TARGET_OS_IPHONE
// iOS / tvOS: the ucontext routines are there but swapcontext returns at
// once, so the switch to the private stack is done here: sp moves to its
// top, fn runs, sp comes back. fn is an ordinary call (frame records and
// unwind info as usual); everything the AAPCS64 lets it clobber is listed.
void RunOnStack(void* stack, size_t size, void (*fn)()) {
  const uintptr_t top = (reinterpret_cast<uintptr_t>(stack) + size) & ~uintptr_t(15);
  asm volatile(
      "mov x9, sp\n"
      "mov sp, %0\n"
      "stp x9, x30, [sp, #-16]!\n"
      "blr %1\n"
      "ldp x9, x30, [sp], #16\n"
      "mov sp, x9\n"
      :
      : "r"(top), "r"(fn)
      : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
        "x13", "x14", "x15", "x16", "x17", "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6",
        "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26",
        "v27", "v28", "v29", "v30", "v31", "cc", "memory");
}
#else
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
#pragma clang diagnostic pop
#endif

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
namespace {
// Mach counts durations and deadlines in absolute-time units, and
// CLOCK_UPTIME_RAW is mach_absolute_time in nanoseconds.
uint64_t MachTicks(uint64_t ns) {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t i;
    mach_timebase_info(&i);
    return i;
  }();
  return uint64_t((unsigned __int128)ns * tb.denom / tb.numer);
}
}  // namespace

uint64_t MonotonicNs() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

void SleepUntilNs(uint64_t t) {
  auto ticks = MachTicks;
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
namespace {
// The default slack (50 us) would delay every wake-up by about that.
void LowTimerSlack() {
  static thread_local bool set = false;
  if (!set) {
    prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
    set = true;
  }
}
}  // namespace

uint64_t MonotonicNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec);
}

void SleepUntilNs(uint64_t t) {
  LowTimerSlack();
  timespec ts;
  ts.tv_sec = time_t(t / 1000000000u);
  ts.tv_nsec = long(t % 1000000000u);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}
#endif

void WaitUntilNs(uint64_t t) {
#if defined(__APPLE__)
  // The same absolute deadline as SleepUntilNs, without touching this
  // thread's scheduling policy: a thread that made itself real-time
  // (SetThreadRealTime) keeps that policy across the wait.
  while (MonotonicNs() < t) mach_wait_until(MachTicks(t));
#else
  LowTimerSlack();
  timespec ts;
  ts.tv_sec = time_t(t / 1000000000u);
  ts.tv_nsec = long(t % 1000000000u);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
#endif
}

bool SetThreadRealTime(uint64_t period_ns, uint64_t computation_ns, uint64_t constraint_ns) {
#if defined(__APPLE__)
  // What a Core Audio render thread runs under: a time-constraint policy,
  // so the scheduler wakes it on its period whatever else the machine
  // does (a timeshare thread the game's threads can starve).
  const uint64_t constraint = constraint_ns < period_ns ? constraint_ns : period_ns;
  const uint64_t computation = computation_ns < constraint ? computation_ns : constraint;
  const thread_act_t self = pthread_mach_thread_np(pthread_self());
  thread_time_constraint_policy_data_t rt = {};
  rt.period = uint32_t(MachTicks(period_ns));
  rt.constraint = uint32_t(MachTicks(constraint));
  rt.computation = uint32_t(MachTicks(computation));
  rt.preemptible = 1;
  return thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY,
                           reinterpret_cast<thread_policy_t>(&rt),
                           THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
  // Linux wants CAP_SYS_NICE for SCHED_FIFO: the thread keeps whatever
  // class it was given and the caller falls back to the plain wait.
  (void)period_ns;
  (void)computation_ns;
  (void)constraint_ns;
  return false;
#endif
}

void SetThreadPriorityClass(int p) {
#if defined(__APPLE__)
  qos_class_t q = p >= 2 ? QOS_CLASS_USER_INTERACTIVE
                  : p >= 0 ? QOS_CLASS_USER_INITIATED
                           : QOS_CLASS_UTILITY;
  pthread_set_qos_class_self_np(q, 0);
#else
  if (p < 0) setpriority(PRIO_PROCESS, 0, p <= -2 ? 10 : 5);  // this thread (Linux)
#endif
}

void SetThreadName(const char* name) {
#if defined(__APPLE__)
  pthread_setname_np(name);
#else
  pthread_setname_np(pthread_self(), name);
#endif
}

void SetThreadQos(ThreadQos q) {
#if defined(__APPLE__)
  const qos_class_t c = q.cls == ThreadQos::kUserInteractive ? QOS_CLASS_USER_INTERACTIVE
                        : q.cls == ThreadQos::kUserInitiated ? QOS_CLASS_USER_INITIATED
                                                             : QOS_CLASS_UTILITY;
  const int rel = q.relative > 0 ? 0 : q.relative < QOS_MIN_RELATIVE_PRIORITY
                                           ? QOS_MIN_RELATIVE_PRIORITY
                                           : q.relative;
  pthread_set_qos_class_self_np(c, rel);
#else
  if (q.cls == ThreadQos::kUtility) setpriority(PRIO_PROCESS, 0, 5);  // this thread (Linux)
#endif
}

const char* ThreadQosName() {
#if defined(__APPLE__)
  qos_class_t c = QOS_CLASS_UNSPECIFIED;
  int rel = 0;
  pthread_get_qos_class_np(pthread_self(), &c, &rel);
  const char* name;
  switch (c) {
    case QOS_CLASS_USER_INTERACTIVE: name = "user-interactive"; break;
    case QOS_CLASS_USER_INITIATED: name = "user-initiated"; break;
    case QOS_CLASS_DEFAULT: name = "default"; break;
    case QOS_CLASS_UTILITY: name = "utility"; break;
    case QOS_CLASS_BACKGROUND: name = "background"; break;
    default: name = "unspecified"; break;
  }
  if (!rel) return name;
  static thread_local char buf[32];
  std::snprintf(buf, sizeof buf, "%s %d", name, rel);
  return buf;
#else
  return "n/a";
#endif
}

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
