#pragma once
// libSceNgs2's signal processing blocks, on planar float grains (one
// buffer of n samples per channel, up to 8 channels in the order L R C
// LFE Ls Rs Lb Rb). Racks (ngs2.cpp) chain them per voice. What each block
// does and how its parameters are clamped follow libSceNgs2 11.00; its
// tables (interpolation, envelope curves, filter and reverb designs) are
// computed or chosen here, so samples differ in the last bits and the
// reverb's tail differs in detail.

#include <cstdint>
#include <vector>

namespace recomp::orbis::ngs2 {

constexpr int kMaxChannels = 8;
constexpr int kLfe = 3;

// The built-in channel matrix for src -> dst channels (each 1, 2, 6 or 8):
// m[s][d] is the gain from source channel s to destination channel d.
void DefaultMatrix(int src, int dst, float m[kMaxChannels][kMaxChannels]);

// The biquad (module 0x13): RBJ designs from {frequency Hz, Q, linear
// gain} or direct coefficients, direct form I.
struct Biquad {
  enum Type : uint32_t {
    kOff = 0, kLowPass, kHighPass, kBandPassQ, kBandPass, kNotch, kLowShelf, kHighShelf,
    kPeaking, kLowPass1, kHighPass1, kDirect = 0x20,
  };
  uint32_t type = kOff;
  float p[5] = {};
  void Design(float rate);  // after type/p or the rate changed
  void Reset();
  // In place; channels whose bit is set in bypass pass unchanged.
  void Process(float* const* ch, int nch, int n, uint32_t bypass);

  float b0 = 1, b1 = 0, b2 = 0, c1 = 0, c2 = 0;  // y = b.x + c1*y1 + c2*y2
  float h[kMaxChannels][4] = {};                 // x1, x2, y1, y2
};

// The envelope (module 0x10): forward points, then on release the release
// points; each point ramps to its height over its duration in one of six
// curve shapes.
struct Envelope {
  struct Point {
    uint32_t curve, duration;
    float height;
  };
  std::vector<Point> points;
  uint32_t forward = 0, release = 0;
  void Reset();
  void Release();  // the voice was stopped
  // Multiplies every channel by the envelope; false once it has ended (the
  // voice ends).
  bool Process(float* const* ch, int nch, int n);

  int phase = 0;  // 1 forward, 2 release
  uint32_t index = 0, pos = 0;
  float start = 1, level = 1;
};

// The compressor / limiter (module 0x12): a peak or RMS detector over
// 32-sample blocks, linked across channels, a hard or soft knee and one-pole
// attack and release.
struct Dynamics {
  enum : uint32_t { kBypass = 1, kRms = 2, kSideChain = 4, kLinkExceptLfe = 8 };
  uint32_t flags = 0;
  float threshold = 1, ratio = 0, knee = 0, attack = 0.001f, release = 0.01f, gain = 1;
  void Design(float rate);
  void Reset();
  // In place; side (or null) is the detector's input with the side chain.
  void Process(float* const* ch, int nch, int n, float* const* side, bool soft_knee);
  float Height(int nch) const;  // the largest gain applied

  float att = 0, rel = 0;
  float g[kMaxChannels] = {1, 1, 1, 1, 1, 1, 1, 1};
};

// The distortion (module 0x11): a waveshaper with clip and gate, mixed
// with the dry signal.
struct Distortion {
  bool enabled = false;
  float a = 0, b = 0, clip = 0, gate = 0, wet = 0, dry = 0;
  void Process(float* const* ch, int nch, int n) const;
};

// The peak meter (module 0x18): the peak or RMS level of each channel over
// the last `blocks` grains.
struct PeakMeter {
  uint32_t mode = 0;  // 0 off, 1 peak, 2 RMS
  void Reset(int blocks);
  void Process(const float* const* ch, int nch, int n);
  float Level(int c) const;
  float Height(int nch) const;  // the largest channel's

  struct Entry {
    float peak, sum;
    uint32_t count;
  };
  std::vector<Entry> ring[kMaxChannels];
  uint32_t next = 0, filled = 0;
};

// The sampler's resampler kernel: 4 taps around the position, 512 phases,
// a Hann-windowed sinc normalised to unity gain; tap 1 is the sample at the
// position.
const float* ResampleTaps(uint32_t phase);

// The mastering LFE low-pass (module 0x14): a 4th-order Chebyshev type I
// (0.1 dB ripple) at 80, 120 or 240 Hz, unity gain at DC.
struct LfeFilter {
  bool enabled = true;
  uint32_t cutoff = 120;
  void Design(float rate);
  void Reset();
  void Process(float* x, int n);

  float b[2][3] = {}, a[2][2] = {};
  float h[2][4] = {};
};

// The reverb (module 0x1b) for I3DL2 properties: early reflections from a
// tapped delay line, a late tail from 16 damped combs (8 per output channel)
// through 4 allpasses per channel, a room high-frequency low-pass and the
// wet/dry mix. The LFE stays out of it.
struct Reverb {
  struct I3dl2 {
    float wet = 1, dry = 0;
    int32_t room = -10000, room_hf = 0;
    uint32_t pattern = 0;
    float decay_time = 1, decay_hf_ratio = 0.5f;
    int32_t reflections = -10000;
    float reflections_delay = 0.02f;
    int32_t reverb = -10000;
    float reverb_delay = 0.04f, diffusion = 100, density = 100, hf_reference = 5000;
  };
  void Init(float rate, uint32_t size);  // size: the rack option's reverbSize
  void Set(const I3dl2& p);
  void Reset();
  void Process(const float* const* in, int nin, float* const* out, int nout, int n);

  struct Comb {
    std::vector<float> line;
    uint32_t len = 1, pos = 0;
    float g = 0, p = 0, s = 0, tone = 0, x1 = 0;
  };
  struct Allpass {
    std::vector<float> line;
    uint32_t len = 1, pos = 0;
  };
  float rate_ = 48000;
  I3dl2 p_;
  std::vector<float> er_;  // the early reflections' line
  uint32_t er_pos_ = 0;
  uint32_t taps_[5] = {}, old_taps_[5] = {};
  float tap_gain_[4] = {}, old_tap_gain_[4] = {};
  bool crossfade_ = false;
  Comb comb_[16];
  Allpass ap_[kMaxChannels][4];
  float ap_g_ = 0.5f, g_er_ = 1, g_late_ = 1;
  float hf_a_ = 0, hf_[kMaxChannels][2] = {};
  float wet_prev_ = 1, dry_prev_ = 0;
  bool reset_ = true;
  float tone_a_ = 0;
};

}  // namespace recomp::orbis::ngs2
