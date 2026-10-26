// xboxkrnl: the console's settings.

#include <cstring>

#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";

}  // namespace

// ExGetXConfigSetting(category, setting, buffer, buffer size, *required
// size): the setting's bytes and (a 16-bit count) their size; without a
// buffer only the size. An NTSTATUS (xam's callers test it for < 0); the
// statuses of failures are Xenia's.
HLE(kKrnl, 16, ExGetXConfigSetting) {
  uint16_t category = uint16_t(Arg(c, 0)), setting = uint16_t(Arg(c, 1)), buffer_size = uint16_t(Arg(c, 3));
  uint32_t buffer = Arg(c, 2), required = Arg(c, 4);
  const uint8_t* data;
  uint16_t size;
  if (!XConfigSetting(category, setting, data, size))
    ppc::Fatal(&c, "ExGetXConfigSetting(%u, %u): no such setting here", category, setting);
  if (!buffer && buffer_size) return kStatusInvalidParameter3;
  if (buffer) {
    if (buffer_size < size) return kStatusBufferTooSmall;
    std::memcpy(Mem() + buffer, data, size);
  }
  if (required) Wr16(required, size);
  return kStatusSuccess;
}

}  // namespace recomp::xbox360
