// libkernel HLE: clocks and sleeping, on the host's clocks. FreeBSD's
// struct timespec and timeval are two 64-bit fields.

#include <chrono>
#include <ctime>
#include <thread>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/lifted.h"
#include "runtime/runtime.h"

namespace recomp::orbis::kernel {
namespace {

using namespace std::chrono;

const steady_clock::time_point kStart = steady_clock::now();

// FreeBSD's clock ids (sys/time.h).
enum : int {
  kRealtime = 0, kVirtual = 1, kProf = 2, kMonotonic = 4, kUptime = 5,
  kUptimePrecise = 7, kUptimeFast = 8, kRealtimePrecise = 9, kRealtimeFast = 10,
  kMonotonicPrecise = 11, kMonotonicFast = 12, kSecond = 13, kThreadCpu = 14,
  kProcessCpu = 15,
};

// Nanoseconds on the clock, or false for an unknown one.
bool ClockNs(int id, int64_t* ns) {
  timespec ts;
  switch (id) {
    case kRealtime: case kRealtimePrecise: case kRealtimeFast: case kSecond:
      clock_gettime(CLOCK_REALTIME, &ts);
      break;
    case kMonotonic: case kMonotonicPrecise: case kMonotonicFast:
    case kUptime: case kUptimePrecise: case kUptimeFast:
      clock_gettime(CLOCK_MONOTONIC, &ts);
      break;
    case kVirtual: case kProf: case kProcessCpu:
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
      break;
    case kThreadCpu:
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
      break;
    default:
      return false;
  }
  *ns = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
  if (id == kSecond) *ns -= *ns % 1000000000;
  return true;
}

void PutTimespec(gaddr p, int64_t ns) {
  Wr64(p, uint64_t(ns / 1000000000));
  Wr64(p + 8, uint64_t(ns % 1000000000));
}

uint64_t ProcessUs() {
  return uint64_t(duration_cast<microseconds>(steady_clock::now() - kStart).count());
}

void Sleep(int64_t us) {
  if (us > 0) std::this_thread::sleep_for(microseconds(us));
}

int64_t RealtimeUs() {
  return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace
}  // namespace recomp::orbis::kernel

using namespace recomp::orbis::kernel;
using recomp::hle::Arg;

ORBIS_HLE("libkernel", sceKernelUsleep, 1) {
  Sleep(int64_t(uint32_t(Arg(c, 0))));
  return 0;
}

ORBIS_HLE("libkernel", usleep, 1) {
  Sleep(int64_t(uint32_t(Arg(c, 0))));
  return 0;
}

// int nanosleep(const struct timespec* req, struct timespec* rem)
ORBIS_HLE("libkernel", nanosleep, 2) {
  const int64_t s = int64_t(recomp::Rd64(Arg(c, 0))), ns = int64_t(recomp::Rd64(Arg(c, 0) + 8));
  if (s < 0 || ns < 0 || ns >= 1000000000) {
    SetErrno(c, kEINVAL);
    return ~uint64_t(0);
  }
  std::this_thread::sleep_for(seconds(s) + nanoseconds(ns));
  if (Arg(c, 1)) PutTimespec(Arg(c, 1), 0);
  return 0;
}

static uint64_t ClockGettime(recomp::Ctx& c) {
  int64_t ns;
  if (!ClockNs(int(Arg(c, 0)), &ns)) {
    SetErrno(c, kEINVAL);
    return ~uint64_t(0);
  }
  PutTimespec(Arg(c, 1), ns);
  return 0;
}
ORBIS_HLE("libkernel", clock_gettime, 2) { return ClockGettime(c); }
ORBIS_HLE_NAMED("libScePosix", "clock_gettime", posix_clock_gettime, 2) { return ClockGettime(c); }

ORBIS_HLE("libkernel", sceKernelClockGettime, 2) {
  int64_t ns;
  if (!ClockNs(int(Arg(c, 0)), &ns)) return SceError(kEINVAL);
  PutTimespec(Arg(c, 1), ns);
  return 0;
}

// int gettimeofday(struct timeval* tv, struct timezone* tz): UTC, and a
// zero timezone (minutes west, DST type).
ORBIS_HLE("libkernel", gettimeofday, 2) {
  const int64_t us = RealtimeUs();
  if (Arg(c, 0)) {
    recomp::Wr64(Arg(c, 0), uint64_t(us / 1000000));
    recomp::Wr64(Arg(c, 0) + 8, uint64_t(us % 1000000));
  }
  if (Arg(c, 1)) recomp::Wr64(Arg(c, 1), 0);
  return 0;
}

ORBIS_HLE("libkernel", sceKernelGettimeofday, 1) {
  const int64_t us = RealtimeUs();
  recomp::Wr64(Arg(c, 0), uint64_t(us / 1000000));
  recomp::Wr64(Arg(c, 0) + 8, uint64_t(us % 1000000));
  return 0;
}

// int sceKernelGettimezone(struct timezone* tz): UTC, no DST.
ORBIS_HLE("libkernel", sceKernelGettimezone, 1) {
  recomp::Wr64(Arg(c, 0), 0);
  return 0;
}

// Microseconds since the process started.
ORBIS_HLE("libkernel", sceKernelGetProcessTime, 0) { return ProcessUs(); }

// A counter at sceKernelGetProcessTimeCounterFrequency (here 1 MHz).
ORBIS_HLE("libkernel", sceKernelGetProcessTimeCounter, 0) { return ProcessUs(); }
ORBIS_HLE("libkernel", sceKernelGetProcessTimeCounterFrequency, 0) { return 1000000; }

// The time stamp counter, as rdtsc reads it (runtime: Rdtsc), and its
// frequency.
ORBIS_HLE("libkernel", sceKernelReadTsc, 0) { return recomp::Rdtsc(c); }
ORBIS_HLE("libkernel", sceKernelGetTscFrequency, 0) { return recomp::kTscHz; }
