// xlive.dll (Games for Windows - LIVE) for 32-bit guests, answering as a
// PC with one local, offline profile: user 0 is signed in locally (no
// LIVE), there is no LIVE matchmaking, no marketplace content and no
// achievements. Exports are by ordinal only. The behaviour follows
// xliveless (ThirteenAG's Ultimate-ASI-Loader, MIT), the stand-in the
// community uses for GTA IV and other GFWL games: the same fake XUID, so
// save folders keyed by it match, and the same notification sequence after
// a listener is created.
//
// Title-specific profile settings (XPROFILE_TITLE_SPECIFIC1..3, where games
// keep their options) persist in DataDir()/xlive/Title<n>.dat as
// {uint32 size, bytes}, the xliveless layout.
//
// Asynchronous calls complete at once: the XOVERLAPPED is filled, its event
// set, and ERROR_IO_PENDING returned, as from a real completed request.

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "xlive.dll";

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorInvalidParameter = 87;
constexpr uint32_t kErrorInsufficientBuffer = 122;
constexpr uint32_t kErrorNoMoreFiles = 18;
constexpr uint32_t kErrorIoPending = 997;
constexpr uint32_t kErrorFunctionFailed = 1627;
constexpr uint32_t kErrorNoSuchUser = 1317;
constexpr uint32_t kErrorNotFound = 1168;
inline uint32_t HresultFromWin32(uint32_t e) { return e ? 0x80070000u | (e & 0xFFFF) : 0; }

constexpr uint64_t kXuid = 0xEE000000DEADC0DEull;  // xliveless's fake XUID
constexpr const char* kUserName = "Player";

// Notification ids: XNID(version, area, index).
constexpr uint32_t Xnid(uint32_t version, uint32_t area, uint32_t index) {
  return area << 25 | version << 16 | index;
}
constexpr uint32_t kAreaSystem = 0, kAreaLive = 1;
constexpr uint32_t kXnSysUi = Xnid(0, kAreaSystem, 0x09);
constexpr uint32_t kXnSysSigninChanged = Xnid(0, kAreaSystem, 0x0A);
constexpr uint32_t kXnSysStorageDevicesChanged = Xnid(0, kAreaSystem, 0x0B);
constexpr uint32_t kXnSysProfileSettingChanged = Xnid(0, kAreaSystem, 0x0E);
constexpr uint32_t kXnSysInputDevicesChanged = Xnid(0, kAreaSystem, 0x12);
constexpr uint32_t kXnSysInputDeviceConfigChanged = Xnid(1, kAreaSystem, 0x13);
constexpr uint32_t kXnLiveConnectionChanged = Xnid(0, kAreaLive, 0x01);
constexpr uint32_t kXnLiveContentInstalled = Xnid(0, kAreaLive, 0x07);
constexpr uint32_t kXnLiveMembershipPurchased = Xnid(0, kAreaLive, 0x08);

std::mutex g_mu;

// ---- XOVERLAPPED ------------------------------------------------------------
// { InternalLow (result), InternalHigh (count), InternalContext, hEvent,
//   pCompletionRoutine, dwCompletionContext, dwExtendedError }

uint32_t Complete(gaddr ov, uint32_t result, uint32_t count = 0) {
  Wr32(ov + 0, result);
  Wr32(ov + 4, count);
  Wr32(ov + 24, result);
  if (Rd32(ov + 16)) WarnF("xlive: XOVERLAPPED completion routine ignored\n");
  if (uint32_t ev = Rd32(ov + 12)) SignalEventHandle(ev, false);
  return kErrorIoPending;
}

// A synchronous result, or the same result through `ov` when given.
uint32_t Finish(gaddr ov, uint32_t result, uint32_t count = 0) {
  return ov ? Complete(ov, result, count) : result;
}

// A dummy handle the guest may close with XCloseHandle / CloseHandle.
uint32_t DummyHandle() { return NewEventHandle(true, false); }

// ---- notifications ----------------------------------------------------------

struct Listener {
  uint32_t areas;
  std::deque<std::pair<uint32_t, uint32_t>> queue;  // {id, param}
};
std::map<uint32_t, Listener> g_listeners;  // by event handle

bool Matches(uint32_t areas, uint32_t id) { return areas & (1u << ((id >> 25) & 0x3F)); }

// ---- profile settings -------------------------------------------------------

