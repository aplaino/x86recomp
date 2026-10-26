#pragma once
// The guest address space as the Xbox 360's kernel lays it out
// (ARCHITECTURE.md, "Runtime: memory"): one 4 GiB arena, guest address a
// at Mem() + a, big-endian. The 512 MiB of physical memory is mapped three
// times, at 0xA0000000 (64 KiB pages), 0xC0000000 (16 MiB pages) and
// 0xE0000000 (4 KiB pages, one page into physical memory), so what is
// written through one view is read through the others, as on the
// console. Allocation state is kept per 4 KiB guest page; the host's
// protection is never changed for it.

#include <cstdint>

namespace recomp::xbox360 {

// The ranges memory is allocated from, each with the page size the kernel
// gives it.
enum class Heap : uint8_t {
  kVirtual4k,    // 0x00000000-0x3FFFFFFF  NtAllocateVirtualMemory
  kVirtual64k,   // 0x40000000-0x7EFFFFFF  the same with MEM_LARGE_PAGES, stacks
  kImage64k,     // 0x80000000-0x8FFFFFFF  XEX images
  kImage4k,      // 0x90000000-0x9FFFFFFF  XEX images, the kernel's own objects
  kPhysical64k,  // 0xA0000000-0xBFFFFFFF  MmAllocatePhysicalMemoryEx
  kPhysical16m,  // 0xC0000000-0xDFFFFFFF
  kPhysical4k,   // 0xE0000000-0xFFCFFFFF
  kCount,
};

constexpr uint32_t kPhysicalSize = 0x20000000;
// The 4 KiB physical view starts this far into physical memory.
constexpr uint32_t kPhysical4kOffset = 0x1000;

// Page protections (NT's PAGE_* values, which the kernel uses).
constexpr uint32_t kPageNoAccess = 0x01, kPageReadOnly = 0x02, kPageReadWrite = 0x04,
                   kPageExecute = 0x10, kPageExecuteRead = 0x20, kPageExecuteReadWrite = 0x40,
                   kPageGuard = 0x100, kPageNoCache = 0x200, kPageWriteCombine = 0x400;
// Allocation types and states (MEM_*).
constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000,
                   kMemRelease = 0x8000, kMemFree = 0x10000, kMemPrivate = 0x20000,
                   kMemImage = 0x1000000, kMemTopDown = 0x100000, kMemNoZero = 0x800000,
                   kMemLargePages = 0x20000000, kMem16MbPages = 0x80000000;

// Maps the arena and the physical views; fatal if the host cannot. Once,
// before any guest code runs.
void MapGuestMemory();
uint8_t* Mem();

uint32_t HeapBase(Heap h);
uint32_t HeapEnd(Heap h);  // exclusive
uint32_t HeapPageSize(Heap h);
// The heap holding va; false outside every heap.
bool HeapOf(uint32_t va, Heap& h);

// Reserves `size` bytes (rounded up to the heap's pages) in heap h: at va,
// or (va = 0) at the lowest free range aligned to `align` (the highest with
// top_down). Returns the base, 0 if the range is taken or none is free. In
// a physical view the physical pages are claimed too, so no other view
// hands them out.
uint32_t Reserve(Heap h, uint32_t va, uint32_t size, uint32_t align, bool top_down);
// The same in a physical view, anywhere its physical pages fall within
// [lo, hi] (MmAllocatePhysicalMemoryEx's bounds).
uint32_t ReservePhysical(Heap h, uint32_t size, uint32_t align, uint32_t lo, uint32_t hi,
                         bool top_down);
// Commits [va, va + size) of one reservation with `protect`; newly
// committed pages are zero. False if the range leaves the reservation.
bool Commit(uint32_t va, uint32_t size, uint32_t protect);
// Decommits [va, va + size) of one reservation (zeroing it).
bool Decommit(uint32_t va, uint32_t size);
// Releases the reservation based at va. Returns its size, 0 if va is not
// the base of one.
uint32_t Release(uint32_t va);
// Sets the protection of committed pages; `old` gets the first page's.
bool Protect(uint32_t va, uint32_t size, uint32_t protect, uint32_t* old);

// NtQueryVirtualMemory's view of the pages from va (rounded down): the
// run of pages in the same state and protection.
struct RegionInfo {
  uint32_t base, alloc_base, alloc_protect, size, state, protect;
};
bool Query(uint32_t va, RegionInfo& r);
// The protection of the page holding va, 0 if it is not committed.
uint32_t PageProtect(uint32_t va);
// The size of the reservation based at va, 0 if none.
uint32_t ReservationSize(uint32_t va);

// What MmQueryStatistics reports: 4 KiB pages in use (every committed
// page outside the physical views, as each takes a physical page on the
// console, plus the physical views' claimed frames) and the bytes reserved
// in the virtual heaps (0x00000000-0x7EFFFFFF).
struct MemoryUse {
  uint32_t pages_in_use, virtual_reserved;
};
MemoryUse QueryMemoryUse();

// The physical address of va in a physical view. False in the others:
// here no physical page backs virtual memory.
bool PhysicalAddress(uint32_t va, uint32_t& pa);

// The kernel's own objects (threads, events of handles, variables):
// committed, zeroed, 16-byte aligned guest memory in the 4 KiB image heap,
// handed out from the top. Fatal when it runs out.
uint32_t SystemAlloc(uint32_t size);
void SystemFree(uint32_t va);

}  // namespace recomp::xbox360
