// xboxkrnl: the audio render driver (XAudio*).
//
// The calls' meaning is Xenia's (the kernel's code is not available);
// docs/xboxkrnl.md, "Audio".

#include <bit>

#include "ppc/fatal.h"
#include "xbox360/audio.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";
constexpr uint32_t kEInvalidArg = 0x80070057;
// A driver handle: "AU" and the client's index.
constexpr uint32_t kDriverTag = 0x41550000;

uint32_t ClientOf(ppc::Ctx& c, uint32_t driver) {
  if ((driver & 0xFFFF0000) != kDriverTag) ppc::Fatal(&c, "XAudio: %x is not a render driver", driver);
  return driver & 0xFFFF;
}

}  // namespace

// XAudioRegisterRenderDriverClient(*{callback, argument}, *driver).
HLE(kKrnl, 499, XAudioRegisterRenderDriverClient) {
  uint32_t registration = Arg(c, 0), out = Arg(c, 1);
  if (!registration || !Rd32(registration)) return kEInvalidArg;
  int index = RegisterAudioClient(Rd32(registration), Rd32(registration + 4));
  if (index < 0) ppc::Fatal(&c, "XAudioRegisterRenderDriverClient: more than %u clients", kAudioClients);
  Wr32(out, kDriverTag | uint32_t(index));
  return 0;
}

HLE(kKrnl, 500, XAudioUnregisterRenderDriverClient) {
  if (!UnregisterAudioClient(ClientOf(c, Arg(c, 0))))
    ppc::Fatal(&c, "XAudioUnregisterRenderDriverClient(%x): not registered", Arg(c, 0));
  return 0;
}

// XAudioSubmitRenderDriverFrame(driver, samples).
HLE(kKrnl, 501, XAudioSubmitRenderDriverFrame) {
  if (!SubmitAudioFrame(ClientOf(c, Arg(c, 0)), Arg(c, 1)))
    ppc::Fatal(&c, "XAudioSubmitRenderDriverFrame(%x): not registered", Arg(c, 0));
  return 0;
}

// The system's per-category volumes (the dashboard's settings) never
// change here: no category's change bit, full volume.
HLE(kKrnl, 503, XAudioGetVoiceCategoryVolumeChangeMask) {
  ClientOf(c, Arg(c, 0));
  Wr32(Arg(c, 1), 0);
  return 0;
}
HLE(kKrnl, 504, XAudioGetVoiceCategoryVolume) {
  Wr32(Arg(c, 1), std::bit_cast<uint32_t>(1.0f));
  return 0;
}

}  // namespace recomp::xbox360
