// xboxkrnl: the display driver (Vd*) and the AV pack.
//
// What the Vd* exports do comes from Xenia (most of it from titles' use,
// not the kernel's code, which is not available), except where a title's
// code shows it; docs/xenos.md lists both and what awaits the console.

#include <bit>
#include <cstring>

#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/gpu.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/memory.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";

// The Xenos runs at 500 MHz.
void InitGpuClock(uint32_t va) { Wr32(va, 500); }
// A critical section, with Xenia's spin count.
void InitHsioCalibrationLock(uint32_t va) { InitCriticalSection(va, 10000); }

// VdGetSystemCommandBuffer's buffer: a title that skips the swap zeroes it
// itself, and reads its second word.
constexpr uint32_t kSystemCommandBufferSize = 0x94;

}  // namespace

// The title's D3D device: titles store it here and read it back.
HLE_VARIABLE(kKrnl, 446, VdGlobalDevice, 4, nullptr);
HLE_VARIABLE(kKrnl, 448, VdGpuClockInMHz, 4, InitGpuClock);
HLE_VARIABLE(kKrnl, 449, VdHSIOCalibrationLock, kCriticalSectionSize, InitHsioCalibrationLock);

HLE(kKrnl, 34, HalGetCurrentAVPack) { return AvPack(); }

// VdQueryVideoMode(*mode).
HLE(kKrnl, 458, VdQueryVideoMode) {
  WriteVideoMode(Arg(c, 0));
  return 0;
}

// VdQueryVideoFlags: widescreen (1), at least 1280 wide (2), at least 1920
// (4).
HLE(kKrnl, 457, VdQueryVideoFlags) {
  VideoMode m = CurrentVideoMode();
  return uint32_t(m.widescreen) | (m.width >= 1280 ? 2 : 0) | (m.width >= 1920 ? 4 : 0);
}

// VdGetCurrentDisplayInformation(*info): X_DISPLAY_INFO (0x58 bytes), the
// front buffer and display at the video mode's size, unscaled, with
// Xenia's overscan.
HLE(kKrnl, 442, VdGetCurrentDisplayInformation) {
  uint32_t info = Arg(c, 0);
  VideoMode m = CurrentVideoMode();
  std::memset(Mem() + info, 0, 0x58);
  Wr16(info + 0x0, uint16_t(m.width));
  Wr16(info + 0x2, uint16_t(m.height));
  Wr32(info + 0x8 + 0x8, m.width);  // scaler source rectangle: x2, y2
  Wr32(info + 0x8 + 0xC, m.height);
  Wr32(info + 0x8 + 0x10, m.width);  // scaled output
  Wr32(info + 0x8 + 0x14, m.height);
  Wr32(info + 0x8 + 0x18, 1);  // vertical, horizontal filter types
  Wr32(info + 0x8 + 0x28, 1);
  Wr16(info + 0x40, 320);  // overscan: left, top, right, bottom
  Wr16(info + 0x42, 180);
  Wr16(info + 0x44, 320);
  Wr16(info + 0x46, 180);
  Wr16(info + 0x48, uint16_t(m.width));
  Wr16(info + 0x4A, uint16_t(m.height));
  Wr32(info + 0x4C, std::bit_cast<uint32_t>(float(m.refresh_hz)));
  Wr32(info + 0x50, m.interlaced);
  Wr16(info + 0x56, uint16_t(m.width));
  return 0;
}

// VdGetCurrentDisplayGamma(*type, *power): 2, a TV's (BT.709), as Xenia
// defaults it, with its power.
HLE(kKrnl, 441, VdGetCurrentDisplayGamma) {
  Wr32(Arg(c, 0), 2);
  Wr32(Arg(c, 1), std::bit_cast<uint32_t>(2.22222233f));
  return 0;
}

// VdSetDisplayMode(flags): the display's mode is fixed here.
HLE(kKrnl, 467, VdSetDisplayMode) { return 0; }

// VdInitializeEngines(?, callback, argument, PFP microcode, ME microcode):
// the command processor here runs no microcode. TRUE.
HLE(kKrnl, 450, VdInitializeEngines) { return 1; }
// VdShutdownEngines: titles shut down and initialize again; the command
// processor keeps running.
HLE(kKrnl, 476, VdShutdownEngines) { return 0; }
HLE(kKrnl, 436, VdEnableDisableClockGating) { return 0; }

