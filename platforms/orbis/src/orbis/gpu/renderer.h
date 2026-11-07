#pragma once
// The GPU's drawing and compute on gfx::Device: a draw or dispatch the
// command processor (cp.h) reaches becomes a program draw or dispatch with
// the registers' state, its shaders translated (shader.h). Colour and depth
// targets are device images found by their guest address, so what one draw
// renders the next samples and the display shows; textures come from guest
// memory otherwise.

#include <cstdint>
#include <functional>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/guest_memory.h"

namespace recomp::orbis::gpu {

// The registers of the command processor's SET_*_REG spaces (dwords).
inline constexpr uint32_t kRegSpace = 0x400;
struct Regs {
  uint32_t config[kRegSpace] = {}, context[kRegSpace] = {}, sh[kRegSpace] = {},
           uconfig[kRegSpace] = {};
};

// A draw packet's operands: vertices (or indices) from base_index on,
// instances, and the index buffer (0: not indexed).
struct DrawCall {
  uint32_t count = 0, instances = 1;
  gaddr indices = 0;
  uint32_t index_size = 2;
};

// The device draws go to (VideoOut's, which owns the window); until then
// draws and dispatches are skipped.
void SetDevice(gfx::Device* dev);

void Draw(const Regs& r, const DrawCall& d);
void Dispatch(const Regs& r, const uint32_t groups[3]);
// Guest memory the command processor wrote (DMA_DATA): a depth target's
// HTILE cleared there clears it.
void MemoryWritten(gaddr base, uint64_t size);
// A new submission: textures read from memory are compared again.
void NextSubmission();

// Dispatches' results in guest memory land once the device has run them
// (gfx::ProgramDispatch::land_later), and the command processor's writes
// the CPU waits on (labels, end-of-pipe events) wait behind them, in
// order, as on the console. Reads of guest memory on the GPU's side land
// what they overlap first (and run the signals queued before).
void BeforeRead(gaddr a, uint64_t n);
// Runs fn, which writes n bytes at a (n 0: none), once every result queued
// so far has landed and the signals before it have run; at once if
// nothing is queued. `words`: what it writes there, if known.
void Signal(gaddr a, uint64_t n, std::function<void()> fn, std::vector<uint32_t> words = {});
// The dword the last queued signal writing at a writes, if one does and
// it is known: what the command processor's waits on memory compare
// (the device runs what follows after the work the signal waits for).
bool SignalValue(gaddr a, uint32_t* v);
// Lands the results the device has finished and runs the signals waiting
// for them; all of them, waiting, with wait.
void Settle(bool wait);

// The colour target the GPU rendered at a guest address with this size, or
// 0 (VideoOut shows it).
gfx::TextureId TargetAt(gaddr addr, uint32_t w, uint32_t h);
// Runs fn with no draw or dispatch in between (they set the device's
// targets): presentation.
void Exclusive(const std::function<void()>& fn);

// The pixels the draws DB_COUNT_CONTROL counts have written so far (the
// occlusion counter, all render backends together), once they have landed.
uint64_t OcclusionCount();
// Runs fn with that count once the draws queued so far are counted, as a
// signal writing n bytes at a (the occlusion counter dump).
void OcclusionCountThen(gaddr a, uint64_t n, std::function<void(uint64_t)> fn);

}  // namespace recomp::orbis::gpu
