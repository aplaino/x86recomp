// xam: content packages (XamContent*), as xam's own code handles them: its
// exports check their arguments and send xam's own app (0xFE) a request,
// whose handlers open, create, close and delete packages and list them.
//
// A package is the console's STFS file at
// Partition1\Content\<XUID>\<title id>\<content type>\<file name> (XUID 0
// for content no one user owns). Here it is a host directory there, and its
// header (the XCONTENT_DATA it was created with, then its creator's XUID)
// a file beside it, <file name>.header. Opening one mounts it as a device
// and links the title's root ("save", as save:) to it, until
// XamContentClose. XCONTENT_DATA (0x134 bytes): device id, content type,
// display name (128 UTF-16 characters), file name (42 characters; its
// first character with bit 7 set in what an enumeration returns for
// content no one user owns).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/fs.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/xam/enumerate.h"
#include "xbox360/xam/msg.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kErrorInvalidParameter = 0x57, kErrorInvalidName = 0x7B, kEInvalidArg = 0x80070057;
constexpr uint32_t kFileNotFound = 0x80070002, kPathNotFound = 0x80070003, kAccessDenied = 0x80070005,
                   kNoMoreFiles = 0x80070012, kSharingViolation = 0x80070020, kDupName = 0x80070034,
                   kBadName = 0x8007007B, kAlreadyExists = 0x800700B7, kDeviceNotConnected = 0x8007048F,
                   kNotLoggedOn = 0x80070525;

constexpr uint32_t kDataSize = 0x134, kDataDisplayName = 8, kDataFileName = 0x108, kFileNameSize = 42;
constexpr uint8_t kNoUser = 0x80;
// The flags of XamContentCreateEx's request that xam reads: the
// disposition (low 4 bits, CREATE_NEW..TRUNCATE_EXISTING), read-only.
constexpr uint32_t kReadOnly = 0x40;

// The one content device: the hard disk's data partition, device id 1 and
// XCONTENTDEVICETYPE_HDD as Xenia gives it, named as the dashboard names
// it (chosen). A title asks for it by id, or with 0 for any.
constexpr uint32_t kDeviceId = 1, kDeviceTypeHdd = 1;
constexpr const char16_t kDeviceName[] = u"Hard Drive";
constexpr const char* kPartition = "\\Device\\Harddisk0\\Partition1";

std::string ReadName(uint32_t va, uint32_t max) {
  std::string s;
  for (uint32_t k = 0; k < max && Rd8(va + k); k++) s += char(Rd8(va + k));
  return s;
}

std::string Hex(uint64_t v, int digits) {
  char s[17];
  std::snprintf(s, sizeof s, "%0*llX", digits, static_cast<unsigned long long>(v));
  return s;
}