std::string TitleSettingPath(uint32_t setting_id) {
  const uint32_t id = setting_id & 0x3FFF;
  if (id < 0x3FFD) return std::string();
  std::string dir = DataDir() + "/xlive";
  MakeDirs(dir);
  return dir + "/Title" + std::to_string(0x3FFF - id + 1) + ".dat";
}

// ---- protected PBuffers: {magic, size, data...} on the process heap ---------

constexpr uint32_t kPBufferMagic = 0xDEADC0DE;
bool PBufferRange(gaddr b, uint32_t off, uint32_t len) {
  return b && Rd32(b) == kPBufferMagic && uint64_t(off) + len <= Rd32(b + 4);
}

// The IXHV2Engine (voice chat) object: its methods are thunks named
// "IXHV2Engine::#<slot>", fatal and named if a game calls one.
gaddr VoiceEngine() {
  static gaddr o = [] {
    constexpr int kSlots = 32;
    gaddr vt = HeapAllocProcess(4 * kSlots, true);
    for (int i = 0; i < kSlots; i++)
      Wr32(vt + 4 * i, ThunkFor(kDll, "IXHV2Engine::#" + std::to_string(i)));
    gaddr obj = HeapAllocProcess(8, true);
    Wr32(obj, vt);
    return obj;
  }();
  return o;
}

}  // namespace

// ---- Winsock (#1-#39) ---------------------------------------------------------
// The X* socket calls are Winsock's with the same arguments: handed to the
// ws2_32 handlers. GFWL's VDP protocol (254) is plain UDP here (no
// encryption, no voice payload split).

namespace {
uint64_t Ws2(Ctx& c, const char* name, int nargs) {
  uint64_t a[6] = {};
  for (int i = 0; i < nargs; i++) a[i] = Arg(c, i);
  const gaddr fn = ThunkFor("ws2_32.dll", name);
  switch (nargs) {
    case 0: return CallGuest(c, fn, {});
    case 1: return CallGuest(c, fn, {a[0]});
    case 2: return CallGuest(c, fn, {a[0], a[1]});
    case 3: return CallGuest(c, fn, {a[0], a[1], a[2]});
    case 4: return CallGuest(c, fn, {a[0], a[1], a[2], a[3]});
    case 5: return CallGuest(c, fn, {a[0], a[1], a[2], a[3], a[4]});
    default: return CallGuest(c, fn, {a[0], a[1], a[2], a[3], a[4], a[5]});
  }
}
}  // namespace

HLE_NAMED("xlive.dll", "#1", XWSAStartup, 2) { return Ws2(c, "WSAStartup", 2); }
HLE_NAMED("xlive.dll", "#2", XWSACleanup, 0) { return Ws2(c, "WSACleanup", 0); }
HLE_NAMED("xlive.dll", "#3", XCreateSocket, 3) {
  const uint32_t af = uint32_t(Arg(c, 0)), type = uint32_t(Arg(c, 1));
  uint32_t proto = uint32_t(Arg(c, 2));
  if (proto == 254) proto = type == 2 ? 17 : 0;  // VDP -> UDP (SOCK_DGRAM)
  return CallGuest(c, ThunkFor("ws2_32.dll", "socket"), {af, type, proto});
}
HLE_NAMED("xlive.dll", "#4", XSocketClose, 1) { return Ws2(c, "closesocket", 1); }
HLE_NAMED("xlive.dll", "#5", XSocketShutdown, 2) { return Ws2(c, "shutdown", 2); }
HLE_NAMED("xlive.dll", "#6", XSocketIOCTLSocket, 3) { return Ws2(c, "ioctlsocket", 3); }
HLE_NAMED("xlive.dll", "#7", XSocketSetSockOpt, 5) { return Ws2(c, "setsockopt", 5); }
HLE_NAMED("xlive.dll", "#9", XSocketGetSockName, 3) { return Ws2(c, "getsockname", 3); }
HLE_NAMED("xlive.dll", "#11", XSocketBind, 3) { return Ws2(c, "bind", 3); }
HLE_NAMED("xlive.dll", "#12", XSocketConnect, 3) { return Ws2(c, "connect", 3); }
HLE_NAMED("xlive.dll", "#13", XSocketListen, 2) { return Ws2(c, "listen", 2); }
HLE_NAMED("xlive.dll", "#14", XSocketAccept, 3) { return Ws2(c, "accept", 3); }
HLE_NAMED("xlive.dll", "#15", XSocketSelect, 5) { return Ws2(c, "select", 5); }
HLE_NAMED("xlive.dll", "#18", XSocketRecv, 4) { return Ws2(c, "recv", 4); }
HLE_NAMED("xlive.dll", "#20", XSocketRecvFrom, 6) { return Ws2(c, "recvfrom", 6); }
HLE_NAMED("xlive.dll", "#22", XSocketSend, 4) { return Ws2(c, "send", 4); }
HLE_NAMED("xlive.dll", "#24", XSocketSendTo, 6) { return Ws2(c, "sendto", 6); }
HLE_NAMED("xlive.dll", "#26", XSocketInet_Addr, 1) { return Ws2(c, "inet_addr", 1); }
HLE_NAMED("xlive.dll", "#27", XSocketWSAGetLastError, 0) { return Ws2(c, "WSAGetLastError", 0); }
HLE_NAMED("xlive.dll", "#38", XSocketNTOHS, 1) { return __builtin_bswap16(uint16_t(Arg(c, 0))); }
HLE_NAMED("xlive.dll", "#39", XSocketNTOHL, 1) { return __builtin_bswap32(uint32_t(Arg(c, 0))); }

