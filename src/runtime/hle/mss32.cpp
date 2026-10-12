// mss32 (Miles Sound System 6): the 68 entry points the game imports,
// implemented on the host mixer (runtime/audio). 2D samples, 3D samples
// (the "Miles Fast 2D Positional Audio" provider: distance attenuation
// and panning), MP3/WAV streams and timers.
//
// Callbacks (end of sample/stream, timers) run on a Miles service thread,
// as with the real library: a guest thread created at driver open that
// polls every few milliseconds. No lock is held while guest code runs, so
// a callback may call back into Miles and may wait on the game's own
// locks. A sequence number per sample drops end-of-sample events that a
// restart on the game thread has made stale.
//
// Handles are small zeroed guest blocks, so they are real, distinct guest
// pointers; the game never looks inside them.

#include "runtime/hle/mss32.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/audio/mixer.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using audio::Voice;
using Clock = std::chrono::steady_clock;

// Sample/stream status values (mss.h).
constexpr uint32_t kSmpFree = 1, kSmpDone = 2, kSmpPlaying = 4, kSmpStopped = 8;
constexpr uint32_t kM3dNoErr = 0, kM3dNotEnabled = 1;
// The one 3D provider we offer. HPROVIDER is an opaque U32.
constexpr uint32_t kProvider = 0x3D0001;
constexpr const char* kProviderName = "Miles Fast 2D Positional Audio";

struct Sample {
  uint32_t handle = 0;
  bool is3d = false;
  Voice v;
  int bytes_per_frame = 0;  // of the loaded WAV
  int64_t length = 0;       // frames
  uint32_t eos_cb = 0;
  uint32_t seq = 0;   // bumped whenever an old end-of-sample event goes stale
  int volume = 127;
  bool start_pos_set = false;  // set_sample_position before start
  int64_t start_pos = 0;
};

struct Stream {
  uint32_t handle = 0;
  Voice v;
  uint32_t cb = 0;
  uint32_t seq = 0;
  int volume = 127;
};

struct Timer {
  uint32_t cb = 0, user = 0;
  int64_t period_us = 0;
  bool running = false;
  Clock::time_point next;
};

struct State {
  // Guarded by the mixer lock (one lock for Miles state and voices).
  std::map<uint32_t, uint32_t> prefs;
  std::string last_error;
  uint32_t error_buf = 0;
  uint32_t redist_buf = 0;
  uint32_t file_open = 0, file_close = 0, file_read = 0;  // no seek needed
  uint32_t driver = 0;  // HDIGDRIVER while open
  int master = 127;
  uint32_t listener = 0;
  audio::Vec3 lpos, lface{0, 0, 1}, lup{0, 1, 0};
  std::map<uint32_t, std::unique_ptr<Sample>> samples;  // by handle
  std::map<uint32_t, std::unique_ptr<Stream>> streams;
  std::vector<uint32_t> free_blocks;  // guest handle blocks to reuse
  std::map<uint32_t, Timer> timers;   // by HTIMER
  uint32_t next_timer = 1;  // 0 reads as "no timer" to the game
  bool service_started = false;
  uint32_t provider_name = 0;
  uint32_t seq = 0;  // source of Sample/Stream::seq, unique across reuse
};

State& S() {
  static State s;
  return s;
}
std::mutex& Mu() { return audio::GetMixer().mu(); }
uint32_t NextSeq() { return ++S().seq; }

// Miles preference defaults (mss.h, MSS 6).
void InitPrefs(State& s) {
  if (!s.prefs.empty()) return;
  s.prefs[0] = 131;   // DIG_RESAMPLING_TOLERANCE
  s.prefs[1] = 64;    // DIG_MIXER_CHANNELS
  s.prefs[2] = 127;   // DIG_DEFAULT_VOLUME
  s.prefs[35] = 64;   // DIG_DS_FRAGMENT_CNT
  s.prefs[42] = 8;    // DIG_DS_MIX_FRAGMENT_CNT
}

uint32_t NewBlock(State& s) {
  if (!s.free_blocks.empty()) {
    uint32_t b = s.free_blocks.back();
    s.free_blocks.pop_back();
    std::memset(GPtr(b), 0, 32);
    return b;
  }
  return HeapAllocProcess(32, true);
}

float F(uint32_t bits) { return std::bit_cast<float>(bits); }

