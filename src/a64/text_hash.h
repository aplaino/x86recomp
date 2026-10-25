#pragma once
// FNV-1a over a module's code: identifies the code a lifted build was made
// from (kLiftedTextHash), so the runtime refuses to run it against a
// different module. Shared by recomp_lift_a64 and the platform loaders.

#include <cstddef>
#include <cstdint>

namespace recomp::a64 {

inline uint64_t TextHash(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

}  // namespace recomp::a64
