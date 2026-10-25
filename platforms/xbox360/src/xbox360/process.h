#pragma once
// Starting a lifted title: what the game project's main() fills in and
// hands to RunProcess.

#include <string>

namespace recomp::xbox360 {

struct GameConfig {
  // Prefix of dev/test switches (<ID>_NAME) and name of the per-user data
  // directory.
  std::string id;
  // The title's executable (host path): the unencrypted XEX that was
  // lifted (xbox360_xex_decrypt makes one from the disc's).
  std::string xex;
};

// Loads the title into the guest address space, binds its imports to the
// HLE modules and runs its entry point on the main thread. Ends the host
// process when the title ends (its last thread, or HalReturnToFirmware),
// with the title's exit status; fatal if the title cannot be loaded.
[[noreturn]] void RunProcess(const GameConfig& cfg);

}  // namespace recomp::xbox360
