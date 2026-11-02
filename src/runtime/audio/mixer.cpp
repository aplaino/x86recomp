#include "runtime/audio/mixer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/log.h"
#include "runtime/platform.h"

namespace recomp::audio {

namespace {

constexpr int kChunk = 1024;  // frames decoded / mixed at a time
constexpr double kPi = 3.14159265358979323846;

// The low-pass EAX hardware puts on a path whose 5 kHz level is below its
// LF level, fitted to the Audigy 2 ZS's responses to obstruction,
// occlusion, exclusion and air absorption (within about 0.5 dB from
// 250 Hz to 16 kHz): two poles placed like an analog low-pass's (radius
// e^(-w/2Q), angle w sqrt(1 - 1/4Q^2) for a cutoff of w radians per
// sample), no zeros, unity at DC. Its Q rises as the cutoff falls, 0.6
// (3 kHz / fc)^0.39 below 3 kHz, and the cutoff stops at 124 Hz (so
// -10000 mB sounds like about -6800). For an HF level of G dB the
// response at 5 kHz is 0.94 G.
constexpr double kLowPassMinCutoff = 124.0, kLowPassAt5k = 0.94;
constexpr int kLowPassSteps = 1000;  // of 10 mB

std::array<float, 2> PolePair(double fc, double rate) {
  double q = 0.6 * std::pow(std::max(1.0, 3000.0 / fc), 0.39);
  double w = 2 * kPi * fc / rate;
  double r = std::exp(-w / (2 * q));
  return {float(-2 * r * std::cos(w * std::sqrt(1 - 1 / (4 * q * q)))), float(r * r)};
}

double GainAt(const std::array<float, 2>& a, double f, double rate) {
  double w = 2 * kPi * f / rate;
  double re = 1 + a[0] * std::cos(w) + a[1] * std::cos(2 * w);
  double im = a[0] * std::sin(w) + a[1] * std::sin(2 * w);
  return (1 + a[0] + a[1]) / std::sqrt(re * re + im * im);
}

// The pole pair for each 10 mB step of HF cut (step 0: none). The cutoff
// is searched up to 0.4 x the rate, where the response at 5 kHz has
// risen to within 0.1 dB of flat.
std::vector<std::array<float, 2>> LowPassTable(int rate) {
  std::vector<std::array<float, 2>> t(kLowPassSteps + 1, {0.0f, 0.0f});
  for (int i = 1; i <= kLowPassSteps; i++) {
    double want = std::pow(10.0, kLowPassAt5k * (-10.0 * i / 100.0) / 20.0);
    double lo = std::log2(kLowPassMinCutoff), hi = std::log2(0.4 * rate);
    if (GainAt(PolePair(kLowPassMinCutoff, rate), 5000, rate) >= want) {
      t[i] = PolePair(kLowPassMinCutoff, rate);
      continue;
    }
    for (int k = 0; k < 50; k++) {
      double mid = (lo + hi) / 2;
      (GainAt(PolePair(std::exp2(mid), rate), 5000, rate) < want ? lo : hi) = mid;
    }
    t[i] = PolePair(std::exp2((lo + hi) / 2), rate);
  }
  return t;
}

// The low-pass EAX hardware puts on a reverb send whose 5 kHz level is
// below its LF level: broader than the direct path's. Fitted to the
// Audigy 2 ZS's reverb spectra under room HF, occlusion and exclusion
// (within about 1 dB from 250 Hz to 8 kHz): one real pole takes the
// first kSendPoleDb of the cut at 5 kHz, a second the rest.
constexpr double kSendPoleDb = 15.0;

// The one-pole low-pass pole p (1 - p z^-1) with gain t at 5 kHz.
double OnePole(double t, double rate) {
  if (!(t < 1.0)) return 0.0;
  double w = 2 * kPi * 5000.0 / rate, c = std::cos(w), t2 = t * t;
  // |(1 - p) / (1 - p e^-jw)|^2 = t^2, the root in [0, 1).
  double b = 1.0 - t2 * c, q = 1.0 - t2;
  return (b - std::sqrt(std::max(b * b - q * q, 0.0))) / q;
}

std::vector<std::array<float, 2>> SendLowPassTable(int rate) {
  std::vector<std::array<float, 2>> t(kLowPassSteps + 1, {0.0f, 0.0f});
  for (int i = 1; i <= kLowPassSteps; i++) {
    double cut = i / 10.0;  // dB at 5 kHz
    double p1 = OnePole(std::pow(10.0, -std::min(cut, kSendPoleDb) / 20.0), rate);
    double p2 = OnePole(std::pow(10.0, -std::max(cut - kSendPoleDb, 0.0) / 20.0), rate);
    t[i] = {float(-(p1 + p2)), float(p1 * p2)};
  }
  return t;
}

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
    const int chunk = v.read_chunk > 0 ? std::min(v.read_chunk, kChunk) : kChunk;
    v.buf_.resize(size_t(chunk) * ch);
    int got = v.src->Read(v.buf_.data(), chunk);
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

// RECOMP_AUDIO_DUMP=dir: what the device is given, as raw float stereo
// at the mixer's rate, to dir/out.f32 (finding clicks).
FILE* DumpFile(const char* name) {
  const char* dir = std::getenv("RECOMP_AUDIO_DUMP");
  if (!dir || !*dir) return nullptr;
  return std::fopen((std::string(dir) + "/" + name).c_str(), "wb");
}

// The mixer thread's period: 10 ms, the length of DirectSound's fragment,
// which games' rings and notifications are built around (Wine's
// DS_TIME_DEL is the same 10 ms).
constexpr double kPeriodMs = 10.0;

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// <ID>_AUDIO_LOG=1 for the device callback when the mixer thread mixes:
// every 5 s, what the device asked for, the underruns (frames of silence
// it played because the ring was short, heard as clicks), and the longest
// gap between requests next to the longest copy.
void LogDevice(int device_rate, int frames, double copy_ms, double gap_ms, int silent) {
  struct Stats {
    uint64_t calls = 0, frames = 0, min_frames = ~0ull, late = 0;
    uint64_t underruns = 0, silent = 0, last_ms = 0;
    double max_copy = 0, max_gap = 0;
  };
  static Stats s;
  s.calls++;
  s.frames += uint64_t(frames);
  s.min_frames = std::min(s.min_frames, uint64_t(frames));
  s.max_copy = std::max(s.max_copy, copy_ms);
  s.max_gap = std::max(s.max_gap, gap_ms);
  if (silent > 0) {
    s.underruns++;
    s.silent += uint64_t(silent);
  }
  if (device_rate && copy_ms > 1000.0 * frames / device_rate) s.late++;
  const uint64_t now = NowMs();
  if (now - s.last_ms < 5000) return;
  if (s.last_ms)
    std::fprintf(stderr,
                 "audio: %llu device requests (%llu-%llu frames), %llu late, %llu underruns "
                 "(%llu frames of silence); longest copy %.2f ms, longest gap between "
                 "requests %.1f ms\n",
                 (unsigned long long)s.calls, (unsigned long long)s.min_frames,
                 (unsigned long long)(s.calls ? s.frames / s.calls : 0),
                 (unsigned long long)s.late, (unsigned long long)s.underruns,
                 (unsigned long long)s.silent, s.max_copy, s.max_gap);
  s = Stats{};
  s.last_ms = now;
}

// <ID>_AUDIO_LOG=1 for the mixer thread: every 5 s, its periods, how many
// of them had nothing to mix (the device had taken nothing since the last
// one), the longest wait for the mixer's lock and the longest mix, and
// how far the ring's fill moved.
void LogMixer(bool mixed, double wait_ms, double mix_ms, double period_ms, int fill,
              int capacity) {
  struct Stats {
    uint64_t periods = 0, idle = 0, last_ms = 0;
    double max_wait = 0, max_mix = 0, max_period = 0;
    int fill_min = 1 << 30, fill_max = 0;
  };
  static Stats s;
  s.periods++;
  if (!mixed) s.idle++;
  s.max_wait = std::max(s.max_wait, wait_ms);
  s.max_mix = std::max(s.max_mix, mix_ms);
  s.max_period = std::max(s.max_period, period_ms);
  s.fill_min = std::min(s.fill_min, fill);
  s.fill_max = std::max(s.fill_max, fill);
  const uint64_t now = NowMs();
  if (now - s.last_ms < 5000) return;
  if (s.last_ms)
    std::fprintf(stderr,
                 "audio: mixer thread: %llu periods (%llu mixed nothing), ring %d-%d of %d "
                 "frames; longest lock wait %.2f ms, longest mix %.2f ms, longest period "
                 "%.1f ms\n",
                 (unsigned long long)s.periods, (unsigned long long)s.idle, s.fill_min,
                 s.fill_max, capacity, s.max_wait, s.max_mix, s.max_period);
  s = Stats{};
  s.last_ms = now;
}

void SDLCALL AudioCallback(void* ud, SDL_AudioStream* s, int additional, int) {
  static FILE* dump = DumpFile("out.f32");
  // RECOMP_AUDIO_MUTE=1: the device plays silence (tests on the real
  // device path, quietly).
  static const bool mute = [] {
    const char* e = std::getenv("RECOMP_AUDIO_MUTE");
    return e && *e == '1';
  }();
  // RECOMP_AUDIO_TONE=1: a 440 Hz tone instead of the mix (is the
  // output path clean?).
  static const bool tone = [] {
    const char* e = std::getenv("RECOMP_AUDIO_TONE");
    return e && *e == '1';
  }();
  static std::chrono::steady_clock::time_point prev;
  auto* m = static_cast<Mixer*>(ud);
  float buf[kChunk * 2];
  const int total = additional / int(2 * sizeof(float));
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  const double gap = prev.time_since_epoch().count()
                         ? std::chrono::duration<double, std::milli>(t0 - prev).count()
                         : 0;
  prev = t0;
  double wait = 0;
  // The n frames now in buf as the device is given them, at the mixer's
  // rate (the dump, the mute and the tone test all see that).
  auto shape = [&](int n) {
    if (dump) std::fwrite(buf, sizeof(float), size_t(n) * 2, dump);
    if (mute) std::memset(buf, 0, sizeof(float) * 2 * size_t(n));
    if (tone) {
      static double phase = 0;
      for (int i = 0; i < n; i++) {
        buf[2 * i] = buf[2 * i + 1] = float(0.2 * std::sin(phase));
        phase += 2 * kPi * 440.0 / m->rate();
        if (phase > 2 * kPi) phase -= 2 * kPi;
      }
    }
  };
  // n frames of the mix into buf (the mixer thread's way, or the old one
  // where this callback mixes).
  auto mix = [&](int n) {
    if (m->log_timing) {
      const auto w = Clock::now();
      m->mu().lock();
      m->mu().unlock();
      wait = std::max(wait, std::chrono::duration<double, std::milli>(Clock::now() - w).count());
    }
    m->Mix(buf, n);
    shape(n);
  };
  // n frames from the ring into buf, silence for the ones it does not
  // hold: what the device plays then is a click.
  int silent = 0;
  auto take = [&](int n) {
    const int got = m->Take(buf, n);
    if (got < n) silent += n - got;
    shape(n);
  };
  const int bpf = int(2 * sizeof(float));  // a stereo float frame
  if (m->mix_thread()) {
    // The mixer thread has mixed it: take the frames from the ring and
    // let SDL convert them. No lock here, and no guest memory.
    if (SDL_AudioStream* conv = m->conv()) {
      for (int left; (left = additional - SDL_GetAudioStreamAvailable(conv)) > 0;) {
        // Frames at the mixer's rate for what the device is still owed
        // (one more: a conversion rounds down).
        const int64_t need = (int64_t(left) * m->rate() + int64_t(m->device_rate()) * bpf - 1) /
                             (int64_t(m->device_rate()) * bpf);
        const int n = int(std::min<int64_t>(need + 1, kChunk));
        take(n);
        SDL_PutAudioStreamData(conv, buf, n * bpf);
      }
      for (int left = additional; left > 0;) {
        const int got = SDL_GetAudioStreamData(conv, buf, std::min(left, int(sizeof buf)));
        if (got <= 0) break;
        SDL_PutAudioStreamData(s, buf, got);
        left -= got;
      }
    } else {
      for (int frames = total; frames > 0;) {
        const int n = std::min(frames, kChunk);
        take(n);
        SDL_PutAudioStreamData(s, buf, n * bpf);
        frames -= n;
      }
    }
  } else if (SDL_AudioStream* conv = m->conv()) {
    // Mixed at our rate, converted to the device's by SDL (band-limited).
    while (SDL_GetAudioStreamAvailable(conv) < additional) {
      mix(kChunk);
      SDL_PutAudioStreamData(conv, buf, int(sizeof buf));
    }
    for (int left = additional; left > 0;) {
      const int got = SDL_GetAudioStreamData(conv, buf, std::min(left, int(sizeof buf)));
      if (got <= 0) break;
      SDL_PutAudioStreamData(s, buf, got);
      left -= got;
    }
  } else {
    for (int frames = total; frames > 0;) {
      const int n = std::min(frames, kChunk);
      mix(n);
      SDL_PutAudioStreamData(s, buf, n * bpf);
      frames -= n;
    }
  }
  if (!m->log_timing || total <= 0) return;
  const double copy = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  if (m->mix_thread())
    LogDevice(m->device_rate(), total, copy, gap, silent);
  else
    m->Timing(wait, copy, gap, total);
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
  // The device runs at its own rate (asked for another, macOS converts and
  // the requests come unevenly: gaps of up to 70 ms on a 23 ms buffer);
  // the mix is converted to it here.
  SDL_AudioSpec dev{};
  if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, nullptr) || dev.freq <= 0)
    dev.freq = rate;
  device_rate_ = dev.freq;
  SDL_AudioSpec spec{SDL_AUDIO_F32, 2, device_rate_};
  if (device_rate_ != rate) {
    SDL_AudioSpec mix{SDL_AUDIO_F32, 2, rate};
    conv_ = SDL_CreateAudioStream(&mix, &spec);
    if (!conv_) {
      WarnF("audio: no %d to %d Hz conversion: %s\n", rate, device_rate_, SDL_GetError());
      spec.freq = device_rate_ = rate;
    }
  }
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                      AudioCallback, this);
  if (!stream_) {
    WarnF("audio: cannot open a playback device: %s\n", SDL_GetError());
    if (conv_) SDL_DestroyAudioStream(conv_);
    conv_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  Init(rate);  // the device starts paused: no Mix runs yet
  // What the device asks for in one request: its buffer, in the mixer's
  // frames (the callback is handed a bufferful at a time). The ring is
  // sized from this, not from a constant.
  int dev_frames = 0;
  SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &dev, &dev_frames);
  if (dev_frames <= 0) dev_frames = kChunk;  // SDL would not say: its usual buffer
  request_frames_ = std::max((dev_frames * rate + device_rate_ - 1) / device_rate_, 1);
  // A thread of our own mixes from here on, into a ring the callback
  // copies out; RECOMP_AUDIO_MIXTHREAD=0 keeps the callback mixing.
  static const bool own_thread = [] {
    const char* e = std::getenv("RECOMP_AUDIO_MIXTHREAD");
    return !(e && *e == '0');
  }();
  if (own_thread) StartThread();
  SDL_ResumeAudioStreamDevice(stream_);
  WarnF("audio: %d Hz stereo on SDL driver '%s' (device %d Hz, %d channels, %d-frame buffer)\n",
        rate, SDL_GetCurrentAudioDriver(), dev.freq, dev.channels, dev_frames);
  if (own_thread)
    WarnF("audio: mixing on a thread of its own, %d-frame periods (%d ms), a %d-frame ring "
          "(%d-frame device requests)\n",
          period_, int(std::lround(period_ * 1000.0 / rate_)), ring_.Capacity(), request_frames_);
  return true;
}

