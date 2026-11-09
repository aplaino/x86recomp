// ws2_32 (Winsock 2): UDP and TCP sockets on host BSD sockets. SOCKETs are
// handles in the kernel32 table; sockaddrs are converted (the families'
// values differ, and BSD's have a length byte). Imported by ordinal or by
// name, so each function is registered under both.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sys/select.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

// Windows' address families and the WSA error codes used here.
constexpr uint16_t kAfInet = 2, kAfInet6 = 23;
constexpr uint32_t kWsaEInval = 10022, kWsaEWouldBlock = 10035, kWsaEMsgSize = 10040,
                   kWsaEAfNoSupport = 10047, kWsaEAddrInUse = 10048,
                   kWsaEAddrNotAvail = 10049, kWsaENetDown = 10050, kWsaENetUnreach = 10051,
                   kWsaEConnReset = 10054, kWsaENotSock = 10038, kWsaEAcces = 10013,
                   kWsaEConnRefused = 10061, kWsaEHostUnreach = 10065;

struct SocketObject : KObject {
  SocketObject() : KObject(Type::Socket) {}
  ~SocketObject() override {
    if (fd >= 0) close(fd);
  }
  int fd = -1;
};

thread_local uint32_t t_wsa_error = 0;

uint32_t WsaError(int e) {
  switch (e) {
    case EAGAIN: return kWsaEWouldBlock;
    case EINVAL: return kWsaEInval;
    case EMSGSIZE: return kWsaEMsgSize;
    case EAFNOSUPPORT: return kWsaEAfNoSupport;
    case EADDRINUSE: return kWsaEAddrInUse;
    case EADDRNOTAVAIL: return kWsaEAddrNotAvail;
    case ENETUNREACH: return kWsaENetUnreach;
    case EHOSTUNREACH: return kWsaEHostUnreach;
    case ECONNRESET: return kWsaEConnReset;
    case ECONNREFUSED: return kWsaEConnRefused;
    case EACCES: return kWsaEAcces;
    case ENOTSOCK: return kWsaENotSock;
    default: return kWsaENetDown;
  }
}

constexpr uint32_t kSocketError = 0xFFFFFFFF;  // SOCKET_ERROR (int -1)

// Sends never raise SIGPIPE (Winsock reports the error instead).
#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0;  // macOS: SO_NOSIGPIPE on the socket
#endif

uint64_t Fail(uint32_t wsa) {
  t_wsa_error = wsa;
  return kSocketError;
}
uint64_t FailErrno() { return Fail(WsaError(errno)); }

uint64_t InvalidSocket() { return g_proc->x64 ? ~0ull : 0xFFFFFFFFu; }

int SocketFd(uint64_t s) {
  auto o = std::dynamic_pointer_cast<SocketObject>(GetHandle(uint32_t(s)));
  return o ? o->fd : -1;
}

int HostFamily(uint16_t af) {
  return af == kAfInet ? AF_INET : af == kAfInet6 ? AF_INET6 : -1;
}

// Guest sockaddr (Windows layout) to host; false if the family is unknown.
bool ToHost(gaddr sa, uint32_t len, sockaddr_storage* out, socklen_t* out_len) {
  std::memset(out, 0, sizeof *out);
  uint16_t af = Rd16(sa);
  if (af == kAfInet && len >= 16) {
    auto* a = reinterpret_cast<sockaddr_in*>(out);
    a->sin_family = AF_INET;
    std::memcpy(&a->sin_port, GPtr(sa + 2), 2);
    std::memcpy(&a->sin_addr, GPtr(sa + 4), 4);
    *out_len = sizeof *a;
    return true;
  }
  if (af == kAfInet6 && len >= 28) {
    auto* a = reinterpret_cast<sockaddr_in6*>(out);
    a->sin6_family = AF_INET6;
    std::memcpy(&a->sin6_port, GPtr(sa + 2), 2);
    std::memcpy(&a->sin6_flowinfo, GPtr(sa + 4), 4);
    std::memcpy(&a->sin6_addr, GPtr(sa + 8), 16);
    std::memcpy(&a->sin6_scope_id, GPtr(sa + 24), 4);
    *out_len = sizeof *a;
    return true;
  }
  return false;
}

