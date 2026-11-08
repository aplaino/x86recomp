// libkernel HLE: virtual memory. Flexible memory and anonymous mmap come
// from the process's guest allocator (reservations at its 64 KiB
// granularity, a multiple of the PS4's 16 KiB pages); GPU protection bits
// are accepted and have no effect on the CPU side.
//
// Direct memory is the PS4's physical memory: allocated as offsets into a
// pool (sceKernelAllocateDirectMemory), then mapped (sceKernelMapDirect-
// Memory). Here an offset is a range of the pool's bookkeeping and each
// mapping fresh guest memory, so a range mapped twice at once (aliasing)
// is fatal until something needs it. A range unmapped while allocated
// keeps its contents (its pages that are not zero, held on the host) for
// its next mapping, as physical memory would.
//
// The mappings these functions make are recorded (kind, protection,
// direct-memory offset, type, name) for sceKernelVirtualQuery; memory the
// process maps itself (images, stacks) is not.

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/log.h"
#include "x86/runtime.h"

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

// The pool's size: a game's direct memory on a base console (4.5 GiB,
// measured on firmware 11.00 in a game's process).
constexpr uint64_t kDirectMemorySize = 0x120000000;

struct DirectBlock {
  uint64_t len;
  int type;
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

// A recorded mapping, from its key (the start) to end.
struct Vma {
  gaddr end;
  bool direct;
  uint64_t offset;  // direct memory: the start's offset
  int prot, type;
  std::string name;
};
std::mutex g_vma_mu;
std::map<gaddr, Vma> g_vma;

// Splits the records so that none crosses at (g_vma_mu held).
void SplitAt(gaddr at) {
  auto it = g_vma.upper_bound(at);
  if (it == g_vma.begin()) return;
  --it;
  if (it->first >= at || it->second.end <= at) return;
  Vma tail = it->second;
  if (tail.direct) tail.offset += at - it->first;
  it->second.end = at;
  g_vma[at] = tail;
}

// The records' pieces in [start, end), split off at its ends.
std::map<gaddr, Vma>::iterator VmaRange(gaddr start, gaddr end) {
  SplitAt(start);
  SplitAt(end);
  return g_vma.lower_bound(start);
}

std::string Name(gaddr p) { return p ? GStr(p) : std::string(); }

// Direct memory's contents while it is not mapped: its pages that are not
// zero, by offset (g_vma_mu).
std::map<uint64_t, std::vector<uint8_t>> g_unmapped;

// Drops the records in [start, len), keeping direct memory's contents.
void Forget(gaddr start, uint64_t len) {
  std::lock_guard<std::mutex> l(g_vma_mu);
  for (auto it = VmaRange(start, start + len); it != g_vma.end() && it->first < start + len;) {
    if (it->second.direct) {
      // Its pages may be mapped without read access.
      g_proc->vm.Commit(it->first, it->second.end - it->first, Prot::RW);
      for (gaddr p = it->first; p < it->second.end; p += kPage) {
        const uint8_t* b = GPtr(p);
        if (std::all_of(b, b + kPage, [](uint8_t v) { return v == 0; })) continue;
        g_unmapped[it->second.offset + (p - it->first)].assign(b, b + kPage);
      }
    }
    it = g_vma.erase(it);
  }
}

void Record(gaddr start, uint64_t len, Vma v) {
  len = RoundUp(len);
  Forget(start, len);
  v.end = start + len;
  std::lock_guard<std::mutex> l(g_vma_mu);
  g_vma[start] = std::move(v);
}

void Reprotect(gaddr start, uint64_t len, int prot) {
  std::lock_guard<std::mutex> l(g_vma_mu);
  const gaddr end = start + RoundUp(len);
  for (auto it = VmaRange(start, end); it != g_vma.end() && it->first < end; ++it) it->second.prot = prot;
}

bool Unmap(gaddr addr, uint64_t len) {
  GuestAllocator& vm = g_proc->vm;
  GuestAllocator::Region r;
  if (addr & (kPage - 1) || !vm.Find(addr, &r)) return false;
  if (addr == r.base && RoundUp(len) >= r.size) {
    Forget(r.base, r.size);
    return vm.Release(r.base);
  }
  Forget(addr, RoundUp(len));
  return vm.Decommit(addr, RoundUp(len));
}

// The mappings of direct memory in [start, end): (address, length).
std::vector<std::pair<gaddr, uint64_t>> DirectMappings(uint64_t start, uint64_t end) {
  std::lock_guard<std::mutex> l(g_vma_mu);
  std::vector<std::pair<gaddr, uint64_t>> out;
  for (const auto& [at, v] : g_vma) {
    if (!v.direct) continue;
    const uint64_t s = std::max(start, v.offset), e = std::min(end, v.offset + (v.end - at));
    if (s < e) out.push_back({at + (s - v.offset), e - s});
  }
  return out;
}

// The allocations in [start, end), unmapping them (g_direct_mu held).
void ReleaseDirect(uint64_t start, uint64_t end) {
  for (auto it = g_direct.lower_bound(start); it != g_direct.end() && it->first < end;) {
    const uint64_t s = it->first, e = s + it->second.len;
    for (const auto& [at, len] : DirectMappings(s, e)) Unmap(at, len);
    std::lock_guard<std::mutex> l(g_vma_mu);
    g_unmapped.erase(g_unmapped.lower_bound(s), g_unmapped.lower_bound(e));
    it = g_direct.erase(it);
  }
}

// Maps len bytes of zeroed memory at addr (fixed: exactly there, unmapping
// what is mapped; otherwise a hint) and returns the address, or 0.
gaddr Map(gaddr addr, uint64_t len, int prot, bool fixed, bool no_overwrite) {
  GuestAllocator& vm = g_proc->vm;
  len = RoundUp(len);
  if (!len) return 0;
  // The console honours addresses past the arena's end (process.cpp);
  // placing the mapping elsewhere would go unnoticed.
  if (addr && (addr >= vm.mem().size() || len > vm.mem().size() - addr))
    recomp::FatalF("mapping of %llX bytes at %llX: beyond the modelled address space "
                   "(%llX), not supported",
                   (unsigned long long)len, (unsigned long long)addr,
                   (unsigned long long)vm.mem().size());
  gaddr at = 0;
  if (fixed) {
    if (addr & (kPage - 1)) return 0;
    GuestAllocator::Region r;
    if (vm.Find(addr, &r) && addr + len <= r.base + r.size) {
      if (no_overwrite) return 0;
      Forget(addr, len);
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
  Record(a, Arg(c, 1), {0, false, 0, int(Arg(c, 2)), 0, ""});
  return a;
}

ORBIS_HLE("libkernel", munmap, 2) {
  if (Unmap(Arg(c, 0), Arg(c, 1))) return 0;
  SetErrno(c, kEINVAL);
  return ~uint64_t(0);
}

ORBIS_HLE("libkernel", mprotect, 3) {
  if (recomp::g_proc->vm.Commit(Arg(c, 0), Arg(c, 1), ToProt(int(Arg(c, 2))))) {
    Reprotect(Arg(c, 0), Arg(c, 1), int(Arg(c, 2)));
    return 0;
  }
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
  Record(a, Arg(c, 1), {0, false, 0, int(Arg(c, 2)), 0, Name(Arg(c, 4))});
  recomp::Wr64(slot, a);
  return 0;
}

ORBIS_HLE("libkernel", sceKernelMapFlexibleMemory, 4) {
  const recomp::gaddr slot = Arg(c, 0);
  const int flags = int(Arg(c, 3));
  const recomp::gaddr a = Map(recomp::Rd64(slot), Arg(c, 1), int(Arg(c, 2)), flags & kMapFixed,
                              flags & kMapNoOverwrite);
  if (!a) return SceError(kENOMEM);
  Record(a, Arg(c, 1), {0, false, 0, int(Arg(c, 2)), 0, ""});
  recomp::Wr64(slot, a);
  return 0;
}

ORBIS_HLE("libkernel", sceKernelMunmap, 2) {
  return Unmap(Arg(c, 0), Arg(c, 1)) ? 0 : SceError(kEINVAL);
}

ORBIS_HLE("libkernel", sceKernelMprotect, 3) {
  if (!recomp::g_proc->vm.Commit(Arg(c, 0), Arg(c, 1), ToProt(int(Arg(c, 2))))) return SceError(kEINVAL);
  Reprotect(Arg(c, 0), Arg(c, 1), int(Arg(c, 2)));
  return 0;
}

// Locking pages in memory has nothing to do here.
ORBIS_HLE("libkernel", sceKernelMlock, 2) { return 0; }

// int sceKernelSetVirtualRangeName(void* addr, size_t len, const char*
// name): in 11.00's libkernel a bare syscall; the name labels the range
// for debugging tools only. What the kernel answers for a range not
// mapped is not measured: fatal.
ORBIS_HLE("libkernel", sceKernelSetVirtualRangeName, 3) {
  recomp::GuestAllocator::Region r;
  const recomp::gaddr a = Arg(c, 0);
  if (!recomp::g_proc->vm.Find(a, &r) || Arg(c, 1) > r.base + r.size - a)
    recomp::FatalF("sceKernelSetVirtualRangeName(%llX, %llX): a range not mapped, not "
                   "implemented yet",
                   (unsigned long long)a, (unsigned long long)Arg(c, 1));
  std::lock_guard<std::mutex> l(g_vma_mu);
  const recomp::gaddr end = a + RoundUp(Arg(c, 1));
  const std::string name = Name(Arg(c, 2));
  for (auto it = VmaRange(a, end); it != g_vma.end() && it->first < end; ++it) it->second.name = name;
  return 0;
}

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
                   uint64_t align, const std::string& name) {
  (void)align;  // reservations are 64 KiB aligned already
  std::lock_guard<std::mutex> l(g_direct_mu);
  auto it = g_direct.upper_bound(off);
  if (it == g_direct.begin()) return SceError(kEACCES);
  --it;
  if (off + len > it->first + it->second.len) return SceError(kEACCES);
  if (!DirectMappings(off, off + len).empty())
    recomp::FatalF("direct memory %llX mapped twice (aliasing): not supported",
                   (unsigned long long)off);
  const recomp::gaddr a =
      Map(recomp::Rd64(slot), len, prot, flags & kMapFixed, flags & kMapNoOverwrite);
  if (!a) return SceError(kENOMEM);
  Record(a, len, {0, true, off, prot, it->second.type, name});
  {
    // The range's contents from when it was last mapped.
    std::lock_guard<std::mutex> vl(g_vma_mu);
    const uint64_t end = off + RoundUp(len);
    for (auto u = g_unmapped.lower_bound(off); u != g_unmapped.end() && u->first < end;) {
      recomp::g_proc->vm.Commit(a + (u->first - off), kPage, recomp::Prot::RW);
      std::memcpy(recomp::GPtr(a + (u->first - off)), u->second.data(), kPage);
      recomp::g_proc->vm.Commit(a + (u->first - off), kPage, ToProt(prot));
      u = g_unmapped.erase(u);
    }
  }
  recomp::Wr64(slot, a);
  return 0;
}

// int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int
// flags, off_t start, size_t alignment)
ORBIS_HLE("libkernel", sceKernelMapDirectMemory, 6) {
  return MapDirect(Arg(c, 0), Arg(c, 1), int(Arg(c, 2)), int(Arg(c, 3)), Arg(c, 4), Arg(c, 5), "");
}

// The same with a name for the mapping (seventh argument).
ORBIS_HLE("libkernel", sceKernelMapNamedDirectMemory, 7) {
  return MapDirect(Arg(c, 0), Arg(c, 1), int(Arg(c, 2)), int(Arg(c, 3)), Arg(c, 4), Arg(c, 5),
                   Name(Arg(c, 6)));
}

// int sceKernelReleaseDirectMemory(off_t start, size_t len): the
// allocations in the range, unmapping them.
ORBIS_HLE("libkernel", sceKernelReleaseDirectMemory, 2) {
  const uint64_t start = Arg(c, 0), end = start + Arg(c, 1);
  std::lock_guard<std::mutex> l(g_direct_mu);
  ReleaseDirect(start, end);
  return 0;
}

// int sceKernelCheckedReleaseDirectMemory(off_t start, size_t len): in
// 11.00's libkernel, like sceKernelReleaseDirectMemory an ioctl on the
// direct memory device (0x80108015 for 0x80108002), whose kernel side also
// checks the range. A range whole allocations fill is released; others
// (what the kernel answers is not measured) are fatal.
ORBIS_HLE("libkernel", sceKernelCheckedReleaseDirectMemory, 2) {
  const uint64_t start = Arg(c, 0), end = start + Arg(c, 1);
  std::lock_guard<std::mutex> l(g_direct_mu);
  uint64_t at = start;
  for (auto it = g_direct.find(start); it != g_direct.end() && it->first == at && at < end; ++it)
    at += it->second.len;
  if (start == end || at != end)
    recomp::FatalF("sceKernelCheckedReleaseDirectMemory(%llX, %llX): a range not filled by "
                   "whole allocations, not implemented yet",
                   (unsigned long long)start, (unsigned long long)(end - start));
  ReleaseDirect(start, end);
  return 0;
}

bool recomp::orbis::kernel::MemoryType(recomp::gaddr a, int* type) {
  std::lock_guard<std::mutex> l(g_vma_mu);
  auto it = g_vma.upper_bound(a);
  if (it == g_vma.begin() || (--it)->second.end <= a) return false;
  *type = it->second.type;
  return true;
}

// int sceKernelVirtualQuery(const void* addr, int flags, SceKernelVirtual-
// QueryInfo* info, size_t size): in 11.00's libkernel a bare syscall
// (572). The mapping holding addr, as a 0x48-byte record: start, end,
// direct-memory offset (of start), protection, memory type, a flags byte
// (bit 0 flexible, 1 direct, 2 stack, 3 pooled, 4 committed) and a
// 32-byte name. The kernel's answers for an address not mapped, for
// SCE_KERNEL_VQ_FIND_NEXT (flags 1) and for other sizes are not measured:
// fatal, like memory the process mapped itself (not recorded).
ORBIS_HLE("libkernel", sceKernelVirtualQuery, 4) {
  const recomp::gaddr a = Arg(c, 0), info = Arg(c, 2);
  if (Arg(c, 1) != 0 || Arg(c, 3) != 0x48)
    recomp::FatalF("sceKernelVirtualQuery(%llX, flags %llX, size %llX): not implemented yet",
                   (unsigned long long)a, (unsigned long long)Arg(c, 1), (unsigned long long)Arg(c, 3));
  std::lock_guard<std::mutex> l(g_vma_mu);
  auto it = g_vma.upper_bound(a);
  if (it == g_vma.begin() || (--it)->second.end <= a)
    recomp::FatalF("sceKernelVirtualQuery(%llX): an address no mapping of the game's holds, not "
                   "implemented yet",
                   (unsigned long long)a);
  const Vma& v = it->second;
  uint8_t out[0x48] = {};
  const uint64_t start = it->first, end = v.end, offset = v.direct ? v.offset : 0;
  std::memcpy(out, &start, 8);
  std::memcpy(out + 8, &end, 8);
  std::memcpy(out + 0x10, &offset, 8);
  std::memcpy(out + 0x18, &v.prot, 4);
  std::memcpy(out + 0x1C, &v.type, 4);
  out[0x20] = (v.direct ? 2 : 1) | 0x10;
  std::memcpy(out + 0x21, v.name.data(), std::min<size_t>(v.name.size(), 31));
  std::memcpy(recomp::GPtr(info), out, sizeof out);
  return 0;
}
