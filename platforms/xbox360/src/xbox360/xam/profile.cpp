// xam: profile settings (XamUserReadProfileSettings and the XGI app's
// message that reads them).
//
// A setting id is its type (bits 28-31), its size (16-27) and its number.
// The local profile here has stored no settings: each one it knows reads
// as NO_VALUE, or DEFAULT where Xenia gives the system's default; one it
// does not know is fatal, to be added with its evidence.

#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/xam/msg.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorInvalidParameter = 0x57, kErrorInsufficientBuffer = 0x7A, kErrorIoPending = 0x3E5,
                   kErrorFunctionFailed = 0x65B, kNotLoggedOn = 0x80070525;
// The dashboard's title id: its settings are the profile's own.
constexpr uint32_t kDashboard = 0xFFFE07D1;
// XUSER_DATA types with data after the settings (a string, bytes).
constexpr uint32_t kTypeUnicode = 4, kTypeBinary = 6;
// XUSER_PROFILE_SOURCE.
constexpr uint32_t kSourceNoValue = 0, kSourceDefault = 1;
constexpr uint32_t kSettingSize = 0x28;

struct Known {
  uint32_t id, source, value;
};
// The profile's own settings the runtime knows (title kDashboard).
constexpr Known kProfileSettings[] = {
    {0x10040003, kSourceDefault, 3},   // controller vibration on (Xenia's default)
    {0x1004000C, kSourceNoValue, 0},   // voice muted
    {0x1004000D, kSourceNoValue, 0},   // voice through the speakers
    {0x1004000E, kSourceNoValue, 0},   // voice volume
};
// A title's own settings (title specific 1-3, 1000 bytes each).
bool TitleSpecific(uint32_t id) { return id == 0x63E83FFF || id == 0x63E83FFE || id == 0x63E83FFD; }

uint32_t Type(uint32_t id) { return id >> 28; }
uint32_t Extra(uint32_t id) { return Type(id) == kTypeUnicode || Type(id) == kTypeBinary ? id >> 16 & 0xFFF : 0; }

const Known* Find(uint32_t title, uint32_t id) {
  if (title == kDashboard)
    for (const Known& k : kProfileSettings)
      if (k.id == id) return &k;
  static constexpr Known kNone{0, kSourceNoValue, 0};
  return title != kDashboard && TitleSpecific(id) ? &kNone : nullptr;
}

}  // namespace

// The XGI app's read {title, slot, XUID count, *XUIDs, count, *ids, buffer
// size, results, flags}: XUSER_READ_PROFILE_SETTING_RESULT (the settings'
// count and their address) then 0x28 bytes per setting and user (source,
// the slot or XUID at 8, the id at 0x10, XUSER_DATA at 0x18: its type,
// then at 0x20 the value or a size and an address into the bytes after
// the settings).
XMSG(kXgiApp, 0xB000C, XgiReadProfileSettings) {
  uint32_t title = Rd32(p1), slot = Rd32(p1 + 4), xuids = Rd32(p1 + 12), count = Rd32(p1 + 16),
           ids = Rd32(p1 + 20), results = Rd32(p1 + 28);
  uint32_t users = xuids ? Rd32(p1 + 8) : 1;
  if (!title) title = ExecutableTitleId();
  if (!xuids && !SignedInProfile(slot)) return kNotLoggedOn;
  uint32_t settings = results + 8, extra = settings + kSettingSize * count * users;
  Wr32(results, count * users);
  Wr32(results + 4, settings);
  for (uint32_t u = 0; u < users; u++)
    for (uint32_t k = 0; k < count; k++) {
      uint32_t id = Rd32(ids + 4 * k), s = settings + kSettingSize * (u * count + k);
      const Known* known = Find(title, id);
      if (!known) ppc::Fatal(&c, "profile setting %x of title %x is not known here", id, title);
      for (uint32_t b = 0; b < kSettingSize; b += 4) Wr32(s + b, 0);
      Wr32(s, known->source);
      if (xuids)
        Wr64(s + 8, Rd64(xuids + 8 * u));
      else
        Wr32(s + 8, slot);
      Wr32(s + 0x10, id);
      Wr8(s + 0x18, uint8_t(Type(id)));
      if (Extra(id)) {
        Wr32(s + 0x20, 0);  // no bytes: nothing stored
        Wr32(s + 0x24, extra);
        extra += Extra(id);
      } else {
        Wr32(s + 0x20, known->value);
      }
    }
  return 0;
}

// XamUserReadProfileSettings(title, slot, XUID count, *XUIDs, count, *ids,
// *size, results, *overlapped): xam's own checks and sizing (0x28 bytes
// per setting and user, a string's or bytes' size more, 8 for the
// header), then the XGI app's read. Win32 codes; ERROR_IO_PENDING with an
// XOVERLAPPED.
HLE(kXam, 537, XamUserReadProfileSettings) {
  uint32_t title = Arg(c, 0), slot = Arg(c, 1), xuid_count = Arg(c, 2), xuids = Arg(c, 3), count = Arg(c, 4),
           ids = Arg(c, 5), size_ptr = Arg(c, 6), results = Arg(c, 7), overlapped = Arg(c, 8);
  if (!xuids) xuid_count = 1;
  if (!count || count > 32 || !ids || !size_ptr) return kErrorInvalidParameter;
  uint32_t size = Rd32(size_ptr);
  if (size && !results) return kErrorInvalidParameter;
  uint32_t per_user = 0;
  for (uint32_t k = 0; k < count; k++) per_user += kSettingSize + Extra(Rd32(ids + 4 * k));
  uint32_t needed = per_user * xuid_count + 8;
  if (size < needed) {
    if (!size) Wr32(size_ptr, needed);
    return kErrorInsufficientBuffer;
  }
  uint32_t request = SystemAlloc(0x28);
  const uint32_t fields[9] = {title, slot, xuid_count, xuids, count, ids, size, results, 0};
  for (uint32_t k = 0; k < 9; k++) Wr32(request + 4 * k, fields[k]);
  StartXMsgRequest(c, kXgiApp, 0xB000C, overlapped, request, 0x28);
  SystemFree(request);
  if (overlapped) return kErrorIoPending;
  return LastError() ? kErrorFunctionFailed : 0;
}

}  // namespace recomp::xbox360
