#pragma once
// Starting a lifted program: what the game project's main() fills in and
// hands to RunProcess.

#include <string>

namespace recomp::horizon {

struct GameConfig {
  // Prefix of dev/test switches (<ID>_NAME) and name of the per-user data
  // directory.
  std::string id;
  // A homebrew NRO to run (host path): the module that was lifted.
  std::string nro;
};

// Loads the program and runs its main thread; returns its exit status.
int RunProcess(const GameConfig& cfg);

}  // namespace recomp::horizon
