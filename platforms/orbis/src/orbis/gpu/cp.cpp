// PM4 (type 3) packets as AMD documents them for Sea Islands (the PS4's
// GPU is that generation): the header's bits 31-30 are 3, 29-16 the body's
// size in dwords minus one, 15-8 the opcode, bit 1 the shader type
// (compute). Type 2 is a one-dword filler.

#include "orbis/gpu/cp.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "orbis/hle/kernel.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::orbis::gpu {
namespace {

enum Op : uint32_t {
  kNop = 0x10, kSetBase = 0x11, kClearState = 0x12, kIndexBufferSize = 0x13,
  kDispatchDirect = 0x15, kDispatchIndirect = 0x16, kSetPredication = 0x20,
  kDrawIndirect = 0x24, kDrawIndexIndirect = 0x25, kIndexBase = 0x26, kDrawIndex2 = 0x27,
  kContextControl = 0x28, kIndexType = 0x2A, kDrawIndexAuto = 0x2D, kNumInstances = 0x2F,
  kIndirectBufferConst = 0x33, kDrawIndexOffset2 = 0x35, kWriteData = 0x37,
  kWaitRegMem = 0x3C, kIndirectBuffer = 0x3F, kSurfaceSync = 0x43, kEventWrite = 0x46,
  kEventWriteEop = 0x47, kEventWriteEos = 0x48, kReleaseMem = 0x49, kDmaData = 0x50,
  kAcquireMem = 0x58, kSetConfigReg = 0x68, kSetContextReg = 0x69, kSetShReg = 0x76,
  kSetUconfigReg = 0x79, kLoadConstRam = 0x80, kWriteConstRam = 0x81, kDumpConstRam = 0x83,
  kIncrementCeCounter = 0x84, kIncrementDeCounter = 0x85, kWaitOnCeCounter = 0x86,
  kWaitOnDeCounterDiff = 0x88,
};

// Register offsets (dwords) of each SET_*_REG space.
constexpr uint32_t kRegSpace = 0x400;
// NOP bodies that start with 0xC01E0000 | offset carry context register
// values (sceGnm*Update*Shader): applied as SET_CONTEXT_REG.
constexpr uint32_t kNopContextRegs = 0xC01E0000;

struct Regs {
  uint32_t config[kRegSpace] = {}, context[kRegSpace] = {}, sh[kRegSpace] = {},
           uconfig[kRegSpace] = {};
};

// One engine's instruction stream: the buffer, and the indirect buffers it
// called (their return points).
struct Stream {
  struct Frame {
    gaddr p, end;
  };
  std::vector<Frame> stack;
  bool done() const { return stack.empty(); }
};

struct Engines {
  Regs regs;
  uint8_t ce_ram[48 * 1024] = {};
  uint64_t ce_count = 0, de_count = 0;
  bool warned_draw = false, warned_dispatch = false;
};

uint64_t Ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
// The GPU's clocks: the 100 MHz reference (global) clock and the 800 MHz
// core clock.
uint64_t GlobalClock() { return Ns() / 10; }
uint64_t CoreClock() { return Ns() * 4 / 5; }

void WriteMem(gaddr a, uint32_t sel, uint64_t data) {
  switch (sel) {
    case 0: break;
    case 1: Wr32(a, uint32_t(data)); break;
    case 2: Wr64(a, data); break;
    case 3: Wr64(a, GlobalClock()); break;
    case 4: Wr64(a, CoreClock()); break;
    default: FatalF("GPU: end-of-pipe data select %u not implemented yet", sel);
  }
}

bool Compare(uint32_t func, uint32_t v, uint32_t ref) {
  switch (func) {
    case 0: return true;
    case 1: return v < ref;
    case 2: return v <= ref;
    case 3: return v == ref;
    case 4: return v != ref;
    case 5: return v >= ref;
    case 6: return v > ref;
  }
  FatalF("GPU: WAIT_REG_MEM function %u", func);
}

void SetRegs(uint32_t* space, const char* name, uint32_t off, const uint32_t* v, uint32_t n) {
  if (off + n > kRegSpace) FatalF("GPU: %s registers %X+%u out of range", name, off, n);
  std::memcpy(space + off, v, 4 * n);
}

// Runs one packet of s; false if it must wait (the stream stays on it).
// ce: s is the constant engine's.
bool Step(Engines& e, Stream& s, bool ce) {
  Stream::Frame& f = s.stack.back();
  if (f.p >= f.end) {
    s.stack.pop_back();
    return true;
  }
  const uint32_t h = Rd32(f.p);
  if (h >> 30 == 2) {
    f.p += 4;
    return true;
  }
  if (h >> 30 != 3) FatalF("GPU: packet type %u (%08X) at %llX", h >> 30, h, (unsigned long long)f.p);
  const uint32_t n = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0xFF;
  const gaddr b = f.p + 4;
  if (b + 4 * uint64_t(n) > f.end)
    FatalF("GPU: packet %02X at %llX runs past its buffer", op, (unsigned long long)f.p);
  std::vector<uint32_t> d(n);
  std::memcpy(d.data(), GPtr(b), 4 * n);
  auto next = [&] { f.p = b + 4 * uint64_t(n); };
  switch (op) {
    case kNop:
      if (n >= 2 && (d[0] & 0xFFFF0000) == kNopContextRegs)
        SetRegs(e.regs.context, "context", d[0] & 0xFFFF, &d[1], n - 1);
      break;
    case kSetConfigReg: SetRegs(e.regs.config, "config", d[0] & 0xFFFF, &d[1], n - 1); break;
    case kSetContextReg: SetRegs(e.regs.context, "context", d[0] & 0xFFFF, &d[1], n - 1); break;
    case kSetShReg: SetRegs(e.regs.sh, "SH", d[0] & 0xFFFF, &d[1], n - 1); break;
    case kSetUconfigReg: SetRegs(e.regs.uconfig, "uconfig", d[0] & 0xFFFF, &d[1], n - 1); break;
    case kClearState: std::memset(e.regs.context, 0, sizeof e.regs.context); break;
    // State and cache control: nothing to do where memory is coherent.
    case kContextControl: case kAcquireMem: case kSurfaceSync: case kIndexType:
    case kIndexBase: case kIndexBufferSize: case kNumInstances: case kSetBase:
    case kSetPredication:
      break;
    case kEventWrite: {
      // Flushes, partial flushes and cache actions only: the events that
      // write query results are not implemented yet.
      const uint32_t type = d[0] & 0x3F;
      if (n > 1) FatalF("GPU: EVENT_WRITE of event %02X with an address not implemented yet", type);
      break;
    }
    case kDrawIndex2: case kDrawIndexAuto: case kDrawIndexOffset2: case kDrawIndirect:
    case kDrawIndexIndirect:
      if (!e.warned_draw) WarnF("GPU: drawing is not implemented yet; draws are skipped\n");
      e.warned_draw = true;
      break;
    case kDispatchDirect: case kDispatchIndirect:
      if (!e.warned_dispatch) WarnF("GPU: compute is not implemented yet; dispatches are skipped\n");
      e.warned_dispatch = true;
      break;
    case kWriteData: {
      const uint32_t dst = (d[0] >> 8) & 0xF;
      if (dst != 1 && dst != 2 && dst != 5) FatalF("GPU: WRITE_DATA to destination %u", dst);
      const gaddr a = d[1] | uint64_t(d[2]) << 32;
      for (uint32_t i = 3; i < n; i++) Wr32(a + 4 * uint64_t(i - 3), d[i]);
      break;
    }
    case kWaitRegMem: {
      if (!(d[0] & 0x10)) FatalF("GPU: WAIT_REG_MEM on a register not implemented yet");
      const gaddr a = (d[1] & ~3u) | uint64_t(d[2] & 0xFFFF) << 32;
      if (!Compare(d[0] & 7, Rd32(a) & d[4], d[3])) return false;
      break;
    }
    case kEventWriteEop: {
      const gaddr a = (d[1] & ~3u) | uint64_t(d[2] & 0xFFFF) << 32;
      WriteMem(a, d[2] >> 29, d[3] | uint64_t(d[4]) << 32);
      if ((d[2] >> 24) & 3) kernel::TriggerEvent(kEqEventGfxEop, kFilterGraphicsCore, kEqEventGfxEop);
      break;
    }
    case kReleaseMem: {
      const gaddr a = (d[2] & ~3u) | uint64_t(d[3] & 0xFFFF) << 32;
      WriteMem(a, d[1] >> 29, d[4] | uint64_t(d[5]) << 32);
      if ((d[1] >> 24) & 3) kernel::TriggerEvent(kEqEventGfxEop, kFilterGraphicsCore, kEqEventGfxEop);
      break;
    }
    case kEventWriteEos: {
      const uint32_t cmd = d[2] >> 29;
      if (cmd != 2) FatalF("GPU: EVENT_WRITE_EOS command %u not implemented yet", cmd);
      Wr32((d[1] & ~3u) | uint64_t(d[2] & 0xFFFF) << 32, d[3]);
      break;
    }
    case kDmaData: {
      // Sources: memory (0, or 3 through the L2), GDS (1), data (2);
      // destinations: memory (0), GDS (1). The command's DAS/SAS (bits
      // 27/26) put either in register space, SAIC/DAIC (28/29) keep its
      // address from incrementing.
      const uint32_t src_sel = (d[0] >> 29) & 3, dst_sel = (d[0] >> 20) & 3;
      const uint32_t bytes = d[5] & 0x1FFFFF;
      const bool dst_reg = d[5] & 0x08000000, src_reg = d[5] & 0x04000000;
      const bool dst_fixed = d[5] & 0x20000000, src_fixed = d[5] & 0x10000000;
      const gaddr dst = d[3] | uint64_t(d[4]) << 32;
      const bool src_mem = src_sel == 0 || src_sel == 3;
      if (dst_sel == 0 && dst_reg && dst_fixed && src_mem && !src_reg) {
        // Memory read into one register: Gnm's prefetch into the L2. Only
        // the cache sees the data.
        break;
      }
      if (dst_sel != 0 || dst_reg || dst_fixed || src_reg || (src_mem && src_fixed))
        FatalF("GPU: DMA_DATA %08X %08X not implemented yet", d[0], d[5]);
      if (src_sel == 2) {
        for (uint32_t i = 0; i + 4 <= bytes; i += 4) Wr32(dst + i, d[1]);
      } else if (src_mem) {
        std::memmove(GPtr(dst), GPtr(d[1] | uint64_t(d[2]) << 32), bytes);
      } else {
        FatalF("GPU: DMA_DATA from GDS not implemented yet");
      }
      break;
    }
    case kIndirectBuffer: case kIndirectBufferConst: {
      if ((op == kIndirectBufferConst) != ce) FatalF("GPU: indirect buffer %02X on the wrong engine", op);
      const gaddr a = (d[0] & ~3u) | uint64_t(d[1] & 0xFFFF) << 32;
      next();
      s.stack.push_back({a, a + 4 * uint64_t(d[2] & 0xFFFFF)});
      return true;
    }
    case kLoadConstRam: {
      const gaddr a = d[0] | uint64_t(d[1]) << 32;
      const uint32_t bytes = 4 * (d[2] & 0x7FFF), at = d[3] & 0xFFFF;
      if (at + bytes > sizeof e.ce_ram) FatalF("GPU: LOAD_CONST_RAM past the CE RAM");
      std::memcpy(e.ce_ram + at, GPtr(a), bytes);
      break;
    }
    case kWriteConstRam: {
      const uint32_t at = d[0] & 0xFFFF;
      if (at + 4 * (n - 1) > sizeof e.ce_ram) FatalF("GPU: WRITE_CONST_RAM past the CE RAM");
      std::memcpy(e.ce_ram + at, &d[1], 4 * (n - 1));
      break;
    }
    case kDumpConstRam: {
      const uint32_t at = d[0] & 0xFFFF, bytes = 4 * (d[1] & 0x7FFF);
      if (at + bytes > sizeof e.ce_ram) FatalF("GPU: DUMP_CONST_RAM past the CE RAM");
      std::memcpy(GPtr(d[2] | uint64_t(d[3]) << 32), e.ce_ram + at, bytes);
      break;
    }
    case kIncrementCeCounter: e.ce_count++; break;
    case kIncrementDeCounter: e.de_count++; break;
    case kWaitOnCeCounter:
      if (e.ce_count <= e.de_count) return false;
      break;
    case kWaitOnDeCounterDiff:
      if (e.ce_count - e.de_count >= d[0]) return false;
      break;
    default:
      FatalF("GPU: PM4 opcode %02X (%08X) at %llX not implemented yet", op, h,
             (unsigned long long)f.p);
  }
  next();
  return true;
}

// Runs the drawing engine's stream beside the constant engine's until both
// are done; a wait on CPU-written memory polls.
void Run(Engines& e, Stream& de, Stream& ce) {
  while (!de.done() || !ce.done()) {
    bool moved = false;
    while (!ce.done() && Step(e, ce, true)) moved = true;
    while (!de.done() && Step(e, de, false)) moved = true;
    if (!moved) {
      GuestSyncFence();
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  GuestSyncFence();
}

struct Submission {
  std::vector<CmdBuffer> dcbs, ccbs;
  std::function<void()> done;
};

// The queues, never destroyed: the GPU threads wait on them until exit.
struct Queues {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Submission> gfx;
  bool gfx_up = false;
  std::map<uint32_t, std::shared_ptr<struct Ring>> rings;
};
Queues& Q() {
  static Queues* q = new Queues;
  return *q;
}

void GraphicsThread() {
  auto e = std::make_unique<Engines>();
  for (;;) {
    Submission sub;
    {
      std::unique_lock<std::mutex> l(Q().mu);
      Q().cv.wait(l, [] { return !Q().gfx.empty(); });
      sub = std::move(Q().gfx.front());
    }
    for (size_t i = 0; i < sub.dcbs.size(); i++) {
      Stream de, ce;
      de.stack.push_back({sub.dcbs[i].addr, sub.dcbs[i].addr + 4 * uint64_t(sub.dcbs[i].dwords)});
      if (i < sub.ccbs.size() && sub.ccbs[i].dwords)
        ce.stack.push_back({sub.ccbs[i].addr, sub.ccbs[i].addr + 4 * uint64_t(sub.ccbs[i].dwords)});
      Run(*e, de, ce);
    }
    if (sub.done) sub.done();
    std::lock_guard<std::mutex> l(Q().mu);
    Q().gfx.pop_front();
  }
}

struct Ring {
  gaddr base, rptr_addr;
  uint32_t size_dw;
  std::atomic<uint32_t> wptr{0};
  std::atomic<bool> unmapped{false};
  std::mutex mu;
  std::condition_variable cv;
};

// A compute queue: its packets run to the write pointer; a packet that
// wraps around the ring's end is not supported (the rings are filled by
// the game's own code, which wraps with NOPs).
void ComputeThread(std::shared_ptr<Ring> r) {
  auto e = std::make_unique<Engines>();
  uint32_t rptr = 0;
  for (;;) {
    uint32_t w;
    {
      std::unique_lock<std::mutex> l(r->mu);
      r->cv.wait(l, [&] { return r->unmapped || r->wptr.load() % r->size_dw != rptr; });
      if (r->unmapped) return;
      w = r->wptr.load() % r->size_dw;
    }
    const uint32_t end = w > rptr ? w : r->size_dw;
    Stream de, ce;
    de.stack.push_back({r->base + 4 * uint64_t(rptr), r->base + 4 * uint64_t(end)});
    Run(*e, de, ce);
    rptr = end % r->size_dw;
    Wr32(r->rptr_addr, rptr);
  }
}

}  // namespace

void SubmitGraphics(std::vector<CmdBuffer> dcbs, std::vector<CmdBuffer> ccbs,
                    std::function<void()> done) {
  GuestSyncFence();
  std::lock_guard<std::mutex> l(Q().mu);
  if (!Q().gfx_up) {
    Q().gfx_up = true;
    std::thread(GraphicsThread).detach();
  }
  Q().gfx.push_back({std::move(dcbs), std::move(ccbs), std::move(done)});
  Q().cv.notify_all();
}

void MapComputeRing(uint32_t vqid, gaddr base, uint32_t size_dw, gaddr rptr_addr) {
  auto r = std::make_shared<Ring>();
  r->base = base;
  r->size_dw = size_dw;
  r->rptr_addr = rptr_addr;
  Wr32(rptr_addr, 0);
  {
    std::lock_guard<std::mutex> l(Q().mu);
    if (Q().rings.count(vqid)) FatalF("GPU: compute queue %u is mapped already", vqid);
    Q().rings[vqid] = r;
  }
  std::thread(ComputeThread, r).detach();
}

void UnmapComputeRing(uint32_t vqid) {
  std::shared_ptr<Ring> r;
  {
    std::lock_guard<std::mutex> l(Q().mu);
    auto it = Q().rings.find(vqid);
    if (it == Q().rings.end()) return;
    r = it->second;
    Q().rings.erase(it);
  }
  std::lock_guard<std::mutex> l(r->mu);
  r->unmapped = true;
  r->cv.notify_all();
}

void KickComputeRing(uint32_t vqid, uint32_t wptr_dw) {
  GuestSyncFence();
  std::shared_ptr<Ring> r;
  {
    std::lock_guard<std::mutex> l(Q().mu);
    auto it = Q().rings.find(vqid);
    if (it == Q().rings.end()) FatalF("sceGnmDingDong: compute queue %u is not mapped", vqid);
    r = it->second;
  }
  std::lock_guard<std::mutex> l(r->mu);
  r->wptr = wptr_dw;
  r->cv.notify_all();
}

}  // namespace recomp::orbis::gpu