Sample* FindSample(uint32_t h, bool is3d, const char* fn) {
  auto& m = S().samples;
  auto it = m.find(h);
  if (it == m.end() || it->second->is3d != is3d) {
    WarnF("mss32: %s: unknown %s handle %08X\n", fn, is3d ? "3D sample" : "sample", h);
    return nullptr;
  }
  return it->second.get();
}

Stream* FindStream(uint32_t h, const char* fn) {
  auto it = S().streams.find(h);
  if (it == S().streams.end()) {
    WarnF("mss32: %s: unknown stream handle %08X\n", fn, h);
    return nullptr;
  }
  return it->second.get();
}

uint32_t StatusOf(const Voice& v) {
  switch (v.state) {
    case Voice::State::Playing: return kSmpPlaying;
    case Voice::State::Paused: return kSmpStopped;
    default: return kSmpDone;
  }
}

void ApplyVolume(Sample& s) { s.v.volume = s.volume / 127.0f; }

// Puts a sample back to its just-initialised state.
void ResetSample(Sample& s) {
  s.v.state = Voice::State::Idle;
  s.v.eos = false;
  s.v.loops = 1;
  s.v.rate = 0;
  s.volume = 127;
  ApplyVolume(s);
  s.eos_cb = 0;
  s.seq = NextSeq();
  s.start_pos_set = false;
  s.start_pos = 0;
  if (s.is3d) {
    s.v.positional = true;
    s.v.pos = {};
    s.v.min_dist = 1.0f;
    s.v.max_dist = 1e30f;
  }
}

// Starts a sample's or stream's voice from frame `from`; `seq` is its
// end-of-sample sequence number.
void Start(Voice& v, uint32_t* seq, int64_t from) {
  v.Seek(from);
  v.passes = 0;
  v.eos = false;
  v.gains_valid_ = false;
  v.state = Voice::State::Playing;
  *seq = NextSeq();
  audio::GetMixer().Add(&v);
}

void Play(Sample& s, int64_t from) {
  if (!s.v.src) return;
  s.start_pos_set = false;
  Start(s.v, &s.seq, from);
}

// ---- service thread ------------------------------------------------------------

struct Event {
  uint32_t cb, arg;
  // For end-of-sample events: deliver only if still current.
  Sample* sample = nullptr;
  Stream* stream = nullptr;
  uint32_t seq = 0;
};

void ServiceLoop(Thread* t) {
  std::vector<Event> events;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    events.clear();
    {
      std::lock_guard<std::mutex> l(Mu());
      State& st = S();
      if (!st.driver) continue;
      auto now = Clock::now();
      for (auto& [h, tm] : st.timers) {
        if (!tm.running || !tm.cb || tm.period_us <= 0) continue;
        if (now < tm.next) continue;
        events.push_back({tm.cb, tm.user});
        tm.next += std::chrono::microseconds(tm.period_us);
        if (tm.next < now) tm.next = now;  // never burst to catch up
      }
      for (auto& [h, s] : st.samples) {
        if (!s->v.eos) continue;
        s->v.eos = false;
        if (s->eos_cb) events.push_back({s->eos_cb, h, s.get(), nullptr, s->seq});
      }
      for (auto& [h, s] : st.streams) {
        if (!s->v.eos) continue;
        s->v.eos = false;
        if (s->cb) events.push_back({s->cb, h, nullptr, s.get(), s->seq});
      }
    }
    for (const Event& e : events) {
      if (e.sample || e.stream) {
        std::lock_guard<std::mutex> l(Mu());
        // The game may have restarted or released it meanwhile.
        // Look up by handle first: e.sample may have been freed.
        if (e.sample) {
          auto it = S().samples.find(e.arg);
          if (it == S().samples.end() || it->second->seq != e.seq ||
              it->second->v.state != Voice::State::Done)
            continue;
        }
        if (e.stream) {
          auto it = S().streams.find(e.arg);
          if (it == S().streams.end() || it->second->seq != e.seq) continue;
        }
      }
      CallGuest(t->ctx, e.cb, {e.arg});
    }
  }
}

void StartService() {
  State& st = S();
  if (st.service_started) return;
  st.service_started = true;
  Thread* t = CreateGuestThread(0x100000);
  StartServiceThread(t, [t] { ServiceLoop(t); });
}

