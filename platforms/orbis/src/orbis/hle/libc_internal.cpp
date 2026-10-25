// libSceLibcInternal HLE: the firmware's own C library, which system
// libraries that ship with games (and the games' PRXs) import; a game's
// libc is its own PRX, lifted with it.

#include <cstring>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/guest_alloc.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

// The library's version marker (a symbol that modules link against).
ORBIS_DATA("libSceLibcInternal", "Need_sceLibcInternal", need_libc_internal, 8, nullptr);
// stdout as a FILE: an opaque block that the stdio handlers recognise.
ORBIS_DATA("libSceLibcInternal", "_Stdout", stdout_file, 0x100, nullptr);

namespace {

// The library's heap (malloc and free): its own, in guest memory.
recomp::GuestHeap& Heap() {
  static recomp::GuestHeap* h = new recomp::GuestHeap(recomp::g_proc->vm, 1u << 20, 0);
  return *h;
}

}  // namespace

using recomp::hle::Arg;

ORBIS_HLE("libSceLibcInternal", memcpy, 3) {
  std::memcpy(recomp::GPtr(Arg(c, 0)), recomp::GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
ORBIS_HLE("libSceLibcInternal", memset, 3) {
  std::memset(recomp::GPtr(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
ORBIS_HLE("libSceLibcInternal", malloc, 1) {
  const recomp::gaddr p = Heap().Alloc(Arg(c, 0) ? Arg(c, 0) : 1, false);
  if (!p) recomp::orbis::kernel::SetErrno(c, recomp::orbis::kernel::kENOMEM);
  return p;
}
ORBIS_HLE("libSceLibcInternal", free, 1) {
  if (Arg(c, 0) && !Heap().Free(Arg(c, 0)))
    recomp::FatalF("libSceLibcInternal free(%llX): not a block of its heap",
                   (unsigned long long)Arg(c, 0));
  return 0;
}
