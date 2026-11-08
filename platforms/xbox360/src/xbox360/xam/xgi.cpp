// xam: the XGI app (0xFB), xam's own handlers for a title's users'
// contexts and properties, achievements, sessions and statistics. What
// they hand to Live (searches, statistics, arbitration) fails here as it
// would with no connection: no profile is signed in to Live (user.cpp).
// docs/xam.md, "XGI".

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/xam/msg.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kEInvalidArg = 0x80070057;
// HRESULT_FROM_WIN32(ERROR_NOT_FOUND): a user index with no profile.
constexpr uint32_t kNoUser = 0x80070490;
// xam's session errors (facility 0x15): a privilege it lacks, no slot
// left, a flag change it refuses, no game type context, the wrong state,
// Live needed, a member it refuses, no network for a host's key or a
// client's registration, arbitration not registered.
constexpr uint32_t kSessionPrivilege = 0x80155201, kSessionFull = 0x80155202, kSessionFlags = 0x80155204,
                   kSessionNoGameType = 0x80155205, kSessionState = 0x80155206, kSessionNeedsLive = 0x80155209,
                   kSessionMember = 0x8015520A, kSessionHostKey = 0x8015520B, kSessionArbitration = 0x8015520C,
                   kSessionRegisterKey = 0x8015520D;
// What a request Live answers gets with no connection (chosen: Live's
// module is not part of the system update, so its own code is not there
// to read; console check).
constexpr uint32_t kLiveNotLoggedOn = 0x80151802;

// Session flags: host, presence, statistics, matchmaking, arbitration, peer
// network (system link).
constexpr uint32_t kHost = 0x1, kStats = 0x4, kArbitration = 0x10, kPeerNetwork = 0x20;
constexpr uint32_t kNeedsLive = 0xE, kLiveFeatures = 0x1E;
constexpr uint32_t kGameTypeContext = 0x800A;

// ---- users' contexts ----------------------------------------------------
std::mutex g_lock;
std::map<uint32_t, uint32_t> g_contexts[kUserSlots];  // by slot: context id -> value

// The slot of a signed-in user named by XUID, or by slot when the XUID is
// 0; kUserSlots if none.
uint32_t UserOf(uint32_t slot, uint64_t xuid) {
  for (uint32_t s = 0; s < kUserSlots; s++) {
    const Profile* p = SignedInProfile(s);
    if (p && (xuid ? p->xuid == xuid : s == slot)) return s;
  }
  return kUserSlots;
}

// ---- sessions -------------------------------------------------------------
// xam's session (behind the session object's first word): its flags,
// owner, slots, members and state (0 idle, 1 arbitration registered, 2
// in progress, 4 deleted).
struct Member {
  uint32_t slot;  // a local user's, or kUserSlots for a remote one
  uint64_t xuid;
  bool private_slot;
};
struct Session {
  uint32_t flags, owner, public_slots, private_slots;
  uint32_t public_used = 0, private_used = 0, state = 0;
  std::vector<Member> members;
};
std::unordered_map<uint32_t, Session> g_sessions;  // by id, the object's first word
std::set<uint32_t> g_session_objects;
uint32_t g_next_session = 1;

void DestroySessionObject(uint32_t obj) {
  {
    std::lock_guard<std::mutex> g(g_lock);
    g_sessions.erase(Rd32(obj));
    g_session_objects.erase(obj);
  }
  SystemFree(obj);
}

// A request's session: the session object is its first word, xam's session
// that object's first word.
Session* SessionOf(uint32_t request) {
  uint32_t obj = Rd32(request);
  if (!obj) return nullptr;
  auto it = g_sessions.find(Rd32(obj));
  return it == g_sessions.end() ? nullptr : &it->second;
}

bool OnlineXuid(uint64_t xuid) {
  uint32_t top = uint32_t(xuid >> 48);
  return top == 9 || ((top & 0xF) == 9 && (top & 0xC0));
}

