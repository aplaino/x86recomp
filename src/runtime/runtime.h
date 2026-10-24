#pragma once
// Process-wide runtime state and the services HLE code builds on: guest
// memory access, calling back into lifted code, guest threads.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "runtime/ctx.h"
#include "runtime/guest_alloc.h"
#include "runtime/guest_memory.h"
#include "runtime/pe_image.h"

namespace recomp {

// What the runtime needs to know about the game it runs. Filled in by the
// game's main() and passed to RunProcess.
struct GameConfig {
  // Short id, e.g. "game": the per-user data directory is
  // $XDG_DATA_HOME/<id>, and dev/test switches are read from environment
  // variables named <ID>_<SWITCH> (GAME_TRACE, ...).
  std::string id;
  // Where the game believes it is installed, e.g. "C:\Games\Game";
  // guest paths below it map onto the host game directory.
  std::string guest_dir;
  // The EXE's file name in guest_dir (GetModuleFileName, command line).
  std::string exe_name;

  // Registry values the retail installer would have written, seeded on
  // the first run (the registry file does not exist yet). Keys are full
  // paths with the root abbreviated: "HKLM\Software\...".
  struct RegistryString {
    std::string key, name, value;
  };
  std::vector<RegistryString> registry;

  // Optional: where the game keeps its own cursor and how it scales
  // pointer movement, for games that re-centre the pointer every frame
  // and move their cursor by the delta (user32 GetCursorPos keeps that
  // cursor under the host pointer). `userdata` is GWL_USERDATA of the
  // game's main window. Returns false if not available yet.
  bool (*game_cursor)(uint32_t userdata, int32_t* x, int32_t* y, float* kx,
                      float* ky) = nullptr;

  // How a game window that covers the whole screen the game sees (a
  // fullscreen window after a display mode change, DirectDraw exclusive
  // mode) is shown on the host. The frame keeps its aspect either way
  // (black bars where it differs from the screen's). <ID>_FULLSCREEN=
  // window|desktop|exclusive overrides it; automated runs (<ID>_BACKGROUND)
  // default to window.
  enum class Fullscreen {
    Window,     // a host window of the mode's size
    Desktop,    // borderless fullscreen at the desktop's resolution
    Exclusive,  // fullscreen in the host display mode closest to the game's
  };
  Fullscreen fullscreen = Fullscreen::Desktop;

  // Video memory DirectDraw reports, in MB. Games pick detail settings
  // from it; a game's best profile may need more than the default.
  uint32_t video_memory_mb = 64;

