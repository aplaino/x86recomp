#include "runtime/guest_alloc.h"

#include <cstring>

namespace recomp {

namespace {
constexpr uint32_t kPage = GuestMemory::kPageSize;
uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
}  // namespace

// ---- GuestAllocator ------------------------------------------------------

void GuestAllocator::Init(GuestMemory* mem, uint32_t lo, uint32_t hi) {
  mem_ = mem;
  lo_ = lo;
  hi_ = hi;
}

void GuestAllocator::MarkReserved(uint32_t va, uint32_t size) {
  std::lock_guard<std::recursive_mutex> l(mu_);
  regions_[va] = size;
}

bool GuestAllocator::FindLocked(uint32_t va, Region* out) const {
  auto it = regions_.upper_bound(va);
  if (it == regions_.begin()) return false;
  --it;
  if (va - it->first >= it->second) return false;
  out->base = it->first;
  out->size = it->second;
  return true;
}

bool GuestAllocator::Find(uint32_t va, Region* out) const {
  std::lock_guard<std::recursive_mutex> l(mu_);
  return FindLocked(va, out);
}

void GuestAllocator::FreeGap(uint32_t va, uint32_t* base, uint32_t* size) const {
  std::lock_guard<std::recursive_mutex> l(mu_);
  uint32_t start = va & ~(kPage - 1);
  auto next = regions_.upper_bound(start);
  uint32_t end = next == regions_.end() ? hi_ : next->first;
  *base = start;
  *size = end > start ? end - start : 0;
}

uint32_t GuestAllocator::Reserve(uint32_t va, uint32_t size, bool top_down) {
  std::lock_guard<std::recursive_mutex> l(mu_);
  if (!size) return 0;
  size = RoundUp(size, kPage);
  auto fits = [&](uint32_t base) {
    if (base < lo_ || uint64_t(base) + size > hi_) return false;
    auto it = regions_.lower_bound(base);
    if (it != regions_.end() && it->first < base + size) return false;
    if (it != regions_.begin()) {
      auto p = std::prev(it);
      if (uint64_t(p->first) + p->second > base) return false;
    }
    return true;
  };
  if (va) {
    va &= ~(kGranularity - 1);
    if (!fits(va)) return 0;
    regions_[va] = size;
    return va;
  }
  // Walk the gaps between reservations.
  if (!top_down) {
    uint32_t cand = lo_;
    for (auto it = regions_.begin(); it != regions_.end(); ++it) {
      if (uint64_t(it->first) + it->second <= cand) continue;
      if (it->first >= cand && it->first - cand >= size) break;
      cand = RoundUp(it->first + it->second, kGranularity);
    }
    if (!fits(cand)) return 0;
    regions_[cand] = size;
    return cand;
  }
  uint32_t top = hi_;
  for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
    uint32_t end = it->first + it->second;
    if (end > top) {
      top = std::min(top, it->first);
      continue;
    }
    if (top - end >= size) break;
    top = it->first;
  }
  if (top < size) return 0;
  uint32_t cand = (top - size) & ~(kGranularity - 1);
  if (!fits(cand)) return 0;
  regions_[cand] = size;
  return cand;
}

bool GuestAllocator::Commit(uint32_t va, uint32_t size, Prot prot) {
  std::lock_guard<std::recursive_mutex> l(mu_);
  Region r;
  if (!FindLocked(va, &r) || uint64_t(va) + size > uint64_t(r.base) + r.size)
    return false;
  // Commit only pages not already committed, so existing data survives
  // (MEM_COMMIT on committed pages is a no-op apart from protection).
  uint32_t s = va & ~(kPage - 1), e = RoundUp(va + size, kPage);
  for (uint32_t p = s; p < e; p += kPage) {
    if (mem_->IsCommitted(p))
      mem_->Protect(p, kPage, prot);
    else
      mem_->Commit(p, kPage, prot);
  }
  return true;
}

bool GuestAllocator::Decommit(uint32_t va, uint32_t size) {
  std::lock_guard<std::recursive_mutex> l(mu_);
  Region r;
  if (!FindLocked(va, &r)) return false;
  mem_->Decommit(va, size);
  return true;
}

bool GuestAllocator::Release(uint32_t base, bool zero) {
  std::lock_guard<std::recursive_mutex> l(mu_);
  auto it = regions_.find(base);
  if (it == regions_.end()) return false;
  mem_->Decommit(base, it->second, zero);
  regions_.erase(it);
  return true;
}

// ---- GuestHeap -------------------------------------------------------------

namespace {
constexpr uint32_t kAlign = 16;
constexpr uint32_t kDefaultSegment = 16u << 20;
}  // namespace

