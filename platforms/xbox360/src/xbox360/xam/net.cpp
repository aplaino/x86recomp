// xam: networking (NetDll_*), xam's own TCP/IP stack behind thin exports.
// There is no network interface here: a console with no cable connected.
// What such a console does is modelled from xam's code: its sockets and
// their checks, delivery to the console's own sockets (127.x), TCP
// connections that never get an answer, XNet's keys and security
// associations, DNS without servers and QoS probes that time out
// (docs/xam.md, "Networking").

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/memory.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
using Clock = std::chrono::steady_clock;

// Winsock errors, as xam sets them.
constexpr uint32_t kWsaEacces = 10013, kWsaEfault = 10014, kWsaEinval = 10022, kWsaEwouldblock = 10035,
                   kWsaEalready = 10037, kWsaEnotsock = 10038, kWsaEmsgsize = 10040,
                   kWsaEnoprotoopt = 10042, kWsaEprotonosupport = 10043, kWsaEsocktnosupport = 10044,
                   kWsaEafnosupport = 10047, kWsaEaddrinuse = 10048, kWsaEaddrnotavail = 10049,
                   kWsaEnetdown = 10050, kWsaEnobufs = 10055, kWsaEisconn = 10056, kWsaEnotconn = 10057,
                   kWsaEtimedout = 10060, kWsaEconnrefused = 10061, kWsaEhostunreach = 10065,
                   kWsaNotInitialised = 10093, kWsaENoMore = 10102, kWsaSyscallFailure = 10107,
                   kWsaHostNotFound = 11001;
constexpr uint32_t kSocketError = 0xFFFFFFFF;

// A socket's flags (xam's own bits).
constexpr uint32_t kSecureOnly = 0x100, kStream = 0x800, kVdp = 0x1000, kBound = 0x8000, kConnected = 0x10000,
                   kReconnect = 0x20000, kBroadcast = 0x200000, kNonBlocking = 0x400000, kReuseAddr = 0x800000,
                   kExclusiveAddr = 0x1000000, kNoDelay = 0x2000000, kInsecure = 0x20000000;
// A TCP socket's state: closed, connecting (SYN sent).
constexpr uint8_t kTcpClosed = 0, kTcpSynSent = 2;

// XNetStartupParams as xam keeps it (0x4C bytes; titles pass the first 13
// or all): xam's defaults and the bounds it clamps a title's values to (a
// title's 0 takes the default). Bytes 2-5: datagram and stream sockets,
// receive and send buffers in KiB; 6, 7: keys and security associations;
// 8-10: QoS data limit / 4, probe timeout in s, probe retries; 0x13 the
// TTL, 0x1F the TOS; 0x42, 0x43 the largest send and receive buffers in
// KiB; 0x45 the TCP connect timeout in s; 0x46 the linger time in s.
constexpr uint8_t kParamsDefault[0x4C] = {
    0x00, 0x00, 0x08, 0x20, 0x10, 0x10, 0x08, 0x20, 0x40, 0x02, 0x03, 0x08, 0x02, 0x46, 0x04, 0x04,
    0x01, 0x0a, 0x01, 0x40, 0x01, 0x09, 0x01, 0x04, 0x02, 0x14, 0x0d, 0x20, 0x40, 0x02, 0x03, 0x00,
    0x04, 0x80, 0x3c, 0x03, 0x09, 0x04, 0x04, 0x04, 0x02, 0x0a, 0x3c, 0x01, 0x78, 0x02, 0x0e, 0x05,
    0x05, 0x03, 0x08, 0x09, 0x05, 0x08, 0x02, 0x14, 0x0d, 0x0a, 0x03, 0x01, 0x05, 0x04, 0x02, 0x05,
    0x08, 0x05, 0x3f, 0xff, 0x1e, 0x14, 0x3c, 0x05, 0x03, 0x01, 0x08, 0x05};
