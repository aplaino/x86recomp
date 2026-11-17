#pragma once
// GLSL to the SDL_GPU backend's shader code at run time: SPIR-V (glslang)
// for Vulkan, and on macOS MSL source from that SPIR-V (SPIRV-Cross, with
// the binding numbers as Metal indices, as the build does for the
// fixed-function shaders).

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::gfx {

enum class ShaderStage { Vertex, Fragment, Compute };

// False with the compiler's messages in *log.
bool GlslToSpirv(const std::string& glsl, ShaderStage stage, std::vector<uint32_t>* spirv,
                 std::string* log);

#if defined(__APPLE__)
bool SpirvToMsl(const std::vector<uint32_t>& spirv, std::string* msl, std::string* log);
#endif

// The backend's code for `glsl`: MSL source on macOS (GlslToSpirv, then
// SpirvToMsl), else SPIR-V. Kept in the directory SetShaderCacheDir names,
// by a hash of the source, so a later run (and a draw met again) does not
// compile the same source again.
bool CompileGlsl(const std::string& glsl, ShaderStage stage, std::string* code, std::string* log);
// Where CompileGlsl keeps code (created if missing); empty: nowhere.
void SetShaderCacheDir(const std::string& dir);

}  // namespace recomp::gfx
