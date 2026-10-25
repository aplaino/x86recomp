#pragma once
// Fatal errors: print the reason, the guest registers and stop.

#include <cstdint>

#include "a64/ctx.h"

namespace recomp::a64 {

[[noreturn, gnu::format(printf, 2, 3)]] void Fatal(const Ctx* c, const char* fmt, ...);

}  // namespace recomp::a64
