#pragma once
// Guest-visible COM objects backed by host C++ objects.
//
// A guest interface pointer points at a small block in guest memory:
//   +0 vtable (guest array of thunk addresses, one per method)
//   +4 magic 'RCOM'
//   +8 object id (host table key)
//   +12 slot in the lock-free table As reads
// Methods are HLE handlers named "Interface::Method" (in the DLL that
// provides the interface), so an unimplemented method fails by name.
// Each host object has exactly one guest pointer; QueryInterface for the
// same interface returns it again, other interfaces are separate objects.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "runtime/hle/ddraw_types.h"

namespace recomp::com {

struct Object {
  virtual ~Object() = default;
  uint32_t guest = 0;  // the guest interface pointer
  std::atomic<uint32_t> refs{1};
  uint32_t tag = 0;    // the class, for Tagged classes (As)
};

// A class tag from its name (FNV-1a), for Tagged.
constexpr uint32_t TagOf(const char* name) {
  uint32_t h = 2166136261u;
  while (*name) h = (h ^ uint8_t(*name++)) * 16777619u;
  return h;
}

// A base for a final class that As recognises by its tag instead of a
// dynamic_cast (which compares type names across the runtime's libraries):
// struct Surface final : com::Tagged<com::TagOf("d3d9.Surface")>.
template <uint32_t Tag>
struct Tagged : Object {
  static constexpr uint32_t kTag = Tag;
  Tagged() { tag = Tag; }
};

// Builds (once per name) the guest vtable for `iface` with the given
// method names, in vtable order.
uint32_t Vtable(const char* dll, const char* iface,
                const std::vector<const char*>& methods);

// Gives `obj` its guest pointer with `vtable`. Returns the pointer.
uint32_t Publish(std::shared_ptr<Object> obj, uint32_t vtable);

// Host object behind a guest interface pointer (nullptr if not ours).
std::shared_ptr<Object> Lookup(uint32_t guest);
// The same without a lock or a reference: through the block's slot. For
// lookups on every draw; the object stays valid while the guest holds a
// reference.
Object* Find(uint32_t guest);

// Bumped whenever an object leaves the table (its guest block can then be
// reused): what As remembers is valid while the generation is unchanged.
extern std::atomic<uint32_t> g_generation;

// The host object behind `guest` as a T (nullptr if not ours or not a T).
// Each thread remembers its last answer per T: calls ask for the same
// device over and over, and even the slot lookup is then skipped. A null
// pointer does not replace what is remembered. A Tagged T is told by its
// tag; other classes by a dynamic_cast.
template <typename T>
T* As(uint32_t guest) {
  if (!guest) return nullptr;
  struct Last {
    uint32_t guest = 0, generation = ~0u;
    T* object = nullptr;
  };
  thread_local Last last;
  const uint32_t gen = g_generation.load(std::memory_order_acquire);
  if (last.guest == guest && last.generation == gen) return last.object;
  Object* o = Find(guest);
  T* t;
  if constexpr (requires { T::kTag; }) {
    static_assert(std::is_final_v<T>, "a Tagged class must be final");
    t = o && o->tag == T::kTag ? static_cast<T*>(o) : nullptr;
  } else {
    t = dynamic_cast<T*>(o);
  }
  last = {guest, gen, t};
  return t;
}

uint32_t AddRef(uint32_t guest);
// Drops a reference; destroys the object at zero. Returns the new count.
uint32_t Release(uint32_t guest);

// The same through the host object, for a caller that holds a reference
// to it already (a binding): an atomic count, no lock and no lookup, as
// DXVK's ComObject counts. Only a release of the last reference goes
// through Release.
inline void AddRefHeld(Object* o) { o->refs.fetch_add(1, std::memory_order_relaxed); }
void ReleaseHeld(Object* o);

bool GuidEq(uint32_t guest_guid, const guest::GUID& g);
guest::GUID ReadGuid(uint32_t va);

// In-process classes for CoCreateInstance, registered by the HLE module
// that provides them. `create` gets the requested IID (a guest GUID*),
// writes the interface pointer (or 0) to *out and returns the HRESULT.
using CreateFn = uint32_t (*)(uint32_t iid, uint32_t* out);
void RegisterCoClass(const guest::GUID& clsid, CreateFn create);
// The class for a guest CLSID*; null if none is registered.
CreateFn FindCoClass(uint32_t clsid);
// Registers a class at load time (a static of the providing module).
struct CoClassRegistrar {
  CoClassRegistrar(const guest::GUID& clsid, CreateFn create) {
    RegisterCoClass(clsid, create);
  }
};

// BSTRs (oleaut32 SysAllocString/SysFreeString): the byte length, then
// the characters and a NUL; the pointer is to the characters.
uint32_t AllocBstr(const std::u16string& s);
void FreeBstr(uint32_t bstr);

constexpr uint32_t kS_OK = 0, kE_NOINTERFACE = 0x80004002;

}  // namespace recomp::com
