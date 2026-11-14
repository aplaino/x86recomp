// libkernel HLE: semaphores and event flags (sceKernel*Sema,
// sceKernel*EventFlag). Handles as for the pthread objects (kernel.h).
// Timeouts are microseconds, passed by pointer: null waits for ever, and
// the time left is written back. Waiters are served first come, first
// served, whichever order (FIFO or priority) the attributes ask for.
//
// A call holds the object it found until it returns, so deleting one
// under its waiters frees nothing they use; they wake and return ESRCH,
// as for a handle no longer there, the way event-queue waiters return
// EBADF (what the kernel answers waiters of a deleted object is not
// measured).

#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "x86/runtime.h"

namespace recomp::orbis::kernel {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kECANCELED = 85;  // FreeBSD

struct Sema {
  std::mutex mu;
  std::condition_variable cv;
  int count = 0, max = 0;
  std::deque<uint64_t> queue;  // waiter tickets, first served first
  uint64_t next_ticket = 0;
  uint64_t cancel_gen = 0;
  bool deleted = false;
};

// Event flags: a 64-bit pattern. Waits are AND (all bits) or OR (any),
// optionally clearing all or the waited bits once satisfied. A
// single-waiter flag refuses a second waiter.
constexpr uint32_t kAttrMulti = 0x20;
constexpr uint32_t kWaitAnd = 1, kWaitOr = 2, kClearAll = 0x10, kClearPat = 0x20;

struct EventFlag {
  std::mutex mu;
  std::condition_variable cv;
  uint64_t bits = 0;
  bool multi = false;
  int waiters = 0;
  uint64_t cancel_gen = 0;
  bool deleted = false;
};

// Never destroyed: threads may still wait at exit.
std::mutex& g_mu = *new std::mutex;
std::map<gaddr, std::shared_ptr<Sema>>& g_semas = *new std::map<gaddr, std::shared_ptr<Sema>>;
std::map<gaddr, std::shared_ptr<EventFlag>>& g_flags =
    *new std::map<gaddr, std::shared_ptr<EventFlag>>;

template <typename T>
std::shared_ptr<T> Get(std::map<gaddr, std::shared_ptr<T>>& m, gaddr h) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = m.find(h);
  return it == m.end() ? nullptr : it->second;
}

// Takes h's object out of m and wakes its waiters; false if there is none.
template <typename T>
bool Delete(std::map<gaddr, std::shared_ptr<T>>& m, gaddr h) {
  std::shared_ptr<T> o;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = m.find(h);
    if (it == m.end()) return false;
    o = std::move(it->second);
    m.erase(it);
    FreeHandle(h);
  }
  std::lock_guard<std::mutex> l(o->mu);
  o->deleted = true;
  o->cv.notify_all();
  return true;
}

// Waits on cv (holding l) until pred, for the time in *timeout_us (null:
// for ever), writing back what is left. False on timeout.
template <typename Pred>
bool WaitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& l, gaddr timeout_us,
             Pred pred) {
  if (!timeout_us) {
    cv.wait(l, pred);
    return true;
  }
  const auto deadline = Clock::now() + std::chrono::microseconds(Rd32(timeout_us));
  const bool ok = cv.wait_until(l, deadline, pred);
  const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
  Wr32(timeout_us, ok && left.count() > 0 ? uint32_t(left.count()) : 0);
  return ok;
}

bool Satisfied(uint64_t bits, uint64_t pattern, uint32_t mode) {
  return mode & kWaitAnd ? (bits & pattern) == pattern : (bits & pattern) != 0;
}

}  // namespace
}  // namespace recomp::orbis::kernel

using namespace recomp::orbis::kernel;
using recomp::hle::Arg;

// int sceKernelCreateSema(SceKernelSema* out, const char* name, uint32_t
// attr, int init, int max, const SceKernelSemaOptParam* opt)
ORBIS_HLE("libkernel", sceKernelCreateSema, 6) {
  const int init = int(Arg(c, 3)), max = int(Arg(c, 4));
  if (max <= 0 || init < 0 || init > max) return SceError(kEINVAL);
  auto s = std::make_shared<Sema>();
  s->count = init;
  s->max = max;
  const recomp::gaddr h = NewHandle();
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_semas[h] = std::move(s);
  }
  recomp::Wr64(Arg(c, 0), h);
  return 0;
}

