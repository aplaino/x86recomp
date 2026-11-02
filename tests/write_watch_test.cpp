// write_watch_test: write watches over guest memory, with the runtime's
// fault path reduced to its watch part (a SIGSEGV/SIGBUS handler that
// calls watch::HandleFault and retries). Checks that:
//   - an armed watch stays clean while its pages are read, and the first
//     write to any of them disarms it and lands;
//   - watches overlapping a disarmed one are disarmed with it, the others
//     are not, and no page is left read-only without an armed watch;
//   - Release makes a range usable by the kernel (read(2) into it);
//   - Commit and Decommit over a watched range disarm it;
//   - a watch destroyed while armed leaves its pages writable;
//   - threads writing while another arms and checks never lose a write.

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "runtime/guest_alloc.h"
#include "runtime/guest_memory.h"
#include "runtime/write_watch.h"

using namespace recomp;

namespace {

GuestMemory g_mem;
GuestAllocator g_vm;
std::atomic<int> g_failures{0};

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

void OnFault(int, siginfo_t* si, void*) {
  uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_mem.base());
  if (a - base < g_mem.size() && watch::HandleFault(a - base)) return;
  const char m[] = "unexpected fault\n";
  (void)!write(2, m, sizeof m - 1);
  _exit(2);
}

// The volatile stores keep the compiler from merging or dropping them.
void Store(gaddr va, uint8_t v) { *reinterpret_cast<volatile uint8_t*>(g_mem.ptr(va)) = v; }
uint8_t Load(gaddr va) { return *reinterpret_cast<volatile uint8_t*>(g_mem.ptr(va)); }

}  // namespace

int main() {
  if (!g_mem.Reserve()) return 1;
  g_vm.Init(&g_mem, 0x10000, 0x80000000u);
  struct sigaction sa{};
  sa.sa_sigaction = OnFault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);

  const uint64_t hp = g_mem.host_page_size();
  const uint64_t size = 16 * hp;
  const gaddr va = g_vm.Reserve(0, uint32_t(size));
  g_vm.Commit(va, uint32_t(size), Prot::RW);
  const auto page = [&](int i) { return va + uint64_t(i) * hp; };

  {
    watch::Watch w;
    w.Arm(g_mem, page(0) + 5, hp);  // pages 0 and 1
    CHECK(w.clean());
    CHECK(Load(page(0)) == 0 && Load(page(1) + 7) == 0);
    CHECK(w.clean());
    Store(page(2), 9);  // outside
    CHECK(w.clean());
    Store(page(1) + 3, 42);
    CHECK(!w.clean());
    CHECK(Load(page(1) + 3) == 42);
    Store(page(0), 1);  // writable again
    w.Arm(g_mem, page(0), 1);
    CHECK(w.clean());
    Store(page(0) + hp - 1, 2);
    CHECK(!w.clean());
  }

  {
    // a: pages 3-4, b: 4-5, c: 7. A write to page 3 disarms a and b (they
    // share page 4); page 5 (only b's) is writable without another fault.
    watch::Watch a, b, c;
    a.Arm(g_mem, page(3), 2 * hp);
    b.Arm(g_mem, page(4), 2 * hp);
    c.Arm(g_mem, page(7), hp);
    Store(page(3), 1);
    CHECK(!a.clean() && !b.clean() && c.clean());
    Store(page(5), 1);
    CHECK(c.clean());
    a.Arm(g_mem, page(3), hp);
    b.Arm(g_mem, page(5), hp);
    Store(page(5), 2);
    CHECK(a.clean() && !b.clean() && c.clean());
  }

  {
    // The kernel does not fault: Release first.
    watch::Watch w;
    w.Arm(g_mem, page(8), hp);
    int fds[2];
    CHECK(pipe(fds) == 0);
    const char msg[] = "watched";
    CHECK(write(fds[1], msg, sizeof msg) == ssize_t(sizeof msg));
    watch::Release(page(8) + 10, sizeof msg);
    CHECK(!w.clean());
    CHECK(read(fds[0], g_mem.ptr(page(8) + 10), sizeof msg) == ssize_t(sizeof msg));
    CHECK(std::memcmp(g_mem.ptr(page(8) + 10), msg, sizeof msg) == 0);
    close(fds[0]);
    close(fds[1]);
  }

  {
    watch::Watch w;
    w.Arm(g_mem, page(9), hp);
    g_mem.Commit(page(9), hp, Prot::R);  // the guest's mprotect
    CHECK(!w.clean());
    Store(page(9), 3);
    w.Arm(g_mem, page(9), hp);
    g_mem.Decommit(page(9), hp);
    CHECK(!w.clean());
    g_mem.Commit(page(9), hp, Prot::RW);
    CHECK(Load(page(9)) == 0);
    Store(page(9), 4);
  }

  {
    auto w = std::make_unique<watch::Watch>();
    w->Arm(g_mem, page(10), hp);
    w.reset();
    Store(page(10), 5);  // no watch, no fault
  }

  {
    // Writers bump a counter in each of their pages; the checker arms a
    // watch per page, then reads the counter: while the watch stays clean
    // the counter must not have moved.
    constexpr int kThreads = 4, kRounds = 2000;
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; t++)
      writers.emplace_back([&, t] {
        auto* p = reinterpret_cast<std::atomic<uint32_t>*>(g_mem.ptr(page(11 + t)));
        while (!stop.load()) p->fetch_add(1);
      });
    std::vector<std::unique_ptr<watch::Watch>> ws;
    for (int t = 0; t < kThreads; t++) ws.push_back(std::make_unique<watch::Watch>());
    for (int r = 0; r < kRounds; r++)
      for (int t = 0; t < kThreads; t++) {
        const gaddr at = page(11 + t);
        ws[size_t(t)]->Arm(g_mem, at, hp);
        const uint32_t seen = *reinterpret_cast<volatile uint32_t*>(g_mem.ptr(at));
        if (ws[size_t(t)]->clean())
          CHECK(seen == *reinterpret_cast<volatile uint32_t*>(g_mem.ptr(at)) || !ws[size_t(t)]->clean());
      }
    stop = true;
    for (auto& t : writers) t.join();
  }

  if (g_failures) return 1;
  std::printf("write watches: ok\n");
  return 0;
}
