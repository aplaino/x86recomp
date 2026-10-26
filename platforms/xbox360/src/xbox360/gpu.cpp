// The GPU (gpu.h): registers, the vertical blank, the command processor.
//
// The packets' meaning is Xenia's command processor's (BSD-3; AMD's R6xx
// documentation for the shared ones); what titles' code shows is noted in
// docs/xenos.md.

#include "xbox360/gpu.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "ppc/fatal.h"
#include "xbox360/config.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

using ppc::Fatal;
using Clock = std::chrono::steady_clock;

// Type-3 opcodes.
enum : uint32_t {
  kPm4Nop = 0x10,
  kPm4RegRmw = 0x21,
  kPm4DrawIndx = 0x22,
  kPm4VizQuery = 0x23,
  kPm4WaitForIdle = 0x26,
  kPm4ImLoad = 0x27,
  kPm4ImLoadImmediate = 0x2B,
  kPm4SetConstant = 0x2D,
  kPm4LoadAluConstant = 0x2F,
  kPm4DrawIndx2 = 0x36,
  kPm4IndirectBufferPfd = 0x37,
  kPm4InvalidateState = 0x3B,
  kPm4WaitRegMem = 0x3C,
  kPm4MemWrite = 0x3D,
  kPm4RegToMem = 0x3E,
  kPm4IndirectBuffer = 0x3F,
  kPm4CondWrite = 0x45,
  kPm4EventWrite = 0x46,
  kPm4MeInit = 0x48,
  kPm4SetBinMask = 0x50,
  kPm4SetBinSelect = 0x51,
  kPm4Interrupt = 0x54,
  kPm4SetConstant2 = 0x55,
  kPm4SetShaderConstants = 0x56,
  kPm4EventWriteShd = 0x58,
  kPm4EventWriteExt = 0x5A,
  kPm4WaitIbPfdComplete = 0x5D,
  kPm4ContextUpdate = 0x5E,
  kPm4SetBinMaskLo = 0x60,
  kPm4SetBinMaskHi = 0x61,
  kPm4SetBinSelectLo = 0x62,
  kPm4SetBinSelectHi = 0x63,
  // Not the GPU's: the packet VdSwap writes here (an opcode the GPU leaves
  // unused, as Xenia picked it), with kSwapSignature.
  kPm4Swap = 0x64,
};
constexpr uint32_t kSwapSignature = 0x53574150;  // "SWAP"
// VGT_EVENT_INITIATOR values of VIZ_QUERY.
constexpr uint32_t kVizQueryStart = 7, kVizQueryEnd = 8;

void SetReg(uint32_t r, uint32_t v) { Wr32(kGpuRegisters + 4 * r, v); }

uint32_t Bswap(uint32_t v) { return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24; }

// The endian swap of a GPU address's low two bits: none, bytes in
// halfwords, bytes in words, halfwords in words.
uint32_t GpuSwap(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1: return (v << 8 & 0xFF00FF00) | (v >> 8 & 0x00FF00FF);
    case 2: return Bswap(v);
    case 3: return v >> 16 | v << 16;
    default: return v;
  }
}
// The GPU's own word accesses: little-endian, after the swap.
uint32_t GpuRead(uint32_t addr) {
  return GpuSwap(Bswap(Rd32(PhysicalToVirtual(addr & ~3u))), addr);
}
void GpuWrite(uint32_t addr, uint32_t v) {
  Wr32(PhysicalToVirtual(addr & ~3u), Bswap(GpuSwap(v, addr)));
}

// The ring buffer and what the driver set up around it.
std::mutex g_ring_lock;
uint32_t g_ring_pa, g_ring_dwords, g_rptr, g_writeback_pa;
// The interrupt callback and its argument, set together.
std::atomic<uint64_t> g_interrupt;
// EVENT_WRITE_SHD's counter: vertical blanks and swaps, as Xenia counts
// it (console check).
std::atomic<uint32_t> g_counter;
std::atomic<uint64_t> g_swaps;