void SetError(const std::string& e) {
  std::lock_guard<std::mutex> l(Mu());
  S().last_error = e;
}

// Copies a WAV file image out of guest memory (paging it in through the
// game's handlers if needed) and makes a source from it.
std::unique_ptr<audio::Source> LoadImage(Ctx& c, uint32_t image, int* bpf,
                                         int64_t* len, const char* fn) {
  if (!image || !EnsureReadable(c, image, 12)) {
    WarnF("mss32: %s: unreadable file image %08X\n", fn, image);
    return nullptr;
  }
  uint32_t size = audio::RiffSize(GPtr(image));
  if (!size) {
    WarnF("mss32: %s: file image %08X is not a RIFF WAVE\n", fn, image);
    return nullptr;
  }
  if (!EnsureReadable(c, image, size)) {
    WarnF("mss32: %s: cannot page in %u bytes at %08X\n", fn, size, image);
    return nullptr;
  }
  auto bytes = std::make_shared<std::vector<uint8_t>>(GPtr(image), GPtr(image) + size);
  std::string err;
  auto src = audio::MakeWavSource(bytes, &err);
  if (!src) {
    WarnF("mss32: %s: %s\n", fn, err.c_str());
    return nullptr;
  }
  *bpf = src->bytes_per_frame();
  *len = src->Length();
  return src;
}

uint32_t SetFile(Ctx& c, uint32_t h, uint32_t image, bool is3d, const char* fn) {
  int bpf = 0;
  int64_t len = 0;
  auto src = LoadImage(c, image, &bpf, &len, fn);
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(h, is3d, fn);
  if (!s) return 0;
  s->v.state = Voice::State::Idle;
  s->v.eos = false;
  s->seq = NextSeq();
  s->v.src = std::move(src);
  s->v.Seek(0);
  s->v.rate = 0;
  s->bytes_per_frame = bpf;
  s->length = len;
  s->start_pos_set = false;
  return s->v.src ? 1 : 0;
}

// ---- library / driver ---------------------------------------------------------------

HLE_NAMED("mss32.dll", "_AIL_startup@0", AIL_startup, 0) {
  std::lock_guard<std::mutex> l(Mu());
  InitPrefs(S());
  return 1;
}

void CloseDriver() {
  State& st = S();
  audio::Mixer& mx = audio::GetMixer();
  for (auto& [h, s] : st.samples) mx.Remove(&s->v);
  for (auto& [h, s] : st.streams) mx.Remove(&s->v);
  for (auto& [h, s] : st.samples) st.free_blocks.push_back(h);
  for (auto& [h, s] : st.streams) st.free_blocks.push_back(h);
  st.samples.clear();
  st.streams.clear();
  st.timers.clear();
  if (st.driver) st.free_blocks.push_back(st.driver);
  if (st.listener) st.free_blocks.push_back(st.listener);
  st.driver = st.listener = 0;
}

HLE_NAMED("mss32.dll", "_AIL_shutdown@0", AIL_shutdown, 0) {
  {
    std::lock_guard<std::mutex> l(Mu());
    CloseDriver();
  }
  audio::GetMixer().Close();  // waits for the audio thread; not under Mu
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_redist_directory@4", AIL_set_redist_directory, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (!st.redist_buf) {
    st.redist_buf = HeapAllocProcess(260, true);
    PutStr(st.redist_buf, ".");
  }
  return st.redist_buf;  // previous directory
}

HLE_NAMED("mss32.dll", "_AIL_set_file_callbacks@16", AIL_set_file_callbacks, 4) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  st.file_open = Arg(c, 0);
  st.file_close = Arg(c, 1);
  st.file_read = Arg(c, 3);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_preference@8", AIL_set_preference, 2) {
  std::lock_guard<std::mutex> l(Mu());
  InitPrefs(S());
  uint32_t& p = S().prefs[Arg(c, 0)];
  uint32_t old = p;
  p = Arg(c, 1);
  return old;
}

HLE_NAMED("mss32.dll", "_AIL_get_preference@4", AIL_get_preference, 1) {
  std::lock_guard<std::mutex> l(Mu());
  InitPrefs(S());
  return S().prefs[Arg(c, 0)];
}

