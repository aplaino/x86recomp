// shfolder (special folders) and wsock32 (by ordinal).
//
// Special folders live in the per-user tree (see HostPath): a game that
// saves under My Documents writes to $XDG_DATA_HOME/<id>/drive_c/
// Documents and Settings/Player/My Documents/...
//
// Networking: wsock32 only answers host-name queries (DirectPlay8 is not
// provided).

#include <cstring>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

std::string FolderFor(uint32_t csidl) {
  const std::string user = "C:\\Documents and Settings\\Player";
  switch (csidl & 0xFF) {
    case 0x05: return user + "\\My Documents";               // CSIDL_PERSONAL
    case 0x1A: return user + "\\Application Data";           // CSIDL_APPDATA
    case 0x1C: return user + "\\Local Settings\\Application Data";
    case 0x23: return "C:\\Documents and Settings\\All Users\\Application Data";
    case 0x2E: return "C:\\Documents and Settings\\All Users\\Documents";
    case 0x26: return "C:\\Program Files";
    case 0x24: return "C:\\WINDOWS";
    case 0x25: return "C:\\WINDOWS\\system32";
    case 0x10: return user + "\\Desktop";
    default: return {};
  }
}

HLE("shfolder.dll", SHGetFolderPathA, 5) {
  uint32_t csidl = Arg(c, 1), out = Arg(c, 4);
  std::string p = FolderFor(csidl);
  if (p.empty()) {
    WarnF("SHGetFolderPathA(CSIDL %X): unknown folder\n", csidl);
    return win32::kEInvalidArg;
  }
  // Create it on the host either way: Windows has these folders.
  MakeDirs(HostPath(p));
  PutStr(out, p);
  return 0;  // S_OK
}

HLE("shfolder.dll", SHGetFolderPathW, 5) {
  uint32_t csidl = Arg(c, 1), out = Arg(c, 4);
  std::string p = FolderFor(csidl);
  if (p.empty()) {
    WarnF("SHGetFolderPathW(CSIDL %X): unknown folder\n", csidl);
    return win32::kEInvalidArg;
  }
  MakeDirs(HostPath(p));
  PutWStr(out, p);
  return 0;
}

// ---- wsock32 ------------------------------------------------------------------------

HLE_NAMED("wsock32.dll", "#115", WSAStartup, 2) {  // WSAStartup(version, WSADATA*)
  uint32_t wsa = Arg(c, 1);
  std::memset(GPtr(wsa), 0, 400);
  Wr16(wsa + 0, 0x0101);
  Wr16(wsa + 2, 0x0202);
  PutStr(wsa + 4, "WinSock 2.0");
  PutStr(wsa + 261, "Running");
  Wr16(wsa + 390, 32767);  // iMaxSockets
  Wr16(wsa + 392, 65467);  // iMaxUdpDg
  return 0;
}

HLE_NAMED("wsock32.dll", "#116", WSACleanup, 0) { return 0; }  // WSACleanup

HLE_NAMED("wsock32.dll", "#57", gethostname, 2) {  // gethostname(buf, len)
  std::string n = "localhost";
  if (Arg(c, 1) < n.size() + 1) return 0xFFFFFFFF;
  PutStr(Arg(c, 0), n);
  return 0;
}

// gethostbyname: a static HOSTENT for 127.0.0.1, whatever the name.
HLE_NAMED("wsock32.dll", "#52", gethostbyname, 1) {
  static uint32_t hostent = 0;
  if (!hostent) {
    hostent = HeapAllocProcess(64, true);
    uint32_t name = hostent + 16, addr = hostent + 32, list = hostent + 40;
    PutStr(name, "localhost");
    Wr8(addr + 0, 127);
    Wr8(addr + 3, 1);
    Wr32(list, addr);
    Wr32(list + 4, 0);
    Wr32(hostent + 0, name);     // h_name
    Wr32(hostent + 4, list + 4); // h_aliases: empty list
    Wr16(hostent + 8, 2);        // h_addrtype AF_INET
    Wr16(hostent + 10, 4);       // h_length
    Wr32(hostent + 12, list);    // h_addr_list
  }
  return hostent;
}

HLE_NAMED("wsock32.dll", "#14", htonl, 1) { return __builtin_bswap32(Arg(c, 0)); }  // htonl

HLE_NAMED("wsock32.dll", "#10", ioctlsocket, 3) {  // ioctlsocket: no sockets exist
  return 0xFFFFFFFF;
}

}  // namespace
}  // namespace recomp::hle
