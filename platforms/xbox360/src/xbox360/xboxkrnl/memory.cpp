// xboxkrnl: virtual and physical memory, the kernel pool.

#include <algorithm>
#include <cstring>

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/memory.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";

// The protection without the page size and caching flags it may carry.
bool ValidProtect(uint32_t protect) {
  uint32_t access = protect & 0xFF;
  return access && !(access & (access - 1));
}

}  // namespace

// NtAllocateVirtualMemory(*base, *size, type, protect, debug): reserve
// and/or commit; MEM_LARGE_PAGES (or 16 MiB pages) allocates 64 KiB pages
// from 0x40000000.
HLE(kKrnl, 204, NtAllocateVirtualMemory) {
  uint32_t base_ptr = Arg(c, 0), size_ptr = Arg(c, 1), type = Arg(c, 2), protect = Arg(c, 3);
  uint32_t base = Rd32(base_ptr), size = Rd32(size_ptr);
  if (!size || !(type & (kMemCommit | kMemReserve))) return kStatusInvalidParameter;
  if (!ValidProtect(protect)) return kStatusInvalidPageProtection;
  Heap h = type & (kMemLargePages | kMem16MbPages) ? Heap::kVirtual64k : Heap::kVirtual4k;
  if (base && !HeapOf(base, h)) return kStatusInvalidParameter;
  uint32_t page = HeapPageSize(h);
  if (type & kMemReserve) {
    uint32_t end = (base + size + page - 1) & ~(page - 1);
    base &= ~(page - 1);
    size = end - base;
    bool fixed = base != 0;
    base = Reserve(h, base, size, page, type & kMemTopDown);
    if (!base) return fixed ? kStatusConflictingAddresses : kStatusNoMemory;
  } else {
    uint32_t end = (base + size + 0xFFF) & ~0xFFFu;
    base &= ~0xFFFu;
    size = end - base;
  }
  if ((type & kMemCommit) && !Commit(base, size, protect)) {
    if (type & kMemReserve) Release(base);
    return kStatusMemoryNotAllocated;
  }
  Wr32(base_ptr, base);
  Wr32(size_ptr, size);
  return kStatusSuccess;
}

// NtFreeVirtualMemory(*base, *size, type, debug): MEM_RELEASE frees the
// reservation at *base (size 0), MEM_DECOMMIT [*base, *base + *size), or to
// the reservation's end for size 0.
HLE(kKrnl, 220, NtFreeVirtualMemory) {
  uint32_t base_ptr = Arg(c, 0), size_ptr = Arg(c, 1), type = Arg(c, 2);
  uint32_t base = Rd32(base_ptr), size = Rd32(size_ptr);
  if (type & kMemRelease) {
    if (size) return kStatusInvalidParameter;
    uint32_t released = Release(base);
    if (!released) return kStatusFreeVmNotAtBase;
    Wr32(size_ptr, released);
    return kStatusSuccess;
  }
  if (!(type & kMemDecommit)) return kStatusInvalidParameter;
  RegionInfo r;
  if (!Query(base, r) || r.state == kMemFree) return kStatusMemoryNotAllocated;
  if (!size) size = r.alloc_base + ReservationSize(r.alloc_base) - base;
  if (!Decommit(base, size)) return kStatusMemoryNotAllocated;
  Wr32(base_ptr, base & ~0xFFFu);
  Wr32(size_ptr, size);
  return kStatusSuccess;
}

// NtQueryVirtualMemory(base, *MEMORY_BASIC_INFORMATION, debug).
HLE(kKrnl, 238, NtQueryVirtualMemory) {
  uint32_t va = Arg(c, 0), info = Arg(c, 1);
  RegionInfo r;
  if (!Query(va, r)) return kStatusInvalidParameter;
  Wr32(info + 0x00, r.base);
  Wr32(info + 0x04, r.alloc_base);
  Wr32(info + 0x08, r.alloc_protect);
  Wr32(info + 0x0C, r.size);
  Wr32(info + 0x10, r.state);
  Wr32(info + 0x14, r.protect);
  Wr32(info + 0x18, r.state == kMemFree ? 0 : kMemPrivate);
  return kStatusSuccess;
}

