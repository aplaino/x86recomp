#pragma once
// The Win32 process on the x86 core's (x86/runtime.h): the game's PE
// image and its lifted DLLs, the PEB, each thread's TEB and static TLS,
// and what kernel32 keeps for the whole process.

#include <cstdint>
#include <string>
#include <vector>

#include "win32/pe_image.h"
#include "x86/runtime.h"

namespace recomp {

namespace win32 {

// What the Win32 HLE needs to know about the game it runs. Filled in by
// the game's main() and passed to RunProcess.
struct GameConfig : recomp::GameConfig {
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

}  // namespace win32

// Fixed guest layout (see ARCHITECTURE.md). TEBs grow down from kTebTop.
inline constexpr uint32_t kPebVa = 0x7FFDF000;
inline constexpr uint32_t kTebTop = 0x7FFDE000;
inline constexpr uint32_t kTebStride = 0x1000;

// x64 guests (ARCHITECTURE.md, "64-bit guests"): a 64 GiB arena with
// nothing below 4 GiB. From the top: the thunks (x86/layout.h), the
// pseudo module handles (win32/hle.h), the PEB, then TEBs growing down
// (two pages each: the x64 TEB is 0x1838 bytes); the allocator hands out
// the rest above 4 GiB.
inline constexpr gaddr kPebVa64 = 0xFFD000000;
inline constexpr gaddr kTebTop64 = kPebVa64;
inline constexpr uint32_t kTebStride64 = 0x2000;
inline constexpr gaddr kTebLo64 = kTebTop64 - 0x1000000;  // room for 2048

// A lifted DLL (win32::GameConfig::lifted_dlls). All of them are mapped at
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

// The Win32 side of g_proc.
struct Win32Process {
  win32::GameConfig game;
  PeImage img;
  std::vector<Module> modules;  // fixed after startup
  std::string game_args; // appended to the guest command line
  gaddr process_heap = 0;
  gaddr cmdline_a = 0, cmdline_w = 0;
  gaddr environ_a = 0, environ_w = 0;
};

extern Win32Process* g_win32;

// The running game's configuration (valid once RunProcess has started).
const win32::GameConfig& Game();

// Thread t's TEB, which fs (32-bit) or gs (x64) finds.
inline gaddr Teb(const Thread* t) {
  return g_proc->x64 ? t->ctx.gs_base : t->ctx.fs_base;
}

// The EXE's or a lifted DLL's image containing va, or null.
const PeImage* ImageAt(gaddr va);

// (Re)maps lifted DLL m from its file: fresh data, imports bound.
void MapModule(Module& m);

// Sets up the process (arena, image, PEB, heaps, command line) and runs
// the guest entry point on a new guest thread. exe_path is the host file
// to load (it need not be in game_dir, e.g. an unpacked copy). Returns the
// exit code.
int RunProcess(const win32::GameConfig& game, const std::string& exe_path,
               const std::string& game_dir, const std::string& game_args = {});

}  // namespace recomp
