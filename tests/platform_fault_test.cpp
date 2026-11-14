// platform_fault_test: ReadFaultContext on real faults. A read of an
// inaccessible page and a write to a read-only one must each report
// their kind (write_known, write) and a pc; the handler then opens the
// page and the access is retried. Portable (every host the runtime
// supports has the context).

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>

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
  if (failures) return 1;
  std::printf("fault contexts: ok\n");
  return 0;
}
