#include "orbis/hle/ngs2_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace recomp::orbis::ngs2 {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kHalf = 0.70710678f;

float Clamp(float v, float lo, float hi) { return v < lo ? lo : std::min(v, hi); }

}  // namespace

// Fold-downs drop the LFE and send C to L and R at -3 dB and surround and
// back channels to their side at -6 dB; up-mixes place channels where they
// belong (mono to C).
void DefaultMatrix(int src, int dst, float m[kMaxChannels][kMaxChannels]) {
  std::memset(m, 0, sizeof(float) * kMaxChannels * kMaxChannels);
  if (src == dst) {
    for (int c = 0; c < src; c++) m[c][c] = 1;
    return;
  }
  // To stereo first, then from stereo.
  float st[kMaxChannels][2] = {};
  if (src == 1) {
    st[0][0] = st[0][1] = kHalf;
  } else {
    st[0][0] = st[1][1] = 1;
    if (src >= 6) {
      st[2][0] = st[2][1] = kHalf;
      st[4][0] = st[5][1] = 0.5f;
    }
    if (src == 8) st[6][0] = st[7][1] = 0.5f;
  }
  if (dst == 2) {
    for (int s = 0; s < src; s++) m[s][0] = st[s][0], m[s][1] = st[s][1];
  } else if (dst == 1) {
    for (int s = 0; s < src; s++) m[s][0] = (st[s][0] + st[s][1]) * kHalf;
    if (src == 2) m[0][0] = m[1][0] = kHalf;
  } else if (src == 1) {
    m[0][2] = 1;  // mono to C
  } else if (src == 2) {
    m[0][0] = m[1][1] = 1;
  } else if (src == 6) {  // 5.1 to 7.1
    for (int c = 0; c < 6; c++) m[c][c] = 1;
  } else {  // 7.1 to 5.1: the backs join the surrounds
    for (int c = 0; c < 6; c++) m[c][c] = 1;
    m[6][4] = m[7][5] = kHalf;
  }
}

// ---- biquad

void Biquad::Design(float rate) {
  b0 = 1, b1 = b2 = c1 = c2 = 0;
  if (type == kDirect) {
    b0 = p[0], b1 = p[1], b2 = p[2], c1 = p[3], c2 = p[4];
    return;
  }
  if (type == kOff || type > kHighPass1) return;
  float fn = std::min(p[0] / rate, 0.499f);
  if (!(fn >= 0.001f)) fn = 0.001f;
  const float w = 2 * kPi * fn, s = std::sin(w), c = std::cos(w);
  const float q = Clamp(p[1], 1e-4f, 32), alpha = s / (2 * q);
  const float A = std::sqrt(std::max(p[2], 1e-4f));
  float n0 = 1, n1 = 0, n2 = 0, d0 = 1, d1 = 0, d2 = 0;
  switch (type) {
    case kLowPass: n0 = n2 = (1 - c) / 2, n1 = 1 - c, d0 = 1 + alpha, d1 = -2 * c, d2 = 1 - alpha; break;
    case kHighPass: n0 = n2 = (1 + c) / 2, n1 = -(1 + c), d0 = 1 + alpha, d1 = -2 * c, d2 = 1 - alpha; break;
    case kBandPassQ: n0 = s / 2, n2 = -s / 2, d0 = 1 + alpha, d1 = -2 * c, d2 = 1 - alpha; break;
    case kBandPass: n0 = alpha, n2 = -alpha, d0 = 1 + alpha, d1 = -2 * c, d2 = 1 - alpha; break;
    case kNotch: n0 = n2 = 1, n1 = -2 * c, d0 = 1 + alpha, d1 = -2 * c, d2 = 1 - alpha; break;
    case kLowShelf: {
      const float k = 2 * std::sqrt(A) * alpha;
      n0 = A * ((A + 1) - (A - 1) * c + k);
      n1 = 2 * A * ((A - 1) - (A + 1) * c);
      n2 = A * ((A + 1) - (A - 1) * c - k);
      d0 = (A + 1) + (A - 1) * c + k;
      d1 = -2 * ((A - 1) + (A + 1) * c);
      d2 = (A + 1) + (A - 1) * c - k;
      break;
    }
    case kHighShelf: {
      const float k = 2 * std::sqrt(A) * alpha;
      n0 = A * ((A + 1) + (A - 1) * c + k);
      n1 = -2 * A * ((A - 1) + (A + 1) * c);
      n2 = A * ((A + 1) + (A - 1) * c - k);
      d0 = (A + 1) - (A - 1) * c + k;
      d1 = 2 * ((A - 1) - (A + 1) * c);
      d2 = (A + 1) - (A - 1) * c - k;
      break;
    }
    case kPeaking:
      n0 = 1 + alpha * A, n1 = -2 * c, n2 = 1 - alpha * A;
      d0 = 1 + alpha / A, d1 = -2 * c, d2 = 1 - alpha / A;
      break;
    case kLowPass1: n0 = n1 = s, d0 = 1 + c + s, d1 = s - c - 1; break;
    case kHighPass1: n0 = 1 + c, n1 = -(1 + c), d0 = 1 + c + s, d1 = s - (1 + c); break;
  }
  b0 = n0 / d0, b1 = n1 / d0, b2 = n2 / d0, c1 = -d1 / d0, c2 = -d2 / d0;
}