constexpr uint8_t kParamsMin[0x4C] = {
    0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x04, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x04, 0x04, 0x01, 0x01, 0x00,
    0x01, 0x04, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
constexpr uint8_t kParamsMax[0x4C] = {
    0x00, 0xff, 0xe8, 0xe8, 0x3f, 0x3f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0x80, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x3f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr uint32_t kParamDgrams = 2, kParamStreams = 3, kParamRecvK = 4, kParamSendK = 5, kParamKeys = 6,
                   kParamSas = 7, kParamQosData = 8, kParamProbeTimeout = 9, kParamProbeRetries = 10,
                   kParamTtl = 0x13, kParamTos = 0x1F, kParamMaxSendK = 0x42, kParamMaxRecvK = 0x43,
                   kParamConnectTimeout = 0x45, kParamLinger = 0x46;
// cfgFlags: no IP configuration at startup.
constexpr uint8_t kNoIpConfig = 0x80;

// The console's Ethernet address: chosen, a Microsoft OUI (console check).
constexpr uint8_t kMac[6] = {0x00, 0x22, 0x48, 0x00, 0x00, 0x01};
// The ports xam keeps from titles' datagram sockets: its own (1256), DHCP
// (68) and system link's (3074).
constexpr uint16_t kReservedPorts[] = {1256, 68, 3074};
constexpr uint16_t kFirstPort = 0x401, kLastPort = 0x4E7;
constexpr uint32_t kLoopback = 0x7F000001;
// XNADDR (0x24 bytes): IP, online IP, online port, Ethernet address at 10,
// online id at 0x10; byte 0x20 is 1 for an address of a console like this
// one.
constexpr uint32_t kXnAddrSize = 0x24, kXnAddrEnet = 10, kXnAddrKind = 0x20;
// XNKID's kinds (its first byte's top three bits).
constexpr uint8_t kKidSystemLink = 0x00, kKidCrossPlatform = 0x40, kKidOnline = 0x80, kKidMask = 0xE0;
// The privileges xam asks for: insecure sockets, cross-platform system link.
constexpr uint32_t kPrivilegeInsecureSockets = 6, kPrivilegeCrossPlatform = 0xE;

bool Privileged(uint32_t privilege) {
  return XexHeaderField(Rd32(ExecutableModule() + kLdrXexHeader), 0x00030000) >> privilege & 1;
}

void RandomBytes(uint32_t va, uint32_t n) {
  static std::random_device device;
  for (uint32_t k = 0; k < n; k++) Wr8(va + k, uint8_t(device()));
}

struct Datagram {
  uint32_t addr;
  uint16_t port;
  std::vector<uint8_t> data;
};

struct Socket {
  uint32_t caller, handle, index, flags;
  uint16_t port = 0;
  uint32_t peer = 0;  // a datagram socket's connected or a TCP socket's remote address
  uint16_t peer_port = 0;
  uint32_t send_timeout = 0, recv_timeout = 0;  // ms, 0 none
  uint8_t ttl, tos;
  uint32_t send_buffer, recv_buffer;
  uint16_t linger_on = 0, linger_time;
  uint32_t mss = 0x200, tcp_5804 = 0;
  uint8_t tcp_flags = 0;  // 0x80: option 0x5805
  uint8_t state = kTcpClosed;
  uint32_t attempt = 0;      // TCP connection attempts, for their timeouts
  uint32_t error = 0;        // the last connection's Winsock error
  bool listed = true;        // in the list bind and connect look through
  bool closed = false;       // closesocket or WSACleanup
  bool receiving = false;    // a blocking receive is waiting
  uint32_t groups[4] = {};   // multicast groups joined
  std::deque<Datagram> queue;
  uint32_t queued = 0;  // bytes
  uint32_t event;       // what waits on the socket wait on
  Socket() {
    event = SystemAlloc(kDispatcherHeaderSize);
    InitObject(event, kNotificationEvent, 0);
  }
  ~Socket() { SystemFree(event); }
  bool stream() const { return flags & kStream; }
};
using SocketRef = std::shared_ptr<Socket>;

struct QosListen {
  bool enabled = false;
  std::vector<uint8_t> data;
};
struct Key {
  uint8_t id[8], key[16];
  std::unique_ptr<QosListen> listen;
};
// A security association: what XNetXnAddrToInAddr hands out for another
// console, an IP address 0.x.y.z (a counter, then the table's slot).
struct Sa {
  uint32_t ina;
  uint8_t xnaddr[kXnAddrSize];
  uint8_t key_id[8];
  uint16_t flags;
};

struct Caller {
  uint32_t xnet = 0, wsa = 0;  // XNetStartup's and WSAStartup's counts
  uint8_t params[0x4C];
  uint32_t sequence = 0;  // in socket handles
  std::vector<SocketRef> sockets;  // by handle index
  uint32_t dgrams = 0, streams = 0;
  std::vector<Key> keys;
  std::vector<std::unique_ptr<Sa>> sas;  // by slot
  bool started() const { return xnet || wsa; }
};

struct Dns {
  uint32_t caller, va, event;
};
struct Qos {
  uint32_t caller, va, event, id;
};

struct Stack {
  Caller callers[4];
  bool ip_config = true;
  uint16_t next_port = kFirstPort;
  uint16_t sa_sequence;
  std::vector<SocketRef> all;  // creation order
  std::vector<Dns> dns;
  std::vector<Qos> qos;
  uint32_t qos_ids = 0;
};

std::mutex g_lock;
std::unique_ptr<Stack> g_stack;

// ---- timeouts -----------------------------------------------------------
// What happens later (a TCP connection's timeout, a QoS probe's end) runs
// on a thread of the network stack's own, under the lock.
std::condition_variable g_timer_wake;
struct Timer {
  void (*fn)(uint64_t);
  uint64_t arg;
};
std::multimap<Clock::time_point, Timer> g_timers;

void TimerThread(void*) {
  std::unique_lock<std::mutex> lock(g_lock);
  for (;;) {
    if (g_timers.empty()) {
      g_timer_wake.wait(lock);
      continue;
    }
    auto first = g_timers.begin();
    if (Clock::now() < first->first) {
      g_timer_wake.wait_until(lock, first->first);
      continue;
    }
    Timer t = first->second;
    g_timers.erase(first);
    t.fn(t.arg);
  }
}

void After(Clock::duration delay, void (*fn)(uint64_t), uint64_t arg) {
  static bool started = [] {
    CreateSystemThread(TimerThread, nullptr);
    return true;
  }();
  (void)started;
  g_timers.insert({Clock::now() + delay, {fn, arg}});
  g_timer_wake.notify_one();
}

// ---- the stack ------------------------------------------------------------
Caller* StartedCaller(uint32_t caller) {
  // Callers past 3 are past xam's table; here they are never started.
  if (!g_stack || caller >= 4 || !g_stack->callers[caller].started()) return nullptr;
  return &g_stack->callers[caller];
}
Caller* WsaCaller(uint32_t caller) {
  Caller* p = StartedCaller(caller);
  return p && p->wsa ? p : nullptr;
}

// The title's XNetStartupParams as xam takes them: only a structure of 13
// or 0x4C bytes, each byte clamped, 0 the default; flags 1 and 8 cleared.
void ReadParams(uint32_t va, uint8_t* params) {
  std::memcpy(params, kParamsDefault, sizeof kParamsDefault);
  uint8_t size = va ? Rd8(va) : 0;
  if (size == 13 || size == 0x4C)
    for (uint32_t k = 0; k < size; k++) {
      uint8_t v = Rd8(va + k);
      params[k] = !v ? kParamsDefault[k] : std::clamp(v, kParamsMin[k], kParamsMax[k]);
    }
  params[1] &= 0xF6;
}

void Signal(Socket& s) { SetSignal(s.event, 1); }

void Close(const SocketRef& s) {
  Caller& owner = g_stack->callers[s->caller];
  owner.sockets[s->index] = nullptr;
  (s->stream() ? owner.streams : owner.dgrams)--;
  auto& all = g_stack->all;
  all.erase(std::find(all.begin(), all.end(), s));
  s->closed = true;
  Signal(*s);
}

// A caller's sockets, keys and security associations go when it has
// cleaned up both XNet and Winsock; the stack goes with the last caller.
void CloseSockets(uint32_t caller) {
  for (SocketRef& s : std::vector<SocketRef>(g_stack->callers[caller].sockets))
    if (s) Close(s);
}
void CallerDone(uint32_t caller) {
  Caller& p = g_stack->callers[caller];
  CloseSockets(caller);
  p.sockets.clear();
  p.keys.clear();
  p.sas.clear();
  for (const Caller& other : g_stack->callers)
    if (other.started()) return;
  for (const Dns& d : g_stack->dns) {
    Dereference(d.event);
    SystemFree(d.va);
  }
  for (const Qos& q : g_stack->qos) {
    if (q.event) Dereference(q.event);
    SystemFree(q.va);
  }
  g_stack.reset();
}

// NetDll_WSAStartup / XNetStartup: the stack is made with the first
// caller (its IP configuration started unless the title's flags ask not
// to), a caller's settings with its first startup, from the title's
// XNetStartupParams (WSAStartup's: the defaults).
uint32_t Startup(uint32_t caller, uint32_t params, bool wsa, uint32_t version, uint32_t data) {
  if (caller != 1 && caller != 2) return kWsaEinval;
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) {
    g_stack = std::make_unique<Stack>();
    g_stack->ip_config = !(params && Rd8(params + 1) & kNoIpConfig);
    std::random_device device;
    g_stack->sa_sequence = uint16_t(device());
  }
  Caller& p = g_stack->callers[caller];
  if (!p.started()) {
    ReadParams(params, p.params);
    p.sockets.assign(p.params[kParamDgrams] + p.params[kParamStreams], nullptr);
    p.sas.clear();
    p.sas.resize(p.params[kParamSas]);
    std::random_device device;
    p.sequence = device();
  }
  if (!wsa) {
    p.xnet++;
    return 0;
  }
  p.wsa++;
  // WSADATA: the version asked for, the highest (2.2), no description or
  // status and no limits.
  Wr16(data, uint16_t(version));
  Wr16(data + 2, 0x0202);
  Wr8(data + 4, 0);
  Wr8(data + 0x105, 0);
  Wr16(data + 0x186, 0);
  Wr16(data + 0x188, 0);
  Wr32(data + 0x18C, 0);
  return 0;
}

uint32_t Cleanup(uint32_t caller, bool wsa) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack || caller >= 4) return kWsaNotInitialised;
  Caller* p = &g_stack->callers[caller];
  uint32_t& count = wsa ? p->wsa : p->xnet;
  if (!count) return kWsaNotInitialised;
  if (!--count && wsa) CloseSockets(caller);
  if (!p->started()) CallerDone(caller);
  return 0;
}

// A Winsock call's failure: the error for WSAGetLastError, SOCKET_ERROR.
uint32_t Fail(uint32_t error) {
  SetLastError(error);
  return kSocketError;
}

// A socket handle: the caller in the top 4 bits, a per-caller sequence
// number, the table's slot in the low 12.
SocketRef Find(Caller& p, uint32_t caller, uint32_t handle) {
  uint32_t index = handle & 0xFFF;
  if (handle && handle != 0xFFFFFFFF && handle >> 28 == (caller & 0xF) && index < p.sockets.size()) {
    SocketRef s = p.sockets[index];
    if (s && s->handle == handle) return s;
  }
  return nullptr;
}

// The socket of a Winsock call: WSANOTINITIALISED before WSAStartup,
// WSAENOTSOCK for a handle that names none.
SocketRef Lookup(uint32_t caller, uint32_t handle, uint32_t& error) {
  Caller* p = WsaCaller(caller);
  SocketRef s = p ? Find(*p, caller, handle) : nullptr;
  error = !p ? kWsaNotInitialised : !s ? kWsaEnotsock : 0;
  return s;
}