// ---- XNet ------------------------------------------------------------------

HLE_NAMED("xlive.dll", "#51", XNetStartup, 1) { return 0; }
HLE_NAMED("xlive.dll", "#52", XNetCleanup, 0) { return 0; }
HLE_NAMED("xlive.dll", "#57", XNetXnAddrToInAddr, 3) {
  if (gaddr p = Arg(c, 2)) Wr32(p, 0);
  return 0;
}
HLE_NAMED("xlive.dll", "#58", XNetServerToInAddr, 3) {
  if (gaddr p = Arg(c, 2)) Wr32(p, uint32_t(Arg(c, 0)));
  return 0;
}
HLE_NAMED("xlive.dll", "#63", XNetUnregisterInAddr, 1) { return 0; }
HLE_NAMED("xlive.dll", "#66", XNetGetConnectStatus, 1) { return 0; }  // XNET_CONNECT_STATUS_IDLE
HLE_NAMED("xlive.dll", "#69", XNetQosListen, 5) { return 0; }
HLE_NAMED("xlive.dll", "#70", XNetQosLookup, 12) { return kErrorFunctionFailed; }
HLE_NAMED("xlive.dll", "#72", XNetQosRelease, 1) { return 0; }
HLE_NAMED("xlive.dll", "#73", XNetGetTitleXnAddr, 1) {
  if (gaddr p = Arg(c, 0)) {
    std::memset(GPtr(p), 0, 36);  // XNADDR
    Wr32(p, 0x0100007F);          // 127.0.0.1
  }
  return 4;  // XNET_GET_XNADDR_STATIC
}
HLE_NAMED("xlive.dll", "#75", XNetGetEthernetLinkStatus, 0) {
  return 0x1 | 0x2 | 0x8;  // active, 100 Mbps, full duplex
}
HLE_NAMED("xlive.dll", "#84", XNetSetSystemLinkPort, 1) { return 0; }

// ---- notifications and overlapped results -----------------------------------

HLE_NAMED("xlive.dll", "#5270", XNotifyCreateListener, 2) {  // (ULONGLONG qwAreas)
  const uint32_t areas = uint32_t(Arg(c, 0));
  const uint32_t h = NewEventHandle(true, true);
  std::lock_guard<std::mutex> l(g_mu);
  Listener& li = g_listeners[h];
  li.areas = areas;
  // xliveless's start-up sequence: the guide opens and closes (the sign-in
  // UI), then every state the game may wait for is announced once.
  const std::pair<uint32_t, uint32_t> sequence[] = {
      {kXnSysUi, 1},
      {kXnSysUi, 0},
      {kXnSysSigninChanged, 1},  // bit mask of signed-in users
      {kXnSysStorageDevicesChanged, 0},
      {kXnSysProfileSettingChanged, 1},
      {kXnSysInputDevicesChanged, 0},
      {kXnSysInputDeviceConfigChanged, 1},
      {kXnLiveConnectionChanged, 0},
      {kXnLiveContentInstalled, 0},
      {kXnLiveMembershipPurchased, 0},
  };
  for (const auto& n : sequence)
    if (Matches(areas, n.first)) li.queue.push_back(n);
  if (li.queue.empty()) ResetEventHandle(h);
  return h;
}

