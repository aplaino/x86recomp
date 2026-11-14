// guest_alloc_test: GuestAllocator reservations and decommits and
// GuestHeap sizes at the edges. Checks that:
//   - top-down reservations smaller than 64 KiB, one after another, each
//     get an aligned base of their own below the last;
//   - a decommit running past its reservation fails and leaves the next
//     one committed; size 0 decommits a whole reservation from its base
//     and fails anywhere else;
//   - heap sizes within 15 of 2^64 fail instead of rounding to 0.

#include <cstdio>

#include "runtime/guest_alloc.h"
#include "runtime/guest_memory.h"

using namespace recomp;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

}  // namespace

int main() {
  GuestMemory mem;
  if (!mem.Reserve()) return 1;
  constexpr uint64_t kG = GuestAllocator::kGranularity;
  constexpr uint64_t kLo = 0x10000, kHi = 0x80000000u;

  {
    GuestAllocator vm;
    vm.Init(&mem, kLo, kHi);
    uint64_t last = kHi;
    for (int i = 0; i < 4; i++) {
      const uint64_t a = vm.Reserve(0, 0x1000, true);
      CHECK(a && a % kG == 0 && a < last);
      last = a;
    }
    CHECK(last == kHi - 4 * kG);
    // A gap above an unaligned end: the base is aligned above that end.
    CHECK(vm.ReserveAt(kHi - 6 * kG + 0x3000, 0x1000));
    const uint64_t b = vm.Reserve(0, 0x1000, true);
    CHECK(b == kHi - 5 * kG);
    const uint64_t c = vm.Reserve(0, 0x1000, true);
    CHECK(c == kHi - 6 * kG);  // below it in the same 64 KiB
    // Filling down to lo_, then nothing is left.
    GuestAllocator small;
    small.Init(&mem, kLo, kLo + 2 * kG);
    CHECK(small.Reserve(0, 0x1000, true) == kLo + kG);
    CHECK(small.Reserve(0, 0x1000, true) == kLo);
    CHECK(small.Reserve(0, 0x1000, true) == 0);
  }

  {
    GuestAllocator vm;
    vm.Init(&mem, kLo, kHi);
    const uint64_t a = vm.Reserve(0, kG), b = vm.Reserve(0, kG);
    CHECK(b == a + kG);
    CHECK(vm.Commit(a, kG, Prot::RW) && vm.Commit(b, kG, Prot::RW));
    CHECK(!vm.Decommit(a, 2 * kG));
    CHECK(mem.IsCommitted(a) && mem.IsCommitted(b));
    CHECK(!vm.Decommit(a + 0x1000, 0));
    CHECK(mem.IsCommitted(a + 0x1000));
    CHECK(vm.Decommit(a + 0x1000, 0x1000));
    CHECK(mem.IsCommitted(a) && !mem.IsCommitted(a + 0x1000) && mem.IsCommitted(a + 0x2000));
    CHECK(vm.Decommit(a, 0));
    CHECK(!mem.IsCommitted(a) && !mem.IsCommitted(a + kG - 0x1000) && mem.IsCommitted(b));
    CHECK(!vm.Commit(b, UINT64_MAX, Prot::RW));
    CHECK(vm.Release(a) && vm.Release(b));
  }

  {
    GuestAllocator vm;
    vm.Init(&mem, kLo, kHi);
    GuestHeap heap(vm, 0, 0);
    for (uint64_t d = 0; d < 16; d++) CHECK(heap.Alloc(UINT64_MAX - d, false) == 0);
    const uint64_t p = heap.Alloc(32, false);
    CHECK(p != 0);
    CHECK(heap.ReAlloc(p, UINT64_MAX - 3, false, false) == 0);
    CHECK(heap.Size(p) == 32);
    const uint64_t q = heap.Alloc(32, false);
    CHECK(q != 0 && q != p);
    CHECK(heap.Free(p) && heap.Free(q));
  }

  if (g_failures) return 1;
  std::printf("guest allocator: ok\n");
  return 0;
}