ORBIS_HLE("libkernel", sceKernelDeleteSema, 1) {
  return Delete(g_semas, Arg(c, 0)) ? 0 : SceError(kESRCH);
}

// int sceKernelWaitSema(SceKernelSema sem, int need, SceKernelUseconds*
// timeout)
ORBIS_HLE("libkernel", sceKernelWaitSema, 3) {
  recomp::GuestSyncPoint sync;
  const std::shared_ptr<Sema> s = Get(g_semas, Arg(c, 0));
  const int need = int(Arg(c, 1));
  if (!s) return SceError(kESRCH);
  if (need <= 0 || need > s->max) return SceError(kEINVAL);
  std::unique_lock<std::mutex> l(s->mu);
  const uint64_t ticket = s->next_ticket++, gen = s->cancel_gen;
  s->queue.push_back(ticket);
  const bool ok = WaitFor(s->cv, l, Arg(c, 2), [&] {
    return s->deleted || s->cancel_gen != gen || (s->queue.front() == ticket && s->count >= need);
  });
  s->queue.erase(std::find(s->queue.begin(), s->queue.end(), ticket));
  s->cv.notify_all();  // the next in line may be satisfied too
  if (s->deleted) return SceError(kESRCH);
  if (s->cancel_gen != gen) return SceError(kECANCELED);
  if (!ok) return SceError(kETIMEDOUT);
  s->count -= need;
  return 0;
}

ORBIS_HLE("libkernel", sceKernelPollSema, 2) {
  recomp::GuestSyncPoint sync;
  const std::shared_ptr<Sema> s = Get(g_semas, Arg(c, 0));
  const int need = int(Arg(c, 1));
  if (!s) return SceError(kESRCH);
  if (need <= 0 || need > s->max) return SceError(kEINVAL);
  std::lock_guard<std::mutex> l(s->mu);
  if (!s->queue.empty() || s->count < need) return SceError(kEBUSY);
  s->count -= need;
  return 0;
}

ORBIS_HLE("libkernel", sceKernelSignalSema, 2) {
  recomp::GuestSyncPoint sync;
  const std::shared_ptr<Sema> s = Get(g_semas, Arg(c, 0));
  const int n = int(Arg(c, 1));
  if (!s) return SceError(kESRCH);
  std::lock_guard<std::mutex> l(s->mu);
  if (n <= 0 || s->count + n > s->max) return SceError(kEINVAL);
  s->count += n;
  s->cv.notify_all();
  return 0;
}

// int sceKernelCancelSema(SceKernelSema sem, int count, int* waiters):
// every waiter returns ECANCELED; count < 0 keeps the count.
ORBIS_HLE("libkernel", sceKernelCancelSema, 3) {
  recomp::GuestSyncPoint sync;
  const std::shared_ptr<Sema> s = Get(g_semas, Arg(c, 0));
  if (!s) return SceError(kESRCH);
  std::lock_guard<std::mutex> l(s->mu);
  if (int(Arg(c, 1)) > s->max) return SceError(kEINVAL);
  if (Arg(c, 2)) recomp::Wr32(Arg(c, 2), uint32_t(s->queue.size()));
  if (int(Arg(c, 1)) >= 0) s->count = int(Arg(c, 1));
  s->cancel_gen++;
  s->cv.notify_all();
  return 0;
}

// int sceKernelCreateEventFlag(SceKernelEventFlag* out, const char* name,
// uint32_t attr, uint64_t init, const SceKernelEventFlagOptParam* opt)
ORBIS_HLE("libkernel", sceKernelCreateEventFlag, 5) {
  auto f = std::make_shared<EventFlag>();
  f->bits = Arg(c, 3);
  f->multi = uint32_t(Arg(c, 2)) & kAttrMulti;
  const recomp::gaddr h = NewHandle();
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_flags[h] = std::move(f);
  }
  recomp::Wr64(Arg(c, 0), h);
  return 0;
}