void Biquad::Reset() { std::memset(h, 0, sizeof h); }

void Biquad::Process(float* const* ch, int nch, int n, uint32_t bypass) {
  for (int c = 0; c < nch; c++) {
    float* x = ch[c];
    float* s = h[c];
    if (bypass >> c & 1) {
      std::memset(s, 0, sizeof h[c]);
      continue;
    }
    if (type == kOff || (type > kHighPass1 && type != kDirect)) {
      // Pass through; the history follows the signal for a later switch.
      if (n >= 2) s[0] = s[2] = x[n - 1], s[1] = s[3] = x[n - 2];
      continue;
    }
    float x1 = s[0], x2 = s[1], y1 = s[2], y2 = s[3];
    for (int i = 0; i < n; i++) {
      const float in = x[i], y = b0 * in + b1 * x1 + b2 * x2 + c1 * y1 + c2 * y2;
      x2 = x1, x1 = in, y2 = y1, y1 = y;
      x[i] = y;
    }
    s[0] = x1, s[1] = x2, s[2] = y1, s[3] = y2;
  }
}

// ---- envelope

namespace {

float Curve(uint32_t curve, float t, bool rising) {
  switch (curve) {
    case 1: return (std::pow(10.0f, t) - 1) / 9;
    case 2: return std::log10(1 + 9 * t);
    case 3: return (1 - std::cos(kPi * t)) / 2;
    case 4: return 0.5f + std::asin(2 * t - 1) / kPi;
    case 5: return rising ? std::sin(kPi * t / 2) : 1 - std::cos(kPi * t / 2);
    default: return t;
  }
}

}  // namespace

void Envelope::Reset() {
  phase = 1, index = 0, pos = 0;
  level = start = forward ? 0.0f : 1.0f;
}

void Envelope::Release() {
  if (phase == 2) return;
  phase = 2, index = forward, pos = 0, start = level;
}

bool Envelope::Process(float* const* ch, int nch, int n) {
  const uint32_t end = phase == 2 ? forward + release : forward;
  for (int i = 0; i < n; i++) {
    while (index < end && pos >= points[index].duration) {
      level = start = points[index].height;
      index++, pos = 0;
    }
    if (index >= end) {
      if (phase == 2 || level == 0) {
        for (int c = 0; c < nch; c++) std::memset(ch[c] + i, 0, sizeof(float) * (n - i));
        level = 0;
        return false;
      }
    } else {
      const Point& p = points[index];
      level = start + (p.height - start) *
                          Curve(p.curve, float(pos) / float(p.duration), p.height >= start);
      pos++;
    }
    for (int c = 0; c < nch; c++) ch[c][i] *= level;
  }
  return true;
}

// ---- dynamics

void Dynamics::Design(float rate) {
  const uint32_t na = uint32_t(rate * attack), nr = uint32_t(rate * release);
  att = na ? std::exp(-1.0f / float(na)) : 0;
  rel = nr ? std::exp(-1.0f / float(nr)) : 0;
}

void Dynamics::Reset() { std::fill(std::begin(g), std::end(g), 1.0f); }

