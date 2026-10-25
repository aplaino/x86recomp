// libkernel HLE: virtual memory. Flexible memory and anonymous mmap come
// from the process's guest allocator (reservations at its 64 KiB
// granularity, a multiple of the PS4's 16 KiB pages); GPU protection bits
// are accepted and have no effect on the CPU side.
//
// Direct memory is the PS4's physical memory: allocated as offsets into a
// pool (sceKernelAllocateDirectMemory), then mapped (sceKernelMapDirect-
// Memory). Here an offset is a range of the pool's bookkeeping and each
// mapping fresh guest memory, so an offset mapped twice (aliasing) is
// fatal until something needs it.

#include <algorithm>
#include <map>
#include <mutex>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::orbis::kernel {
namespace {

constexpr int kProtRead = 1, kProtWrite = 2, kProtExec = 4;
constexpr int kMapFixed = 0x10, kMapNoOverwrite = 0x80, kMapAnon = 0x1000;
constexpr uint64_t kPage = 0x4000;
constexpr uint64_t kMapFailed = ~uint64_t(0);

Prot ToProt(int prot) {
  // Lifted code never executes guest memory: exec maps like read.
  const bool w = prot & kProtWrite, r = prot & (kProtRead | kProtExec);
  return w ? Prot::RW : r ? Prot::R : Prot::None;
}

uint64_t RoundUp(uint64_t v) { return (v + kPage - 1) & ~(kPage - 1); }

// Maps len bytes of zeroed memory at addr (fixed: exactly there, replacing
// what is mapped; otherwise a hint) and returns the address, or 0.
gaddr Map(gaddr addr, uint64_t len, int prot, bool fixed, bool no_overwrite) {
  GuestAllocator& vm = g_proc->vm;
  len = RoundUp(len);
  if (!len) return 0;
  gaddr at = 0;
  if (fixed) {
    if (addr & (kPage - 1)) return 0;
    GuestAllocator::Region r;
    if (vm.Find(addr, &r) && addr + len <= r.base + r.size) {
      if (no_overwrite) return 0;
      vm.Decommit(addr, len);  // its pages read as zero again
      at = addr;
    } else {
      at = vm.Reserve(addr, len);
      if (at != addr) return 0;
    }
  } else {
    if (addr) at = vm.Reserve(addr, len);
    if (!at || at != addr) at = vm.Reserve(0, len);
    if (!at) return 0;
  }
  if (!vm.Commit(at, len, ToProt(prot))) return 0;
  return at;
}

// The pool's size: the 5 GiB commonly documented as a game's direct
// memory budget (not measured on hardware yet).
constexpr uint64_t kDirectMemorySize = 5ull << 30;

struct DirectBlock {
  uint64_t len;
  int type;
  gaddr mapped = 0;
};
std::mutex g_direct_mu;
std::map<uint64_t, DirectBlock> g_direct;  // by offset

// First fit for len bytes aligned to align in [start, end), or ~0.
uint64_t FindDirect(uint64_t start, uint64_t end, uint64_t len, uint64_t align) {
  if (!align) align = kPage;
  end = std::min(end, kDirectMemorySize);
  uint64_t cand = (start + align - 1) & ~(align - 1);
  for (const auto& [off, b] : g_direct) {
    if (off + b.len <= cand) continue;
    if (off >= cand + len) break;
    cand = (off + b.len + align - 1) & ~(align - 1);
  }
  return cand + len <= end ? cand : ~uint64_t(0);
}

bool Unmap(gaddr addr, uint64_t len) {
  GuestAllocator& vm = g_proc->vm;
  GuestAllocator::Region r;
  if (addr & (kPage - 1) || !vm.Find(addr, &r)) return false;
  if (addr == r.base && RoundUp(len) >= r.size) return vm.Release(r.base);
  return vm.Decommit(addr, RoundUp(len));
}

}  // namespace
}  // namespace recomp::orbis::kernel

using namespace recomp::orbis::kernel;
using recomp::hle::Arg;

