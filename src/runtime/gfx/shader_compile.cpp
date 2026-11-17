#include "runtime/gfx/shader_compile.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>

#if defined(__APPLE__)
#include <spirv_cross/spirv_msl.hpp>
#endif

namespace recomp::gfx {

namespace {

std::mutex g_cache_mu;
std::string g_cache_dir;

// A cache file's name: two 64-bit hashes of the stage and the source.
// The tag names what the code is made with; change it when the compilers'
// options change.
std::string CacheName(const std::string& glsl, ShaderStage stage) {
  const std::string key = "glslang-spv1.3-msl2.1:" + std::to_string(int(stage)) + ":" + glsl;
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char ch : key) h = (h ^ ch) * 0x100000001B3ull;
  char b[40];
  std::snprintf(b, sizeof b, "%016llx%016llx", (unsigned long long)h,
                (unsigned long long)std::hash<std::string>{}(key));
  return b;
}

}  // namespace

void SetShaderCacheDir(const std::string& dir) {
  std::error_code ec;
  if (!dir.empty()) std::filesystem::create_directories(dir, ec);
  std::lock_guard<std::mutex> l(g_cache_mu);
  g_cache_dir = ec ? std::string() : dir;
}

bool CompileGlsl(const std::string& glsl, ShaderStage stage, std::string* code, std::string* log) {
  std::string path;
  {
    std::lock_guard<std::mutex> l(g_cache_mu);
    if (!g_cache_dir.empty()) path = g_cache_dir + "/" + CacheName(glsl, stage);
  }
  if (!path.empty()) {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      code->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      if (!code->empty()) return true;
    }
  }
  std::vector<uint32_t> spirv;
  if (!GlslToSpirv(glsl, stage, &spirv, log)) return false;
#if defined(__APPLE__)
  if (!SpirvToMsl(spirv, code, log)) return false;
#else
  code->assign(reinterpret_cast<const char*>(spirv.data()), spirv.size() * 4);
#endif
  if (!path.empty()) {
    // Written aside and renamed: a reader sees all of a file or none.
    static std::atomic<unsigned> n{0};
    const std::string tmp = path + "." + std::to_string(getpid()) + "-" + std::to_string(n++);
    std::ofstream f(tmp, std::ios::binary);
    f.write(code->data(), std::streamsize(code->size()));
    f.close();
    if (!f || std::rename(tmp.c_str(), path.c_str()) != 0) std::remove(tmp.c_str());
  }
  return true;
}

bool GlslToSpirv(const std::string& glsl, ShaderStage stage, std::vector<uint32_t>* spirv,
                 std::string* log) {
  static std::once_flag once;
  std::call_once(once, [] { glslang::InitializeProcess(); });
  const EShLanguage lang = stage == ShaderStage::Vertex   ? EShLangVertex
                           : stage == ShaderStage::Compute ? EShLangCompute
                                                           : EShLangFragment;
  glslang::TShader sh(lang);
  const char* src = glsl.c_str();
  sh.setStrings(&src, 1);
  sh.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
  // Vulkan 1.1 (the device asks for it): subgroup operations.
  sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
  sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
  const EShMessages msg = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!sh.parse(GetDefaultResources(), 450, false, msg)) {
    *log = sh.getInfoLog();
    return false;
  }
  glslang::TProgram prog;
  prog.addShader(&sh);
  if (!prog.link(msg)) {
    *log = prog.getInfoLog();
    return false;
  }
  spirv->clear();
  glslang::GlslangToSpv(*prog.getIntermediate(lang), *spirv);
  return true;
}

#if defined(__APPLE__)
bool SpirvToMsl(const std::vector<uint32_t>& spirv, std::string* msl, std::string* log) {
  try {
    spirv_cross::CompilerMSL c(spirv);
    spirv_cross::CompilerMSL::Options o = c.get_msl_options();
    o.set_msl_version(2, 1);
    o.enable_decoration_binding = true;
    o.enable_clip_distance_user_varying = false;
    c.set_msl_options(o);
    *msl = c.compile();
  } catch (const std::exception& e) {
    *log = e.what();
    return false;
  }
  return true;
}
#endif

}  // namespace recomp::gfx