// Host sockaddr to the guest buffer `sa` of *len bytes (updated).
void ToGuest(const sockaddr_storage& in, gaddr sa, gaddr len_va) {
  if (!sa || !len_va) return;
  uint32_t cap = Rd32(len_va);
  uint8_t b[28] = {};
  uint32_t n = 0;
  if (in.ss_family == AF_INET) {
    const auto* a = reinterpret_cast<const sockaddr_in*>(&in);
    b[0] = kAfInet;
    std::memcpy(b + 2, &a->sin_port, 2);
    std::memcpy(b + 4, &a->sin_addr, 4);
    n = 16;
  } else if (in.ss_family == AF_INET6) {
    const auto* a = reinterpret_cast<const sockaddr_in6*>(&in);
    b[0] = kAfInet6;
    std::memcpy(b + 2, &a->sin6_port, 2);
    std::memcpy(b + 4, &a->sin6_flowinfo, 4);
    std::memcpy(b + 8, &a->sin6_addr, 16);
    std::memcpy(b + 24, &a->sin6_scope_id, 4);
    n = 28;
  }
  std::memcpy(GPtr(sa), b, std::min(cap, n));
  Wr32(len_va, n);
}

}  // namespace

#define RECOMP_WS2(ord, name, nargs)                                                \
  static uint64_t ws_##name(::recomp::Ctx& c);                                      \
  static ::recomp::hle::Registrar ws_reg_##name{"ws2_32.dll", #name, nargs,         \
                                                &ws_##name, true};                  \
  static ::recomp::hle::Registrar ws_ord_##name{"ws2_32.dll", "#" #ord, nargs,      \
                                                &ws_##name, true};                  \
  static uint64_t ws_##name([[maybe_unused]] ::recomp::Ctx& c)

// WSADATA: wVersion is the version asked for when it is 2.2 or lower (as
// Winsock answers; games check it), wHighVersion 2.2. Win64 moves
// iMaxSockets, iMaxUdpDg and lpVendorInfo before the strings.
RECOMP_WS2(115, WSAStartup, 2) {
  gaddr d = Arg(c, 1);
  const bool x64 = g_proc->x64;
  const uint16_t req = uint16_t(Arg(c, 0));
  const uint8_t major = req & 0xFF, minor = req >> 8;
  std::memset(GPtr(d), 0, x64 ? 408 : 400);
  Wr16(d + 0, major > 2 || (major == 2 && minor > 2) ? 0x0202 : req);
  Wr16(d + 2, 0x0202);
  PutStr(d + (x64 ? 16 : 4), "WinSock 2.0");
  PutStr(d + (x64 ? 16 + 257 : 4 + 257), "Running");
  return 0;
}
RECOMP_WS2(116, WSACleanup, 0) { return 0; }
RECOMP_WS2(111, WSAGetLastError, 0) { return t_wsa_error; }
RECOMP_WS2(112, WSASetLastError, 1) {
  t_wsa_error = uint32_t(Arg(c, 0));
  return 0;
}

RECOMP_WS2(23, socket, 3) {
  int af = HostFamily(uint16_t(Arg(c, 0)));
  if (af < 0) {
    t_wsa_error = kWsaEAfNoSupport;
    return InvalidSocket();
  }
  int fd = ::socket(af, int(Arg(c, 1)), int(Arg(c, 2)));
  if (fd < 0) {
    t_wsa_error = WsaError(errno);
    return InvalidSocket();
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  auto o = std::make_shared<SocketObject>();
  o->fd = fd;
  return NewHandle(o);
}

RECOMP_WS2(3, closesocket, 1) {
  if (SocketFd(Arg(c, 0)) < 0) return Fail(kWsaENotSock);
  CloseHandleValue(uint32_t(Arg(c, 0)));
  return 0;
}

RECOMP_WS2(2, bind, 3) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  sockaddr_storage sa;
  socklen_t len;
  if (!ToHost(Arg(c, 1), uint32_t(Arg(c, 2)), &sa, &len)) return Fail(kWsaEAfNoSupport);
  return ::bind(fd, reinterpret_cast<sockaddr*>(&sa), len) == 0 ? 0 : FailErrno();
}