void Interrupt(uint32_t source, uint32_t cpu) {
  uint64_t cb = g_interrupt.load();
  if (cb >> 32) CallDpc(uint32_t(cb >> 32), source, uint32_t(cb), cpu);
}

// ---- the command processor ---------------------------------------------------
// A packet stream: `remaining` words from `pos` of a buffer of `size`
// words at guest address `va`, wrapping (the ring buffer does).
struct Reader {
  uint32_t va, size, pos, remaining;
  uint32_t Next() {
    if (!remaining) Fatal(nullptr, "GPU: a packet runs past the end of its buffer");
    uint32_t v = Rd32(va + 4 * pos);
    pos = (pos + 1) % size;
    remaining--;
    return v;
  }
  void Skip(uint32_t n) {
    for (uint32_t k = 0; k < n; k++) Next();
  }
};

// Bin mask and select, against which predicated packets run (PFP state).
uint64_t g_bin_mask = 0xFFFFFFFF, g_bin_select = 0xFFFFFFFF;

void WriteRegister(uint32_t r, uint32_t v) {
  if (r >= kGpuRegisterCount) Fatal(nullptr, "GPU: write to register %x, past the register file", r);
  SetReg(r, v);
  if (r - kRegScratchReg0 < 8) {
    // Scratch registers enabled in SCRATCH_UMSK are written back to memory.
    uint32_t n = r - kRegScratchReg0;
    if (GpuRegister(kRegScratchUmsk) >> n & 1) Wr32(PhysicalToVirtual(GpuRegister(kRegScratchAddr) + 4 * n), v);
  } else if (r == kRegCoherStatusHost) {
    // A coherency request: pending until a wait looks at it.
    SetReg(r, v | 0x80000000);
  }
}

// WAIT_REG_MEM's and COND_WRITE's comparison of wait_info's low 3 bits:
// never, <, <=, ==, !=, >=, >, always.
bool Match(uint32_t value, uint32_t ref, uint32_t wait_info) {
  switch (wait_info & 7) {
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    case 7: return true;
    default: return false;
  }
}

void Execute(Reader& in);