HLE_NAMED("xlive.dll", "#651", XNotifyGetNext, 4) {
  const uint32_t h = uint32_t(Arg(c, 0)), filter = uint32_t(Arg(c, 1));
  const gaddr pid = Arg(c, 2), pparam = Arg(c, 3);
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_listeners.find(h);
  if (it == g_listeners.end()) return 0;
  auto& q = it->second.queue;
  auto n = q.begin();
  while (n != q.end() && filter && n->first != filter) ++n;
  if (n == q.end()) return 0;
  if (pid) Wr32(pid, n->first);
  if (pparam) Wr32(pparam, n->second);
  q.erase(n);
  if (q.empty()) ResetEventHandle(h);
  return 1;
}

HLE_NAMED("xlive.dll", "#652", XNotifyPositionUI, 1) { return 0; }

HLE_NAMED("xlive.dll", "#1082", XGetOverlappedExtendedError, 1) {
  gaddr ov = Arg(c, 0);
  return ov ? Rd32(ov + 24) : kErrorInvalidParameter;
}
HLE_NAMED("xlive.dll", "#1083", XGetOverlappedResult, 3) {
  gaddr ov = Arg(c, 0);
  if (gaddr r = Arg(c, 1)) Wr32(r, Rd32(ov + 4));
  return Rd32(ov + 0);
}
HLE_NAMED("xlive.dll", "#5254", XCancelOverlapped, 1) { return 0; }

HLE_NAMED("xlive.dll", "#5251", XCloseHandle, 1) {
  const uint32_t h = uint32_t(Arg(c, 0));
  if (!h) return 0;
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_listeners.erase(h);
  }
  return CloseHandleValue(h) ? 1 : 0;
}

// Every enumeration is empty.
HLE_NAMED("xlive.dll", "#5256", XEnumerate, 5) {
  const gaddr pcount = Arg(c, 3), ov = Arg(c, 4);
  if (ov) return Complete(ov, kErrorNoMoreFiles, 0);
  if (!pcount) return kErrorInvalidParameter;
  Wr32(pcount, 0);
  return kErrorNoMoreFiles;
}

// ---- XLive lifetime and rendering hooks --------------------------------------

HLE_NAMED("xlive.dll", "#5297", XLiveInitializeEx, 2) { return 0; }
HLE_NAMED("xlive.dll", "#5003", XLiveUninitialize, 0) { return 0; }
HLE_NAMED("xlive.dll", "#5002", XLiveRender, 0) { return 0; }
HLE_NAMED("xlive.dll", "#5005", XLiveOnCreateDevice, 2) { return 0; }
HLE_NAMED("xlive.dll", "#5006", XLiveOnDestroyDevice, 0) { return 0; }
HLE_NAMED("xlive.dll", "#5007", XLiveOnResetDevice, 1) { return 0; }
HLE_NAMED("xlive.dll", "#5030", XLivePreTranslateMessage, 1) { return 0; }  // not the guide's
HLE_NAMED("xlive.dll", "#5022", XLiveGetUpdateInformation, 1) { return 0xFFFFFFFF; }  // no update
HLE_NAMED("xlive.dll", "#5024", XLiveUpdateSystem, 1) { return 0; }
HLE_NAMED("xlive.dll", "#5310", XOnlineStartup, 0) { return 0; }
HLE_NAMED("xlive.dll", "#5311", XOnlineCleanup, 0) { return 0; }
HLE_NAMED("xlive.dll", "#5324", XOnlineGetNatType, 0) { return 1; }  // NAT_OPEN

HLE_NAMED("xlive.dll", "#5008", XHVCreateEngine, 3) {
  if (gaddr p = Arg(c, 1)) Wr32(p, DummyHandle());
  if (gaddr p = Arg(c, 2)) Wr32(p, VoiceEngine());
  return 0;
}

// ---- guide UI: never shown -------------------------------------------------

HLE_NAMED("xlive.dll", "#5214", XShowPlayerReviewUI, 3) { return 0; }
HLE_NAMED("xlive.dll", "#5215", XShowGuideUI, 1) { return 0; }
HLE_NAMED("xlive.dll", "#5252", XShowGamerCardUI, 3) { return 0; }
HLE_NAMED("xlive.dll", "#5260", XShowSigninUI, 2) { return 0; }
HLE_NAMED("xlive.dll", "#5365", XShowMarketplaceUI, 5) { return 0; }  // (.., ULONGLONG offer, ..)

// ---- protected data and PBuffers ---------------------------------------------