void Dynamics::Process(float* const* ch, int nch, int n, float* const* side, bool soft_knee) {
  if (flags & kBypass) {
    Reset();
    return;
  }
  float* const* det = (flags & kSideChain) && side ? side : ch;
  for (int b = 0; b < n; b += 32) {
    const int m = std::min(32, n - b);
    float lv[kMaxChannels];
    for (int c = 0; c < nch; c++) {
      float v = 0;
      if (flags & kRms) {
        for (int i = 0; i < m; i++) v += det[c][b + i] * det[c][b + i];
        v = std::sqrt(v / float(m));
      } else {
        for (int i = 0; i < m; i++) v = std::max(v, std::fabs(det[c][b + i]));
      }
      lv[c] = v;
    }
    float all = 0;
    for (int c = 0; c < nch; c++)
      if (!(flags & kLinkExceptLfe) || c != kLfe) all = std::max(all, lv[c]);
    for (int c = 0; c < nch; c++) {
      const float l = (flags & kLinkExceptLfe) && c == kLfe ? lv[c] : all;
      float t = 1;
      const float ks = threshold * (1 - 0.5f * knee), e = l - ks;
      if (soft_knee && knee > 0 && e > 0 && e < threshold - ks)
        t = (ks + e * (1 + 0.5f * (ratio - 1) * e / (threshold - ks))) / l;
      else if (l > threshold)
        t = (threshold + (l - threshold) * ratio) / l;
      float gc = g[c];
      const float a = gc <= t ? rel : att;
      float* x = ch[c] + b;
      for (int i = 0; i < m; i++) {
        gc = t + a * (gc - t);
        x[i] *= gain * gc;
      }
      if (std::fabs(t - gc) < 0.01f && t + a * (gc - t) == gc) gc = t;
      g[c] = gc;
    }
  }
}

float Dynamics::Height(int nch) const {
  float h = 0;
  for (int c = 0; c < nch; c++) h = std::max(h, g[c]);
  return h;
}

// ---- distortion

void Distortion::Process(float* const* ch, int nch, int n) const {
  if (!enabled) return;
  for (int c = 0; c < nch; c++)
    for (int i = 0; i < n; i++) {
      const float x = ch[c][i];
      float y = Clamp(x * (a - b * std::fabs(x)), -clip, clip);
      if (y > -gate && y < gate) y = 0;
      ch[c][i] = dry * x + wet * y;
    }
}

// ---- peak meter

void PeakMeter::Reset(int blocks) {
  for (auto& r : ring) r.assign(blocks, Entry{});
  next = filled = 0;
}

void PeakMeter::Process(const float* const* ch, int nch, int n) {
  if (!mode || ring[0].empty()) return;
  for (int c = 0; c < nch; c++) {
    Entry e{0, 0, uint32_t(n)};
    for (int i = 0; i < n; i++) {
      e.peak = std::max(e.peak, std::fabs(ch[c][i]));
      e.sum += ch[c][i] * ch[c][i];
    }
    ring[c][next] = e;
  }
  for (int c = nch; c < kMaxChannels; c++) ring[c][next] = Entry{};
  next = (next + 1) % ring[0].size();
  filled = std::min<uint32_t>(filled + 1, ring[0].size());
}

float PeakMeter::Level(int c) const {
  if (!mode) return 0;
  float peak = 0, sum = 0;
  uint32_t count = 0;
  for (uint32_t i = 0; i < filled; i++) {
    peak = std::max(peak, ring[c][i].peak);
    sum += ring[c][i].sum, count += ring[c][i].count;
  }
  return mode == 1 ? peak : count ? std::sqrt(sum / float(count)) : 0;
}

float PeakMeter::Height(int nch) const {
  float h = 0;
  for (int c = 0; c < nch; c++) h = std::max(h, Level(c));
  return h;
}

// ---- resampler taps

const float* ResampleTaps(uint32_t phase) {
  static const auto* table = [] {
    auto* t = new float[512][4];
    for (int ph = 0; ph < 512; ph++) {
      const double frac = ph / 512.0;
      double sum = 0, w[4];
      for (int k = 0; k < 4; k++) {
        const double d = k - 1 - frac;
        const double sinc = d == 0 ? 1 : std::sin(M_PI * d) / (M_PI * d);
        w[k] = sinc * 0.5 * (1 + std::cos(M_PI * d / 2));
        sum += w[k];
      }
      for (int k = 0; k < 4; k++) t[ph][k] = float(w[k] / sum);
    }
    return t;
  }();
  return table[phase & 511];
}

