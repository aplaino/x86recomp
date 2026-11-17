// CompileGlsl's cache (runtime/gfx/shader_compile.h): code made once is
// kept by its source and read back the same, another source or stage is
// another file, the directory set is the one used even when its path
// later names another, and a directory others may write is not used.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "runtime/gfx/shader_compile.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

size_t Files(const std::filesystem::path& dir) {
  size_t n = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) n += e.is_regular_file();
  return n;
}

const char* kFrag =
    "#version 450\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = vec4(0.25, 0.5, 0.75, 1.0); }\n";
const char* kFrag2 =
    "#version 450\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = vec4(1.0); }\n";
const char* kFrag3 =
    "#version 450\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = vec4(0.0); }\n";

}  // namespace

int main() {
  using recomp::gfx::CompileGlsl;
  using recomp::gfx::SetShaderCacheDir;
  using recomp::gfx::ShaderStage;
  char tmpl[] = "/tmp/shader_cache_test.XXXXXX";
  if (!mkdtemp(tmpl)) return 1;
  const std::filesystem::path root = tmpl;
  const std::filesystem::path dir = root / "cache";
  std::string code, again, log;

  SetShaderCacheDir(dir.string());
  Check(CompileGlsl(kFrag, ShaderStage::Fragment, &code, &log) && !code.empty(), "compiles");
  Check(Files(dir) == 1, "the code is kept");
  Check(CompileGlsl(kFrag, ShaderStage::Fragment, &again, &log) && again == code,
        "the kept code is read back the same");
  Check(Files(dir) == 1, "the same source is one file");
  Check(CompileGlsl(kFrag2, ShaderStage::Fragment, &again, &log) && again != code,
        "another source is compiled");
  Check(Files(dir) == 2, "another source is another file");

  // A kept file is what is used: a change to it is read back.
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    std::FILE* f = std::fopen(e.path().c_str(), "ab");
    std::fputs("\n// kept\n", f);
    std::fclose(f);
  }
  Check(CompileGlsl(kFrag, ShaderStage::Fragment, &again, &log) && again == code + "\n// kept\n",
        "the kept file is used");

  // The directory set stays the one used when its path names another.
  const std::filesystem::path moved = root / "moved";
  std::filesystem::rename(dir, moved);
  std::filesystem::create_directories(dir);
  Check(CompileGlsl(kFrag3, ShaderStage::Fragment, &again, &log), "compiles after a rename");
  Check(Files(moved) == 3 && Files(dir) == 0, "the directory set is the one used");

  // A directory others may write is not used.
  const std::filesystem::path shared = root / "shared";
  std::filesystem::create_directories(shared);
  ::chmod(shared.c_str(), 0777);
  SetShaderCacheDir(shared.string());
  Check(CompileGlsl(kFrag, ShaderStage::Fragment, &again, &log) && again == code,
        "compiles without a usable cache");
  Check(Files(shared) == 0, "a directory others may write keeps nothing");

  SetShaderCacheDir("");
  std::filesystem::remove_all(root);
  if (g_failures) return 1;
  std::printf("shader cache: ok\n");
  return 0;
}