namespace {
// XLiveProtectData / XLiveUnprotectData: a plain copy.
uint32_t CopyData(Ctx& c) {
  const gaddr in = Arg(c, 0), out = Arg(c, 2), psize = Arg(c, 3);
  const uint32_t n = uint32_t(Arg(c, 1));
  if (!psize) return HresultFromWin32(kErrorInvalidParameter);
  if (Rd32(psize) < n || !out) {
    Wr32(psize, n);
    return HresultFromWin32(kErrorInsufficientBuffer);
  }
  std::memmove(GPtr(out), GPtr(in), n);
  Wr32(psize, n);
  return 0;
}
}  // namespace
HLE_NAMED("xlive.dll", "#5034", XLiveProtectData, 5) { return CopyData(c); }
HLE_NAMED("xlive.dll", "#5035", XLiveUnprotectData, 5) { return CopyData(c); }
HLE_NAMED("xlive.dll", "#5036", XLiveCreateProtectedDataContext, 2) {
  gaddr ph = Arg(c, 1);
  if (!ph) return 1;
  Wr32(ph, DummyHandle());
  return 0;
}
HLE_NAMED("xlive.dll", "#5037", XLiveQueryProtectedDataInformation, 2) { return 0; }
HLE_NAMED("xlive.dll", "#5038", XLiveCloseProtectedDataContext, 1) {
  CloseHandleValue(uint32_t(Arg(c, 0)));
  return 0;
}

HLE_NAMED("xlive.dll", "#5016", XLivePBufferAllocate, 2) {
  const uint32_t size = uint32_t(Arg(c, 0));
  const gaddr pp = Arg(c, 1);
  if (!pp) return 0x8007000E;  // E_OUTOFMEMORY
  gaddr b = HeapAllocProcess(8 + uint64_t(size), true);
  if (!b) return 0x8007000E;
  Wr32(b, kPBufferMagic);
  Wr32(b + 4, size);
  Wr32(pp, b);
  return 0;
}
HLE_NAMED("xlive.dll", "#5017", XLivePBufferFree, 1) {
  gaddr b = Arg(c, 0);
  if (b && Rd32(b) == kPBufferMagic) {
    Wr32(b, 0);
    HeapFreeProcess(b);
  }
  return 0;
}
HLE_NAMED("xlive.dll", "#5018", XLivePBufferGetByte, 3) {
  const gaddr b = Arg(c, 0), out = Arg(c, 2);
  const uint32_t off = uint32_t(Arg(c, 1));
  if (!out || !PBufferRange(b, off, 1)) return 0xFFFFFFFF;
  Wr8(out, Rd8(b + 8 + off));
  return 0;
}
HLE_NAMED("xlive.dll", "#5019", XLivePBufferSetByte, 3) {
  const gaddr b = Arg(c, 0);
  const uint32_t off = uint32_t(Arg(c, 1));
  if (!PBufferRange(b, off, 1)) return 0xFFFFFFFF;
  Wr8(b + 8 + off, uint8_t(Arg(c, 2)));
  return 0;
}
HLE_NAMED("xlive.dll", "#5020", XLivePBufferGetDWORD, 3) {
  const gaddr b = Arg(c, 0), out = Arg(c, 2);
  const uint32_t off = uint32_t(Arg(c, 1));
  if (!out || !PBufferRange(b, off, 4)) return 0xFFFFFFFF;
  Wr32(out, Rd32(b + 8 + off));
  return 0;
}
HLE_NAMED("xlive.dll", "#5021", XLivePBufferSetDWORD, 3) {
  const gaddr b = Arg(c, 0);
  const uint32_t off = uint32_t(Arg(c, 1));
  if (!PBufferRange(b, off, 4)) return 0xFFFFFFFF;
  Wr32(b + 8 + off, uint32_t(Arg(c, 2)));
  return 0;
}
HLE_NAMED("xlive.dll", "#5294", XLivePBufferGetByteArray, 4) {
  const gaddr b = Arg(c, 0), out = Arg(c, 2);
  const uint32_t off = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 3));
  if (!out || !PBufferRange(b, off, n)) return 0xFFFFFFFF;
  std::memmove(GPtr(out), GPtr(b + 8 + off), n);
  return 0;
}
HLE_NAMED("xlive.dll", "#5295", XLivePBufferSetByteArray, 4) {
  const gaddr b = Arg(c, 0), in = Arg(c, 2);
  const uint32_t off = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 3));
  if (!in || !PBufferRange(b, off, n)) return 0xFFFFFFFF;
  std::memmove(GPtr(b + 8 + off), GPtr(in), n);
  return 0;
}

// ---- users ----------------------------------------------------------------