// ---- LFE low-pass

void LfeFilter::Design(float rate) {
  // Analog prototype poles of a 4th-order Chebyshev I with 0.1 dB ripple,
  // as two sections (w0, Q), cutoff prewarped for the bilinear transform.
  const double eps = std::sqrt(std::pow(10.0, 0.01) - 1);
  const double asinh_e = std::asinh(1 / eps) / 4;
  const double k = std::tan(M_PI * cutoff / rate);
  for (int s = 0; s < 2; s++) {
    const double th = (2 * s + 1) * M_PI / 8;
    const double sig = std::sinh(asinh_e) * std::sin(th), om = std::cosh(asinh_e) * std::cos(th);
    const double w0 = std::sqrt(sig * sig + om * om), q = w0 / (2 * sig);
    const double w = w0 * k, d = 1 + w / q + w * w;
    b[s][0] = b[s][2] = float(w * w / d);
    b[s][1] = float(2 * w * w / d);
    a[s][0] = float(-2 * (w * w - 1) / d);
    a[s][1] = float(-(1 - w / q + w * w) / d);
  }
}

void LfeFilter::Reset() { std::memset(h, 0, sizeof h); }

void LfeFilter::Process(float* x, int n) {
  if (!enabled) {
    Reset();
    return;
  }
  for (int s = 0; s < 2; s++) {
    float x1 = h[s][0], x2 = h[s][1], y1 = h[s][2], y2 = h[s][3];
    for (int i = 0; i < n; i++) {
      const float in = x[i];
      const float y = b[s][0] * in + b[s][1] * x1 + b[s][2] * x2 + a[s][0] * y1 + a[s][1] * y2;
      x2 = x1, x1 = in, y2 = y1, y1 = y;
      x[i] = y;
    }
    h[s][0] = x1, h[s][1] = x2, h[s][2] = y1, h[s][3] = y2;
  }
}

// ---- reverb

namespace {

// Early-reflection patterns: 4 taps each {ms after the reflections
// delay, gain}.
constexpr float kPatterns[8][4][2] = {
    {{0, 0.70f}, {11.3f, -0.52f}, {27.9f, 0.41f}, {46.1f, -0.30f}},
    {{0, 0.66f}, {7.9f, 0.48f}, {19.7f, -0.39f}, {38.3f, 0.27f}},
    {{0, 0.70f}, {14.9f, -0.45f}, {33.1f, 0.36f}, {58.7f, -0.22f}},
    {{0, 0.62f}, {5.3f, -0.55f}, {16.1f, 0.42f}, {29.3f, -0.31f}},
    {{0, 0.70f}, {9.7f, 0.50f}, {24.5f, -0.37f}, {52.9f, 0.25f}},
    {{0, 0.68f}, {12.7f, -0.47f}, {30.7f, -0.33f}, {44.3f, 0.29f}},
    {{0, 0.64f}, {6.7f, 0.51f}, {21.1f, 0.38f}, {61.9f, -0.20f}},
    {{0, 0.70f}, {17.3f, -0.43f}, {36.7f, 0.34f}, {55.1f, -0.24f}},
};
// Comb lengths in ms at full density, and which 8 combs each output
// channel sums.
constexpr float kCombMs[16] = {22.3f, 23.9f, 25.1f, 26.6f, 28.3f, 29.7f, 31.1f, 32.6f,
                               34.0f, 35.3f, 36.9f, 38.2f, 39.7f, 41.0f, 42.1f, 43.4f};
constexpr uint16_t kCombMasks[kMaxChannels] = {0x5A5A, 0xA5A5, 0x3C3C, 0, 0xC3C3, 0x6969,
                                               0x9696, 0x0FF0};
constexpr float kAllpassMs[4] = {11.4f, 8.7f, 6.3f, 4.8f};

uint32_t Samples(float seconds, float rate) { return uint32_t(std::lround(seconds * rate)); }

float Millibels(int32_t mb) { return mb <= -10000 ? 0.0f : std::pow(10.0f, float(mb) / 2000); }

}  // namespace

