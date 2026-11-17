#include "runtime/gfx/shader_compile.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>

#if defined(__APPLE__)
#include <spirv_cross/spirv_msl.hpp>
#endif

namespace recomp::gfx {

namespace {

std::mutex g_cache_mu;
int g_cache_fd = -1;  // the cache directory, opened; -1: none

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
  int fd = -1;
  if (!dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    fd = ec ? -1 : ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  }
  // Only a directory of this user's that no one else may write: the code
  // in it runs on the GPU as the game's. The checks and all the files go
  // through the directory opened once, so renaming a directory on its
  // path later cannot put another in its place.
  struct stat st;
  if (fd >= 0 && (::fstat(fd, &st) != 0 || st.st_uid != ::getuid() ||
                  (st.st_mode & (S_IWGRP | S_IWOTH)))) {
    ::close(fd);
    fd = -1;
  }
  std::lock_guard<std::mutex> l(g_cache_mu);
  if (g_cache_fd >= 0) ::close(g_cache_fd);
  g_cache_fd = fd;
}

bool CompileGlsl(const std::string& glsl, ShaderStage stage, std::string* code, std::string* log) {
  // A duplicate of the cache directory's descriptor, closed on return.
  struct Dir {
    int fd = -1;
    ~Dir() {
      if (fd >= 0) ::close(fd);
    }
  } d;
  {
    std::lock_guard<std::mutex> l(g_cache_mu);
    if (g_cache_fd >= 0) d.fd = ::fcntl(g_cache_fd, F_DUPFD_CLOEXEC, 0);
  }
  const int dir = d.fd;
  const std::string name = dir >= 0 ? CacheName(glsl, stage) : std::string();
  if (dir >= 0) {
    const int fd = ::openat(dir, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
      code->clear();
      char b[65536];
      ssize_t n;
      while ((n = ::read(fd, b, sizeof b)) > 0) code->append(b, size_t(n));
      ::close(fd);
      if (n == 0 && !code->empty()) return true;
    }
  }
  std::vector<uint32_t> spirv;
  if (!GlslToSpirv(glsl, stage, &spirv, log)) return false;
#if defined(__APPLE__)
  if (!SpirvToMsl(spirv, code, log)) return false;
#else
  code->assign(reinterpret_cast<const char*>(spirv.data()), spirv.size() * 4);
#endif
  if (dir >= 0) {
    // Written aside (a new file, never through a link) and renamed: a
    // reader sees all of a file or none.
    static std::atomic<unsigned> n{0};
    const std::string tmp = name + "." + std::to_string(getpid()) + "-" + std::to_string(n++);
    const int fd = ::openat(dir, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd >= 0) {
      const bool ok = ::write(fd, code->data(), code->size()) == ssize_t(code->size());
      if (::close(fd) != 0 || !ok || ::renameat(dir, tmp.c_str(), dir, name.c_str()) != 0)
        ::unlinkat(dir, tmp.c_str(), 0);
    }
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