// (dwUserIndex, PXUID pXuid)
HLE_NAMED("xlive.dll", "#5261", XUserGetXUID, 2) {
  if (Arg(c, 0) != 0) return kErrorNoSuchUser;
  if (gaddr out = Arg(c, 1)) Wr64(out, kXuid);
  return kErrorSuccess;
}

HLE_NAMED("xlive.dll", "#5262", XUserGetSigninState, 1) {
  return Arg(c, 0) == 0 ? 1 : 0;  // eXUserSigninState_SignedInLocally
}

HLE_NAMED("xlive.dll", "#5263", XUserGetName, 3) {
  const gaddr buf = Arg(c, 1);
  const uint32_t cap = uint32_t(Arg(c, 2));
  if (Arg(c, 0) != 0 || !buf) return kErrorNoSuchUser;
  CopyOut(buf, cap, kUserName);
  return kErrorSuccess;
}

// XUSER_SIGNIN_INFO: { XUID xuid; dwInfoFlags; UserSigninState;
//   dwGuestNumber; dwSponsorUserIndex; char szUserName[16]; }
HLE_NAMED("xlive.dll", "#5267", XUserGetSigninInfo, 3) {
  const gaddr info = Arg(c, 2);
  if (!info) return kErrorInvalidParameter;
  std::memset(GPtr(info), 0, 40);
  if (Arg(c, 0) != 0) return kErrorNoSuchUser;  // xuid stays INVALID_XUID (0)
  Wr64(info + 0, kXuid);
  Wr32(info + 8, 1);   // XUSER_INFO_FLAG_LIVE_ENABLED
  Wr32(info + 12, 1);  // signed in locally
  PutStr(info + 24, kUserName);
  return kErrorSuccess;
}

HLE_NAMED("xlive.dll", "#5265", XUserCheckPrivilege, 3) {
  const gaddr out = Arg(c, 2);
  const bool user0 = Arg(c, 0) == 0;
  if (out) Wr32(out, user0 ? 1 : 0);
  return user0 ? kErrorSuccess : kErrorNoSuchUser;
}