// void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t
// offset): anonymous mappings only.
ORBIS_HLE("libkernel", mmap, 6) {
  const int flags = int(Arg(c, 3)), fd = int(Arg(c, 4));
  if (!(flags & kMapAnon) || fd != -1)
    recomp::FatalF("mmap of file descriptor %d (flags %X): not supported", fd, flags);
  const recomp::gaddr a = Map(Arg(c, 0), Arg(c, 1), int(Arg(c, 2)), flags & kMapFixed, false);
  if (!a) {
    SetErrno(c, kENOMEM);
    return kMapFailed;
  }
  return a;
}

ORBIS_HLE("libkernel", munmap, 2) {
  if (Unmap(Arg(c, 0), Arg(c, 1))) return 0;
  SetErrno(c, kEINVAL);
  return ~uint64_t(0);
}

ORBIS_HLE("libkernel", mprotect, 3) {
  if (recomp::g_proc->vm.Commit(Arg(c, 0), Arg(c, 1), ToProt(int(Arg(c, 2))))) return 0;
  SetErrno(c, kEINVAL);
  return ~uint64_t(0);
}

// int sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int prot,
// int flags, const char* name): *addr is the hint (or, with
// SCE_KERNEL_MAP_FIXED, the address) and receives the mapping.
ORBIS_HLE("libkernel", sceKernelMapNamedFlexibleMemory, 5) {
  const recomp::gaddr slot = Arg(c, 0);
  const int flags = int(Arg(c, 3));
  const recomp::gaddr a = Map(recomp::Rd64(slot), Arg(c, 1), int(Arg(c, 2)), flags & kMapFixed,
                              flags & kMapNoOverwrite);
  if (!a) return SceError(kENOMEM);
  recomp::Wr64(slot, a);
  return 0;
}

ORBIS_HLE("libkernel", sceKernelMapFlexibleMemory, 4) {
  const recomp::gaddr slot = Arg(c, 0);
  const int flags = int(Arg(c, 3));
  const recomp::gaddr a = Map(recomp::Rd64(slot), Arg(c, 1), int(Arg(c, 2)), flags & kMapFixed,
                              flags & kMapNoOverwrite);
  if (!a) return SceError(kENOMEM);
  recomp::Wr64(slot, a);
  return 0;
}

ORBIS_HLE("libkernel", sceKernelMunmap, 2) {
  return Unmap(Arg(c, 0), Arg(c, 1)) ? 0 : SceError(kEINVAL);
}

ORBIS_HLE("libkernel", sceKernelMprotect, 3) {
  return recomp::g_proc->vm.Commit(Arg(c, 0), Arg(c, 1), ToProt(int(Arg(c, 2))))
             ? 0
             : SceError(kEINVAL);
}

// Locking pages in memory has nothing to do here.
ORBIS_HLE("libkernel", sceKernelMlock, 2) { return 0; }

ORBIS_HLE("libkernel", getpagesize, 0) { return kPage; }

ORBIS_HLE("libkernel", sceKernelGetDirectMemorySize, 0) { return kDirectMemorySize; }

// int sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len,
// size_t alignment, int type, off_t* out): the first free range of len
// bytes in [start, end).
ORBIS_HLE("libkernel", sceKernelAllocateDirectMemory, 6) {
  const uint64_t len = RoundUp(Arg(c, 2));
  if (!len) return SceError(kEINVAL);
  std::lock_guard<std::mutex> l(g_direct_mu);
  const uint64_t off = FindDirect(Arg(c, 0), Arg(c, 1), len, Arg(c, 3));
  if (off == ~uint64_t(0)) return SceError(kEAGAIN);
  g_direct[off] = {len, int(Arg(c, 4))};
  recomp::Wr64(Arg(c, 5), off);
  return 0;
}