// A port for a socket: one asked for, refused if another socket of its
// kind has it (unless this one may reuse it and neither is exclusive) or,
// for a datagram socket, if xam keeps it; for 0 the next free one from
// 1025-1255, trying at least 231.
bool PortTaken(const Socket& s, uint16_t port) {
  for (const SocketRef& o : g_stack->all)
    if (o.get() != &s && o->listed && o->flags & kBound && o->stream() == s.stream() && o->port == port &&
        (o->flags & kExclusiveAddr || s.flags & kExclusiveAddr || !(s.flags & kReuseAddr)))
      return true;
  return false;
}
uint32_t BindPort(Socket& s, uint16_t port) {
  if (port) {
    if (!s.stream() && std::find(std::begin(kReservedPorts), std::end(kReservedPorts), port) != std::end(kReservedPorts))
      return kWsaEaddrinuse;
    if (PortTaken(s, port)) return kWsaEaddrinuse;
  } else {
    const Caller& p = g_stack->callers[s.caller];
    uint32_t tries = std::max<uint32_t>(p.params[s.stream() ? kParamStreams : kParamDgrams], 0xE7);
    for (;; tries--) {
      if (!tries) return kWsaEaddrinuse;
      port = g_stack->next_port++;
      if (g_stack->next_port > kLastPort) g_stack->next_port = kFirstPort;
      if (!PortTaken(s, port)) break;
    }
  }
  s.port = port;
  s.flags |= kBound;
  return 0;
}

// Whether a destination can be reached: a security association's address
// (0.x.y.z) only when this caller holds one for it.
const Sa* FindSa(uint32_t ina, uint32_t& owner) {
  if (!ina || ina >> 24) return nullptr;
  for (owner = 1; owner < 4; owner++) {
    const auto& sas = g_stack->callers[owner].sas;
    if ((ina & 0xFF) < sas.size() && sas[ina & 0xFF] && sas[ina & 0xFF]->ina == ina) return sas[ina & 0xFF].get();
  }
  return nullptr;
}
bool Reachable(const Socket& s, uint32_t addr) {
  if (!addr || addr >> 24) return true;
  uint32_t owner;
  return FindSa(addr, owner) && owner == s.caller;
}

// A datagram for one of the console's own addresses (127.x): to the
// socket bound to its port (from that port, if connected), unless its
// receive buffer is full (chosen: dropped, console check).
void Deliver(const Socket& from, uint32_t addr, uint16_t port, uint32_t buf, uint32_t len) {
  for (const SocketRef& o : g_stack->all) {
    if (o->stream() || !(o->flags & kBound) || o->port != port) continue;
    if (o->flags & kConnected && (o->peer != addr || o->peer_port != from.port)) continue;
    if (o->queued + len > o->recv_buffer) return;
    o->queue.push_back({addr, from.port, std::vector<uint8_t>(Mem() + buf, Mem() + buf + len)});
    o->queued += len;
    Signal(*o);
    return;
  }
}

// A datagram sent: refused when too large (0x8000 bytes, 0x4F0 to a
// security association; a VDP one carries a 2-byte header), delivered to
// the console's own sockets for 127.x, otherwise lost: there is no link.
uint32_t SendDatagram(Socket& s, uint32_t addr, uint16_t port, uint32_t buf, uint32_t len) {
  if (!Reachable(s, addr)) return kWsaEhostunreach;
  uint32_t size = len;
  if (s.flags & kVdp) {
    if (len < 2) return kWsaEmsgsize;
    size = len - 2;
  }
  if (size > (!addr || addr >> 24 ? 0x8000u : 0x4F0u)) return kWsaEmsgsize;
  if (addr >> 24 == 0x7F) Deliver(s, addr, port, buf, len);
  return 0;
}

// Waits on a socket's event without the lock, until `deadline` or for
// ever; false when it timed out.
bool WaitSocket(std::unique_lock<std::mutex>& lock, Socket& s, Clock::time_point deadline, bool forever) {
  lock.unlock();
  uint32_t status;
  if (forever) {
    status = Wait(&s.event, 1, false, nullptr, false);
  } else {
    auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
    int64_t timeout = -std::max<int64_t>(left / 100, 0);
    status = Wait(&s.event, 1, false, &timeout, false);
  }
  lock.lock();
  return status != kStatusTimeout;
}

// ---- TCP --------------------------------------------------------------------
// A connection ends: its error kept, the socket closed and free to connect
// again.
void EndConnection(Socket& s, uint32_t error) {
  s.state = kTcpClosed;
  s.error = error;
  s.listed = false;
  s.flags |= kReconnect;
  Signal(s);
}

// No answer comes: the connect timeout (XNetStartupParams' byte 0x45, in
// seconds) ends the attempt.
void ConnectTimeout(uint64_t arg) {
  if (!g_stack) return;
  for (const SocketRef& s : g_stack->all)
    if (uint64_t(s->handle) << 32 == (arg & ~0xFFFFFFFFull) && s->attempt == uint32_t(arg) &&
        s->state == kTcpSynSent)
      EndConnection(*s, kWsaEtimedout);
}

// ---- QoS ------------------------------------------------------------------------
// XNQOS: count, pending, then XNQOSINFO (0x18 bytes) per entry: flags
// (1 complete, 2 target contacted, 4 target disabled, 8 data received),
// probes sent and received, data size and pointer, RTTs, bits per second
// up and down; the listener's data after the entries.
constexpr uint32_t kQosInfo = 0x18, kQosComplete = 1;

// An entry's end: complete (its flags may say so already), one less
// pending, the event set.
void QosEntryDone(Qos& q, uint32_t entry) {
  uint32_t info = q.va + 8 + kQosInfo * entry;
  Wr8(info, Rd8(info) | kQosComplete);
  Wr32(q.va + 4, Rd32(q.va + 4) - 1);
  if (q.event) SetSignal(q.event, 1);
}
void QosProbesEnd(uint64_t arg) {
  if (!g_stack) return;
  for (Qos& q : g_stack->qos)
    if (q.id == arg >> 32) QosEntryDone(q, uint32_t(arg));
}

const Key* FindKey(uint32_t id_va, uint32_t* owner = nullptr) {
  for (uint32_t caller = 1; caller < 4; caller++)
    for (const Key& k : g_stack->callers[caller].keys)
      if (!std::memcmp(k.id, Mem() + id_va, 8)) {
        if (owner) *owner = caller;
        return &k;
      }
  return nullptr;
}
Key* FindKey(const uint8_t* id, uint32_t* owner) {
  for (uint32_t caller = 1; caller < 4; caller++)
    for (Key& k : g_stack->callers[caller].keys)
      if (!std::memcmp(k.id, id, 8)) {
        if (owner) *owner = caller;
        return &k;
      }
  return nullptr;
}

}  // namespace

// ---- startup ---------------------------------------------------------------
// NetDll_WSAStartup(caller, version, *data); NetDll_WSAStartupEx(caller,
// version, *data, title version) is the same: xam passes the version on
// to its stack unchecked. Callers are a title (1) or the system (2).
HLE(kXam, 1, NetDll_WSAStartup) { return Startup(Arg(c, 0), 0, true, Arg(c, 1), Arg(c, 2)); }
HLE(kXam, 36, NetDll_WSAStartupEx) { return Startup(Arg(c, 0), 0, true, Arg(c, 1), Arg(c, 2)); }

// NetDll_XNetStartup(caller, *parameters); NetDll_XNetStartupEx(caller,
// *parameters, title version) is the same.
HLE(kXam, 51, NetDll_XNetStartup) { return Startup(Arg(c, 0), Arg(c, 1), false, 0, 0); }
HLE(kXam, 80, NetDll_XNetStartupEx) { return Startup(Arg(c, 0), Arg(c, 1), false, 0, 0); }