void Mixer::OpenOffline(int rate) {
  if (!stream_) Init(rate);
}

void Mixer::Init(int rate) {
  rate_ = rate;
  low_pass_ = LowPassTable(rate);
  send_low_pass_ = SendLowPassTable(rate);
  reverb_.Open(rate);
  // The reverb's ring is whole mixer fragments; ours are chunks.
  digital_.Open(rate, kChunk);
  mixed_ = 0;
}

void Mixer::Close() {
  // The mixer thread first: it fills the ring the callback drains (both
  // lock-free, so it may stop while the device still asks), then the
  // streams stop the callback.
  StopThread();
  if (stream_) {
    SDL_DestroyAudioStream(stream_);  // stops the callback first
    stream_ = nullptr;
    if (conv_) SDL_DestroyAudioStream(conv_);
    conv_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
  thread_mode_.store(false, std::memory_order_relaxed);
  reverb_.Close();
}

// A period is 10 ms of the mixer's rate. The ring holds two device
// requests and a period at its fill: two requests of slack are what the
// old in-callback mixing had (it ran on the device's own thread), and
// they let the thread come back from a stall of 40 ms without silence,
// mixing several periods in the one wake it gets.
void Mixer::StartThread() {
  period_ = std::max(int(std::lround(rate_ * kPeriodMs / 1000.0)), 1);
  period_target_ = 2 * request_frames_ + period_;
  int capacity = 1;
  while (capacity < period_target_ + period_) capacity *= 2;
  ring_.Reset(capacity);
  // A ring of silence at the fill to start with: the device's first
  // requests can come before the thread's first periods.
  mix_buf_.assign(size_t(2) * size_t(period_target_), 0.0f);
  ring_.Write(mix_buf_.data(), period_target_);
  stop_.store(false, std::memory_order_relaxed);
  thread_mode_.store(true, std::memory_order_relaxed);
  try {
    thread_ = std::thread(&Mixer::ThreadMain, this);
  } catch (const std::system_error& e) {
    // No thread to be had: the callback mixes, the way it used to.
    WarnF("audio: no mixer thread (%s), mixing in the device callback\n", e.what());
    thread_mode_.store(false, std::memory_order_relaxed);
  }
}

void Mixer::StopThread() {
  if (!thread_.joinable()) return;
  stop_.store(true, std::memory_order_relaxed);
  thread_.join();  // a period at most
}

// The mixer thread: real-time on its period, each wake topping the ring
// back up to its fill with what the device has taken since the last one.
void Mixer::ThreadMain() {
  platform::SetThreadName("recomp-mixer");
  // The class a game's own threads run in first: a host that refuses the
  // real-time policy below leaves the thread at that class.
  platform::SetThreadQos({platform::ThreadQos::kUserInteractive, 0});
  // Then an audio render thread as Core Audio makes one: woken every
  // period, two milliseconds of mix in it (a period measures 0.1-0.6 ms,
  // and a stall is caught up with two periods' worth at once), five to be
  // done by. Without it the game's own user-interactive threads starve the
  // mixer thread and the device plays silence.
  // RECOMP_AUDIO_MIXRT=0: no real-time policy, for an A/B against the
  // host's scheduler.
  static const bool want_realtime = [] {
    const char* e = std::getenv("RECOMP_AUDIO_MIXRT");
    return !(e && *e == '0');
  }();
  const uint64_t period_ns = uint64_t(1e9) * uint64_t(period_) / uint64_t(rate_);
  const bool realtime = want_realtime && platform::SetThreadRealTime(period_ns, 2000000, 5000000);
  if (want_realtime && !realtime)
    WarnF("audio: the mixer thread is not real-time; a busy host can starve it\n");
  uint64_t next = 0;  // the next period's start, on the monotonic clock
  auto woken = std::chrono::steady_clock::now();
  while (!stop_.load(std::memory_order_relaxed)) {
    const auto t0 = std::chrono::steady_clock::now();
    const double period_ms = std::chrono::duration<double, std::milli>(t0 - woken).count();
    woken = t0;
    double wait = 0, mix_ms = 0;
    // What the device took since the last period is what the ring is
    // short of its fill; that, and what fits, is what is mixed now: a
    // stall of several periods is caught up in this one wake.
    const int free = ring_.Free();
    const int n = std::min(period_target_ - (ring_.Capacity() - free), free);
    if (n > 0) {
      if (log_timing) {
        const auto w = std::chrono::steady_clock::now();
        mu_.lock();
        mu_.unlock();
        wait = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w)
                   .count();
      }
      Mix(mix_buf_.data(), n);
      ring_.Write(mix_buf_.data(), n);
      mix_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                   .count();
    }
    if (log_timing)
      LogMixer(n > 0, wait, mix_ms, period_ms, ring_.Capacity() - ring_.Free(), ring_.Capacity());
    // The period grid, waited for by absolute deadline: a wake less than a
    // period late keeps its deadline (the next wait is shorter, lateness
    // does not add up), one a period late or more moves it (no burst of
    // catch-up wakes).
    const uint64_t now = platform::MonotonicNs();
    if (!next || now >= next + period_ns) next = now;
    next += period_ns;
    if (next <= now) continue;  // already late: mix again at once
    if (realtime)
      platform::WaitUntilNs(next);
    else
      platform::SleepUntilNs(next);
  }
}