HLE_NAMED("mss32.dll", "_AIL_last_error@0", AIL_last_error, 0) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (!st.error_buf) st.error_buf = HeapAllocProcess(256, true);
  PutStr(st.error_buf, st.last_error.substr(0, 255));
  return st.error_buf;
}

// AIL_open_digital_driver(frequency, bits, channels, flags) -> HDIGDRIVER.
HLE_NAMED("mss32.dll", "_AIL_open_digital_driver@16", AIL_open_digital_driver, 4) {
  uint32_t rate = Arg(c, 0);
  {
    std::lock_guard<std::mutex> l(Mu());
    if (S().driver) return S().driver;
  }
  if (!audio::GetMixer().Open(int(rate ? rate : 44100))) {
    SetError("No digital audio device available");
    return 0;
  }
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  st.driver = NewBlock(st);
  audio::GetMixer().set_master(st.master / 127.0f);
  StartService();
  return st.driver;
}

HLE_NAMED("mss32.dll", "_AIL_set_digital_master_volume@8", AIL_set_digital_master_volume, 2) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  st.master = std::clamp<int>(int32_t(Arg(c, 1)), 0, 127);
  audio::GetMixer().set_master(st.master / 127.0f);
  return 0;
}

// ---- 2D samples ---------------------------------------------------------------------

HLE_NAMED("mss32.dll", "_AIL_allocate_sample_handle@4", AIL_allocate_sample_handle, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (!st.driver || Arg(c, 0) != st.driver) return 0;
  auto s = std::make_unique<Sample>();
  s->handle = NewBlock(st);
  ResetSample(*s);
  uint32_t h = s->handle;
  st.samples[h] = std::move(s);
  return h;
}

void Release(uint32_t h, bool is3d, const char* fn) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(h, is3d, fn);
  if (!s) return;
  audio::GetMixer().Remove(&s->v);
  S().samples.erase(h);
  S().free_blocks.push_back(h);
}

HLE_NAMED("mss32.dll", "_AIL_release_sample_handle@4", AIL_release_sample_handle, 1) {
  Release(Arg(c, 0), false, "AIL_release_sample_handle");
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_init_sample@4", AIL_init_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_init_sample")) {
    audio::GetMixer().Remove(&s->v);
    s->v.src.reset();
    ResetSample(*s);
  }
  return 0;
}

// AIL_set_sample_file(S, file_image, block) -> nonzero on success.
HLE_NAMED("mss32.dll", "_AIL_set_sample_file@12", AIL_set_sample_file, 3) {
  return SetFile(c, Arg(c, 0), Arg(c, 1), false, "AIL_set_sample_file");
}

HLE_NAMED("mss32.dll", "_AIL_set_sample_loop_count@8", AIL_set_sample_loop_count, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_loop_count"))
    s->v.loops = std::max<int32_t>(int32_t(Arg(c, 1)), 0);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_sample_volume@8", AIL_set_sample_volume, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_volume")) {
    s->volume = std::clamp<int>(int32_t(Arg(c, 1)), 0, 127);
    ApplyVolume(*s);
  }
  return 0;
}

uint32_t RateOf(const Sample& s) {
  if (s.v.rate > 0) return uint32_t(std::lround(s.v.rate));
  return s.v.src ? uint32_t(s.v.src->rate()) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_sample_playback_rate@4", AIL_sample_playback_rate, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_sample_playback_rate");
  return s ? RateOf(*s) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_sample_playback_rate@8", AIL_set_sample_playback_rate, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_playback_rate"))
    s->v.rate = float(int32_t(Arg(c, 1)));
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_register_EOS_callback@8", AIL_register_EOS_callback, 2) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_register_EOS_callback");
  if (!s) return 0;
  uint32_t old = s->eos_cb;
  s->eos_cb = Arg(c, 1);
  return old;
}

void SetPosition(Sample& s, uint32_t bytes) {
  int64_t f = s.bytes_per_frame ? bytes / s.bytes_per_frame : 0;
  if (s.v.state == Voice::State::Playing || s.v.state == Voice::State::Paused) {
    s.v.Seek(f);
  } else {
    s.start_pos_set = true;
    s.start_pos = f;
  }
}

uint32_t PositionOf(const Sample& s) {
  int64_t f = s.start_pos_set ? s.start_pos : s.v.Position();
  if (s.v.state == Voice::State::Done) f = s.length;
  return uint32_t(f * s.bytes_per_frame);
}