  // DLLs the game loads itself (LoadLibrary) whose code is lifted with
  // the EXE (recomp_lift --module), e.g. a game logic DLL. They
  // are read from guest_dir (the host game directory) by file name.
  struct LiftedDll {
    std::string name;   // file name, e.g. "game.dll"
    uint32_t base = 0;  // the lifter's --base for it; 0 = preferred base
  };
  std::vector<LiftedDll> lifted_dlls;
};

// The running game's configuration (valid once RunProcess has started).
const GameConfig& Game();

// Dev/test switch `name` from the environment: getenv("<ID>_<name>"),
// e.g. Env("TRACE") reads GAME_TRACE for id "game". Null if unset.
const char* Env(const char* name);

// Processors reported to the game (PEB, GetSystemInfo): never 1. Games
// that see one CPU may install uniprocessor lock functions (plain inc/dec),
// which corrupt their data once their threads really run in parallel.
uint32_t GuestProcessorCount();

// Fixed guest layout (see ARCHITECTURE.md). TEBs grow down from kTebTop.
inline constexpr uint32_t kPebVa = 0x7FFDF000;
inline constexpr uint32_t kTebTop = 0x7FFDE000;
inline constexpr uint32_t kTebStride = 0x1000;
// Return address pushed when the host calls into guest code; reaching it
// means the guest function returned to us.
inline constexpr uint32_t kReturnToHost = kThunkBase + kThunkRegionSize - 0x10;

// x64 guests (ARCHITECTURE.md, "64-bit guests"): a 64 GiB arena with
// nothing below 4 GiB. From the top: the thunks (pe_image.h), the pseudo
// module handles (hle.h), the PEB, then TEBs growing down (two pages each:
// the x64 TEB is 0x1838 bytes); the allocator hands out the rest above
// 4 GiB.
inline constexpr unsigned kArenaBits64 = 36;
inline constexpr gaddr kUserLo64 = 0x100000000;
inline constexpr gaddr kPebVa64 = 0xFFD000000;
inline constexpr gaddr kTebTop64 = kPebVa64;
inline constexpr uint32_t kTebStride64 = 0x2000;
inline constexpr gaddr kTebLo64 = kTebTop64 - 0x1000000;  // room for 2048
inline constexpr gaddr kReturnToHost64 = kThunkBase64 + kThunkRegionSize - 0x10;

// A lifted DLL (GameConfig::lifted_dlls). All of them are mapped at
// startup, so their address ranges stay free; LoadLibrary makes one
// visible and runs its DllMain.
struct Module {
  std::string name;  // lower case, e.g. "game.dll"
  std::string host_path;
  gaddr base = 0;    // where the lifter put it; 0 = preferred base
  PeImage img;
  int refs = 0;        // LoadLibrary count (under kernel32's loader lock)
  bool stale = false;  // has run since it was mapped: map afresh on load
  int tls_index = -1;  // its static TLS slot (x64 guests), -1: none
};

struct Process {
  GameConfig game;
  GuestMemory mem;
  PeImage img;
  std::vector<Module> modules;  // fixed after startup
  GuestAllocator vm;     // VirtualAlloc-style regions
  std::string game_dir;  // host path of the game install, no trailing '/'
  std::string game_args; // appended to the guest command line
  bool x64 = false;      // the EXE is a PE32+ AMD64 image (Win64 ABI)
  gaddr process_heap = 0;
  gaddr cmdline_a = 0, cmdline_w = 0;
  gaddr environ_a = 0, environ_w = 0;
  std::atomic<uint32_t> next_tid{0x100};
};

extern Process* g_proc;

struct Thread {
  Ctx ctx;
  uint32_t tid = 0;
  gaddr teb = 0;
  gaddr stack_lo = 0, stack_hi = 0;
};

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

// The EXE's or a lifted DLL's image containing va, or null.
const PeImage* ImageAt(gaddr va);

// (Re)maps lifted DLL m from its file: fresh data, imports bound.
void MapModule(Module& m);

// Calls guest function `va` with stdcall/cdecl arguments from the host
// (WndProc, enumeration callbacks, thread start routines). Returns eax.
// Handles both conventions: the stack pointer is restored afterwards. In
// x64 processes the Win64 convention: rcx, rdx, r8, r9, then the stack
// above 32 bytes of shadow space; returns rax.
uint64_t CallGuest(Ctx& c, gaddr va, std::initializer_list<uint64_t> args);

// Dispatch for x64 guests (lifted.h declares it to x64 lifted code only).
void Dispatch(Ctx& c, uint64_t target, uint64_t va);

// Sets up the process (arena, image, PEB, heaps, command line) and runs
// the guest entry point on a new guest thread. exe_path is the host file
// to load (it need not be in game_dir, e.g. an unpacked copy). Returns the
// exit code.
int RunProcess(const GameConfig& game, const std::string& exe_path,
               const std::string& game_dir, const std::string& game_args = {});

// Creates the guest-side structures of a thread (TEB, TLS, stack) and
// initialises its Ctx. Does not start it.
Thread* CreateGuestThread(uint64_t stack_size);

// Per-user data directory: $XDG_DATA_HOME/<id>, else ~/.local/share/<id>
// (~/Library/Application Support/<id> on macOS) (settings, saves, the guest's C: drive, crash and hang reports).
std::string DataDir();
// Creates `path` and any missing parents (mkdir -p).
void MakeDirs(const std::string& path);
// A new report file name, e.g. ReportPath("crash") is
// DataDir()/crashes/crash-<time>-<pid>.txt; creates the directory.
std::string ReportPath(const char* kind);

// Runs host function fn on a new detached host thread bound to guest thread
// t (from CreateGuestThread), so fn may CallGuest(t->ctx, ...).
void StartServiceThread(Thread* t, std::function<void()> fn);

}  // namespace recomp