// A root as XamContentCreateEx and XamContentClose take it: after a \??\ or
// \System??\ prefix, none of these characters.
bool ValidRoot(const std::string& root) {
  if (root.empty()) return false;
  size_t start = root.compare(0, 4, "\\??\\") == 0 ? 4 : root.compare(0, 10, "\\System??\\") == 0 ? 10 : 0;
  return root.find_first_of(":;<=>?@!\"#$%&'()*+,-./[\\]", start) == std::string::npos;
}
// A file name as XamContentCreateEx takes it: ASCII but for that bit.
bool ValidFileName(uint32_t name) {
  uint32_t k = Rd8(name) & 0x80 ? 1 : 0;
  for (; k < kFileNameSize && Rd8(name + k); k++)
    if (Rd8(name + k) >= 0x80) return false;
  return Rd8(name) != 0;
}
// The symbolic link a title's root names: \??\root: (xam's format).
std::string RootLink(ppc::Ctx& c, const std::string& root) {
  if (root.compare(0, 10, "\\System??\\") == 0)
    ppc::Fatal(&c, "content root %s: the system's namespace is not supported", root.c_str());
  return (root.compare(0, 4, "\\??\\") == 0 ? root : "\\??\\" + root) + ":";
}
std::string Lower(std::string s) {
  for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// Content types a title may create and delete (xam's list); a package of
// another type opens read-only.
bool TitleWritable(uint32_t type) {
  static constexpr uint32_t kTypes[] = {1, 3, 0x4000, 0x10000, 0x20000, 0x30000, 0x40000, 0x60000,
                                         0xA0000, 0xB0000, 0xE0000, 0xF0000};
  return (type & 0xFFF00000) || std::find(std::begin(kTypes), std::end(kTypes), type) != std::end(kTypes);
}

// What xam does for XamContentCreateEx's disposition, the package there or
// not: open (0), create (1), open and empty it (2), or an HRESULT.
uint32_t Disposition(uint32_t create, bool exists, uint32_t& action) {
  action = 0;
  switch (create) {
    case 1: action = !exists; return exists ? kAlreadyExists : 0;      // CREATE_NEW
    case 2: action = exists ? 2 : 1; return 0;                         // CREATE_ALWAYS
    case 3: return exists ? 0 : kPathNotFound;                         // OPEN_EXISTING
    case 4: action = !exists; return 0;                                // OPEN_ALWAYS
    case 5: action = exists ? 2 : 0; return exists ? 0 : kPathNotFound;  // TRUNCATE_EXISTING
    default: return kEInvalidArg;
  }
}

// The user's XUID: 0 for an index past the slots (no user).
uint32_t UserXuid(uint32_t user, uint64_t& xuid) {
  xuid = 0;
  if (user >= kUserSlots) return 0;
  const Profile* p = SignedInProfile(user);
  if (!p) return kNotLoggedOn;
  xuid = p->xuid;
  return 0;
}

// ---- packages ---------------------------------------------------------------
std::mutex g_lock;
std::map<std::string, std::filesystem::path> g_mounts;  // root link (lower case) -> package
uint64_t g_packages;

// A package's path components under the partition: Content, the owner's
// XUID, the title, the type, its file name. The name must be one FATX
// takes (chosen: xam leaves that to the file system).
struct Package {
  std::vector<std::string> parts;
  uint32_t status = 0;
};
Package PackageOf(uint32_t user, uint32_t data) {
  Package p;
  uint32_t name = data + kDataFileName;
  bool no_user = Rd8(name) & kNoUser;
  std::string file = ReadName(name, kFileNameSize);
  if (no_user) file[0] = char(file[0] & ~kNoUser);
  if (file.empty() || file == "." || file == ".." || file.find_first_of("\\/:*?\"<>|") != std::string::npos) {
    p.status = kBadName;
    return p;
  }
  uint64_t xuid = 0;
  if (!no_user && (p.status = UserXuid(user, xuid))) return p;
  p.parts = {"Content", Hex(xuid, 16), Hex(ExecutableTitleId(), 8), Hex(Rd32(data + 4), 8), file};
  return p;
}
std::string GuestPath(const std::vector<std::string>& parts) {
  std::string s = kPartition;
  for (const std::string& part : parts) s += "\\" + part;
  return s;
}

// The package's host directory, false if it is not there.
bool Find(const Package& p, std::filesystem::path& dir) {
  Location loc;
  if (Resolve(GuestPath(p.parts), loc) != kStatusSuccess) return false;
  dir = loc.host;
  std::error_code ec;
  return loc.exists && std::filesystem::is_directory(dir, ec);
}
std::filesystem::path HeaderOf(const std::filesystem::path& dir) {
  return dir.parent_path() / (dir.filename().string() + ".header");
}
bool Mounted(const std::filesystem::path& dir) {
  for (const auto& m : g_mounts)
    if (m.second == dir) return true;
  return false;
}

std::filesystem::path Create(ppc::Ctx& c, const Package& p, uint32_t data, uint64_t creator) {
  Location loc;
  std::vector<std::string> parts;
  for (const std::string& part : p.parts) {
    parts.push_back(part);
    std::error_code ec;
    if (Resolve(GuestPath(parts), loc) != kStatusSuccess ||
        (!loc.exists && !std::filesystem::create_directory(loc.host, ec)))
      ppc::Fatal(&c, "content: cannot create %s", loc.host.c_str());
  }
  std::vector<uint8_t> header(Mem() + data, Mem() + data + kDataSize);
  for (int k = 7; k >= 0; k--) header.push_back(uint8_t(creator >> (8 * k)));
  std::ofstream out(HeaderOf(loc.host), std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
  if (!out) ppc::Fatal(&c, "content: cannot write %s", HeaderOf(loc.host).c_str());
  return loc.host;
}
// The header's bytes; empty if the package has none.
std::vector<uint8_t> Header(const std::filesystem::path& dir) {
  std::ifstream in(HeaderOf(dir), std::ios::binary);
  std::vector<uint8_t> h(kDataSize + 8);
  if (!in.read(reinterpret_cast<char*>(h.data()), std::streamsize(h.size()))) h.clear();
  return h;
}
void Remove(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::remove(HeaderOf(dir), ec);
}

// ---- enumerations ---------------------------------------------------------
// XamContentCreateEnumerator's: a user's packages of one type, then (unless
// XCONTENTFLAG_ENUM_EXCLUDECOMMON or other flags at 0x7FFFF000) those no
// one user owns, as xam's second enumerator returns them once the first
// has none left. Each pass lists the type's directory when it starts, by
// name (chosen: FATX lists in the order entries were made).
struct Enumeration {
  uint64_t xuid;
  uint32_t device, type, flags;
  bool second = false, listed = false, done = false;
  std::vector<std::string> names;
  size_t next = 0;
};
std::map<uint32_t, Enumeration> g_enumerations;  // by state address

void CloseEnumeration(uint32_t state) {
  std::lock_guard<std::mutex> g(g_lock);
  g_enumerations.erase(state);
}

void List(Enumeration& e) {
  e.listed = true;
  e.names.clear();
  e.next = 0;
  Location loc;
  std::vector<std::string> parts = {"Content", Hex(e.second ? 0 : e.xuid, 16), Hex(ExecutableTitleId(), 8),
                                    Hex(e.type, 8)};
  if (Resolve(GuestPath(parts), loc) != kStatusSuccess || !loc.exists) return;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(loc.host, ec)) {
    std::string name = entry.path().filename().string();
    if (entry.is_directory(ec) && name.size() < kFileNameSize) e.names.push_back(name);
  }
  std::sort(e.names.begin(), e.names.end());
}

// Up to `count` items from the current pass into `buffer`; their display
// names unless XamEnumerate's flags have 0x80.
uint32_t Take(Enumeration& e, uint32_t buffer, uint32_t count, uint32_t flags) {
  if (!e.listed) List(e);
  uint32_t n = 0;
  for (; n < count && e.next < e.names.size(); n++, e.next++) {
    uint32_t item = buffer + n * kDataSize;
    std::memset(Mem() + item, 0, kDataSize);
    Wr32(item, kDeviceId);
    Wr32(item + 4, e.type);
    const std::string& name = e.names[e.next];
    std::vector<std::string> parts = {"Content", Hex(e.second ? 0 : e.xuid, 16), Hex(ExecutableTitleId(), 8),
                                      Hex(e.type, 8), name};
    Location loc;
    if (!(flags & 0x80) && Resolve(GuestPath(parts), loc) == kStatusSuccess) {
      std::vector<uint8_t> h = Header(loc.host);
      if (!h.empty()) std::memcpy(Mem() + item + kDataDisplayName, h.data() + kDataDisplayName, 0x100);
    }
    std::memcpy(Mem() + item + kDataFileName, name.data(), name.size());
    if (e.second) Wr8(item + kDataFileName, Rd8(item + kDataFileName) | kNoUser);
  }
  return n;
}

}  // namespace