HLE_NAMED("xlive.dll", "#5264", XUserAreUsersFriends, 5) {
  if (gaddr r = Arg(c, 3)) Wr32(r, 0);
  return Finish(Arg(c, 4), kErrorSuccess);
}
HLE_NAMED("xlive.dll", "#5314", XUserMuteListQuery, 4) {  // (user, XUID, PBOOL)
  if (gaddr r = Arg(c, 3)) Wr32(r, 0);
  return 0;
}
HLE_NAMED("xlive.dll", "#5276", XUserSetProperty, 4) { return 0; }
HLE_NAMED("xlive.dll", "#5277", XUserSetContext, 3) { return 0; }
HLE_NAMED("xlive.dll", "#5292", XUserSetContextEx, 4) { return Finish(Arg(c, 3), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5278", XUserWriteAchievements, 3) { return Finish(Arg(c, 2), kErrorSuccess); }

// (title, user, XUID xuid, flags, start, count, pcbBuffer, phEnum)
HLE_NAMED("xlive.dll", "#5280", XUserCreateAchievementEnumerator, 9) {
  if (gaddr p = Arg(c, 7)) Wr32(p, uint32_t(Arg(c, 6)) * 36);  // XACHIEVEMENT_DETAILS
  if (gaddr p = Arg(c, 8)) Wr32(p, DummyHandle());
  return kErrorSuccess;
}

HLE_NAMED("xlive.dll", "#5281", XUserReadStats, 8) {
  const gaddr pcb = Arg(c, 5), results = Arg(c, 6), ov = Arg(c, 7);
  if (pcb && Rd32(pcb) == 0) {
    Wr32(pcb, 4);
    return Finish(ov, kErrorInsufficientBuffer);
  }
  if (results) Wr32(results, 0);
  return Finish(ov, kErrorSuccess);
}
HLE_NAMED("xlive.dll", "#5284", XUserCreateStatsEnumeratorByRank, 7) {
  if (gaddr p = Arg(c, 5)) Wr32(p, 0);
  if (gaddr p = Arg(c, 6)) Wr32(p, 0);
  return kErrorFunctionFailed;
}
HLE_NAMED("xlive.dll", "#5286", XUserCreateStatsEnumeratorByXuid, 8) {
  if (gaddr p = Arg(c, 6)) Wr32(p, 0);
  if (gaddr p = Arg(c, 7)) Wr32(p, 0);
  return kErrorFunctionFailed;
}
HLE_NAMED("xlive.dll", "#5312", XFriendsCreateEnumerator, 5) {
  if (gaddr p = Arg(c, 3)) Wr32(p, uint32_t(Arg(c, 2)) * 0xC4);  // XONLINE_FRIEND
  if (gaddr p = Arg(c, 4)) Wr32(p, DummyHandle());
  return kErrorSuccess;
}

// XUSER_READ_PROFILE_SETTING_RESULT { dwSettingsLen; pSettings } followed
// by the XUSER_PROFILE_SETTING array (40 bytes each: source @0, user @8,
// dwSettingId @16, XUSER_DATA @24 = {type @24, union @32}) and the data.
// Setting id bits: type 31..28, size 27..16, id 13..0.
HLE_NAMED("xlive.dll", "#5331", XUserReadProfileSettings, 7) {
  const uint32_t n = uint32_t(Arg(c, 2));
  const gaddr ids = Arg(c, 3), pcb = Arg(c, 4), res = Arg(c, 5), ov = Arg(c, 6);
  if (!pcb || (n && !ids)) return Finish(ov, kErrorInvalidParameter);
  uint32_t need = 8 + 40 * n;
  for (uint32_t i = 0; i < n; i++) need += (Rd32(ids + 4 * i) >> 16) & 0xFFF;
  if (Rd32(pcb) < need || !res) {
    Wr32(pcb, need);
    return Finish(ov, kErrorInsufficientBuffer, need);
  }
  std::memset(GPtr(res), 0, need);
  const gaddr settings = res + 8;
  Wr32(res + 0, n);
  Wr32(res + 4, settings);
  gaddr data = settings + 40 * n;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t sid = Rd32(ids + 4 * i);
    const uint32_t type = sid >> 28, size = (sid >> 16) & 0xFFF;
    const gaddr s = settings + 40 * i;
    Wr32(s + 0, 0);  // XSOURCE_NO_VALUE
    Wr32(s + 8, uint32_t(Arg(c, 1)));
    Wr32(s + 16, sid);
    Wr8(s + 24, uint8_t(type));
    const std::string path = TitleSettingPath(sid);
    if (type == 6 && !path.empty()) {  // XUSER_DATA_TYPE_BINARY
      if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint32_t len = 0;
        if (std::fread(&len, 4, 1, f) == 1) {
          len = std::min(len, size);
          len = uint32_t(std::fread(GPtr(data), 1, len, f));
          Wr32(s + 0, 2);  // XSOURCE_TITLE
          Wr32(s + 32, len);
          Wr32(s + 36, data);
        }
        std::fclose(f);
      }
    }
    data += size;
  }
  return Finish(ov, kErrorSuccess);
}

HLE_NAMED("xlive.dll", "#5337", XUserWriteProfileSettings, 4) {
  const uint32_t n = uint32_t(Arg(c, 1));
  const gaddr settings = Arg(c, 2), ov = Arg(c, 3);
  for (uint32_t i = 0; i < n; i++) {
    const gaddr s = settings + 40 * i;
    const uint32_t sid = Rd32(s + 16);
    const std::string path = TitleSettingPath(sid);
    if (sid >> 28 != 6 || path.empty()) continue;
    const uint32_t len = Rd32(s + 32);
    const gaddr bytes = Rd32(s + 36);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(&len, 4, 1, f);
      if (len) std::fwrite(GPtr(bytes), 1, len, f);
      std::fclose(f);
    } else {
      WarnF("xlive: cannot write %s\n", path.c_str());
    }
  }
  return Finish(ov, kErrorSuccess);
}

// ---- sessions (no network play) ---------------------------------------------