// int sceKernelAllocateMainDirectMemory(size_t len, size_t alignment, int
// type, off_t* out): anywhere in the pool.
ORBIS_HLE("libkernel", sceKernelAllocateMainDirectMemory, 4) {
  const uint64_t len = RoundUp(Arg(c, 0));
  if (!len) return SceError(kEINVAL);
  std::lock_guard<std::mutex> l(g_direct_mu);
  const uint64_t off = FindDirect(0, kDirectMemorySize, len, Arg(c, 1));
  if (off == ~uint64_t(0)) return SceError(kEAGAIN);
  g_direct[off] = {len, int(Arg(c, 2))};
  recomp::Wr64(Arg(c, 3), off);
  return 0;
}

// int sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t
// alignment, off_t* start_out, size_t* size_out): the largest free range.
ORBIS_HLE("libkernel", sceKernelAvailableDirectMemorySize, 5) {
  uint64_t align = Arg(c, 2) ? Arg(c, 2) : kPage;
  const uint64_t start = Arg(c, 0), end = std::min<uint64_t>(Arg(c, 1), kDirectMemorySize);
  std::lock_guard<std::mutex> l(g_direct_mu);
  uint64_t best = 0, best_at = 0, cand = (start + align - 1) & ~(align - 1);
  auto gap = [&](uint64_t to) {
    if (to > cand && to - cand > best) best = to - cand, best_at = cand;
  };
  for (const auto& [off, b] : g_direct) {
    if (off + b.len <= cand) continue;
    gap(std::min(off, end));
    cand = (off + b.len + align - 1) & ~(align - 1);
  }
  gap(end);
  if (!best) return SceError(kEAGAIN);
  if (Arg(c, 3)) recomp::Wr64(Arg(c, 3), best_at);
  if (Arg(c, 4)) recomp::Wr64(Arg(c, 4), best);
  return 0;
}

// The mapping of direct memory [off, off + len) at *slot (a hint, or the
// address with SCE_KERNEL_MAP_FIXED).
uint64_t MapDirect(recomp::gaddr slot, uint64_t len, int prot, int flags, uint64_t off,
                   uint64_t align) {
  (void)align;  // reservations are 64 KiB aligned already
  std::lock_guard<std::mutex> l(g_direct_mu);
  auto it = g_direct.upper_bound(off);
  if (it == g_direct.begin()) return SceError(kEACCES);
  --it;
  if (off + len > it->first + it->second.len) return SceError(kEACCES);
  if (it->second.mapped)
    recomp::FatalF("direct memory %llX mapped twice (aliasing): not supported",
                   (unsigned long long)off);
  const recomp::gaddr a =
      Map(recomp::Rd64(slot), len, prot, flags & kMapFixed, flags & kMapNoOverwrite);
  if (!a) return SceError(kENOMEM);
  it->second.mapped = a;
  recomp::Wr64(slot, a);
  return 0;
}

// int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int
// flags, off_t start, size_t alignment)
ORBIS_HLE("libkernel", sceKernelMapDirectMemory, 6) {
  return MapDirect(Arg(c, 0), Arg(c, 1), int(Arg(c, 2)), int(Arg(c, 3)), Arg(c, 4), Arg(c, 5));
}

// The same with a name for the mapping (seventh argument).
ORBIS_HLE("libkernel", sceKernelMapNamedDirectMemory, 7) {
  return MapDirect(Arg(c, 0), Arg(c, 1), int(Arg(c, 2)), int(Arg(c, 3)), Arg(c, 4), Arg(c, 5));
}

// int sceKernelReleaseDirectMemory(off_t start, size_t len): the
// allocations in the range, unmapping them.
ORBIS_HLE("libkernel", sceKernelReleaseDirectMemory, 2) {
  const uint64_t start = Arg(c, 0), end = start + Arg(c, 1);
  std::lock_guard<std::mutex> l(g_direct_mu);
  for (auto it = g_direct.lower_bound(start); it != g_direct.end() && it->first < end;) {
    if (it->second.mapped) Unmap(it->second.mapped, it->second.len);
    it = g_direct.erase(it);
  }
  return 0;
}