// ---- xam's app: the content messages ----------------------------------------
// 0x20001, XamContentCreateEx's {user, root (64 bytes), XCONTENT_DATA at
// 0x48, ..., flags at 0x248, *disposition, *license mask, cache size,
// content size (8 bytes), *overlapped, the data's size}: the disposition
// (1 created, 2 opened, 0 on failure) to *disposition and as the length.
// A root already linked is ERROR_DUP_NAME before the package is touched
// (chosen: xam finds it when it makes the link, and undoes what it made).
XMSG(kXamApp, 0x20001, ContentCreate) {
  uint32_t req = p1, user = Rd32(req), data = req + 0x48, flags = Rd32(req + 0x248),
           disposition_ptr = Rd32(req + 0x24C);
  if (Rd32(req + 0x264) != kDataSize) ppc::Fatal(&c, "XamContentCreateEx: content aggregates are not supported yet");
  uint32_t disposition = 0;
  auto finish = [&](uint32_t hr) {
    if (disposition_ptr) Wr32(disposition_ptr, disposition);
    length = disposition;
    return hr;
  };
  uint32_t device = Rd32(data) ? Rd32(data) : kDeviceId;
  if (device != kDeviceId) return finish(kDeviceNotConnected);
  bool writable = TitleWritable(Rd32(data + 4)) && !(flags & kReadOnly);
  Package p = PackageOf(user, data);
  if (p.status) return finish(p.status);
  std::string link = RootLink(c, ReadName(req + 4, 0x3F));
  std::lock_guard<std::mutex> g(g_lock);
  if (!SymbolicLinkTarget(link).empty()) return finish(kDupName);
  std::filesystem::path dir;
  bool exists = Find(p, dir);
  uint32_t action, hr = Disposition(flags & 0xF, exists, action);
  if (hr) return finish(hr);
  if (exists && (flags & 0xF) == 2) {
    if (!writable) return finish(kAccessDenied);
    Remove(dir);
    exists = false;
    action = 1;
  }
  if (exists) {
    if (action == 2) {
      if (!writable) return finish(kAccessDenied);
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) std::filesystem::remove_all(entry, ec);
    }
    disposition = 2;
  } else {
    if (!writable) return finish(kAccessDenied);
    uint64_t creator;
    UserXuid(user, creator);
    dir = Create(c, p, data, creator);
    disposition = 1;
  }
  // A package as the kernel's STFS driver mounts it (its name as xam
  // formats it, the numbers chosen; 4 KiB blocks).
  std::string name = "\\Device\\Package_" + Hex(0, 16) + Hex(++g_packages, 16);
  MountDevice({name, dir, writable, "STFS", kFileDeviceDisk, 0x1000});
  CreateSymbolicLink(link, name);
  g_mounts[Lower(link)] = dir;
  return finish(0);
}

