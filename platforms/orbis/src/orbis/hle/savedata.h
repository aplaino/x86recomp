#pragma once
// libSceSaveData's HLE (recomp_orbis_savedata): the user's save data for
// the system's own use (its save data dialog).

#include <cstdint>
#include <string>

#include "runtime/guest_memory.h"

namespace recomp::orbis::savedata {

constexpr size_t kParamSize = 0x530;  // SceSaveDataParam

struct Entry {
  uint8_t param[kParamSize];  // as the game set it, mtime (0x508) the time below
  uint64_t blocks;            // its size, in 32 KiB blocks
  int64_t mtime;              // when a file of it last changed, Unix seconds
  std::string icon;           // its icon's PNG file; empty: none
};

// The game's save data directory `dir` of the user, if it exists.
bool Find(const std::string& dir, Entry* e);

// Whether a title id the game passes (a string; null: the game's own)
// names the game itself: its param.sfo's TITLE_ID.
bool OwnTitle(gaddr title);

}  // namespace recomp::orbis::savedata