// VdInitializeRingBuffer(physical address, log2 of the size in 8-byte
// units).
HLE(kKrnl, 451, VdInitializeRingBuffer) {
  InitializeRingBuffer(Arg(c, 0), Arg(c, 1));
  return 0;
}

// VdEnableRingBufferRPtrWriteBack(address, log2 of the update block).
HLE(kKrnl, 438, VdEnableRingBufferRPtrWriteBack) {
  EnableReadPointerWriteBack(Arg(c, 0), Arg(c, 1));
  return 0;
}

// VdSetGraphicsInterruptCallback(callback, argument).
HLE(kKrnl, 469, VdSetGraphicsInterruptCallback) {
  SetGraphicsInterruptCallback(Arg(c, 0), Arg(c, 1));
  return 0;
}

// VdGetSystemCommandBuffer(*buffer, *value) and
// VdSetSystemCommandBufferGpuIdentifierAddress(address): the kernel's own
// command buffer for the display, which this driver does not have; the
// buffer is empty, the value 0, and the GPU identifier never written.
HLE(kKrnl, 445, VdGetSystemCommandBuffer) {
  std::memset(Mem() + Arg(c, 0), 0, kSystemCommandBufferSize);
  Wr32(Arg(c, 1), 0);
  return 0;
}
HLE(kKrnl, 473, VdSetSystemCommandBufferGpuIdentifierAddress) { return 0; }

// VdSwap(ring buffer words, front buffer fetch constant, writeback, system
// command buffer, its value, *front buffer, *format, *color space, *width,
// *height): fills the 64 ring buffer words the title set aside.
HLE(kKrnl, 603, VdSwap) {
  uint32_t fetch[6];
  for (uint32_t k = 0; k < 6; k++) fetch[k] = Rd32(Arg(c, 1) + 4 * k);
  WriteSwapPackets(Arg(c, 0), fetch, Rd32(Arg(c, 8)), Rd32(Arg(c, 9)));
  return 0;
}

// VdInitializeScalerCommandBuffer(source xy, source size, output xy,
// output size, front buffer size, vertical filter, its parameters,
// horizontal filter, its parameters, ?, buffer, words): the scaler's
// commands; here the display is not scaled, so the words are type-2
// no-ops. Returns the count.
HLE(kKrnl, 453, VdInitializeScalerCommandBuffer) {
  uint32_t dest = Arg(c, 10), words = Arg(c, 11);
  for (uint32_t k = 0; k < words; k++) Wr32(dest + 4 * k, 0x80000000);
  return words;
}

// VdCallGraphicsNotificationRoutines(?, scaling): calls the routines
// registered with VdRegisterGraphicsNotificationRoutine, of which there are
// none here (it has no handler).
HLE(kKrnl, 433, VdCallGraphicsNotificationRoutines) { return 0; }

// VdPersistDisplay(?, *buffer): keeps the display across a title's end,
// for which nothing is needed here; the buffer is physical memory the
// title frees with MmFreePhysicalMemory. TRUE.
HLE(kKrnl, 455, VdPersistDisplay) {
  if (uint32_t out = Arg(c, 1)) {
    uint32_t va = ReservePhysical(Heap::kPhysical4k, 0x1000, 0, 0, kPhysicalSize - 1, false);
    if (!va || !Commit(va, 0x1000, kPageReadWrite)) ppc::Fatal(&c, "VdPersistDisplay: out of physical memory");
    Wr32(out, va);
  }
  return 1;
}

// The display link's training (HSIO, EDRAM): nothing to train here.
HLE(kKrnl, 454, VdIsHSIOTrainingSucceeded) { return 1; }
HLE(kKrnl, 617, VdRetrainEDRAM) { return 0; }
HLE(kKrnl, 618, VdRetrainEDRAMWorker) { return 0; }

// KiApcNormalRoutineNop: an APC normal routine that does nothing.
HLE(kKrnl, 479, KiApcNormalRoutineNop) { return 0; }

}  // namespace recomp::xbox360
