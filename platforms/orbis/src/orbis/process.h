#pragma once
// An Orbis (PS4) process: what the kernel and libkernel's dynamic linker
// do before a game's code runs, for lifted modules. Maps the eboot and the
// PRXs the game ships (each where the lifter put it), binds every import
// to another module's export or else to the HLE (orbis/hle.h), lays out
// static TLS, gives each thread a TCB (through fs), runs the PRXs'
// initialisers and calls the eboot's entry point.

#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace recomp::orbis {

struct Prx {
  std::string path;  // host path
  gaddr base;        // the lifter's --base for it
};

struct Config {
  std::string eboot;              // host path
  std::vector<Prx> prxs;          // in initialisation order
  std::vector<std::string> args;  // argv, argv[0] first
  uint64_t main_stack = 8u << 20;
};

// Runs the process to the end of its entry point; returns the exit code.
int RunProcess(const GameConfig& game, const std::string& game_dir, const Config& config);

}  // namespace recomp::orbis
