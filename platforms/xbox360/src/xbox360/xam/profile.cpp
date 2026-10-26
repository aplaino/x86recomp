// xam: profile settings (XamUserReadProfileSettings,
// XamUserWriteProfileSettings and the XGI app's messages behind them).
//
// A setting id is its type (bits 28-31), its size (16-27) and its number.
// What a title writes is kept, as the console keeps it in the profile: the
// profile's own settings (numbers below 0x58) for the profile, a title's
// own (title specific 1-3) for that title, saved in the profile's
// directory. A setting not written reads as the system's default where
// Xenia gives one, as NO_VALUE where the runtime knows the setting, and is
// fatal otherwise, to be added with its evidence.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/xam/msg.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorInvalidParameter = 0x57, kErrorInsufficientBuffer = 0x7A, kErrorIoPending = 0x3E5,
                   kErrorFunctionFailed = 0x65B, kEInvalidArg = 0x80070057, kNotLoggedOn = 0x80070525;
// The dashboard's title id: its settings are the profile's own.
constexpr uint32_t kDashboard = 0xFFFE07D1;
// XUSER_DATA types with data after the settings (a string, bytes).
constexpr uint32_t kTypeUnicode = 4, kTypeBinary = 6;
// XUSER_PROFILE_SOURCE.
constexpr uint32_t kSourceNoValue = 0, kSourceDefault = 1, kSourceTitle = 2;
constexpr uint32_t kSettingSize = 0x28;

struct Known {
  uint32_t id, source, value;
};
// The profile's own settings the runtime knows, as a profile that has
// written none reads them.
constexpr Known kProfileSettings[] = {
    {0x10040003, kSourceDefault, 3},   // controller vibration on (Xenia's default)
    {0x1004000C, kSourceNoValue, 0},   // voice muted
    {0x1004000D, kSourceNoValue, 0},   // voice through the speakers
    {0x1004000E, kSourceNoValue, 0},   // voice volume
};
// A title's own settings (title specific 1-3, 1000 bytes each).
bool TitleSpecific(uint32_t id) { return id == 0x63E83FFF || id == 0x63E83FFE || id == 0x63E83FFD; }
bool ProfileOwn(uint32_t id) { return (id & 0x3FFF) < 0x58; }

uint32_t Type(uint32_t id) { return id >> 28; }
uint32_t Extra(uint32_t id) { return Type(id) == kTypeUnicode || Type(id) == kTypeBinary ? id >> 16 & 0xFFF : 0; }

uint32_t Title(uint32_t title) { return title ? title : ExecutableTitleId(); }
// Whose setting it is: the profile's (kept under the dashboard) or the
// title's.
uint32_t Owner(uint32_t title, uint32_t id) { return ProfileOwn(id) ? kDashboard : title; }

// ---- the store --------------------------------------------------------------
// (owner, id) -> the setting's bytes: a string's or bytes' own, or the
// 8 bytes XUSER_DATA holds a number in. One file each, <owner>/<id> in the
// profile's directory, read when an owner's settings are first needed.
std::mutex g_lock;
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_store;
std::set<uint32_t> g_loaded;

std::string Hex(uint32_t v) {
  char s[9];
  std::snprintf(s, sizeof s, "%08X", v);
  return s;
}

void Load(const Profile& p, uint32_t owner) {
  if (!g_loaded.insert(owner).second) return;
  std::error_code ec;
  for (const auto& f : std::filesystem::directory_iterator(p.directory / Hex(owner), ec)) {
    uint32_t id = uint32_t(std::stoul(f.path().filename().string(), nullptr, 16));
    std::ifstream in(f.path(), std::ios::binary);
    g_store[{owner, id}].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
}

const std::vector<uint8_t>* Stored(const Profile& p, uint32_t owner, uint32_t id) {
  Load(p, owner);
  auto it = g_store.find({owner, id});
  return it == g_store.end() ? nullptr : &it->second;
}

void Store(ppc::Ctx& c, const Profile& p, uint32_t owner, uint32_t id, std::vector<uint8_t> bytes) {
  Load(p, owner);
  std::filesystem::path dir = p.directory / Hex(owner);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(dir / Hex(id), std::ios::binary | std::ios::trunc);
  if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  if (!out) ppc::Fatal(&c, "profile setting %x: cannot write %s", id, (dir / Hex(id)).c_str());
  g_store[{owner, id}] = std::move(bytes);
}

}  // namespace

