#pragma once
// libSceVideoOut's HLE (recomp_orbis_videoout): what libSceGnmDriver's
// uses of it need.

#include <cstdint>

#include "runtime/guest_memory.h"

namespace recomp::orbis::videoout {

// The port's buffer labels (sceVideoOutGetBufferLabelAddress): 16 words,
// one per buffer index, nonzero while a flip to that buffer is queued;
// the GPU waits on them (sceGnmInsertWaitFlipDone). 0 if no such port.
gaddr LabelAddress(int handle);

// Queues a flip as sceVideoOutSubmitFlip does, from the GPU's end of pipe
// (sceGnmSubmitAndFlipCommandBuffers); returns its result.
// `after`: the number of graphics submissions the GPU finishes first.
int32_t SubmitFlip(int handle, int index, uint32_t mode, int64_t arg, uint64_t after = 0);

}  // namespace recomp::orbis::videoout
