#pragma once
// Write watches: whether guest memory was written since a point, without
// comparing it.
//
// An armed watch keeps the host pages of its range read-only. The first
// write to any of them, by any thread and from guest or host code, faults;
// the runtime's fault handler (dispatch.cpp) calls HandleFault, which
// disarms every watch on that page (and those overlapping them, so that
// read-only pages are always exactly the armed watches' pages), and the
// write is retried. Watched pages read as usual.
//
// GuestMemory releases the watches on a range before it changes the
// range's host protection (Commit, Decommit). Host code that hands a
// guest buffer to the kernel to fill (read(2)) must Release it first: the
// kernel returns EFAULT on a read-only page instead of faulting.

#include <atomic>
#include <cstdint>

#include "runtime/guest_memory.h"

namespace recomp::watch {

class Watch {
 public:
  Watch() = default;
  ~Watch() { Disarm(); }
  Watch(const Watch&) = delete;
  Watch& operator=(const Watch&) = delete;

  // Arms the watch over [va, va + size) (committed memory), rounded out to
  // host pages. Armed already, it is disarmed first.
  void Arm(GuestMemory& mem, gaddr va, uint64_t size);
  void Disarm();
  // Armed, and nothing written in its pages since.
  bool clean() const { return armed_.load(std::memory_order_acquire); }

 private:
  friend struct Registry;
  std::atomic<bool> armed_{false};
  GuestMemory* mem_ = nullptr;
  gaddr lo_ = 0, hi_ = 0;  // host pages
  uint32_t slot_ = 0;      // index in the registry while armed
  Watch* next_ = nullptr;  // the registry's list of those being disarmed
};

// Fault handler entry: true if `addr` is on a watched page, which is now
// writable (retry the access).
bool HandleFault(gaddr addr);

// Disarms the watches overlapping [va, va + size).
void Release(gaddr va, uint64_t size);

}  // namespace recomp::watch
