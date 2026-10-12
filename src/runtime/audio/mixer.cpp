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
  Init(rate);  // the device starts paused: no Mix runs yet
  SDL_ResumeAudioStreamDevice(stream_);
  WarnF("audio: %d Hz stereo on SDL driver '%s'\n", rate,
        SDL_GetCurrentAudioDriver());
  return true;
}

void Mixer::OpenOffline(int rate) {
  if (!stream_) Init(rate);
}

void Mixer::Init(int rate) {
  rate_ = rate;
  split_ = 1.0f - std::exp(-2.0f * 3.14159265f * 5000.0f / float(rate));
  reverb_.Open(rate);
  // The reverb's ring is whole mixer fragments; ours are chunks.
  digital_.Open(rate, kChunk);
  mixed_ = 0;
}

void Mixer::Close() {
  if (stream_) {
    SDL_DestroyAudioStream(stream_);  // stops the callback first
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
  reverb_.Close();
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
  listener65_ = miles65::MakeListener(pos, face, up);
}

// Miles 6.5 voices: runtime/audio/miles65.h.
//
// Miles 6.1c: positional voices follow its Fast 2D provider (Mssfast.m3d)
// exactly: it reduces each 3D sample to a 2D volume and pan, which Mss32
// turns into channel gains. (The asymmetry at +-90 degrees is the
// original's: hard left leaves 2/128 on the right, hard right nothing on
// the left; Call of Duty's FINDINGS.md.)
//  - volume: 127 / 10^((A + B) / 20) with A = -20 log10(vol / 127) and B =
//    6 dB per doubling of distance beyond min_dist (so vol * min_dist / d),
//    rounded and clamped to 0..127; 0 past max_dist; then * 71 / 100 when
//    the source is behind the listener.
//  - pan: 127 - floor(acos(dot(dir, up x face)) * 127 / pi), looked up in a
//    2049-entry table; 64 when the source is at the listener.
//  - Mss32: v = vol * master / 127; the far channel scales by a linear
//    balance table, the near one stays full; gains are out of 2048.
void Mixer::TargetGains(const Voice& v, float* l, float* r, float* send_volume) const {
  *send_volume = v.volume;
  if (v.miles == Voice::Miles::k65) {
    miles65::Levels lv =
        v.positional
            ? miles65::Fast2D({v.pos, v.volume, v.min_dist, v.max_dist}, listener65_)
            : miles65::FromVolumePan(v.volume, v.pan);
    *l = float(miles65::DirectGain(lv.left, master_, digital_.dry(), v.dry)) / 2048.0f;
    *r = float(miles65::DirectGain(lv.right, master_, digital_.dry(), v.dry)) / 2048.0f;
    *send_volume = lv.volume;
    return;
  }
  float g = v.volume * master_;
  // Mss32's balance table: 2 * pan up to 62, then full (128).
  auto bal = [](int p) { return p < 63 ? 2 * p : 128; };
  if (!v.positional) {
    int p = int(std::lround(std::clamp(v.pan, 0.0f, 1.0f) * 127));
    *l = g * float(bal(127 - p)) / 128.0f;
    *r = g * float(bal(p)) / 128.0f;
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
  int mv = std::clamp(vol * int(std::lround(master_ * 127)) / 127, 0, 127);
  int sl = bal(127 - pan) * mv / 8, sr = bal(pan) * mv / 8;
  if (sl >= 0x7F0) sl = 0x7FF;
  if (sr >= 0x7F0) sr = 0x7FF;
  *l = sl / 2048.0f;
  *r = sr / 2048.0f;
}

void Mixer::MixVoice(Voice& v, float* out, int frames, bool reverb) {
  if (!v.src || (!v.primed_ && !Prime(v))) {
    v.state = Voice::State::Done;
    v.eos = true;
    return;
  }
  float tl, tr, send_volume;
  TargetGains(v, &tl, &tr, &send_volume);
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
  // Two-band levels and reverb sends, ramped the same way.
  constexpr int kB = Reverb::kBuses;
  bool banded = v.direct_lf != 1 || v.direct_hf != 1 || v.cur_dlf_ != 1 || v.cur_dhf_ != 1;
  bool sending[kB];
  for (int b = 0; b < kB; b++) {
    sending[b] = has_reverb() && (v.send_lf[b] > 0 || v.send_hf[b] > 0 ||
                                  v.cur_slf_[b] > 0 || v.cur_shf_[b] > 0);
    banded |= sending[b];
  }
  float ddlf = (v.direct_lf - v.cur_dlf_) / frames, ddhf = (v.direct_hf - v.cur_dhf_) / frames;
  float dslf[kB], dshf[kB];
  for (int b = 0; b < kB; b++) {
    dslf[b] = (v.send_lf[b] - v.cur_slf_[b]) / frames;
    dshf[b] = (v.send_hf[b] - v.cur_shf_[b]) / frames;
  }
  const float send_gain = v.volume * master_;
  // The digital reverb's wet scale, out of 2048 like Miles' other mixer
  // gains; a sample mixed with a non-zero one keeps the tail alive.
  float wet = 0;
  if (reverb && v.wet > 0) {
    int scale = miles65::WetGain(send_volume, master_, digital_.wet(), v.wet);
    if (scale > 0) {
      digital_.KeepAlive(now_ms());
      wet = scale / 2048.0f;
    }
  }
  for (int i = 0; i < frames; i++) {
    float f = float(v.frac_);
    float sl = v.a_[0] + (v.b_[0] - v.a_[0]) * f;
    float sr = v.a_[1] + (v.b_[1] - v.a_[1]) * f;
    if (wet != 0) wet_[i] += 0.5f * (sl + sr) * wet;
    v.cur_l_ += dl;
    v.cur_r_ += dr;
    if (banded) {
      v.low_[0] += split_ * (sl - v.low_[0]);
      v.low_[1] += split_ * (sr - v.low_[1]);
      float hl = sl - v.low_[0], hr = sr - v.low_[1];
      for (int b = 0; b < kB; b++) {
        if (!sending[b]) continue;
        v.cur_slf_[b] += dslf[b];
        v.cur_shf_[b] += dshf[b];
        float* s = &sends_[b][2 * i];
        s[0] += (v.cur_slf_[b] * v.low_[0] + v.cur_shf_[b] * hl) * send_gain;
        s[1] += (v.cur_slf_[b] * v.low_[1] + v.cur_shf_[b] * hr) * send_gain;
      }
      v.cur_dlf_ += ddlf;
      v.cur_dhf_ += ddhf;
      sl = v.cur_dlf_ * v.low_[0] + v.cur_dhf_ * hl;
      sr = v.cur_dlf_ * v.low_[1] + v.cur_dhf_ * hr;
    }
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
        SettleLevels(v);
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
  SettleLevels(v);
}

void Mixer::SettleLevels(Voice& v) {
  v.cur_dlf_ = v.direct_lf;
  v.cur_dhf_ = v.direct_hf;
  for (int b = 0; b < Reverb::kBuses; b++) {
    v.cur_slf_[b] = v.send_lf[b];
    v.cur_shf_[b] = v.send_hf[b];
  }
}

void Mixer::Mix(float* out, int frames) {
  std::memset(out, 0, sizeof(float) * 2 * frames);
  std::lock_guard<std::mutex> l(mu_);
  if (has_reverb())
    for (auto& s : sends_) s.assign(size_t(frames) * 2, 0.0f);
  bool reverb = digital_.Begin(now_ms());
  if (reverb) wet_.assign(size_t(frames), 0.0f);
  for (Voice* v : voices_)
    if (v->state == Voice::State::Playing) MixVoice(*v, out, frames, reverb);
  if (has_reverb()) {
    const float* sends[Reverb::kBuses];
    for (int b = 0; b < Reverb::kBuses; b++) sends[b] = sends_[b].data();
    reverb_.Render(sends, out, frames);
  }
  // The digital reverb works on Miles' 32-bit mix scale.
  if (reverb) {
    wet_int_.resize(size_t(frames));
    for (int i = 0; i < frames; i++)
      wet_int_[i] = int32_t(std::lrint(std::clamp(double(wet_[i]) * DigitalReverb::kScale,
                                                  -2147483648.0, 2147483647.0)));
    reverb_out_.assign(size_t(frames) * 2, 0);
    digital_.Render(wet_int_.data(), frames, reverb_out_.data(), true);
    for (int i = 0; i < 2 * frames; i++) out[i] += float(reverb_out_[i]) / DigitalReverb::kScale;
  }
  mixed_ += uint64_t(frames);
  for (int i = 0; i < 2 * frames; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

}  // namespace recomp::audio
