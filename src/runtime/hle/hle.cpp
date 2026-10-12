#include "runtime/hle/hle.h"

#include <cstdlib>

#include <algorithm>
#include <map>
#include <mutex>
#include <deque>
#include <vector>

#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/watchdog.h"

namespace recomp::hle {

thread_local int t_hle_depth = 0;

namespace {

std::map<std::pair<std::string, std::string>, Def>& Registry() {
  static std::map<std::pair<std::string, std::string>, Def> r;
  return r;
}

struct Slot {
  std::string dll, name;
  const Def* def;  // nullptr: unimplemented
  // DirectDraw/Direct3D 7 entry points run under one process-wide lock,
  // like DirectDraw with DDSCL_MULTITHREADED (which the game requests):
  // its worker thread uploads textures while the main thread renders,
  // and neither our ddraw state nor the d3d9 device is reentrant.
  bool graphics() const { return dll == "ddraw.dll"; }
};

std::mutex g_slots_mu;
std::deque<Slot> g_slots;  // stable refs; index = (thunk - kThunkBase) / kThunkStride

// Pseudo HMODULEs for DLLs we provide. Real modules are guest pointers to
// a PE header; nothing in the game dereferences these, so a recognisable
// value outside the arena's used ranges is enough.
const std::vector<std::string>& KnownDlls() {
  static const std::vector<std::string> dlls = {
      "kernel32.dll", "user32.dll",  "gdi32.dll",    "advapi32.dll",
      "ole32.dll",    "version.dll", "winmm.dll",    "wsock32.dll",
      "ddraw.dll",    "mss32.dll",   "binkw32.dll",  "dbghelp.dll",
      "imm32.dll",    "shfolder.dll", "ntdll.dll"};
  return dlls;
}

uint32_t AddSlot(const std::string& dll, const std::string& name) {
  std::lock_guard<std::mutex> l(g_slots_mu);
  for (size_t i = 0; i < g_slots.size(); i++)
    if (g_slots[i].dll == dll && g_slots[i].name == name)
      return kThunkBase + uint32_t(i) * kThunkStride;
  uint32_t thunk = kThunkBase + uint32_t(g_slots.size()) * kThunkStride;
  if (thunk >= kReturnToHost) FatalF("thunk region exhausted");
  auto it = Registry().find({dll, name});
  g_slots.push_back({dll, name, it == Registry().end() ? nullptr : &it->second});
  return thunk;
}

}  // namespace

Registrar::Registrar(const char* dll, const char* name, int nargs, Fn fn) {
  Registry()[{dll, name}] = Def{nargs, fn};
}

std::string NormaliseDll(std::string dll) {
  dll = win32::Lower(dll);
  size_t slash = dll.find_last_of("\\/");
  if (slash != std::string::npos) dll = dll.substr(slash + 1);
  if (dll.find('.') == std::string::npos) dll += ".dll";
  return dll;
}

uint32_t ImportThunk(const std::string& dll, const std::string& name) {
  std::string n = NormaliseDll(dll);
  if (!Registry().count({n, name}))
    RECOMP_TRACE("hle: import %s!%s has no handler\n", n.c_str(), name.c_str());
  return AddSlot(n, name);
}

void CallThunk(Ctx& c, uint32_t thunk) {
  size_t idx = (thunk - kThunkBase) / kThunkStride;
  const Slot* s = nullptr;
  {
    std::lock_guard<std::mutex> l(g_slots_mu);
    if ((thunk - kThunkBase) % kThunkStride == 0 && idx < g_slots.size())
      s = &g_slots[idx];
  }
  if (!s)
    FatalF("call to unknown thunk %08X (return address %08X)", thunk,
           Rd32(c.esp));
  if (!s->def)
    FatalF("unimplemented import %s!%s called (return address %08X)",
           s->dll.c_str(), s->name.c_str(), Rd32(c.esp));
  const Def& d = *s->def;
  // <ID>_TRACE_FILTER=mss32 (substring of "dll!name") narrows the trace.
  static const char* filter = Env("TRACE_FILTER");
  const bool trace = g_trace && (!filter || (s->dll + "!" + s->name).find(filter) !=
                                                std::string::npos);
  if (trace) {
    std::string args;
    for (int i = 0; i < std::max(d.nargs, 0) && i < 8; i++) {
      char buf[16];
      std::snprintf(buf, sizeof buf, i ? ", %X" : "%X", Arg(c, i));
      args += buf;
    }
    TraceF("[%04X] %s!%s(%s) from %08X\n", CurrentThread()->tid,
           s->dll.c_str(), s->name.c_str(), args.c_str(), Rd32(c.esp));
  }
  // For hang dumps: which import this thread is inside (innermost).
  const char* saved_name = t_wd_hle_name;
  uint32_t saved_ret = t_wd_hle_ret;
  t_wd_hle_name = s->name.c_str();
  t_wd_hle_ret = Rd32(c.esp);
  ++t_hle_depth;
  static std::recursive_mutex graphics_mu;
  std::unique_lock<std::recursive_mutex> gl;
  if (s->graphics()) gl = std::unique_lock<std::recursive_mutex>(graphics_mu);
  uint32_t r = d.fn(c);
  if (gl) gl.unlock();
  --t_hle_depth;
  t_wd_hle_name = saved_name;
  t_wd_hle_ret = saved_ret;
  if (trace) TraceF("[%04X]   -> %X\n", CurrentThread()->tid, r);
  c.eax = r;
  c.esp += 4 + (d.nargs > 0 ? 4 * uint32_t(d.nargs) : 0);
}

uint32_t ModuleHandle(const std::string& dll) {
  std::string n = NormaliseDll(dll);
  const auto& k = KnownDlls();
  auto it = std::find(k.begin(), k.end(), n);
  if (it == k.end()) return 0;
  return kModuleRegion + uint32_t(it - k.begin()) * 0x10000;
}

uint32_t ProcAddress(uint32_t hmodule, const std::string& name) {
  const auto& k = KnownDlls();
  uint32_t idx = (hmodule - kModuleRegion) / 0x10000;
  if (hmodule < kModuleRegion || idx >= k.size()) return 0;
  const std::string& dll = k[idx];
  if (!Registry().count({dll, name})) {
    WarnF("GetProcAddress(%s, %s): not implemented, returning NULL\n",
          dll.c_str(), name.c_str());
    return 0;
  }
  return AddSlot(dll, name);
}

uint32_t ThunkFor(const std::string& dll, const std::string& name) {
  return AddSlot(dll, name);
}

void SetLastError(uint32_t e) { Wr32(CurrentThread()->teb + 0x34, e); }

uint32_t CopyOut(uint32_t buf, uint32_t size, const std::string& s) {
  if (s.size() + 1 > size) return uint32_t(s.size() + 1);
  PutStr(buf, s);
  return uint32_t(s.size());
}

uint32_t PutCounted(uint32_t buf, uint32_t cap, const std::string& s, bool wide) {
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