// 0x20003, XamContentClose's {root (64 bytes), *overlapped}: the root's link
// goes; open files keep the package.
XMSG(kXamApp, 0x20003, ContentClose) {
  std::string link = RootLink(c, ReadName(p1, 0x3F));
  std::lock_guard<std::mutex> g(g_lock);
  if (SymbolicLinkTarget(link).empty()) return kFileNotFound;
  auto it = g_mounts.find(Lower(link));
  if (it == g_mounts.end()) return kEInvalidArg;
  DeleteSymbolicLink(link);
  g_mounts.erase(it);
  return 0;
}

// 0x20004, XamContentDelete's {user, XCONTENT_DATA at 8, *overlapped at
// 0x208, the data's size}. An open package is ERROR_SHARING_VIOLATION
// (chosen).
XMSG(kXamApp, 0x20004, ContentDelete) {
  uint32_t data = p1 + 8;
  if (Rd32(p1 + 0x20C) != kDataSize) ppc::Fatal(&c, "XamContentDelete: content aggregates are not supported yet");
  if (Rd32(data) != kDeviceId) return kDeviceNotConnected;
  Package p = PackageOf(Rd32(p1), data);
  if (p.status) return p.status;
  std::lock_guard<std::mutex> g(g_lock);
  std::filesystem::path dir;
  if (!Find(p, dir)) return kFileNotFound;
  if (Mounted(dir)) return kSharingViolation;
  Remove(dir);
  return 0;
}

// 0x20005, XamEnumerate's request (enumerate.h) for a content enumerator:
// up to the items per call, ERROR_NO_MORE_FILES when there are none; the
// count to *items returned and as the length.
XMSG(kXamApp, 0x20005, ContentEnumerate) {
  uint32_t flags = Rd32(p1 + 4), state = Rd32(p1 + 8), buffer = Rd32(p1 + 0xC), count = Rd32(p1 + 0x14),
           returned = Rd32(p1 + 0x18);
  if (flags & 1) return kEInvalidArg;
  std::lock_guard<std::mutex> g(g_lock);
  auto it = g_enumerations.find(state);
  if (it == g_enumerations.end()) ppc::Fatal(&c, "XamEnumerate: %x is not a content enumerator", state);
  Enumeration& e = it->second;
  uint32_t n = 0, hr = 0;
  if (e.device && e.device != kDeviceId) {
    hr = kDeviceNotConnected;
  } else if (count && !e.done) {
    n = Take(e, buffer, count, flags);
    if (!n && !e.second && e.xuid && !(e.flags & 0x7FFFF000)) {
      e.second = true;
      e.listed = false;
      n = Take(e, buffer, count, flags);
    }
    if (!n) {
      e.done = true;
      hr = kNoMoreFiles;
    }
  } else if (count) {
    hr = kNoMoreFiles;
  }
  if (returned) Wr32(returned, n);
  length = n;
  return hr;
}

