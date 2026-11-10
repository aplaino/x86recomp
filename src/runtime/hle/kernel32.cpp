// kernel32: process, memory, heaps, TLS, synchronisation basics, locale and
// time. Files, kernel objects, resources and conversions, and SEH live in
// kernel32_file.cpp, kernel32_sync.cpp, kernel32_misc.cpp and
// kernel32_seh.cpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/perf.h"
#include "runtime/platform.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp {

namespace {

std::mutex g_heaps_mu;
std::map<gaddr, std::unique_ptr<GuestHeap>> g_heaps;
GuestHeap* g_process_heap = nullptr;

const auto g_start = std::chrono::steady_clock::now();

GuestHeap* NewHeap(uint32_t initial, uint32_t max) {
  auto h = std::make_unique<GuestHeap>(g_proc->vm, initial, max);
  GuestHeap* raw = h.get();
  if (!raw->handle()) return nullptr;
  std::lock_guard<std::mutex> l(g_heaps_mu);
  g_heaps[raw->handle()] = std::move(h);
  return raw;
}

}  // namespace

uint64_t PerfCounter() {
  using namespace std::chrono;
  return uint64_t(duration_cast<nanoseconds>(steady_clock::now() - g_start).count() / 100);
}

gaddr HeapAllocProcess(uint64_t size, bool zero) {
  return g_process_heap->Alloc(size, zero);
}

void HeapFreeProcess(gaddr p) { g_process_heap->Free(p); }

gaddr HeapReAllocProcess(gaddr p, uint64_t size) {
  return g_process_heap->ReAlloc(p, size, false, false);
}

uint64_t HeapSizeProcess(gaddr p) { return g_process_heap->Size(p); }

uint32_t TickCount() {
  using namespace std::chrono;
  return uint32_t(duration_cast<milliseconds>(steady_clock::now() - g_start).count());
}

void WriteSystemTime(uint32_t st, const std::tm& t, int ms) {
  Wr16(st + 0, uint16_t(t.tm_year + 1900));
  Wr16(st + 2, uint16_t(t.tm_mon + 1));
  Wr16(st + 4, uint16_t(t.tm_wday));
  Wr16(st + 6, uint16_t(t.tm_mday));
  Wr16(st + 8, uint16_t(t.tm_hour));
  Wr16(st + 10, uint16_t(t.tm_min));
  Wr16(st + 12, uint16_t(t.tm_sec));
  Wr16(st + 14, uint16_t(ms));
}

GuestHeap* FindHeap(gaddr handle) {
  std::lock_guard<std::mutex> l(g_heaps_mu);
  auto it = g_heaps.find(handle);
  return it == g_heaps.end() ? nullptr : it->second.get();
}

void SetEnvironmentBlock(const std::vector<std::string>& vars);

void InitKernel32State() {
  Process& p = *g_proc;
  g_process_heap = NewHeap(1u << 20, 0);
  if (!g_process_heap) FatalF("cannot create the process heap");
  p.process_heap = g_process_heap->handle();
  if (p.x64)
    Wr64(kPebVa64 + 0x30, p.process_heap);  // PEB ProcessHeap
  else
    Wr32(kPebVa + 0x18, uint32_t(p.process_heap));

  std::string exe = GuestExePath();
  std::string cmd = "\"" + exe + "\"";
  if (!p.game_args.empty()) cmd += " " + p.game_args;
  p.cmdline_a = HeapAllocProcess(uint32_t(cmd.size() + 1), false);
  PutStr(p.cmdline_a, cmd);
  std::u16string wcmd = win32::Widen(cmd);
  p.cmdline_w = HeapAllocProcess(uint32_t(wcmd.size() * 2 + 2), true);
  std::memcpy(GPtr(p.cmdline_w), wcmd.data(), wcmd.size() * 2);

  // A small, fixed environment; the game only needs it to exist.
  SetEnvironmentBlock({"PATH=C:\\WINDOWS\\system32;C:\\WINDOWS",
                       "SystemRoot=C:\\WINDOWS", "windir=C:\\WINDOWS",
                       "TEMP=C:\\TEMP", "TMP=C:\\TEMP"});
}

std::vector<std::string> EnvironmentVars() {
  std::vector<std::string> vars;
  const char* env = g_proc->mem.ptr<const char>(g_proc->environ_a);
  for (const char* e = env; *e; e += std::strlen(e) + 1) vars.push_back(e);
  return vars;
}

void SetEnvironmentBlock(const std::vector<std::string>& vars) {
  Process& p = *g_proc;
  std::string block;
  for (const std::string& e : vars) block.append(e).push_back('\0');
  block.push_back('\0');
  // The old blocks stay allocated: the game may still hold pointers into
  // them (GetEnvironmentStrings).
  p.environ_a = HeapAllocProcess(uint32_t(block.size()), false);
  std::memcpy(GPtr(p.environ_a), block.data(), block.size());
  std::u16string wblock = win32::Widen(block);
  p.environ_w = HeapAllocProcess(uint32_t(wblock.size() * 2), false);
  std::memcpy(GPtr(p.environ_w), wblock.data(), wblock.size() * 2);
}