// The XGI app's read {title, slot, XUID count, *XUIDs, count, *ids, buffer
// size, results, flags}: XUSER_READ_PROFILE_SETTING_RESULT (the settings'
// count and their address) then 0x28 bytes per setting and user (source,
// the slot or XUID at 8, the id at 0x10, XUSER_DATA at 0x18: its type,
// then at 0x20 the value or a size and an address into the bytes after
// the settings). Other users' settings (by XUID) are the signed-in
// profile's only.
XMSG(kXgiApp, 0xB000C, XgiReadProfileSettings) {
  uint32_t title = Title(Rd32(p1)), slot = Rd32(p1 + 4), xuids = Rd32(p1 + 12), count = Rd32(p1 + 16),
           ids = Rd32(p1 + 20), results = Rd32(p1 + 28);
  uint32_t users = xuids ? Rd32(p1 + 8) : 1;
  const Profile* profile = SignedInProfile(slot);
  if (!profile) return kNotLoggedOn;
  for (uint32_t u = 0; u < users && xuids; u++)
    if (Rd64(xuids + 8 * u) != profile->xuid) ppc::Fatal(&c, "profile settings of another user's XUID");
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t settings = results + 8, extra = settings + kSettingSize * count * users;
  Wr32(results, count * users);
  Wr32(results + 4, settings);
  for (uint32_t u = 0; u < users; u++)
    for (uint32_t k = 0; k < count; k++) {
      uint32_t id = Rd32(ids + 4 * k), s = settings + kSettingSize * (u * count + k);
      for (uint32_t b = 0; b < kSettingSize; b += 4) Wr32(s + b, 0);
      if (xuids)
        Wr64(s + 8, Rd64(xuids + 8 * u));
      else
        Wr32(s + 8, slot);
      Wr32(s + 0x10, id);
      Wr8(s + 0x18, uint8_t(Type(id)));
      const std::vector<uint8_t>* stored = Stored(*profile, Owner(title, id), id);
      if (Extra(id)) {
        uint32_t n = stored ? uint32_t(stored->size()) : 0;
        for (uint32_t b = 0; b < n; b++) Wr8(extra + b, (*stored)[b]);
        Wr32(s + 0x20, n);
        Wr32(s + 0x24, extra);
        extra += Extra(id);
      }
      if (stored) {
        Wr32(s, kSourceTitle);
        if (!Extra(id))
          for (uint32_t b = 0; b < 8 && b < stored->size(); b++) Wr8(s + 0x20 + b, (*stored)[b]);
        continue;
      }
      const Known* known = nullptr;
      for (const Known& k : kProfileSettings)
        if (k.id == id && Owner(title, id) == kDashboard) known = &k;
      if (!known && !(TitleSpecific(id) && title != kDashboard))
        ppc::Fatal(&c, "profile setting %x of title %x is not known here", id, title);
      Wr32(s, known ? known->source : kSourceNoValue);
      if (known && !Extra(id)) Wr32(s + 0x20, known->value);
    }
  return 0;
}

// The XGI app's write {?, title, slot, count, settings}: each setting's
// XUSER_DATA kept as written. A string's or bytes' size may be 0, with an
// address or without; their bytes are copied only when there are any.
XMSG(kXgiApp, 0xB000D, XgiWriteProfileSettings) {
  uint32_t title = Title(Rd32(p1 + 4)), slot = Rd32(p1 + 8), count = Rd32(p1 + 12), settings = Rd32(p1 + 16);
  const Profile* profile = SignedInProfile(slot);
  if (!profile) return kNotLoggedOn;
  std::lock_guard<std::mutex> g(g_lock);
  for (uint32_t k = 0; k < count; k++) {
    uint32_t s = settings + kSettingSize * k, id = Rd32(s + 0x10);
    std::vector<uint8_t> bytes;
    if (Extra(id)) {
      uint32_t size = Rd32(s + 0x20), ptr = Rd32(s + 0x24);
      if (size > Extra(id) || (size && !ptr)) return kEInvalidArg;
      if (size) bytes.assign(Mem() + ptr, Mem() + ptr + size);
    } else {
      bytes.assign(Mem() + s + 0x20, Mem() + s + 0x28);
    }
    Store(c, *profile, Owner(title, id), id, std::move(bytes));
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

// XamUserWriteProfileSettings(title, slot, count, settings, *overlapped):
// xam refuses no settings and any id a title may not write (only the
// profile's own and, for a title, its own), then sends the XGI app's
// write.
HLE(kXam, 538, XamUserWriteProfileSettings) {
  uint32_t title = Arg(c, 0), slot = Arg(c, 1), count = Arg(c, 2), settings = Arg(c, 3), overlapped = Arg(c, 4);
  if (!count || !settings) return kErrorInvalidParameter;
  for (uint32_t k = 0; k < count; k++) {
    uint32_t id = Rd32(settings + kSettingSize * k + 0x10);
    if (!ProfileOwn(id) && !(TitleSpecific(id) && title != kDashboard)) return kErrorInvalidParameter;
  }
  uint32_t request = SystemAlloc(0x14);
  const uint32_t fields[5] = {0, title, slot, count, settings};
  for (uint32_t k = 0; k < 5; k++) Wr32(request + 4 * k, fields[k]);
  StartXMsgRequest(c, kXgiApp, 0xB000D, overlapped, request, 0x14);
  SystemFree(request);
  if (overlapped) return kErrorIoPending;
  return LastError() ? kErrorFunctionFailed : 0;
}

}  // namespace recomp::xbox360