// 0x20014, XamContentGetCreator's {user, XCONTENT_DATA at 4, *is creator
// at 0x138, *creator XUID, *overlapped}: whether the user created the
// package (also when it could not be read: then whether the user is none).
XMSG(kXamApp, 0x20014, ContentGetCreator) {
  uint32_t user = Rd32(p1), data = p1 + 4, is_creator = Rd32(p1 + 0x138), creator_ptr = Rd32(p1 + 0x13C);
  uint64_t xuid;
  if (uint32_t hr = UserXuid(user, xuid)) return hr;
  uint64_t creator = 0;
  uint32_t hr = 0;
  Package p = PackageOf(user, data);
  std::filesystem::path dir;
  std::lock_guard<std::mutex> g(g_lock);
  if (Rd32(data) != kDeviceId) {
    hr = kDeviceNotConnected;
  } else if (p.status) {
    hr = p.status;
  } else if (!Find(p, dir)) {
    hr = kFileNotFound;
  } else {
    std::vector<uint8_t> h = Header(dir);
    for (size_t k = 0; k < 8 && !h.empty(); k++) creator = creator << 8 | h[kDataSize + k];
    if (creator_ptr) Wr64(creator_ptr, creator);
  }
  Wr32(is_creator, creator == xuid);
  return hr;
}

// 0x20015, XamContentGetDeviceState's {device id}.
XMSG(kXamApp, 0x20015, ContentGetDeviceState) { return Rd32(p1) == kDeviceId ? 0 : kDeviceNotConnected; }

// 0x20029, XamContentGetDeviceData's {device id, *XCONTENT_DEVICE_DATA}:
// id, type, size and free bytes (the partition's, as its volume reports
// them), name (27 UTF-16 characters, the last 0).
XMSG(kXamApp, 0x20029, ContentGetDeviceData) {
  if (Rd32(p1) != kDeviceId) return kDeviceNotConnected;
  uint32_t out = Rd32(p1 + 4);
  Location loc;
  if (Resolve(kPartition, loc) != kStatusSuccess) ppc::Fatal(&c, "content: %s is not mounted", kPartition);
  VolumeSize size = QueryVolumeSize(*loc.device);
  std::memset(Mem() + out, 0, 0x50);
  Wr32(out, kDeviceId);
  Wr32(out + 4, kDeviceTypeHdd);
  Wr64(out + 8, size.total * loc.device->cluster);
  Wr64(out + 0x10, size.free * loc.device->cluster);
  for (uint32_t k = 0; kDeviceName[k] && k < 26; k++) Wr16(out + 0x18 + 2 * k, uint16_t(kDeviceName[k]));
  return 0;
}

// ---- the exports ------------------------------------------------------------
// Each returns a Win32 code: with an XOVERLAPPED, ERROR_IO_PENDING once the
// request is sent (here: complete).

// XamContentCreateEx(user, root, *XCONTENT_DATA, flags, *disposition,
// *license mask, cache size, content size (8 bytes), *overlapped).
HLE(kXam, 601, XamContentCreateEx) {
  uint32_t user = Arg(c, 0), root = Arg(c, 1), data = Arg(c, 2), flags = Arg(c, 3), disposition = Arg(c, 4),
           license = Arg(c, 5), cache_size = Arg(c, 6), ov = Arg(c, 8);
  if (!ValidFileName(data + kDataFileName) || cache_size > 0xEF000) return kErrorInvalidParameter;
  if (!root || !ValidRoot(ReadName(root, 0x3F))) return kErrorInvalidName;
  if (disposition) Wr32(disposition, 0);
  if (license) Wr32(license, 0);
  uint32_t req = SystemAlloc(0x268);
  std::memset(Mem() + req, 0, 0x268);
  Wr32(req, user);
  std::string r = ReadName(root, 0x3F);
  std::memcpy(Mem() + req + 4, r.data(), r.size());
  std::memcpy(Mem() + req + 0x48, Mem() + data, kDataSize);
  const uint32_t fields[5] = {flags, disposition, license, cache_size, 0};
  for (uint32_t k = 0; k < 4; k++) Wr32(req + 0x248 + 4 * k, fields[k]);
  Wr64(req + 0x258, Arg64(c, 7));
  Wr32(req + 0x260, ov);
  Wr32(req + 0x264, kDataSize);
  uint32_t hr = ProcessXMsgRequest(c, kXamApp, 0x20001, ov, req, 0x268);
  SystemFree(req);
  return Win32FromHResult(hr);
}