// A member's index, or -1.
int FindMember(const Session& s, uint32_t slot, uint64_t xuid) {
  for (size_t i = 0; i < s.members.size(); i++)
    if (slot < kUserSlots ? s.members[i].slot == slot : s.members[i].xuid == xuid) return int(i);
  return -1;
}

void Remove(Session& s, size_t i) {
  (s.members[i].private_slot ? s.private_used : s.public_used)--;
  s.members.erase(s.members.begin() + long(i));
}

}  // namespace

// XamSessionCreateHandle(*handle): a session object, its first word 0 until
// XSessionCreate (0xB0010) gives it a session; a Win32 code.
HLE(kXam, 790, XamSessionCreateHandle) {
  uint32_t obj = SystemAlloc(4);
  Wr32(obj, 0);
  Own(obj, DestroySessionObject);
  {
    std::lock_guard<std::mutex> g(g_lock);
    g_session_objects.insert(obj);
  }
  Wr32(Arg(c, 0), NewHandle(obj));
  return 0;
}

// XamSessionRefObjByHandle(handle, *object): the session object,
// referenced; ERROR_INVALID_HANDLE for a handle to anything else.
HLE(kXam, 791, XamSessionRefObjByHandle) {
  uint32_t obj = HandleObject(Arg(c, 0));
  {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_session_objects.count(obj)) return 6;
  }
  Reference(obj);
  Wr32(Arg(c, 1), obj);
  return 0;
}

// XUserSetContextEx {slot, ?, XUID (a user by XUID unless 0), context id,
// value}: kept with the user, whose game type context (0x800A) a session
// needs. Success whether or not there is such a user.
XMSG(kXgiApp, 0xB0006, XUserSetContextEx) {
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t slot = UserOf(Rd32(p1), Rd64(p1 + 8));
  if (slot < kUserSlots) g_contexts[slot][Rd32(p1 + 0x10)] = Rd32(p1 + 0x14);
  return 0;
}

// XUserSetPropertyEx {slot, ?, XUID, property id, size, value}: kept by xam
// for presence and Live, which nothing reads here; success whatever the
// user.
XMSG(kXgiApp, 0xB0007, XUserSetPropertyEx) { return 0; }

// XUserWriteAchievements {count, *(slot, id) pairs}: each user's
// achievements, kept with its profile for the running title; a slot past 3
// or with no profile is ERROR_NOT_FOUND. S_FALSE (1) if one was already
// earned.
XMSG(kXgiApp, 0xB0008, XUserWriteAchievements) {
  uint32_t count = Rd32(p1), list = Rd32(p1 + 4);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t slot = Rd32(list + 8 * i);
    if (slot >= kUserSlots || !SignedInProfile(slot)) return kNoUser;
  }
  uint32_t result = 0;
  std::lock_guard<std::mutex> g(g_lock);
  for (uint32_t i = 0; i < count; i++) {
    const Profile* p = SignedInProfile(Rd32(list + 8 * i));
    char title[9];
    std::snprintf(title, sizeof title, "%08X", ExecutableTitleId());
    std::filesystem::path dir = p->directory / "achievements", file = dir / title;
    std::set<uint32_t> earned;
    std::ifstream in(file);
    for (uint32_t id; in >> id;) earned.insert(id);
    if (!earned.insert(Rd32(list + 8 * i + 4)).second) {
      result = 1;
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(file, std::ios::trunc);
    for (uint32_t id : earned) out << id << '\n';
    if (!out) ppc::Fatal(&c, "achievements: cannot write %s", file.c_str());
  }
  return result;
}