// NetDll_WSACleanup(caller): its sockets close when its count ends;
// NetDll_XNetCleanup(caller) returns its error instead of setting it.
HLE(kXam, 2, NetDll_WSACleanup) {
  uint32_t error = Cleanup(Arg(c, 0), true);
  return error ? Fail(error) : 0;
}
HLE(kXam, 52, NetDll_XNetCleanup) { return Cleanup(Arg(c, 0), false); }

// NetDll_XNetGetEthernetLinkStatus(caller): no link, 0. xam does not check
// the caller here.
HLE(kXam, 75, NetDll_XNetGetEthernetLinkStatus) { return 0; }

// NetDll_WSAGetLastError: the thread's last error.
HLE(kXam, 27, NetDll_WSAGetLastError) { return LastError(); }

// NetDll_WSACreateEvent: a manual-reset event, not signalled (NtCreateEvent).
HLE(kXam, 29, NetDll_WSACreateEvent) {
  uint32_t obj = SystemAlloc(kDispatcherHeaderSize);
  InitObject(obj, kNotificationEvent, 0);
  Own(obj, SystemFree);
  SetLastError(0);
  return NewHandle(obj);
}

// NetDll_inet_addr(text): a.b.c.d with each part decimal, octal (0...) or
// hex (0x...), spaces around the parts allowed; fewer parts fill the last
// one's low bytes; INADDR_NONE (all ones) for anything else, 0 for "".
HLE(kXam, 26, NetDll_inet_addr) {
  uint32_t p = Arg(c, 0), parts[4], n = 0;
  for (;;) {
    while (Rd8(p) == ' ') p++;
    uint8_t ch = Rd8(p);
    if (n > 3 || !(ch >= '0' && ch <= '9')) {
      if (ch) return 0xFFFFFFFF;
      break;
    }
    uint32_t v = 0;
    if (ch == '0' && (Rd8(p + 1) == 'x' || Rd8(p + 1) == 'X')) {
      uint32_t start = p += 2;
      for (;; p++) {
        ch = Rd8(p);
        if (ch >= '0' && ch <= '9') v = v * 16 + ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = v * 16 + ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = v * 16 + ch - 'A' + 10;
        else break;
      }
      if (p == start) return 0xFFFFFFFF;
    } else {
      uint32_t base = ch == '0' ? 8 : 10;
      do {
        v = v * base + ch - '0';
        ch = Rd8(++p);
      } while (ch >= '0' && ch < '0' + base);
    }
    while (Rd8(p) == ' ') p++;
    if (Rd8(p) == '.') p++;
    parts[n++] = v;
  }
  switch (n) {
    case 1: return parts[0];
    case 2: return (parts[0] & 0xFF) << 24 | (parts[1] & 0xFFFFFF);
    case 3: return (parts[0] & 0xFF) << 24 | (parts[1] & 0xFF) << 16 | (parts[2] & 0xFFFF);
    case 4: return (parts[0] & 0xFF) << 24 | (parts[1] & 0xFF) << 16 | (parts[2] & 0xFF) << 8 | (parts[3] & 0xFF);
  }
  return 0;
}

// ---- sockets ---------------------------------------------------------------
// NetDll_socket(caller, family, type, protocol): AF_UNSPEC or AF_INET;
// SOCK_STREAM (TCP, 6) or SOCK_DGRAM (UDP, 17, or VDP, 254), a missing
// type or protocol taken from the other. Out of sockets of the kind
// (XNetStartupParams' counts): WSAENOBUFS.
HLE(kXam, 3, NetDll_socket) {
  uint32_t caller = Arg(c, 0), family = Arg(c, 1), type = Arg(c, 2), protocol = Arg(c, 3);
  std::lock_guard<std::mutex> g(g_lock);
  Caller* p = WsaCaller(caller);
  if (!p) return Fail(kWsaNotInitialised);
  if (family != 0 && family != 2) return Fail(kWsaEafnosupport);
  if (type > 2) return Fail(kWsaEsocktnosupport);
  if (protocol != 0 && protocol != 6 && protocol != 17 && protocol != 254) return Fail(kWsaEprotonosupport);
  if (!type) type = protocol == 17 || protocol == 254 ? 2 : 1;
  if (!protocol) protocol = type == 2 ? 17 : 6;
  if ((type == 1) != (protocol == 6)) return Fail(kWsaEprotonosupport);
  bool stream = type == 1;
  uint32_t& count = stream ? p->streams : p->dgrams;
  if (count >= p->params[stream ? kParamStreams : kParamDgrams]) return Fail(kWsaEnobufs);
  uint32_t index = 0;
  while (p->sockets[index]) index++;
  auto s = std::make_shared<Socket>();
  s->caller = caller;
  s->index = index;
  s->handle = caller << 28 | (p->sequence++ & 0xFFFF) << 12 | index;
  s->flags = (stream ? kStream : 0) | (protocol == 254 ? kVdp : 0);
  s->ttl = p->params[kParamTtl];
  s->tos = p->params[kParamTos];
  s->send_buffer = p->params[kParamSendK] << 10;
  s->recv_buffer = p->params[kParamRecvK] << 10;
  s->linger_time = p->params[kParamLinger];
  p->sockets[index] = s;
  count++;
  g_stack->all.push_back(s);
  return s->handle;
}

// NetDll_closesocket(caller, socket): a receive or connect waiting on it
// ends with WSAENOTSOCK.
HLE(kXam, 4, NetDll_closesocket) {
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t error;
  SocketRef s = Lookup(Arg(c, 0), Arg(c, 1), error);
  if (!s) return Fail(error);
  Close(s);
  return 0;
}

// NetDll_ioctlsocket(caller, socket, command, *argument): FIONBIO
// (non-blocking), FIONREAD (a datagram socket's next datagram's size, 1
// for an empty one; a stream socket's buffered bytes, none here).
HLE(kXam, 6, NetDll_ioctlsocket) {
  constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t error, command = Arg(c, 2), arg = Arg(c, 3);
  SocketRef s = Lookup(Arg(c, 0), Arg(c, 1), error);
  if (!s) return Fail(error);
  if (command == kFionbio) {
    s->flags = Rd32(arg) ? s->flags | kNonBlocking : s->flags & ~kNonBlocking;
  } else if (command == kFionread) {
    Wr32(arg, s->stream() || s->queue.empty() ? 0 : std::max<uint32_t>(uint32_t(s->queue.front().data.size()), 1));
  } else {
    return Fail(kWsaEnoprotoopt);
  }
  return 0;
}

// SO_SNDBUF / SO_RCVBUF: the send buffer 1 to XNetStartupParams' largest
// (byte 0x42), the receive buffer 0 to its (0x43), at most 63 KiB without
// window scaling; a connected TCP socket's receive buffer cannot shrink
// (WSAEINVAL, the send buffer set all the same).
static uint32_t SetBuffers(Socket& s, int32_t send, int32_t recv) {
  const Caller& p = g_stack->callers[s.caller];
  int32_t max_send = p.params[kParamMaxSendK] << 10, max_recv = p.params[kParamMaxRecvK] << 10;
  send = send > max_send ? max_send : send < 1 ? 1 : send;
  recv = recv > max_recv ? max_recv : recv < 0 ? 0 : recv;
  if (recv > 0xFC00 && (!s.stream() || (!(s.flags & kInsecure) && !(s.tcp_flags & 0x20)))) recv = 0xFC00;
  s.send_buffer = uint32_t(send);
  if (s.stream() && s.state > 1 && uint32_t(recv) < s.recv_buffer) return kWsaEinval;
  s.recv_buffer = uint32_t(recv);
  return 0;
}

// Multicast groups (IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP, a datagram
// socket's up to 4): joining one twice, a fifth, or leaving one not joined
// is WSAEINVAL.
static uint32_t Membership(Socket& s, uint32_t group, bool join) {
  for (uint32_t k = 0; k < 4; k++) {
    if (!s.groups[k]) {
      if (!join) return kWsaEinval;
      s.groups[k] = group;
      return 0;
    }
    if (s.groups[k] == group) {
      if (join) return kWsaEinval;
      std::memmove(s.groups + k, s.groups + k + 1, (3 - k) * 4);
      s.groups[3] = 0;
      return 0;
    }
  }
  return kWsaEinval;
}

