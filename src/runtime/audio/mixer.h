#pragma once
// Software mixer on an SDL3 audio stream: the host side of Miles' digital
// driver. Voices are linear-interpolated, gain-ramped and summed into
// stereo float. Positional voices get the "Miles Fast 2D Positional Audio"
// treatment (distance attenuation and stereo panning), which is the
// provider the game selects when EAX is off.
//
// Locking: Voice fields are shared with the audio thread and may only be
// touched with Mixer::mu() held. The mixer never calls into guest code;
// it only raises Voice::eos for the owner to act on.

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

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
};

class Mixer {
 public:
  // Opens the default playback device at `rate` Hz stereo. False if no
  // device (the game then runs without sound, as on a soundless PC).
  bool Open(int rate);
  void Close();

  std::mutex& mu() { return mu_; }
  // With mu() held:
  void Add(Voice* v);
  void Remove(Voice* v);
  void set_master(float g) { master_ = g; }
  void SetListener(const Vec3& pos, const Vec3& face, const Vec3& up);

  // Called by the audio thread.
  void Mix(float* out, int frames);

 private:
  void MixVoice(Voice& v, float* out, int frames);
  void TargetGains(const Voice& v, float* l, float* r) const;

  std::mutex mu_;
  SDL_AudioStream* stream_ = nullptr;
  int rate_ = 0;
  float master_ = 1.0f;
  Vec3 lpos_, lface_{0, 0, 1}, lup_{0, 1, 0};
  std::vector<Voice*> voices_;
};

Mixer& GetMixer();

}  // namespace recomp::audio