// MmAllocatePhysicalMemoryEx(flags, size, protect, min, max, alignment):
// committed memory in the physical view the protection's page size picks
// (MEM_LARGE_PAGES: 64 KiB at 0xA0000000, MEM_16MB_PAGES: 0xC0000000,
// otherwise 4 KiB at 0xE0000000), its physical pages within [min, max]
// (max 0: anywhere). Returns the address, 0 if none is free.
HLE(kKrnl, 186, MmAllocatePhysicalMemoryEx) {
  uint32_t size = Arg(c, 1), protect = Arg(c, 2), lo = Arg(c, 3), hi = Arg(c, 4), align = Arg(c, 5);
  Heap h = protect & kMemLargePages ? Heap::kPhysical64k
           : protect & kMem16MbPages ? Heap::kPhysical16m
                                     : Heap::kPhysical4k;
  if (!hi) hi = kPhysicalSize - 1;
  uint32_t va = ReservePhysical(h, size, align, lo, hi, false);
  if (!va) return 0;
  Commit(va, size, protect & ~(kMemLargePages | kMem16MbPages));
  return va;
}

// MmFreePhysicalMemory(type, address).
HLE(kKrnl, 189, MmFreePhysicalMemory) {
  Release(Arg(c, 1));
  return 0;
}

HLE(kKrnl, 190, MmGetPhysicalAddress) {
  uint32_t pa;
  if (!PhysicalAddress(Arg(c, 0), pa))
    ppc::Fatal(&c, "MmGetPhysicalAddress(%x): not in a physical view (no physical page backs it here)",
               Arg(c, 0));
  return pa;
}

HLE(kKrnl, 196, MmQueryAddressProtect) { return PageProtect(Arg(c, 0)); }

// MmSetAddressProtect(base, size, protect).
HLE(kKrnl, 199, MmSetAddressProtect) {
  Protect(Arg(c, 0), Arg(c, 1), Arg(c, 2) & ~(kMemLargePages | kMem16MbPages), nullptr);
  return 0;
}

HLE(kKrnl, 197, MmQueryAllocationSize) {
  RegionInfo r;
  if (!Query(Arg(c, 0), r) || r.state == kMemFree) return 0;
  return ReservationSize(r.alloc_base);
}

// MmQueryStatistics(*statistics): MM_STATISTICS (0x68 bytes, its size
// first): total physical pages, the kernel's, then the title's and the
// system's sections (available pages, total and reserved virtual bytes,
// then pages by use), the highest physical page. A title reads the totals
// and the title's available pages and virtual bytes (GlobalMemoryStatus).
// The kernel's 0x100 pages and the title's 0x2FFE0000 virtual bytes are
// Xenia's; the pages by use are left 0. Console check.
HLE(kKrnl, 198, MmQueryStatistics) {
  constexpr uint32_t kSize = 0x68, kTotalPages = kPhysicalSize / 0x1000, kKernelPages = 0x100;
  uint32_t s = Arg(c, 0);
  if (!s) return kStatusInvalidParameter;
  if (Rd32(s) != kSize) return kStatusBufferTooSmall;
  MemoryUse u = QueryMemoryUse();
  std::memset(Mem() + s, 0, kSize);
  Wr32(s, kSize);
  Wr32(s + 0x4, kTotalPages);
  Wr32(s + 0x8, kKernelPages);
  Wr32(s + 0xC, kTotalPages - kKernelPages - std::min(u.pages_in_use, kTotalPages - kKernelPages));
  Wr32(s + 0x10, 0x2FFE0000);
  Wr32(s + 0x14, u.virtual_reserved);
  Wr32(s + 0x64, kTotalPages - 1);
  return kStatusSuccess;
}

// The kernel pool (ExAllocatePool, ExAllocatePoolWithTag): the kernel's
// own memory.
HLE(kKrnl, 9, ExAllocatePool) { return SystemAlloc(Arg(c, 0)); }
HLE(kKrnl, 10, ExAllocatePoolWithTag) { return SystemAlloc(Arg(c, 0)); }
HLE(kKrnl, 15, ExFreePool) {
  SystemFree(Arg(c, 0));
  return 0;
}

}  // namespace recomp::xbox360
