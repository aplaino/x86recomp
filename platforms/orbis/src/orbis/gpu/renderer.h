#pragma once
// The GPU's drawing and compute on gfx::Device: a draw or dispatch the
// command processor (cp.h) reaches becomes a program draw or dispatch with
// the registers' state, its shaders translated (shader.h). Colour and depth
// targets are device images found by their guest address, so what one draw
// renders the next samples and the display shows; textures come from guest
// memory otherwise.

#include <cstdint>
#include <functional>

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
// A new submission: textures read from memory are compared again.
void NextSubmission();

// The colour target the GPU rendered at a guest address with this size, or
// 0 (VideoOut shows it).
gfx::TextureId TargetAt(gaddr addr, uint32_t w, uint32_t h);
// Runs fn with no draw or dispatch in between (they set the device's
// targets): presentation.
void Exclusive(const std::function<void()>& fn);

}  // namespace recomp::orbis::gpu
