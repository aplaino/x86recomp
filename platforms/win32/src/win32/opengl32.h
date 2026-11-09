#pragma once
// The opengl32 HLE's GL contexts, for native code that draws with the
// guest's current context directly instead of through its entry points
// (the same gl::Context, so the two can be mixed in any order).

#include "runtime/gl/context.h"

namespace recomp::hle {

// The calling thread's current context (wglMakeCurrent), or null (then
// reported once).
gl::Context* CurrentGl();

}  // namespace recomp::hle
