// winmm's waveOut (wave audio output) on the software mixer
// (runtime/audio/mixer.h): one stereo device, and no input devices. 8- and 16-bit PCM and IEEE
// float (also as WAVE_FORMAT_EXTENSIBLE) play; of more than two channels
// the front pair plays. A WAVEHDR's data is converted and queued when it
// is written, and a host thread marks the headers done (WHDR_DONE), in
// order, once playback has passed their end, signalling the device's event
// for CALLBACK_EVENT. Function, window and thread callbacks are fatal and
// named.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/audio/mixer.h"
#include "runtime/audio/source.h"
#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kNoError = 0, kBadDeviceId = 2, kInvalHandle = 5, kNoDriver = 6,
                   kBadFormat = 32, kStillPlaying = 33;
constexpr uint32_t kWaveMapper = 0xFFFFFFFF, kFormatQuery = 0x1;
constexpr uint32_t kCallbackMask = 0x70000, kCallbackNull = 0, kCallbackEvent = 0x50000;
constexpr uint32_t kWhdrDone = 0x1, kWhdrPrepared = 0x2, kWhdrInQueue = 0x10;

// WAVEHDR: lpData, dwBufferLength and dwFlags, by the guest's pointer size
// (x64: @0, @8, @24; 32-bit: @0, @4, @16).
gaddr HdrLength(gaddr h) { return h + (g_proc->x64 ? 8 : 4); }
gaddr HdrFlags(gaddr h) { return h + (g_proc->x64 ? 24 : 16); }

struct WaveOut {
  uint32_t event = 0;  // CALLBACK_EVENT's event, or 0
  int channels = 2, bits = 16, block = 4;
  bool is_float = false;
  audio::QueueSource* src = nullptr;  // owned by voice
  audio::Voice voice;
  // Written headers not yet done, with the frame count pushed through
  // their end.
  struct Pending {
    gaddr hdr;
    int64_t end;
  };
  std::deque<Pending> pending;
  int64_t pushed = 0;
};

std::mutex g_mu;
std::map<uint32_t, std::unique_ptr<WaveOut>> g_devices;  // by HWAVEOUT
uint32_t g_next = 0x7A000;

audio::Mixer& M() { return audio::GetMixer(); }

WaveOut* Device(uint64_t h) {
  auto it = g_devices.find(uint32_t(h));
  return it == g_devices.end() ? nullptr : it->second.get();
}

void Done(WaveOut& d, gaddr hdr) {
  Wr32(HdrFlags(hdr), (Rd32(HdrFlags(hdr)) | kWhdrDone) & ~kWhdrInQueue);
  if (d.event) SignalEventHandle(d.event, false);
}

// Marks headers done as playback passes them.
void Poll() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& [h, d] : g_devices) {
      const int64_t played = d->src->Consumed();
      while (!d->pending.empty() && d->pending.front().end <= played) {
        Done(*d, d->pending.front().hdr);
        d->pending.pop_front();
      }
    }
  }
}

// WAVEFORMATEX {wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec,
// nBlockAlign, wBitsPerSample, cbSize}; EXTENSIBLE's SubFormat GUID starts
// at +24 with the format tag.
bool ReadFormat(gaddr f, int* channels, int* rate, int* bits, int* block, bool* is_float) {
  uint16_t tag = Rd16(f);
  if (tag == 0xFFFE) tag = Rd16(f + 24);
  *channels = Rd16(f + 2);
  *rate = int(Rd32(f + 4));
  *block = Rd16(f + 12);
  *bits = Rd16(f + 14);
  *is_float = tag == 3;
  if (*channels < 1 || *rate <= 0 || *bits % 8 || *block != *channels * (*bits / 8)) return false;
  if (tag == 1) return *bits == 8 || *bits == 16;
  return tag == 3 && *bits == 32;
}