void Reverb::Init(float rate, uint32_t size) {
  rate_ = rate;
  er_.assign(size ? 14400 : 4800, 0.0f);
  for (int i = 0; i < 16; i++) comb_[i].line.assign(uint32_t(kCombMs[i] * 48) + 2, 0.0f);
  for (auto& ch : ap_)
    for (int k = 0; k < 4; k++) ch[k].line.assign(uint32_t(kAllpassMs[k] * 48) + 2, 0.0f);
  Set(I3dl2{});
}

void Reverb::Set(const I3dl2& p) {
  p_ = p;
  std::memcpy(old_taps_, taps_, sizeof taps_);
  std::memcpy(old_tap_gain_, tap_gain_, sizeof tap_gain_);
  const uint32_t len = uint32_t(er_.size());
  const uint32_t base = std::min(Samples(p.reflections_delay, rate_), len - 1);
  const uint32_t late = std::min(Samples(p.reflections_delay + p.reverb_delay, rate_), len - 1);
  const auto& pat = kPatterns[p.pattern % 8];
  const uint32_t spread = Samples(pat[3][0] / 1000, rate_);
  float scale = 1;
  if (p.pattern < 8 && spread && late < base + spread) scale = float(late - base) / float(spread);
  for (int k = 0; k < 4; k++) {
    taps_[k] = std::min(base + Samples(scale * pat[k][0] / 1000, rate_), len - 1);
    tap_gain_[k] = k && taps_[k] == taps_[k - 1] ? 0 : pat[k][1];
  }
  taps_[4] = std::min(base + Samples(p.reverb_delay, rate_), len - 1);
  crossfade_ = !reset_;

  const float hf = std::min(p.decay_hf_ratio, 1.0f);
  const float alpha = hf >= 0.8f ? hf : 0.8f - 0.857f * (0.8f - hf);
  for (int i = 0; i < 16; i++) {
    Comb& c = comb_[i];
    uint32_t l = Samples(kCombMs[i] / (4 - 0.03f * p.density) / 1000, rate_) | 1;
    c.len = std::min<uint32_t>(std::max<uint32_t>(l, 1), uint32_t(c.line.size()));
    if (c.pos >= c.len) c.pos = 0;
    c.g = std::pow(10.0f, -3 * (float(c.len) / rate_) / p.decay_time);
    c.p = std::min(0.99978f, float(std::log(10.0) / 4) * std::log10(c.g) * (1 - 1 / (alpha * alpha)));
  }
  const float k = std::sqrt(hf);
  tone_a_ = (1 - k) / (1 + k);
  ap_g_ = 0.005f * p.diffusion;
  for (int ch = 0; ch < kMaxChannels; ch++)
    for (int s = 0; s < 4; s++) {
      Allpass& a = ap_[ch][s];
      a.len = std::max<uint32_t>(Samples(kAllpassMs[s] / 1000, rate_) - 3 * ch - s, 1);
      a.len = std::min<uint32_t>(a.len, uint32_t(a.line.size()));
      if (a.pos >= a.len) a.pos = 0;
    }
  const float room = std::min(Millibels(p.room), 1.0f);
  g_er_ = room * Millibels(p.reflections);
  g_late_ = room * Millibels(p.reverb <= -10000 ? -10000 : p.reverb - 2400);
  const float G = Millibels(p.room_hf);
  if (G < 1) {
    const float c = std::cos(2 * kPi * p.hf_reference / rate_);
    hf_a_ = (1 - G * c - std::sqrt(std::max(0.0f, 2 * G * (1 - c) - G * G * (1 - c * c)))) / (1 - G);
  } else {
    hf_a_ = 0;
  }
}

void Reverb::Reset() {
  std::fill(er_.begin(), er_.end(), 0.0f);
  er_pos_ = 0;
  for (Comb& c : comb_) {
    std::fill(c.line.begin(), c.line.end(), 0.0f);
    c.pos = 0, c.s = c.x1 = 0;
  }
  for (auto& ch : ap_)
    for (Allpass& a : ch) std::fill(a.line.begin(), a.line.end(), 0.0f), a.pos = 0;
  std::memset(hf_, 0, sizeof hf_);
  wet_prev_ = p_.wet, dry_prev_ = p_.dry;
  crossfade_ = false;
  reset_ = false;
}

