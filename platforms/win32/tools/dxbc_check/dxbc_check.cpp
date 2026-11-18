// dxbc_check: runs the Direct3D 11 shader translation (win32/dxbc.h) over
// every DXBC container found in the files given (each "DXBC" magic with
// its total-size field: loose shaders, or archives that store them
// uncompressed), compiles each translation as the device would (its
// prelude, GlslToSpirv, and on macOS SpirvToMsl), and prints a summary:
// counts by stage and the failures with their first error.
//
//   dxbc_check [-v] [-D NAME=VALUE] [--dump DIR] [--msl DIR] [--show N] [--max-failures N] FILE...
//
// -v lists every shader; --dump writes each shader's listing and GLSL
// (DIR/<n>.txt) for reading them side by side; --msl writes each one's
// MSL (DIR/<n>.metal, macOS) for a Metal compiler; --show N prints shader
// n's listing and GLSL. Link-time code is what a layer would put in front of
// the GLSL: location defines from the shader's own signature (its
// varyings numbered in order), an r_fetch that reads a buffer, and
// R_FMT_T# / R_FMT_U# as R32G32B32A32_FLOAT; -D NAME=VALUE defines a
// macro of the link-time code (R_FMT_T3=28u) instead.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/gfx/shader_compile.h"
#include "win32/dxbc.h"

namespace dxbc = recomp::hle::d3d11::dxbc;
namespace gfx = recomp::gfx;

