#pragma once
// The x86 core's process: guest memory, the guest's threads and calls
// between the host and lifted code, whatever the platform. A platform
// module (platforms/<name>) loads the program's images and sets the
// hooks below; the services HLE code builds on are here.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "runtime/guest_alloc.h"
#include "runtime/guest_memory.h"
#include "x86/ctx.h"
#include "x86/layout.h"

namespace recomp {

// What every platform needs to know about the game it runs; a platform's
// own configuration extends it (win32::GameConfig).
struct GameConfig {
  // Short id, e.g. "game": the per-user data directory is
  // $XDG_DATA_HOME/<id>, and dev/test switches are read from environment
  // variables named <ID>_<SWITCH> (GAME_TRACE, ...).
  std::string id;
};

// Dev/test switch `name` from the environment: getenv("<ID>_<name>"),
// e.g. Env("TRACE") reads GAME_TRACE for id "game". Null if unset.
const char* Env(const char* name);

// Processors reported to the game (PEB, GetSystemInfo): never 1. Games
// that see one CPU may install uniprocessor lock functions (plain inc/dec),
// which corrupt their data once their threads really run in parallel.
uint32_t GuestProcessorCount();

struct Thread;

struct Process {
  GameConfig game;
  GuestMemory mem;
  GuestAllocator vm;     // VirtualAlloc-style regions
  std::string game_dir;  // host path of the game install, no trailing '/'
  bool x64 = false;      // x64 code (Win64 ABI unless sysv), else 32-bit
  bool sysv = false;     // x64 with the System V ABI (a console platform's)
  uint16_t fpu_control = 0x027F;  // x87 control word of new threads (Windows')
  // Code ranges [lo, lo + size) of the program's lifted images, which
  // dispatch looks lifted functions up in; the first is the fast path.
  std::vector<std::pair<gaddr, uint64_t>> code;
  std::atomic<uint32_t> next_tid{0x100};

