# Xenos: behaviour the runtime reimplements

What the GPU and the kernel's display driver (the Vd* exports) do, as far
as the runtime (ARCHITECTURE.md, "The GPU") depends on it, and where each
fact comes from. The kernel's driver code is not available; the evidence
is titles' and xam's code, then Xenia's command processor and video
exports (BSD-3), then AMD's documentation of the R6xx command processor,
whose PM4 packets the Xenos shares. "Console check" marks what only a
console can settle.

## Registers
- The register file is at 0x7FC80000, register r at + 4r: a title writes
  CP_RB_WPTR (0x1C5) there and reads the interrupt status (0x1951), the
  scanline counter (0x194C) and the viewport size (0x1961); it also
  writes the primary surface address (0x1844) directly.
- Read-only values titles read: RB_EDRAM_TIMING 0x08100748, RB_BC_CONTROL
  0x0000200E, RB_HSIO_INTERFACE_ALIGNER 0x00BBBBBB (Xenia's), the
  interrupt status 1 (a vertical blank), the viewport 1280x720. They are
  set at power-on; a title that writes one changes it (Xenia ignores such
  writes). Console check.
- Writes the command processor makes (type 0 and 1 packets, constants)
  have the side effects Xenia gives them: SCRATCH_REG0-7 are written back
  to SCRATCH_ADDR + 4n when SCRATCH_UMSK enables them; a write to
  COHER_STATUS_HOST sets its bit 31, and a WAIT_REG_MEM on it clears the
  register (caches here are always coherent).

## The ring buffer
- VdInitializeRingBuffer(physical address, log2): 2^(log2 + 3) bytes,
  emptied, read pointer 0. VdEnableRingBufferRPtrWriteBack(address,
  block log2): the read pointer (in words) is written there, big-endian,
  after each batch (not every block). Xenia's.
- The title advances CP_RB_WPTR by plain stores, which nothing signals:
  the command processor polls the register (yielding, then every 50 us
  when idle).
- A title writes the ring with pre-incrementing stores: its write
  position is the last word written, and VdSwap's buffer starts one word
  after it.

## Packets
- Types: 0 writes count registers from a base (bit 15: all to one
  register), 1 two registers, 2 is a no-op, 3 an opcode with count
  words. A zero word and 0x0BADF00D are skipped, as Xenia does. A type-3
  packet with bit 0 runs only if BIN_SELECT & BIN_MASK (both
  0xFFFFFFFF at first).
- The GPU accesses memory little-endian, after the swap the address's low
  two bits name: none, bytes in halfwords, bytes in words (what a
  big-endian CPU reads as the value), halfwords in words.
- Handled as Xenia does: NOP, ME_INIT (no microcode runs here),
  INDIRECT_BUFFER(_PFD) (at once), WAIT_REG_MEM, REG_RMW, REG_TO_MEM,
  MEM_WRITE, COND_WRITE, EVENT_WRITE (without an address), EVENT_WRITE_SHD
  (a fence; bit 31 of the initiator writes the counter of vertical blanks
  and swaps instead, console check; addresses above physical memory go to
  the writeback window at 0x7F000000), EVENT_WRITE_EXT (the largest
  extents, since nothing is drawn), INTERRUPT (the callback with source 1
  for each processor in the mask), SET_CONSTANT, SET_CONSTANT2,
  SET_SHADER_CONSTANTS, LOAD_ALU_CONSTANT, VIZ_QUERY, the bin masks,
  CONTEXT_UPDATE, WAIT_IB_PFD_COMPLETE, WAIT_FOR_IDLE, INVALIDATE_STATE.
- Draws (DRAW_INDX, DRAW_INDX_2) and shader loads (IM_LOAD,
  IM_LOAD_IMMEDIATE) are consumed and nothing is drawn yet.
- Any other opcode is fatal and names it; EVENT_WRITE_ZPD (occlusion
  queries) waits for drawing.

## Interrupts and the vertical blank
- VdSetGraphicsInterruptCallback(callback, argument): the callback runs as
  a DPC, callback(source, argument), source 0 for the vertical blank
  (Xenia: on processor 2), 1 for a packet's interrupt. Here it runs on a
  kernel thread at DISPATCH_LEVEL with the KPCR's DPC flag set.
- The vertical blank comes at the video mode's rate; the scanline counter
  moves on by a frame's lines each time, as Xenia moves it. Console check.

## The display driver's exports
- VdSwap(ring words, front buffer fetch, writeback, system command buffer,
  its value, *front buffer, *format, *color space, *width, *height):
  fills the 64 words the title set aside with the front buffer's fetch
  constant (a type-0 write of 0x4800-0x4805, the address made physical),
  a swap packet (type 3, opcode 0x64, which the GPU leaves unused, then
  "SWAP", the front buffer, width, height), and type-2 no-ops, as Xenia
  does. The real kernel's words are not known. Console check.
- VdGetSystemCommandBuffer(buffer, value): the 0x94 bytes zeroed, the
  value 0; a title zeroes the buffer itself when it skips the swap, and
  reads its second word. VdSetSystemCommandBufferGpuIdentifierAddress
  records nothing (there is no system command buffer here).
- VdInitializeEngines returns TRUE; VdShutdownEngines does nothing
  (titles shut down and initialize again). Xenia's.
- VdInitializeScalerCommandBuffer fills its words with no-ops and
  returns their count; VdPersistDisplay returns TRUE and a page of
  physical memory, which the title frees with MmFreePhysicalMemory.
  Xenia's.
- VdQueryVideoFlags: widescreen 1, 1280 wide 2, 1920 wide 4.
  VdGetCurrentDisplayInformation: X_DISPLAY_INFO (0x58 bytes) at the
  video mode's size, unscaled, overscan 320/180. VdGetCurrentDisplayGamma:
  type 2 (BT.709), power 2.2222. Xenia's.
- The HSIO and EDRAM training calls succeed (nothing to train);
  VdCallGraphicsNotificationRoutines has no routines to call.