// NetDll_setsockopt(caller, socket, level, option, *value, length): the
// value a byte below 4 bytes. Unknown options: WSAENOPROTOOPT; unknown
// levels: WSAEINVAL (xam's code, docs/xam.md).
HLE(kXam, 7, NetDll_setsockopt) {
  uint32_t caller = Arg(c, 0), level = Arg(c, 2), value = Arg(c, 4);
  int32_t option = int32_t(Arg(c, 3)), length = int32_t(Arg(c, 5));
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t error;
  SocketRef s = Lookup(caller, Arg(c, 1), error);
  if (!s) return Fail(error);
  uint32_t v = length < 4 ? Rd8(value) : Rd32(value);
  bool stream = s->stream();
  error = 0;
  auto flag = [&](uint32_t bit, bool on) { s->flags = on ? s->flags | bit : s->flags & ~bit; };
  if (level == 0) {  // IPPROTO_IP
    if (option == 5 || option == 6) {
      error = stream ? kWsaEnoprotoopt : Membership(*s, Rd32(value), option == 5);
    } else if (option == 7) {  // IP_TTL
      if (length != 4 || !v || v > 0xFF) error = kWsaEinval;
      else s->ttl = uint8_t(v);
    } else if (option == 100) {  // the DSCP bits of the TOS
      if (length != 1 || Rd8(value) > 0x3F) error = kWsaEinval;
      else s->tos = uint8_t(Rd8(value) << 2);
    } else {
      error = kWsaEnoprotoopt;
    }
  } else if (level == 6) {  // IPPROTO_TCP
    if (!stream) error = kWsaEnoprotoopt;
    else if (option == 1) flag(kNoDelay, v);
    else if (option == 0x5803 || option == 0x5804) {
      if (v < 0x58 || v > 0x5B4) error = kWsaEinval;
      else (option == 0x5803 ? s->mss : s->tcp_5804) = v;
    } else if (option == 0x5805) s->tcp_flags = uint8_t((v ? 0x80 : 0) | (s->tcp_flags & 0x7F));
    else error = kWsaEnoprotoopt;
  } else if (level == 0xFFFF) {  // SOL_SOCKET
    switch (option) {
      case 0x1001: error = SetBuffers(*s, int32_t(v), int32_t(s->recv_buffer)); break;  // SO_SNDBUF
      case 0x1002: error = SetBuffers(*s, int32_t(s->send_buffer), int32_t(v)); break;  // SO_RCVBUF
      case ~0x80:  // SO_DONTLINGER
        if (!stream) error = kWsaEnoprotoopt;
        else s->linger_on = !v;
        break;
      case ~4:  // SO_EXCLUSIVEADDRUSE: setting it again is WSAEINVAL
        if (v && s->flags & kExclusiveAddr) error = kWsaEinval;
        else flag(kExclusiveAddr, v);
        break;
      case 4:  // SO_REUSEADDR: the same
        if (v && s->flags & kReuseAddr) error = kWsaEinval;
        else flag(kReuseAddr, v);
        break;
      case 0x20:  // SO_BROADCAST: UDP only
        if (s->flags & (kStream | kVdp)) error = kWsaEnoprotoopt;
        else flag(kBroadcast, v);
        break;
      case 0x80:  // SO_LINGER
        if (!stream) error = kWsaEnoprotoopt;
        else s->linger_on = Rd16(value), s->linger_time = Rd16(value + 2);
        break;
      case 0x1005: s->send_timeout = v; break;  // SO_SNDTIMEO
      case 0x1006: s->recv_timeout = v; break;  // SO_RCVTIMEO
      case 0x5801:  // insecure: the title's needs the privilege
        if (s->flags & kVdp) error = kWsaEnoprotoopt;
        else if (s->flags & kInsecure && v) error = kWsaEinval;
        else if (caller == 1 && v && !Privileged(kPrivilegeInsecureSockets)) error = kWsaEacces;
        else flag(kInsecure, v);
        break;
      case 0x5802: flag(kSecureOnly, v); break;
      case 0x5803:  // needs a Live logon
        if (s->flags & kVdp) error = kWsaEnoprotoopt;
        else if (s->flags & kInsecure && v) error = kWsaEinval;
        else error = kWsaEnetdown;
        break;
      default: error = kWsaEnoprotoopt;
    }
  } else {
    error = kWsaEinval;
  }
  return error ? Fail(error) : 0;
}

// NetDll_getsockopt(caller, socket, level, option, *value, *length): the
// value cleared over *length, then written as a byte below 4 bytes (the
// length then 1), otherwise 4 bytes.
HLE(kXam, 8, NetDll_getsockopt) {
  uint32_t level = Arg(c, 2), value = Arg(c, 4), length = Arg(c, 5);
  int32_t option = int32_t(Arg(c, 3));
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t error;
  SocketRef s = Lookup(Arg(c, 0), Arg(c, 1), error);
  if (!s) return Fail(error);
  std::memset(Mem() + value, 0, Rd32(length));
  bool stream = s->stream(), known = true;
  uint32_t v = 0;
  if (level == 0) {
    if (option == 7) v = s->ttl;
    else if (option == 100) v = s->tos >> 2;
    else known = false;
  } else if (level == 6) {
    if (!stream) known = false;
    else if (option == 1) v = (s->flags & kNoDelay) != 0;
    else if (option == 0x5802) v = 0;
    else if (option == 0x5803) v = s->mss;
    else if (option == 0x5804) v = s->tcp_5804;
    else known = false;
  } else if (level == 0xFFFF) {
    switch (option) {
      case 0x1001: v = s->send_buffer; break;
      case 0x1002: v = s->recv_buffer; break;
      case 0x1005: v = s->send_timeout; break;
      case 0x1006: v = s->recv_timeout; break;
      case 0x1008: v = stream ? 1 : 2; break;  // SO_TYPE
      case 0x5801:
        if (s->flags & kVdp) known = false;
        else v = (s->flags & kInsecure) != 0;
        break;
      case 0x5802: v = (s->flags & kSecureOnly) != 0; break;
      case ~0x80:
        if (!stream) known = false;
        else v = !s->linger_on;
        break;
      case ~4: v = (s->flags & kExclusiveAddr) != 0; break;
      case 2:  // SO_ACCEPTCONN: never listening here
        if (!stream) known = false;
        break;
      case 4: v = (s->flags & kReuseAddr) != 0; break;
      case 0x20:
        if (s->flags & (kStream | kVdp)) known = false;
        else v = (s->flags & kBroadcast) != 0;
        break;
      case 0x80:
        if (!stream) known = false;
        else {
          Wr16(value, s->linger_on);
          Wr16(value + 2, s->linger_time);
          Wr32(length, 4);
          return 0;
        }
        break;
      default: known = false;
    }
  } else {
    return Fail(kWsaEinval);
  }
  if (!known) return Fail(kWsaEnoprotoopt);
  if (Rd32(length) < 4) {
    Wr8(value, uint8_t(v));
    Wr32(length, 1);
  } else {
    Wr32(value, v);
    Wr32(length, 4);
  }
  return 0;
}

// NetDll_getsockname(caller, socket, *name, *length): a bound socket's
// port on any address (sockaddr_in, 16 bytes; the length is not read).
HLE(kXam, 9, NetDll_getsockname) {
  uint32_t name = Arg(c, 2);
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t error;
  SocketRef s = Lookup(Arg(c, 0), Arg(c, 1), error);
  if (!s) return Fail(error);
  if (!(s->flags & kBound)) return Fail(kWsaEinval);
  std::memset(Mem() + name, 0, 16);
  Wr16(name, 2);
  Wr16(name + 2, s->port);
  Wr32(Arg(c, 3), 16);
  return 0;
}

