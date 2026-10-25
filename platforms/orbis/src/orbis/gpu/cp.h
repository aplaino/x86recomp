#pragma once
// The GPU's command processor: runs the PM4 command buffers the game
// submits (libSceGnmDriver) on host threads, one per queue, as the GPU
// would beside the CPU. Packets that touch memory the CPU sees (WRITE_DATA,
// the end-of-pipe and end-of-shader writes, DMA_DATA, the constant
// engine's dumps) and the waits (WAIT_REG_MEM, the CE/DE counters) are
// exact; register writes are recorded. Drawing and dispatching are not
// implemented yet: those packets are skipped (docs/hle.md).

#include <cstdint>
#include <functional>
#include <vector>

#include "runtime/guest_memory.h"

namespace recomp::orbis::gpu {

struct CmdBuffer {
  gaddr addr;
  uint32_t dwords;
};

// Queues a graphics submission: dcbs[i] runs on the drawing engine beside
// ccbs[i] (dwords 0: none) on the constant engine; done runs on the GPU
// thread once the last has finished.
void SubmitGraphics(std::vector<CmdBuffer> dcbs, std::vector<CmdBuffer> ccbs,
                    std::function<void()> done);

// A compute queue's ring (sceGnmMapComputeQueue): the GPU runs what lies
// between its read pointer and the write pointer Kick moves it to, and
// keeps the read pointer (in dwords) at rptr_addr.
void MapComputeRing(uint32_t vqid, gaddr base, uint32_t size_dw, gaddr rptr_addr);
void UnmapComputeRing(uint32_t vqid);
void KickComputeRing(uint32_t vqid, uint32_t wptr_dw);

// The graphics end-of-pipe interrupt's event (sceGnmAddEqEvent id 0x40).
inline constexpr uint64_t kEqEventGfxEop = 0x40;
inline constexpr int16_t kFilterGraphicsCore = -14;

}  // namespace recomp::orbis::gpu
