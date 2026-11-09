#pragma once
// Dev/test switches: environment variables named <ID>_<SWITCH>, where ID
// is the game's id (GameConfig::id) in upper case.

#include <string>

namespace recomp {

// Sets the id switches are read under; the platform calls it once, when
// the process starts and before anything reads a switch.
void SetEnvId(const std::string& id);
// Switch `name`: getenv("<ID>_<name>"), e.g. Env("TRACE") reads
// GAME_TRACE for id "game". Null if unset or before SetEnvId.
const char* Env(const char* name);

}  // namespace recomp
