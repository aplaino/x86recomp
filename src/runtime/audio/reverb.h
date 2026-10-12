#pragma once
// Environmental reverb for the software mixer, rendered by OpenAL Soft's
// EFX EAX reverb on a loopback device: OpenAL never opens an output; the
// mixer hands it each bus's send signal and adds the wet result to its own
// mix. Direct sound never goes through OpenAL.
//
// Buses are independent reverbs (an EAX listener environment, Miles'
// digital master reverb). All calls come from the audio thread with the
// mixer's lock held, so OpenAL only ever runs there.

#include <cstdint>
#include <vector>

namespace recomp::audio {

// EFX EAX reverb parameters (AL_EAXREVERB_*), in EFX units and ranges.
// Pans are in OpenAL's right-handed coordinates.
struct ReverbParams {
  float density = 1.0f, diffusion = 1.0f;
  float gain = 0.32f, gain_hf = 0.89f, gain_lf = 1.0f;
  float decay_time = 1.49f, decay_hf_ratio = 0.83f, decay_lf_ratio = 1.0f;
  float reflections_gain = 0.05f, reflections_delay = 0.007f;
  float reflections_pan[3] = {};
  float late_reverb_gain = 1.26f, late_reverb_delay = 0.011f;
  float late_reverb_pan[3] = {};
  float echo_time = 0.25f, echo_depth = 0.0f;
  float modulation_time = 0.25f, modulation_depth = 0.0f;
  float air_absorption_gain_hf = 0.994f;
  float hf_reference = 5000.0f, lf_reference = 250.0f;
  float room_rolloff_factor = 0.0f;
  bool decay_hf_limit = true;
};

class Reverb {
 public:
  static constexpr int kBuses = 2;

  ~Reverb();
  // A loopback device at `rate` Hz stereo float. False (and a warning) if
  // OpenAL Soft or its loopback/EFX extensions are missing: the mixer then
  // runs dry.
  bool Open(int rate);
  void Close();
  bool open() const { return ctx_ != nullptr; }

  // Takes effect at the next Render.
  void Set(int bus, const ReverbParams& p);

  // sends[b]: `frames` interleaved stereo frames fed to bus b. Adds the
  // wet output of all buses to `out` (interleaved stereo).
  void Render(const float* const* sends, float* out, int frames);

 private:
  struct Bus {
    uint32_t source = 0, slot = 0, effect = 0;
    std::vector<uint32_t> free_buffers;
    ReverbParams params;
    bool dirty = true;
  };
  void Apply(Bus& b);
  void Queue(Bus& b, const float* frames, int n);

  void* dev_ = nullptr;  // ALCdevice*
  void* ctx_ = nullptr;  // ALCcontext*
  int rate_ = 0;
  uint32_t mute_ = 0;  // filter: silences a bus source's direct path
  Bus buses_[kBuses];
  std::vector<float> wet_;
};

}  // namespace recomp::audio
