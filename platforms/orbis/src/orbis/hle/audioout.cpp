// libSceAudioOut HLE: output ports as libSceAudioOut 11.00 numbers and
// checks them (handle 0x20000000 | type << 16 | slot; main ports take
// slots 7 down to 0, BGM 8, voice 9-12, personal 13-16, pad speaker
// 17-20, aux 23). A port holds one buffer: Output copies the game's into
// it and returns at once if it was free, else once the system mixer has
// taken the one before, every `length` samples at 48 kHz. That clock is
// the host's, so a run without an audio device keeps time. The samples
// go to the software mixer (runtime/audio), a voice per port fed by a
// queue, scaled by the port's volumes; 8 channels are folded to stereo
// (front, centre and surrounds at -3 dB, LFE dropped).

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "orbis/hle.h"
#include "runtime/audio/mixer.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kErrNotOpened = 0x80260001, kErrInvalidPointer = 0x80260004,
                   kErrInvalidPort = 0x80260003, kErrPortFull = 0x80260005,
                   kErrInvalidSize = 0x80260006, kErrInvalidFormat = 0x80260007,
                   kErrInvalidSampleFreq = 0x80260008, kErrInvalidPortType = 0x8026000A,
                   kErrAlreadyInit = 0x8026000E, kErrNotInit = 0x8026000F,
                   kErrInvalidMixLevel = 0x80260014;
constexpr int kSlots = 26;
constexpr uint32_t kVolume0dB = 32768;

struct Port {
  bool open = false;
  uint32_t type = 0, length = 0, format = 0;
  int channels = 2;
  bool is_float = false;
  float volume[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  uint32_t pad_mix_level = 0;
  Clock::time_point start;  // when the buffer in the slot starts playing
  bool queued = false;
  // The mixer's voice and its queue (the mixer owns neither).
  std::unique_ptr<recomp::audio::Voice> voice;
  recomp::audio::QueueSource* queue = nullptr;
};

std::mutex g_mu;
bool g_init = false;
Port g_ports[kSlots];
bool g_audio_up = false;

// Slots of each port type, first choice first.
std::vector<int> SlotsOf(uint32_t type) {
  switch (type) {
    case 0: return {7, 6, 5, 4, 3, 2, 1, 0};
    case 1: return {8};
    case 2: return {12, 11, 10, 9};
    case 3: return {16, 15, 14, 13};
    case 4: return {20, 19, 18, 17};
    case 0xE: return {22};
    case 0x7F: return {23};
  }
  return {};
}

Port* Find(uint32_t h, uint32_t* err) {
  const uint32_t slot = h & 0xFF;
  if (slot >= kSlots || (h & 0x3F000000) != 0x20000000) {
    *err = kErrInvalidPort;
    return nullptr;
  }
  if (!g_ports[slot].open) {
    *err = kErrNotOpened;
    return nullptr;
  }
  return &g_ports[slot];
}

// The port's samples as float stereo with its volumes, to its voice.
void Play(Port& p, recomp::gaddr buf) {
  if (!p.queue) return;
  const uint8_t* src = recomp::GPtr(buf);
  auto sample = [&](size_t i) {
    float v;
    if (p.is_float) {
      std::memcpy(&v, src + 4 * i, 4);
    } else {
      int16_t s;
      std::memcpy(&s, src + 2 * i, 2);
      v = float(s) / 32768.0f;
    }
    return v * p.volume[i % p.channels];
  };
  std::vector<float> out(size_t(p.length) * 2);
  constexpr float kHalf = 0.70710678f;
  for (size_t f = 0; f < p.length; f++) {
    const size_t i = f * p.channels;
    float l, r;
    if (p.channels == 1) {
      l = r = sample(i);
    } else if (p.channels == 2) {
      l = sample(i);
      r = sample(i + 1);
    } else {  // L, R, C, LFE, LS, RS, LB, RB
      const float c = sample(i + 2) * kHalf;
      l = sample(i) + c + (sample(i + 4) + sample(i + 6)) * kHalf;
      r = sample(i + 1) + c + (sample(i + 5) + sample(i + 7)) * kHalf;
    }
    out[2 * f] = l;
    out[2 * f + 1] = r;
  }
  p.queue->Push(out.data(), int(p.length));
}

// The port's voice in the mixer (opened at 48 kHz with the first port).
void OpenVoice(Port& p) {
  recomp::audio::Mixer& m = recomp::audio::GetMixer();
  if (!g_audio_up) {
    g_audio_up = true;
    if (m.rate() == 0 && !m.Open(48000))
      recomp::WarnF("audio: no host audio device; running silent\n");
  }
  if (m.rate() == 0) return;
  auto q = std::make_unique<recomp::audio::QueueSource>(2, 48000);
  p.queue = q.get();
  p.voice = std::make_unique<recomp::audio::Voice>();
  p.voice->src = std::move(q);
  p.voice->dsound = true;  // linear volume, centred: unity gain
  p.voice->pan = 0;
  std::lock_guard<std::mutex> l(m.mu());
  p.voice->state = recomp::audio::Voice::State::Playing;
  m.Add(p.voice.get());
}

void CloseVoice(Port& p) {
  if (!p.voice) return;
  recomp::audio::Mixer& m = recomp::audio::GetMixer();
  std::lock_guard<std::mutex> l(m.mu());
  m.Remove(p.voice.get());
}

// Puts buf (null: nothing, a wait for the slot) into the port's slot,
// waiting while it is full; the samples output.
uint64_t Output(uint32_t h, recomp::gaddr buf) {
  std::unique_lock<std::mutex> l(g_mu);
  uint32_t err;
  Port* p = Find(h, &err);
  if (!p) return err;
  const auto grain = std::chrono::nanoseconds(int64_t(p->length) * 1000000000 / 48000);
  if (p->queued) {
    // Full: the system takes the buffer in the slot at its start.
    const auto at = p->start;
    l.unlock();
    std::this_thread::sleep_until(at);
    l.lock();
  }
  if (!buf) return p->length;
  const auto now = Clock::now();
  p->start = p->queued && p->start + grain > now ? p->start + grain : now;
  p->queued = true;
  Play(*p, buf);
  return p->length;
}

}  // namespace