// ioctlsocket: FIONBIO (non-blocking on/off) and FIONREAD.
RECOMP_WS2(10, ioctlsocket, 3) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  uint32_t cmd = uint32_t(Arg(c, 1));
  gaddr argp = Arg(c, 2);
  if (cmd == 0x8004667E) {  // FIONBIO
    int fl = fcntl(fd, F_GETFL);
    fl = Rd32(argp) ? fl | O_NONBLOCK : fl & ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, fl) == 0 ? 0 : FailErrno();
  }
  if (cmd == 0x4004667F) {  // FIONREAD
    int n = 0;
    if (ioctl(fd, FIONREAD, &n) != 0) return FailErrno();
    Wr32(argp, uint32_t(n));
    return 0;
  }
  WarnF("ioctlsocket(%08X): not supported\n", cmd);
  return Fail(kWsaEInval);
}

RECOMP_WS2(20, sendto, 6) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  sockaddr_storage sa;
  socklen_t len = 0;
  gaddr to = Arg(c, 4);
  if (to && !ToHost(to, uint32_t(Arg(c, 5)), &sa, &len)) return Fail(kWsaEAfNoSupport);
  ssize_t n = ::sendto(fd, GPtr(Arg(c, 1)), size_t(uint32_t(Arg(c, 2))),
                       int(Arg(c, 3)) | kNoSignal,
                       to ? reinterpret_cast<sockaddr*>(&sa) : nullptr, len);
  return n < 0 ? FailErrno() : uint64_t(uint32_t(n));
}

RECOMP_WS2(17, recvfrom, 6) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  sockaddr_storage sa{};
  socklen_t len = sizeof sa;
  ssize_t n = ::recvfrom(fd, GPtr(Arg(c, 1)), size_t(uint32_t(Arg(c, 2))), int(Arg(c, 3)),
                         reinterpret_cast<sockaddr*>(&sa), &len);
  if (n < 0) return FailErrno();
  ToGuest(sa, Arg(c, 4), Arg(c, 5));
  return uint32_t(n);
}

// Byte order: the guest is little-endian, network order big-endian.
RECOMP_WS2(8, htonl, 1) { return __builtin_bswap32(uint32_t(Arg(c, 0))); }
RECOMP_WS2(14, ntohl, 1) { return __builtin_bswap32(uint32_t(Arg(c, 0))); }
RECOMP_WS2(9, htons, 1) { return __builtin_bswap16(uint16_t(Arg(c, 0))); }
RECOMP_WS2(15, ntohs, 1) { return __builtin_bswap16(uint16_t(Arg(c, 0))); }

// inet_pton / inet_ntop: the host's, with Windows' family values.
HLE64("ws2_32.dll", inet_pton, 3) {
  int af = HostFamily(uint16_t(Arg(c, 0)));
  if (af < 0) {
    t_wsa_error = kWsaEAfNoSupport;
    return kSocketError;
  }
  return uint32_t(::inet_pton(af, GStr(Arg(c, 1)).c_str(), GPtr(Arg(c, 2))));
}
HLE64("ws2_32.dll", inet_ntop, 4) {
  int af = HostFamily(uint16_t(Arg(c, 0)));
  if (af < 0) {
    t_wsa_error = kWsaEAfNoSupport;
    return 0;
  }
  char buf[64];
  if (!::inet_ntop(af, GPtr(Arg(c, 1)), buf, sizeof buf)) {
    t_wsa_error = WsaError(errno);
    return 0;
  }
  uint64_t cap = Arg(c, 3);
  if (std::strlen(buf) + 1 > cap) {
    t_wsa_error = kWsaEInval;
    return 0;
  }
  PutStr(Arg(c, 2), buf);
  return Arg(c, 2);
}

// The functions below use 32-bit layouts (fd_set of 4-byte SOCKETs,
// hostent): registered for 32-bit guests only.
#define RECOMP_WS2_32(ord, name, nargs)                                             \
  static uint64_t ws_##name(::recomp::Ctx& c);                                      \
  static ::recomp::hle::Registrar ws_reg_##name{"ws2_32.dll", #name, nargs,         \
                                                &ws_##name};                        \
  static ::recomp::hle::Registrar ws_ord_##name{"ws2_32.dll", "#" #ord, nargs,      \
                                                &ws_##name};                        \
  static uint64_t ws_##name([[maybe_unused]] ::recomp::Ctx& c)

