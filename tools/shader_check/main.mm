// recomp_shader_check: compiled Direct3D 9 shaders through the bytecode
// translator and the Metal backend's compile path, offline.
//
//   recomp_shader_check [--list FILE] [--dump DIR] [--all-pairs] DIR...
//
// Every *.vso / *.pso file under each DIR is a shader function (version
// token to END, as CreateVertexShader / CreatePixelShader take it). Each
// one is read as the runtime reads it (hlsl::ReadBytecode, which must end
// at the file's end), translated (hlsl::TranslateBytecode), completed with
// the Metal prelude (gfx/metal_prelude.h), compiled to SPIR-V, to MSL and
// by Metal, exactly as the backend does at run time.
//
// The vertex and pixel shaders of one directory are then paired as the
// backend pairs them: a pixel shader reading varyings the vertex shader
// does not write gets its variant (hlsl::GlslWithDefaultInputs), which
// must compile, and Metal must accept the pair as a render pipeline; with
// --all-pairs every pair is made a pipeline.
//
// --list writes "<file> <hash>" per shader, the hash being the backend's
// (RECOMP_SHADER_DUMP names), so two runs can be compared for changed
// translations; --dump writes each GLSL source there. Exit status 1 if
// anything fails.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "runtime/gfx/metal_prelude.h"
#include "runtime/gfx/shader_compile.h"
#include "runtime/hlsl/hlsl.h"

namespace {

namespace fs = std::filesystem;
using namespace recomp;

uint64_t Fnv64(const void* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001B3ull;
  return h;
}

struct Checker {
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  FILE* list = nullptr;
  std::string dump;
  bool all_pairs = false;
  int shaders = 0, failures = 0, pairs = 0, variants = 0, pipelines = 0;

  struct Compiled {
    std::string name;
    hlsl::Program program;
    id<MTLFunction> fn = nil;
    uint32_t varyings = 0;
    std::map<uint32_t, id<MTLFunction>> variants;  // by the inputs written
  };
  std::map<fs::path, std::vector<Compiled>> vs_, ps_;  // by directory

  void Fail(const std::string& name, const std::string& what) {
    std::printf("FAIL %s: %s\n", name.c_str(), what.c_str());
    failures++;
  }

  // GLSL (with the prelude) through SPIR-V and MSL to a Metal function.
  id<MTLFunction> Compile(const std::string& name, const std::string& glsl, hlsl::Stage stage) {
    std::vector<uint32_t> spirv;
    std::string log, msl;
    bool vertex = stage == hlsl::Stage::Vertex;
    if (!gfx::GlslToSpirv(glsl, vertex ? gfx::ShaderStage::Vertex : gfx::ShaderStage::Fragment, &spirv,
                          &log)) {
      Fail(name, "GLSL: " + log);
      return nil;
    }
    if (!gfx::SpirvToMsl(spirv, &msl, &log)) {
      Fail(name, "MSL: " + log);
      return nil;
    }
    // As the backend: its lean variant's switch made a macro, both
    // variants compiled; the full one drawn.
    id<MTLFunction> fn = nil;
    const bool lean = gfx::MetalLeanMacros(&msl, vertex);
    for (int full = lean ? 0 : 1; full < 2; full++) {
      MTLCompileOptions* o = nil;
      if (lean) {
        o = [MTLCompileOptions new];
        o.preprocessorMacros = @{@"R_ALPHA_TEST" : full ? @"true" : @"false"};
      }
      NSError* err = nil;
      id<MTLLibrary> lib = [dev newLibraryWithSource:@(msl.c_str()) options:o error:&err];
      if (!lib) {
        Fail(name, std::string(full ? "Metal: " : "Metal (lean): ") + err.localizedDescription.UTF8String);
        return nil;
      }
      fn = [lib newFunctionWithName:@"main0"];
    }
    return fn;
  }

