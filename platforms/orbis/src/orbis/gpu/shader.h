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
// Lanes: in most stages each invocation is lane 0 of its own wave. Masks
// (EXEC, VCC and the SGPR pairs that hold them) keep that lane's bit in
// bit 0, so a comparison sets or clears bit 0 and a branch on an empty
// EXEC branches when this lane is inactive; ds_swizzle within quads reads
// the other lanes through the host's subgroup operations. A compute stage
// that reads other lanes (Resolved::wave) runs in waves of 64 lanes
// instead: each invocation runs lanes l and l + 32 of its wave, one host
// subgroup of 32 invocations a wave, its SGPRs and masks the wave's (the
// same in each invocation), its branches the wave's, comparisons and
// carries ballots and cross-lane reads subgroup shuffles. A host whose
// subgroups are not those 32 invocations in order is reported (the fault
// word).
// Scalars the stage loads at a run-time offset (an index from a
// v_readfirstlane) are loaded as it runs, from the range the stage reads
// at known offsets from the same pointer; descriptors among them are
// picked from those that range holds (Resolved::match). Export (ES) and
// Geometry (GS) stages run as compute passes of 64-invocation groups, one
// group per wave and the invocation's index in it as its lane, which
// swizzled buffer accesses (the rings between ES, GS and the copy shader)
// address by.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/guest_memory.h"

namespace recomp::orbis::gpu {

enum class Stage : uint8_t { Vertex, Pixel, Compute, Export, Geometry };

// Stages that run as compute programs: they can write their ranges.
inline bool RunsAsCompute(Stage s) {
  return s == Stage::Compute || s == Stage::Export || s == Stage::Geometry;
}

// How a stage starts: its code, its SPI_SHADER_PGM_RSRC1/2 and user data
// registers and, per stage, what the hardware gives it beside them.
struct StageState {
  Stage stage = Stage::Vertex;
  gaddr code = 0;
  uint32_t rsrc1 = 0, rsrc2 = 0;
  uint32_t user[16] = {};
  // Pixel: SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR, SPI_PS_INPUT_CNTL_0-31,
  // SPI_SHADER_COL_FORMAT and SPI_SHADER_Z_FORMAT; per target, 4 bits:
  // the COMP_SWAP of one of one or two components (bits 2-3: how many).
  uint32_t ps_input_ena = 0, ps_input_addr = 0, ps_input_cntl[32] = {};
  uint32_t col_format = 0, z_format = 0, col_swap = 0;
  // Pixel: the location of an output of 1 for each pixel the draw writes
  // (occlusion counts, Renderer), -1 for none.
  int count = -1;
  // Compute: COMPUTE_NUM_THREAD_X/Y/Z.
  uint32_t threads[3] = {1, 1, 1};
  // Export and Geometry passes: their invocations (a draw's vertices, its
  // primitives), each ES and GS wave's bytes in its ring, the vertices of
  // an input primitive, the pass's first primitive and the GS's maximum
  // output vertices. They reach the code as uniforms (not in the key).
  uint32_t invocations = 0, es_wave_bytes = 0, gs_wave_bytes = 0, prim_verts = 0, first_prim = 0,
           max_vert = 0;
  // Vertex: the draw is a rectangle list, drawn as two triangles a
  // rectangle (six vertices, corners 0 1 2 and 1 3 2): the fourth corner's
  // outputs are the second's and third's less the first's.
  bool rects = false;
};

// A guest range a stage reads or writes, and a texture with its sampler.
struct BufferRange {
  gaddr base = 0;
  uint32_t size = 0;
  bool written = false;
  uint32_t vsharp[4] = {};  // the V# of its first buffer access (none: 0)
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
  // Descriptors loaded at run-time offsets: the words of those each such
  // access can find (Uniforms puts them last), and whether the stage
  // reports a load outside its range or a descriptor not among them (one
  // more read-write range, after the others: a word, the instruction's
  // address once one did).
  std::vector<uint32_t> match;
  bool faults = false;
  bool wave = false;        // a compute stage run in waves (above)
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
  // Compute stages: their LDS size in dwords and the host's group size
  // (in waves: 32 invocations a wave).
  uint32_t lds_dwords = 0;
  uint32_t local_size[3] = {1, 1, 1};
};

Resolved Resolve(const StageState& st);
Translated Translate(const StageState& st, const Resolved& r);

// The stage's uniform block for a draw: its user data, the guest base and
// size of each bound range, and the code's address (for s_getpc). An
// Export stage binds one more read-only range after its own: the vertex
// index of each invocation (32 bits, at address 0); a Geometry stage one
// more read-write range: per invocation, max_vert words for the vertices
// it emits (bit 0 set: emitted; bit 1: a strip starts with it).
std::vector<uint32_t> Uniforms(const StageState& st, const Resolved& r);

// The shader component in each of a target's components (col_swap): a
// target of one or two components holds them in memory order.
void ExportOrder(uint32_t col_swap, uint32_t target, int from[4]);
// A T#'s format (IMG_DATA_FORMAT, IMG_NUM_FORMAT), its components in
// memory order, as a device format; false for one not drawn yet.
bool TextureFormat(const uint32_t t[8], gfx::Format* out);
// The GLSL format qualifier of a storage image of format f; null for one
// not written yet.
const char* StorageQualifier(gfx::Format f);

}  // namespace recomp::orbis::gpu
