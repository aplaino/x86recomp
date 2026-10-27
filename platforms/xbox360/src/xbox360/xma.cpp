// The XMA decoder (xma.h): the context array, its registers, commands.
//
// The registers' and the commands' meaning is Xenia's XMA decoder's
// (BSD-3); what xam's and titles' code shows is noted in docs/xboxkrnl.md,
// "XMA".

#include "xbox360/xma.h"

#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/memory.h"

namespace recomp::xbox360 {
namespace {

using ppc::Fatal;

constexpr uint32_t kCommandWords = (kXmaContexts + 31) / 32;
// A context's input read offset after a clear: past the first packet's
// header (32 bits), Xenia's.
constexpr uint32_t kPacketHeaderBits = 32;

std::mutex g_lock;
uint32_t g_array;  // the context array's guest address
std::bitset<kXmaContexts> g_used;

uint32_t Bswap(uint32_t v) { return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24; }
uint32_t RegisterVa(uint32_t r) { return kXmaRegisters + 4 * r; }

// Takes the bits written to a command register, leaving it 0 for the next.
// The word's bytes are the register's, little-endian.
uint32_t TakeCommand(uint32_t r) {
  auto* word = reinterpret_cast<uint32_t*>(Mem() + RegisterVa(r));
  uint32_t bytes = std::atomic_ref<uint32_t>(*word).exchange(0);
  return std::endian::native == std::endian::little ? bytes : Bswap(bytes);
}

// Clear: no buffer valid, the read and write offsets at their start
// (Xenia's). The context's words are big-endian.
void ClearContext(uint32_t index) {
  uint32_t va = g_array + index * kXmaContextSize;
  Wr32(va, Rd32(va) & ~(0x3u << 20 | 0x1Fu << 27));       // input buffers valid, output write offset
  Wr32(va + 4, Rd32(va + 4) & ~(1u << 31));                // output buffer valid
  Wr32(va + 8, (Rd32(va + 8) & ~0x3FFFFFFu) | kPacketHeaderBits);  // input read offset
  Wr32(va + 36, Rd32(va + 36) & ~0x1Fu);                   // output read offset
}

void CheckContexts(uint32_t bits, uint32_t k, const char* command) {
  for (uint32_t n = 0; n < 32; n++)
    if (bits >> n & 1 && 32 * k + n >= kXmaContexts)
      Fatal(nullptr, "XMA: context %u %s: no such context", 32 * k + n, command);
}

// Takes the commands written to the registers, which nothing signals, so
// the decoder polls them. A kick (decode) needs the decoder, which is not
// implemented yet. A lock stops a context's decoding (Xenia's), and with
// nothing decoded there is none to stop: it is taken and changes nothing.
void Decoder(void*) {
  for (unsigned idle = 0;; idle++) {
    bool worked = false;
    for (uint32_t k = 0; k < kCommandWords; k++) {
      if (uint32_t bits = TakeCommand(kXmaRegKick + k))
        Fatal(nullptr, "XMA: contexts %x of %u..%u kicked: XMA decoding is not implemented yet", bits, 32 * k,
              32 * k + 31);
      if (uint32_t bits = TakeCommand(kXmaRegLock + k)) {
        CheckContexts(bits, k, "locked");
        worked = true;
      }
      if (uint32_t bits = TakeCommand(kXmaRegClear + k)) {
        CheckContexts(bits, k, "cleared");
        std::lock_guard<std::mutex> g(g_lock);
        for (uint32_t n = 0; n < 32; n++)
          if (bits >> n & 1) ClearContext(32 * k + n);
        worked = true;
      }
    }
    if (worked) {
      idle = 0;
    } else if (idle < 1000) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
}

}  // namespace

// The kernel keeps the array in uncached physical memory and writes its
// physical address to the register (Xenia's); xam and titles read it there.
void StartXma() {
  uint32_t size = kXmaContexts * kXmaContextSize;
  g_array = ReservePhysical(Heap::kPhysical4k, size, 0x1000, 0, kPhysicalSize - 1, false);
  if (!g_array || !Commit(g_array, size, kPageReadWrite | kPageNoCache))
    Fatal(nullptr, "XMA: no physical memory for the context array");
  uint32_t pa;
  PhysicalAddress(g_array, pa);
  Wr32(RegisterVa(kXmaRegContextArray), Bswap(pa));
  CreateSystemThread(Decoder, nullptr);
}

uint32_t AllocateXmaContext() {
  std::lock_guard<std::mutex> g(g_lock);
  for (uint32_t k = 0; k < kXmaContexts; k++)
    if (!g_used[k]) {
      g_used[k] = true;
      return g_array + k * kXmaContextSize;
    }
  return 0;
}

bool ReleaseXmaContext(uint32_t va) {
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t offset = va - g_array, k = offset / kXmaContextSize;
  if (offset % kXmaContextSize || k >= kXmaContexts || !g_used[k]) return false;
  std::memset(Mem() + va, 0, kXmaContextSize);
  g_used[k] = false;
  return true;
}

}  // namespace recomp::xbox360