  void Shader(const fs::path& path) {
    std::string name = path.string();
    shaders++;
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 8 || bytes.size() % 4) return Fail(name, "not a whole number of tokens");
    std::vector<uint32_t> file(bytes.size() / 4);
    std::memcpy(file.data(), bytes.data(), bytes.size());
    std::vector<uint32_t> tokens = hlsl::ReadBytecode([&](size_t i) { return file[i]; }, file.size());
    if (tokens.size() != file.size())
      return Fail(name, tokens.empty() ? "no END" : "END at token " + std::to_string(tokens.size() - 1));
    hlsl::Program p;
    std::string err;
    if (!hlsl::TranslateBytecode(tokens.data(), tokens.size(), &p, &err)) return Fail(name, err);
    std::string glsl = "#version 450\n" + gfx::MetalShaderPrelude(p.stage) + p.glsl;
    uint64_t hash = Fnv64(glsl.data(), glsl.size()) ^ uint64_t(p.stage);
    bool vertex = p.stage == hlsl::Stage::Vertex;
    if (list) std::fprintf(list, "%s %016llx\n", name.c_str(), (unsigned long long)hash);
    if (!dump.empty()) {
      char file[64];
      std::snprintf(file, sizeof file, "/%016llx.%s.glsl", (unsigned long long)hash, vertex ? "vs" : "ps");
      std::ofstream(dump + file, std::ios::binary) << glsl;
    }
    id<MTLFunction> fn = Compile(name, glsl, p.stage);
    if (!fn) return;
    uint32_t varyings = hlsl::VaryingMask(p);
    (vertex ? vs_ : ps_)[path.parent_path()].push_back({name, std::move(p), fn, varyings, {}});
  }

  void Pair(const Compiled& vs, Compiled& ps) {
    pairs++;
    uint32_t written = vs.varyings & ps.varyings;
    id<MTLFunction> fn = ps.fn;
    if (written != ps.varyings) {
      auto it = ps.variants.find(written);
      if (it == ps.variants.end()) {
        variants++;
        std::string glsl = "#version 450\n" + gfx::MetalShaderPrelude(hlsl::Stage::Pixel) +
                           hlsl::GlslWithDefaultInputs(ps.program, written);
        if (!dump.empty()) {
          char file[64];
          std::snprintf(file, sizeof file, "/%016llx.ps.glsl", (unsigned long long)Fnv64(glsl.data(), glsl.size()));
          std::ofstream(dump + file, std::ios::binary) << glsl;
        }
        it = ps.variants.emplace(written, Compile(ps.name + " (variant)", glsl, hlsl::Stage::Pixel)).first;
      }
      fn = it->second;
      if (!fn) return;
    } else if (!all_pairs) {
      return;
    }
    pipelines++;
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs.fn;
    pd.fragmentFunction = fn;
    for (int i = 0; i < 4; i++)
      if (ps.program.color_outputs & (1u << i)) pd.colorAttachments[NSUInteger(i)].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (ps.program.writes_depth) pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError* err = nil;
    if (![dev newRenderPipelineStateWithDescriptor:pd error:&err])
      Fail(vs.name + " + " + ps.name, std::string("pipeline: ") + err.localizedDescription.UTF8String);
  }

  void Pairs() {
    for (auto& [dir, pss] : ps_)
      for (Compiled& ps : pss)
        for (const Compiled& vs : vs_[dir]) {
          @autoreleasepool {
            Pair(vs, ps);
          }
        }
  }
};

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    Checker c;
    std::vector<fs::path> dirs;
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      if (a == "--list" && i + 1 < argc) {
        c.list = std::fopen(argv[++i], "w");
      } else if (a == "--dump" && i + 1 < argc) {
        c.dump = argv[++i];
      } else if (a == "--all-pairs") {
        c.all_pairs = true;
      } else if (a[0] == '-') {
        std::fprintf(stderr, "usage: recomp_shader_check [--list FILE] [--dump DIR] [--all-pairs] DIR...\n");
        return 2;
      } else {
        dirs.push_back(a);
      }
    }
    if (!c.dev) {
      std::fprintf(stderr, "no Metal device\n");
      return 2;
    }
    std::vector<fs::path> files;
    for (const fs::path& d : dirs)
      for (const auto& e : fs::recursive_directory_iterator(d))
        if (e.is_regular_file() && (e.path().extension() == ".vso" || e.path().extension() == ".pso"))
          files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
      @autoreleasepool {
        c.Shader(f);
      }
    }
    if (c.list) std::fclose(c.list);
    c.Pairs();
    std::printf("%d shaders, %d vertex/pixel pairs, %d pixel shader variants, %d pipelines, %d failures\n",
                c.shaders, c.pairs, c.variants, c.pipelines, c.failures);
    return c.failures ? 1 : 0;
  }
}
