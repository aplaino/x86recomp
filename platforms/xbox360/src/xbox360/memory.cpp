#include "xbox360/memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ppc/fatal.h"

namespace recomp::xbox360 {
namespace {

using ppc::Fatal;

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kPhysical64kBase = 0xA0000000, kPhysical16mBase = 0xC0000000,
                   kPhysical4kBase = 0xE0000000;

struct HeapDef {
  uint32_t base, end, page;
};
// The last 3 MiB of the 4 KiB physical view are never handed out (their
// physical pages, one page further, would run past the end).
constexpr HeapDef kHeaps[] = {
    {0x00000000, 0x40000000, 0x1000},    {0x40000000, 0x7F000000, 0x10000},
    {0x80000000, 0x90000000, 0x10000},   {0x90000000, 0xA0000000, 0x1000},
    {kPhysical64kBase, 0xC0000000, 0x10000}, {kPhysical16mBase, 0xE0000000, 0x1000000},
    {kPhysical4kBase, 0xFFD00000, 0x1000},
};
static_assert(sizeof kHeaps / sizeof kHeaps[0] == size_t(Heap::kCount));

enum State : uint8_t { kFree, kReserved, kCommitted };
// One 4 KiB guest page. `base` and `pages` (the reservation's size, kept
// at its first page) are valid unless the page is free.
struct Page {
  uint32_t base, pages;
  uint16_t protect, alloc_protect;
  State state;
};

uint8_t* g_mem;
Page* g_pages;                    // 2^20 of them, zero (free) until used
std::vector<uint8_t> g_frames;    // physical 4 KiB frames in use
std::mutex g_lock;

const HeapDef& Def(Heap h) { return kHeaps[size_t(h)]; }
bool IsPhysical(Heap h) { return h >= Heap::kPhysical64k; }
uint32_t ViewOffset(Heap h) {
  return h == Heap::kPhysical4k ? kPhysical4kBase - kPhysical4kOffset : Def(h).base;
}
uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// A physical memory object of kPhysicalSize, for the views to share.
int PhysicalMemoryFd() {
#if defined(__linux__)
  int fd = memfd_create("xbox360-physical", MFD_CLOEXEC);
#else
  char name[64];
  std::snprintf(name, sizeof name, "/xbox360-physical-%d", int(getpid()));
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) shm_unlink(name);
#endif
  if (fd < 0 || ftruncate(fd, kPhysicalSize) != 0)
    Fatal(nullptr, "cannot create the guest's physical memory: %s", std::strerror(errno));
  return fd;
}

void MapView(int fd, uint32_t va, uint32_t size, uint32_t offset) {
  void* p = mmap(g_mem + va, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, offset);
  if (p == MAP_FAILED) Fatal(nullptr, "cannot map the physical view at %x: %s", va, std::strerror(errno));
}

// Every page of [va, va + size) free, and in a physical view its frames.
bool RangeFree(Heap h, uint32_t va, uint32_t size) {
  for (uint32_t p = va / kPage; p < (uint64_t(va) + size) / kPage; p++)
    if (g_pages[p].state != kFree) return false;
  if (IsPhysical(h)) {
    uint32_t f = (va - ViewOffset(h)) / kPage;
    for (uint32_t k = 0; k < size / kPage; k++)
      if (g_frames[f + k]) return false;
  }
  return true;
}

void Claim(Heap h, uint32_t va, uint32_t size) {
  uint32_t first = va / kPage, n = size / kPage;
  for (uint32_t k = 0; k < n; k++) g_pages[first + k] = {va, 0, 0, 0, kReserved};
  g_pages[first].pages = n;
  if (IsPhysical(h)) std::memset(&g_frames[(va - ViewOffset(h)) / kPage], 1, n);
}

// The lowest (highest) free range of `size` bytes whose start is in
// [lo, hi] and aligned to `align`: in a physical view the physical address
// is aligned (the 4 KiB view's addresses are a page off it). 0 if none.
uint32_t Find(Heap h, uint32_t size, uint32_t align, uint64_t lo, uint64_t hi, bool top_down) {
  const HeapDef& d = Def(h);
  uint64_t bias = IsPhysical(h) ? ViewOffset(h) : 0;
  lo = std::max<uint64_t>(lo, d.base);
  if (size > d.end - d.base) return 0;
  hi = std::min<uint64_t>(hi, uint64_t(d.end) - size);
  lo = bias + (lo - bias + align - 1) / align * align;
  if (lo > hi) return 0;
  hi -= (hi - lo) % align;
  for (uint64_t k = 0; k <= (hi - lo) / align; k++) {
    uint64_t va = top_down ? hi - k * align : lo + k * align;
    if (RangeFree(h, uint32_t(va), size)) return uint32_t(va);
  }
  return 0;
}

uint32_t ReserveLocked(Heap h, uint32_t va, uint32_t size, uint32_t align, uint64_t lo, uint64_t hi,
                       bool top_down) {
  const HeapDef& d = Def(h);
  size = RoundUp(size, d.page);
  align = std::max(RoundUp(align ? align : d.page, d.page), d.page);
  if (!size) return 0;
  if (va) {
    va &= ~(d.page - 1);
    if (va < d.base || uint64_t(va) + size > d.end || !RangeFree(h, va, size)) return 0;
  } else {
    va = Find(h, size, align, lo, hi, top_down);
    if (!va) return 0;
  }
  Claim(h, va, size);
  return va;
}

// [va, va + size) as whole pages of one reservation; false if it is not.
bool InOneReservation(uint32_t& va, uint32_t& size) {
  uint64_t end = (uint64_t(va) + size + kPage - 1) & ~uint64_t(kPage - 1);
  va &= ~(kPage - 1);
  if (end <= va || end > (1ull << 32)) return false;
  size = uint32_t(end - va);
  const Page& first = g_pages[va / kPage];
  if (first.state == kFree) return false;
  for (uint32_t p = va / kPage; p < end / kPage; p++)
    if (g_pages[p].state == kFree || g_pages[p].base != first.base) return false;
  return true;
}

}  // namespace