// XamContentClose(root, *overlapped).
HLE(kXam, 602, XamContentClose) {
  uint32_t root = Arg(c, 0), ov = Arg(c, 1);
  if (!root || !ValidRoot(ReadName(root, 0x3F))) return kErrorInvalidName;
  uint32_t req = SystemAlloc(0x44);
  std::memset(Mem() + req, 0, 0x44);
  std::string r = ReadName(root, 0x3F);
  std::memcpy(Mem() + req, r.data(), r.size());
  Wr32(req + 0x40, ov);
  uint32_t hr = ProcessXMsgRequest(c, kXamApp, 0x20003, ov, req, 0x44);
  SystemFree(req);
  return Win32FromHResult(hr);
}

// XamContentDelete(user, *XCONTENT_DATA, *overlapped).
HLE(kXam, 603, XamContentDelete) {
  uint32_t user = Arg(c, 0), data = Arg(c, 1), ov = Arg(c, 2);
  uint32_t req = SystemAlloc(0x210);
  std::memset(Mem() + req, 0, 0x210);
  Wr32(req, user);
  std::memcpy(Mem() + req + 8, Mem() + data, kDataSize);
  Wr32(req + 0x208, ov);
  Wr32(req + 0x20C, kDataSize);
  uint32_t hr = ProcessXMsgRequest(c, kXamApp, 0x20004, ov, req, 0x210);
  SystemFree(req);
  return Win32FromHResult(hr);
}

// XamContentCreateEnumerator(user, device id (0 for any), content type,
// flags, items per call, *buffer size, *handle): the buffer one call
// needs, items * 0x134 bytes.
HLE(kXam, 604, XamContentCreateEnumerator) {
  uint32_t user = Arg(c, 0), device = Arg(c, 1), type = Arg(c, 2), flags = Arg(c, 3), items = Arg(c, 4),
           size = Arg(c, 5), handle = Arg(c, 6);
  uint64_t xuid;
  if (uint32_t hr = UserXuid(user, xuid)) return Win32FromHResult(hr);
  // xam's flags for listing directories and more.
  if (flags & 0x70000000) ppc::Fatal(&c, "XamContentCreateEnumerator: flags %x are not supported yet", flags);
  if (!handle) ppc::Fatal(&c, "XamContentCreateEnumerator: no handle pointer");
  uint32_t state;
  uint32_t h = CreateEnumerator(kXamApp, 0x20005, items, 0x98, CloseEnumeration, state);
  {
    std::lock_guard<std::mutex> g(g_lock);
    g_enumerations[state] = Enumeration{xuid, device, type, flags};
  }
  if (size) Wr32(size, items * kDataSize);
  Wr32(handle, h);
  return 0;
}

// XamContentGetDeviceData(device id, *XCONTENT_DEVICE_DATA). xam reports a
// device of type 0x10 as 2 to titles; the one here is a hard disk.
HLE(kXam, 606, XamContentGetDeviceData) {
  uint32_t req = SystemAlloc(8);
  Wr32(req, Arg(c, 0));
  Wr32(req + 4, Arg(c, 1));
  uint32_t hr = CallXMsg(c, kXamApp, 0x20029, req, 0);
  SystemFree(req);
  return Win32FromHResult(hr);
}

// XamContentGetCreator(user, *XCONTENT_DATA, *is creator, *creator XUID,
// *overlapped).
HLE(kXam, 610, XamContentGetCreator) {
  uint32_t user = Arg(c, 0), data = Arg(c, 1), is_creator = Arg(c, 2), creator = Arg(c, 3), ov = Arg(c, 4);
  if (!is_creator) return kErrorInvalidParameter;
  Wr32(is_creator, 0);
  uint32_t req = SystemAlloc(0x144);
  std::memset(Mem() + req, 0, 0x144);
  Wr32(req, user);
  std::memcpy(Mem() + req + 4, Mem() + data, kDataSize);
  Wr32(req + 0x138, is_creator);
  Wr32(req + 0x13C, creator);
  Wr32(req + 0x140, ov);
  uint32_t hr = ProcessXMsgRequest(c, kXamApp, 0x20014, ov, req, 0x144);
  SystemFree(req);
  return Win32FromHResult(hr);
}

// XamContentGetDeviceState(device id, *overlapped).
HLE(kXam, 613, XamContentGetDeviceState) {
  uint32_t req = SystemAlloc(4);
  Wr32(req, Arg(c, 0));
  uint32_t hr = ProcessXMsgRequest(c, kXamApp, 0x20015, Arg(c, 1), req, 4);
  SystemFree(req);
  return Win32FromHResult(hr);
}

}  // namespace recomp::xbox360
