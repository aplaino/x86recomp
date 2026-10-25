#pragma once
// Fatal errors: print the reason, the guest registers and stop.

#include <cstdint>

#include "ppc/ctx.h"

namespace recomp::ppc {

[[noreturn, gnu::format(printf, 2, 3)]] void Fatal(const Ctx* c, const char* fmt, ...);

}  // namespace recomp::ppc
