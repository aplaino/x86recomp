#include "a64/platform.h"

#include <sys/mman.h>
#include <unistd.h>

namespace recomp::a64::platform {

size_t PageSize() { return size_t(sysconf(_SC_PAGESIZE)); }

bool MapFixed(uint64_t addr, uint64_t size) {
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
  flags |= MAP_FIXED_NOREPLACE;
#endif
  // Without MAP_FIXED_NOREPLACE (macOS) the address is a hint the kernel
  // honours when the range is free; anything else is a collision.
  void* want = reinterpret_cast<void*>(addr);
  void* got = mmap(want, size, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (got == MAP_FAILED) return false;
  if (got != want) {
    munmap(got, size);
    return false;
  }
  return true;
}

bool ProtectReadOnly(uint64_t addr, uint64_t size) {
  return mprotect(reinterpret_cast<void*>(addr), size, PROT_READ) == 0;
}

}  // namespace recomp::a64::platform