void Type3(Reader& in, uint32_t packet) {
  uint32_t opcode = packet >> 8 & 0x7F, count = (packet >> 16 & 0x3FFF) + 1;
  if (count > in.remaining) Fatal(nullptr, "GPU: type-3 packet %x runs past the end of its buffer", packet);
  // Predicated packets run only for a selected bin; never a swap.
  if (packet & 1 && (!(g_bin_select & g_bin_mask) || opcode == kPm4Swap)) {
    in.Skip(count);
    return;
  }
  switch (opcode) {
    case kPm4Nop:
    case kPm4MeInit:  // the micro-engine's microcode: there is none to run
    case kPm4WaitIbPfdComplete:  // indirect buffers run at once
    case kPm4WaitForIdle:  // the GPU is idle between packets
    case kPm4InvalidateState:
    case kPm4ContextUpdate:
      in.Skip(count);
      return;
    case kPm4Interrupt: {
      uint32_t cpus = in.Next();
      for (uint32_t n = 0; n < 6; n++)
        if (cpus >> n & 1) Interrupt(1, n);
      return;
    }
    case kPm4Swap: {
      if (in.Next() != kSwapSignature) Fatal(nullptr, "GPU: a swap packet without its signature");
      in.Skip(count - 1);
      g_swaps++;
      g_counter++;
      return;
    }
    case kPm4IndirectBuffer:
    case kPm4IndirectBufferPfd: {
      uint32_t pa = in.Next() & 0x1FFFFFFF, words = in.Next() & 0xFFFFF;
      Reader ib{PhysicalToVirtual(pa), words ? words : 1, 0, words};
      while (ib.remaining) Execute(ib);
      return;
    }
    case kPm4WaitRegMem: {
      uint32_t info = in.Next(), poll = in.Next(), ref = in.Next(), mask = in.Next(), wait = in.Next();
      for (;;) {
        uint32_t value;
        if (info & 0x10) {
          value = GpuRead(poll);
        } else {
          // Caches here are always coherent: a request completes when looked at.
          if (poll == kRegCoherStatusHost && GpuRegister(poll) & 0x80000000) SetReg(poll, 0);
          value = GpuRegister(poll);
        }
        if (Match(value & mask, ref, info)) return;
        if (wait >= 0x100)
          std::this_thread::sleep_for(std::chrono::milliseconds(wait / 0x100));
        else
          std::this_thread::yield();
      }
    }
    case kPm4RegRmw: {
      uint32_t info = in.Next(), and_mask = in.Next(), or_mask = in.Next();
      uint32_t v = GpuRegister(info & 0x1FFF);
      v &= info >> 31 ? GpuRegister(and_mask & 0x1FFF) : and_mask;
      v |= info >> 30 & 1 ? GpuRegister(or_mask & 0x1FFF) : or_mask;
      WriteRegister(info & 0x1FFF, v);
      return;
    }
    case kPm4RegToMem: {
      uint32_t r = in.Next(), addr = in.Next();
      GpuWrite(addr, GpuRegister(r));
      return;
    }
    case kPm4MemWrite: {
      uint32_t addr = in.Next();
      for (uint32_t k = 1; k < count; k++, addr += 4) GpuWrite(addr, in.Next());
      return;
    }
    case kPm4CondWrite: {
      uint32_t info = in.Next(), poll = in.Next(), ref = in.Next(), mask = in.Next(), dest = in.Next(),
               data = in.Next();
      uint32_t value = info & 0x10 ? GpuRead(poll) : GpuRegister(poll);
      if (Match(value & mask, ref, info)) {
        if (info & 0x100)
          GpuWrite(dest, data);
        else
          WriteRegister(dest, data);
      }
      return;
    }
    case kPm4EventWrite:
      SetReg(kRegVgtEventInitiator, in.Next() & 0x3F);
      if (count > 1) Fatal(nullptr, "GPU: EVENT_WRITE with an address is not supported yet");
      return;
    case kPm4EventWriteShd: {
      // A fence: the value (or, with bit 31, the counter) to memory, or to
      // the writeback window at 0x7F000000 for addresses above physical
      // memory.
      uint32_t initiator = in.Next(), addr = in.Next(), value = in.Next();
      SetReg(kRegVgtEventInitiator, initiator & 0x3F);
      if (initiator >> 31) value = g_counter.load();
      uint32_t base = GpuRegister(kRegWritebackStart), offset = (addr & ~3u) - base;
      if (addr > 0x1FFFFFFF && base && offset < GpuRegister(kRegWritebackSize))
        Wr32(0x7F000000 + offset, Bswap(GpuSwap(value, addr)));
      else
        GpuWrite(addr, value);
      return;
    }
    case kPm4EventWriteExt: {
      // The screen extent of earlier draws: with no drawing, the largest,
      // in 8-pixel units, as halfwords.
      SetReg(kRegVgtEventInitiator, in.Next() & 0x3F);
      uint32_t va = PhysicalToVirtual(in.Next() & ~3u);
      const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
      for (uint32_t k = 0; k < 6; k++) Wr16(va + 2 * k, extents[k]);
      return;
    }
    case kPm4DrawIndx:
    case kPm4DrawIndx2:
    case kPm4ImLoad:
    case kPm4ImLoadImmediate:
      // Draws and shader loads: nothing is drawn yet.
      in.Skip(count);
      return;
    case kPm4VizQuery:
      SetReg(kRegVgtEventInitiator, in.Next() & 0x100 ? kVizQueryEnd : kVizQueryStart);
      return;
    case kPm4SetConstant: {
      // Constants by type: ALU, fetch, bool, loop, registers.
      static constexpr uint32_t kBase[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      uint32_t offset_type = in.Next(), type = offset_type >> 16 & 0xFF;
      if (type > 4) Fatal(nullptr, "GPU: SET_CONSTANT of type %u", type);
      for (uint32_t k = 1; k < count; k++) WriteRegister(kBase[type] + (offset_type & 0x7FF) + k - 1, in.Next());
      return;
    }
    case kPm4SetConstant2:
    case kPm4SetShaderConstants: {
      uint32_t index = in.Next() & 0xFFFF;
      for (uint32_t k = 1; k < count; k++) WriteRegister(index + k - 1, in.Next());
      return;
    }
    case kPm4LoadAluConstant: {
      static constexpr uint32_t kBase[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      uint32_t addr = in.Next() & 0x3FFFFFFF, offset_type = in.Next(), words = in.Next() & 0xFFF;
      uint32_t type = offset_type >> 16 & 0xFF;
      if (type > 4) Fatal(nullptr, "GPU: LOAD_ALU_CONSTANT of type %u", type);
      for (uint32_t k = 0; k < words; k++)
        WriteRegister(kBase[type] + (offset_type & 0x7FF) + k, Rd32(PhysicalToVirtual(addr + 4 * k)));
      return;
    }
    case kPm4SetBinMaskLo: g_bin_mask = (g_bin_mask & ~0xFFFFFFFFull) | in.Next(); return;
    case kPm4SetBinMaskHi: g_bin_mask = (g_bin_mask & 0xFFFFFFFF) | uint64_t(in.Next()) << 32; return;
    case kPm4SetBinSelectLo: g_bin_select = (g_bin_select & ~0xFFFFFFFFull) | in.Next(); return;
    case kPm4SetBinSelectHi: g_bin_select = (g_bin_select & 0xFFFFFFFF) | uint64_t(in.Next()) << 32; return;
    case kPm4SetBinMask: {
      uint64_t hi = in.Next();
      g_bin_mask = hi << 32 | in.Next();
      return;
    }
    case kPm4SetBinSelect: {
      uint64_t hi = in.Next();
      g_bin_select = hi << 32 | in.Next();
      return;
    }
    default:
      Fatal(nullptr, "GPU: type-3 packet opcode %x (%u words) is not supported yet", opcode, count);
  }
}

void Execute(Reader& in) {
  uint32_t packet = in.Next();
  // Zero (and the driver's filler) are skipped, as Xenia does.
  if (!packet || packet == 0x0BADF00D) return;
  switch (packet >> 30) {
    case 0: {
      // count registers from base, or count values to the one register.
      uint32_t count = (packet >> 16 & 0x3FFF) + 1, base = packet & 0x7FFF;
      if (count > in.remaining) Fatal(nullptr, "GPU: type-0 packet %x runs past the end of its buffer", packet);
      for (uint32_t k = 0; k < count; k++) WriteRegister(packet >> 15 & 1 ? base : base + k, in.Next());
      return;
    }
    case 1: {
      uint32_t a = in.Next(), b = in.Next();
      WriteRegister(packet & 0x7FF, a);
      WriteRegister(packet >> 11 & 0x7FF, b);
      return;
    }
    case 2: return;
    default: Type3(in, packet);
  }
}

// Follows CP_RB_WPTR: guest code writes it to the register's memory, which
// nothing signals, so the command processor polls it.
void CommandProcessor(void*) {
  for (unsigned idle = 0;; idle++) {
    uint32_t va, size, rptr, wptr, writeback;
    {
      std::lock_guard<std::mutex> g(g_ring_lock);
      size = g_ring_dwords;
      rptr = g_rptr;
      wptr = size ? GpuRegister(kRegCpRbWptr) % size : 0;
      va = PhysicalToVirtual(g_ring_pa);
      writeback = g_writeback_pa;
    }
    if (!size || wptr == rptr) {
      if (idle < 1000)
        std::this_thread::yield();
      else
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }
    idle = 0;
    Reader in{va, size, rptr, (wptr + size - rptr) % size};
    while (in.remaining) Execute(in);
    std::lock_guard<std::mutex> g(g_ring_lock);
    if (g_ring_dwords != size) continue;  // set up again meanwhile
    g_rptr = wptr;
    SetReg(kRegCpRbRptr, wptr);
    if (writeback) Wr32(PhysicalToVirtual(writeback), wptr);
  }
}

// ---- the display ---------------------------------------------------------------
// The vertical blank at the video mode's rate: the scanline counter moves
// on by a frame's lines (as Xenia moves it), the counter by one, and the
// interrupt callback runs with source 0 (on processor 2, Xenia's choice).
void VerticalBlank(void*) {
  VideoMode m = CurrentVideoMode();
  auto period = std::chrono::nanoseconds(1000000000 / m.refresh_hz);
  Clock::time_point next = Clock::now();
  for (;;) {
    next += period;
    std::this_thread::sleep_until(next);
    // A host stall is not made up beyond a second.
    if (Clock::now() - next > std::chrono::seconds(1)) next = Clock::now();
    SetReg(kRegD1ModeVCounter, GpuRegister(kRegD1ModeVCounter) + m.height);
    g_counter++;
    Interrupt(0, 2);
  }
}

}  // namespace

uint32_t GpuRegister(uint32_t r) { return Rd32(kGpuRegisters + 4 * r); }

// Registers whose power-on values titles read: EDRAM timing, BC control
// and the HSIO aligner (Xenia's), the interrupt status (a vertical blank),
// the viewport's size.
void StartGpu() {
  VideoMode m = CurrentVideoMode();
  SetReg(kRegRbEdramTiming, 0x08100748);
  SetReg(kRegRbBcControl, 0x0000200E);
  SetReg(kRegRbHsioAligner, 0x00BBBBBB);
  SetReg(kRegInterruptStatus, 1);
  SetReg(kRegD1ModeViewportSize, m.width << 16 | m.height);
  CreateSystemThread(CommandProcessor, nullptr);
  CreateSystemThread(VerticalBlank, nullptr);
}

void InitializeRingBuffer(uint32_t pa, uint32_t size_log2) {
  std::lock_guard<std::mutex> g(g_ring_lock);
  uint32_t bytes = 1u << (size_log2 + 3);
  std::memset(Mem() + PhysicalToVirtual(pa), 0, bytes);
  g_ring_pa = pa;
  g_ring_dwords = bytes / 4;
  g_rptr = 0;
  SetReg(kRegCpRbRptr, 0);
  SetReg(kRegCpRbWptr, 0);
}

void EnableReadPointerWriteBack(uint32_t pa, uint32_t) {
  std::lock_guard<std::mutex> g(g_ring_lock);
  g_writeback_pa = pa;
}

void SetGraphicsInterruptCallback(uint32_t callback, uint32_t user) {
  g_interrupt = uint64_t(callback) << 32 | user;
}

void WriteSwapPackets(uint32_t va, const uint32_t fetch[6], uint32_t width, uint32_t height) {
  // The fetch constant holds the front buffer's virtual address (bits
  // 12-31 of its second word); the GPU's is physical.
  uint32_t fb = fetch[1] & 0xFFFFF000, pa;
  if (!PhysicalAddress(fb, pa)) Fatal(nullptr, "VdSwap: the front buffer at %x is not physical memory", fb);
  uint32_t w = 0;
  auto put = [&](uint32_t v) { Wr32(va + 4 * w++, v); };
  put(5u << 16 | kRegShaderConstantFetch);  // type 0: six registers
  for (uint32_t k = 0; k < 6; k++) put(k == 1 ? (fetch[1] & 0xFFF) | (pa & 0xFFFFF000) : fetch[k]);
  put(0xC0000000 | 3u << 16 | kPm4Swap << 8);  // type 3: four words
  put(kSwapSignature);
  put(pa);
  put(width);
  put(height);
  while (w < kSwapWords) put(0x80000000);  // type 2
}

uint64_t SwapCount() { return g_swaps.load(); }

}  // namespace recomp::xbox360
