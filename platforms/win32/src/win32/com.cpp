#include "win32/com.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"

namespace recomp::com {

namespace {

constexpr uint32_t kMagic = 0x4D4F4352;  // 'RCOM'

std::recursive_mutex g_mu;
std::map<std::string, gaddr> g_vtables;
std::map<gaddr, std::shared_ptr<Object>> g_objects;  // by guest pointer, owning
std::vector<std::pair<guest::GUID, CreateFn>> g_classes;

// Lookups without the lock (Find): each object has a slot, whose index is
// in its guest block (+12, unused otherwise), holding the object and its
// guest pointer, so a stale block (released, its memory reused) finds
// nothing. Slots are in chunks that never move; they are taken and freed
// under g_mu.
constexpr uint32_t kChunkBits = 16, kChunk = 1u << kChunkBits, kChunks = 256;
struct Slot {
  std::atomic<Object*> obj{nullptr};
  std::atomic<gaddr> guest{0};
};
std::atomic<Slot*> g_chunks[kChunks];
uint32_t g_next_slot = 0;
std::vector<uint32_t> g_free_slots;

Slot* SlotAt(uint32_t i) {
  if (i >= kChunks * kChunk) return nullptr;
  Slot* chunk = g_chunks[i >> kChunkBits].load(std::memory_order_acquire);
  return chunk ? &chunk[i & (kChunk - 1)] : nullptr;
}

}  // namespace

gaddr Vtable(const char* dll, const char* iface, const std::vector<const char*>& methods) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = g_vtables.find(iface);
  if (it != g_vtables.end()) return it->second;
  const bool x64 = g_proc->x64;
  gaddr vt = HeapAllocProcess(methods.size() * (x64 ? 8 : 4), false);
  for (size_t i = 0; i < methods.size(); i++) {
    gaddr t = hle::ThunkFor(dll, std::string(iface) + "::" + methods[i]);
    if (x64)
      Wr64(vt + 8 * i, t);
    else
      Wr32(vt + 4 * i, uint32_t(t));
  }
  g_vtables[iface] = vt;
  return vt;
}

gaddr Publish(std::shared_ptr<Object> obj, gaddr vtable) {
  gaddr p = HeapAllocProcess(16, true);
  if (g_proc->x64) {
    Wr64(p, vtable);
    Wr32(p + 8, kMagic);
  } else {
    Wr32(p, uint32_t(vtable));
    Wr32(p + 4, kMagic);
    Wr32(p + 8, uint32_t(p));
  }
  obj->guest = p;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t i;
  if (!g_free_slots.empty()) {
    i = g_free_slots.back();
    g_free_slots.pop_back();
  } else {
    if (g_next_slot == kChunks * kChunk) FatalF("com: more than %u live objects", kChunks * kChunk);
    i = g_next_slot++;
    if (!(i & (kChunk - 1))) g_chunks[i >> kChunkBits].store(new Slot[kChunk], std::memory_order_release);
  }
  Slot* s = SlotAt(i);
  s->guest.store(p, std::memory_order_relaxed);
  s->obj.store(obj.get(), std::memory_order_release);
  obj->slot = i;
  Wr32(p + 12, i);
  g_objects[p] = std::move(obj);
  return p;
}

Object* Find(gaddr guest) {
  // Ours are 16-byte blocks: one page, or the next one committed too.
  if (!guest || !g_proc->mem.IsCommitted(guest) || !g_proc->mem.IsCommitted(guest + 15))
    return nullptr;
  if (Rd32(guest + (g_proc->x64 ? 8 : 4)) != kMagic) return nullptr;
  Slot* s = SlotAt(Rd32(guest + 12));
  if (!s) return nullptr;
  Object* o = s->obj.load(std::memory_order_acquire);
  return o && s->guest.load(std::memory_order_relaxed) == guest ? o : nullptr;
}

std::shared_ptr<Object> Lookup(gaddr guest) {
  Object* o = Find(guest);
  return o ? o->shared_from_this() : nullptr;
}

uint32_t AddRef(gaddr guest) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = g_objects.find(guest);
  if (it == g_objects.end()) FatalF("AddRef on unknown COM object %08llX", (unsigned long long)guest);
  return ++it->second->refs;
}

uint32_t Release(gaddr guest) {
  std::shared_ptr<Object> dead;
  uint32_t left;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    auto it = g_objects.find(guest);
    if (it == g_objects.end())
      FatalF("Release on unknown COM object %08llX", (unsigned long long)guest);
    left = --it->second->refs;
    if (!left) {
      dead = it->second;
      g_objects.erase(it);
      Slot* s = SlotAt(dead->slot);
      s->obj.store(nullptr, std::memory_order_release);
      s->guest.store(0, std::memory_order_relaxed);
      g_free_slots.push_back(dead->slot);
      // Poison the guest block so a use-after-release is caught.
      Wr64(guest, 0);
      Wr32(guest + 8, 0);
      HeapFreeProcess(guest);
    }
  }
  return left;  // `dead` destroys the host object outside the lock
}

guest::GUID ReadGuid(gaddr va) {
  guest::GUID g;
  std::memcpy(&g, GPtr(va), sizeof g);
  return g;
}

bool GuidEq(gaddr guest_guid, const guest::GUID& g) {
  return guest_guid && std::memcmp(GPtr(guest_guid), &g, sizeof g) == 0;
}

void RegisterCoClass(const guest::GUID& clsid, CreateFn create) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_classes.emplace_back(clsid, create);
}

CreateFn FindCoClass(gaddr clsid) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (const auto& [g, create] : g_classes)
    if (GuidEq(clsid, g)) return create;
  return nullptr;
}

uint32_t AllocBstr(const std::u16string& s) {
  const uint32_t bytes = uint32_t(s.size() * 2);
  const uint32_t p = HeapAllocProcess(4 + bytes + 2, false);
  Wr32(p, bytes);
  std::memcpy(GPtr(p + 4), s.data(), bytes);
  Wr16(p + 4 + bytes, 0);
  return p + 4;
}

void FreeBstr(uint32_t bstr) {
  if (bstr) HeapFreeProcess(bstr - 4);
}

}  // namespace recomp::com