void MapGuestMemory() {
  long host_page = sysconf(_SC_PAGESIZE);
  if (host_page != long(kPhysical4kOffset))
    Fatal(nullptr,
          "this host's pages are %ld bytes: the 0xE0000000 physical view, which starts one 4 KiB "
          "page into physical memory, can only be aliased with 4 KiB host pages for now "
          "(platforms/xbox360/ARCHITECTURE.md, \"Runtime: memory\")",
          host_page);
  void* p = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) Fatal(nullptr, "cannot reserve the 4 GiB guest arena: %s", std::strerror(errno));
  g_mem = static_cast<uint8_t*>(p);
  int fd = PhysicalMemoryFd();
  MapView(fd, kPhysical64kBase, kPhysicalSize, 0);
  MapView(fd, kPhysical16mBase, kPhysicalSize, 0);
  MapView(fd, kPhysical4kBase, kPhysicalSize - kPhysical4kOffset, kPhysical4kOffset);
  close(fd);
  g_pages = static_cast<Page*>(std::calloc(size_t(1) << 20, sizeof(Page)));
  g_frames.assign(kPhysicalSize / kPage, 0);
  if (!g_pages) Fatal(nullptr, "out of host memory for the guest page table");
  // The first 64 KiB are never mapped, as on the console: a null pointer
  // faults.
  Claim(Heap::kVirtual4k, 0, 0x10000);
  if (mprotect(g_mem, 0x10000, PROT_NONE) != 0)
    Fatal(nullptr, "cannot protect guest page 0: %s", std::strerror(errno));
}

uint8_t* Mem() { return g_mem; }

uint32_t HeapBase(Heap h) { return Def(h).base; }
uint32_t HeapEnd(Heap h) { return Def(h).end; }
uint32_t HeapPageSize(Heap h) { return Def(h).page; }

bool HeapOf(uint32_t va, Heap& h) {
  for (size_t k = 0; k < size_t(Heap::kCount); k++)
    if (va >= kHeaps[k].base && va < kHeaps[k].end) {
      h = Heap(k);
      return true;
    }
  return false;
}

uint32_t Reserve(Heap h, uint32_t va, uint32_t size, uint32_t align, bool top_down) {
  std::lock_guard<std::mutex> g(g_lock);
  return ReserveLocked(h, va, size, align, 0, ~0ull, top_down);
}

uint32_t ReservePhysical(Heap h, uint32_t size, uint32_t align, uint32_t lo, uint32_t hi,
                         bool top_down) {
  std::lock_guard<std::mutex> g(g_lock);
  // Bounds on the physical pages, as bounds on where in the view the
  // allocation may start.
  uint64_t off = ViewOffset(h), rounded = RoundUp(size, Def(h).page), top = uint64_t(hi) + 1;
  if (top < lo + rounded) return 0;
  return ReserveLocked(h, 0, size, align, off + lo, off + top - rounded, top_down);
}

bool Commit(uint32_t va, uint32_t size, uint32_t protect) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!InOneReservation(va, size)) return false;
  for (uint32_t p = va / kPage; p < (uint64_t(va) + size) / kPage; p++) {
    if (g_pages[p].state != kCommitted) g_pages[p].alloc_protect = uint16_t(protect);
    g_pages[p].state = kCommitted;
    g_pages[p].protect = uint16_t(protect);
  }
  return true;
}

bool Decommit(uint32_t va, uint32_t size) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!InOneReservation(va, size)) return false;
  for (uint32_t p = va / kPage; p < (uint64_t(va) + size) / kPage; p++)
    if (g_pages[p].state == kCommitted) {
      std::memset(g_mem + uint64_t(p) * kPage, 0, kPage);
      g_pages[p].state = kReserved;
      g_pages[p].protect = 0;
    }
  return true;
}

