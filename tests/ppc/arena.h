#pragma once
// The tests' guest arena: 4 GiB reserved and committed on demand, guest
// address a at the returned pointer + a, as the platform's runtime lays
// it out.
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

inline uint8_t* MapArena() {
  void* p = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                 -1, 0);
  if (p == MAP_FAILED) {
    std::perror("mmap of the 4 GiB guest arena");
    std::exit(2);
  }
  return static_cast<uint8_t*>(p);
}