RECOMP_WS2_32(4, connect, 3) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  sockaddr_storage sa;
  socklen_t len;
  if (!ToHost(Arg(c, 1), uint32_t(Arg(c, 2)), &sa, &len)) return Fail(kWsaEAfNoSupport);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), len) == 0) return 0;
  return Fail(errno == EINPROGRESS ? kWsaEWouldBlock : WsaError(errno));
}

RECOMP_WS2_32(6, getsockname, 3) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  sockaddr_storage sa{};
  socklen_t len = sizeof sa;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) != 0) return FailErrno();
  ToGuest(sa, Arg(c, 1), Arg(c, 2));
  return 0;
}

RECOMP_WS2_32(13, listen, 2) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  return ::listen(fd, int(Arg(c, 1))) == 0 ? 0 : FailErrno();
}

RECOMP_WS2_32(1, accept, 3) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) {
    t_wsa_error = kWsaENotSock;
    return InvalidSocket();
  }
  sockaddr_storage sa{};
  socklen_t len = sizeof sa;
  int nfd = ::accept(fd, reinterpret_cast<sockaddr*>(&sa), &len);
  if (nfd < 0) {
    t_wsa_error = WsaError(errno);
    return InvalidSocket();
  }
  fcntl(nfd, F_SETFD, FD_CLOEXEC);
  ToGuest(sa, Arg(c, 1), Arg(c, 2));
  auto o = std::make_shared<SocketObject>();
  o->fd = nfd;
  return NewHandle(o);
}

RECOMP_WS2_32(19, send, 4) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  ssize_t n = ::send(fd, GPtr(Arg(c, 1)), size_t(uint32_t(Arg(c, 2))), int(Arg(c, 3)) | kNoSignal);
  return n < 0 ? FailErrno() : uint64_t(uint32_t(n));
}

RECOMP_WS2_32(16, recv, 4) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  ssize_t n = ::recv(fd, GPtr(Arg(c, 1)), size_t(uint32_t(Arg(c, 2))), int(Arg(c, 3)));
  return n < 0 ? FailErrno() : uint64_t(uint32_t(n));
}

// shutdown: SD_RECEIVE/SD_SEND/SD_BOTH are SHUT_RD/WR/RDWR's values.
RECOMP_WS2_32(22, shutdown, 2) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  return ::shutdown(fd, int(Arg(c, 1))) == 0 ? 0 : FailErrno();
}

// setsockopt / getsockopt: SOL_SOCKET options with the same meaning
// (SO_BROADCAST, SO_REUSEADDR, SO_SNDBUF, SO_RCVBUF), whose Windows values
// differ from the host's; others are accepted and ignored.
int HostSockOpt(uint32_t level, uint32_t name, int* host_level) {
  if (level == 0xFFFF) {  // SOL_SOCKET
    *host_level = SOL_SOCKET;
    switch (name) {
      case 0x0004: return SO_REUSEADDR;
      case 0x0020: return SO_BROADCAST;
      case 0x1001: return SO_SNDBUF;
      case 0x1002: return SO_RCVBUF;
    }
  }
  return -1;
}

RECOMP_WS2_32(21, setsockopt, 5) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  int level;
  const int name = HostSockOpt(uint32_t(Arg(c, 1)), uint32_t(Arg(c, 2)), &level);
  if (name < 0) return 0;
  int v = Arg(c, 3) && Arg(c, 4) >= 4 ? int(Rd32(Arg(c, 3))) : 0;
  return ::setsockopt(fd, level, name, &v, sizeof v) == 0 ? 0 : FailErrno();
}

RECOMP_WS2_32(7, getsockopt, 5) {
  int fd = SocketFd(Arg(c, 0));
  if (fd < 0) return Fail(kWsaENotSock);
  int level;
  const int name = HostSockOpt(uint32_t(Arg(c, 1)), uint32_t(Arg(c, 2)), &level);
  int v = 0;
  socklen_t len = sizeof v;
  if (name >= 0 && ::getsockopt(fd, level, name, &v, &len) != 0) return FailErrno();
  if (Arg(c, 3)) Wr32(Arg(c, 3), uint32_t(v));
  if (Arg(c, 4)) Wr32(Arg(c, 4), 4);
  return 0;
}