// NetDll_bind(caller, socket, *name, length): any address only (others
// WSAEADDRNOTAVAIL), once (WSAEINVAL); the length is not read.
HLE(kXam, 11, NetDll_bind) {
  uint32_t name = Arg(c, 2);
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t error;
  SocketRef s = Lookup(Arg(c, 0), Arg(c, 1), error);
  if (!s) return Fail(error);
  error = s->flags & kBound ? kWsaEinval : Rd32(name + 4) ? kWsaEaddrnotavail : BindPort(*s, Rd16(name + 2));
  return error ? Fail(error) : 0;
}

// NetDll_connect(caller, socket, *name, length).
// A datagram socket takes the address as its peer (0.0.0.0:0 drops it).
// A TCP connection goes out, to 127.0.0.1 only to this console, where
// nothing listens: refused at once. Anywhere else nothing answers (no
// link) and it times out after XNetStartupParams' connect timeout. A
// non-blocking socket returns WSAEWOULDBLOCK and WSAEALREADY while it
// tries.
HLE(kXam, 12, NetDll_connect) {
  uint32_t name = Arg(c, 2);
  std::unique_lock<std::mutex> lock(g_lock);
  uint32_t error;
  SocketRef s = Lookup(Arg(c, 0), Arg(c, 1), error);
  if (!s) return Fail(error);
  uint32_t addr = Rd32(name + 4);
  uint16_t port = Rd16(name + 2);
  if (s->stream() && s->flags & kConnected) return Fail(kWsaEisconn);
  if (!Reachable(*s, addr)) return Fail(kWsaEhostunreach);
  bool multicast = addr >> 28 == 0xE, other_loopback = addr >> 24 == 0x7F && addr != kLoopback;
  if (!s->stream()) {
    if (!addr != !port || (multicast && addr != 0xFFFFFFFF) || other_loopback) return Fail(kWsaEaddrnotavail);
    if (addr == s->peer && port == s->peer_port) return 0;
    if (addr == 0xFFFFFFFF && !(s->flags & kBroadcast)) return Fail(kWsaEacces);
    s->flags &= ~kConnected;
    s->peer = s->peer_port = 0;
    if (addr) {
      if (!(s->flags & kBound) && (error = BindPort(*s, 0))) return Fail(error);
      s->peer = addr;
      s->peer_port = port;
      s->flags |= kConnected;
    }
    return 0;
  }
  if (!port || !addr || addr == 0xFFFFFFFF || multicast || other_loopback) return Fail(kWsaEaddrnotavail);
  if (!(s->flags & kBound) && (error = BindPort(*s, 0))) return Fail(error);
  if (s->flags & kReconnect) {
    s->flags &= ~kReconnect;
    s->error = 0;
    s->listed = true;
  }
  if (s->state >= kTcpSynSent) return Fail(kWsaEalready);
  for (const SocketRef& o : g_stack->all)
    if (o != s && o->listed && o->stream() && o->port == s->port && o->peer == addr && o->peer_port == port)
      return Fail(kWsaEaddrinuse);
  s->peer = addr;
  s->peer_port = port;
  s->state = kTcpSynSent;
  uint32_t attempt = ++s->attempt;
  if (addr == kLoopback) EndConnection(*s, kWsaEconnrefused);
  else
    After(std::chrono::seconds(g_stack->callers[s->caller].params[kParamConnectTimeout]), ConnectTimeout,
          uint64_t(s->handle) << 32 | attempt);
  if (s->flags & kNonBlocking) return Fail(kWsaEwouldblock);
  for (;;) {
    SetSignal(s->event, 0);
    if (s->state != kTcpSynSent || s->closed) break;
    WaitSocket(lock, *s, {}, true);
  }
  return Fail(s->closed ? kWsaEnotsock : s->error);
}

// A receive (recv, recvfrom): a datagram socket must be bound, a TCP one
// connected (never, here). With nothing queued a non-blocking socket gets
// WSAEWOULDBLOCK, as does a second blocking receive; a blocking one waits
// up to SO_RCVTIMEO (WSAETIMEDOUT). A datagram larger than the buffer
// fills it and is lost: WSAEMSGSIZE.
static uint32_t Receive(ppc::Ctx& c, uint32_t from) {
  uint32_t buf = Arg(c, 2), len = Arg(c, 3);
  std::unique_lock<std::mutex> lock(g_lock);
  uint32_t error;
  SocketRef s = Lookup(Arg(c, 0), Arg(c, 1), error);
  if (!s) return Fail(error);
  if (from) {
    std::memset(Mem() + from, 0, 16);
    Wr32(Arg(c, 6), 16);
    Wr16(from, 2);
  }
  if (s->stream()) return Fail(kWsaEnotconn);
  if (!(s->flags & kBound)) return Fail(kWsaEinval);
  if (s->queue.empty()) {
    if (s->flags & kNonBlocking || s->receiving) return Fail(kWsaEwouldblock);
    s->receiving = true;
    auto deadline = Clock::now() + std::chrono::milliseconds(s->recv_timeout);
    bool timed_out = false;
    while (s->queue.empty() && !s->closed && !timed_out) {
      SetSignal(s->event, 0);
      if (!s->queue.empty() || s->closed) break;
      timed_out = !WaitSocket(lock, *s, deadline, !s->recv_timeout);
    }
    s->receiving = false;
    if (s->closed) return Fail(kWsaEnotsock);
    if (s->queue.empty()) return Fail(kWsaEtimedout);
  }
  Datagram d = std::move(s->queue.front());
  s->queue.pop_front();
  s->queued -= uint32_t(d.data.size());
  uint32_t n = std::min<uint32_t>(len, uint32_t(d.data.size()));
  std::memcpy(Mem() + buf, d.data.data(), n);
  if (from) {
    Wr16(from + 2, d.port);
    Wr32(from + 4, d.addr);
  }
  if (n < d.data.size()) return Fail(kWsaEmsgsize);
  return n;
}

// NetDll_recv(caller, socket, *buffer, length, flags).
HLE(kXam, 18, NetDll_recv) { return Receive(c, 0); }
// NetDll_recvfrom(caller, socket, *buffer, length, flags, *from, *length):
// the sender's sockaddr_in (16 bytes, family set before the receive).
HLE(kXam, 20, NetDll_recvfrom) { return Receive(c, Arg(c, 5)); }

// A send (send, sendto): only to a datagram socket's peer or address, a
// TCP socket never being connected here (WSAENOTCONN). sendto checks the
// address: 0, port 0 and 127.x but 127.0.0.1 WSAEADDRNOTAVAIL, multicast
// only on an insecure socket, broadcast only with SO_BROADCAST
// (WSAEACCES). The socket is bound first if it is not.
static uint32_t Send(ppc::Ctx& c, uint32_t to) {
  uint32_t buf = Arg(c, 2), len = Arg(c, 3);
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t error;
  SocketRef s = Lookup(Arg(c, 0), Arg(c, 1), error);
  if (!s) return Fail(error);
  uint32_t addr = s->peer;
  uint16_t port = s->peer_port;
  if (!s->stream() && to) {
    addr = Rd32(to + 4);
    port = Rd16(to + 2);
    if (!addr || !port || (addr >> 24 == 0x7F && addr != kLoopback)) return Fail(kWsaEaddrnotavail);
    if (addr >> 28 == 0xE && addr != 0xFFFFFFFF && !(s->flags & kInsecure)) return Fail(kWsaEaddrnotavail);
    if (addr == 0xFFFFFFFF && !(s->flags & kBroadcast)) return Fail(kWsaEacces);
    if (!(s->flags & kBound) && (error = BindPort(*s, 0))) return Fail(error);
  } else if (!(s->flags & kConnected)) {
    return Fail(kWsaEnotconn);
  }
  error = SendDatagram(*s, addr, port, buf, len);
  return error ? Fail(error) : len;
}