using recomp::hle::Arg;

ORBIS_HLE("libSceAudioOut", sceAudioOutInit, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_init) return kErrAlreadyInit;
  g_init = true;
  return 0;
}

// int sceAudioOutOpen(SceUserServiceUserId user, int type, int index,
// uint32_t length, uint32_t freq, uint32_t param): length 256-2048
// samples in steps of 256, 48 kHz; param's low byte is the format (S16
// mono, stereo, 8 channels, float mono, stereo, 8 channels, then the
// "standard" 8-channel orders of both).
ORBIS_HLE("libSceAudioOut", sceAudioOutOpen, 6) {
  const uint32_t type = uint32_t(Arg(c, 1)), length = uint32_t(Arg(c, 3)),
                 freq = uint32_t(Arg(c, 4)), param = uint32_t(Arg(c, 5));
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInit;
  if (length - 1 > 0x7FF || length & 0xFF) return kErrInvalidSize;
  if ((type != 3 && param & 0x20000) || (type != 0 && param & 0x70000000) ||
      param & 0x8FFCFF00)
    return kErrInvalidFormat;
  const std::vector<int> slots = SlotsOf(type & 0x7FFFFFFF);
  if (slots.empty()) return kErrInvalidPortType;
  if (freq != 48000) return kErrInvalidSampleFreq;
  const uint32_t format = param & 0xFF;
  static const int kChannels[8] = {1, 2, 8, 1, 2, 8, 8, 8};
  if (format > 7) return kErrInvalidFormat;
  if (format >= 6) recomp::FatalF("sceAudioOutOpen: the standard 8-channel order not implemented yet");
  if (param & ~0xFFu) recomp::FatalF("sceAudioOutOpen: param %08X not implemented yet", param);
  for (int s : slots) {
    Port& p = g_ports[s];
    if (p.open) continue;
    p = Port{};
    p.open = true;
    p.type = type & 0x7FFFFFFF;
    p.length = length;
    p.format = format;
    p.channels = kChannels[format];
    p.is_float = format >= 3;
    OpenVoice(p);
    return 0x20000000 | p.type << 16 | uint32_t(s);
  }
  return kErrPortFull;
}

