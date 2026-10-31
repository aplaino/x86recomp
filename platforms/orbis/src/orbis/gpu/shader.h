#pragma once
// GCN shaders to gfx::Device programs (GLSL). A draw or dispatch first
// resolves each stage: its scalar instructions run on the CPU with the
// stage's user data and guest memory (they are the same in every lane),
// which gives the descriptors its memory instructions use: the guest
// ranges it reads (and a compute stage writes) and its textures and
// samplers. Then the stage is translated, or taken from the cache: the
// GLSL runs the stage's instructions for one lane per invocation, its
// registers as variables, its branches as a switch over basic blocks, and
// its memory instructions on the ranges the draw binds, by guest address,
// so one translation serves every draw with the same code and formats.
//
// Lanes: each invocation is lane 0 of its own wave. Masks (EXEC, VCC and
// the SGPR pairs that hold them) keep that lane's bit in bit 0, so a
// comparison sets or clears bit 0 and a branch on an empty EXEC branches
// when this lane is inactive. What needs the other lanes (counts of
// active lanes, cross-lane moves) is not translated yet.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/guest_memory.h"

namespace recomp::orbis::gpu {

enum class Stage : uint8_t { Vertex, Pixel, Compute };

// How a stage starts: its code, its SPI_SHADER_PGM_RSRC1/2 and user data
// registers and, per stage, what the hardware gives it beside them.
struct StageState {
  Stage stage = Stage::Vertex;
  gaddr code = 0;
  uint32_t rsrc1 = 0, rsrc2 = 0;
  uint32_t user[16] = {};
  // Pixel: SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR, SPI_PS_INPUT_CNTL_0-31,
  // SPI_SHADER_COL_FORMAT and SPI_SHADER_Z_FORMAT.
  uint32_t ps_input_ena = 0, ps_input_addr = 0, ps_input_cntl[32] = {};
  uint32_t col_format = 0, z_format = 0;
  // Compute: COMPUTE_NUM_THREAD_X/Y/Z.
  uint32_t threads[3] = {1, 1, 1};
};

// A guest range a stage reads or writes, and a texture with its sampler.
struct BufferRange {
  gaddr base = 0;
  uint32_t size = 0;
  bool written = false;
};
struct ImageRef {
  uint32_t t[8] = {};  // T#
  uint32_t s[4] = {};  // S#; all zero without a sampler (image loads)
  bool storage = false;  // written (compute image stores)
};

// What a stage's resolution found: its ranges and images in the order its
// translation binds them, the code it runs (the stage's own and the fetch
// shader it calls) and the static facts its translation depends on.
struct Resolved {
  std::vector<BufferRange> buffers;
  std::vector<ImageRef> images;
  gaddr fetch = 0;          // the fetch shader the stage calls (0: none)
  std::string key;          // code and facts: equal keys, equal translation
  std::string error;        // why the stage cannot run (empty: it can)
};

// A translated stage: its GLSL, what it declares (gfx::ProgramStage) and
// how its uniform block is laid out (Uniforms below).
struct Translated {
  gfx::ProgramStage stage;
  std::string error;  // why it could not be translated (empty: it was)
  // Pixel stages: which colour targets (bit per MRT) the shader exports.
  uint32_t mrt_mask = 0;
  bool writes_depth = false;
  // Compute stages: their LDS size in dwords.
  uint32_t lds_dwords = 0;
};

Resolved Resolve(const StageState& st);
Translated Translate(const StageState& st, const Resolved& r);

// The stage's uniform block for a draw: its user data, the guest base and
// size of each bound range, and the code's address (for s_getpc).
std::vector<uint32_t> Uniforms(const StageState& st, const Resolved& r);

// A T#'s format (IMG_DATA_FORMAT, IMG_NUM_FORMAT, BGRA by its destination
// selects) as a device format; false for one not drawn yet.
bool TextureFormat(const uint32_t t[8], gfx::Format* out);
// The GLSL format qualifier of a storage image of format f; null for one
// not written yet.
const char* StorageQualifier(gfx::Format f);

}  // namespace recomp::orbis::gpu
