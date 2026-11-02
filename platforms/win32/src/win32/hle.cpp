// The Win32 side of the HLE (win32/hle.h).

#include "win32/hle.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "runtime/log.h"
#include "win32/process.h"
#include "win32/win32.h"

namespace recomp::hle {

namespace {

// Real export tables of provided DLLs whose games fetch every entry point
// up front (see Exports in win32/hle.h).
std::map<std::string, std::set<std::string>>& DeclaredExports() {
  static std::map<std::string, std::set<std::string>> e;
  return e;
}

// Pseudo HMODULEs for DLLs we provide. Real modules are guest pointers to
// a PE header; nothing in the game dereferences these, so a recognisable
// value outside the arena's used ranges is enough.
const std::vector<std::string>& KnownDlls() {
  static const std::vector<std::string> dlls = {
      "kernel32.dll", "user32.dll",  "gdi32.dll",    "advapi32.dll",
      "ole32.dll",    "version.dll", "winmm.dll",    "wsock32.dll",
      "ddraw.dll",    "mss32.dll",   "binkw32.dll",  "dbghelp.dll",
      "imm32.dll",    "shfolder.dll", "ntdll.dll",   "opengl32.dll"};
  return dlls;
}

gaddr ModuleRegion() { return g_proc->x64 ? kModuleRegion64 : kModuleRegion; }

}  // namespace

Exports::Exports(const char* dll, std::initializer_list<const char*> names) {
  auto& s = DeclaredExports()[dll];
  for (const char* n : names) s.insert(n);
}

std::string NormaliseDll(std::string dll) {
  dll = win32::Lower(dll);
  size_t slash = dll.find_last_of("\\/");
  if (slash != std::string::npos) dll = dll.substr(slash + 1);
  if (dll.find('.') == std::string::npos) dll += ".dll";
  // The UCRT's API sets (api-ms-win-crt-runtime-l1-1-0.dll, ...) are all
  // ucrtbase.dll, as Windows' API set map resolves them.
  if (dll.rfind("api-ms-win-crt-", 0) == 0) return "ucrtbase.dll";
  return dll;
}

gaddr ImportThunk(const std::string& dll, const std::string& name) {
  return BindImport(NormaliseDll(dll), name);
}

gaddr ModuleHandle(const std::string& dll) {
  std::string n = NormaliseDll(dll);
  const auto& k = KnownDlls();
  auto it = std::find(k.begin(), k.end(), n);
  if (it == k.end()) return 0;
  return ModuleRegion() + uint32_t(it - k.begin()) * 0x10000;
}

gaddr ProcAddress(gaddr hmodule, const std::string& name) {
  const auto& k = KnownDlls();
  gaddr idx = (hmodule - ModuleRegion()) / 0x10000;
  if (hmodule < ModuleRegion() || idx >= k.size()) return 0;
  const std::string& dll = k[idx];
  if (IsDataExport(dll, name)) return BindImport(dll, name);
  if (!HasHandler(dll, name)) {
    auto ex = DeclaredExports().find(dll);
    if (ex != DeclaredExports().end() && ex->second.count(name))
      return ThunkFor(dll, name);  // a real export; fatal if ever called
    WarnF("GetProcAddress(%s, %s): not implemented, returning NULL\n",
          dll.c_str(), name.c_str());
    return 0;
  }
  return ThunkFor(dll, name);
}

// TEB LastErrorValue: +0x34 (32-bit), +0x68 (x64).
void SetLastError(uint32_t e) {
  Wr32(Teb(CurrentThread()) + (g_proc->x64 ? 0x68 : 0x34), e);
}

uint32_t CopyOut(gaddr buf, uint32_t size, const std::string& s) {
  if (s.size() + 1 > size) return uint32_t(s.size() + 1);
  PutStr(buf, s);
  return uint32_t(s.size());
}

uint32_t PutCounted(gaddr buf, uint32_t cap, const std::string& s, bool wide) {
  if (!cap) return uint32_t(s.size() + 1);
  if (s.size() + 1 > cap) {
    SetLastError(win32::kErrorInsufficientBuffer);
    return 0;
  }
  if (wide)
    PutWStr(buf, s);
  else
    PutStr(buf, s);
  return uint32_t(s.size() + 1);
}

}  // namespace recomp::hle
