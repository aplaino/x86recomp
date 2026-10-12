#pragma once
// Guest memory management with Win32 semantics, all inside the arena.
//
// GuestAllocator: VirtualAlloc-style reservations (64 KiB granularity) and
// page commits. GuestHeap: HeapAlloc-style blocks carved from reserved
// segments. Block metadata lives on the host side, so guest overruns can't
// corrupt the allocator (nothing in the guest relies on heap headers).

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "runtime/guest_memory.h"

namespace recomp {

class GuestAllocator {
 public:
  static constexpr uint32_t kGranularity = 0x10000;

  void Init(GuestMemory* mem, uint32_t lo, uint32_t hi);
  // Marks a range as permanently reserved (image, thunks, TEBs).
  void MarkReserved(uint32_t va, uint32_t size);

  // Reserves `size` bytes at `va` (rounded down to 64 KiB) or, if va == 0,
  // at the lowest (or highest with top_down) free address. Returns the base
  // or 0.
  uint32_t Reserve(uint32_t va, uint32_t size, bool top_down = false);
  // Commits pages inside a reservation. Returns false if any part of the
  // range is not reserved.
  bool Commit(uint32_t va, uint32_t size, Prot prot);
  bool Decommit(uint32_t va, uint32_t size);
  // Releases a whole reservation given its base. zero=false: see
  // GuestMemory::Decommit.
  bool Release(uint32_t base, bool zero = true);

  struct Region {
    uint32_t base = 0, size = 0;  // reservation containing the address
  };
  // Finds the reservation containing va; false if va is free.
  bool Find(uint32_t va, Region* out) const;
  // First free address >= va and the size of that free gap (for
  // VirtualQuery on unreserved memory).
  void FreeGap(uint32_t va, uint32_t* base, uint32_t* size) const;

  GuestMemory& mem() { return *mem_; }

 private:
  bool FindLocked(uint32_t va, Region* out) const;

  GuestMemory* mem_ = nullptr;
  uint32_t lo_ = 0, hi_ = 0;
  std::map<uint32_t, uint32_t> regions_;  // base -> size
  mutable std::recursive_mutex mu_;
};

class GuestHeap {
 public:
  // growable when max_size == 0 (HeapCreate semantics).
  GuestHeap(GuestAllocator& vm, uint32_t initial_size, uint32_t max_size);
  ~GuestHeap();

  uint32_t handle() const { return handle_; }

  uint32_t Alloc(uint32_t size, bool zero);
  bool Free(uint32_t p);
  // Returns the new address, or 0 on failure. in_place_only: fail rather
  // than move.
  uint32_t ReAlloc(uint32_t p, uint32_t size, bool zero, bool in_place_only);
  // Requested size of block p, or UINT32_MAX if p is not a live block.
  uint32_t Size(uint32_t p) const;

 private:
  struct Block {
    uint32_t size;       // rounded size actually taken
    uint32_t requested;  // size the caller asked for (HeapSize)
  };

  bool Grow(uint32_t need);
  void InsertFree(uint32_t addr, uint32_t size);
  void EraseFree(uint32_t addr);
  bool IsSegmentStart(uint32_t addr) const;

  GuestAllocator& vm_;
  uint32_t max_size_;
  uint32_t handle_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> segments_;  // base, size
  std::map<uint32_t, Block> used_;
  std::map<uint32_t, uint32_t> free_;                // addr -> size
  std::multimap<uint32_t, uint32_t> free_by_size_;   // size -> addr
  mutable std::mutex mu_;
};

}  // namespace recomp
