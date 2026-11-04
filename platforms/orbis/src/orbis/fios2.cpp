// Fixes to libSceFios2.prx builds that games ship (lifter/middleware/*.manifest
// `host` lines): the build with SHA-256
// 8d52cf49b87b8127a071d8a66f12af7e49277bcde6c8c87d4a0a59aab8211f32 loses
// scheduler kicks. Each function here stands in for one of the build's
// (offsets from the module's base, which the lifter passes) and calls the
// build's own functions as its lifted code would, with the original's
// return addresses.
//
// The kicks (0x2A150, 0x2A1D0, 0x2A240), made to count a kick even when
// the scheduler's queue is empty.
//
// A kick tells a scheduler that something its chunks wait for has changed
// (an I/O completed, a psarc buffer was released, the library resumed):
// under the scheduler's mutex it bumps the kick counter (+0x40F0), clears
// the busy hints (busySkip +0x40E8, the busy count +0x40F8) and wakes the
// scheduler's thread. The originals do all of that only when the queue
// (count +0x40D8) is not empty.
//
// But the queue is also empty while the only chunk is being executed:
// ExecuteChunk (0x2A3E0) takes it off the queue, notes the kick counter
// and drops the mutex while the chunk runs. If the chunk answers busy
// (its data not read yet, no free psarc buffer), ExecuteChunk queues it
// again and, when the kick counter has not moved, marks it skipped; a busy
// psarc buffer also sets the busy count, under which the scheduler skips
// the archive's chunks without asking again. A kick that came during the
// run, with the queue empty, changed nothing, so the chunk stays skipped
// and no later kick comes for what already happened: every chunk queued
// behind it on that scheduler waits for good. The same holds for a chunk
// AddToSchedule (0x29BA0) runs at once on the caller's thread.
//
// The window is in the library's own code; it shows when file reads
// complete almost at once, as host reads do (in one game about one start
// in 50, with four runs at once, stopped for good while loading). Counting
// every kick closes it; the thread is still only woken for a non-empty
// queue (with an empty one it is either running the chunk or has nothing
// to do).

#include <cstdint>

#define RECOMP_LIFT_X64 1
#include "x86/lifted.h"

namespace {

using namespace ::recomp::lift;
using ::recomp::Ctx;
using ::recomp::g_guest_base;

// The build's functions (offsets).
constexpr uint64_t kSignal = 0x2E290;  // the library's condition signal
constexpr uint64_t kLock = 0x2E610, kUnlock = 0x2E6B0;  // mutex
constexpr uint64_t kSchedulers = 0x64808;  // list head, next at +8
constexpr uint64_t kDefault = 0x4CEE0;     // the default scheduler
// Scheduler fields.
constexpr uint64_t kMutex = 0x10, kCond = 0x28, kCount = 0x40D8, kBusySkip = 0x40E8,
                   kKicks = 0x40F0, kBusy = 0x40F8, kShutdown = 0x4115;

// A call as lifted code makes it, returning to the original's call site.
void Call(Ctx& c, uint64_t f, uint64_t arg, uint64_t ret) {
  c.rdi = arg;
  c.rsp -= 8;
  Wr<uint64_t>(g_guest_base, c.rsp, ret);
  ::recomp::Dispatch(c, f, ret);
}

// The original's body for one scheduler, with its mutex held; the call
// sites' return addresses are the original's.
void Kick(Ctx& c, uint64_t m, uint64_t s, uint64_t signal_ret) {
  uint8_t* const B = g_guest_base;
  Wr<uint32_t>(B, s + kKicks, Rd<uint32_t>(B, s + kKicks) + 1);
  Wr<uint64_t>(B, s + kBusySkip, 0);
  Wr<uint64_t>(B, s + kBusy, 0);
  if (Rd<uint64_t>(B, s + kCount) != 0) Call(c, m + kSignal, s + kCond, m + signal_ret);
}

// Every scheduler except `except` and, if skip_default, the default one.
void KickAll(Ctx& c, uint64_t m, uint64_t except, bool skip_default, uint64_t lock_ret,
             uint64_t signal_ret, uint64_t unlock_ret) {
  uint8_t* const B = g_guest_base;
  for (uint64_t s = Rd<uint64_t>(B, m + kSchedulers); s; s = Rd<uint64_t>(B, s + 8)) {
    if (s == except || (skip_default && s == m + kDefault) || Rd<uint8_t>(B, s + kShutdown)) {
      continue;
    }
    Call(c, m + kLock, s + kMutex, m + lock_ret);
    Kick(c, m, s, signal_ret);
    Call(c, m + kUnlock, s + kMutex, m + unlock_ret);
  }
  c.rsp += 8;  // ret
}

}  // namespace

// Kicks every scheduler (sceFiosResume and others).
extern "C" void recomp_orbis_fios2_kick_all(Ctx& c, uint64_t m) {
  KickAll(c, m, 0, false, 0x2A18E, 0x2A1BD, 0x2A1C5);
}

// Kicks the scheduler in rdi (I/O completions).
extern "C" void recomp_orbis_fios2_kick(Ctx& c, uint64_t m) {
  uint8_t* const B = g_guest_base;
  const uint64_t s = c.rdi;
  if (Rd<uint8_t>(B, s + kShutdown)) {
    c.rsp += 8;  // ret
    return;
  }
  Call(c, m + kLock, s + kMutex, m + 0x2A1F4);
  Kick(c, m, s, 0x2A226);
  c.rdi = s + kMutex;
  ::recomp::Dispatch(c, m + kUnlock, m + 0x2A1D0);  // the original's tail jump
}

// Kicks every scheduler but the one in rdi and the default one (psarc
// buffer releases).
extern "C" void recomp_orbis_fios2_kick_others(Ctx& c, uint64_t m) {
  KickAll(c, m, c.rdi, true, 0x2A28F, 0x2A2BE, 0x2A2C6);
}