namespace {

struct Blob {
  std::string file;
  size_t at = 0;
  std::vector<uint8_t> bytes;
};

struct Result {
  dxbc::ShaderType type = dxbc::ShaderType::Pixel;
  bool parsed = false, translated = false, spirv = false, msl = false;
  std::string error;  // the first failure
  std::string where;  // the GLSL line or instruction it names
  std::string glsl;   // as compiled
  std::string msl_text;
};

// The device's prelude for a program stage (gpu_device.cpp's
// CompileProgramStage), on SDL_GPU's Vulkan sets. On macOS, where the
// MSL is for Metal's flat index spaces (one for buffers, one for
// textures and images), the buffers, uniform blocks and images take
// indices of their own so that a Metal compiler accepts it.
#if defined(__APPLE__)
constexpr uint32_t kMem = 0, kHeap = 1, kRw = 2, kBufs = 3, kUniforms = 4, kImages = 16;
#endif

std::string Prelude(const gfx::ProgramStage& st, gfx::ShaderStage stage, const uint32_t* local_size) {
  const int res = stage == gfx::ShaderStage::Fragment ? 2 : 0;
  const int uni = stage == gfx::ShaderStage::Vertex ? 1 : stage == gfx::ShaderStage::Fragment ? 3 : 2;
#if defined(__APPLE__)
  const uint32_t mem = kMem, heap = kHeap, rw = kRw, bufs = kBufs, uniforms = kUniforms;
  const char* image = "#define R_IMAGE(n) set = 1, binding = (16 + (n))\n";
  static_assert(kImages == 16);
#else
  const uint32_t mem = st.textures, heap = st.textures + 1, rw = st.rw_images, bufs = 0,
                 uniforms = st.buffers || st.rw_buffers ? 1 : 0;
  const char* image = "#define R_IMAGE(n) set = 1, binding = (n)\n";
#endif
  char b[256];
  std::string pre = "#version 450\n";
  std::snprintf(b, sizeof b, "#define R_TEXTURE(n) set = %d, binding = (n)\n", res);
  pre += b;
  std::snprintf(b, sizeof b, "#define R_SAMPLER(n) set = %d, binding = (n)\n", res);
  pre += b;
  if (st.buffers) {
    std::snprintf(b, sizeof b,
                  "layout(std430, set = %d, binding = %u) readonly buffer RMem { uint r_mem[]; };\n"
                  "layout(std430, set = %d, binding = %u) readonly buffer RHeap { uint r_heap[]; };\n",
                  res, mem, res, heap);
    pre += b;
  }
  if (stage == gfx::ShaderStage::Compute) {
    pre += image;
    if (st.rw_buffers) {
      std::snprintf(b, sizeof b, "layout(std430, set = 1, binding = %u) buffer RRw { uint r_rw[]; };\n", rw);
      pre += b;
    }
    std::snprintf(b, sizeof b, "layout(local_size_x = %u, local_size_y = %u, local_size_z = %u) in;\n",
                  local_size[0], local_size[1], local_size[2]);
    pre += b;
  }
  if (st.buffers || st.rw_buffers) {
    std::snprintf(b, sizeof b, "layout(std140, set = %d, binding = %u) uniform RBufs {", uni, bufs);
    pre += b;
    if (st.buffers) {
      std::snprintf(b, sizeof b, " uvec4 r_buf[%u];", st.buffers);
      pre += b;
    }
    if (st.rw_buffers) {
      std::snprintf(b, sizeof b, " uvec4 r_rwbuf[%u];", st.rw_buffers);
      pre += b;
    }
    pre += " };\n";
  }
  if (st.buffers)
    pre += "uint r_ld(uint b, uint i) {\n  uint w = r_buf[b].x + i;\n"
           "  return r_buf[b].z != 0u ? r_heap[w] : r_mem[w];\n}\n";
  std::snprintf(b, sizeof b, "#define R_UNIFORMS set = %d, binding = %u\n", uni, uniforms);
  pre += b;
  return pre;
}

// What a layer links in front of the translation (see the file comment).
std::map<std::string, std::string> g_defines;  // -D

std::string Define(const std::string& name, const std::string& value) {
  auto it = g_defines.find(name);
  return "#define " + name + " " + (it != g_defines.end() ? it->second : value) + "\n";
}

std::string LinkCode(const dxbc::Shader& sh, gfx::ProgramStage* st) {
  std::string g;
  // A domain shader is drawn as a vertex stage (dxbc.h, point patches).
  const bool vertex = sh.type == dxbc::ShaderType::Vertex || sh.type == dxbc::ShaderType::Domain;
  if (vertex) g += dxbc::VaryingDefines(&sh, nullptr);
  if (sh.type == dxbc::ShaderType::Pixel) g += dxbc::VaryingDefines(nullptr, &sh);
  for (const dxbc::ShaderResource& r : sh.resources)
    if (r.dim == dxbc::ResourceDim::Buffer) g += Define("R_FMT_T" + std::to_string(r.slot), "2u");
  for (const dxbc::UnorderedAccess& u : sh.uavs)
    if (u.dim == dxbc::ResourceDim::Buffer) g += Define("R_FMT_U" + std::to_string(u.slot), "2u");
  if (vertex) {
    // One vertex buffer after the translation's, 16 bytes a register.
    const uint32_t vb = st->buffers++;
    g += "uvec4 r_fetch(uint reg) {\n  uint i = uint(gl_VertexIndex) * 16u + reg * 4u;\n"
         "  return uvec4(r_ld(" + std::to_string(vb) + "u, i), r_ld(" + std::to_string(vb) +
         "u, i + 1u), r_ld(" + std::to_string(vb) + "u, i + 2u), r_ld(" + std::to_string(vb) + "u, i + 3u));\n}\n";
  }
  return g;
}

std::string Line(const std::string& text, int n) {
  size_t at = 0;
  for (int i = 1; i < n && at != std::string::npos; i++) {
    at = text.find('\n', at);
    if (at != std::string::npos) at++;
  }
  if (at == std::string::npos) return {};
  return text.substr(at, text.find('\n', at) - at);
}

void Run(const Blob& b, Result* r, bool annotate) {
  dxbc::Shader sh;
  r->translated = dxbc::Translate(b.bytes.data(), b.bytes.size(), &sh, annotate);
  r->type = sh.type;
  r->parsed = true;
  if (!r->translated) {
    r->error = sh.error;
    return;
  }
  gfx::ProgramStage st = sh.stage;
  const gfx::ShaderStage stage = sh.type == dxbc::ShaderType::Vertex || sh.type == dxbc::ShaderType::Domain
                                     ? gfx::ShaderStage::Vertex
                                 : sh.type == dxbc::ShaderType::Pixel ? gfx::ShaderStage::Fragment
                                                                      : gfx::ShaderStage::Compute;
  const std::string link = LinkCode(sh, &st);
  r->glsl = Prelude(st, stage, sh.local_size) + link + sh.stage.glsl;
  std::vector<uint32_t> spirv;
  std::string log;
  if (!gfx::GlslToSpirv(r->glsl, stage, &spirv, &log)) {
    r->error = log.substr(0, log.find('\n'));
    // "ERROR: 0:LINE: ..."
    int line = 0;
    if (std::sscanf(log.c_str(), "ERROR: %*d:%d", &line) == 1) r->where = Line(r->glsl, line);
    return;
  }
  r->spirv = true;
#if defined(__APPLE__)
  std::string& msl = r->msl_text;
  if (!gfx::SpirvToMsl(spirv, &msl, &log)) {
    r->error = "SPIR-V to MSL: " + log;
    return;
  }
#endif
  r->msl = true;
}

const char* TypeName(dxbc::ShaderType t) {
  switch (t) {
    case dxbc::ShaderType::Vertex: return "vs";
    case dxbc::ShaderType::Pixel: return "ps";
    case dxbc::ShaderType::Compute: return "cs";
    case dxbc::ShaderType::Geometry: return "gs";
    case dxbc::ShaderType::Hull: return "hs";
    case dxbc::ShaderType::Domain: return "ds";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  bool verbose = false;
  std::string dump, msl_dir;
  long show = -1;
  size_t max_failures = 20;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "-v") verbose = true;
    else if (a == "-D" && i + 1 < argc) {
      const std::string d = argv[++i];
      const size_t eq = d.find('=');
      g_defines[d.substr(0, eq)] = eq == std::string::npos ? "" : d.substr(eq + 1);
    }
    else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
    else if (a == "--msl" && i + 1 < argc) msl_dir = argv[++i];
    else if (a == "--show" && i + 1 < argc) show = std::strtol(argv[++i], nullptr, 10);
    else if (a == "--max-failures" && i + 1 < argc) max_failures = std::strtoul(argv[++i], nullptr, 10);
    else files.push_back(a);
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: dxbc_check [-v] [-D NAME=VALUE] [--dump DIR] [--msl DIR] [--show N] [--max-failures N] FILE...\n");
    return 2;
  }