// The device callback's side of the ring: n frames, silence for the ones
// the mixer thread has not put there yet.
int Mixer::Take(float* out, int n) {
  const int got = ring_.Read(out, n);
  if (got < n) std::memset(out + 2 * size_t(got), 0, sizeof(float) * 2 * size_t(n - got));
  return got;
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

// Miles 6.5 voices: runtime/audio/miles65.h, docs/miles.md.
//
// Miles 6.1c: positional voices follow its Fast 2D provider (Mssfast.m3d)
// exactly: it reduces each 3D sample to a 2D volume and pan, which Mss32
// turns into channel gains. (The asymmetry at +-90 degrees is the
// original's: hard left leaves 2/128 on the right, hard right nothing on
// the left; docs/miles.md.)
//  - volume: 127 / 10^((A + B) / 20) with A = -20 log10(vol / 127) and B =
//    6 dB per doubling of distance beyond min_dist (so vol * min_dist / d),
//    rounded and clamped to 0..127; 0 past max_dist; then * 71 / 100 when
//    the source is behind the listener.
//  - pan: 127 - floor(acos(dot(dir, up x face)) * 127 / pi), looked up in a
//    2049-entry table; 64 when the source is at the listener.
//  - Mss32: v = vol * master / 127; the far channel scales by a linear
//    balance table, the near one stays full; gains are out of 2048.
// DS3D voices follow DirectSound3D hardware as measured on an Audigy 2 ZS
// (Miles' DS3D and EAX providers, stereo output):
//  - distance: min_dist / d (to the power of the rolloff factor, which
//    only DirectSound changes), continuous; DirectSound stops attenuating at
//    max_dist rather than muting; no cut behind the listener;
//  - pan: the azimuth folded to the front half (135 degrees pans like 45)
//    is panned between virtual speakers at +-45 degrees with gains
//    summing to sqrt(2) (tangent law): 0.707 per channel ahead, 1.414 on
//    one channel at 45; beyond 45 the near gain falls as cos(az - 45) to
//    1.0 at 90 while the far channel stays silent (-55 dB measured). The
//    scale makes a side source as loud as Fast 2D's on the same card.
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
  if (v.positional && v.ds3d) {
    Vec3 rel = v.head_relative ? v.pos : Sub(v.pos, lpos_);
    float d = std::sqrt(Dot(rel, rel));
    float min = std::max(v.min_dist, 1e-4f);
    d = std::min(d, std::max(v.max_dist, min));
    if (d > min) g *= rolloff_ == 1.0f ? min / d : std::pow(min / d, rolloff_);
    float x = v.head_relative ? rel.x : Dot(rel, Cross(lup_, lface_));
    float z = std::fabs(v.head_relative ? rel.z : Dot(rel, lface_));
    float az = x == 0 && z == 0 ? 0.0f : std::atan2(std::fabs(x), z);
    float near, far = 0;
    if (az <= float(kPi / 4)) {
      float t = std::tan(az);
      near = 0.5f * (1 + t);
      far = 0.5f * (1 - t);
    } else {
      near = std::cos(az - float(kPi / 4));
    }
    near *= g * float(std::sqrt(2.0));
    far *= g * float(std::sqrt(2.0));
    *l = x < 0 ? near : far;
    *r = x < 0 ? far : near;
    return;
  }
  if (v.dsound) {
    // DSBPAN: the attenuation of the channel away from the pan.
    *l = g * (v.pan > 0 ? std::pow(10.0f, -v.pan / 2000.0f) : 1.0f);
    *r = g * (v.pan < 0 ? std::pow(10.0f, v.pan / 2000.0f) : 1.0f);
    return;
  }
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
  constexpr int kB = Reverb::kBuses;
  // Low-pass targets; levels and coefficients ramp across the chunk like
  // the gains (a straight line between two stable pole pairs stays stable).
  float da[2], sa[kB][2];
  LowPass(low_pass_, v.direct_lf, v.direct_hf, da);
  for (int b = 0; b < kB; b++) LowPass(send_low_pass_, v.send_lf[b], v.send_hf[b], sa[b]);
  auto settle = [&] {
    v.cur_l_ = tl;
    v.cur_r_ = tr;
    v.cur_dlf_ = v.direct_lf;
    v.cur_da_[0] = da[0];
    v.cur_da_[1] = da[1];
    for (int b = 0; b < kB; b++) {
      v.cur_slf_[b] = v.send_lf[b];
      v.cur_sa_[b][0] = sa[b][0];
      v.cur_sa_[b][1] = sa[b][1];
    }
  };
  if (!v.gains_valid_) {
    settle();
    v.gains_valid_ = true;
  }
  // Ramp gain changes across the chunk so volume and pan updates do not
  // click.
  float dl = (tl - v.cur_l_) / frames, dr = (tr - v.cur_r_) / frames;
  double rate = v.rate > 0 ? v.rate : v.src->rate();
  double step = rate / rate_;
  bool banded = v.direct_lf != 1 || v.direct_hf != 1 || v.cur_dlf_ != 1 ||
                v.cur_da_[0] != 0 || v.cur_da_[1] != 0;
  bool sending[kB];
  for (int b = 0; b < kB; b++) {
    sending[b] = v.send_lf[b] > 0 || v.cur_slf_[b] > 0;
    banded |= sending[b];
  }
  float ddlf = (v.direct_lf - v.cur_dlf_) / frames;
  float dda[2] = {(da[0] - v.cur_da_[0]) / frames, (da[1] - v.cur_da_[1]) / frames};
  float dslf[kB], dsa[kB][2];
  for (int b = 0; b < kB; b++) {
    dslf[b] = (v.send_lf[b] - v.cur_slf_[b]) / frames;
    dsa[b][0] = (sa[b][0] - v.cur_sa_[b][0]) / frames;
    dsa[b][1] = (sa[b][1] - v.cur_sa_[b][1]) / frames;
  }
  // One sample of a path's low-pass, both channels.
  auto filter = [](const float* a, float (*y)[2], float& l, float& r) {
    float g = 1 + a[0] + a[1];
    float yl = g * l - a[0] * y[0][0] - a[1] * y[0][1];
    float yr = g * r - a[0] * y[1][0] - a[1] * y[1][1];
    y[0][1] = y[0][0];
    y[0][0] = yl;
    y[1][1] = y[1][0];
    y[1][0] = yr;
    l = yl;
    r = yr;
  };
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
      for (int b = 0; b < kB; b++) {
        if (!sending[b]) continue;
        v.cur_slf_[b] += dslf[b];
        v.cur_sa_[b][0] += dsa[b][0];
        v.cur_sa_[b][1] += dsa[b][1];
        float xl = sl, xr = sr;
        filter(v.cur_sa_[b], v.sy_[b], xl, xr);
        float* s = &sends_[b][2 * i];
        s[0] += v.cur_slf_[b] * xl * send_gain;
        s[1] += v.cur_slf_[b] * xr * send_gain;
      }
      v.cur_dlf_ += ddlf;
      v.cur_da_[0] += dda[0];
      v.cur_da_[1] += dda[1];
      filter(v.cur_da_, v.dy_, sl, sr);
      sl *= v.cur_dlf_;
      sr *= v.cur_dlf_;
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
        settle();
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
  settle();
}

