#pragma once
// An Orbis (PS4) process: what the kernel and libkernel's dynamic linker
// do before a game's code runs, for lifted modules. Maps the eboot and the
// PRXs the game ships (each where the lifter put it), binds every import
// to another module's export or else to the HLE (orbis/hle.h), lays out
// static TLS, gives each thread a TCB (through fs), runs the PRXs'
// initialisers and calls the eboot's entry point.

#include <string>
#include <vector>

#include "orbis/app.h"
#include "x86/runtime.h"

namespace recomp::orbis {

// Each thread's TCB (Ctx::fs_base): its own address, its DTV, then
// libkernel's per-thread words: errno at kTcbErrno.
inline constexpr uint64_t kTcbErrno = 0x20;

struct Prx {
  std::string path;  // host path
  gaddr base;        // the lifter's --base for it
};

struct Config {
  std::string eboot;              // host path
  std::vector<Prx> prxs;          // in initialisation order
  std::vector<std::string> args;  // argv, argv[0] first
  uint64_t main_stack = 8u << 20;
  ParamSfo param;  // the application's param.sfo (empty without one)
};

// The eboot's process parameters (PT_SCE_PROCPARAM), 0 if it has none.
gaddr ProcParam();

// The application's param.sfo (Config::param).
const ParamSfo& AppParam();

// main() of a game's executable: the eboot and the PRXs it starts with
// from its app0 (orbis/app.h, as the lifter's lift script took them),
// then RunProcess. Usage:
//   <program> --game-dir APP0 [--param-sfo FILE] [-- GAME ARGS...]
// APP0 holds eboot.bin and sce_module/; param.sfo defaults to
// APP0/sce_sys/param.sfo (absent: none). The game's main() gets argv
// "/app0/eboot.bin" and GAME ARGS. <ID>_TRACE=1 logs every HLE call.
int Main(const GameConfig& game, int argc, char** argv);

// Runs the process to the end of its entry point; returns the exit code.
int RunProcess(const GameConfig& game, const std::string& game_dir, const Config& config);

}  // namespace recomp::orbis
