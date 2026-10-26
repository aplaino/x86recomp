// xam: enumerators (enumerate.h).

#include "xbox360/xam/enumerate.h"

#include <cstring>
#include <map>
#include <mutex>

#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/xam/msg.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorInvalidHandle = 6, kHeaderSize = 0x18;

std::mutex g_lock;
std::map<uint32_t, void (*)(uint32_t)> g_enumerators;  // object -> its app's close

void DestroyEnumerator(uint32_t obj) {
  void (*close)(uint32_t);
  {
    std::lock_guard<std::mutex> g(g_lock);
    close = g_enumerators[obj];
    g_enumerators.erase(obj);
  }
  close(obj + kHeaderSize);
  SystemFree(obj);
}

}  // namespace

// The header's app, message and items per call, as xam fills them (its
// close message, caller and flags stay 0 here).
uint32_t CreateEnumerator(uint32_t app, uint32_t message, uint32_t items, uint32_t state_size,
                          void (*close)(uint32_t state), uint32_t& state) {
  uint32_t obj = SystemAlloc(kHeaderSize + state_size);
  std::memset(Mem() + obj, 0, kHeaderSize + state_size);
  Wr32(obj, app);
  Wr32(obj + 4, message);
  Wr32(obj + 0x10, items);
  {
    std::lock_guard<std::mutex> g(g_lock);
    g_enumerators[obj] = close;
  }
  Own(obj, DestroyEnumerator);
  state = obj + kHeaderSize;
  return NewHandle(obj);
}

// XamEnumerate(handle, flags, buffer, size, *items returned, *overlapped):
// a Win32 code; ERROR_IO_PENDING with an XOVERLAPPED, whose length is the
// items returned.
HLE(kXam, 592, XamEnumerate) {
  uint32_t flags = Arg(c, 1), buffer = Arg(c, 2), size = Arg(c, 3), returned = Arg(c, 4), ov = Arg(c, 5);
  uint32_t obj = HandleObject(Arg(c, 0));
  {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_enumerators.count(obj)) return kErrorInvalidHandle;
  }
  Reference(obj);
  uint32_t request = SystemAlloc(0x20);
  const uint32_t fields[8] = {0, flags | 0x80000000, obj + kHeaderSize, buffer, size, Rd32(obj + 0x10), returned, ov};
  for (uint32_t k = 0; k < 8; k++) Wr32(request + 4 * k, fields[k]);
  uint32_t hr = ProcessXMsgRequest(c, Rd32(obj), Rd32(obj + 4), ov, request, 0x20);
  SystemFree(request);
  Dereference(obj);
  return Win32FromHResult(hr);
}

}  // namespace recomp::xbox360