  // The platform's hooks; each may be null.
  // Per-thread setup run by CreateGuestThread once the Ctx is set up: the
  // thread's control block (a TEB, a TCB) and the segment base that finds
  // it (Ctx::fs_base or gs_base).
  void (*thread_init)(Thread* t) = nullptr;
  // A guest thread's start routine returned or exited (GuestThreadExit),
  // on that thread: whoever waits for it may go on.
  void (*thread_exit)(Thread* t, uint32_t code) = nullptr;
  // A fault in guest memory that is not the runtime's own (lazy_mem.h),
  // in 32-bit guest code: the platform's exception dispatch (Win32's SEH
  // chain). Returns true to retry the faulting instruction; may throw
  // GuestResume to continue elsewhere. Without it the fault is fatal.
  bool (*guest_fault)(Ctx& c, bool write, gaddr addr) = nullptr;
  // Services the host's window system for the guest (Win32: SDL events to
  // window messages), after each presented frame (x86/present.h).
  void (*pump_events)() = nullptr;
};

extern Process* g_proc;

struct Thread {
  Ctx ctx;
  uint32_t tid = 0;
  gaddr stack_lo = 0, stack_hi = 0;
};

// Thrown to end a guest thread (ExitThread, pthread_exit): unwinds its host
// stack back to where it started, which returns `code` as its exit code.
struct GuestThreadExit {
  uint32_t code;
};

// <ID>_BACKGROUND=1 (used for automated test runs): never move or hide the
// host pointer, don't take focus when the window appears, and answer
// message boxes with their default button.
bool Background();

// Per-host-thread pointer to the guest thread running on it.
Thread* CurrentThread();

// Guest memory helpers for HLE code.
inline uint8_t* GPtr(gaddr va) { return g_proc->mem.ptr(va); }
inline uint32_t Rd32(gaddr va) { return g_proc->mem.read<uint32_t>(va); }
inline uint16_t Rd16(gaddr va) { return g_proc->mem.read<uint16_t>(va); }
inline uint8_t Rd8(gaddr va) { return g_proc->mem.read<uint8_t>(va); }
inline void Wr32(gaddr va, uint32_t v) { g_proc->mem.write<uint32_t>(va, v); }
inline uint64_t Rd64(gaddr va) { return g_proc->mem.read<uint64_t>(va); }
inline void Wr64(gaddr va, uint64_t v) { g_proc->mem.write<uint64_t>(va, v); }
inline void Wr16(gaddr va, uint16_t v) { g_proc->mem.write<uint16_t>(va, v); }
inline void Wr8(gaddr va, uint8_t v) { g_proc->mem.write<uint8_t>(va, v); }
// A guest pointer stored in guest memory: 4 bytes in 32-bit guests, 8 in
// x64 ones.
inline gaddr RdPtr(gaddr va) { return g_proc->x64 ? Rd64(va) : Rd32(va); }
inline void WrPtr(gaddr va, gaddr v) {
  if (g_proc->x64)
    Wr64(va, v);
  else
    Wr32(va, uint32_t(v));
}
std::string GStr(gaddr va);                  // NUL-terminated char*
std::u16string GWStr(gaddr va);              // NUL-terminated WCHAR*
void PutStr(gaddr va, const std::string& s); // writes s + NUL
void PutWStr(gaddr va, const std::string& s); // ASCII s as WCHARs + NUL

// Calls guest function `va` with stdcall/cdecl arguments from the host
// (WndProc, enumeration callbacks, thread start routines). Returns eax.
// Handles both conventions: the stack pointer is restored afterwards. In
// x64 processes the Win64 convention: rcx, rdx, r8, r9, then the stack
// above 32 bytes of shadow space; returns rax. System V processes:
// rdi, rsi, rdx, rcx, r8, r9, then the stack.
uint64_t CallGuest(Ctx& c, gaddr va, std::initializer_list<uint64_t> args);

// Dispatch for x64 guests (lifted.h declares it to x64 lifted code only).
void Dispatch(Ctx& c, uint64_t target, uint64_t va);

// Creates a guest thread (its stack, the platform's thread_init) and
// initialises its Ctx. Does not start it.
Thread* CreateGuestThread(uint64_t stack_size);

// A platform's process bring-up (Win32's RunProcess, a console platform's
// loader). StartProcess creates g_proc with its arena and thunk region;
// InitDispatch registers Process::code once the images are mapped;
// RunMainThread runs body (its guest calls) as guest thread t on the
// process main thread, with a big host stack, and returns its exit code.
Process& StartProcess(const GameConfig& game, const std::string& game_dir, bool x64);
void InitDispatch();
uint32_t RunMainThread(Thread* t, const std::function<uint64_t(Ctx&)>& body);
// For platforms whose guest has no UI thread of its own (consoles): body
// runs as guest thread t on a host thread of its own, and the process
// main thread runs host_loop (the window, its events, presentation),
// which returns once done is set (body has returned).
uint32_t RunMainThreadBeside(Thread* t, const std::function<uint64_t(Ctx&)>& body,
                             const std::function<void(const std::atomic<bool>&)>& host_loop);
// Whether va is the entry of a lifted function in Process::code (once
// InitDispatch has run).
bool IsLifted(gaddr va);

// Per-user data directory: $XDG_DATA_HOME/<id>, else ~/.local/share/<id>
// (~/Library/Application Support/<id> on macOS) (settings, saves, the guest's C: drive, crash and hang reports).
std::string DataDir();
// Creates `path` and any missing parents (mkdir -p).
void MakeDirs(const std::string& path);
// A new report file name, e.g. ReportPath("crash") is
// DataDir()/crashes/crash-<time>-<pid>.txt; creates the directory.
std::string ReportPath(const char* kind);

// Runs guest function entry(param) on a new detached host thread bound to
// guest thread t (from CreateGuestThread).
void StartHostThread(Thread* t, gaddr entry, uint64_t param);

// Runs host function fn on a new detached host thread bound to guest thread
// t (from CreateGuestThread), so fn may CallGuest(t->ctx, ...).
void StartServiceThread(Thread* t, std::function<void()> fn);

// Memory ordering of guest synchronisation calls. On Windows every one of
// them (critical sections, waits, events, mutexes, Interlocked*, thread
// start and exit, TlsAlloc/TlsFree, Sleep) is a full barrier: a locked
// instruction or a kernel entry. x86 is TSO, but arm64 hosts are weakly
// ordered and lifted code reaches guest memory with plain loads and
// stores, so a guest that hands data over through one of these calls
// relies on more than the host primitive behind it gives. A host mutex is
// acquire on lock and release on unlock: enough for data written inside a
// critical section to be seen by the next owner, but accesses outside it
// may move into it and pass each other there, so lock + unlock is not a
// full barrier. Each such handler therefore opens a GuestSyncPoint, a
// seq_cst fence on entry and on exit (also when it returns early or
// throws): every guest access before the call is ordered before the
// call's own effects, and those before every guest access after it.
inline void GuestSyncFence() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
struct GuestSyncPoint {
  GuestSyncPoint() { GuestSyncFence(); }
  ~GuestSyncPoint() { GuestSyncFence(); }
  GuestSyncPoint(const GuestSyncPoint&) = delete;
  GuestSyncPoint& operator=(const GuestSyncPoint&) = delete;
};

}  // namespace recomp