ORBIS_HLE("libkernel", sceKernelDeleteEventFlag, 1) {
  return Delete(g_flags, Arg(c, 0)) ? 0 : SceError(kESRCH);
}

ORBIS_HLE("libkernel", sceKernelSetEventFlag, 2) {
  recomp::GuestSyncPoint sync;
  const std::shared_ptr<EventFlag> f = Get(g_flags, Arg(c, 0));
  if (!f) return SceError(kESRCH);
  std::lock_guard<std::mutex> l(f->mu);
  f->bits |= Arg(c, 1);
  f->cv.notify_all();
  return 0;
}

// Keeps the bits set in the pattern.
ORBIS_HLE("libkernel", sceKernelClearEventFlag, 2) {
  recomp::GuestSyncPoint sync;
  const std::shared_ptr<EventFlag> f = Get(g_flags, Arg(c, 0));
  if (!f) return SceError(kESRCH);
  std::lock_guard<std::mutex> l(f->mu);
  f->bits &= Arg(c, 1);
  return 0;
}

// int sceKernelWaitEventFlag(SceKernelEventFlag ef, uint64_t pattern,
// uint32_t mode, uint64_t* result, SceKernelUseconds* timeout)
ORBIS_HLE("libkernel", sceKernelWaitEventFlag, 5) {
  recomp::GuestSyncPoint sync;
  const std::shared_ptr<EventFlag> f = Get(g_flags, Arg(c, 0));
  const uint64_t pattern = Arg(c, 1);
  const uint32_t mode = uint32_t(Arg(c, 2));
  if (!f) return SceError(kESRCH);
  if (!pattern || !(mode & (kWaitAnd | kWaitOr)) || (mode & kWaitAnd && mode & kWaitOr))
    return SceError(kEINVAL);
  std::unique_lock<std::mutex> l(f->mu);
  if (!f->multi && f->waiters) return SceError(kEPERM);
  const uint64_t gen = f->cancel_gen;
  f->waiters++;
  const bool ok = WaitFor(f->cv, l, Arg(c, 4), [&] {
    return f->deleted || f->cancel_gen != gen || Satisfied(f->bits, pattern, mode);
  });
  f->waiters--;
  if (f->deleted) return SceError(kESRCH);
  if (Arg(c, 3)) recomp::Wr64(Arg(c, 3), f->bits);
  if (f->cancel_gen != gen) return SceError(kECANCELED);
  if (!ok) return SceError(kETIMEDOUT);
  if (mode & kClearAll)
    f->bits = 0;
  else if (mode & kClearPat)
    f->bits &= ~pattern;
  return 0;
}

ORBIS_HLE("libkernel", sceKernelPollEventFlag, 4) {
  recomp::GuestSyncPoint sync;
  const std::shared_ptr<EventFlag> f = Get(g_flags, Arg(c, 0));
  const uint64_t pattern = Arg(c, 1);
  const uint32_t mode = uint32_t(Arg(c, 2));
  if (!f) return SceError(kESRCH);
  if (!pattern || !(mode & (kWaitAnd | kWaitOr))) return SceError(kEINVAL);
  std::lock_guard<std::mutex> l(f->mu);
  if (Arg(c, 3)) recomp::Wr64(Arg(c, 3), f->bits);
  if (!Satisfied(f->bits, pattern, mode)) return SceError(kEBUSY);
  if (mode & kClearAll)
    f->bits = 0;
  else if (mode & kClearPat)
    f->bits &= ~pattern;
  return 0;
}

// int sceKernelCancelEventFlag(SceKernelEventFlag ef, uint64_t pattern,
// int* waiters): every waiter returns ECANCELED; the bits become pattern.
ORBIS_HLE("libkernel", sceKernelCancelEventFlag, 3) {
  recomp::GuestSyncPoint sync;
  const std::shared_ptr<EventFlag> f = Get(g_flags, Arg(c, 0));
  if (!f) return SceError(kESRCH);
  std::lock_guard<std::mutex> l(f->mu);
  if (Arg(c, 2)) recomp::Wr32(Arg(c, 2), uint32_t(f->waiters));
  f->bits = Arg(c, 1);
  f->cancel_gen++;
  f->cv.notify_all();
  return 0;
}