// NetDll_send(caller, socket, *buffer, length, flags).
HLE(kXam, 22, NetDll_send) { return Send(c, 0); }
// NetDll_sendto(caller, socket, *buffer, length, flags, *to, length).
HLE(kXam, 24, NetDll_sendto) { return Send(c, Arg(c, 5)); }

// ---- XNet ------------------------------------------------------------------
// NetDll_XNetRandom(caller, *buffer, length): random bytes once started.
HLE(kXam, 53, NetDll_XNetRandom) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  RandomBytes(Arg(c, 1), Arg(c, 2));
  return 0;
}

// NetDll_XNetCreateKey(caller, *XNKID, *XNKEY): a random id (a system link
// one, cross-platform with the privilege) and key.
HLE(kXam, 54, NetDll_XNetCreateKey) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  uint32_t id = Arg(c, 1);
  RandomBytes(id, 8);
  RandomBytes(Arg(c, 2), 16);
  Wr8(id, (Rd8(id) & 0x0F) | (Privileged(kPrivilegeCrossPlatform) ? kKidCrossPlatform : kKidSystemLink));
  return 0;
}

// NetDll_XNetRegisterKey(caller, *XNKID, *XNKEY): an online key, or a
// system link one (a cross-platform one with the privilege, a plain one
// without); WSAEALREADY when registered, WSA_E_NO_MORE past
// XNetStartupParams' count.
HLE(kXam, 55, NetDll_XNetRegisterKey) {
  uint32_t caller = Arg(c, 0), id = Arg(c, 1), key = Arg(c, 2);
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  uint8_t kind = Rd8(id) & kKidMask;
  bool privileged = Privileged(kPrivilegeCrossPlatform);
  if (kind != kKidOnline && kind != kKidMask && kind != (privileged ? kKidCrossPlatform : kKidSystemLink))
    return kWsaEinval;
  Caller* p = StartedCaller(caller);
  if (!p) return kWsaNotInitialised;
  if (FindKey(id)) return kWsaEalready;
  if (p->keys.size() >= p->params[kParamKeys]) return kWsaENoMore;
  Key k;
  std::memcpy(k.id, Mem() + id, 8);
  std::memcpy(k.key, Mem() + key, 16);
  p->keys.push_back(std::move(k));
  return 0;
}

// NetDll_XNetUnregisterKey(caller, *XNKID): the caller's own key, with its
// security associations and QoS listener.
HLE(kXam, 56, NetDll_XNetUnregisterKey) {
  uint32_t caller = Arg(c, 0), id = Arg(c, 1);
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  Caller* p = StartedCaller(caller);
  if (!p) return kWsaNotInitialised;
  uint32_t owner = 0;
  if (!FindKey(id, &owner) || owner != caller) return kWsaEinval;
  for (auto& sa : p->sas)
    if (sa && !std::memcmp(sa->key_id, Mem() + id, 8)) sa.reset();
  p->keys.erase(std::find_if(p->keys.begin(), p->keys.end(),
                             [&](const Key& k) { return !std::memcmp(k.id, Mem() + id, 8); }));
  return 0;
}

// NetDll_XNetXnAddrToInAddr(caller, *XNADDR, *XNKID, *IN_ADDR): this
// console's address is 127.0.0.1; another console's gets a security
// association under a registered key, whose address (0.x.y.z) titles then
// use. Online keys need a Live logon (WSAEADDRNOTAVAIL).
HLE(kXam, 57, NetDll_XNetXnAddrToInAddr) {
  uint32_t caller = Arg(c, 0), xnaddr = Arg(c, 1), id = Arg(c, 2), out = Arg(c, 3);
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  Wr32(out, 0);
  const uint8_t* enet = Mem() + xnaddr + kXnAddrEnet;
  if (std::all_of(enet, enet + 6, [](uint8_t b) { return !b; })) return kWsaEinval;
  if (!std::memcmp(enet, kMac, 6)) {
    Wr32(out, kLoopback);
    return 0;
  }
  uint8_t kind = Rd8(id) & kKidMask, peer = Rd8(xnaddr + kXnAddrKind);
  if ((kind == kKidCrossPlatform && !Rd32(xnaddr)) || (kind == kKidOnline && !Rd32(xnaddr + 4))) return kWsaEinval;
  bool plain = false;
  if (peer != 1 && (kind == kKidSystemLink || kind == kKidCrossPlatform)) {
    if (kind == kKidSystemLink && !peer && caller != 2) plain = true;
    else if (kind != kKidCrossPlatform || !peer || !Privileged(kPrivilegeCrossPlatform)) return kWsaEaddrnotavail;
  }
  if (kind == kKidOnline) return kWsaEaddrnotavail;
  uint32_t owner = 0;
  if (!FindKey(id, &owner) || (owner != caller && owner != 2)) return kWsaEinval;
  Caller* p = StartedCaller(caller);
  if (!p) return kWsaEinval;
  for (const auto& sa : p->sas)
    if (sa && !std::memcmp(sa->xnaddr + kXnAddrEnet, enet, 6) && !std::memcmp(sa->key_id, Mem() + id, 8)) {
      std::memcpy(sa->xnaddr, Mem() + xnaddr, kXnAddrSize);
      Wr32(out, sa->ina);
      return 0;
    }
  bool privileged = Privileged(kPrivilegeCrossPlatform);
  if (kind != (privileged ? kKidCrossPlatform : kKidSystemLink)) return kWsaENoMore;
  auto slot = std::find(p->sas.begin(), p->sas.end(), nullptr);
  if (slot == p->sas.end()) return kWsaENoMore;
  if (!++g_stack->sa_sequence) ++g_stack->sa_sequence;
  auto sa = std::make_unique<Sa>();
  sa->ina = uint32_t(g_stack->sa_sequence) << 8 | uint32_t(slot - p->sas.begin());
  std::memcpy(sa->xnaddr, Mem() + xnaddr, kXnAddrSize);
  std::memcpy(sa->key_id, Mem() + id, 8);
  sa->flags = peer != 1 && !plain ? 0x100 : 0;
  Wr32(out, sa->ina);
  *slot = std::move(sa);
  return 0;
}

// NetDll_XNetGetTitleXnAddr(caller, *XNADDR): XNET_GET_XNADDR_NONE (1)
// before startup. After, this console's address: no IP, its Ethernet
// address, byte 0x20 1. The IP configuration finds no link on its first
// tick, due at once, and settles with no address: Ethernet (2) and
// troubleshoot (0x8000). A stack started without the IP configuration
// stays pending (0).
HLE(kXam, 73, NetDll_XNetGetTitleXnAddr) {
  uint32_t xnaddr = Arg(c, 1);
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return 1;
  std::memset(Mem() + xnaddr, 0, kXnAddrSize);
  std::memcpy(Mem() + xnaddr + kXnAddrEnet, kMac, 6);
  Wr8(xnaddr + kXnAddrKind, 1);
  return g_stack->ip_config ? 0x8002 : 0;
}

// NetDll_XNetGetDebugXnAddr(caller, *XNADDR): a retail system has none,
// XNET_GET_XNADDR_NONE, and writes nothing.
HLE(kXam, 74, NetDll_XNetGetDebugXnAddr) { return 1; }

// NetDll_XNetDnsLookup(caller, name, event, **XNDNS): a host name (under
// 254 characters, labels up to 63, none empty) or WSAEFAULT; an event
// handle that names none is WSASYSCALLFAILURE. With no DNS server the
// lookup ends at once: XNDNS (status, address count, 8 addresses) with
// WSAHOST_NOT_FOUND, the event set.
HLE(kXam, 67, NetDll_XNetDnsLookup) {
  uint32_t caller = Arg(c, 0), name = Arg(c, 1), handle = Arg(c, 2), out = Arg(c, 3);
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  std::string host = name ? ReadString(name) : std::string();
  bool valid = name && !host.empty() && host.size() < 0xFE;
  for (size_t start = 0; valid && start < host.size();) {
    size_t dot = host.find('.', start), end = dot == std::string::npos ? host.size() : dot;
    if (end - start > 63 || (end == start && dot != std::string::npos)) valid = false;
    start = end + 1;
  }
  if (!valid) return kWsaEfault;
  uint32_t event = 0;
  if (handle) {
    event = HandleObject(handle);
    if (!event || (Rd8(event) != kNotificationEvent && Rd8(event) != kSynchronizationEvent))
      return kWsaSyscallFailure;
    Reference(event);
  }
  uint32_t va = SystemAlloc(0x28);
  Wr32(va, kWsaHostNotFound);
  g_stack->dns.push_back({caller, va, event});
  if (event) SetSignal(event, 1);
  Wr32(out, va);
  return 0;
}