// XSessionCreate {object, flags, public slots, private slots, slot, *session
// info, *nonce}. A session that needs Live (presence, statistics,
// matchmaking) needs its user signed in to Live; a system link one with
// any of those or arbitration Live's multiplayer privilege; neither is
// held here. A host on the peer network makes an XNet key, a client
// registers the host's: there is no network here (chosen errors are
// xam's for those failures). Otherwise the user's game type context must
// be set, unless the session uses arbitration.
XMSG(kXgiApp, 0xB0010, XSessionCreate) {
  uint32_t obj = Rd32(p1), flags = Rd32(p1 + 4), slot = Rd32(p1 + 0x10);
  if (!obj) return kEInvalidArg;
  Wr32(obj, 0);
  if (slot >= kUserSlots || !SignedInProfile(slot)) return kNoUser;
  if (flags & kNeedsLive) return kSessionNeedsLive;
  if (flags & kPeerNetwork && flags & kLiveFeatures) return kSessionPrivilege;
  std::lock_guard<std::mutex> g(g_lock);
  if (!(flags & kArbitration) && !g_contexts[slot][kGameTypeContext]) return kSessionNoGameType;
  if (flags & kHost) return kSessionHostKey;
  if (flags & kPeerNetwork) return kSessionRegisterKey;
  // Neither host nor peer: no session info.
  std::memset(Mem() + Rd32(p1 + 0x14), 0, 0x3C);
  uint32_t id = g_next_session++;
  g_sessions[id] = Session{flags, slot, Rd32(p1 + 8), Rd32(p1 + 0xC)};
  Wr32(obj, id);
  return 0;
}

// XSessionDelete {object}.
XMSG(kXgiApp, 0xB0011, XSessionDelete) {
  std::lock_guard<std::mutex> g(g_lock);
  Session* s = SessionOf(p1);
  if (!s) return kEInvalidArg;
  s->state = 4;
  return 0;
}

// XSessionJoinLocal / XSessionJoinRemote {object, count, *XUIDs, *slots,
// *private}: one of the lists, at most 4 local users, each signed in.
// Members already in the session take no slot; the others take a private
// one if they ask for it and one is left, else a public one, or the
// session is full. A remote member with an offline XUID is refused in a
// session that uses Live's features.
XMSG(kXgiApp, 0xB0012, XSessionJoin) {
  uint32_t count = Rd32(p1 + 4), xuids = Rd32(p1 + 8), slots = Rd32(p1 + 0xC), want = Rd32(p1 + 0x10);
  if (!count || (slots && count > 4) || !xuids == !slots) return kEInvalidArg;
  for (uint32_t i = 0; slots && i < count; i++) {
    uint32_t slot = Rd32(slots + 4 * i);
    if (slot >= kUserSlots || !SignedInProfile(slot)) return kNoUser;
  }
  std::lock_guard<std::mutex> g(g_lock);
  Session* s = SessionOf(p1);
  if (!s) return kEInvalidArg;
  if (s->state == 4) return kSessionState;
  uint32_t new_public = 0, new_private = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t slot = slots ? Rd32(slots + 4 * i) : kUserSlots;
    uint64_t xuid = slots ? SignedInProfile(slot)->xuid : Rd64(xuids + 8 * i);
    if (!slots && !OnlineXuid(xuid) && s->flags & kLiveFeatures) return kSessionMember;
    if (FindMember(*s, slot, xuid) < 0) (Rd32(want + 4 * i) ? new_private : new_public)++;
  }
  if (s->public_slots < s->public_used + new_public ||
      s->public_slots + s->private_slots < s->public_used + s->private_used + new_public + new_private)
    return kSessionFull;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t slot = slots ? Rd32(slots + 4 * i) : kUserSlots;
    uint64_t xuid = slots ? SignedInProfile(slot)->xuid : Rd64(xuids + 8 * i);
    if (FindMember(*s, slot, xuid) >= 0) continue;
    bool private_slot = Rd32(want + 4 * i) && s->private_used < s->private_slots;
    (private_slot ? s->private_used : s->public_used)++;
    s->members.push_back({slot, xuid, private_slot});
  }
  return 0;
}

// XSessionLeaveLocal / XSessionLeaveRemote {object, count, *XUIDs, *slots}:
// the members named leave; others are skipped.
XMSG(kXgiApp, 0xB0013, XSessionLeave) {
  uint32_t count = Rd32(p1 + 4), xuids = Rd32(p1 + 8), slots = Rd32(p1 + 0xC);
  if (!count || (slots && count > 4) || !xuids == !slots) return kEInvalidArg;
  std::lock_guard<std::mutex> g(g_lock);
  Session* s = SessionOf(p1);
  if (!s) return kEInvalidArg;
  if (s->state == 4) return kSessionState;
  for (uint32_t i = 0; i < count; i++) {
    int m = slots ? FindMember(*s, Rd32(slots + 4 * i), 0) : FindMember(*s, kUserSlots, Rd64(xuids + 8 * i));
    if (m >= 0) Remove(*s, size_t(m));
  }
  return 0;
}

