// xam: voice (XamVoice*), the headset's voice chat. There is no headset
// here; xam hands the headset itself to the kernel (XVoiced*), whose code
// is not part of the system update.

#include <cstring>
#include <mutex>

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kXam = "xam.xex";
constexpr uint32_t kEOutOfMemory = 0x8007000E;
constexpr uint32_t kUsers = 4, kVoiceSize = 0x38, kPacketSize = 0x10;

std::mutex g_lock;
uint32_t g_voices;  // xam's table: a voice per user

}  // namespace

// XamVoiceCreate(slot, packets, *voice): the user's voice in xam's table,
// reset: the packet count at 4, the packets (0x10 bytes each, each naming
// the voice at 8, linked through 0xC) at 8 with the first at 0xC and the
// last at 0x10, 1 at 0x14, an empty list at 0x1C, the time it was made at
// 0x28; E_OUTOFMEMORY (and no voice) when the packets cannot be
// allocated (xam's code).
HLE(kXam, 780, XamVoiceCreate) {
  uint32_t slot = Arg(c, 0), packets = Arg(c, 1), out = Arg(c, 2);
  if (slot >= kUsers) ppc::Fatal(&c, "XamVoiceCreate: slot %u is past xam's table", slot);
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_voices) {
    g_voices = SystemAlloc(kUsers * kVoiceSize);
    std::memset(Mem() + g_voices, 0, kUsers * kVoiceSize);
  }
  uint32_t v = g_voices + slot * kVoiceSize, first = Rd32(v);
  std::memset(Mem() + v, 0, kVoiceSize);
  Wr32(v, first);
  Wr32(v + 4, packets);
  Wr8(v + 0x14, 1);
  Wr32(v + 0x1C, v + 0x1C);
  Wr32(v + 0x20, v + 0x1C);
  uint32_t list = packets < 0x10000000 ? SystemAlloc(packets ? packets * kPacketSize : kPacketSize) : 0;
  if (!list) {
    Wr32(out, 0);
    return kEOutOfMemory;
  }
  Wr32(v + 8, list);
  for (uint32_t k = packets; k-- > 0;) {
    uint32_t p = list + k * kPacketSize;
    Wr32(p + 8, v);
    Wr32(p + 0xC, 0);
    if (Rd32(v + 0x10))
      Wr32(Rd32(v + 0x10) + 0xC, p);
    else
      Wr32(v + 0xC, p);
    Wr32(v + 0x10, p);
  }
  Wr64(v + 0x28, SystemTime());
  Wr64(v + 0x30, 0);
  Wr32(out, v);
  return 0;
}

// XamVoiceHeadsetPresent(voice): the kernel's XVoicedHeadsetPresent; no
// headset (chosen; console check).
HLE(kXam, 781, XamVoiceHeadsetPresent) { return 0; }

// XamVoiceSubmitPacket(voice, size, packet): the kernel's
// XVoicedSubmitPacket plays a remote talker's voice on the headset; with
// none it is dropped and succeeds (chosen; console check).
HLE(kXam, 782, XamVoiceSubmitPacket) { return 0; }

// XamVoiceClose(voice): its packets freed (and the kernel's side closed),
// the count cleared; TRUE (xam's code).
HLE(kXam, 783, XamVoiceClose) {
  uint32_t v = Arg(c, 0);
  std::lock_guard<std::mutex> g(g_lock);
  Wr64(v + 0x28, 0);
  Wr64(v + 0x30, 0);
  if (uint32_t list = Rd32(v + 8)) {
    SystemFree(list);
    Wr32(v + 8, 0);
    Wr32(v + 4, 0);
  }
  return 1;
}

}  // namespace recomp::xbox360