// NetDll_XNetDnsRelease(caller, *XNDNS): the caller's own lookup.
HLE(kXam, 68, NetDll_XNetDnsRelease) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  auto& list = g_stack->dns;
  auto d = std::find_if(list.begin(), list.end(), [&](const Dns& d) { return d.va == Arg(c, 1); });
  if (d == list.end() || d->caller != Arg(c, 0)) return kWsaEinval;
  if (d->event) Dereference(d->event);
  SystemFree(d->va);
  list.erase(d);
  return 0;
}

// NetDll_XNetQosListen(caller, *XNKID, *data, size, bits per second,
// flags): answers to QoS probes under a registered key: enable (1),
// disable (2), data (4, up to XNetStartupParams' limit), bits per second
// (8), release (0x10).
HLE(kXam, 69, NetDll_XNetQosListen) {
  uint32_t caller = Arg(c, 0), data = Arg(c, 2), size = Arg(c, 3), flags = Arg(c, 5);
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  uint32_t owner = 0;
  Key* k = FindKey(Mem() + Arg(c, 1), &owner);
  if (!k || (owner != caller && owner != 2)) return kWsaEinval;
  if (flags & 0x10) {
    k->listen.reset();
    return 0;
  }
  if (!k->listen) k->listen = std::make_unique<QosListen>();
  if (flags & 4) {
    size = std::min(size, uint32_t(g_stack->callers[caller].params[kParamQosData]) * 4);
    k->listen->data.assign(Mem() + data, Mem() + data + size);
  }
  if (flags & 3) k->listen->enabled = flags & 1;
  return 0;
}

// NetDll_XNetQosLookup(caller, XNADDR count, **XNADDR, **XNKID, **XNKEY,
// address count, *IN_ADDR, *service ids, probes, bits per second, flags,
// event, **XNQOS). Entries for this console complete at once (contacted,
// with the listener's data, or plain complete with none); system link
// ones probe and fail after their retries (XNetStartupParams' timeout
// times retries + 1); the rest (online, security gateways) need Live and
// fail at once. The event is set as each completes.
HLE(kXam, 70, NetDll_XNetQosLookup) {
  uint32_t caller = Arg(c, 0), cxna = Arg(c, 1), apxna = Arg(c, 2), apxnkid = Arg(c, 3), apxnkey = Arg(c, 4),
           cina = Arg(c, 5), aina = Arg(c, 6), services = Arg(c, 7), probes = Arg(c, 8), flags = Arg(c, 10),
           handle = Arg(c, 11), out = Arg(c, 12);
  (void)flags;
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  auto ok = [](uint32_t base, uint32_t count) { return base > 0xFFFF && base + count * 4 > 0xFFFF; };
  if ((cxna && !(ok(apxna, cxna) && ok(apxnkid, cxna) && ok(apxnkey, cxna))) ||
      (cina && !(ok(aina, cina) && ok(services, cina)))) {
    Wr32(out, 0);
    return kWsaEinval;
  }
  for (uint32_t k = 0; k < cxna; k++) {
    uint32_t a = Rd32(apxna + 4 * k), i = Rd32(apxnkid + 4 * k), y = Rd32(apxnkey + 4 * k);
    if (a < 0x10000 || a + 0x24 < 0x10000 || i < 0x10000 || i + 8 < 0x10000 || y < 0x10000 || y + 0x10 < 0x10000) {
      Wr32(out, 0);
      return kWsaEinval;
    }
  }
  probes = std::min(probes, 0x40u);
  uint32_t count = cxna + cina, limit = uint32_t(g_stack->callers[caller < 4 ? caller : 0].params[kParamQosData]) * 4;
  uint32_t va = SystemAlloc(8 + count * (kQosInfo + limit));
  Wr32(va, count);
  Wr32(va + 4, count);
  uint32_t event = handle ? HandleObject(handle) : 0;
  if (event && Rd8(event) != kNotificationEvent && Rd8(event) != kSynchronizationEvent) event = 0;
  if (event) Reference(event);
  Qos q{caller, va, event, ++g_stack->qos_ids};
  g_stack->qos.push_back(q);
  const auto& p = g_stack->callers[caller < 4 ? caller : 0].params;
  auto probing = std::chrono::seconds(uint32_t(p[kParamProbeTimeout]) * (p[kParamProbeRetries] + 1u));
  for (uint32_t k = 0; k < count; k++) {
    uint32_t info = va + 8 + kQosInfo * k;
    if (k >= cxna) {  // a security gateway: Live's
      QosEntryDone(q, k);
      continue;
    }
    uint32_t xnaddr = Rd32(apxna + 4 * k), id = Rd32(apxnkid + 4 * k);
    uint8_t kind = Rd8(id) & kKidMask, peer = Rd8(xnaddr + kXnAddrKind);
    const uint8_t* enet = Mem() + xnaddr + kXnAddrEnet;
    bool fail = (kind != kKidSystemLink && kind != kKidCrossPlatform && kind != kKidOnline) ||
                std::all_of(enet, enet + 6, [](uint8_t b) { return !b; });
    if (!fail && peer != 1 && kind != kKidOnline &&
        !(kind == kKidSystemLink && !peer && caller != 2) &&
        !(kind == kKidCrossPlatform && peer && Privileged(kPrivilegeCrossPlatform)))
      fail = true;
    if (!fail && !std::memcmp(enet, kMac, 6)) {
      const Key* key = FindKey(id);
      if (key && key->listen) {
        uint8_t bits = 3 | (key->listen->enabled ? 0 : 4);
        Wr16(info + 2, uint16_t(probes));
        Wr16(info + 4, uint16_t(probes));
        if (!key->listen->data.empty()) {
          uint32_t data = va + 8 + count * kQosInfo + k * limit;
          std::memcpy(Mem() + data, key->listen->data.data(), key->listen->data.size());
          Wr32(info + 8, data);
          Wr16(info + 6, uint16_t(key->listen->data.size()));
          bits |= 8;
        }
        Wr8(info, bits);
        Wr32(info + 0x10, 100000000);
        Wr32(info + 0x14, 100000000);
      }
      QosEntryDone(q, k);
    } else if (!fail && kind == kKidSystemLink && !Privileged(kPrivilegeCrossPlatform)) {
      After(probing, QosProbesEnd, uint64_t(q.id) << 32 | k);
    } else {
      QosEntryDone(q, k);
    }
  }
  Wr32(out, va);
  return 0;
}

// NetDll_XNetQosRelease(caller, *XNQOS): the caller's own lookup.
HLE(kXam, 72, NetDll_XNetQosRelease) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_stack) return kWsaNotInitialised;
  auto& list = g_stack->qos;
  auto q = std::find_if(list.begin(), list.end(), [&](const Qos& q) { return q.va == Arg(c, 1); });
  if (q == list.end() || q->caller != Arg(c, 0)) return kWsaEinval;
  if (q->event) Dereference(q->event);
  SystemFree(q->va);
  list.erase(q);
  return 0;
}

// XNetLogonGetTitleID(service): the title Live's logon is for, or for a
// service a different title's when the logon names one. Only signing in to
// Live (or the system updater's logon) sets it; without, xam returns 0.
HLE(kXam, 310, XNetLogonGetTitleID) { return 0; }

}  // namespace recomp::xbox360
