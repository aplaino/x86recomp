#include "runtime/audio/mixer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "runtime/log.h"

namespace recomp::audio {

namespace {

constexpr int kChunk = 1024;  // frames decoded / mixed at a time

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 Normalize(const Vec3& v, const Vec3& fallback) {
  float l = std::sqrt(Dot(v, v));
  if (!(l > 1e-6f)) return fallback;
  return {v.x / l, v.y / l, v.z / l};
}

// Next frame of the current pass into dst[0..1] (mono is duplicated,
// positional stereo is folded to mono). False at the end of the pass.
bool Fetch(Voice& v, float* dst) {
  int ch = v.src->channels();
  if (v.buf_head_ * ch >= v.buf_.size()) {
    v.buf_.resize(size_t(kChunk) * ch);
    int got = v.src->Read(v.buf_.data(), kChunk);
    v.buf_.resize(size_t(std::max(got, 0)) * ch);
    v.buf_head_ = 0;
    if (got <= 0) return false;
  }
  const float* f = &v.buf_[v.buf_head_ * ch];
  v.buf_head_++;
  if (ch == 1) {
    dst[0] = dst[1] = f[0];
  } else if (v.positional) {
    dst[0] = dst[1] = 0.5f * (f[0] + f[1]);
  } else {
    dst[0] = f[0];
    dst[1] = f[1];
  }
  return true;
}

// Loads a_ and b_ at the current source position. False if the pass has
// no frames at all.
bool Prime(Voice& v) {
  v.primed_ = true;
  v.at_end_ = false;
  if (!Fetch(v, v.a_)) return false;
  if (!Fetch(v, v.b_)) {
    v.at_end_ = true;
    v.b_[0] = v.a_[0];
    v.b_[1] = v.a_[1];
  }
  return true;
}

void SDLCALL AudioCallback(void* ud, SDL_AudioStream* s, int additional, int) {
  auto* m = static_cast<Mixer*>(ud);
  float buf[kChunk * 2];
  int frames = additional / int(2 * sizeof(float));
  while (frames > 0) {
    int n = std::min(frames, kChunk);
    m->Mix(buf, n);
    SDL_PutAudioStreamData(s, buf, n * int(2 * sizeof(float)));
    frames -= n;
  }
}

}  // namespace

void Voice::Seek(int64_t f) {
  if (src) src->Seek(f);
  buf_.clear();
  buf_head_ = 0;
  pos_ = f;
  frac_ = 0;
  primed_ = false;
  at_end_ = false;
}

Mixer& GetMixer() {
  static Mixer m;
  return m;
}

bool Mixer::Open(int rate) {
  if (stream_) return true;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    WarnF("audio: SDL audio init failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_AudioSpec spec{SDL_AUDIO_F32, 2, rate};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                      AudioCallback, this);
  if (!stream_) {
    WarnF("audio: cannot open a playback device: %s\n", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  rate_ = rate;
  SDL_ResumeAudioStreamDevice(stream_);
  WarnF("audio: %d Hz stereo on SDL driver '%s'\n", rate,
        SDL_GetCurrentAudioDriver());
  return true;
}

void Mixer::Close() {
  if (!stream_) return;
  SDL_DestroyAudioStream(stream_);  // stops the callback first
  stream_ = nullptr;
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void Mixer::Add(Voice* v) {
  if (std::find(voices_.begin(), voices_.end(), v) == voices_.end())
    voices_.push_back(v);
}

void Mixer::Remove(Voice* v) {
  voices_.erase(std::remove(voices_.begin(), voices_.end(), v), voices_.end());
}

void Mixer::SetListener(const Vec3& pos, const Vec3& face, const Vec3& up) {
  lpos_ = pos;
  lface_ = Normalize(face, {0, 0, 1});
  lup_ = Normalize(up, {0, 1, 0});
}

// Positional voices follow the Miles Fast 2D provider (Mssfast.m3d) exactly:
// it reduces each 3D sample to a 2D volume and pan, which Mss32 turns into
// channel gains.
//  - volume: 127 / 10^((A + B) / 20) with A = -20 log10(vol / 127) and B =
//    6 dB per doubling of distance beyond min_dist (so vol * min_dist / d),
//    rounded and clamped to 0..127; 0 past max_dist; then * 71 / 100 when
//    the source is behind the listener.
//  - pan: 127 - floor(acos(dot(dir, up x face)) * 127 / pi), looked up in a
//    2049-entry table; 64 when the source is at the listener.
//  - Mss32: v = vol * master / 127; the far channel scales by a linear
//    balance table, the near one stays full; gains are out of 2048.
void Mixer::TargetGains(const Voice& v, float* l, float* r) const {
  float g = v.volume * master_;
  if (!v.positional) {
    *l = *r = g;
    return;
  }
  static const auto pan_table = [] {
    std::array<uint8_t, 2049> t{};
    for (int i = 0; i <= 2048; i++) {
      double p = 127 - std::floor(std::acos(i / 1024.0 - 1) * 127.0 * 0.31830987732601135);
      t[i] = uint8_t(std::clamp(p, 0.0, 127.0));
    }
    return t;
  }();
  Vec3 rel = Sub(v.pos, lpos_);
  float d = std::sqrt(Dot(rel, rel));
  int vol3 = int(std::lround(std::clamp(v.volume, 0.0f, 1.0f) * 127));
  int vol = 0;
  if (d <= v.max_dist && vol3 > 0) {
    float a = -20.0f * std::log10(vol3 / 127.0f);
    float min = std::max(v.min_dist, 1e-4f);
    float b = d <= min ? 0.0f : 6.0f * std::log2(d / min);
    vol = std::clamp<int>(int(std::lrint(127.0 / std::pow(10.0, (a + b) * 0.05))), 0, 127);
    if (Dot(lface_, rel) < 0) vol = vol * 71 / 100;
  }
  int pan = 64;
  if (d > 1e-4f) {
    float dot = Dot(rel, Cross(lup_, lface_)) / d;
    pan = pan_table[std::clamp<long>(std::lrint((dot + 1) * 1024), 0, 2048)];
  }
  // Mss32's balance table: 2 * pan up to 62, then full (128).
  auto bal = [](int p) { return p < 63 ? 2 * p : 128; };
  int mv = std::clamp(vol * int(std::lround(master_ * 127)) / 127, 0, 127);
  int sl = bal(127 - pan) * mv / 8, sr = bal(pan) * mv / 8;
  if (sl >= 0x7F0) sl = 0x7FF;
  if (sr >= 0x7F0) sr = 0x7FF;
  *l = sl / 2048.0f;
  *r = sr / 2048.0f;
}

void Mixer::MixVoice(Voice& v, float* out, int frames) {
  if (!v.src || (!v.primed_ && !Prime(v))) {
    v.state = Voice::State::Done;
    v.eos = true;
    return;
  }
  float tl, tr;
  TargetGains(v, &tl, &tr);
  if (!v.gains_valid_) {
    v.cur_l_ = tl;
    v.cur_r_ = tr;
    v.gains_valid_ = true;
  }
  // Ramp gain changes across the chunk so volume and pan updates do not
  // click.
  float dl = (tl - v.cur_l_) / frames, dr = (tr - v.cur_r_) / frames;
  double rate = v.rate > 0 ? v.rate : v.src->rate();
  double step = rate / rate_;
  for (int i = 0; i < frames; i++) {
    float f = float(v.frac_);
    float sl = v.a_[0] + (v.b_[0] - v.a_[0]) * f;
    float sr = v.a_[1] + (v.b_[1] - v.a_[1]) * f;
    v.cur_l_ += dl;
    v.cur_r_ += dr;
    out[2 * i] += sl * v.cur_l_;
    out[2 * i + 1] += sr * v.cur_r_;
    v.frac_ += step;
    while (v.frac_ >= 1.0) {
      v.frac_ -= 1.0;
      if (v.at_end_) {
        // a_ was the last frame of this pass.
        v.passes++;
        if (v.loops == 0 || v.passes < v.loops) {
          v.Seek(0);
          if (Prime(v)) continue;
        }
        v.state = Voice::State::Done;
        v.eos = true;
        v.cur_l_ = tl;
        v.cur_r_ = tr;
        return;
      }
      v.a_[0] = v.b_[0];
      v.a_[1] = v.b_[1];
      v.pos_++;
      if (!Fetch(v, v.b_)) {
        v.at_end_ = true;
        v.b_[0] = v.a_[0];
        v.b_[1] = v.a_[1];
      }
    }
  }
  v.cur_l_ = tl;
  v.cur_r_ = tr;
}

void Mixer::Mix(float* out, int frames) {
  std::memset(out, 0, sizeof(float) * 2 * frames);
  std::lock_guard<std::mutex> l(mu_);
  for (Voice* v : voices_)
    if (v->state == Voice::State::Playing) MixVoice(*v, out, frames);
  for (int i = 0; i < 2 * frames; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

}  // namespace recomp::audio
