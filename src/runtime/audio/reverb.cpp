#include "runtime/audio/reverb.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include <algorithm>

#include "runtime/log.h"

namespace recomp::audio {

namespace {

// Frames of silence queued ahead of each bus's send signal, so OpenAL never
// runs dry between two Render calls (it reads a little past what it mixes).
constexpr int kLead = 256;

ALCdevice* Dev(void* p) { return static_cast<ALCdevice*>(p); }

}  // namespace

Reverb::~Reverb() { Close(); }

bool Reverb::Open(int rate) {
  Close();
  if (!alcIsExtensionPresent(nullptr, "ALC_SOFT_loopback")) {
    WarnF("audio: OpenAL has no ALC_SOFT_loopback; reverb is off\n");
    return false;
  }
  ALCdevice* dev = alcLoopbackOpenDeviceSOFT(nullptr);
  if (!dev) {
    WarnF("audio: cannot open an OpenAL loopback device; reverb is off\n");
    return false;
  }
  if (!alcIsRenderFormatSupportedSOFT(dev, rate, ALC_STEREO_SOFT, ALC_FLOAT_SOFT) ||
      !alcIsExtensionPresent(dev, "ALC_EXT_EFX")) {
    WarnF("audio: OpenAL loopback lacks %d Hz stereo float or EFX; reverb is off\n",
          rate);
    alcCloseDevice(dev);
    return false;
  }
  const ALCint attrs[] = {ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
                          ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT,
                          ALC_FREQUENCY, rate,
                          ALC_MAX_AUXILIARY_SENDS, 1,
                          ALC_HRTF_SOFT, ALC_FALSE,
                          0};
  ALCcontext* ctx = alcCreateContext(dev, attrs);
  if (!ctx || !alcMakeContextCurrent(ctx)) {
    WarnF("audio: cannot create an OpenAL context; reverb is off\n");
    if (ctx) alcDestroyContext(ctx);
    alcCloseDevice(dev);
    return false;
  }
  dev_ = dev;
  ctx_ = ctx;
  rate_ = rate;
  alGetError();
  alGenFilters(1, &mute_);
  alFilteri(mute_, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
  alFilterf(mute_, AL_LOWPASS_GAIN, 0.0f);
  for (Bus& b : buses_) {
    alGenSources(1, &b.source);
    alGenAuxiliaryEffectSlots(1, &b.slot);
    alGenEffects(1, &b.effect);
    alEffecti(b.effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
    alAuxiliaryEffectSloti(b.slot, AL_EFFECTSLOT_AUXILIARY_SEND_AUTO, AL_FALSE);
    // The send signal sits on the listener and reaches only its slot.
    alSourcei(b.source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(b.source, AL_POSITION, 0, 0, 0);
    alSourcef(b.source, AL_ROLLOFF_FACTOR, 0);
    alSourcef(b.source, AL_ROOM_ROLLOFF_FACTOR, 0);
    alSourcei(b.source, AL_DIRECT_FILTER, ALint(mute_));
    alSource3i(b.source, AL_AUXILIARY_SEND_FILTER, ALint(b.slot), 0, AL_FILTER_NULL);
    b.dirty = true;
  }
  if (ALenum e = alGetError(); e != AL_NO_ERROR) {
    WarnF("audio: OpenAL EFX setup failed (%04X); reverb is off\n", unsigned(e));
    Close();
    return false;
  }
  WarnF("audio: reverb on OpenAL Soft (%s)\n", alGetString(AL_VERSION));
  return true;
}

void Reverb::Close() {
  if (!ctx_) return;
  for (Bus& b : buses_) {
    alSourceStop(b.source);
    alSourcei(b.source, AL_BUFFER, 0);
    alDeleteSources(1, &b.source);
    if (!b.free_buffers.empty())
      alDeleteBuffers(ALsizei(b.free_buffers.size()), b.free_buffers.data());
    alDeleteAuxiliaryEffectSlots(1, &b.slot);
    alDeleteEffects(1, &b.effect);
    b = Bus{};
  }
  alDeleteFilters(1, &mute_);
  mute_ = 0;
  alcMakeContextCurrent(nullptr);
  alcDestroyContext(static_cast<ALCcontext*>(ctx_));
  alcCloseDevice(Dev(dev_));
  ctx_ = dev_ = nullptr;
}

void Reverb::Set(int bus, const ReverbParams& p) {
  Bus& b = buses_[bus];
  b.params = p;
  b.dirty = true;
}

void Reverb::Apply(Bus& b) {
  b.dirty = false;
  const ReverbParams& p = b.params;
  auto f = [&](ALenum param, float v, float lo, float hi) {
    alEffectf(b.effect, param, std::clamp(v, lo, hi));
  };
  auto pan = [&](ALenum param, const float v[3]) {
    float c[3] = {v[0], v[1], v[2]};
    alEffectfv(b.effect, param, c);
  };
  f(AL_EAXREVERB_DENSITY, p.density, AL_EAXREVERB_MIN_DENSITY, AL_EAXREVERB_MAX_DENSITY);
  f(AL_EAXREVERB_DIFFUSION, p.diffusion, AL_EAXREVERB_MIN_DIFFUSION,
    AL_EAXREVERB_MAX_DIFFUSION);
  f(AL_EAXREVERB_GAIN, p.gain, AL_EAXREVERB_MIN_GAIN, AL_EAXREVERB_MAX_GAIN);
  f(AL_EAXREVERB_GAINHF, p.gain_hf, AL_EAXREVERB_MIN_GAINHF, AL_EAXREVERB_MAX_GAINHF);
  f(AL_EAXREVERB_GAINLF, p.gain_lf, AL_EAXREVERB_MIN_GAINLF, AL_EAXREVERB_MAX_GAINLF);
  f(AL_EAXREVERB_DECAY_TIME, p.decay_time, AL_EAXREVERB_MIN_DECAY_TIME,
    AL_EAXREVERB_MAX_DECAY_TIME);
  f(AL_EAXREVERB_DECAY_HFRATIO, p.decay_hf_ratio, AL_EAXREVERB_MIN_DECAY_HFRATIO,
    AL_EAXREVERB_MAX_DECAY_HFRATIO);
  f(AL_EAXREVERB_DECAY_LFRATIO, p.decay_lf_ratio, AL_EAXREVERB_MIN_DECAY_LFRATIO,
    AL_EAXREVERB_MAX_DECAY_LFRATIO);
  f(AL_EAXREVERB_REFLECTIONS_GAIN, p.reflections_gain,
    AL_EAXREVERB_MIN_REFLECTIONS_GAIN, AL_EAXREVERB_MAX_REFLECTIONS_GAIN);
  f(AL_EAXREVERB_REFLECTIONS_DELAY, p.reflections_delay,
    AL_EAXREVERB_MIN_REFLECTIONS_DELAY, AL_EAXREVERB_MAX_REFLECTIONS_DELAY);
  pan(AL_EAXREVERB_REFLECTIONS_PAN, p.reflections_pan);
  f(AL_EAXREVERB_LATE_REVERB_GAIN, p.late_reverb_gain,
    AL_EAXREVERB_MIN_LATE_REVERB_GAIN, AL_EAXREVERB_MAX_LATE_REVERB_GAIN);
  f(AL_EAXREVERB_LATE_REVERB_DELAY, p.late_reverb_delay,
    AL_EAXREVERB_MIN_LATE_REVERB_DELAY, AL_EAXREVERB_MAX_LATE_REVERB_DELAY);
  pan(AL_EAXREVERB_LATE_REVERB_PAN, p.late_reverb_pan);
  f(AL_EAXREVERB_ECHO_TIME, p.echo_time, AL_EAXREVERB_MIN_ECHO_TIME,
    AL_EAXREVERB_MAX_ECHO_TIME);
  f(AL_EAXREVERB_ECHO_DEPTH, p.echo_depth, AL_EAXREVERB_MIN_ECHO_DEPTH,
    AL_EAXREVERB_MAX_ECHO_DEPTH);
  f(AL_EAXREVERB_MODULATION_TIME, p.modulation_time, AL_EAXREVERB_MIN_MODULATION_TIME,
    AL_EAXREVERB_MAX_MODULATION_TIME);
  f(AL_EAXREVERB_MODULATION_DEPTH, p.modulation_depth,
    AL_EAXREVERB_MIN_MODULATION_DEPTH, AL_EAXREVERB_MAX_MODULATION_DEPTH);
  f(AL_EAXREVERB_AIR_ABSORPTION_GAINHF, p.air_absorption_gain_hf,
    AL_EAXREVERB_MIN_AIR_ABSORPTION_GAINHF, AL_EAXREVERB_MAX_AIR_ABSORPTION_GAINHF);
  f(AL_EAXREVERB_HFREFERENCE, p.hf_reference, AL_EAXREVERB_MIN_HFREFERENCE,
    AL_EAXREVERB_MAX_HFREFERENCE);
  f(AL_EAXREVERB_LFREFERENCE, p.lf_reference, AL_EAXREVERB_MIN_LFREFERENCE,
    AL_EAXREVERB_MAX_LFREFERENCE);
  f(AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, p.room_rolloff_factor,
    AL_EAXREVERB_MIN_ROOM_ROLLOFF_FACTOR, AL_EAXREVERB_MAX_ROOM_ROLLOFF_FACTOR);
  alEffecti(b.effect, AL_EAXREVERB_DECAY_HFLIMIT, p.decay_hf_limit ? AL_TRUE : AL_FALSE);
  // A slot copies its effect when attached; re-attach to apply changes.
  alAuxiliaryEffectSloti(b.slot, AL_EFFECTSLOT_EFFECT, ALint(b.effect));
}

void Reverb::Queue(Bus& b, const float* frames, int n) {
  ALuint buf;
  if (!b.free_buffers.empty()) {
    buf = b.free_buffers.back();
    b.free_buffers.pop_back();
  } else {
    alGenBuffers(1, &buf);
  }
  alBufferData(buf, AL_FORMAT_STEREO_FLOAT32, frames, ALsizei(n * 2 * sizeof(float)),
               rate_);
  alSourceQueueBuffers(b.source, 1, &buf);
}

void Reverb::Render(const float* const* sends, float* out, int frames) {
  if (!ctx_ || frames <= 0) return;
  for (int i = 0; i < kBuses; i++) {
    Bus& b = buses_[i];
    if (b.dirty) Apply(b);
    ALint done = 0;
    alGetSourcei(b.source, AL_BUFFERS_PROCESSED, &done);
    while (done-- > 0) {
      ALuint buf;
      alSourceUnqueueBuffers(b.source, 1, &buf);
      b.free_buffers.push_back(buf);
    }
    ALint state = AL_STOPPED;
    alGetSourcei(b.source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING) {
      static const std::vector<float> silence(size_t(kLead) * 2);
      Queue(b, silence.data(), kLead);
    }
    Queue(b, sends[i], frames);
    if (state != AL_PLAYING) alSourcePlay(b.source);
  }
  wet_.resize(size_t(frames) * 2);
  alcRenderSamplesSOFT(Dev(dev_), wet_.data(), frames);
  for (size_t i = 0; i < wet_.size(); i++) out[i] += wet_[i];
}

}  // namespace recomp::audio
