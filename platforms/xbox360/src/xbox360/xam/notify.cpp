// xam: notifications (XamNotifyCreateListener, XNotifyGetNext), as xam's
// own code keeps them: an id is an area (bits 25-30), a version (16-24)
// and a number; a listener takes the areas of its mask up to its highest
// version, holds each pending id once with its latest value, and is a
// synchronization event, signalled while any is pending. A new listener
// gets the notifications the system still holds pending (xam copies them
// from its own listener).

#include <algorithm>
#include <map>
#include <mutex>

#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kMaxVersion = 10, kListenerSize = 0x30;

// The system's pending notifications when a title starts, as Xenia gives
// a title's first listeners (console check): the system UI is not showing,
// the profiles signed in, Live is disconnected (XONLINE_S_LOGON_DISCONNECTED)
// with no network link.
struct Pending {
  uint32_t id, value;
};
uint32_t SignedInMask() {
  uint32_t mask = 0;
  for (uint32_t slot = 0; slot < kUserSlots; slot++)
    if (SignedInProfile(slot)) mask |= 1u << slot;
  return mask;
}

struct Listener {
  uint64_t areas;
  uint32_t version;
  std::map<uint32_t, Pending> pending;  // by Key
};
std::mutex g_lock;
std::map<uint32_t, Listener> g_listeners;  // by object

uint32_t Area(uint32_t id) { return id >> 25 & 0x3F; }
uint32_t Version(uint32_t id) { return id >> 16 & 0x1FF; }
// Ids in the order xam's listener walks them: by area, then number.
uint32_t Key(uint32_t id) { return id & 0x7E00FFFF; }

void Post(uint32_t obj, Listener& l, uint32_t id, uint32_t value) {
  if (!(l.areas >> Area(id) & 1) || Version(id) > l.version) return;
  l.pending[Key(id)] = {id, value};
  SetSignal(obj, 1);
}

void DestroyListener(uint32_t obj) {
  {
    std::lock_guard<std::mutex> g(g_lock);
    g_listeners.erase(obj);
  }
  SystemFree(obj);
}

}  // namespace

// XamNotifyCreateListener(areas, highest version): a handle to a new
// listener.
HLE(kXam, 650, XamNotifyCreateListener) {
  uint64_t areas = Arg64(c, 0);
  uint32_t version = std::min(Arg(c, 1), kMaxVersion);
  uint32_t obj = SystemAlloc(kListenerSize);
  InitObject(obj, kSynchronizationEvent, 0);
  Own(obj, DestroyListener);
  std::lock_guard<std::mutex> g(g_lock);
  Listener& l = g_listeners[obj] = Listener{areas, version, {}};
  const Pending system[] = {{0x00000009, 0}, {0x0000000A, SignedInMask()}, {0x02000001, 0x001510F1},
                            {0x02000003, 0}};
  for (const Pending& p : system) Post(obj, l, p.id, p.value);
  return NewHandle(obj);
}

// XNotifyGetNext(handle, id or 0 for any, *id, *value): TRUE and the first
// pending notification (of that id), which is no longer pending.
HLE(kXam, 651, XNotifyGetNext) {
  uint32_t match = Arg(c, 1), id_ptr = Arg(c, 2), value_ptr = Arg(c, 3);
  if (id_ptr) Wr32(id_ptr, 0);
  if (value_ptr) Wr32(value_ptr, 0);
  uint32_t obj = HandleObject(Arg(c, 0));
  std::lock_guard<std::mutex> g(g_lock);
  auto it = g_listeners.find(obj);
  if (it == g_listeners.end()) return 0;
  auto& pending = it->second.pending;
  auto p = match ? pending.find(Key(match)) : pending.begin();
  if (p == pending.end()) return 0;
  if (id_ptr) Wr32(id_ptr, p->second.id);
  if (value_ptr) Wr32(value_ptr, p->second.value);
  pending.erase(p);
  if (pending.empty()) SetSignal(obj, 0);
  return 1;
}

// XNotifyPositionUI(position): where the system's notifications show on
// screen; none show here.
HLE(kXam, 652, XNotifyPositionUI) { return 0; }

}  // namespace recomp::xbox360