void Mixer::Timing(double wait_ms, double total_ms, double gap_ms, int frames) {
  static double max_wait = 0, max_total = 0, max_gap = 0;
  static uint64_t late = 0, calls = 0, last_ms = 0, frames_sum = 0, min_frames = ~0ull;
  max_wait = std::max(max_wait, wait_ms);
  max_total = std::max(max_total, total_ms);
  max_gap = std::max(max_gap, gap_ms);
  frames_sum += uint64_t(frames);
  min_frames = std::min<uint64_t>(min_frames, uint64_t(frames));
  calls++;
  if (device_rate_ && total_ms > 1000.0 * frames / device_rate_) late++;
  const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
  if (now - last_ms < 5000) return;
  if (last_ms)
    std::fprintf(stderr,
                 "audio: %llu device requests (%llu-%llu frames), %llu late; longest lock wait "
                 "%.2f ms, longest %.2f ms, longest gap between requests %.1f ms\n",
                 (unsigned long long)calls, (unsigned long long)min_frames,
                 (unsigned long long)(calls ? frames_sum / calls : 0), (unsigned long long)late,
                 max_wait, max_total, max_gap);
  last_ms = now;
  max_wait = max_total = max_gap = 0;
  late = calls = frames_sum = 0;
  min_frames = ~0ull;
}