HLE_NAMED("mss32.dll", "_AIL_set_sample_position@8", AIL_set_sample_position, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_position"))
    SetPosition(*s, Arg(c, 1));
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_sample_position@4", AIL_sample_position, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_sample_position");
  return s ? PositionOf(*s) : 0;
}

// AIL_sample_ms_position(S, S32* total_ms, S32* current_ms).
HLE_NAMED("mss32.dll", "_AIL_sample_ms_position@12", AIL_sample_ms_position, 3) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_sample_ms_position");
  uint32_t total = 0, cur = 0;
  if (s && s->v.src && s->v.src->rate()) {
    double r = s->v.src->rate();
    int64_t f = s->v.state == Voice::State::Done ? s->length : s->v.Position();
    total = uint32_t(s->length * 1000.0 / r);
    cur = uint32_t(f * 1000.0 / r);
  }
  if (Arg(c, 1)) Wr32(Arg(c, 1), total);
  if (Arg(c, 2)) Wr32(Arg(c, 2), cur);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_start_sample@4", AIL_start_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_start_sample"))
    Play(*s, s->start_pos_set ? s->start_pos : 0);
  return 0;
}

void Stop(Sample& s) {
  if (s.v.state == Voice::State::Playing) s.v.state = Voice::State::Paused;
}

void Resume(Sample& s) {
  if (s.v.state == Voice::State::Paused) {
    s.v.state = Voice::State::Playing;
  } else if (s.v.state != Voice::State::Playing) {
    // Resuming a sample that was never started plays it from the offset
    // set with set_sample_position / set_3D_sample_offset.
    Play(s, s.start_pos_set ? s.start_pos : s.v.Position());
  }
}

void End(Sample& s) {
  if (s.v.state == Voice::State::Playing || s.v.state == Voice::State::Paused)
    s.v.state = Voice::State::Done;
  s.v.eos = false;  // ended by request: no callback
  s.seq = NextSeq();
}

HLE_NAMED("mss32.dll", "_AIL_stop_sample@4", AIL_stop_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_stop_sample")) Stop(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_resume_sample@4", AIL_resume_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_resume_sample")) Resume(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_end_sample@4", AIL_end_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_end_sample")) End(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_sample_status@4", AIL_sample_status, 1) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().samples.find(Arg(c, 0));
  if (it == S().samples.end()) return kSmpFree;
  return StatusOf(it->second->v);
}

// ---- 3D provider, listener and samples ---------------------------------------------

// AIL_enumerate_3D_providers(HPROENUM* next, HPROVIDER* dest, char** name).
HLE_NAMED("mss32.dll", "_AIL_enumerate_3D_providers@12", AIL_enumerate_3D_providers, 3) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (Rd32(Arg(c, 0)) != 0) return 0;
  if (!st.provider_name) {
    st.provider_name = HeapAllocProcess(64, true);
    PutStr(st.provider_name, kProviderName);
  }
  Wr32(Arg(c, 0), 1);
  Wr32(Arg(c, 1), kProvider);
  Wr32(Arg(c, 2), st.provider_name);
  return 1;
}

HLE_NAMED("mss32.dll", "_AIL_open_3D_provider@4", AIL_open_3D_provider, 1) {
  return Arg(c, 0) == kProvider ? kM3dNoErr : kM3dNotEnabled;
}

HLE_NAMED("mss32.dll", "_AIL_close_3D_provider@4", AIL_close_3D_provider, 1) {
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_open_3D_listener@4", AIL_open_3D_listener, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (Arg(c, 0) != kProvider || !st.driver) return 0;
  if (!st.listener) st.listener = NewBlock(st);
  return st.listener;
}

HLE_NAMED("mss32.dll", "_AIL_close_3D_listener@4", AIL_close_3D_listener, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (Arg(c, 0) == st.listener && st.listener) {
    st.free_blocks.push_back(st.listener);
    st.listener = 0;
  }
  return 0;
}

// Room types only matter to EAX providers.
HLE_NAMED("mss32.dll", "_AIL_set_3D_room_type@8", AIL_set_3D_room_type, 2) { return 0; }

