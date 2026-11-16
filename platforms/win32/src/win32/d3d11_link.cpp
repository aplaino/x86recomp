// The shader translator's side of Direct3D 11's program interface
// (d3d11_impl.h, "shader programs"): a gfx program, and its binding plan,
// for a vertex shader, pixel shader and input layout, or for a compute
// shader. Until DXBC is translated no shader has one: the layer skips
// those draws and dispatches and counts them.

#include "win32/d3d11_impl.h"

namespace recomp::hle::d3d11 {

std::shared_ptr<const LinkedProgram> LinkProgram(gfx::Device&, const Shader&, const Shader*,
                                                 const InputLayout*) {
  return nullptr;
}

std::shared_ptr<const LinkedProgram> LinkComputeProgram(gfx::Device&, const Shader&) {
  return nullptr;
}

}  // namespace recomp::hle::d3d11
