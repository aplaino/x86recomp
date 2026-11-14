// platform_fault_test: ReadFaultContext on real faults. A read of an
// inaccessible page and a write to a read-only one must each report
// their kind (write_known, write) and a pc; the handler then opens the
// page and the access is retried. Then RedirectToCall: a fault whose
// handler redirects to a function that throws must unwind through the
// faulting function into a handler around the call to it, again and again
// with the stack intact. (A handler around the faulting access in the
// same function is not reached: LLVM's call-site tables cover calls only,
// whatever -fnon-call-exceptions says.) Built like code that faults on
// guest memory (frame records, no red zone). Portable (every host the
// runtime supports has both).

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "runtime/platform.h"

namespace {

uint8_t* g_page;
size_t g_size;
recomp::platform::FaultContext g_fault;
int g_faults = 0;

void OnFault(int, siginfo_t* si, void* uc) {
  if (static_cast<uint8_t*>(si->si_addr) != g_page) _exit(2);
  g_fault = recomp::platform::ReadFaultContext(uc);
  g_faults++;
  mprotect(g_page, g_size, PROT_READ | PROT_WRITE);
}

struct Thrown {};

[[noreturn]] void Throw() { throw Thrown{}; }

void OnFaultRedirect(int, siginfo_t* si, void* uc) {
  if (size_t(static_cast<uint8_t*>(si->si_addr) - g_page) >= g_size) _exit(2);
  if (!recomp::platform::RedirectToCall(uc, Throw)) _exit(3);
}

__attribute__((noinline)) int Read(volatile uint8_t* p) { return p[1]; }

// Through a pointer, so the compiler cannot see that Read only loads (a
// function it knows cannot throw is called with no call-site entry).
int (*volatile g_read)(volatile uint8_t*) = Read;

__attribute__((noinline)) int ReadInCallee(volatile uint8_t* p) {
  try {
    return g_read(p);
  } catch (const Thrown&) {
    return -2;
  }
}

}  // namespace

int main() {
  g_size = size_t(sysconf(_SC_PAGESIZE));
  g_page = static_cast<uint8_t*>(
      mmap(nullptr, g_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (g_page == MAP_FAILED) return 1;
  struct sigaction sa{};
  sa.sa_sigaction = OnFault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  int failures = 0;

  volatile uint8_t* p = g_page;
  const uint8_t v = p[0];  // read of an inaccessible page
  const recomp::platform::FaultContext read = g_fault;
  mprotect(g_page, g_size, PROT_READ);
  p[0] = uint8_t(v + 7);  // write to a read-only page
  const recomp::platform::FaultContext write = g_fault;

  if (g_faults != 2 || p[0] != 7) {
    std::printf("FAIL: %d faults, value %u\n", g_faults, unsigned(p[0]));
    failures++;
  }
  if (!read.write_known || read.write || !read.pc) {
    std::printf("FAIL: the read: known %d, write %d, pc %p\n", read.write_known, read.write,
                reinterpret_cast<void*>(read.pc));
    failures++;
  }
  if (!write.write_known || !write.write || !write.pc) {
    std::printf("FAIL: the write: known %d, write %d, pc %p\n", write.write_known, write.write,
                reinterpret_cast<void*>(write.pc));
    failures++;
  }
  // RedirectToCall: the page inaccessible, the handler redirects.
  mprotect(g_page, g_size, PROT_NONE);
  sa.sa_sigaction = OnFaultRedirect;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  volatile char canary[64];
  std::memset(const_cast<char*>(canary), 0x5A, sizeof canary);
  int caught = 0;
  for (int i = 0; i < 100; i++) caught += ReadInCallee(g_page) == -2;
  bool intact = true;
  for (char ch : canary) intact = intact && ch == 0x5A;
  if (caught != 100 || !intact) {
    std::printf("FAIL: redirected faults caught %d times of 100, stack %s\n", caught,
                intact ? "intact" : "overwritten");
    failures++;
  }

  if (failures) return 1;
  std::printf("fault contexts: ok\n");
  return 0;
}