HLE_NAMED("mss32.dll", "_AIL_allocate_3D_sample_handle@4", AIL_allocate_3D_sample_handle, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (Arg(c, 0) != kProvider || !st.driver) return 0;
  auto s = std::make_unique<Sample>();
  s->handle = NewBlock(st);
  s->is3d = true;
  ResetSample(*s);
  uint32_t h = s->handle;
  st.samples[h] = std::move(s);
  return h;
}

HLE_NAMED("mss32.dll", "_AIL_release_3D_sample_handle@4", AIL_release_3D_sample_handle, 1) {
  Release(Arg(c, 0), true, "AIL_release_3D_sample_handle");
  return 0;
}

// AIL_set_3D_sample_file(S, file_image) -> nonzero on success. Like
// AIL_set_sample_file it also resets the sample.
HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_file@8", AIL_set_3D_sample_file, 2) {
  {
    std::lock_guard<std::mutex> l(Mu());
    Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_file");
    if (!s) return 0;
    audio::GetMixer().Remove(&s->v);
    s->v.src.reset();
    uint32_t keep_cb = s->eos_cb;
    ResetSample(*s);
    s->eos_cb = keep_cb;
  }
  return SetFile(c, Arg(c, 0), Arg(c, 1), true, "AIL_set_3D_sample_file");
}

HLE_NAMED("mss32.dll", "_AIL_start_3D_sample@4", AIL_start_3D_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_start_3D_sample")) Play(*s, 0);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_resume_3D_sample@4", AIL_resume_3D_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_resume_3D_sample")) Resume(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_stop_3D_sample@4", AIL_stop_3D_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_stop_3D_sample")) Stop(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_end_3D_sample@4", AIL_end_3D_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_end_3D_sample")) End(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_offset@8", AIL_set_3D_sample_offset, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_offset"))
    SetPosition(*s, Arg(c, 1));
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_3D_sample_offset@4", AIL_3D_sample_offset, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_3D_sample_offset");
  return s ? PositionOf(*s) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_3D_sample_length@4", AIL_3D_sample_length, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_3D_sample_length");
  return s ? uint32_t(s->length * s->bytes_per_frame) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_register_3D_EOS_callback@8", AIL_register_3D_EOS_callback, 2) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_register_3D_EOS_callback");
  if (!s) return 0;
  uint32_t old = s->eos_cb;
  s->eos_cb = Arg(c, 1);
  return old;
}

// Reverb send level: only EAX providers have reverb.
HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_effects_level@8", AIL_set_3D_sample_effects_level, 2) {
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_playback_rate@8", AIL_set_3D_sample_playback_rate, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_playback_rate"))
    s->v.rate = float(int32_t(Arg(c, 1)));
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_3D_sample_playback_rate@4", AIL_3D_sample_playback_rate, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_3D_sample_playback_rate");
  return s ? RateOf(*s) : 0;
}

// AIL_set_3D_sample_distances(S, F32 max_dist, F32 min_dist).
HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_distances@12", AIL_set_3D_sample_distances, 3) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_distances")) {
    // Mss32 swaps the pair so the larger is max (the game passes min first).
    float a = F(Arg(c, 1)), b = F(Arg(c, 2));
    s->v.max_dist = std::max(a, b);
    s->v.min_dist = std::min(a, b);
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_volume@8", AIL_set_3D_sample_volume, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_volume")) {
    s->volume = std::clamp<int>(int32_t(Arg(c, 1)), 0, 127);
    ApplyVolume(*s);
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_3D_sample_volume@4", AIL_3D_sample_volume, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_3D_sample_volume");
  return s ? uint32_t(s->volume) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_loop_count@8", AIL_set_3D_sample_loop_count, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_loop_count"))
    s->v.loops = std::max<int32_t>(int32_t(Arg(c, 1)), 0);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_3D_sample_status@4", AIL_3D_sample_status, 1) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().samples.find(Arg(c, 0));
  if (it == S().samples.end()) return kSmpFree;
  return StatusOf(it->second->v);
}

// AIL_set_3D_position(H3DPOBJECT, F32 x, F32 y, F32 z): the listener or a
// 3D sample.
HLE_NAMED("mss32.dll", "_AIL_set_3D_position@16", AIL_set_3D_position, 4) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  audio::Vec3 p{F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
  uint32_t obj = Arg(c, 0);
  if (obj && obj == st.listener) {
    st.lpos = p;
    audio::GetMixer().SetListener(st.lpos, st.lface, st.lup);
  } else if (Sample* s = FindSample(obj, true, "AIL_set_3D_position")) {
    s->v.pos = p;
  }
  return 0;
}

