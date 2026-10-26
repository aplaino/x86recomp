// xam: its memory allocator (XamAlloc, XMemFree).

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/memory.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kEInvalidArg = 0x80070057, kEOutOfMemory = 0x8007000E;

}  // namespace

// XamAlloc(attributes, size, *address): zeroed memory from the kernel's
// heap. The attributes choose xam's heaps and physical memory; only 0 (the
// caller's default heap) is read yet.
HLE(kXam, 490, XamAlloc) {
  uint32_t attributes = Arg(c, 0), size = Arg(c, 1), out = Arg(c, 2);
  if (!out) return kEInvalidArg;
  if (attributes) ppc::Fatal(&c, "XamAlloc(%x, %u): allocation attributes are not read yet", attributes, size);
  uint32_t va = SystemAlloc(size);
  Wr32(out, va);
  return va ? 0 : kEOutOfMemory;
}

// XamFree (xam's XMemFree): heap memory below 0xA0000000, physical memory
// above, as xam tells them apart.
HLE(kXam, 492, XamFree) {
  uint32_t va = Arg(c, 0);
  if (!va) return 0;
  if (va < 0xA0000000)
    SystemFree(va);
  else
    Release(va);
  return 0;
}

}  // namespace recomp::xbox360