namespace hle {
namespace {

using namespace win32;

// ---- process / module ----------------------------------------------------

HLE("kernel32.dll", GetVersion, 0) {
  return 0x0A280105;  // Windows XP: build 2600, 5.1, NT platform
}

HLE("kernel32.dll", GetVersionExA, 1) {
  uint32_t vi = Arg(c, 0);
  uint32_t size = Rd32(vi);
  Wr32(vi + 4, 5);
  Wr32(vi + 8, 1);
  Wr32(vi + 12, 2600);
  Wr32(vi + 16, 2);  // VER_PLATFORM_WIN32_NT
  std::memset(GPtr(vi + 20), 0, 128);
  PutStr(vi + 20, "Service Pack 3");
  if (size >= 156) {  // OSVERSIONINFOEXA
    Wr16(vi + 148, 3);
    Wr16(vi + 150, 0);
    Wr16(vi + 152, 0x100);  // VER_SUITE_SINGLEUSERTS
    Wr8(vi + 154, 1);       // VER_NT_WORKSTATION
  }
  return 1;
}

// Lifted DLLs (GameConfig::lifted_dlls) are mapped at startup; LoadLibrary
// counts references and runs DllMain under the loader lock, as Windows
// does. After the last FreeLibrary the next load maps the file afresh, so
// the DLL sees initial data again as after a real unload.
std::recursive_mutex g_loader_mu;

Module* LiftedDll(const std::string& name) {
  std::string n = NormaliseDll(name);
  for (Module& m : g_proc->modules)
    if (m.name == n) return &m;
  return nullptr;
}

Module* LoadedDllAt(gaddr hmodule) {
  for (Module& m : g_proc->modules)
    if (m.refs && m.img.image_base == hmodule) return &m;
  return nullptr;
}

constexpr uint32_t kDllProcessDetach = 0, kDllProcessAttach = 1;

// DllMain(hinstDLL, reason, NULL); false if it failed.
bool CallDllMain(Ctx& c, Module& m, uint32_t reason) {
  if (!m.img.entry) return true;
  m.stale = true;
  RECOMP_TRACE("  %s: DllMain(%u)\n", m.name.c_str(), reason);
  return uint32_t(CallGuest(c, m.img.entry, {m.img.image_base, reason, 0})) != 0;
}

// The lifted DLLs m imports from statically, each once.
std::vector<Module*> LiftedDeps(Module& m) {
  std::vector<Module*> deps;
  for (const PeImport& imp : m.img.imports) {
    Module* d = LiftedDll(imp.dll);
    if (d && d != &m && std::find(deps.begin(), deps.end(), d) == deps.end()) deps.push_back(d);
  }
  return deps;
}

void ReleaseLifted(Ctx& c, Module& m);

// Takes a reference on each lifted DLL m imports from, loading (and
// initialising) those not loaded yet, then runs m's DllMain(PROCESS_ATTACH):
// as the Windows loader orders it, a module's static dependencies are
// initialised before it, and they stay loaded while it is. Caller holds
// the loader lock and has counted m's reference.
bool AttachLifted(Ctx& c, Module& m) {
  std::vector<Module*> taken;
  auto undo = [&] {
    for (Module* d : taken)
      if (--d->refs == 0) ReleaseLifted(c, *d);
  };
  for (Module* d : LiftedDeps(m)) {
    if (d->refs++) {
      taken.push_back(d);
      continue;
    }
    if (d->stale) MapModule(*d);
    if (!AttachLifted(c, *d)) {
      WarnF("%s: DllMain of its dependency %s failed\n", m.name.c_str(), d->name.c_str());
      d->refs = 0;
      undo();
      return false;
    }
    taken.push_back(d);
  }
  if (!CallDllMain(c, m, kDllProcessAttach)) {
    undo();
    return false;
  }
  return true;
}

// The last reference to m is gone: DllMain(PROCESS_DETACH), then its
// references on its dependencies, which unload in turn when that was their
// last. m maps afresh on its next load (CallDllMain marked it stale).
void ReleaseLifted(Ctx& c, Module& m) {
  CallDllMain(c, m, kDllProcessDetach);
  for (Module* d : LiftedDeps(m))
    if (d->refs && --d->refs == 0) ReleaseLifted(c, *d);
}

// A guest string argument, ANSI or UTF-16.
std::string GArg(gaddr p, bool wide) { return wide ? Narrow(GWStr(p)) : GStr(p); }

gaddr ModuleHandleOf(gaddr name, bool wide) {
  if (!name) return g_proc->img.image_base;
  std::string n = GArg(name, wide);
  std::string exe = Lower(Game().exe_name);
  if (NormaliseDll(n) == exe || Lower(n) == exe.substr(0, exe.rfind('.')))
    return g_proc->img.image_base;
  if (Module* m = LiftedDll(n)) {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    if (m->refs) return m->img.image_base;
    SetLastError(kErrorModNotFound);
    return 0;
  }
  gaddr h = ModuleHandle(n);
  if (!h) SetLastError(kErrorModNotFound);
  return h;
}

HLE64("kernel32.dll", GetModuleHandleA, 1) { return ModuleHandleOf(Arg(c, 0), false); }
HLE64("kernel32.dll", GetModuleHandleW, 1) { return ModuleHandleOf(Arg(c, 0), true); }

// GetModuleFileNameW: as GetModuleFileNameA, in WCHARs.
HLE64("kernel32.dll", GetModuleFileNameW, 3) {
  gaddr mod = Arg(c, 0), buf = Arg(c, 1);
  uint32_t size = uint32_t(Arg(c, 2));
  std::string path;
  if (mod && mod != g_proc->img.image_base) {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    Module* m = LoadedDllAt(mod);
    if (!m) {
      SetLastError(kErrorModNotFound);
      return 0;
    }
    path = GuestGameDir() + "\\" + Game().lifted_dlls[size_t(m - g_proc->modules.data())].name;
  } else {
    path = GuestExePath();
  }
  if (!size) return 0;
  const std::u16string w = Widen(path.substr(0, size - 1));
  for (size_t i = 0; i < w.size(); i++) Wr16(buf + 2 * gaddr(i), w[i]);
  Wr16(buf + 2 * gaddr(w.size()), 0);
  return uint32_t(w.size());
}

HLE64("kernel32.dll", GetModuleFileNameA, 3) {
  gaddr mod = Arg(c, 0), buf = Arg(c, 1);
  uint32_t size = uint32_t(Arg(c, 2));
  std::string path = GuestExePath();
  if (mod && mod != g_proc->img.image_base) {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    Module* m = LoadedDllAt(mod);
    if (!m) {
      SetLastError(kErrorModNotFound);
      return 0;
    }
    path = GuestGameDir() + "\\" +
           Game().lifted_dlls[size_t(m - g_proc->modules.data())].name;
  }
  if (!size) return 0;
  std::string s = path.substr(0, size - 1);
  PutStr(buf, s);
  return uint32_t(s.size());
}

// LoadLibrary(Ex)A/W: lifted DLLs, and the HLE modules.
gaddr LoadModule(Ctx& c, const char* api, const std::string& n) {
  if (Module* m = LiftedDll(n)) {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    if (m->refs++) return m->img.image_base;
    if (m->stale) MapModule(*m);
    if (!AttachLifted(c, *m)) {
      WarnF("%s(%s): DllMain failed\n", api, n.c_str());
      m->refs = 0;
      SetLastError(kErrorDllInitFailed);
      return 0;
    }
    return m->img.image_base;
  }
  gaddr h = ModuleHandle(n);
  if (!h) {
    WarnF("%s(%s): not provided\n", api, n.c_str());
    SetLastError(kErrorModNotFound);
  }
  return h;
}

HLE64("kernel32.dll", LoadLibraryA, 1) { return LoadModule(c, "LoadLibraryA", GStr(Arg(c, 0))); }
HLE64("kernel32.dll", LoadLibraryW, 1) {
  return LoadModule(c, "LoadLibraryW", Narrow(GWStr(Arg(c, 0))));
}
HLE64("kernel32.dll", LoadLibraryExA, 3) {
  return LoadModule(c, "LoadLibraryExA", GStr(Arg(c, 0)));
}
HLE64("kernel32.dll", LoadLibraryExW, 3) {
  return LoadModule(c, "LoadLibraryExW", Narrow(GWStr(Arg(c, 0))));
}

HLE64("kernel32.dll", FreeLibrary, 1) {
  std::lock_guard<std::recursive_mutex> l(g_loader_mu);
  if (Module* m = LoadedDllAt(Arg(c, 0)); m && --m->refs == 0) ReleaseLifted(c, *m);
  return 1;
}

// DLL_THREAD_ATTACH/DETACH are never sent, so there is nothing to disable.
HLE64("kernel32.dll", DisableThreadLibraryCalls, 1) { return 1; }

HLE64("kernel32.dll", GetProcAddress, 2) {
  gaddr mod = Arg(c, 0), name = Arg(c, 1);
  {
    std::lock_guard<std::recursive_mutex> l(g_loader_mu);
    if (Module* m = LoadedDllAt(mod)) {
      std::string n = name < 0x10000 ? std::string() : GStr(name);
      for (const PeExport& e : m->img.exports)
        if (name < 0x10000 ? e.ordinal == name : e.name == n) return e.va;
      SetLastError(kErrorProcNotFound);
      return 0;
    }
  }
  if (name < 0x10000) {
    WarnF("GetProcAddress by ordinal %u: not supported\n", uint32_t(name));
    return 0;
  }
  std::string n = GStr(name);
  gaddr a = ProcAddress(mod, n);
  if (!a) SetLastError(kErrorProcNotFound);
  return a;
}

HLE64("kernel32.dll", GetCommandLineA, 0) { return g_proc->cmdline_a; }
HLE64("kernel32.dll", GetCommandLineW, 0) { return g_proc->cmdline_w; }

// STARTUPINFOA/W: 68 bytes in 32-bit guests, 104 in x64 ones (its handles
// and string pointers are pointer-sized); nothing set but cb.
uint64_t StartupInfo(gaddr si) {
  const uint32_t size = g_proc->x64 ? 104 : 68;
  std::memset(GPtr(si), 0, size);
  Wr32(si, size);
  return 0;
}
HLE64("kernel32.dll", GetStartupInfoA, 1) { return StartupInfo(Arg(c, 0)); }
HLE64("kernel32.dll", GetStartupInfoW, 1) { return StartupInfo(Arg(c, 0)); }

HLE("kernel32.dll", GetEnvironmentStrings, 0) { return g_proc->environ_a; }
HLE("kernel32.dll", GetEnvironmentStringsW, 0) { return g_proc->environ_w; }
HLE("kernel32.dll", FreeEnvironmentStringsA, 1) { return 1; }
HLE("kernel32.dll", FreeEnvironmentStringsW, 1) { return 1; }

HLE("kernel32.dll", GetEnvironmentVariableA, 3) {
  std::string name = GStr(Arg(c, 0));
  uint32_t buf = Arg(c, 1), size = Arg(c, 2);
  const char* env = g_proc->mem.ptr<const char>(g_proc->environ_a);
  std::string key = Lower(name) + "=";
  for (const char* e = env; *e; e += std::strlen(e) + 1) {
    std::string kv = e;
    if (Lower(kv.substr(0, key.size())) == key)
      return CopyOut(buf, size, kv.substr(key.size()));
  }
  SetLastError(kErrorEnvVarNotFound);
  return 0;
}

// value null: delete the variable.
uint32_t SetEnvVar(const std::string& name, const std::string* value) {
  if (name.empty() || name.find('=') != std::string::npos) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  std::vector<std::string> vars = EnvironmentVars();
  std::string key = Lower(name) + "=";
  std::erase_if(vars, [&](const std::string& kv) {
    return Lower(kv.substr(0, key.size())) == key;
  });
  if (value) vars.push_back(name + "=" + *value);
  SetEnvironmentBlock(vars);
  return 1;
}

HLE("kernel32.dll", SetEnvironmentVariableA, 2) {
  std::string v = Arg(c, 1) ? GStr(Arg(c, 1)) : "";
  return SetEnvVar(GStr(Arg(c, 0)), Arg(c, 1) ? &v : nullptr);
}

HLE("kernel32.dll", SetEnvironmentVariableW, 2) {
  std::string v = Arg(c, 1) ? Narrow(GWStr(Arg(c, 1))) : "";
  return SetEnvVar(Narrow(GWStr(Arg(c, 0))), Arg(c, 1) ? &v : nullptr);
}

HLE("kernel32.dll", ExpandEnvironmentStringsA, 3) {
  // No variable the game uses needs expanding; copy through.
  std::string s = GStr(Arg(c, 0));
  uint32_t buf = Arg(c, 1), size = Arg(c, 2);
  if (s.size() + 1 <= size) PutStr(buf, s);
  return uint32_t(s.size() + 1);
}

HLE("kernel32.dll", SetErrorMode, 1) {
  static std::atomic<uint32_t> mode{0};
  return mode.exchange(Arg(c, 0));
}

// Power requests (keep the display on) have no effect here; the previous
// state is reported as on Windows.
HLE("kernel32.dll", SetThreadExecutionState, 1) {
  static std::atomic<uint32_t> state{0x80000000};  // ES_CONTINUOUS
  uint32_t v = Arg(c, 0);
  return (v & 0x80000000) ? state.exchange(v) : state.load();
}

// Console control events (Ctrl+C, close) are never delivered: the host
// terminal's signals end the process directly. Registration succeeds.
HLE("kernel32.dll", SetConsoleCtrlHandler, 2) { return 1; }

HLE("kernel32.dll", GetSystemDirectoryA, 2) {
  return CopyOut(Arg(c, 0), Arg(c, 1), "C:\\WINDOWS\\system32");
}
HLE("kernel32.dll", GetWindowsDirectoryA, 2) {
  return CopyOut(Arg(c, 0), Arg(c, 1), "C:\\WINDOWS");
}

// There is no console: the mode queries fail (as for a GUI process whose
// standard handles are files or pipes), writes go to stdout.
HLE("kernel32.dll", GetConsoleCP, 0) { return 437; }
HLE("kernel32.dll", GetConsoleOutputCP, 0) { return 437; }
HLE("kernel32.dll", GetConsoleMode, 2) {
  SetLastError(kErrorInvalidHandle);
  return 0;
}
HLE("kernel32.dll", WriteConsoleA, 5) {
  const uint32_t n = Arg(c, 2);
  std::fwrite(GPtr(Arg(c, 1)), 1, n, stdout);
  if (Arg(c, 3)) Wr32(Arg(c, 3), n);
  return 1;
}
HLE("kernel32.dll", WriteConsoleW, 5) {
  const uint32_t n = Arg(c, 2);
  std::u16string w(n, u'\0');
  for (uint32_t i = 0; i < n; i++) w[i] = Rd16(Arg(c, 1) + 2 * i);
  std::fputs(AnsiToUtf8(Narrow(w)).c_str(), stdout);
  if (Arg(c, 3)) Wr32(Arg(c, 3), n);
  return 1;
}

HLE("kernel32.dll", GetProcessId, 1) {
  const uint32_t h = Arg(c, 0);
  if (h == 0xFFFFFFFF) return kGuestPid;
  SetLastError(kErrorInvalidHandle);
  return 0;
}

// GetSystemTimes(idle, kernel, user): FILETIME totals over all processors.
// Idle grows with wall time, so CPU load computed from them reads as low.
HLE("kernel32.dll", GetSystemTimes, 3) {
  const uint64_t t = PerfCounter() * GuestProcessorCount();  // 100 ns units
  if (Arg(c, 0)) Wr64(Arg(c, 0), t);
  if (Arg(c, 1)) Wr64(Arg(c, 1), t);
  if (Arg(c, 2)) Wr64(Arg(c, 2), 0);
  return 1;
}

HLE("kernel32.dll", HeapSetInformation, 4) { return 1; }

// IsWow64Process(process, &wow64): a 32-bit Windows, as the guest sees it.
HLE("kernel32.dll", IsWow64Process, 2) {
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  return 1;
}

// CallNtPowerInformation(level, in, in_size, out, out_size): only
// ProcessorInformation (11), PROCESSOR_POWER_INFORMATION { Number, MaxMhz,
// CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState } per processor.
HLE("powrprof.dll", CallNtPowerInformation, 5) {
  const uint32_t level = Arg(c, 0), out = Arg(c, 3), size = Arg(c, 4);
  if (level != 11) {
    WarnF("CallNtPowerInformation(%u): not supported\n", level);
    return 0xC0000002;  // STATUS_NOT_IMPLEMENTED
  }
  const uint32_t n = GuestProcessorCount();
  if (size < 24 * n) return 0xC0000023;  // STATUS_BUFFER_TOO_SMALL
  for (uint32_t i = 0; i < n; i++) {
    const gaddr p = out + 24 * i;
    const uint32_t v[6] = {i, 3000, 3000, 3000, 0, 0};
    for (int k = 0; k < 6; k++) Wr32(p + 4 * k, v[k]);
  }
  return 0;
}

// Pointer encoding (the CRT's function-pointer hardening): the identity.
HLE("kernel32.dll", EncodePointer, 1) { return Arg(c, 0); }
HLE("kernel32.dll", DecodePointer, 1) { return Arg(c, 0); }

// NtQueryPerformanceCounter(counter, frequency): QueryPerformanceCounter's
// clock; STATUS_SUCCESS.
HLE("ntdll.dll", NtQueryPerformanceCounter, 2) {
  Wr64(Arg(c, 0), PerfCounter());
  if (Arg(c, 1)) Wr64(Arg(c, 1), 10000000);
  return 0;
}

HLE64("kernel32.dll", GetCurrentProcess, 0) { return HandleRet(0xFFFFFFFF); }
HLE64("kernel32.dll", GetCurrentProcessId, 0) { return kGuestPid; }
HLE64("kernel32.dll", GetCurrentThread, 0) { return HandleRet(0xFFFFFFFE); }
HLE64("kernel32.dll", GetCurrentThreadId, 0) { return CurrentThread()->tid; }

HLE64("kernel32.dll", ExitProcess, 1) {
  uint32_t code = uint32_t(Arg(c, 0));
  if (code) {
    std::fprintf(stderr, "ExitProcess(%u) esp=%08X ebp=%08X\n", code, c.esp, c.ebp);
    uint32_t ebp = c.ebp;
    for (int i = 0; i < 24; i++) {
      if (!g_proc->mem.IsCommitted(ebp) || !g_proc->mem.IsCommitted(ebp + 4)) break;
      std::fprintf(stderr, "  #%-2d ret %08X (frame %08X)\n", i, Rd32(ebp + 4), ebp);
      uint32_t next = Rd32(ebp);
      if (next <= ebp) break;
      ebp = next;
    }
  }
  if (Game().before_exit) Game().before_exit();
  std::fflush(stdout);
  std::fprintf(stderr, "ExitProcess(%u)\n", code);
  std::fflush(stderr);
  std::_Exit(int(code));
}

HLE64("kernel32.dll", TerminateProcess, 2) {
  if (uint32_t(Arg(c, 0)) == 0xFFFFFFFF) {
    std::fprintf(stderr, "TerminateProcess(self, %u)\n", uint32_t(Arg(c, 1)));
    std::_Exit(int(Arg(c, 1)));
  }
  return 0;
}

HLE64("kernel32.dll", GetLastError, 0) {
  return Rd32(CurrentThread()->teb + 0x34);
}
HLE64("kernel32.dll", SetLastError, 1) {
  SetLastError(Arg(c, 0));
  return 0;
}

HLE64("kernel32.dll", SetUnhandledExceptionFilter, 1) {
  static gaddr filter = 0;
  gaddr old = filter;
  filter = Arg(c, 0);
  return old;
}

HLE64("kernel32.dll", IsDebuggerPresent, 0) { return 0; }

HLE64("kernel32.dll", OutputDebugStringA, 1) {
  std::fprintf(stderr, "[guest] %s", GStr(Arg(c, 0)).c_str());
  return 0;
}

HLE("kernel32.dll", SetHandleCount, 1) { return Arg(c, 0); }

// Looked up dynamically by the CRT (Pentium FDIV check). Matches the CPU
// that Cpuid() reports: MMX yes, no SSE/3DNow!, no FDIV erratum.
HLE64("kernel32.dll", IsProcessorFeaturePresent, 1) {
  switch (Arg(c, 0)) {
    case 3: return 1;   // PF_MMX_INSTRUCTIONS_AVAILABLE
    case 6: return 1;   // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
    case 8: return 1;   // PF_RDTSC_INSTRUCTION_AVAILABLE
    case 10: return 1;  // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
    default: return 0;
  }
}

// x64 guests: SYSTEM_INFO's pointer-sized fields, an AMD64 processor (the
// CPUID model's family 16h) and the address range guest allocations use.
HLE64("kernel32.dll", GetSystemInfo, 1) {
  gaddr si = Arg(c, 0);
  uint32_t ncpu = GuestProcessorCount();
  if (g_proc->x64) {
    std::memset(GPtr(si), 0, 48);
    Wr16(si + 0, 9);              // PROCESSOR_ARCHITECTURE_AMD64
    Wr32(si + 4, 0x1000);         // page size
    Wr64(si + 8, kUserLo64);      // lowest application address
    Wr64(si + 16, kTebLo64 - 1);  // highest
    Wr64(si + 24, (1ull << ncpu) - 1);
    Wr32(si + 32, ncpu);
    Wr32(si + 36, 8664);          // PROCESSOR_AMD_X8664
    Wr32(si + 40, 0x10000);       // allocation granularity
    Wr16(si + 44, 0x16);          // level: the family
    Wr16(si + 46, 0x0001);        // revision: model 0, stepping 1
    return 0;
  }
  std::memset(GPtr(si), 0, 36);
  Wr16(si + 0, 0);            // PROCESSOR_ARCHITECTURE_INTEL
  Wr32(si + 4, 0x1000);       // page size
  Wr32(si + 8, 0x00010000);   // lowest application address
  Wr32(si + 12, 0x7FFEFFFF);  // highest
  Wr32(si + 16, (1u << ncpu) - 1);  // active processor mask
  Wr32(si + 20, ncpu);              // number of processors
  Wr32(si + 24, 586);         // PROCESSOR_INTEL_PENTIUM
  Wr32(si + 28, 0x10000);     // allocation granularity
  Wr16(si + 32, 6);           // level
  Wr16(si + 34, 0x0803);      // revision
  return 0;
}

HLE("kernel32.dll", GlobalMemoryStatus, 1) {
  // A roomy 2002-era machine: 1 GiB RAM, 1.5 GiB page file. Every value
  // stays below 2 GiB: the game compares these as signed ints (a 2 GiB
  // page file reads as negative and triggers its "swap file" error).
  uint32_t ms = Arg(c, 0);
  Wr32(ms + 0, 32);
  Wr32(ms + 4, 30);           // memory load %
  // SIZE_T fields: capped at 2 GiB - 1, as Windows does for a process that
  // is not large address aware.
  const uint64_t phys = uint64_t(Game().total_phys_mb) << 20;
  auto cap = [](uint64_t v) { return uint32_t(std::min<uint64_t>(v, 0x7FFFFFFF)); };
  Wr32(ms + 8, cap(phys));                // total phys
  Wr32(ms + 12, cap(phys / 4 * 3));       // avail phys
  Wr32(ms + 16, cap(phys + 0x20000000));  // total page file
  Wr32(ms + 20, cap(phys));               // avail page file
  Wr32(ms + 24, 0x7FFE0000);  // total virtual
  Wr32(ms + 28, 0x70000000);  // avail virtual
  return 0;
}

// The same machine as GlobalMemoryStatus, through MEMORYSTATUSEX. x64
// guests see a current one instead (16 GiB RAM, 24 GiB commit limit) and
// the virtual address range guest allocations use.
HLE64("kernel32.dll", GlobalMemoryStatusEx, 1) {
  gaddr ms = Arg(c, 0);
  if (Rd32(ms) != 64) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  if (g_proc->x64) {
    Wr32(ms + 4, 25);
    Wr64(ms + 8, 16ull << 30);
    Wr64(ms + 16, 12ull << 30);
    Wr64(ms + 24, 24ull << 30);
    Wr64(ms + 32, 20ull << 30);
    Wr64(ms + 40, kTebLo64 - kUserLo64);
    Wr64(ms + 48, kTebLo64 - kUserLo64);  // not tracked: all of it
    Wr64(ms + 56, 0);
    return 1;
  }
  const uint64_t phys = uint64_t(Game().total_phys_mb) << 20;
  Wr32(ms + 4, 30);
  Wr64(ms + 8, phys);                // total phys
  Wr64(ms + 16, phys / 4 * 3);       // avail phys
  Wr64(ms + 24, phys + 0x20000000);  // total page file (commit limit)
  Wr64(ms + 32, phys);               // avail page file
  Wr64(ms + 40, 0x7FFE0000);
  Wr64(ms + 48, 0x70000000);
  Wr64(ms + 56, 0);  // ullAvailExtendedVirtual: reserved
  return 1;
}

// a * b / c in 64 bits, rounded to nearest with halves away from zero;
// -1 if c is 0 or the result does not fit in an int.
HLE("kernel32.dll", MulDiv, 3) {
  int64_t p = int64_t(int32_t(Arg(c, 0))) * int32_t(Arg(c, 1));
  int64_t d = int32_t(Arg(c, 2));
  if (d == 0) return 0xFFFFFFFFu;
  if (d < 0) {
    p = -p;
    d = -d;
  }
  int64_t q = (p >= 0 ? p + d / 2 : p - d / 2) / d;
  if (q > INT32_MAX || q < -INT32_MAX) return 0xFFFFFFFFu;
  return uint32_t(int32_t(q));
}

HLE("kernel32.dll", GetComputerNameA, 2) {
  uint32_t buf = Arg(c, 0), psize = Arg(c, 1);
  std::string n = win32::Upper(Game().id) + "HOST";
  if (Rd32(psize) < n.size() + 1) {
    Wr32(psize, uint32_t(n.size() + 1));
    SetLastError(kErrorBufferOverflow);
    return 0;
  }
  PutStr(buf, n);
  Wr32(psize, uint32_t(n.size()));
  return 1;
}

// ---- heaps ---------------------------------------------------------------

HLE64("kernel32.dll", GetProcessHeap, 0) { return g_proc->process_heap; }

HLE("kernel32.dll", HeapCreate, 3) {
  GuestHeap* h = NewHeap(Arg(c, 1), Arg(c, 2));
  return h ? h->handle() : 0;
}

HLE("kernel32.dll", HeapDestroy, 1) {
  std::lock_guard<std::mutex> l(g_heaps_mu);
  return g_heaps.erase(Arg(c, 0)) ? 1 : 0;
}

HLE64("kernel32.dll", HeapAlloc, 3) {
  GuestHeap* h = FindHeap(Arg(c, 0));
  if (!h) FatalF("HeapAlloc on unknown heap %08llX", (unsigned long long)Arg(c, 0));
  return h->Alloc(Arg(c, 2), Arg(c, 1) & kHeapZeroMemory);
}

HLE64("kernel32.dll", HeapFree, 3) {
  gaddr p = Arg(c, 2);
  if (!p) return 1;
  GuestHeap* h = FindHeap(Arg(c, 0));
  if (!h || !h->Free(p)) {
    WarnF("HeapFree(%08llX, %08llX): not a live block\n", (unsigned long long)Arg(c, 0),
          (unsigned long long)p);
    return 0;
  }
  return 1;
}

HLE("kernel32.dll", HeapReAlloc, 4) {
  GuestHeap* h = FindHeap(Arg(c, 0));
  uint32_t flags = Arg(c, 1);
  if (!h) return 0;
  return h->ReAlloc(Arg(c, 2), Arg(c, 3), flags & kHeapZeroMemory,
                    flags & kHeapReallocInPlaceOnly);
}

HLE("kernel32.dll", HeapSize, 3) {
  GuestHeap* h = FindHeap(Arg(c, 0));
  return h ? h->Size(Arg(c, 2)) : 0xFFFFFFFF;
}

HLE("kernel32.dll", GlobalAlloc, 2) {
  // GMEM_MOVEABLE handles are returned as plain pointers; GlobalLock is
  // then the identity. Only used for clipboard data here.
  return HeapAllocProcess(Arg(c, 1), Arg(c, 0) & 0x40);
}
HLE("kernel32.dll", GlobalLock, 1) { return Arg(c, 0); }
HLE("kernel32.dll", GlobalSize, 1) {
  uint32_t s = g_process_heap->Size(Arg(c, 0));
  return s == 0xFFFFFFFF ? 0 : s;
}
HLE("kernel32.dll", GlobalUnlock, 1) { return 1; }
HLE("kernel32.dll", GlobalFree, 1) {
  g_process_heap->Free(Arg(c, 0));
  return 0;
}

HLE("kernel32.dll", LocalAlloc, 2) {
  // LMEM_MOVEABLE handles are plain pointers, as GlobalAlloc's.
  return HeapAllocProcess(Arg(c, 1), Arg(c, 0) & 0x40);
}

HLE64("kernel32.dll", LocalFree, 1) {
  if (Arg(c, 0)) g_process_heap->Free(Arg(c, 0));
  return 0;
}

// ---- virtual memory ------------------------------------------------------

Prot ProtFromWin(uint32_t p) {
  switch (p & 0xFF) {
    case kPageNoAccess: return Prot::None;
    case kPageReadOnly: return Prot::R;
    case kPageReadWrite:
    case kPageWriteCopy: return Prot::RW;
    case kPageExecute: return Prot::X;
    case kPageExecuteRead: return Prot::RX;
    default: return Prot::RWX;
  }
}

uint32_t ProtToWin(Prot p) {
  switch (p) {
    case Prot::None: return kPageNoAccess;
    case Prot::R: return kPageReadOnly;
    case Prot::RW: return kPageReadWrite;
    case Prot::X: return kPageExecute;
    case Prot::RX: return kPageExecuteRead;
    default: return kPageExecuteReadWrite;
  }
}

HLE64("kernel32.dll", VirtualAlloc, 4) {
  gaddr addr = Arg(c, 0);
  uint64_t size = Arg(c, 1);
  uint32_t type = uint32_t(Arg(c, 2)), prot = uint32_t(Arg(c, 3));
  GuestAllocator& vm = g_proc->vm;
  // MEM_COMMIT with no address reserves and commits, like MEM_RESERVE|COMMIT.
  if (!addr && (type & kMemCommit)) type |= kMemReserve;
  if (type & kMemReserve) {
    gaddr base = vm.Reserve(addr, size + (addr & 0xFFFF), type & kMemTopDown);
    if (!base) {
      SetLastError(kErrorNotEnoughMemory);
      return 0;
    }
    if (type & kMemCommit) vm.Commit(base, size, ProtFromWin(prot));
    return addr ? addr & ~gaddr(0xFFF) : base;
  }
  if (type & kMemCommit) {
    if (!vm.Commit(addr, size, ProtFromWin(prot))) {
      SetLastError(kErrorInvalidAddress);
      return 0;
    }
    return addr & ~gaddr(0xFFF);
  }
  SetLastError(kErrorInvalidParameter);
  return 0;
}

HLE64("kernel32.dll", VirtualFree, 3) {
  gaddr addr = Arg(c, 0);
  uint64_t size = Arg(c, 1);
  uint32_t type = uint32_t(Arg(c, 2));
  if (type & kMemRelease) return g_proc->vm.Release(addr) ? 1 : 0;
  if (type & kMemDecommit) return g_proc->vm.Decommit(addr, size) ? 1 : 0;
  return 0;
}

HLE("kernel32.dll", VirtualProtect, 4) {
  uint32_t addr = Arg(c, 0), size = Arg(c, 1), prot = Arg(c, 2),
           old = Arg(c, 3);
  if (old) Wr32(old, ProtToWin(g_proc->mem.QueryProt(addr)));
  return g_proc->mem.Protect(addr, size, ProtFromWin(prot)) ? 1 : 0;
}

HLE("kernel32.dll", VirtualLock, 2) { return 1; }

// MEMORY_BASIC_INFORMATION: {base, allocation base, allocation protect,
// region size, state, protect, type}, 28 bytes with 32-bit pointers and
// sizes, 48 on x64. Returns its size.
uint32_t QueryMemory(gaddr addr, gaddr mbi) {
  GuestMemory& mem = g_proc->mem;
  GuestAllocator::Region r;
  const bool x64 = g_proc->x64;
  const uint32_t size = x64 ? 48 : 28;
  auto put = [&](gaddr base, gaddr alloc_base, uint32_t alloc_prot, uint64_t region,
                 uint32_t state, uint32_t prot, uint32_t type) {
    std::memset(GPtr(mbi), 0, size);
    if (x64) {
      Wr64(mbi + 0, base);
      Wr64(mbi + 8, alloc_base);
      Wr32(mbi + 16, alloc_prot);
      Wr64(mbi + 24, region);
      Wr32(mbi + 32, state);
      Wr32(mbi + 36, prot);
      Wr32(mbi + 40, type);
    } else {
      Wr32(mbi + 0, uint32_t(base));
      Wr32(mbi + 4, uint32_t(alloc_base));
      Wr32(mbi + 8, alloc_prot);
      Wr32(mbi + 12, uint32_t(region));
      Wr32(mbi + 16, state);
      Wr32(mbi + 20, prot);
      Wr32(mbi + 24, type);
    }
  };
  gaddr page = addr & ~gaddr(0xFFF);
  if (!g_proc->vm.Find(addr, &r)) {
    uint64_t base, gap;
    g_proc->vm.FreeGap(addr, &base, &gap);
    put(base, 0, 0, gap, kMemFree, kPageNoAccess, 0);
    return size;
  }
  // Extend over pages with the same state.
  bool committed = mem.IsCommitted(page);
  Prot prot = mem.QueryProt(page);
  gaddr end = page;
  while (end < r.base + r.size && mem.IsCommitted(end) == committed &&
         (!committed || mem.QueryProt(end) == prot))
    end += 0x1000;
  const PeImage* img = ImageAt(r.base);
  put(page, r.base, kPageReadWrite, end - page, committed ? kMemCommit : kMemReserve,
      committed ? ProtToWin(prot) : 0,
      img && img->image_base == r.base ? kMemImage : kMemPrivate);
  return size;
}

// VirtualQuery(addr, mbi, length): 0 (ERROR_BAD_LENGTH) if the buffer is
// smaller than the structure.
HLE64("kernel32.dll", VirtualQuery, 3) {
  if (Arg(c, 2) < (g_proc->x64 ? 48u : 28u)) {
    SetLastError(24);  // ERROR_BAD_LENGTH
    return 0;
  }
  uint32_t r = QueryMemory(Arg(c, 0), Arg(c, 1));
  RECOMP_TRACE("  VirtualQuery(%llX) -> %u\n", (unsigned long long)Arg(c, 0), r);
  return r;
}

HLE("kernel32.dll", VirtualQueryEx, 4) {
  return QueryMemory(Arg(c, 1), Arg(c, 2));
}

bool Readable(uint32_t p, uint32_t n) {
  if (!n) return true;
  for (uint32_t a = p & ~0xFFFu; a < p + n && a >= (p & ~0xFFFu); a += 0x1000)
    if (!g_proc->mem.IsCommitted(a)) return false;
  return true;
}

HLE("kernel32.dll", IsBadReadPtr, 2) {
  return Readable(Arg(c, 0), Arg(c, 1)) ? 0 : 1;
}
HLE("kernel32.dll", IsBadWritePtr, 2) {
  return Readable(Arg(c, 0), Arg(c, 1)) ? 0 : 1;
}
HLE("kernel32.dll", IsBadCodePtr, 1) {
  return ImageAt(Arg(c, 0)) ? 0 : 1;
}

// ---- TLS (TlsAlloc slots live in the TEB, as on Windows) -----------------
// TlsSlots: TEB+0xE10 (4-byte slots) in 32-bit guests, TEB+0x1480 (8-byte)
// in x64 ones. TlsGetValue/TlsSetValue touch only the calling thread's TEB
// and are plain accesses on Windows too; TlsAlloc/TlsFree take a process
// lock there, so they are sync points here.

std::mutex g_tls_mu;
uint64_t g_tls_used = 0;

gaddr TlsSlot(uint32_t i) {
  return g_proc->x64 ? CurrentThread()->teb + 0x1480 + 8 * i
                     : CurrentThread()->teb + 0xE10 + 4 * i;
}

HLE64("kernel32.dll", TlsAlloc, 0) {
  GuestSyncPoint sync;
  std::lock_guard<std::mutex> l(g_tls_mu);
  for (uint32_t i = 0; i < 64; i++)
    if (!(g_tls_used & (1ull << i))) {
      g_tls_used |= 1ull << i;
      return i;
    }
  return 0xFFFFFFFF;
}

HLE64("kernel32.dll", TlsFree, 1) {
  GuestSyncPoint sync;
  std::lock_guard<std::mutex> l(g_tls_mu);
  g_tls_used &= ~(1ull << (Arg(c, 0) & 63));
  return 1;
}

// Fiber-local storage: there are no fibers, so FLS is TLS (the CRT uses it
// through GetProcAddress when present). FlsAlloc's destructor callback is
// not run at thread exit.
HLE("kernel32.dll", FlsAlloc, 1) {
  GuestSyncPoint sync;
  std::lock_guard<std::mutex> l(g_tls_mu);
  for (uint32_t i = 0; i < 64; i++)
    if (!(g_tls_used & (1ull << i))) {
      g_tls_used |= 1ull << i;
      return i;
    }
  return 0xFFFFFFFF;
}
HLE("kernel32.dll", FlsFree, 1) {
  GuestSyncPoint sync;
  std::lock_guard<std::mutex> l(g_tls_mu);
  g_tls_used &= ~(1ull << (Arg(c, 0) & 63));
  return 1;
}
HLE("kernel32.dll", FlsGetValue, 1) {
  uint32_t i = uint32_t(Arg(c, 0));
  if (i >= 64) return 0;
  SetLastError(0);
  return RdPtr(TlsSlot(i));
}
HLE("kernel32.dll", FlsSetValue, 2) {
  uint32_t i = uint32_t(Arg(c, 0));
  if (i >= 64) return 0;
  WrPtr(TlsSlot(i), Arg(c, 1));
  return 1;
}

HLE64("kernel32.dll", TlsGetValue, 1) {
  uint32_t i = uint32_t(Arg(c, 0));
  if (i >= 64) return 0;
  SetLastError(0);
  return RdPtr(TlsSlot(i));
}

HLE64("kernel32.dll", TlsSetValue, 2) {
  uint32_t i = uint32_t(Arg(c, 0));
  if (i >= 64) return 0;
  WrPtr(TlsSlot(i), Arg(c, 1));
  return 1;
}

// ---- critical sections ---------------------------------------------------
// Windows XP's algorithm on the guest CRITICAL_SECTION itself, so the
// fields games may read hold what they hold there: LockCount is -1 when
// free and counts the Enters past the first (recursions and waiters),
// RecursionCount, OwningThread the owner's thread id. An uncontended Enter
// or Leave is one atomic on LockCount with no global lock. A thread that
// finds the section owned by another waits on a host semaphore keyed by
// the section's address (Windows' LockSemaphore, also created on first
// contention), signalled by the Leave that sees a waiter in LockCount.
// Enter and Leave are full barriers (ARCHITECTURE.md, memory ordering)
// through their seq_cst read-modify-write of LockCount: a lock-prefixed
// instruction on x86-64, LDADDAL on arm64 (LSE); no separate fence.
//
// Layouts: 32-bit {DebugInfo, LockCount +4, RecursionCount +8,
// OwningThread +12, LockSemaphore, SpinCount +20}, 24 bytes; x64 with
// pointer-sized DebugInfo, handles and SpinCount: LockCount +8,
// RecursionCount +0xC, OwningThread +0x10, SpinCount +0x20, 40 bytes (the
// thread id in OwningThread's low half).

struct CsSemaphore {
  std::mutex m;
  std::condition_variable cv;
  uint32_t count = 0;
};

std::mutex g_cs_mu;
std::map<gaddr, std::shared_ptr<CsSemaphore>> g_cs;  // under g_cs_mu

std::shared_ptr<CsSemaphore> CsSemaphoreFor(gaddr cs) {
  std::lock_guard<std::mutex> l(g_cs_mu);
  auto& s = g_cs[cs];
  if (!s) s = std::make_shared<CsSemaphore>();
  return s;
}

struct CsLayout {
  uint32_t lock_count, recursion, owner, spin, size;
};
const CsLayout& Cs() {
  static const CsLayout k32{4, 8, 12, 20, 24}, k64{8, 0xC, 0x10, 0x20, 40};
  return g_proc->x64 ? k64 : k32;
}

uint32_t* CsField(gaddr cs, uint32_t off) {
  return reinterpret_cast<uint32_t*>(GPtr(cs + off));
}

// DebugInfo is -1, as Windows Vista and later leave it without debug
// information: games test it to tell an initialised section (GTA IV's RAGE
// takes its heap and other locks only when it is non-zero).
void InitCs(gaddr cs, uint32_t spin) {
  std::memset(GPtr(cs), 0, Cs().size);
  WrPtr(cs, g_proc->x64 ? ~gaddr(0) : 0xFFFFFFFF);  // DebugInfo
  Wr32(cs + Cs().lock_count, 0xFFFFFFFF);  // -1 = free
  Wr32(cs + Cs().spin, spin);              // no effect here
}

// TryEnterCriticalSection: takes a free section or one this thread owns.
bool TryEnterCs(gaddr cs) {
  uint32_t tid = CurrentThread()->tid;
  uint32_t* lock_count = CsField(cs, Cs().lock_count);
  uint32_t* recursion = CsField(cs, Cs().recursion);
  uint32_t* owner = CsField(cs, Cs().owner);
  uint32_t free = 0xFFFFFFFF;
  if (__atomic_compare_exchange_n(lock_count, &free, 0, false, __ATOMIC_SEQ_CST,
                                  __ATOMIC_SEQ_CST)) {
    __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
    __atomic_store_n(recursion, 1, __ATOMIC_RELAXED);
    return true;
  }
  if (__atomic_load_n(owner, __ATOMIC_RELAXED) != tid) return false;
  __atomic_add_fetch(lock_count, 1, __ATOMIC_SEQ_CST);
  __atomic_store_n(recursion, *recursion + 1, __ATOMIC_RELAXED);
  return true;
}

}  // namespace

void AttachExeImports(Ctx& c) {
  std::lock_guard<std::recursive_mutex> l(g_loader_mu);
  for (const PeImport& imp : g_proc->img.imports) {
    Module* d = LiftedDll(imp.dll);
    if (!d || d->refs) continue;
    d->refs = 1;
    if (!AttachLifted(c, *d)) FatalF("%s: DllMain failed at startup", d->name.c_str());
  }
}

void EnterCs(gaddr cs) {
  uint32_t tid = CurrentThread()->tid;
  uint32_t* lock_count = CsField(cs, Cs().lock_count);
  uint32_t* recursion = CsField(cs, Cs().recursion);
  uint32_t* owner = CsField(cs, Cs().owner);
  if (__atomic_add_fetch(lock_count, 1, __ATOMIC_SEQ_CST) != 0) {
    // Only the owner writes OwningThread, so it reads its own tid here
    // exactly when it already holds the section.
    if (__atomic_load_n(owner, __ATOMIC_RELAXED) == tid) {
      __atomic_store_n(recursion, *recursion + 1, __ATOMIC_RELAXED);
      return;
    }
    // Owned by another thread: wait for a Leave to hand it over.
    auto s = CsSemaphoreFor(cs);
    std::unique_lock<std::mutex> l(s->m);
    s->cv.wait(l, [&] { return s->count > 0; });
    s->count--;
  }
  __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
  __atomic_store_n(recursion, 1, __ATOMIC_RELAXED);
}

void LeaveCs(gaddr cs) {
  uint32_t* lock_count = CsField(cs, Cs().lock_count);
  uint32_t* recursion = CsField(cs, Cs().recursion);
  uint32_t* owner = CsField(cs, Cs().owner);
  if (__atomic_load_n(owner, __ATOMIC_RELAXED) != CurrentThread()->tid) {
    // Released as Windows does, which does not check the owner: a game
    // may hand a held section from one thread to another (GTA IV's job
    // workers: one enters, another leaves). Ignoring it left the section
    // held for good and the next Enter waiting forever.
    static std::atomic<bool> noted{false};
    if (!noted.exchange(true))
      WarnF("LeaveCriticalSection(%08llX) by a thread that does not own it (thread %04X, owner %04X): "
            "released, as Windows does\n",
            (unsigned long long)cs, CurrentThread()->tid, __atomic_load_n(owner, __ATOMIC_RELAXED));
  }
  uint32_t rc = *recursion - 1;
  __atomic_store_n(recursion, rc, __ATOMIC_RELAXED);
  if (rc) {
    __atomic_sub_fetch(lock_count, 1, __ATOMIC_SEQ_CST);
    return;
  }
  __atomic_store_n(owner, 0, __ATOMIC_RELAXED);
  if (int32_t(__atomic_sub_fetch(lock_count, 1, __ATOMIC_SEQ_CST)) >= 0) {
    // A waiter counted itself in LockCount: hand the section over.
    auto s = CsSemaphoreFor(cs);
    {
      std::lock_guard<std::mutex> l(s->m);
      s->count++;
    }
    s->cv.notify_one();
  }
}

namespace {

HLE64("kernel32.dll", InitializeCriticalSection, 1) {
  InitCs(Arg(c, 0), 0);
  return 0;
}

HLE64("kernel32.dll", InitializeCriticalSectionAndSpinCount, 2) {
  InitCs(Arg(c, 0), uint32_t(Arg(c, 1)));
  return 1;
}

HLE64("kernel32.dll", DeleteCriticalSection, 1) {
  std::lock_guard<std::mutex> l(g_cs_mu);
  g_cs.erase(Arg(c, 0));
  return 0;
}

HLE64("kernel32.dll", EnterCriticalSection, 1) {
  WaitLog log(c, "EnterCriticalSection", Arg(c, 0));
  EnterCs(Arg(c, 0));
  return 0;
}

HLE64("kernel32.dll", TryEnterCriticalSection, 1) { return TryEnterCs(Arg(c, 0)); }

HLE64("kernel32.dll", LeaveCriticalSection, 1) {
  LeaveCs(Arg(c, 0));
  return 0;
}

// x64 SLIST_HEADER: 16 bytes, empty when zero.
HLE64("kernel32.dll", InitializeSListHead, 1) {
  std::memset(GPtr(Arg(c, 0)), 0, g_proc->x64 ? 16 : 8);
  return 0;
}

// ---- interlocked ---------------------------------------------------------
// seq_cst read-modify-writes, as lifted lock-prefixed instructions are.
// C++ orders those only against other seq_cst atomics, not against the
// plain accesses lifted code makes, so the GuestSyncPoint fences make each
// call a full barrier like `lock xadd`/`xchg`.

HLE("kernel32.dll", InterlockedIncrement, 1) {
  GuestSyncPoint sync;
  return __atomic_add_fetch(reinterpret_cast<uint32_t*>(GPtr(Arg(c, 0))), 1,
                            __ATOMIC_SEQ_CST);
}
HLE("kernel32.dll", InterlockedDecrement, 1) {
  GuestSyncPoint sync;
  return __atomic_sub_fetch(reinterpret_cast<uint32_t*>(GPtr(Arg(c, 0))), 1,
                            __ATOMIC_SEQ_CST);
}
HLE("kernel32.dll", InterlockedExchange, 2) {
  GuestSyncPoint sync;
  return __atomic_exchange_n(reinterpret_cast<uint32_t*>(GPtr(Arg(c, 0))),
                             Arg(c, 1), __ATOMIC_SEQ_CST);
}
HLE("kernel32.dll", InterlockedExchangeAdd, 2) {
  GuestSyncPoint sync;
  return __atomic_fetch_add(reinterpret_cast<uint32_t*>(GPtr(Arg(c, 0))),
                            uint32_t(Arg(c, 1)), __ATOMIC_SEQ_CST);
}
// InterlockedCompareExchange(dest, exchange, comparand): the old value.
HLE("kernel32.dll", InterlockedCompareExchange, 3) {
  GuestSyncPoint sync;
  uint32_t expected = uint32_t(Arg(c, 2));
  __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(GPtr(Arg(c, 0))), &expected,
                              uint32_t(Arg(c, 1)), false, __ATOMIC_SEQ_CST,
                              __ATOMIC_SEQ_CST);
  return expected;
}

// 32-bit SLIST_HEADER: { Next (first entry), Depth (16), Sequence (16) },
// updated as one 64-bit compare-exchange like the cmpxchg8b loops of
// ntdll; an entry starts with its Next link.
namespace {
uint64_t* SListHeader(gaddr h) { return reinterpret_cast<uint64_t*>(GPtr(h)); }
}  // namespace

HLE("kernel32.dll", InterlockedPushEntrySList, 2) {
  GuestSyncPoint sync;
  const gaddr head = Arg(c, 0), entry = Arg(c, 1);
  uint64_t old = __atomic_load_n(SListHeader(head), __ATOMIC_SEQ_CST), next;
  do {
    Wr32(entry, uint32_t(old));
    const uint32_t depth_seq = uint32_t(old >> 32);
    const uint32_t depth = (depth_seq & 0xFFFF) + 1, seq = ((depth_seq >> 16) + 1) & 0xFFFF;
    next = uint64_t(seq << 16 | (depth & 0xFFFF)) << 32 | entry;
  } while (!__atomic_compare_exchange_n(SListHeader(head), &old, next, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
  return uint32_t(old);  // the previous first entry
}

HLE("kernel32.dll", InterlockedPopEntrySList, 1) {
  GuestSyncPoint sync;
  const gaddr head = Arg(c, 0);
  uint64_t old = __atomic_load_n(SListHeader(head), __ATOMIC_SEQ_CST), next;
  do {
    const uint32_t first = uint32_t(old);
    if (!first) return 0;
    const uint32_t depth_seq = uint32_t(old >> 32);
    const uint32_t depth = ((depth_seq & 0xFFFF) - 1) & 0xFFFF,
                   seq = ((depth_seq >> 16) + 1) & 0xFFFF;
    next = uint64_t(seq << 16 | depth) << 32 | Rd32(first);
  } while (!__atomic_compare_exchange_n(SListHeader(head), &old, next, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
  return uint32_t(old);
}

HLE("kernel32.dll", InterlockedFlushSList, 1) {
  GuestSyncPoint sync;
  const gaddr head = Arg(c, 0);
  uint64_t old = __atomic_load_n(SListHeader(head), __ATOMIC_SEQ_CST), next;
  do {
    if (!uint32_t(old)) return 0;
    next = uint64_t((((old >> 48) + 1) & 0xFFFF) << 16) << 32;  // depth 0, next NULL
  } while (!__atomic_compare_exchange_n(SListHeader(head), &old, next, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
  return uint32_t(old);
}

// ---- time ----------------------------------------------------------------

HLE("kernel32.dll", GetTickCount, 0) { return TickCount(); }

HLE64("kernel32.dll", QueryPerformanceFrequency, 1) {
  Wr64(Arg(c, 0), 10000000);
  return 1;
}

HLE64("kernel32.dll", QueryPerformanceCounter, 1) {
  Wr64(Arg(c, 0), PerfCounter());
  return 1;
}

HLE64("kernel32.dll", GetSystemTimeAsFileTime, 1) {
  using namespace std::chrono;
  auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
  Wr64(Arg(c, 0), uint64_t(ns / 100) + kFileTimeUnixEpoch);
  return 0;
}

HLE("kernel32.dll", GetSystemTime, 1) {
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t tt = system_clock::to_time_t(now);
  std::tm t{};
  gmtime_r(&tt, &t);
  int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  WriteSystemTime(Arg(c, 0), t, ms);
  return 0;
}

HLE("kernel32.dll", GetLocalTime, 1) {
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t tt = system_clock::to_time_t(now);
  std::tm t{};
  localtime_r(&tt, &t);
  int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  WriteSystemTime(Arg(c, 0), t, ms);
  return 0;
}

// The host's current UTC offset as a zone without DST transitions
// (TIME_ZONE_ID_UNKNOWN): the guest's local time then equals the host's
// now, though not across a DST change while it runs.
HLE("kernel32.dll", GetTimeZoneInformation, 1) {
  uint32_t tzi = Arg(c, 0);
  std::time_t now = std::time(nullptr);
  std::tm t{};
  localtime_r(&now, &t);
  std::memset(GPtr(tzi), 0, 172);
  Wr32(tzi + 0, uint32_t(int32_t(-t.tm_gmtoff / 60)));  // Bias, minutes
  std::u16string name = Widen(t.tm_zone ? t.tm_zone : "UTC").substr(0, 31);
  std::memcpy(GPtr(tzi + 4), name.data(), name.size() * 2);    // StandardName
  std::memcpy(GPtr(tzi + 88), name.data(), name.size() * 2);   // DaylightName
  return 0;
}

HLE("winmm.dll", timeGetTime, 0) { return TickCount(); }
// The host timer is always fine-grained; nothing to change.
HLE("winmm.dll", timeBeginPeriod, 1) { return Arg(c, 0) ? 0 : 97; }  // TIMERR_NOCANDO
HLE("winmm.dll", timeEndPeriod, 1) { return Arg(c, 0) ? 0 : 97; }

// No joystick or MIDI input drivers: games see zero devices.
HLE("winmm.dll", joyGetNumDevs, 0) { return 0; }
HLE("winmm.dll", joyGetDevCapsA, 3) { return 6; }   // MMSYSERR_NODRIVER
HLE("winmm.dll", joyGetPosEx, 2) { return 6; }
HLE("winmm.dll", midiInGetNumDevs, 0) { return 0; }
HLE("winmm.dll", midiInGetDevCapsA, 3) { return 2; }  // MMSYSERR_BADDEVICEID
HLE("winmm.dll", midiInOpen, 5) { return 2; }
HLE("winmm.dll", midiInStart, 1) { return 5; }  // MMSYSERR_INVALHANDLE
HLE("winmm.dll", midiInStop, 1) { return 5; }
HLE("winmm.dll", midiInClose, 1) { return 5; }

uint32_t (*g_sleep_filter)(uint32_t) = nullptr;  // SetSleepFilter
namespace {
// Sleep(0) gives up the rest of the time slice, as on Windows.
void GuestSleep(uint32_t ms) {
  if (g_sleep_filter) ms = g_sleep_filter(ms);
  if (ms == 0)
    std::this_thread::yield();
  else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
}  // namespace

// A kernel entry on Windows, so a barrier for guests that poll a flag
// with Sleep in the loop.
HLE("kernel32.dll", Sleep, 1) {
  GuestSyncPoint sync;
  if (perf::On()) perf::CountPoll(Arg(c, 0) ? perf::Poll::kSleep : perf::Poll::kSleep0, Caller(c));
  GuestSleep(Arg(c, 0));
  return 0;
}

// (dwMilliseconds, bAlertable): an alertable sleep returns
// WAIT_IO_COMPLETION as soon as it has run queued completion routines.
HLE("kernel32.dll", SleepEx, 2) {
  GuestSyncPoint sync;
  constexpr uint32_t kWaitIoCompletion = 0xC0;
  if (Arg(c, 1) && RunUserApcs(c)) return kWaitIoCompletion;
  GuestSleep(Arg(c, 0));
  return 0;
}

// ---- code pages and locale -----------------------------------------------

HLE("kernel32.dll", GetACP, 0) { return 1252; }
HLE("kernel32.dll", GetOEMCP, 0) { return 437; }
HLE("kernel32.dll", IsValidCodePage, 1) {
  uint32_t cp = Arg(c, 0);
  return cp == 1252 || cp == 437 || cp == 0 ? 1 : 0;
}

HLE("kernel32.dll", GetCPInfo, 2) {
  uint32_t info = Arg(c, 1);
  std::memset(GPtr(info), 0, 20);
  Wr32(info, 1);       // MaxCharSize
  Wr8(info + 4, '?');  // DefaultChar
  return 1;
}

// CP_UTF8 is UTF-8; every other code page is treated as Windows-1252 (the
// ANSI code page reported by GetACP).
constexpr uint32_t kCpUtf8 = 65001;
constexpr uint32_t kMbErrInvalidChars = 0x8, kWcErrInvalidChars = 0x80;
constexpr uint32_t kErrorNoUnicodeTranslation = 1113;

HLE64("kernel32.dll", MultiByteToWideChar, 6) {
  uint32_t cp = uint32_t(Arg(c, 0)), flags = uint32_t(Arg(c, 1));
  gaddr src = Arg(c, 2), dst = Arg(c, 4);
  int32_t n = int32_t(Arg(c, 3)), cap = int32_t(Arg(c, 5));
  if (n < 0) n = int32_t(std::strlen(g_proc->mem.ptr<const char>(src)) + 1);
  std::u16string w;
  if (cp == kCpUtf8) {
    bool bad;
    w = Utf8ToUtf16(GPtr(src), size_t(n), &bad);
    if (bad && (flags & kMbErrInvalidChars)) {
      SetLastError(kErrorNoUnicodeTranslation);
      return 0;
    }
  } else {
    for (int32_t i = 0; i < n; i++) w.push_back(Cp1252ToUnicode(g_proc->mem.read<uint8_t>(src + i)));
  }
  if (!cap) return w.size();
  if (w.size() > size_t(cap)) {
    SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  std::memcpy(GPtr(dst), w.data(), 2 * w.size());
  return w.size();
}

HLE64("kernel32.dll", WideCharToMultiByte, 8) {
  uint32_t cp = uint32_t(Arg(c, 0)), flags = uint32_t(Arg(c, 1));
  gaddr src = Arg(c, 2), dst = Arg(c, 4), default_char = Arg(c, 6), used_default = Arg(c, 7);
  int32_t n = int32_t(Arg(c, 3)), cap = int32_t(Arg(c, 5));
  if (n < 0) n = int32_t(GWStr(src).size() + 1);
  std::u16string w(size_t(n), u'\0');
  std::memcpy(w.data(), GPtr(src), 2 * size_t(n));
  std::string s;
  if (cp == kCpUtf8) {
    // UTF-8 has no default character.
    if (default_char || used_default) {
      SetLastError(kErrorInvalidParameter);
      return 0;
    }
    bool bad;
    s = Utf16ToUtf8(w.data(), w.size(), &bad);
    if (bad && (flags & kWcErrInvalidChars)) {
      SetLastError(kErrorNoUnicodeTranslation);
      return 0;
    }
  } else {
    if (used_default) Wr32(used_default, 0);
    for (char16_t ch : w) {
      bool ok = true;
      s.push_back(char(UnicodeToCp1252(ch, &ok)));
      if (!ok && used_default) Wr32(used_default, 1);
    }
  }
  if (!cap) return s.size();
  if (s.size() > size_t(cap)) {
    SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  std::memcpy(GPtr(dst), s.data(), s.size());
  return s.size();
}

HLE("kernel32.dll", GetStringTypeW, 4) {
  uint32_t type = Arg(c, 0), src = Arg(c, 1), out = Arg(c, 3);
  int32_t n = int32_t(Arg(c, 2));
  if (type != kCtCtype1) return 0;
  if (n < 0) n = int32_t(GWStr(src).size() + 1);
  for (int32_t i = 0; i < n; i++) Wr16(out + 2 * i, CharType1(Rd16(src + 2 * i)));
  return 1;
}

HLE("kernel32.dll", GetStringTypeA, 5) {
  uint32_t type = Arg(c, 1), src = Arg(c, 2), out = Arg(c, 4);
  int32_t n = int32_t(Arg(c, 3));
  if (type != kCtCtype1) return 0;
  if (n < 0) n = int32_t(std::strlen(g_proc->mem.ptr<const char>(src)) + 1);
  for (int32_t i = 0; i < n; i++)
    Wr16(out + 2 * i,
         CharType1(Cp1252ToUnicode(g_proc->mem.read<uint8_t>(src + i))));
  return 1;
}

HLE("kernel32.dll", LCMapStringW, 6) {
  uint32_t flags = Arg(c, 1), src = Arg(c, 2), dst = Arg(c, 4);
  int32_t n = int32_t(Arg(c, 3)), cap = int32_t(Arg(c, 5));
  if (n < 0) n = int32_t(GWStr(src).size() + 1);
  if (!cap) return uint32_t(n);
  if (n > cap) return 0;
  for (int32_t i = 0; i < n; i++)
    Wr16(dst + 2 * i, MapCase(Rd16(src + 2 * i), flags));
  return uint32_t(n);
}

HLE("kernel32.dll", LCMapStringA, 6) {
  uint32_t flags = Arg(c, 1), src = Arg(c, 2), dst = Arg(c, 4);
  int32_t n = int32_t(Arg(c, 3)), cap = int32_t(Arg(c, 5));
  if (n < 0) n = int32_t(std::strlen(g_proc->mem.ptr<const char>(src)) + 1);
  if (!cap) return uint32_t(n);
  if (n > cap) return 0;
  for (int32_t i = 0; i < n; i++) {
    bool ok;
    uint16_t u = MapCase(Cp1252ToUnicode(g_proc->mem.read<uint8_t>(src + i)),
                         flags);
    Wr8(dst + i, UnicodeToCp1252(u, &ok));
  }
  return uint32_t(n);
}

// CompareStringA/W(locale, flags, s1, n1, s2, n2): CSTR_LESS_THAN (1),
// CSTR_EQUAL (2) or CSTR_GREATER_THAN (3), by code point after the case
// mapping NORM_IGNORECASE (1) asks for; the other flags are ignored.
uint32_t CompareStrings(std::u16string a, std::u16string b, uint32_t flags) {
  if (flags & 1) {
    for (auto& ch : a) ch = MapCase(ch, kLcmapLowercase);
    for (auto& ch : b) ch = MapCase(ch, kLcmapLowercase);
  }
  const int r = a.compare(b);
  return r < 0 ? 1 : r == 0 ? 2 : 3;
}
std::u16string CountedW(gaddr s, int32_t n) {
  if (n < 0) return GWStr(s);
  std::u16string w(size_t(n), u'\0');
  for (int32_t i = 0; i < n; i++) w[size_t(i)] = Rd16(s + 2 * uint32_t(i));
  return w;
}
std::u16string CountedA(gaddr s, int32_t n) {
  std::string a = n < 0 ? GStr(s) : std::string(g_proc->mem.ptr<const char>(s), size_t(n));
  return Widen(a);
}
HLE("kernel32.dll", CompareStringW, 6) {
  return CompareStrings(CountedW(Arg(c, 2), int32_t(Arg(c, 3))),
                        CountedW(Arg(c, 4), int32_t(Arg(c, 5))), Arg(c, 1));
}
HLE("kernel32.dll", CompareStringA, 6) {
  return CompareStrings(CountedA(Arg(c, 2), int32_t(Arg(c, 3))),
                        CountedA(Arg(c, 4), int32_t(Arg(c, 5))), Arg(c, 1));
}

// FoldStringW(flags, src, n, dst, cap): Windows-1252 text has nothing to
// fold, so a copy.
HLE("kernel32.dll", FoldStringW, 5) {
  const std::u16string w = CountedW(Arg(c, 1), int32_t(Arg(c, 2)));
  const uint32_t n = uint32_t(w.size()) + (int32_t(Arg(c, 2)) < 0 ? 1 : 0);
  const uint32_t cap = Arg(c, 4);
  if (!cap) return n;
  if (n > cap) {
    SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  for (uint32_t i = 0; i < n; i++) Wr16(Arg(c, 3) + 2 * i, i < w.size() ? w[i] : 0);
  return n;
}

HLE("kernel32.dll", lstrcmpiA, 2) {
  const int r = CompareStrings(Widen(GStr(Arg(c, 0))), Widen(GStr(Arg(c, 1))), 1);
  return uint32_t(r - 2);  // -1, 0, 1
}
HLE("kernel32.dll", lstrlenW, 1) {
  return Arg(c, 0) ? uint32_t(GWStr(Arg(c, 0)).size()) : 0;
}

HLE("kernel32.dll", GetUserDefaultLCID, 0) { return 0x0409; }
HLE("kernel32.dll", GetUserDefaultLangID, 0) { return 0x0409; }
HLE("kernel32.dll", GetUserDefaultUILanguage, 0) { return 0x0409; }
HLE("kernel32.dll", GetSystemDefaultUILanguage, 0) { return 0x0409; }
HLE("kernel32.dll", IsValidLocale, 2) { return Arg(c, 0) == 0x0409 ? 1 : 0; }

HLE("kernel32.dll", EnumSystemLocalesA, 2) {
  uint32_t buf = HeapAllocProcess(16, true);
  PutStr(buf, "00000409");
  CallGuest(c, Arg(c, 0), {buf});
  g_process_heap->Free(buf);
  return 1;
}

std::string LocaleString(uint32_t type) {
  switch (type & 0xFFFF) {
    case 0x0001: return "0409";            // LOCALE_ILANGUAGE
    case 0x0002: return "English (United States)";
    case 0x0003: return "ENU";             // LOCALE_SABBREVLANGNAME
    case 0x0004: return "English";         // LOCALE_SNATIVELANGNAME
    case 0x0005: return "1";               // LOCALE_ICOUNTRY
    case 0x0006: return "United States";
    case 0x0007: return "USA";             // LOCALE_SABBREVCTRYNAME
    case 0x0009: return "0409";            // LOCALE_IDEFAULTLANGUAGE
    case 0x000A: return "1";               // LOCALE_IDEFAULTCOUNTRY
    case 0x000B: return "437";             // LOCALE_IDEFAULTCODEPAGE
    case 0x000E: return ".";               // LOCALE_SDECIMAL
    case 0x000F: return ",";               // LOCALE_STHOUSAND
    case 0x0010: return "3;0";             // LOCALE_SGROUPING
    case 0x0014: return "$";               // LOCALE_SCURRENCY
    case 0x0015: return ".";               // LOCALE_SMONDECIMALSEP
    case 0x0016: return ",";               // LOCALE_SMONTHOUSANDSEP
    case 0x0018: return "3;0";
    case 0x0019: return "2";
    case 0x001A: return "2";
    case 0x001B: return "0";
    case 0x001C: return "0";
    case 0x0050: return "";                // LOCALE_SPOSITIVESIGN
    case 0x0051: return "-";               // LOCALE_SNEGATIVESIGN
    case 0x0059: return "en";              // LOCALE_SISO639LANGNAME
    case 0x005A: return "US";              // LOCALE_SISO3166CTRYNAME
    case 0x005C: return "en-US";           // LOCALE_SNAME (Vista and later)
    case 0x1001: return "English";         // LOCALE_SENGLANGUAGE
    case 0x1002: return "United States";   // LOCALE_SENGCOUNTRY
    case 0x1004: return "1252";            // LOCALE_IDEFAULTANSICODEPAGE
    default: return {};
  }
}

// GetLocaleInfoA/W(lcid, type, buf, cap).
uint32_t LocaleInfo(Ctx& c, bool wide) {
  std::string s = LocaleString(Arg(c, 1));
  if (s.empty() && (Arg(c, 1) & 0xFFFF) != 0x0050) {
    WarnF("GetLocaleInfo%c(%X, %X): unknown type\n", wide ? 'W' : 'A', uint32_t(Arg(c, 0)),
          uint32_t(Arg(c, 1)));
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  return PutCounted(Arg(c, 2), uint32_t(Arg(c, 3)), s, wide);
}

HLE64("kernel32.dll", GetLocaleInfoA, 4) { return LocaleInfo(c, false); }
HLE("kernel32.dll", GetLocaleInfoW, 4) { return LocaleInfo(c, true); }

}  // namespace
}  // namespace hle
}  // namespace recomp

namespace recomp {

static double WaitLogMs() {
  static const double ms = [] {
    const char* e = Env("WAIT_LOG");
    return e ? std::atof(e) : 0.0;
  }();
  return ms;
}

WaitLog::WaitLog(const Ctx& c, const char* w, uint64_t o)
    : what(w), object(o), caller(WaitLogMs() > 0 ? Rd32(c.esp) : 0), esp(c.esp),
      t0(WaitLogMs() > 0 ? platform::MonotonicNs() : 0) {}

WaitLog::~WaitLog() {
  if (!t0) return;
  const double ms = double(platform::MonotonicNs() - t0) / 1e6;
  if (ms <= WaitLogMs()) return;
  // Callers further up, guessed: stack words that point into the image.
  char up[64] = "";
  const PeImage& img = g_proc->img;
  for (uint32_t a = esp + 4, n = 0, len = 0; a < esp + 256 && n < 4; a += 4) {
    const uint32_t v = Rd32(a);
    if (v > img.image_base && v < img.image_base + img.image_size) {
      len += uint32_t(std::snprintf(up + len, sizeof up - len, " %08X", v));
      n++;
    }
  }
  std::fprintf(stderr, "wait: thread %04X %s(%08llX) %.1f ms from %08X (stack:%s)\n",
               CurrentThread()->tid, what, (unsigned long long)object, ms, caller, up);
}

namespace hle {
void SetSleepFilter(uint32_t (*filter)(uint32_t)) { g_sleep_filter = filter; }
}  // namespace hle

}  // namespace recomp

namespace recomp { namespace hle {

// Unicode (W) entry points and small process queries
HLE("kernel32.dll", OutputDebugStringW, 1) { return 0; }
HLE("kernel32.dll", lstrcmpW, 2) {
  std::u16string a = GWStr(Arg(c, 0)), b = GWStr(Arg(c, 1));
  return a < b ? uint32_t(-1) : a > b ? 1 : 0;
}
HLE("kernel32.dll", GetSystemDefaultLCID, 0) { return 0x0409; }
HLE("kernel32.dll", GetSystemDefaultLangID, 0) { return 0x0409; }
HLE("kernel32.dll", GetExitCodeProcess, 2) { if (Arg(c,1)) Wr32(Arg(c,1), 259 /*STILL_ACTIVE*/); return 1; }
// The guest reads as created when first asked, its parent a minute earlier:
// programs that look up their parent check it started before them.
HLE("kernel32.dll", GetProcessTimes, 5) {
  using namespace std::chrono;
  static const uint64_t start =
      uint64_t(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100) +
      kFileTimeUnixEpoch;
  uint32_t h = uint32_t(Arg(c, 0)), pid = kGuestPid;
  if (h != 0xFFFFFFFF) {
    auto p = std::dynamic_pointer_cast<ProcessObject>(GetHandle(h));
    if (!p) { SetLastError(kErrorInvalidHandle); return 0; }
    pid = p->pid;
  }
  if (Arg(c, 1)) Wr64(Arg(c, 1), pid == kGuestPid ? start : start - 60ull * 10000000ull);
  for (int i = 2; i < 5; i++)
    if (Arg(c, i)) Wr64(Arg(c, i), 0);
  return 1;
}
HLE("kernel32.dll", LocalFileTimeToFileTime, 2) { if (Arg(c,0)&&Arg(c,1)) Wr64(Arg(c,1), Rd64(Arg(c,0))); return 1; }
HLE("kernel32.dll", SetThreadIdealProcessor, 2) { return (uint32_t)-1; }
HLE("kernel32.dll", CreateNamedPipeW, 8) { SetLastError(2); return (g_proc->x64?~0ull:0xFFFFFFFFu); }
HLE("kernel32.dll", SetNamedPipeHandleState, 4) { return 1; }
// PROCESSENTRY32W (x86): 9 dwords, then szExeFile[260] at +36; 556 bytes.
HLE("kernel32.dll", Process32FirstW, 2) {
  auto o = GetHandle(uint32_t(Arg(c, 0)));
  gaddr pe = Arg(c, 1);
  if (!o || o->type != KObject::Type::Snapshot) { SetLastError(kErrorInvalidHandle); return 0; }
  if (!pe || Rd32(pe) < 556) { SetLastError(24 /*ERROR_BAD_LENGTH*/); return 0; }
  for (gaddr off = 4; off < 36; off += 4) Wr32(pe + off, 0);
  Wr32(pe + 8, kGuestPid);
  Wr32(pe + 20, 1);
  Wr32(pe + 24, kGuestParentPid);
  Wr32(pe + 28, 8);
  const std::u16string w = Widen(Game().exe_name.substr(0, 259));
  for (size_t i = 0; i < w.size(); i++) Wr16(pe + 36 + 2 * gaddr(i), w[i]);
  Wr16(pe + 36 + 2 * gaddr(w.size()), 0);
  return 1;
}
HLE("kernel32.dll", Process32NextW, 2) { SetLastError(18); return 0; }
// (app, key, nDefault, file): nothing writes .ini files here
// (WritePrivateProfileSectionW is a no-op), so a key is found only in
// GameConfig::profile_ints; any other gets nDefault, as on Windows for a
// missing file.
HLE("kernel32.dll", GetPrivateProfileIntW, 4) {
  auto str = [](gaddr p) {
    std::string s;
    if (p) for (char16_t ch : GWStr(p)) s += char(ch);
    return Lower(s);
  };
  const std::string section = str(Arg(c, 0)), key = str(Arg(c, 1));
  for (const auto& e : Game().profile_ints)
    if (Lower(e.section) == section && Lower(e.key) == key) return uint32_t(e.value);
  return uint32_t(Arg(c, 2));
}
HLE("kernel32.dll", WritePrivateProfileSectionW, 3) { return 1; }

}} // compat
