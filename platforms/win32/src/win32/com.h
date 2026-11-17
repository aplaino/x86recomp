#pragma once
// Guest-visible COM objects backed by host C++ objects.
//
// A guest interface pointer points at a small block in guest memory:
//   +0 vtable (guest array of thunk addresses, one per method)
//   then magic 'RCOM' (+4, or +8 in x64 guests, after the 8-byte vtable),
//   and at +12 the object's index in the lookup table (com.cpp)
// Pointers and vtable slots are the guest's pointer size.
// Methods are HLE handlers named "Interface::Method" (in the DLL that
// provides the interface), so an unimplemented method fails by name.
// Each host object has exactly one guest pointer; QueryInterface for the
// same interface returns it again, other interfaces are separate objects.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "runtime/guest_memory.h"
#include "win32/ddraw_types.h"

namespace recomp::com {

struct Object : std::enable_shared_from_this<Object> {
  virtual ~Object() = default;
  gaddr guest = 0;  // the guest interface pointer
  uint32_t refs = 1;
  uint32_t slot = ~0u;  // its entry in the lookup table (com.cpp)
};

// Builds (once per name) the guest vtable for `iface` with the given
// method names, in vtable order.
gaddr Vtable(const char* dll, const char* iface, const std::vector<const char*>& methods);

// Gives `obj` its guest pointer with `vtable`. Returns the pointer.
gaddr Publish(std::shared_ptr<Object> obj, gaddr vtable);

// Host object behind a guest interface pointer (nullptr if not ours).
// Without a lock: guest code calls through its objects from many threads
// at once (deferred contexts recording in parallel).
Object* Find(gaddr guest);
std::shared_ptr<Object> Lookup(gaddr guest);

template <typename T>
T* As(gaddr guest) {
  return dynamic_cast<T*>(Find(guest));
}

uint32_t AddRef(gaddr guest);
// Drops a reference; destroys the object at zero. Returns the new count.
uint32_t Release(gaddr guest);

bool GuidEq(gaddr guest_guid, const guest::GUID& g);
guest::GUID ReadGuid(gaddr va);

// In-process classes for CoCreateInstance, registered by the HLE module
// that provides them. `create` gets the requested IID (a guest GUID*),
// writes the interface pointer (or 0) to *out and returns the HRESULT.
using CreateFn = uint32_t (*)(gaddr iid, gaddr* out);
void RegisterCoClass(const guest::GUID& clsid, CreateFn create);
// The class for a guest CLSID*; null if none is registered.
CreateFn FindCoClass(gaddr clsid);
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