// Interleaved float frames (one or two channels) from the guest's samples.
std::vector<float> Convert(const WaveOut& d, gaddr data, uint32_t bytes) {
  const int frames = int(bytes / uint32_t(d.block)), out_ch = d.channels > 1 ? 2 : 1;
  std::vector<float> out(size_t(frames) * out_ch);
  const uint8_t* p = GPtr(data);
  for (int i = 0; i < frames; i++)
    for (int ch = 0; ch < out_ch; ch++) {
      const uint8_t* s = p + size_t(i) * d.block + size_t(ch) * (d.bits / 8);
      float v;
      if (d.is_float) {
        std::memcpy(&v, s, 4);
      } else if (d.bits == 16) {
        int16_t x;
        std::memcpy(&x, s, 2);
        v = x / 32768.0f;
      } else {
        v = (int(*s) - 128) / 128.0f;
      }
      out[size_t(i) * out_ch + ch] = v;
    }
  return out;
}

}  // namespace

HLE64("winmm.dll", waveOutGetNumDevs, 0) { return 1; }
// No recording devices.
HLE64("winmm.dll", waveInGetNumDevs, 0) { return 0; }

// waveOutGetDevCapsA/W(device, WAVEOUTCAPS*, size): {wMid, wPid,
// vDriverVersion, szPname[32], dwFormats, wChannels, wReserved1,
// dwSupport}; the name in the set's characters.
uint64_t GetDevCaps(Ctx& c, bool wide) {
  const uint32_t dev = uint32_t(Arg(c, 0));
  if (dev != 0 && dev != kWaveMapper) return kBadDeviceId;
  const gaddr caps = Arg(c, 1);
  const uint32_t name_bytes = wide ? 64 : 32, size = 8 + name_bytes + 12;
  std::memset(GPtr(caps), 0, std::min<uint32_t>(uint32_t(Arg(c, 2)), size));
  Wr16(caps + 0, 1);  // MM_MICROSOFT
  Wr32(caps + 4, 0x0100);
  if (wide)
    PutWStr(caps + 8, "Speakers");
  else
    PutStr(caps + 8, "Speakers");
  Wr32(caps + 8 + name_bytes, 0x000FFFFF);  // every WAVE_FORMAT_* of 11-96 kHz
  Wr16(caps + 12 + name_bytes, 2);
  Wr32(caps + 16 + name_bytes, 0x0C);       // WAVECAPS_VOLUME | LRVOLUME
  return kNoError;
}
HLE64("winmm.dll", waveOutGetDevCapsA, 3) { return GetDevCaps(c, false); }
HLE64("winmm.dll", waveOutGetDevCapsW, 3) { return GetDevCaps(c, true); }

// waveOutOpen(&hwo, device, format, callback, instance, flags)
HLE64("winmm.dll", waveOutOpen, 6) {
  const uint32_t dev = uint32_t(Arg(c, 1)), flags = uint32_t(Arg(c, 5));
  if (dev != 0 && dev != kWaveMapper) return kBadDeviceId;
  auto d = std::make_unique<WaveOut>();
  int rate;
  if (!ReadFormat(Arg(c, 2), &d->channels, &rate, &d->bits, &d->block, &d->is_float))
    return kBadFormat;
  if (flags & kFormatQuery) return kNoError;
  switch (flags & kCallbackMask) {
    case kCallbackNull: break;
    case kCallbackEvent: d->event = uint32_t(Arg(c, 3)); break;
    default: FatalF("waveOutOpen: callback type %X is not supported", flags & kCallbackMask);
  }
  if (!M().rate() && !M().Open(48000)) return kNoDriver;
  auto src = std::make_unique<audio::QueueSource>(d->channels > 1 ? 2 : 1, rate);
  d->src = src.get();
  d->voice.src = std::move(src);
  d->voice.dsound = true;  // linear volume, centre pan
  d->voice.pan = 0;
  d->voice.state = audio::Voice::State::Playing;
  {
    std::lock_guard<std::mutex> lm(M().mu());
    M().Add(&d->voice);
  }
  static std::once_flag poller;
  std::call_once(poller, [] { std::thread(Poll).detach(); });
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t h = g_next++;
  g_devices[h] = std::move(d);
  WrPtr(Arg(c, 0), h);
  return kNoError;
}