// XSessionStart {object, flags}: from idle (or, with arbitration, once
// registered) to in progress.
XMSG(kXgiApp, 0xB0014, XSessionStart) {
  std::lock_guard<std::mutex> g(g_lock);
  Session* s = SessionOf(p1);
  if (!s) return kEInvalidArg;
  if (s->state >= 2) return kSessionState;
  if (s->flags & kArbitration && s->state != 1) return kSessionArbitration;
  s->state = 2;
  return 0;
}

// XSessionEnd {object}: from in progress back to idle.
XMSG(kXgiApp, 0xB0015, XSessionEnd) {
  std::lock_guard<std::mutex> g(g_lock);
  Session* s = SessionOf(p1);
  if (!s) return kEInvalidArg;
  if (s->state != 2) return kSessionState;
  s->state = 0;
  return 0;
}

// XSessionSearch: a Live query.
XMSG(kXgiApp, 0xB0016, XSessionSearch) { return kLiveNotLoggedOn; }

// XSessionModify {object, flags, public slots, private slots}: only the
// arbitration flag and those that close the session to invites and joins
// (0x10, 0xF00) may change.
XMSG(kXgiApp, 0xB0018, XSessionModify) {
  std::lock_guard<std::mutex> g(g_lock);
  Session* s = SessionOf(p1);
  if (!s) return kEInvalidArg;
  uint32_t flags = Rd32(p1 + 4);
  if ((s->flags ^ flags) & ~0xF10u) return kSessionFlags;
  s->flags = flags;
  s->public_slots = Rd32(p1 + 8);
  s->private_slots = Rd32(p1 + 0xC);
  return 0;
}

// XSessionArbitrationRegister {object, flags, nonce, ?, seconds, *size,
// results}: Live registers an arbitrated session; one without arbitration
// only moves to registered.
XMSG(kXgiApp, 0xB001A, XSessionArbitrationRegister) {
  std::lock_guard<std::mutex> g(g_lock);
  Session* s = SessionOf(p1);
  if (!s) return kEInvalidArg;
  if (s->state > 1) return kSessionState;
  if (s->flags & kArbitration) return kLiveNotLoggedOn;
  s->state = 1;
  return 0;
}

// XSessionSearchByID: a Live query.
XMSG(kXgiApp, 0xB001B, XSessionSearchByID) { return kLiveNotLoggedOn; }

// XUserReadStats {title, count, ...; size at 0x14, results at 0x18}: xam
// clears the results and asks Live.
XMSG(kXgiApp, 0xB0021, XUserReadStats) {
  std::memset(Mem() + Rd32(p1 + 0x18), 0, Rd32(p1 + 0x14));
  return kLiveNotLoggedOn;
}

// XSessionWriteStats {object, ?, XUID, ...}: only a session with
// statistics, in progress (which needs Live).
XMSG(kXgiApp, 0xB0025, XSessionWriteStats) {
  std::lock_guard<std::mutex> g(g_lock);
  Session* s = SessionOf(p1);
  if (!s) return kEInvalidArg;
  if (!(s->flags & kStats)) return kSessionFlags;
  if (s->state != 2) return kSessionState;
  return kLiveNotLoggedOn;
}

// XSessionFlushStats {object}: a session in progress sends its statistics
// to Live; one without has none to send.
XMSG(kXgiApp, 0xB0026, XSessionFlushStats) {
  std::lock_guard<std::mutex> g(g_lock);
  Session* s = SessionOf(p1);
  if (!s) return kEInvalidArg;
  if (s->state != 2) return kSessionState;
  return s->flags & kStats ? kLiveNotLoggedOn : 0;
}

}  // namespace recomp::xbox360