ORBIS_HLE("libSceAudioOut", sceAudioOutClose, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInit;
  uint32_t err;
  Port* p = Find(uint32_t(Arg(c, 0)), &err);
  if (!p) return err;
  CloseVoice(*p);
  *p = Port{};
  return 0;
}

// int sceAudioOutOutput(int handle, const void* buf): null waits for the
// slot.
ORBIS_HLE("libSceAudioOut", sceAudioOutOutput, 2) {
  recomp::GuestSyncPoint sync;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_init) return kErrNotInit;
  }
  return Output(uint32_t(Arg(c, 0)), Arg(c, 1));
}

// int sceAudioOutOutputs(SceAudioOutOutputParam* p, uint32_t n): {handle,
// buf} pairs, output together.
ORBIS_HLE("libSceAudioOut", sceAudioOutOutputs, 2) {
  recomp::GuestSyncPoint sync;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_init) return kErrNotInit;
  }
  if (!Arg(c, 0)) return kErrInvalidPointer;
  uint64_t r = 0;
  for (uint32_t i = 0; i < uint32_t(Arg(c, 1)); i++) {
    const recomp::gaddr e = Arg(c, 0) + 16 * uint64_t(i);
    r = Output(recomp::Rd32(e), recomp::Rd64(e + 8));
    if (int32_t(r) < 0) return r;
  }
  return r;
}

// int sceAudioOutSetVolume(int handle, int flag, int* vol): flag has a bit
// per channel (L, R, C, LFE, LS, RS, LE, RE); 32768 is 0 dB.
ORBIS_HLE("libSceAudioOut", sceAudioOutSetVolume, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInit;
  if (!Arg(c, 2)) return kErrInvalidPointer;
  uint32_t err;
  Port* p = Find(uint32_t(Arg(c, 0)), &err);
  if (!p) return err;
  const uint32_t flag = uint32_t(Arg(c, 1));
  for (int ch = 0; ch < 8; ch++)
    if (flag & (1u << ch)) {
      const uint32_t v = recomp::Rd32(Arg(c, 2) + 4 * uint64_t(ch));
      if (v > kVolume0dB) recomp::FatalF("sceAudioOutSetVolume: volume %u", v);
      p->volume[ch] = float(v) / float(kVolume0dB);
    }
  return 0;
}

// The pad speaker ports' mix level (up to 32768).
ORBIS_HLE("libSceAudioOut", sceAudioOutSetMixLevelPadSpk, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInit;
  const uint32_t h = uint32_t(Arg(c, 0)), slot = h & 0xFF;
  if ((h & 0x3F000000) != 0x20000000) return kErrInvalidPort;
  if (slot - 0x11 >= 5 || (h & 0xFF0000) != 0x40000) return kErrInvalidPortType;
  if (!g_ports[slot].open) return kErrNotOpened;
  if (uint32_t(Arg(c, 1)) > 0x8000) return kErrInvalidMixLevel;
  g_ports[slot].pad_mix_level = uint32_t(Arg(c, 1));
  return 0;
}

// int sceAudioOutGetPortState(int handle, SceAudioOutPortState* s): the
// output (1: the primary output, connected), its channels and the volume
// (-1 but for the pad speaker).
ORBIS_HLE("libSceAudioOut", sceAudioOutGetPortState, 2) {
  if (!Arg(c, 1)) return kErrInvalidPointer;
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_init) return kErrNotInit;
  uint32_t err;
  Port* p = Find(uint32_t(Arg(c, 0)), &err);
  if (!p) return err;
  const recomp::gaddr s = Arg(c, 1);
  std::memset(recomp::GPtr(s), 0, 32);
  recomp::Wr16(s, 1);
  recomp::GPtr(s)[2] = 2;
  recomp::Wr16(s + 4, p->type == 4 ? uint16_t(p->pad_mix_level) : 0xFFFF);
  return 0;
}