// AIL_set_3D_orientation(obj, face xyz, up xyz). Sample orientation only
// matters for sound cones, which the game does not set.
HLE_NAMED("mss32.dll", "_AIL_set_3D_orientation@28", AIL_set_3D_orientation, 7) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (Arg(c, 0) && Arg(c, 0) == st.listener) {
    st.lface = {F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
    st.lup = {F(Arg(c, 4)), F(Arg(c, 5)), F(Arg(c, 6))};
    audio::GetMixer().SetListener(st.lpos, st.lface, st.lup);
  }
  return 0;
}

// ---- streams ---------------------------------------------------------------------------

// Reads a whole file through the game's file callbacks (its own file
// system), on the calling guest thread.
bool ReadThroughCallbacks(Ctx& c, uint32_t name, std::vector<uint8_t>* out) {
  uint32_t open_cb, close_cb, read_cb;
  {
    std::lock_guard<std::mutex> l(Mu());
    open_cb = S().file_open;
    close_cb = S().file_close;
    read_cb = S().file_read;
  }
  if (!open_cb || !read_cb) return false;
  // Guest scratch: the handle out-parameter and a read buffer.
  static uint32_t scratch = 0;
  constexpr uint32_t kBuf = 0x10000;
  if (!scratch) scratch = HeapAllocProcess(kBuf + 16, true);
  uint32_t hptr = scratch, buf = scratch + 16;
  Wr32(hptr, 0);
  if (!CallGuest(c, open_cb, {name, hptr})) return false;
  uint32_t fh = Rd32(hptr);
  for (;;) {
    uint32_t got = CallGuest(c, read_cb, {fh, buf, kBuf});
    if (got > kBuf) got = 0;
    out->insert(out->end(), GPtr(buf), GPtr(buf) + got);
    if (got < kBuf) break;
  }
  if (close_cb) CallGuest(c, close_cb, {fh});
  return true;
}

// AIL_open_stream(HDIGDRIVER, char const* filename, S32 stream_mem).
HLE_NAMED("mss32.dll", "_AIL_open_stream@12", AIL_open_stream, 3) {
  std::string name = GStr(Arg(c, 1));
  {
    std::lock_guard<std::mutex> l(Mu());
    if (!S().driver || Arg(c, 0) != S().driver) return 0;
  }
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  if (!ReadThroughCallbacks(c, Arg(c, 1), bytes.get()) || bytes->empty()) {
    WarnF("mss32: AIL_open_stream(%s): cannot read the file\n", name.c_str());
    SetError("Unable to open file " + name);
    return 0;
  }
  std::string err;
  auto src = audio::MakeSource(bytes, &err);
  if (!src) {
    WarnF("mss32: AIL_open_stream(%s): %s\n", name.c_str(), err.c_str());
    SetError("Unsupported stream format: " + err);
    return 0;
  }
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  auto s = std::make_unique<Stream>();
  s->handle = NewBlock(st);
  s->v.src = std::move(src);
  s->v.state = Voice::State::Paused;  // opened, not started
  uint32_t h = s->handle;
  RECOMP_TRACE("  AIL_open_stream(%s) -> %08X (%d Hz, %d ch)\n", name.c_str(), h,
            s->v.src->rate(), s->v.src->channels());
  st.streams[h] = std::move(s);
  return h;
}

HLE_NAMED("mss32.dll", "_AIL_close_stream@4", AIL_close_stream, 1) {
  std::lock_guard<std::mutex> l(Mu());
  uint32_t h = Arg(c, 0);
  Stream* s = FindStream(h, "AIL_close_stream");
  if (!s) return 0;
  audio::GetMixer().Remove(&s->v);
  S().streams.erase(h);
  S().free_blocks.push_back(h);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_start_stream@4", AIL_start_stream, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_start_stream")) Start(s->v, &s->seq, 0);
  return 0;
}