void Reverb::Process(const float* const* in, int nin, float* const* out, int nout, int n) {
  if (reset_) Reset();
  const uint32_t len = uint32_t(er_.size());
  // Grains are at most 1024 samples (the system option's limit).
  static thread_local float mono[1024], er[1024], feed[1024], comb_out[16][1024];
  const float k = nin == 1 ? 1.0f : nin == 2 ? 0.7071f : nin == 6 ? 0.316f : 0.267f;
  for (int i = 0; i < n; i++) {
    float s = 0;
    for (int c = 0; c < nin; c++)
      if (c != kLfe || nin < 6) s += in[c][i];
    mono[i] = s * k;
  }
  // Early reflections and the late feed, crossfaded from the previous tap
  // set after a change.
  for (int i = 0; i < n; i++) {
    er_[(er_pos_ + i) % len] = mono[i];
    auto tap = [&](uint32_t d) { return er_[(er_pos_ + i + len - d) % len]; };
    float e = 0, f = tap(taps_[4]);
    for (int t = 0; t < 4; t++) e += tap_gain_[t] * tap(taps_[t]);
    if (crossfade_) {
      float eo = 0;
      for (int t = 0; t < 4; t++) eo += old_tap_gain_[t] * tap(old_taps_[t]);
      const float w = float(i) / float(n);
      e = eo + (e - eo) * w;
      f = tap(old_taps_[4]) + (f - tap(old_taps_[4])) * w;
    }
    er[i] = e, feed[i] = f;
  }
  er_pos_ = (er_pos_ + n) % len;
  crossfade_ = false;

  for (int ci = 0; ci < 16; ci++) {
    Comb& c = comb_[ci];
    for (int i = 0; i < n; i++) {
      const float x = (feed[i] - tone_a_ * c.x1) / (1 - tone_a_);
      c.x1 = feed[i];
      const float d = c.line[c.pos];
      c.s = (1 - c.p) * d + c.p * c.s;
      const float fb = c.g * c.s;
      c.line[c.pos] = x + fb;
      c.pos = c.pos + 1 >= c.len ? 0 : c.pos + 1;
      comb_out[ci][i] = fb;
    }
  }
  const float wet0 = wet_prev_, dry0 = dry_prev_;
  for (int ch = 0; ch < nout; ch++) {
    float* y = out[ch];
    const float* x = ch < nin ? in[ch] : nullptr;
    if (ch == kLfe && nout >= 6) {
      for (int i = 0; i < n; i++)
        y[i] = x ? (dry0 + (p_.dry - dry0) * float(i) / float(n)) * x[i] : 0;
      continue;
    }
    const uint16_t mask = kCombMasks[nout == 1 ? 0 : ch];
    for (int i = 0; i < n; i++) {
      float late = 0;
      for (int ci = 0, k2 = 0; ci < 16; ci++)
        if (mask >> ci & 1) {
          const float w = (k2 & 1) ? 1 : 1 - (100 - p_.density) / (10 * k2 + 40);
          late += w * comb_out[ci][i];
          k2++;
        }
      for (Allpass& a : ap_[ch]) {
        const float v = late + ap_g_ * a.line[a.pos];
        late = a.line[a.pos] - ap_g_ * v;
        a.line[a.pos] = v;
        a.pos = a.pos + 1 >= a.len ? 0 : a.pos + 1;
      }
      float o = g_er_ * er[i] + g_late_ * late;
      if (hf_a_ > 0) {
        float* h = hf_[ch];
        o = (1 - hf_a_) * (1 - hf_a_) * o + 2 * hf_a_ * h[0] - hf_a_ * hf_a_ * h[1];
        h[1] = h[0], h[0] = o;
      }
      const float t = float(i) / float(n);
      y[i] = (wet0 + (p_.wet - wet0) * t) * o + (x ? (dry0 + (p_.dry - dry0) * t) * x[i] : 0);
    }
  }
  wet_prev_ = p_.wet, dry_prev_ = p_.dry;
}

}  // namespace recomp::orbis::ngs2
