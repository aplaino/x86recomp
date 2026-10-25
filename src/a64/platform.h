#pragma once
// What POSIX does not cover the same way on Linux and macOS
// (platforms/horizon/ARCHITECTURE.md, "Platform layer"). The rest of the
// runtime has no OS #ifdefs.

#include <cstddef>
#include <cstdint>

namespace recomp::a64::platform {

size_t PageSize();
// Maps `size` zeroed read-write bytes at exactly `addr`. Fails, mapping
// nothing, if any part of the range is already in use.
bool MapFixed(uint64_t addr, uint64_t size);
bool ProtectReadOnly(uint64_t addr, uint64_t size);

}  // namespace recomp::a64::platform
