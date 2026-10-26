#pragma once
// The Xenos as the kernel's display driver (the Vd* exports) and titles
// see it (ARCHITECTURE.md, "Runtime: the GPU"): its registers, which guest
// code reads and writes in guest memory where the console maps them; the
// command processor, which executes the PM4 packets of the ring buffer the
// driver sets up (registers, memory writes, waits, fences, interrupts),
// without drawing yet; and the display's vertical blank. GPU addresses
// are physical (the low 29 bits); the GPU reads and writes memory
// little-endian, with the endian swap a packet's address asks for.

#include <cstdint>

namespace recomp::xbox360 {

// Register r is the word at kGpuRegisters + 4 * r.
constexpr uint32_t kGpuRegisters = 0x7FC80000, kGpuRegisterCount = 0x5003;
// Registers named here.
constexpr uint32_t kRegCpRbRptr = 0x01C4, kRegCpRbWptr = 0x01C5, kRegScratchUmsk = 0x01DC,
                   kRegScratchAddr = 0x01DD, kRegScratchReg0 = 0x0578, kRegWritebackStart = 0x0A04,
                   kRegWritebackSize = 0x0A05, kRegCoherStatusHost = 0x0A31,
                   kRegRbEdramTiming = 0x0F00, kRegRbBcControl = 0x0F01,
                   kRegRbHsioAligner = 0x0F2D, kRegD1ModeVCounter = 0x194C,
                   kRegInterruptStatus = 0x1951, kRegD1ModeViewportSize = 0x1961,
                   kRegVgtEventInitiator = 0x21F9, kRegShaderConstantFetch = 0x4800;

uint32_t GpuRegister(uint32_t r);

// The GPU at power-on: its registers, the vertical blank. Once, after the
// process's TLS layout is known (it starts system threads).
void StartGpu();

// VdInitializeRingBuffer: the primary ring buffer at physical address
// `pa`, 2^(size_log2 + 3) bytes, emptied; the command processor follows
// CP_RB_WPTR from then on.
void InitializeRingBuffer(uint32_t pa, uint32_t size_log2);
// VdEnableRingBufferRPtrWriteBack: where the read pointer is written as
// the command processor advances it.
void EnableReadPointerWriteBack(uint32_t pa, uint32_t block_size_log2);
// VdSetGraphicsInterruptCallback: callback(source, user) as a DPC, source
// 0 for the vertical blank, 1 for a packet's interrupt.
void SetGraphicsInterruptCallback(uint32_t callback, uint32_t user);

// The 64 words VdSwap writes into the ring buffer at `va`: the front
// buffer's texture fetch (`fetch`, six words, its address made physical)
// and the swap packet, which the command processor counts as a frame.
constexpr uint32_t kSwapWords = 64;
void WriteSwapPackets(uint32_t va, const uint32_t fetch[6], uint32_t width, uint32_t height);
// Frames swapped so far.
uint64_t SwapCount();

// A guest address of physical address pa (in the 64 KiB view).
inline uint32_t PhysicalToVirtual(uint32_t pa) { return 0xA0000000 + (pa & 0x1FFFFFFF); }

}  // namespace recomp::xbox360
