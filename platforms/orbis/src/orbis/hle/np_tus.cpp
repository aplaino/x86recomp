// libSceNpTus HLE: title user storage on the PSN servers. The library
// initializes itself when loaded (11.00's module_start), so its calls
// never see it uninitialized here. No title context is ever created: the
// calls that create one are not implemented (fatal), and the user is not
// signed in (docs/hle.md).

#include "orbis/hle.h"
#include "x86/runtime.h"

namespace {

constexpr uint32_t kErrNoSuchContext = 0x8055070E;

}  // namespace

using recomp::hle::Arg;

// int sceNpTusDeleteNpTitleCtx(int ctxId): an id no context has.
ORBIS_HLE("libSceNpTus", sceNpTusDeleteNpTitleCtx, 1) {
  (void)c;
  return kErrNoSuchContext;
}
