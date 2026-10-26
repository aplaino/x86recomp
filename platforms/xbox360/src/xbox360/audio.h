#pragma once
// The console's audio output as the kernel's render driver (the XAudio*
// exports) gives it to its clients (ARCHITECTURE.md, "Audio"): each client
// registers a callback, which the driver calls whenever the hardware is
// ready for another frame, and submits frames of 256 samples of 5.1 at
// 48 kHz. Frames play on the host's default device through SDL, which
// mixes the clients and downmixes to the device; with no device they are
// dropped and the callbacks keep their pace.

#include <cstdint>

namespace recomp::xbox360 {

// A frame: six channels (front left, right, centre, LFE, back left,
// right) one after the other, 256 big-endian floats each.
constexpr uint32_t kAudioChannels = 6, kAudioFrameSamples = 256, kAudioRate = 48000;
constexpr uint32_t kAudioClients = 8;

// The new client's index, or -1 if all are taken. The callback is called
// on the driver's thread with the address of a word holding `arg`.
int RegisterAudioClient(uint32_t callback, uint32_t arg);
// False if `index` is not a client.
bool UnregisterAudioClient(uint32_t index);
bool SubmitAudioFrame(uint32_t index, uint32_t samples);

}  // namespace recomp::xbox360
