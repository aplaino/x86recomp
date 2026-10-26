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

}  // namespace

// NetDll_WSAStartup(caller, version, *data): WSADATA with the version asked
// for, the highest (2.2), no description or status and no limits, as xam
// fills it.
HLE(kXam, 1, NetDll_WSAStartup) {
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

// NetDll_XNetStartup(caller, *parameters).
HLE(kXam, 51, NetDll_XNetStartup) { return Caller(Arg(c, 0)) ? 0 : kWsaEinval; }

}  // namespace recomp::xbox360