GuestHeap::GuestHeap(GuestAllocator& vm, uint32_t initial_size,
                     uint32_t max_size)
    : vm_(vm), max_size_(max_size) {
  uint32_t first = max_size ? RoundUp(max_size, kPage)
                            : std::max(RoundUp(initial_size, kPage),
                                       kDefaultSegment);
  Grow(first);
  // Heap handles are guest pointers on Windows; use the first segment base.
  handle_ = segments_.empty() ? 0 : segments_.front().first;
}

GuestHeap::~GuestHeap() {
  for (auto& s : segments_) vm_.Release(s.first);
}

bool GuestHeap::Grow(uint32_t need) {
  uint32_t size = RoundUp(std::max(need, kDefaultSegment),
                          GuestAllocator::kGranularity);
  uint32_t base = vm_.Reserve(0, size);
  if (!base) return false;
  // Committing is cheap: host pages are only materialised when touched.
  vm_.Commit(base, size, Prot::RW);
  segments_.push_back({base, size});
  InsertFree(base, size);
  return true;
}

bool GuestHeap::IsSegmentStart(uint32_t addr) const {
  for (const auto& s : segments_)
    if (s.first == addr) return true;
  return false;
}

void GuestHeap::InsertFree(uint32_t addr, uint32_t size) {
  free_[addr] = size;
  free_by_size_.emplace(size, addr);
}

void GuestHeap::EraseFree(uint32_t addr) {
  auto it = free_.find(addr);
  auto range = free_by_size_.equal_range(it->second);
  for (auto r = range.first; r != range.second; ++r)
    if (r->second == addr) {
      free_by_size_.erase(r);
      break;
    }
  free_.erase(it);
}

uint32_t GuestHeap::Alloc(uint32_t size, bool zero) {
  std::lock_guard<std::mutex> l(mu_);
  uint32_t req = size;
  size = RoundUp(std::max<uint32_t>(size, 1), kAlign);
  auto it = free_by_size_.lower_bound(size);
  if (it == free_by_size_.end()) {
    if (max_size_ || !Grow(size)) return 0;
    it = free_by_size_.lower_bound(size);
  }
  uint32_t addr = it->second, have = it->first;
  EraseFree(addr);
  if (have - size >= kAlign)
    InsertFree(addr + size, have - size);
  else
    size = have;
  used_[addr] = {size, req};
  if (zero) std::memset(vm_.mem().ptr(addr), 0, size);
  return addr;
}

bool GuestHeap::Free(uint32_t p) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = used_.find(p);
  if (it == used_.end()) return false;
  uint32_t addr = p, size = it->second.size;
  used_.erase(it);
  // Coalesce with the following free block, unless it starts a segment.
  auto nx = free_.find(addr + size);
  if (nx != free_.end() && !IsSegmentStart(nx->first)) {
    uint32_t s = nx->second;
    EraseFree(nx->first);
    size += s;
  }
  // And with the preceding one.
  auto pv = free_.lower_bound(addr);
  if (pv != free_.begin()) {
    --pv;
    if (pv->first + pv->second == addr && !IsSegmentStart(addr)) {
      uint32_t a = pv->first, s = pv->second;
      EraseFree(a);
      addr = a;
      size += s;
    }
  }
  InsertFree(addr, size);
  return true;
}

uint32_t GuestHeap::ReAlloc(uint32_t p, uint32_t size, bool zero,
                            bool in_place_only) {
  uint32_t old_req, old_size;
  {
    std::lock_guard<std::mutex> l(mu_);
    auto it = used_.find(p);
    if (it == used_.end()) return 0;
    old_req = it->second.requested;
    old_size = it->second.size;
    uint32_t want = RoundUp(std::max<uint32_t>(size, 1), kAlign);
    if (want <= old_size) {  // shrink (keep the block) or same
      it->second.requested = size;
      return p;
    }
    // Try to grow into a following free block.
    auto nx = free_.find(p + old_size);
    if (nx != free_.end() && !IsSegmentStart(nx->first) &&
        old_size + nx->second >= want) {
      uint32_t total = old_size + nx->second;
      EraseFree(nx->first);
      if (total - want >= kAlign) {
        InsertFree(p + want, total - want);
        total = want;
      }
      it->second = {total, size};
      if (zero)
        std::memset(vm_.mem().ptr(p + old_req), 0, total - old_req);
      return p;
    }
  }
  if (in_place_only) return 0;
  uint32_t n = Alloc(size, zero);
  if (!n) return 0;
  std::memcpy(vm_.mem().ptr(n), vm_.mem().ptr(p), std::min(old_req, size));
  Free(p);
  return n;
}

uint32_t GuestHeap::Size(uint32_t p) const {
  std::lock_guard<std::mutex> l(mu_);
  auto it = used_.find(p);
  return it == used_.end() ? UINT32_MAX : it->second.requested;
}

}  // namespace recomp
