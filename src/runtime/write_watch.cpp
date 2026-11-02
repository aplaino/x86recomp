#include "runtime/write_watch.h"

#include <sys/mman.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace recomp::watch {

struct Registry {
  std::mutex mu;
  std::vector<Watch*> armed;   // under mu
  std::atomic<size_t> count{0};  // armed.size(), read without the lock

  static void Protect(const Watch* w, int prot) {
    if (mprotect(w->mem_->base() + w->lo_, w->hi_ - w->lo_, prot)) {
      std::perror("watch mprotect");
      std::abort();
    }
  }

  // Takes armed[i] off the list; it reads as written from now on.
  Watch* Remove(size_t i) {
    Watch* w = armed[i];
    armed[i] = armed.back();
    armed[i]->slot_ = uint32_t(i);
    armed.pop_back();
    count.store(armed.size());
    w->armed_.store(false, std::memory_order_release);
    return w;
  }

  // Disarms the watches overlapping [lo, hi) and, in turn, those
  // overlapping theirs, and makes their pages writable; false if none
  // did. No allocation: this runs in the fault handler.
  bool DisarmOverlapping(gaddr lo, gaddr hi) {
    Watch* stack = nullptr;
    const auto take = [&](gaddr a, gaddr b) {
      for (size_t i = 0; i < armed.size();) {
        if (armed[i]->lo_ < b && armed[i]->hi_ > a) {
          Watch* w = Remove(i);
          w->next_ = stack;
          stack = w;
        } else {
          i++;
        }
      }
    };
    take(lo, hi);
    const bool any = stack != nullptr;
    while (stack) {
      Watch* w = stack;
      stack = w->next_;
      take(w->lo_, w->hi_);  // read as written before their pages open
      Protect(w, PROT_READ | PROT_WRITE);
    }
    return any;
  }
};

namespace {

Registry& R() {
  static Registry* r = new Registry;  // faults may come during exit
  return *r;
}

thread_local bool t_held = false;

// Holds the registry lock. A fault on a watched page while this thread
// holds it is not handled (it would deadlock); nothing writes guest memory
// with it held.
struct Guard {
  Guard() {
    R().mu.lock();
    t_held = true;
  }
  ~Guard() {
    t_held = false;
    R().mu.unlock();
  }
};

}  // namespace

void Watch::Arm(GuestMemory& mem, gaddr va, uint64_t size) {
  Guard g;
  if (armed_.load()) R().DisarmOverlapping(lo_, hi_);
  const uint64_t mask = mem.host_page_size() - 1;
  mem_ = &mem;
  lo_ = va & ~mask;
  hi_ = (va + size + mask) & ~mask;
  if (hi_ <= lo_) return;
  slot_ = uint32_t(R().armed.size());
  R().armed.push_back(this);
  R().count.store(R().armed.size());
  armed_.store(true, std::memory_order_release);
  Registry::Protect(this, PROT_READ);
}

void Watch::Disarm() {
  if (!armed_.load()) return;
  Guard g;
  if (armed_.load()) R().DisarmOverlapping(lo_, hi_);
}

bool HandleFault(gaddr addr) {
  if (!R().count.load() || t_held) return false;
  Guard g;
  return R().DisarmOverlapping(addr, addr + 1);
}

void Release(gaddr va, uint64_t size) {
  if (!R().count.load() || !size) return;
  Guard g;
  R().DisarmOverlapping(va, va + size);
}

}  // namespace recomp::watch