HLE_NAMED("xlive.dll", "#5300", XSessionCreate, 8) {
  if (gaddr p = Arg(c, 7)) Wr32(p, DummyHandle());
  return Finish(Arg(c, 6), kErrorSuccess);
}
HLE_NAMED("xlive.dll", "#5318", XSessionStart, 3) { return Finish(Arg(c, 2), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5332", XSessionEnd, 2) { return Finish(Arg(c, 1), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5330", XSessionDelete, 2) { return Finish(Arg(c, 1), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5329", XSessionFlushStats, 2) { return Finish(Arg(c, 1), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5317", XSessionWriteStats, 6) { return Finish(Arg(c, 5), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5322", XSessionModify, 5) { return Finish(Arg(c, 4), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5323", XSessionMigrateHost, 4) { return Finish(Arg(c, 3), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5325", XSessionLeaveLocal, 4) { return Finish(Arg(c, 3), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5336", XSessionLeaveRemote, 4) { return Finish(Arg(c, 3), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5326", XSessionJoinRemote, 5) { return Finish(Arg(c, 4), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5327", XSessionJoinLocal, 5) { return Finish(Arg(c, 4), kErrorSuccess); }
HLE_NAMED("xlive.dll", "#5319", XSessionSearchEx, 11) { return Finish(Arg(c, 10), kErrorFunctionFailed); }
HLE_NAMED("xlive.dll", "#5333", XSessionArbitrationRegister, 7) {  // (.., ULONGLONG nonce, ..)
  return Finish(Arg(c, 6), kErrorFunctionFailed);
}
HLE_NAMED("xlive.dll", "#5328", XSessionGetDetails, 4) {
  const gaddr pcb = Arg(c, 1), ov = Arg(c, 3);
  if (pcb) Wr32(pcb, 0);
  return Finish(ov, kErrorFunctionFailed);
}
HLE_NAMED("xlive.dll", "#5315", XInviteGetAcceptedInfo, 2) { return kErrorFunctionFailed; }
HLE_NAMED("xlive.dll", "#5316", XInviteSend, 5) { return Finish(Arg(c, 4), kErrorSuccess); }

// ---- strings, storage, content, marketplace ---------------------------------

// STRING_VERIFY_RESPONSE { WORD wNumStrings; HRESULT* pStringResult; }:
// every string passes.
HLE_NAMED("xlive.dll", "#5303", XStringVerify, 7) {
  const uint32_t n = uint32_t(Arg(c, 2));
  const gaddr res = Arg(c, 5), ov = Arg(c, 6);
  if (res) {
    Wr16(res, uint16_t(n));
    Wr32(res + 4, res + 8);
    for (uint32_t i = 0; i < n; i++) Wr32(res + 8 + 4 * i, 0);
  }
  return Finish(ov, kErrorSuccess);
}

HLE_NAMED("xlive.dll", "#5344", XStorageBuildServerPath, 7) {
  const gaddr out = Arg(c, 5), plen = Arg(c, 6);
  if (out) PutWStr(out, "");
  if (plen) Wr32(plen, 1);
  return kErrorFunctionFailed;
}
HLE_NAMED("xlive.dll", "#5345", XStorageDownloadToMemory, 7) {
  return Finish(Arg(c, 6), kErrorNotFound);
}
HLE_NAMED("xlive.dll", "#5305", XStorageUploadFromMemory, 5) {
  return Finish(Arg(c, 4), kErrorFunctionFailed);
}

HLE_NAMED("xlive.dll", "#5335", XTitleServerCreateEnumerator, 4) {
  if (gaddr p = Arg(c, 2)) Wr32(p, 0);
  if (gaddr p = Arg(c, 3)) Wr32(p, 0);
  return kErrorFunctionFailed;
}

// XLiveContentCreateEnumerator(cItems, pRetrievalInfo, pcbBuffer, phEnum).
HLE_NAMED("xlive.dll", "#5360", XLiveContentCreateEnumerator, 4) {
  if (gaddr p = Arg(c, 2)) Wr32(p, uint32_t(Arg(c, 0)) * 32);  // XCONTENT_DATA
  if (gaddr p = Arg(c, 3)) Wr32(p, DummyHandle());
  return kErrorSuccess;
}
HLE_NAMED("xlive.dll", "#5350", XLiveContentCreateAccessHandle, 7) {
  return Finish(Arg(c, 6), kErrorFunctionFailed);
}
HLE_NAMED("xlive.dll", "#5355", XLiveContentGetPath, 4) {
  return HresultFromWin32(kErrorInvalidParameter);  // no content is ever listed
}
HLE_NAMED("xlive.dll", "#5367", XContentGetMarketplaceCounts, 5) {
  if (gaddr r = Arg(c, 3)) std::memset(GPtr(r), 0, 8);
  return Finish(Arg(c, 4), kErrorSuccess);
}
HLE_NAMED("xlive.dll", "#5372", XMarketplaceCreateOfferEnumerator, 6) {
  if (gaddr p = Arg(c, 4)) Wr32(p, uint32_t(Arg(c, 3)) * 0x40);
  if (gaddr p = Arg(c, 5)) Wr32(p, DummyHandle());
  return kErrorSuccess;
}

}  // namespace recomp::hle