// select(nfds, read, write, except, timeout): fd_set { u_int fd_count;
// SOCKET fd_array[64]; }, rewritten to the sockets that are ready.
RECOMP_WS2_32(18, select, 5) {
  const gaddr sets[3] = {gaddr(Arg(c, 1)), gaddr(Arg(c, 2)), gaddr(Arg(c, 3))};
  fd_set host[3];
  int maxfd = -1;
  for (int k = 0; k < 3; k++) {
    FD_ZERO(&host[k]);
    if (!sets[k]) continue;
    const uint32_t n = std::min<uint32_t>(Rd32(sets[k]), 64);
    for (uint32_t i = 0; i < n; i++) {
      int fd = SocketFd(Rd32(sets[k] + 4 + 4 * i));
      if (fd < 0) return Fail(kWsaENotSock);
      FD_SET(fd, &host[k]);
      maxfd = std::max(maxfd, fd);
    }
  }
  timeval tv{}, *ptv = nullptr;
  if (const gaddr t = Arg(c, 4)) {
    tv.tv_sec = int32_t(Rd32(t));
    tv.tv_usec = int32_t(Rd32(t + 4));
    ptv = &tv;
  }
  int r = ::select(maxfd + 1, &host[0], &host[1], &host[2], ptv);
  if (r < 0) return FailErrno();
  for (int k = 0; k < 3; k++) {
    if (!sets[k]) continue;
    const uint32_t n = std::min<uint32_t>(Rd32(sets[k]), 64);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
      const uint32_t s = Rd32(sets[k] + 4 + 4 * i);
      if (FD_ISSET(SocketFd(s), &host[k])) Wr32(sets[k] + 4 + 4 * kept++, s);
    }
    Wr32(sets[k], kept);
  }
  return uint32_t(r);
}

RECOMP_WS2_32(151, __WSAFDIsSet, 2) {
  const uint32_t s = uint32_t(Arg(c, 0));
  const gaddr set = Arg(c, 1);
  const uint32_t n = std::min<uint32_t>(Rd32(set), 64);
  for (uint32_t i = 0; i < n; i++)
    if (Rd32(set + 4 + 4 * i) == s) return 1;
  return 0;
}

RECOMP_WS2_32(11, inet_addr, 1) {
  in_addr a;
  return ::inet_aton(GStr(Arg(c, 0)).c_str(), &a) ? a.s_addr : 0xFFFFFFFFu;  // INADDR_NONE
}

// inet_ntoa: a per-thread buffer, as Winsock's.
RECOMP_WS2_32(12, inet_ntoa, 1) {
  thread_local gaddr buf = 0;
  if (!buf) buf = HeapAllocProcess(16, true);
  in_addr a;
  a.s_addr = uint32_t(Arg(c, 0));
  PutStr(buf, ::inet_ntoa(a));
  return buf;
}

RECOMP_WS2_32(57, gethostname, 2) {
  const uint32_t cap = uint32_t(Arg(c, 1));
  if (cap < 10) return Fail(10014);  // WSAEFAULT
  PutStr(Arg(c, 0), "localhost");
  return 0;
}

// gethostbyname: names are not resolved (WSAHOST_NOT_FOUND).
RECOMP_WS2_32(52, gethostbyname, 1) {
  t_wsa_error = 11001;
  return 0;
}

// ---- WSA async/event and DNS, offline -----------------------------------------
RECOMP_WS2_32(101, WSAAsyncSelect, 4) { return 0; }
RECOMP_WS2_32(102, WSAEventSelect, 3) { return 0; }
RECOMP_WS2_32(31, WSACreateEvent, 0) {
  static uint32_t ev = 0x80010000;
  return ++ev;
}
RECOMP_WS2_32(29, WSACloseEvent, 1) { return 1; }
RECOMP_WS2_32(36, WSAEnumNetworkEvents, 3) {
  if (Arg(c, 2)) std::memset(GPtr(Arg(c, 2)), 0, 44); // WSA_NETWORK_EVENTS
  return 0;
}
RECOMP_WS2_32(58, WSAWaitForMultipleEvents, 5) { return 0x102; } // WSA_WAIT_TIMEOUT
RECOMP_WS2_32(51, gethostbyaddr, 3) { t_wsa_error = 11001; return 0; }
RECOMP_WS2_32(55, getservbyname, 2) { t_wsa_error = 11001; return 0; }
RECOMP_WS2_32(56, getservbyport, 2) { t_wsa_error = 11001; return 0; }

}  // namespace recomp::hle
