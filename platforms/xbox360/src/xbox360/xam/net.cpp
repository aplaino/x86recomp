// xam: networking (NetDll_*). There is no network interface here: a
// console with no cable connected.

#include <cstring>

#include "xbox360/hle.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kWsaEinval = 10022;
// NetDll_* calls' first argument: who calls, a title (1) or the system (2).
bool Caller(uint32_t c) { return c == 1 || c == 2; }

// NetDll_WSAStartup(caller, version, *data): WSADATA with the version asked
// for, the highest (2.2), no description or status and no limits, as xam
// fills it. NetDll_WSAStartupEx(caller, version, *data, title version) is
// the same; xam passes the title's version on without checking it.
uint32_t WsaStartup(ppc::Ctx& c) {
  if (!Caller(Arg(c, 0))) return kWsaEinval;
  uint32_t data = Arg(c, 2);
  Wr16(data, uint16_t(Arg(c, 1)));
  Wr16(data + 2, 0x0202);
  Wr8(data + 4, 0);
  Wr8(data + 0x105, 0);
  Wr16(data + 0x186, 0);
  Wr16(data + 0x188, 0);
  Wr32(data + 0x18C, 0);
  return 0;
}

}  // namespace

HLE(kXam, 1, NetDll_WSAStartup) { return WsaStartup(c); }
HLE(kXam, 36, NetDll_WSAStartupEx) { return WsaStartup(c); }

// NetDll_XNetStartup(caller, *parameters); NetDll_XNetStartupEx(caller,
// *parameters, version) is the same with the title's version, which xam
// passes on to its network stack without checking it.
HLE(kXam, 51, NetDll_XNetStartup) { return Caller(Arg(c, 0)) ? 0 : kWsaEinval; }
HLE(kXam, 80, NetDll_XNetStartupEx) { return Caller(Arg(c, 0)) ? 0 : kWsaEinval; }

// NetDll_XNetGetEthernetLinkStatus(caller): no link, 0. xam does not check
// the caller here.
HLE(kXam, 75, NetDll_XNetGetEthernetLinkStatus) { return 0; }

}  // namespace recomp::xbox360