// AIL_pause_stream(S, onoff).
HLE_NAMED("mss32.dll", "_AIL_pause_stream@8", AIL_pause_stream, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_pause_stream")) {
    if (Arg(c, 1)) {
      if (s->v.state == Voice::State::Playing) s->v.state = Voice::State::Paused;
    } else if (s->v.state == Voice::State::Paused) {
      s->v.state = Voice::State::Playing;
      audio::GetMixer().Add(&s->v);
    }
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_stream_loop_count@8", AIL_set_stream_loop_count, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_set_stream_loop_count"))
    s->v.loops = std::max<int32_t>(int32_t(Arg(c, 1)), 0);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_stream_volume@8", AIL_set_stream_volume, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_set_stream_volume")) {
    s->volume = std::clamp<int>(int32_t(Arg(c, 1)), 0, 127);
    s->v.volume = s->volume / 127.0f;
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_stream_volume@4", AIL_stream_volume, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Stream* s = FindStream(Arg(c, 0), "AIL_stream_volume");
  return s ? uint32_t(s->volume) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_register_stream_callback@8", AIL_register_stream_callback, 2) {
  std::lock_guard<std::mutex> l(Mu());
  Stream* s = FindStream(Arg(c, 0), "AIL_register_stream_callback");
  if (!s) return 0;
  uint32_t old = s->cb;
  s->cb = Arg(c, 1);
  return old;
}

HLE_NAMED("mss32.dll", "_AIL_stream_status@4", AIL_stream_status, 1) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().streams.find(Arg(c, 0));
  if (it == S().streams.end()) return kSmpFree;
  return StatusOf(it->second->v);
}

// ---- timers --------------------------------------------------------------------------------

// AIL_register_timer(AILTIMERCB) -> HTIMER (-1 on failure).
HLE_NAMED("mss32.dll", "_AIL_register_timer@4", AIL_register_timer, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  uint32_t h = st.next_timer++;
  st.timers[h].cb = Arg(c, 0);
  return h;
}

HLE_NAMED("mss32.dll", "_AIL_set_timer_user@8", AIL_set_timer_user, 2) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().timers.find(Arg(c, 0));
  if (it == S().timers.end()) return 0;
  uint32_t old = it->second.user;
  it->second.user = Arg(c, 1);
  return old;
}

// AIL_set_timer_period(HTIMER, U32 microseconds).
HLE_NAMED("mss32.dll", "_AIL_set_timer_period@8", AIL_set_timer_period, 2) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().timers.find(Arg(c, 0));
  if (it != S().timers.end()) {
    it->second.period_us = Arg(c, 1);
    it->second.next = Clock::now() + std::chrono::microseconds(Arg(c, 1));
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_start_timer@4", AIL_start_timer, 1) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().timers.find(Arg(c, 0));
  if (it != S().timers.end() && !it->second.running) {
    it->second.running = true;
    it->second.next = Clock::now() + std::chrono::microseconds(it->second.period_us);
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_release_timer_handle@4", AIL_release_timer_handle, 1) {
  std::lock_guard<std::mutex> l(Mu());
  S().timers.erase(Arg(c, 0));
  return 0;
}

// ---- misc ----------------------------------------------------------------------------------

// AIL_WAV_info(void const* data, AILSOUNDINFO* info) -> nonzero if a WAV.
HLE_NAMED("mss32.dll", "_AIL_WAV_info@8", AIL_WAV_info, 2) {
  uint32_t data = Arg(c, 0), info = Arg(c, 1);
  if (!data || !EnsureReadable(c, data, 12)) return 0;
  uint32_t size = audio::RiffSize(GPtr(data));
  if (!size || !EnsureReadable(c, data, size)) return 0;
  audio::WavInfo w;
  if (!audio::ParseWav(GPtr(data), size, &w)) return 0;
  // AILSOUNDINFO {format, data_ptr, data_len, rate, bits, channels,
  // samples, block_size, initial_ptr}
  Wr32(info + 0x00, w.format);
  Wr32(info + 0x04, data + w.data_offset);
  Wr32(info + 0x08, w.data_len);
  Wr32(info + 0x0C, w.rate);
  Wr32(info + 0x10, w.bits);
  Wr32(info + 0x14, w.channels);
  Wr32(info + 0x18, w.block_align ? w.data_len / w.block_align : 0);
  Wr32(info + 0x1C, w.block_align);
  Wr32(info + 0x20, data + w.data_offset);
  return 1;
}

}  // namespace

bool MilesDriverOpen() {
  std::lock_guard<std::mutex> l(Mu());
  return S().driver != 0;
}

}  // namespace recomp::hle