  std::vector<Blob> blobs;
  for (const std::string& f : files) {
    std::ifstream in(f, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "dxbc_check: cannot read %s\n", f.c_str());
      return 2;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (size_t at = 0; at + 32 <= data.size();) {
      const uint8_t* p = static_cast<const uint8_t*>(std::memchr(data.data() + at, 'D', data.size() - at));
      if (!p) break;
      at = size_t(p - data.data());
      if (at + 32 > data.size() || std::memcmp(p, "DXBC", 4) != 0) {
        at++;
        continue;
      }
      uint32_t total;
      std::memcpy(&total, p + 24, 4);
      if (total < 32 || total > data.size() - at) {
        at++;
        continue;
      }
      blobs.push_back({f, at, std::vector<uint8_t>(p, p + total)});
      at += total;
    }
  }

  if (show >= 0) {
    if (size_t(show) >= blobs.size()) {
      std::fprintf(stderr, "dxbc_check: no shader %ld (%zu found)\n", show, blobs.size());
      return 2;
    }
    const Blob& b = blobs[size_t(show)];
    std::string err;
    std::printf("%s @ 0x%zx\n%s\n", b.file.c_str(), b.at,
                dxbc::Disassemble(b.bytes.data(), b.bytes.size(), &err).c_str());
    if (!err.empty()) std::printf("listing: %s\n", err.c_str());
    Result r;
    Run(b, &r, true);
    std::printf("%s\n", r.glsl.c_str());
    if (!r.msl) std::printf("FAILED: %s\n  %s\n", r.error.c_str(), r.where.c_str());
    return r.msl ? 0 : 1;
  }

  std::vector<Result> results(blobs.size());
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  const unsigned n = std::max(1u, std::thread::hardware_concurrency());
  for (unsigned t = 0; t < n; t++)
    pool.emplace_back([&] {
      for (size_t i; (i = next++) < blobs.size();) Run(blobs[i], &results[i], !dump.empty());
    });
  for (std::thread& t : pool) t.join();

  struct Count {
    size_t total = 0, translated = 0, spirv = 0, msl = 0;
  };
  std::map<std::string, Count> counts;
  std::map<std::string, std::vector<size_t>> failures;  // by first error
  for (size_t i = 0; i < blobs.size(); i++) {
    const Result& r = results[i];
    Count& c = counts[TypeName(r.type)];
    c.total++;
    c.translated += r.translated;
    c.spirv += r.spirv;
    c.msl += r.msl;
    if (!r.msl) {
      // Group by the error without its instruction's offset and operands.
      std::string key = r.error;
      if (size_t p = key.find(" (at "); p != std::string::npos) key = key.substr(0, p);
      int skip = 0;
      if (std::sscanf(key.c_str(), "ERROR: %*d:%*d:%n", &skip) >= 0 && skip > 0) key = "ERROR:" + key.substr(size_t(skip));
      failures[key].push_back(i);
    }
    if (verbose)
      std::printf("%5zu %s %s@0x%zx %s%s\n", i, TypeName(r.type), blobs[i].file.c_str(), blobs[i].at,
                  r.msl ? "ok" : "FAILED: ", r.error.c_str());
    if (!msl_dir.empty() && r.msl) std::ofstream(msl_dir + "/" + std::to_string(i) + ".metal") << r.msl_text;
    if (!dump.empty()) {
      std::string err;
      std::ofstream out(dump + "/" + std::to_string(i) + ".txt");
      out << blobs[i].file << " @ 0x" << std::hex << blobs[i].at << std::dec << "\n"
          << dxbc::Disassemble(blobs[i].bytes.data(), blobs[i].bytes.size(), &err) << "\n"
          << (r.glsl.empty() ? "" : r.glsl) << "\n"
          << (r.msl ? "" : "FAILED: " + r.error + "\n  " + r.where + "\n");
    }
  }
  std::printf("%zu shaders\n", blobs.size());
  for (const auto& [t, c] : counts)
    std::printf("  %s: %zu, translated %zu, SPIR-V %zu, MSL %zu\n", t.c_str(), c.total, c.translated, c.spirv,
                c.msl);
  std::vector<std::pair<std::string, std::vector<size_t>>> sorted(failures.begin(), failures.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
  size_t shown = 0;
  for (const auto& [key, list] : sorted) {
    if (shown++ == max_failures) {
      std::printf("... %zu more kinds of failure\n", sorted.size() - max_failures);
      break;
    }
    const Result& r = results[list[0]];
    std::printf("\n%zu x %s\n  first: shader %zu (%s @ 0x%zx): %s\n", list.size(), key.c_str(), list[0],
                blobs[list[0]].file.c_str(), blobs[list[0]].at, r.error.c_str());
    if (!r.where.empty()) std::printf("  %s\n", r.where.c_str());
  }
  return failures.empty() ? 0 : 1;
}