uint32_t Release(uint32_t va) {
  std::lock_guard<std::mutex> g(g_lock);
  const Page& first = g_pages[va / kPage];
  if (va % kPage || first.state == kFree || first.base != va) return 0;
  uint32_t n = first.pages;
  Heap h;
  bool physical = HeapOf(va, h) && IsPhysical(h);
  for (uint32_t k = 0; k < n; k++) {
    Page& pg = g_pages[va / kPage + k];
    if (pg.state == kCommitted) std::memset(g_mem + uint64_t(va) + uint64_t(k) * kPage, 0, kPage);
    pg = {};
  }
  if (physical) std::memset(&g_frames[(va - ViewOffset(h)) / kPage], 0, n);
  return n * kPage;
}

bool Protect(uint32_t va, uint32_t size, uint32_t protect, uint32_t* old) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!InOneReservation(va, size)) return false;
  for (uint32_t p = va / kPage; p < (uint64_t(va) + size) / kPage; p++)
    if (g_pages[p].state != kCommitted) return false;
  if (old) *old = g_pages[va / kPage].protect;
  for (uint32_t p = va / kPage; p < (uint64_t(va) + size) / kPage; p++)
    g_pages[p].protect = uint16_t(protect);
  return true;
}

bool Query(uint32_t va, RegionInfo& r) {
  std::lock_guard<std::mutex> g(g_lock);
  Heap h;
  if (!HeapOf(va, h)) return false;
  uint32_t p = va / kPage;
  const Page& first = g_pages[p];
  r = {p * kPage, first.state == kFree ? 0 : first.base,
       first.state == kFree ? 0u : g_pages[first.base / kPage].alloc_protect, 0,
       first.state == kFree ? kMemFree : first.state == kReserved ? kMemReserve : kMemCommit,
       first.protect};
  uint32_t end = Def(h).end / kPage;
  for (; p < end; p++) {
    const Page& pg = g_pages[p];
    if (pg.state != first.state || pg.protect != first.protect ||
        (first.state != kFree && pg.base != first.base))
      break;
  }
  r.size = p * kPage - r.base;
  return true;
}

uint32_t PageProtect(uint32_t va) {
  std::lock_guard<std::mutex> g(g_lock);
  const Page& pg = g_pages[va / kPage];
  return pg.state == kCommitted ? pg.protect : 0;
}

uint32_t ReservationSize(uint32_t va) {
  std::lock_guard<std::mutex> g(g_lock);
  const Page& pg = g_pages[va / kPage];
  return pg.state != kFree && pg.base == va ? pg.pages * kPage : 0;
}

bool PhysicalAddress(uint32_t va, uint32_t& pa) {
  Heap h;
  if (!HeapOf(va, h) || !IsPhysical(h)) return false;
  pa = va - ViewOffset(h);
  return true;
}

namespace {
// Size classes of SystemAlloc, 16 bytes to 2 KiB, carved from 64 KiB
// chunks; larger requests take whole pages.
constexpr uint32_t kChunk = 0x10000, kClasses = 8;
std::vector<uint32_t> g_free[kClasses];
std::unordered_map<uint32_t, uint32_t> g_sizes;  // va -> class, or bytes for page allocations
std::mutex g_sys_lock;

uint32_t SystemPages(uint32_t size) {
  uint32_t va = Reserve(Heap::kImage4k, 0, size, kPage, true);
  if (!va || !Commit(va, size, kPageReadWrite))
    Fatal(nullptr, "out of guest memory for kernel objects (%u bytes)", size);
  return va;
}
}  // namespace

MemoryUse QueryMemoryUse() {
  std::lock_guard<std::mutex> g(g_lock);
  MemoryUse u{0, 0};
  for (uint32_t p = 0; p < kPhysical64kBase / kPage; p++) {
    if (g_pages[p].state == kCommitted) u.pages_in_use++;
    if (p < Def(Heap::kImage64k).base / kPage && g_pages[p].state != kFree) u.virtual_reserved += kPage;
  }
  for (uint8_t f : g_frames) u.pages_in_use += f;
  return u;
}

uint32_t SystemAlloc(uint32_t size) {
  std::lock_guard<std::mutex> g(g_sys_lock);
  uint32_t cls = 0;
  while (cls < kClasses && (16u << cls) < size) cls++;
  if (cls == kClasses) {
    uint32_t va = SystemPages(RoundUp(size, kPage));
    g_sizes[va] = RoundUp(size, kPage);
    return va;
  }
  std::vector<uint32_t>& list = g_free[cls];
  if (list.empty()) {
    uint32_t chunk = SystemPages(kChunk);
    for (uint32_t off = kChunk; off >= (16u << cls); off -= 16u << cls)
      list.push_back(chunk + off - (16u << cls));
  }
  uint32_t va = list.back();
  list.pop_back();
  g_sizes[va] = cls;
  return va;
}

void SystemFree(uint32_t va) {
  std::lock_guard<std::mutex> g(g_sys_lock);
  auto it = g_sizes.find(va);
  if (it == g_sizes.end()) Fatal(nullptr, "SystemFree(%x): not allocated", va);
  if (it->second >= kClasses) {
    Release(va);
  } else {
    std::memset(g_mem + va, 0, 16u << it->second);
    g_free[it->second].push_back(va);
  }
  g_sizes.erase(it);
}

}  // namespace recomp::xbox360
