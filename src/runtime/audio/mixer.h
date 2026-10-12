#pragma once
// Software mixer on an SDL3 audio stream: the host side of Miles' digital
// driver. Voices are linear-interpolated, gain-ramped and summed into
// stereo float. Positional voices get the "Miles Fast 2D Positional Audio"
// treatment (distance attenuation and stereo panning), which Miles' EAX
// providers also get here. Voices may also feed the EAX reverb bus, with
// two-band levels on the direct path and the send (see Voice::direct_lf),
// and the Miles-compatible digital master reverb (Voice::wet,
// DigitalReverb).
//
// Locking: Voice fields are shared with the audio thread and may only be
// touched with Mixer::mu() held. The mixer never calls into guest code;
// it only raises Voice::eos for the owner to act on.

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/audio/digital_reverb.h"
#include "runtime/audio/reverb.h"
#include "runtime/audio/source.h"

struct SDL_AudioStream;

namespace recomp::audio {

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

struct Voice {
  enum class State { Idle, Playing, Paused, Done };

  std::unique_ptr<Source> src;
  State state = State::Idle;
  float volume = 1.0f;  // linear amplitude
  float pan = 0.5f;     // non-positional voices: 0 left, 0.5 centre, 1 right
  float rate = 0;       // source frames per second; 0 = the source's own
  int loops = 1;        // total passes; 0 = forever
  int passes = 0;       // completed passes

  // Positional voices (Miles 3D samples). Coordinates are Miles' (left-
  // handed: x right, y up, z forward).
  bool positional = false;
  Vec3 pos;
  float min_dist = 1.0f, max_dist = 1e30f;

  // Linear levels below and above a 5 kHz split (EAX's HF reference) for
  // the direct path and the send to each reverb bus. Sends are not panned
  // or distance-attenuated; they scale with volume and the master. A voice
  // with flat direct levels and no sends takes the plain path.
  float direct_lf = 1.0f, direct_hf = 1.0f;
  float send_lf[Reverb::kBuses] = {}, send_hf[Reverb::kBuses] = {};
  // Send to the digital master reverb (the sample's wet level): mono,
  // scaled by the reverb's wet level, volume and the master as in Miles.
  float wet = 0.0f;

  // Set by the mixer when the last pass ends (state becomes Done). The
  // owner clears it after delivering its end-of-sample callback.
  bool eos = false;

  // Repositions playback to source frame f (also rewinds loop counting).
  void Seek(int64_t f);
  // Source frame playing now.
  int64_t Position() const { return pos_; }

  // ---- mixer-private state ----
  int64_t pos_ = 0;   // absolute source frame of a_
  double frac_ = 0;   // 0..1 between a_ and b_
  bool primed_ = false;
  bool at_end_ = false;  // b_ is past the end of this pass
  float a_[2] = {}, b_[2] = {};
  std::vector<float> buf_;  // decoded frames not yet consumed
  size_t buf_head_ = 0;     // in frames
  float cur_l_ = 0, cur_r_ = 0;
  bool gains_valid_ = false;
  float low_[2] = {};  // one-pole low band of each channel
  float cur_dlf_ = 1, cur_dhf_ = 1;
  float cur_slf_[Reverb::kBuses] = {}, cur_shf_[Reverb::kBuses] = {};
};

class Mixer {
 public:
  // Opens the default playback device at `rate` Hz stereo. False if no
  // device (the game then runs without sound, as on a soundless PC).
  bool Open(int rate);
  // No device: the caller pulls the mix with Mix() (offline rendering).
  void OpenOffline(int rate);
  void Close();

  std::mutex& mu() { return mu_; }
  // With mu() held:
  void Add(Voice* v);
  void Remove(Voice* v);
  void set_master(float g) { master_ = g; }
  // False when OpenAL Soft is unavailable: sends are then silent.
  bool has_reverb() const { return reverb_.open(); }
  void SetReverb(int bus, const ReverbParams& p) { reverb_.Set(bus, p); }
  void SetListener(const Vec3& pos, const Vec3& face, const Vec3& up);
  DigitalReverb& digital_reverb() { return digital_; }
  // The millisecond clock of the reverb's tail: the time mixed so far.
  uint32_t now_ms() const { return rate_ ? uint32_t(mixed_ * 1000 / uint64_t(rate_)) : 0; }

  // Called by the audio thread.
  void Mix(float* out, int frames);

 private:
  void Init(int rate);
  void MixVoice(Voice& v, float* out, int frames, bool reverb);
  static void SettleLevels(Voice& v);
  void TargetGains(const Voice& v, float* l, float* r) const;

  std::mutex mu_;
  SDL_AudioStream* stream_ = nullptr;
  int rate_ = 0;
  float master_ = 1.0f;
  float split_ = 0;  // one-pole coefficient of the 5 kHz band split
  Reverb reverb_;
  std::vector<float> sends_[Reverb::kBuses];
  DigitalReverb digital_;
  std::vector<float> wet_;  // mono sends to digital_
  std::vector<int32_t> wet_int_, reverb_out_;
  uint64_t mixed_ = 0;  // frames
  Vec3 lpos_, lface_{0, 0, 1}, lup_{0, 1, 0};
  std::vector<Voice*> voices_;
};

Mixer& GetMixer();

}  // namespace recomp::audio
