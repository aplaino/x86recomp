// libSceLibcInternal HLE: the firmware's own C library, which system
// libraries that ship with games (and the games' PRXs) import; a game's
// libc is its own PRX, lifted with it.

#include "orbis/hle.h"

// The library's version marker (a symbol that modules link against).
ORBIS_DATA("libSceLibcInternal", "Need_sceLibcInternal", need_libc_internal, 8, nullptr);
// stdout as a FILE: an opaque block that the stdio handlers recognise.
ORBIS_DATA("libSceLibcInternal", "_Stdout", stdout_file, 0x100, nullptr);
