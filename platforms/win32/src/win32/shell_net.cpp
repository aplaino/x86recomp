// shfolder and shell32 (special folders, directory trees, command lines)
// and wsock32 (by ordinal).
//
// Special folders live in the per-user tree (see HostPath): a game that
// saves under My Documents writes to $XDG_DATA_HOME/<id>/drive_c/
// Documents and Settings/Player/My Documents/...
//
// Networking: wsock32 only answers host-name queries (DirectPlay8 is not
// provided).

#include <sys/stat.h>

#include <cstring>
#include <string>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"

namespace recomp::hle {
namespace {

// x64 guests see Windows 10 (C:\Users\Player), 32-bit ones Windows XP.
std::string FolderFor(uint32_t csidl) {
  if (g_proc->x64) {
    const std::string user = "C:\\Users\\Player";
    switch (csidl & 0xFF) {
      case 0x05: return user + "\\Documents";            // CSIDL_PERSONAL
      case 0x1A: return user + "\\AppData\\Roaming";    // CSIDL_APPDATA
      case 0x1C: return user + "\\AppData\\Local";      // CSIDL_LOCAL_APPDATA
      case 0x23: return "C:\\ProgramData";              // CSIDL_COMMON_APPDATA
      case 0x2E: return "C:\\Users\\Public\\Documents";  // CSIDL_COMMON_DOCUMENTS
      case 0x26: return "C:\\Program Files";
      case 0x24: return "C:\\Windows";
      case 0x25: return "C:\\Windows\\system32";
      case 0x10: return user + "\\Desktop";
      default: return {};
    }
  }
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

uint32_t GetFolderPathA(Ctx& c) {
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

// shfolder.dll is the redistributable for systems before Windows 2000;
// later games import the same function from shell32.
HLE_NAMED("shfolder.dll", "SHGetFolderPathA", ShfolderGetFolderPathA, 5) {
  return GetFolderPathA(c);
}
HLE_NAMED("shell32.dll", "SHGetFolderPathA", Shell32GetFolderPathA, 5) {
  return GetFolderPathA(c);
}

uint32_t GetFolderPathW(Ctx& c) {
  uint32_t csidl = uint32_t(Arg(c, 1));
  gaddr out = Arg(c, 4);
  std::string p = FolderFor(csidl);
  if (p.empty()) {
    WarnF("SHGetFolderPathW(CSIDL %X): unknown folder\n", csidl);
    return win32::kEInvalidArg;
  }
  MakeDirs(HostPath(p));
  PutWStr(out, p);
  return 0;
}
HLE64("shfolder.dll", SHGetFolderPathW, 5) { return GetFolderPathW(c); }
HLE_NAMED_X("shell32.dll", "SHGetFolderPathW", Shell32GetFolderPathW, 5, true) {
  return GetFolderPathW(c);
}

// SHCreateDirectoryExW(hwnd, path, security): as SHCreateDirectoryExA.
uint32_t CreateDirectoryTree(const std::string& g);
HLE64("shell32.dll", SHCreateDirectoryExW, 3) { return CreateDirectoryTree(NarrowPath(GWStr(Arg(c, 1)))); }

// SHGetKnownFolderPath(id, flags, token, &path): Vista and later, so the
// layout is Windows 10's (C:\Users\<user>). The path is CoTaskMemAlloc'd.
// Like SHGetFolderPath it creates the folder whatever the flags say.
HLE64("shell32.dll", SHGetKnownFolderPath, 4) {
  struct Known {
    uint8_t id[16];  // the GUID as stored
    const char* path;
  };
  static const Known kKnown[] = {
      {{0xFF, 0x32, 0x5C, 0x4C, 0x9D, 0xBB, 0xB0, 0x43, 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E,
        0xAA, 0xA4},
       "C:\\Users\\Player\\Saved Games"},  // FOLDERID_SavedGames
      {{0x85, 0x27, 0xB3, 0xF1, 0xBA, 0x6F, 0xCF, 0x4F, 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15,
        0x70, 0x91},
       "C:\\Users\\Player\\AppData\\Local"},  // FOLDERID_LocalAppData
      {{0xDB, 0x85, 0xB6, 0x3E, 0xF9, 0x65, 0xF6, 0x4C, 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72,
        0x9F, 0x3D},
       "C:\\Users\\Player\\AppData\\Roaming"},  // FOLDERID_RoamingAppData
      {{0xD0, 0x9A, 0xD3, 0xFD, 0x8F, 0x23, 0xAF, 0x46, 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03,
        0x69, 0xC7},
       "C:\\Users\\Player\\Documents"},  // FOLDERID_Documents
  };
  gaddr id = Arg(c, 0), out = Arg(c, 3);
  WrPtr(out, 0);
  for (const Known& k : kKnown) {
    if (std::memcmp(GPtr(id), k.id, 16)) continue;
    MakeDirs(HostPath(k.path));
    gaddr buf = HeapAllocProcess(2 * (std::strlen(k.path) + 1), false);
    PutWStr(buf, k.path);
    WrPtr(out, buf);
    return 0;
  }
  WarnF("SHGetKnownFolderPath({%08X-...}): unknown folder\n", Rd32(id));
  return 0x80070002;  // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
}

// SHCreateDirectoryExA(hwnd, path, security): the directory and any missing
// parents. Returns a Win32 error code, not a BOOL.
uint32_t CreateDirectoryTree(const std::string& g) {
  bool absolute = (g.size() > 2 && g[1] == ':' && (g[2] == '\\' || g[2] == '/')) ||
                  g.rfind("\\\\", 0) == 0;
  if (!absolute) return 161;  // ERROR_BAD_PATHNAME
  struct stat st;
  if (stat(HostPath(g).c_str(), &st) == 0)
    return S_ISDIR(st.st_mode) ? win32::kErrorAlreadyExists
                               : win32::kErrorFileExists;
  MakeDirs(HostPath(g, PathAccess::kWrite));
  return win32::kErrorSuccess;
}
HLE("shell32.dll", SHCreateDirectoryExA, 3) { return CreateDirectoryTree(GStr(Arg(c, 1))); }

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
// CommandLineToArgvW(line, &argc): one LocalAlloc block holding the
// pointer array (null-terminated) and the strings; LocalFree frees it.
HLE64("shell32.dll", CommandLineToArgvW, 2) {
  std::vector<std::u16string> args = win32::SplitCommandLine(GWStr(Arg(c, 0)));
  const uint32_t ptr = g_proc->x64 ? 8 : 4;
  uint64_t size = ptr * (args.size() + 1);
  for (const auto& a : args) size += 2 * (a.size() + 1);
  gaddr block = HeapAllocProcess(size, true);
  gaddr str = block + ptr * (args.size() + 1);
  for (size_t i = 0; i < args.size(); i++) {
    WrPtr(block + ptr * i, str);
    std::memcpy(GPtr(str), args[i].data(), 2 * args[i].size());
    str += 2 * (args[i].size() + 1);
  }
  Wr32(Arg(c, 1), uint32_t(args.size()));
  return block;
}

}  // namespace recomp::hle