HLE64("winmm.dll", waveOutPrepareHeader, 3) {
  Wr32(HdrFlags(Arg(c, 1)), (Rd32(HdrFlags(Arg(c, 1))) | kWhdrPrepared) & ~kWhdrDone);
  return kNoError;
}
HLE64("winmm.dll", waveOutUnprepareHeader, 3) {
  const gaddr hdr = Arg(c, 1);
  if (Rd32(HdrFlags(hdr)) & kWhdrInQueue) return kStillPlaying;
  Wr32(HdrFlags(hdr), Rd32(HdrFlags(hdr)) & ~kWhdrPrepared);
  return kNoError;
}

// waveOutWrite(hwo, hdr, size): the samples are queued now.
HLE64("winmm.dll", waveOutWrite, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  WaveOut* d = Device(Arg(c, 0));
  if (!d) return kInvalHandle;
  const gaddr hdr = Arg(c, 1);
  std::vector<float> f = Convert(*d, RdPtr(hdr), Rd32(HdrLength(hdr)));
  const int frames = int(f.size()) / (d->channels > 1 ? 2 : 1);
  d->src->Push(f.data(), frames);
  d->pushed += frames;
  Wr32(HdrFlags(hdr), (Rd32(HdrFlags(hdr)) | kWhdrInQueue) & ~kWhdrDone);
  d->pending.push_back({hdr, d->pushed});
  return kNoError;
}

// waveOutReset(hwo): stops, and every queued header is done.
HLE64("winmm.dll", waveOutReset, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  WaveOut* d = Device(Arg(c, 0));
  if (!d) return kInvalHandle;
  d->src->Clear();
  d->pushed = d->src->Consumed();
  for (const auto& p : d->pending) Done(*d, p.hdr);
  d->pending.clear();
  return kNoError;
}

uint64_t SetState(Ctx& c, audio::Voice::State s) {
  std::lock_guard<std::mutex> l(g_mu);
  WaveOut* d = Device(Arg(c, 0));
  if (!d) return kInvalHandle;
  std::lock_guard<std::mutex> lm(M().mu());
  d->voice.state = s;
  return kNoError;
}
HLE64("winmm.dll", waveOutPause, 1) { return SetState(c, audio::Voice::State::Paused); }
HLE64("winmm.dll", waveOutRestart, 1) { return SetState(c, audio::Voice::State::Playing); }

// (hwo, volume): left in the low word, right in the high; one gain here.
HLE64("winmm.dll", waveOutSetVolume, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  WaveOut* d = Device(Arg(c, 0));
  if (!d) return kInvalHandle;
  const uint32_t v = uint32_t(Arg(c, 1));
  std::lock_guard<std::mutex> lm(M().mu());
  d->voice.volume = ((v & 0xFFFF) + (v >> 16)) / (2.0f * 0xFFFF);
  return kNoError;
}

// waveOutGetPosition(hwo, MMTIME*, size): MMTIME {wType, value}; samples
// and bytes are given, anything else is answered in bytes as Windows does.
HLE64("winmm.dll", waveOutGetPosition, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  WaveOut* d = Device(Arg(c, 0));
  if (!d) return kInvalHandle;
  const gaddr t = Arg(c, 1);
  const int64_t frames = d->src->Consumed();
  if (Rd32(t) == 2) {  // TIME_SAMPLES
    Wr32(t + 4, uint32_t(frames));
  } else {
    Wr32(t, 4);  // TIME_BYTES
    Wr32(t + 4, uint32_t(frames * d->block));
  }
  return kNoError;
}

HLE64("winmm.dll", waveOutClose, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  WaveOut* d = Device(Arg(c, 0));
  if (!d) return kInvalHandle;
  if (!d->pending.empty()) return kStillPlaying;
  {
    std::lock_guard<std::mutex> lm(M().mu());
    M().Remove(&d->voice);
  }
  g_devices.erase(uint32_t(Arg(c, 0)));
  return kNoError;
}

}  // namespace recomp::hle
