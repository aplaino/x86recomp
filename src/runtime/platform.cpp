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
#include <signal.h>
#include <time.h>
#include <ucontext.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
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
  while (MonotonicNs() < t) {
    const unsigned __int128 ticks = (unsigned __int128)t * tb.denom / tb.numer;
    mach_wait_until(uint64_t(ticks));
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