void Mixer::LowPass(const std::vector<std::array<float, 2>>& table, float lf, float hf,
                    float* a) const {
  a[0] = a[1] = 0;
  if (!(lf > 0) || !(hf < lf) || table.empty()) return;
  float steps = hf > 0 ? -200.0f * std::log10(hf / lf) : float(kLowPassSteps);
  steps = std::min(steps, float(kLowPassSteps));
  int i = std::min(int(steps), kLowPassSteps - 1);
  float t = steps - float(i);
  a[0] = table[i][0] + (table[i + 1][0] - table[i][0]) * t;
  a[1] = table[i][1] + (table[i + 1][1] - table[i][1]) * t;
}

void Mixer::Mix(float* out, int frames) {
  std::memset(out, 0, sizeof(float) * 2 * frames);
  std::lock_guard<std::mutex> l(mu_);
  for (auto& s : sends_) s.assign(size_t(frames) * 2, 0.0f);
  bool reverb = digital_.Begin(now_ms());
  if (reverb) wet_.assign(size_t(frames), 0.0f);
  for (Voice* v : voices_)
    if (v->state == Voice::State::Playing) MixVoice(*v, out, frames, reverb);
  const float* sends[Reverb::kBuses];
  for (int b = 0; b < Reverb::kBuses; b++) sends[b] = sends_[b].data();
  reverb_.Render(sends, out, frames);
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
