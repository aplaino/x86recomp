// libSceNgs2 HLE: the system, its racks (sampler, submixer, reverb,
// mastering) and their voices, rendered on the calling thread into the
// game's buffers, as libSceNgs2 11.00 does; the game hands them to
// libSceAudioOut itself. Handles, the allocator protocol, the option and
// parameter layouts, their checks and codes, the voice event and state
// machine, the patch graph and its render order are 11.00's; the signal
// processing is in ngs2_dsp.cpp. The voice and rack objects live here,
// not in the game's buffers, which only give the handles their addresses;
// a rack's buffer size is this HLE's own (docs/hle.md).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/ngs2_dsp.h"
#include "orbis/process.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace {

using recomp::gaddr;
using recomp::Rd32;
using recomp::Rd64;
using recomp::Wr32;
using recomp::Wr64;
using namespace recomp::orbis::ngs2;

constexpr uint32_t kErrInvalidMaxGrain = 0x804A0050, kErrInvalidNumGrain = 0x804A0051,
                   kErrInvalidChannels = 0x804A0052, kErrInvalidOutAddress = 0x804A0053,
                   kErrInvalidStateSize = 0x804A0054, kErrInvalidOptionSize = 0x804A0081,
                   kErrInvalidOptionFlag = 0x804A0082, kErrInvalidMaxMatrices = 0x804A0100,
                   kErrInvalidMaxPorts = 0x804A0101, kErrInvalidMaxDelayBlocks = 0x804A0102,
                   kErrInvalidMaxVoices = 0x804A0103, kErrInvalidMaxChannels = 0x804A0104,
                   kErrInvalidMaxChannelWorks = 0x804A0105,
                   kErrInvalidMaxWaveformBlocks = 0x804A0106,
                   kErrInvalidMaxEnvelopePoints = 0x804A0107, kErrInvalidMaxFilters = 0x804A0108,
                   kErrInvalidMaxCodecCaches = 0x804A0109,
                   kErrInvalidMaxAtrac9Decoders = 0x804A010A,
                   kErrInvalidMaxAtrac9ChannelWorks = 0x804A010B,
                   kErrInvalidMaxInputs = 0x804A010C, kErrInvalidMatrixLevels = 0x804A0150,
                   kErrWaveformBlocks = 0x804A0151, kErrInvalidEnvelopePoints = 0x804A0152,
                   kErrInvalidMatrixLevelAddress = 0x804A0153,
                   kErrInvalidBlockAddress = 0x804A0154,
                   kErrInvalidEnvelopePointAddress = 0x804A0155,
                   kErrInvalidPeakMeterBlocks = 0x804A0156, kErrInvalidSampleRate = 0x804A0201,
                   kErrInvalidBufferInfo = 0x804A0206, kErrInvalidBufferAddress = 0x804A0207,
                   kErrInvalidBufferSize = 0x804A0209, kErrInvalidAllocator = 0x804A020A,
                   kErrInvalidSystemHandle = 0x804A0230, kErrInvalidRackId = 0x804A0260,
                   kErrInvalidRackHandle = 0x804A0261, kErrInvalidVoiceHandle = 0x804A0300,
                   kErrInvalidVoiceIndex = 0x804A0302, kErrInvalidEvent = 0x804A0303,
                   kErrInvalidPort = 0x804A0304, kErrInvalidInput = 0x804A0305,
                   kErrInvalidControl = 0x804A0308, kErrInvalidParamSize = 0x804A030A,
                   kErrCircularControl = 0x804A030B, kErrNoCallbacks = 0x804A030C,
                   kErrInvalidWaveformType = 0x804A0402,
                   kErrInvalidWaveformRate = 0x804A0404,
                   kErrInvalidWaveformFrame = 0x804A0405,
                   kErrInvalidWaveformAddress = 0x804A0406,
                   kErrInvalidWaveformBlock = 0x804A0409, kErrInvalidEnvelopeCurve = 0x804A0500,
                   kErrInvalidPeakMeterMode = 0x804A0510, kErrInvalidFilterIndex = 0x804A0600,
                   kErrInvalidFilterLocation = 0x804A0602, kErrInvalidLfeCutoff = 0x804A0603,
                   kErrInvalidMatrixIndex = 0x804A0700, kErrInvalidReverbSize = 0x804A0900;

constexpr uint32_t kRackSampler = 0x1000, kRackSubmixer = 0x2000, kRackReverb = 0x2001,
                   kRackMastering = 0x3000;
constexpr uint32_t kForever = 0xFFFFFFFF;

// Voice state flags (SceNgs2VoiceState's).
constexpr uint8_t kInUse = 1, kPlaying = 2, kPaused = 4, kStopped = 8, kEmpty = 0x20;
// Events, as pending bits.
constexpr uint8_t kEvPlay = 1, kEvStop = 2, kEvStopImm = 4, kEvKill = 8, kEvPause = 0x10,
                  kEvResume = 0x20;
// Fades applied by the render: 1 out at the grain's end (pausing), 2 in
// at its start (resuming), 3 silence (paused), 4 and 5 out at its start
// (stopping, killed).
constexpr int kFadeLen = 64;

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
float Clamp(float v, float lo, float hi) { return v < lo ? lo : std::min(v, hi); }
float RdF(gaddr va) {
  const uint32_t u = Rd32(va);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void WrF(gaddr va, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  Wr32(va, u);
}

// The game's compiled SDK version (its process parameters'): some checks
// and defaults depend on it.
uint32_t SdkVersion() {
  static const uint32_t v = [] {
    const gaddr p = recomp::orbis::ProcParam();
    return p ? Rd32(p + 0x10) : 0u;
  }();
  return v;
}

struct System;
struct Rack;
struct Voice;

struct Input {
  int ch = 0;
  std::vector<float> ring;  // blocks x 8 channels x maxGrain
  std::vector<bool> written;
};

struct Port {
  bool used = false, patched = false;
  Voice* dst = nullptr;
  uint32_t input = 0;
  int matrix = -1;
  float volume = 1;
  uint32_t delay = 0;
  float prev[kMaxChannels][kMaxChannels] = {};
  bool fresh = true;
};

struct Block {
  gaddr base;
  uint64_t user;
  uint32_t offset, size, repeats, skip, samples;
  bool stream, skipped;
};

struct Filter {
  bool set = false;
  uint32_t location = 0, bypass = 0;
  Biquad q;
};

struct Voice {
  Rack* rack;
  uint32_t index;
  gaddr handle;

  // The control word: published flags, pending events, "played, not
  // rendered yet" and "set up again".
  uint8_t flags = 0, pending = 0;
  bool just_played = false, setup_changed = false;
  // The render's copy.
  uint8_t rflags = 0, rpending = 0;
  int fade = 0;
  bool reset = true;  // the first grain after play

  float matrix[16][kMaxChannels * kMaxChannels] = {};
  uint16_t matrix_set = 0;
  Port ports[16];
  std::vector<Input> inputs;
  int in_ch = 0, out_ch = 0;  // as set up; 0: never

  // Modules: whether a parameter was ever set enables them.
  bool env_set = false, dyn_set = false, dist_set = false, meter_set = false;
  Envelope env;
  Dynamics dyn;
  Distortion dist;
  PeakMeter meter, meter_post;
  Filter filters[16];

  // Sampler.
  bool wave_set = false;
  uint32_t wtype = 0, wch = 0, wrate = 0, frame_offset = 0, frame_margin = 0;
  std::vector<Block> blocks;
  bool terminated = false;
  uint32_t exit_loops = 0;
  bool have_block = false, starved = false, ended = false;
  uint32_t span = 0, byte = 0, repeat = 0;
  uint64_t pos = 0, end = 0;
  uint64_t decoded_samples = 0, decoded_bytes = 0, block_user = 0;
  gaddr read_addr = 0;
  float ratio = 1;
  uint32_t step = 0x4000, frac = 0;
  std::vector<float> fifo[kMaxChannels];
  uint32_t tail_zeros = 0;

  // Reverb.
  std::unique_ptr<Reverb> reverb;

  // Mastering.
  float mmatrix[16][kMaxChannels][kMaxChannels] = {}, mprev[16][kMaxChannels][kMaxChannels] = {};
  uint16_t mmatrix_set = 0;
  bool lfe_set = false, gain_set = false, output_set = false;
  LfeFilter lfe;
  float gain[kMaxChannels] = {}, gain_prev[kMaxChannels] = {};
  uint32_t output = 0;

  uint8_t state[0x4c] = {}, back[0x4c] = {};
};

struct Rack {
  System* sys;
  uint32_t id;
  int kind;  // 0 sources, 1 submixers and reverbs, 2 mastering
  gaddr handle, buffer_info;
  uint8_t info[0x40];
  gaddr free_fn = 0;
  uint32_t flags, max_grain, max_voices, max_delay_blocks, max_matrices, max_ports;
  uint32_t max_channels = 8, max_env_points = 0, max_filters = 0, max_inputs = 0,
           meter_blocks = 8, max_wave_blocks = 0, reverb_size = 0;
  uint32_t state_size;
  std::vector<std::unique_ptr<Voice>> voices;
  std::vector<Voice*> active;  // sources being played, in play order
  uint32_t ring_cur = 0;
};

struct System {
  gaddr handle;
  uint8_t info[0x40];
  gaddr free_fn = 0;
  uint32_t max_grain, grain, rate;
  uint32_t win_min = 64, win_max;
  std::vector<Rack*> lists[3];
  std::vector<Voice*> ordered;  // submixer, reverb and mastering voices
  bool dirty = false;
};

std::mutex g_mu;
std::map<gaddr, std::unique_ptr<System>> g_systems;
std::map<gaddr, std::unique_ptr<Rack>> g_racks;
std::map<gaddr, Voice*> g_voices;

System* FindSystem(gaddr h) {
  auto it = g_systems.find(h);
  return it == g_systems.end() ? nullptr : it->second.get();
}
Rack* FindRack(gaddr h) {
  auto it = g_racks.find(h);
  return it == g_racks.end() ? nullptr : it->second.get();
}
Voice* FindVoice(gaddr h) {
  auto it = g_voices.find(h);
  return it == g_voices.end() ? nullptr : it->second;
}

// Calls a guest allocator callback with a SceNgs2ContextBufferInfo (0x40
// bytes: hostBuffer, hostBufferSize, 5 reserved words, userData) put
// below the caller's stack pointer.
int32_t CallBufferHandler(recomp::Ctx& c, gaddr fn, uint8_t info[0x40]) {
  const uint64_t saved = c.rsp;
  const gaddr p = (c.rsp - 0x100) & ~uint64_t(15);
  std::memcpy(recomp::GPtr(p), info, 0x40);
  c.rsp = p;
  const int32_t r = int32_t(recomp::CallGuest(c, fn, {p}));
  c.rsp = saved;
  std::memcpy(info, recomp::GPtr(p), 0x40);
  return r;
}
uint64_t InfoU64(const uint8_t* info, int off) {
  uint64_t v;
  std::memcpy(&v, info + off, 8);
  return v;
}

// ---- system option (SceNgs2SystemOption, 0x40 bytes)

struct SystemConfig {
  uint32_t max_grain = 512, grain = 256, rate = 48000;
};

uint32_t ParseSystemOption(gaddr opt, SystemConfig& s) {
  if (!opt) return 0;
  if (Rd64(opt) != 0x40) return kErrInvalidOptionSize;
  s.max_grain = Rd32(opt + 0x1c);
  s.grain = Rd32(opt + 0x20);
  s.rate = Rd32(opt + 0x24);
  auto grain_ok = [](uint32_t g) { return g >= 64 && g <= 1024 && !(g & 63); };
  if (!grain_ok(s.max_grain)) return kErrInvalidMaxGrain;
  if (!grain_ok(s.grain)) return kErrInvalidNumGrain;
  for (uint32_t r : {11025u, 12000u, 22050u, 24000u, 44100u, 48000u, 88200u, 96000u, 176400u,
                     192000u})
    if (s.rate == r) return 0;
  return kErrInvalidSampleRate;
}

uint64_t SystemBufferSize(const SystemConfig& s) { return 0x410 + uint64_t(s.max_grain) * 0x80; }

// ---- rack options (SceNgs2RackOption, 0x80 bytes, then the rack's own)

struct RackConfig {
  int kind = 0;
  uint32_t flags = 0, max_grain = 512, max_voices = 1, max_delay_blocks = 0, max_matrices = 1,
           max_ports = 8;
  uint32_t max_channels = 8, max_env_points = 0, max_filters = 0, max_inputs = 0,
           meter_blocks = 8, max_wave_blocks = 0, reverb_size = 0;
};

int RackKind(uint32_t id) {
  switch (id) {
    case kRackSampler: return 0;
    case kRackSubmixer:
    case kRackReverb: return 1;
    case kRackMastering: return 2;
    case 0x4001:
    case 0x4002:
    case 0x4003: recomp::FatalF("libSceNgs2: custom racks (id %X) are not implemented yet", id);
    default: return -1;
  }
}

uint32_t ParseRackOption(uint32_t id, gaddr opt, RackConfig& r) {
  r.kind = RackKind(id);
  static const uint32_t kDelay[3] = {0, 1, 1}, kMatrices[3] = {1, 1, 0}, kPorts[3] = {8, 8, 0};
  r.max_delay_blocks = kDelay[r.kind], r.max_matrices = kMatrices[r.kind],
  r.max_ports = kPorts[r.kind];
  const uint64_t size = id == kRackSampler ? 0xA8 : id == kRackSubmixer ? 0x98 : 0x88;
  // Defaults, then the option's fields in the order they are checked.
  switch (id) {
    case kRackSampler:
      r.max_voices = 0x100, r.max_wave_blocks = 4, r.max_env_points = 4, r.max_filters = 8;
      break;
    case kRackSubmixer: r.max_env_points = 4, r.max_filters = 8, r.max_inputs = 1; break;
    case kRackReverb: r.reverb_size = 1, r.max_inputs = 1; break;
    case kRackMastering: r.max_voices = 2, r.max_inputs = 1; break;
  }
  if (!opt) return 0;
  if (Rd64(opt) != size) return kErrInvalidOptionSize;
  r.flags = Rd32(opt + 0x18);
  if (r.flags & 0x3FFFFFFF) return kErrInvalidOptionFlag;
  r.max_grain = Rd32(opt + 0x1c);
  r.max_voices = Rd32(opt + 0x20);
  auto in = [](uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; };
  if (!in(r.max_grain, 64, 1024)) return kErrInvalidMaxGrain;
  switch (id) {
    case kRackSampler: {
      if (!in(r.max_voices, 1, 0x400)) return kErrInvalidMaxVoices;
      const uint32_t works = Rd32(opt + 0x80), a9 = Rd32(opt + 0x94), ajm = Rd32(opt + 0x9c);
      if (!in(works, r.max_voices, r.max_voices * 8)) return kErrInvalidMaxChannelWorks;
      if (a9 > r.max_voices) return kErrInvalidMaxAtrac9Decoders;
      r.max_wave_blocks = Rd32(opt + 0x88);
      if (!in(r.max_wave_blocks, 1, 16)) return kErrInvalidMaxWaveformBlocks;
      r.max_env_points = Rd32(opt + 0x8c);
      if (r.max_env_points > 16) return kErrInvalidMaxEnvelopePoints;
      r.max_filters = Rd32(opt + 0x90);
      if (r.max_filters > 16) return kErrInvalidMaxFilters;
      if (ajm > std::min(r.max_voices, 256u)) return kErrInvalidMaxAtrac9Decoders;
      r.meter_blocks = Rd32(opt + 0xa0);
      if (!in(r.meter_blocks, 1, 256)) return kErrInvalidPeakMeterBlocks;
      if (a9 && !in(Rd32(opt + 0x98), a9, a9 * 8)) return kErrInvalidMaxAtrac9ChannelWorks;
      if (ajm && !in(Rd32(opt + 0x84), 10, ajm * 8)) return kErrInvalidMaxCodecCaches;
      break;
    }
    case kRackSubmixer:
      if (!in(r.max_voices, 1, 1024)) return kErrInvalidMaxVoices;
      r.max_channels = Rd32(opt + 0x80);
      if (!in(r.max_channels, 1, 8)) return kErrInvalidMaxChannels;
      r.max_env_points = Rd32(opt + 0x84);
      if (r.max_env_points > 16) return kErrInvalidMaxEnvelopePoints;
      r.max_filters = Rd32(opt + 0x88);
      if (r.max_filters > 16) return kErrInvalidMaxFilters;
      r.max_inputs = Rd32(opt + 0x8c);
      if (!in(r.max_inputs, 1, 2)) return kErrInvalidMaxInputs;
      r.meter_blocks = Rd32(opt + 0x90);
      if (!in(r.meter_blocks, 1, 256)) return kErrInvalidPeakMeterBlocks;
      break;
    case kRackReverb:
      if (!in(r.max_voices, 1, 16)) return kErrInvalidMaxVoices;
      r.max_channels = Rd32(opt + 0x80);
      if (!in(r.max_channels, 1, 8)) return kErrInvalidMaxChannels;
      r.reverb_size = Rd32(opt + 0x84);
      if (r.reverb_size > 1) return kErrInvalidReverbSize;
      break;
    case kRackMastering:
      if (!in(r.max_voices, 1, 256)) return kErrInvalidMaxVoices;
      r.max_channels = Rd32(opt + 0x80);
      if (!in(r.max_channels, 1, 8)) return kErrInvalidMaxChannels;
      r.meter_blocks = Rd32(opt + 0x84);
      if (!in(r.meter_blocks, 1, 256)) return kErrInvalidPeakMeterBlocks;
      break;
  }
  if (r.max_grain & 63) return kErrInvalidMaxGrain;
  r.max_delay_blocks = Rd32(opt + 0x24);
  if (r.kind ? !in(r.max_delay_blocks, 1, 1024) : r.max_delay_blocks != 0)
    return kErrInvalidMaxDelayBlocks;
  r.max_matrices = Rd32(opt + 0x28);
  if (r.kind == 2 ? r.max_matrices != 0 : !in(r.max_matrices, 1, 16))
    return kErrInvalidMaxMatrices;
  r.max_ports = Rd32(opt + 0x2c);
  if (r.kind == 2 ? r.max_ports != 0 : !in(r.max_ports, 1, 16)) return kErrInvalidMaxPorts;
  return 0;
}

// The rack object (0x1040 bytes, its handle at +0x70) and its voice
// objects (0xc0 bytes each, a voice's handle its address) are placed in
// the buffer as 11.00 places them; the voices' work memory lives on the
// host, so the buffer is no bigger.
constexpr uint64_t kRackObject = 0x1040, kVoiceObject = 0xc0;
uint64_t RackBufferSize(const RackConfig& r) {
  return 0x10 + kRackObject + r.max_voices * kVoiceObject + 8;
}

int InputsOf(const Rack& r) { return r.kind == 0 ? 0 : int(r.max_inputs); }

// ---- voices

void ResetPort(Port& p) {
  p.patched = false, p.dst = nullptr, p.input = 0, p.fresh = true;
}

void ResetModules(Voice& v) {
  v.reset = true;
  v.env.Reset();
  v.dyn.Reset();
  v.meter.Reset(int(v.rack->meter_blocks));
  v.meter_post.Reset(int(v.rack->meter_blocks));
  for (Filter& f : v.filters) f.q.Reset();
  v.lfe.Reset();
  for (Port& p : v.ports) p.fresh = true;
  if (v.reverb) v.reverb->reset_ = true;
  for (int c = 0; c < kMaxChannels; c++) v.fifo[c].assign(4, 0.0f);
  v.tail_zeros = 0, v.frac = 0;
  v.have_block = v.starved = v.ended = false;
  std::memcpy(v.mprev, v.mmatrix, sizeof v.mprev);
  std::memcpy(v.gain_prev, v.gain, sizeof v.gain);
}

void AllocInputs(Voice& v) {
  const Rack& r = *v.rack;
  v.inputs.assign(InputsOf(r), Input{});
  const int blocks = int(r.max_delay_blocks) + 1;
  int ch = v.in_ch;
  if (r.id == kRackMastering) ch = std::max(ch, int(r.max_channels));
  if (r.id == kRackReverb) ch = kMaxChannels;
  for (Input& in : v.inputs) {
    in.ch = ch;
    in.ring.assign(size_t(blocks) * kMaxChannels * r.max_grain, 0.0f);
    in.written.assign(blocks, false);
  }
}

// A voice that ended: its inputs are released and its ports unpatched.
void FreeVoice(System& s, Voice& v) {
  v.rflags = 0, v.flags = 0;
  v.inputs.clear();
  for (Port& p : v.ports) ResetPort(p);
  auto& a = v.rack->active;
  a.erase(std::remove(a.begin(), a.end(), &v), a.end());
  s.ordered.erase(std::remove(s.ordered.begin(), s.ordered.end(), &v), s.ordered.end());
  s.dirty = true;
}

// Drops a rack from its system: patches into it and out of it go, its
// voices leave the render lists.
void DetachRack(Rack& r) {
  System* s = r.sys;
  if (!s) return;
  for (int k = 0; k < 3; k++)
    for (Rack* o : s->lists[k])
      for (auto& v : o->voices)
        for (Port& p : v->ports)
          if (p.dst && (p.dst->rack == &r || o == &r)) ResetPort(p);
  for (auto& v : r.voices)
    s->ordered.erase(std::remove(s->ordered.begin(), s->ordered.end(), v.get()),
                     s->ordered.end());
  r.active.clear();
  auto& l = s->lists[r.kind];
  l.erase(std::remove(l.begin(), l.end(), &r), l.end());
  s->win_max = s->max_grain;
  for (auto& list : s->lists)
    for (Rack* o : list) s->win_max = std::min(s->win_max, o->max_grain);
  s->dirty = true;
  r.sys = nullptr;
}

// ---- voice parameters

struct Param {
  gaddr p;
  uint16_t size;
  uint32_t id;
};

uint32_t PortCheck(Voice& v, uint32_t port) {
  return port < v.rack->max_ports ? 0 : kErrInvalidPort;
}
Port& TouchPort(Voice& v, uint32_t port) {
  Port& p = v.ports[port];
  if (!p.used) {
    p = Port{};
    p.used = true;
  }
  return p;
}

uint32_t SetEnvelope(Voice& v, gaddr p, uint32_t max_points) {
  const uint32_t fwd = Rd32(p + 8), rel = Rd32(p + 0xc), n = fwd + rel;
  const gaddr pts = Rd64(p + 0x10);
  if (!pts && n) return kErrInvalidEnvelopePointAddress;
  if (n > max_points) return kErrInvalidEnvelopePoints;
  std::vector<Envelope::Point> points(n);
  for (uint32_t i = 0; i < n; i++) {
    points[i] = {Rd32(pts + i * 12), Rd32(pts + i * 12 + 4), Clamp(RdF(pts + i * 12 + 8), 0, 1)};
    if (points[i].curve > 5) return kErrInvalidEnvelopeCurve;
  }
  v.env.points = std::move(points);
  v.env.forward = fwd, v.env.release = rel;
  v.env_set = true;
  return 0;
}

uint32_t SetFilter(Voice& v, gaddr p) {
  const uint32_t index = Rd32(p + 8), location = Rd32(p + 0xc);
  if (index >= v.rack->max_filters) return kErrInvalidFilterIndex;
  if (location >= 18) return kErrInvalidFilterLocation;
  Filter& f = v.filters[index];
  f.set = true, f.location = location, f.bypass = Rd32(p + 0x14) & 0xFF;
  f.q.type = Rd32(p + 0x10);
  for (int i = 0; i < 5; i++) f.q.p[i] = RdF(p + 0x18 + 4 * i);
  f.q.Design(float(v.rack->sys ? v.rack->sys->rate : 48000));
  return 0;
}

uint32_t SetDistortion(Voice& v, gaddr p) {
  Distortion& d = v.dist;
  d.enabled = !(Rd32(p + 8) & 1);
  d.a = Clamp(RdF(p + 0xc), 0, 10), d.b = Clamp(RdF(p + 0x10), 0, 10);
  d.clip = Clamp(RdF(p + 0x14), 0, 1), d.gate = Clamp(RdF(p + 0x18), 0, 1);
  d.wet = Clamp(RdF(p + 0x1c), 0, 32), d.dry = Clamp(RdF(p + 0x20), 0, 32);
  v.dist_set = true;
  return 0;
}

uint32_t SetMeter(Voice& v, gaddr p) {
  const uint32_t mode = Rd32(p + 8);
  if (mode > 2) return kErrInvalidPeakMeterMode;
  v.meter.mode = v.meter_post.mode = mode;
  v.meter_set = true;
  return 0;
}

[[noreturn]] void NotImplemented(const char* what, uint32_t id) {
  recomp::FatalF("libSceNgs2: %s (voice parameter %X) is not implemented yet", what, id);
}

// Sampler parameters (0x1000000x).
uint32_t SamplerParam(Voice& v, const Param& q) {
  static const uint16_t kSize[11] = {0x28, 0x20, 0x18, 0x10, 8, 0x10, 0x18, 0x28, 0x28, 0x10, 0x30};
  if (q.id >= 0x10008000 && q.id <= 0x1000800B) NotImplemented("reading a sampler parameter", q.id);
  if (q.id < 0x10000000 || q.id > 0x1000000A) return kErrInvalidControl;
  if (q.size != kSize[q.id - 0x10000000]) return kErrInvalidParamSize;
  const gaddr p = q.p;
  const Rack& r = *v.rack;
  switch (q.id) {
    case 0x10000000: {  // setup: waveform format
      const uint32_t type = Rd32(p + 8), ch = Rd32(p + 0xc), rate = Rd32(p + 0x10);
      const uint32_t offset = Rd32(p + 0x18), margin = Rd32(p + 0x1c);
      if (!((type >= 0x10 && type <= 0x1c) || type == 0x40 || (type >= 0x80 && type <= 0x87)))
        return kErrInvalidWaveformType;
      if (ch - 1 >= 8) return kErrInvalidChannels;
      if (rate - 1 >= 192000) return kErrInvalidWaveformRate;
      if (offset > margin || margin > 0xFFFF) return kErrInvalidWaveformFrame;
      if (type == 0x1c) recomp::FatalF("libSceNgs2: VAG waveforms are not implemented yet");
      if (type == 0x40) recomp::FatalF("libSceNgs2: ATRAC9 waveforms are not implemented yet");
      if (type >= 0x80) recomp::FatalF("libSceNgs2: oscillator waveforms are not implemented yet");
      v.wtype = type, v.wch = ch, v.wrate = rate, v.frame_offset = offset, v.frame_margin = margin;
      v.wave_set = true;
      v.in_ch = v.out_ch = int(ch);
      v.blocks.clear();
      v.terminated = false, v.exit_loops = 0, v.have_block = false;
      v.ratio = 1;
      v.step = uint32_t(std::min(16384.0 * v.wrate / (r.sys ? r.sys->rate : 48000), 65535.0));
      v.setup_changed = true;
      return 0;
    }
    case 0x10000001: {  // waveform blocks
      const gaddr data = Rd64(p + 8), list = Rd64(p + 0x18);
      const uint32_t flags = Rd32(p + 0x10), n = Rd32(p + 0x14);
      if (!list && n) return kErrInvalidBlockAddress;
      if (flags & 4) {
        v.blocks.clear();
        v.terminated = false, v.exit_loops = 0, v.have_block = false;
        for (auto& f : v.fifo) f.assign(4, 0.0f);
        v.tail_zeros = 0, v.frac = 0;
      } else if (v.terminated) {
        return kErrWaveformBlocks;
      }
      if (n && !data) return kErrInvalidWaveformAddress;
      std::vector<Block> add;
      for (uint32_t i = 0; i < n; i++) {
        const gaddr b = list + i * 0x20;
        Block k{data, Rd64(b + 0x18), Rd32(b), Rd32(b + 4), Rd32(b + 8), Rd32(b + 0xc),
                Rd32(b + 0x10), (flags & 2) != 0, false};
        if (SdkVersion() >= 0x01700000 && k.samples == 0 && k.repeats == kForever)
          return kErrInvalidWaveformBlock;
        if (k.stream) {
          k.repeats = k.skip = k.samples = 0, k.user = 0;
          const Block* last = !add.empty() ? &add.back() : !v.blocks.empty() ? &v.blocks.back() : nullptr;
          if (!(flags & 0x10) && last && last->base + last->offset + last->size == data + k.offset) {
            (!add.empty() ? add.back() : v.blocks.back()).size += k.size;
            continue;
          }
        }
        add.push_back(k);
      }
      size_t pending = 0;
      for (const Block& b : v.blocks) pending += !b.skipped;
      if (pending + add.size() > r.max_wave_blocks) return kErrWaveformBlocks;
      if ((flags & 8) && !(flags & 4))
        for (size_t i = v.have_block ? 1 : 0; i < v.blocks.size(); i++) v.blocks[i].skipped = true;
      v.blocks.insert(v.blocks.end(), add.begin(), add.end());
      if (!(flags & 1)) v.terminated = true;
      return 0;
    }
    case 0x10000002: {  // waveform address
      const gaddr from = Rd64(p + 8), to = Rd64(p + 0x10);
      if (!from || !to) return kErrInvalidWaveformAddress;
      for (Block& b : v.blocks)
        if (b.base == from) b.base = to;
      return 0;
    }
    case 0x10000003:
      if (Rd32(p + 8) >= 0x10000) return kErrInvalidWaveformFrame;
      v.frame_offset = Rd32(p + 8);
      return 0;
    case 0x10000004: v.exit_loops++; return 0;
    case 0x10000005:  // pitch
      v.ratio = Clamp(RdF(p + 8), 0, 4);
      v.step = uint32_t(std::min(double(v.ratio) * 16384.0 * v.wrate / (r.sys ? r.sys->rate : 48000),
                                 65535.0));
      return 0;
    case 0x10000006: return SetEnvelope(v, p, r.max_env_points);
    case 0x10000007: return SetDistortion(v, p);
    case 0x10000008: NotImplemented("a sampler user effect", q.id);
    case 0x10000009: return SetMeter(v, p);
    case 0x1000000A: return SetFilter(v, p);
  }
  return kErrInvalidControl;
}

// Submixer parameters (0x2000000x).
uint32_t SubmixerParam(Voice& v, const Param& q) {
  static const uint16_t kSize[7] = {0x10, 0x18, 0x28, 0x28, 0x28, 0x10, 0x30};
  if (q.id >= 0x20008000 && q.id <= 0x20008007) NotImplemented("reading a submixer parameter", q.id);
  if (q.id < 0x20000000 || q.id > 0x20000006) return kErrInvalidControl;
  if (q.size != kSize[q.id - 0x20000000]) return kErrInvalidParamSize;
  const gaddr p = q.p;
  switch (q.id) {
    case 0x20000000: {
      const uint32_t ch = Rd32(p + 8);
      if (ch > v.rack->max_channels || !(ch == 1 || ch == 2 || ch == 6 || ch == 8))
        return kErrInvalidChannels;
      v.in_ch = v.out_ch = int(ch);
      v.setup_changed = true;
      return 0;
    }
    case 0x20000001: return SetEnvelope(v, p, v.rack->max_env_points);
    case 0x20000002: {
      Dynamics& d = v.dyn;
      d.flags = Rd32(p + 8) & 0xF;
      if (v.rack->max_inputs < 2) d.flags &= ~Dynamics::kSideChain;
      d.threshold = Clamp(RdF(p + 0xc), 0, 1), d.ratio = Clamp(RdF(p + 0x10), 0, 1);
      d.knee = Clamp(RdF(p + 0x14), 0, 1), d.attack = Clamp(RdF(p + 0x18), 0, 10);
      d.release = Clamp(RdF(p + 0x1c), 0, 10), d.gain = Clamp(RdF(p + 0x20), 0, 32);
      d.Design(float(v.rack->sys ? v.rack->sys->rate : 48000));
      v.dyn_set = true;
      return 0;
    }
    case 0x20000003: return SetDistortion(v, p);
    case 0x20000004: NotImplemented("a submixer user effect", q.id);
    case 0x20000005: return SetMeter(v, p);
    case 0x20000006: return SetFilter(v, p);
  }
  return kErrInvalidControl;
}

// Reverb parameters (0x2001000x).
uint32_t ReverbParam(Voice& v, const Param& q) {
  if (q.id == 0x20018000 || q.id == 0x20018001) NotImplemented("reading a reverb parameter", q.id);
  if (q.id == 0x20010002) NotImplemented("custom reverb parameters", q.id);
  if (q.id != 0x20010000 && q.id != 0x20010001) return kErrInvalidControl;
  if (q.size != (q.id == 0x20010000 ? 0x18 : 0x60)) return kErrInvalidParamSize;
  const gaddr p = q.p;
  auto ok = [&](uint32_t ch) {
    return ch <= v.rack->max_channels && (ch == 1 || ch == 2 || ch == 6 || ch == 8);
  };
  if (q.id == 0x20010000) {
    const uint32_t in = Rd32(p + 8), out = Rd32(p + 0xc);
    if (!ok(in) || !ok(out)) return kErrInvalidChannels;
    v.in_ch = int(in), v.out_ch = int(out);
    v.reverb->Set(Reverb::I3dl2{});
    v.setup_changed = true;
    return 0;
  }
  Reverb::I3dl2 r;
  const float wet_max = SdkVersion() >= 0x06000000 ? 32 : 1;
  auto mb = [&](int off, int32_t lo, int32_t hi) {
    return std::clamp(int32_t(Rd32(p + off)), lo, hi);
  };
  r.wet = Clamp(RdF(p + 8), 0, wet_max), r.dry = Clamp(RdF(p + 0xc), 0, wet_max);
  r.room = std::max(int32_t(Rd32(p + 0x10)), -10000);
  r.room_hf = mb(0x14, -10000, 0);
  r.pattern = Rd32(p + 0x18) >= 16 ? 0 : Rd32(p + 0x18);
  r.decay_time = Clamp(RdF(p + 0x1c), 0.1f, 20), r.decay_hf_ratio = Clamp(RdF(p + 0x20), 0.01f, 2);
  r.reflections = mb(0x24, -10000, 1000);
  r.reflections_delay = Clamp(RdF(p + 0x28), 0, 0.3f);
  r.reverb = mb(0x2c, -10000, 2000);
  r.reverb_delay = Clamp(RdF(p + 0x30), 0, 0.1f);
  r.diffusion = Clamp(RdF(p + 0x34), 0, 100), r.density = Clamp(RdF(p + 0x38), 0, 100);
  r.hf_reference = Clamp(RdF(p + 0x3c), 20, 20000);
  v.reverb->Set(r);
  return 0;
}

int LayoutOf(int ch) { return ch == 1 ? 0 : ch == 2 ? 1 : ch == 6 ? 2 : 3; }
constexpr int kLayoutChannels[4] = {1, 2, 6, 8};

void MasteringDefaults(Voice& v) {
  for (int t = 0; t < 16; t++) {
    if (t >> 2 == (t & 3)) continue;
    float m[kMaxChannels][kMaxChannels];
    DefaultMatrix(kLayoutChannels[t >> 2], kLayoutChannels[t & 3], m);
    std::memcpy(v.mmatrix[t], m, sizeof m);
    v.mmatrix_set |= uint16_t(1 << t);
  }
  v.lfe.enabled = true, v.lfe.cutoff = 120, v.lfe_set = true;
  v.lfe.Design(float(v.rack->sys ? v.rack->sys->rate : 48000));
  v.dyn = Dynamics{};
  v.dyn.Design(float(v.rack->sys ? v.rack->sys->rate : 48000));
  v.dyn_set = true;
  v.output = 0, v.output_set = true;
}

// Mastering parameters (0x3000000x).
uint32_t MasteringParam(Voice& v, const Param& q) {
  if (q.id >= 0x30008000 && q.id <= 0x30008006) NotImplemented("reading a mastering parameter", q.id);
  if (q.id < 0x30000000 || q.id > 0x30000006) return kErrInvalidControl;
  if (q.size != (q.id == 0x30000001 ? 0x18 : 0x10)) return kErrInvalidParamSize;
  const gaddr p = q.p;
  const float rate = float(v.rack->sys ? v.rack->sys->rate : 48000);
  switch (q.id) {
    case 0x30000000: {
      const uint32_t ch = Rd32(p + 8);
      if (ch > v.rack->max_channels || !(ch == 1 || ch == 2 || ch == 6 || ch == 8))
        return kErrInvalidChannels;
      v.in_ch = v.out_ch = int(ch);
      MasteringDefaults(v);
      v.setup_changed = true;
      return 0;
    }
    case 0x30000001: {
      const uint32_t type = Rd32(p + 8), n = Rd32(p + 0xc);
      const gaddr levels = Rd64(p + 0x10);
      if (type >= 16) return kErrInvalidMatrixIndex;
      if (!levels && n) return kErrInvalidMatrixLevelAddress;
      if (n > 64) return kErrInvalidMatrixLevels;
      const int rows = kLayoutChannels[type >> 2], cols = kLayoutChannels[type & 3];
      std::memset(v.mmatrix[type], 0, sizeof v.mmatrix[type]);
      for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
          const uint32_t i = uint32_t(r * cols + c);
          v.mmatrix[type][r][c] = i < n ? Clamp(RdF(levels + 4 * i), -4, 4) : 0;
        }
      v.mmatrix_set |= uint16_t(1 << type);
      return 0;
    }
    case 0x30000002: {
      const uint32_t fc = Rd32(p + 0xc);
      if (fc != 80 && fc != 120 && fc != 240) return kErrInvalidLfeCutoff;
      v.lfe.enabled = Rd32(p + 8) != 0, v.lfe.cutoff = fc, v.lfe_set = true;
      v.lfe.Design(rate);
      return 0;
    }
    case 0x30000003:
      v.dyn.flags = Rd32(p + 8) ? v.dyn.flags & ~Dynamics::kBypass : v.dyn.flags | Dynamics::kBypass;
      v.dyn.threshold = Clamp(RdF(p + 0xc), 0, 4);
      v.dyn_set = true;
      return 0;
    case 0x30000004: {
      const float fbw = Clamp(RdF(p + 8), 0, 4), lfe = Clamp(RdF(p + 0xc), 0, 4);
      for (int c = 0; c < kMaxChannels; c++) v.gain[c] = c == kLfe ? lfe : fbw;
      if (!v.gain_set) std::memcpy(v.gain_prev, v.gain, sizeof v.gain);
      v.gain_set = true;
      return 0;
    }
    case 0x30000005: v.output = Rd32(p + 8), v.output_set = true; return 0;
    case 0x30000006: return SetMeter(v, p);
  }
  return kErrInvalidControl;
}

// The generic parameters (1-7), then the rack's own.
uint32_t ApplyParam(Voice& v, const Param& q) {
  const gaddr p = q.p;
  Rack& r = *v.rack;
  auto size = [&](uint16_t want) { return q.size == want ? 0 : kErrInvalidParamSize; };
  switch (q.id) {
    case 1: {  // matrix levels
      if (uint32_t e = size(0x18)) return e;
      const uint32_t m = Rd32(p + 8), n = Rd32(p + 0xc);
      const gaddr levels = Rd64(p + 0x10);
      if (m >= r.max_matrices) return kErrInvalidMatrixIndex;
      if (!levels && n) return kErrInvalidMatrixLevelAddress;
      if (n - 1 > 63) return kErrInvalidMatrixLevels;
      for (uint32_t i = 0; i < n; i++) v.matrix[m][i] = Clamp(RdF(levels + 4 * i), -4, 4);
      v.matrix_set |= uint16_t(1 << m);
      return 0;
    }
    case 2: {  // port volume
      if (uint32_t e = size(0x10)) return e;
      const uint32_t port = Rd32(p + 8);
      if (uint32_t e = PortCheck(v, port)) return e;
      TouchPort(v, port).volume = Clamp(RdF(p + 0xc), -4, 4);
      return 0;
    }
    case 3: {  // port matrix
      if (uint32_t e = size(0x10)) return e;
      const uint32_t port = Rd32(p + 8);
      if (uint32_t e = PortCheck(v, port)) return e;
      const int32_t m = int32_t(Rd32(p + 0xc));
      if (m != -1 && (m < 0 || m > int32_t(r.max_matrices)) && (m & ~3) != 0x10)
        return kErrInvalidMatrixIndex;
      if ((m & ~3) == 0x10) NotImplemented("the alternative default matrices", q.id);
      TouchPort(v, port).matrix = m;
      return 0;
    }
    case 4: {  // port delay
      if (uint32_t e = size(0x10)) return e;
      const uint32_t port = Rd32(p + 8);
      if (uint32_t e = PortCheck(v, port)) return e;
      TouchPort(v, port).delay = std::min(Rd32(p + 0xc), 0xFFFFu);
      return 0;
    }
    case 5: {  // patch
      if (uint32_t e = size(0x18)) return e;
      const uint32_t port = Rd32(p + 8), input = Rd32(p + 0xc);
      const gaddr dest = Rd64(p + 0x10);
      if (uint32_t e = PortCheck(v, port)) return e;
      Port& pt = TouchPort(v, port);
      if (!dest) {
        ResetPort(pt);
      } else {
        // A rack handle stands for its voice 0.
        Voice* d = FindVoice(dest);
        if (!d)
          if (Rack* dr = FindRack(dest)) d = dr->voices[0].get();
        if (!d) return kErrInvalidVoiceHandle;
        if (d->rack->sys != r.sys) return kErrInvalidInput;
        if (input >= uint32_t(InputsOf(*d->rack))) return kErrInvalidInput;
        pt.dst = d, pt.input = input, pt.patched = true, pt.fresh = true;
      }
      if (r.sys) r.sys->dirty = true;
      return 0;
    }
    case 6: {  // event
      if (uint32_t e = size(0xc)) return e;
      const uint32_t ev = Rd32(p + 8);
      if (ev == 0) {
        v.pending |= kEvPlay;
        v.flags |= kInUse;
        v.just_played = true;
        return 0;
      }
      if (!(v.flags & kInUse)) return 0;
      static const uint8_t kBits[6] = {0, kEvStop, kEvStopImm, kEvKill, kEvPause, kEvResume};
      if (ev > 5) return kErrInvalidEvent;
      v.pending |= kBits[ev];
      if (ev == 2 || ev == 3)
        for (Input& in : v.inputs) std::fill(in.written.begin(), in.written.end(), false);
      return 0;
    }
    case 7:  // callback
      if (uint32_t e = size(0x20)) return e;
      if (!(r.flags & 0x40000000)) return kErrNoCallbacks;
      NotImplemented("voice callbacks", q.id);
  }
  switch (r.id) {
    case kRackSampler: return SamplerParam(v, q);
    case kRackSubmixer: return SubmixerParam(v, q);
    case kRackReverb: return ReverbParam(v, q);
    default: return MasteringParam(v, q);
  }
}

// ---- render

thread_local float t_work[kMaxChannels][1024], t_port[kMaxChannels][1024],
    t_side[kMaxChannels][1024];

struct RenderBuffer {
  gaddr buffer;
  uint64_t size;
  uint32_t type, channels;
  bool written;
};

void Fade(Voice& v, float* const* ch, int nch, int n) {
  const int f = v.fade;
  if (!f) return;
  const int m = std::min(kFadeLen, n);
  for (int c = 0; c < nch; c++) {
    float* x = ch[c];
    switch (f) {
      case 1:
        for (int i = 0; i < m; i++) x[n - m + i] *= 1 - float(i) / float(m);
        break;
      case 2:
        for (int i = 0; i < m; i++) x[i] *= float(i) / float(m);
        break;
      case 3: std::memset(x, 0, sizeof(float) * n); break;
      default:
        for (int i = 0; i < m; i++) x[i] *= 1 - float(i) / float(m);
        std::memset(x + m, 0, sizeof(float) * (n - m));
    }
  }
}

void RunFilters(Voice& v, uint32_t location, float* const* ch, int nch, int n) {
  for (uint32_t i = 0; i < v.rack->max_filters; i++) {
    Filter& f = v.filters[i];
    if (f.set && f.location == location) f.q.Process(ch, nch, n, f.bypass);
  }
}

// A voice's output through its ports into the inputs they are patched to.
void SendPorts(Voice& v, float* const* ch, int nch, int n) {
  for (uint32_t pi = 0; pi < v.rack->max_ports; pi++) {
    Port& pt = v.ports[pi];
    if (!pt.used || !pt.patched || !pt.dst || pt.dst->inputs.size() <= pt.input) continue;
    Voice& d = *pt.dst;
    const Rack& dr = *d.rack;
    Input& in = d.inputs[pt.input];
    float* src[kMaxChannels];
    bool filtered = false;
    for (uint32_t i = 0; i < v.rack->max_filters; i++)
      if (v.filters[i].set && v.filters[i].location == 2 + pi) filtered = true;
    for (int c = 0; c < nch; c++) {
      src[c] = ch[c];
      if (filtered) {
        std::memcpy(t_port[c], ch[c], sizeof(float) * n);
        src[c] = t_port[c];
      }
    }
    if (filtered) RunFilters(v, 2 + pi, src, nch, n);

    float m[kMaxChannels][kMaxChannels];
    if (pt.matrix >= 0 && pt.matrix < 16 && (v.matrix_set >> pt.matrix & 1)) {
      for (int s = 0; s < kMaxChannels; s++)
        for (int c = 0; c < kMaxChannels; c++) m[s][c] = v.matrix[pt.matrix][s * kMaxChannels + c];
    } else {
      DefaultMatrix(nch, in.ch, m);
    }
    float end[kMaxChannels][kMaxChannels];
    for (int s = 0; s < kMaxChannels; s++)
      for (int c = 0; c < kMaxChannels; c++) end[s][c] = pt.volume * m[s][c];
    if (pt.fresh) std::memcpy(pt.prev, end, sizeof end);
    pt.fresh = false;

    const int blocks = int(dr.max_delay_blocks) + 1;
    const uint32_t delay = std::min<uint32_t>(pt.delay, uint32_t(std::max(blocks - 2, 0)) * n);
    const int b0 = int((dr.ring_cur + delay / n) % blocks), off = int(delay % n);
    const size_t stride = size_t(kMaxChannels) * dr.max_grain;
    auto block = [&](int b) {
      float* base = in.ring.data() + b * stride;
      if (!in.written[b]) {
        std::memset(base, 0, sizeof(float) * stride);
        in.written[b] = true;
      }
      return base;
    };
    float* first = block(b0);
    float* second = off ? block((b0 + 1) % blocks) : nullptr;
    for (int s = 0; s < nch; s++)
      for (int c = 0; c < in.ch; c++) {
        const float g0 = pt.prev[s][c], g1 = end[s][c];
        if (g0 == 0 && g1 == 0) continue;
        const float* x = src[s];
        for (int i = 0; i < n; i++) {
          const float g = g0 + (g1 - g0) * float(i) / float(n);
          const int at = i + off;
          float* dst = at < n ? first + c * dr.max_grain + at : second + c * dr.max_grain + (at - n);
          *dst += g * x[i];
        }
      }
    std::memcpy(pt.prev, end, sizeof end);
  }
}

// The voice's input `i` for this grain (zero if nothing was sent to it).
void Gather(Voice& v, size_t i, float (*out)[1024], int nch, int n) {
  if (i >= v.inputs.size()) {
    for (int c = 0; c < nch; c++) std::memset(out[c], 0, sizeof(float) * n);
    return;
  }
  Input& in = v.inputs[i];
  const Rack& r = *v.rack;
  const float* base = in.ring.data() + r.ring_cur * size_t(kMaxChannels) * r.max_grain;
  for (int c = 0; c < nch; c++) {
    if (in.written[r.ring_cur] && c < in.ch)
      std::memcpy(out[c], base + c * r.max_grain, sizeof(float) * n);
    else
      std::memset(out[c], 0, sizeof(float) * n);
  }
}

void ConvertFrame(int type, uint32_t ch, const uint8_t* f, float* out) {
  for (uint32_t c = 0; c < ch; c++) {
    switch (type) {
      case 0x10: out[c] = float(int8_t(f[c])) / 128; break;
      case 0x11: out[c] = float(int(f[c]) - 128) / 128; break;
      case 0x12: out[c] = float(int16_t(f[2 * c] | f[2 * c + 1] << 8)) / 32768; break;
      case 0x13: out[c] = float(int16_t(f[2 * c] << 8 | f[2 * c + 1])) / 32768; break;
      case 0x14: case 0x15: {
        const uint8_t* s = f + 3 * c;
        const uint32_t u = type == 0x14 ? uint32_t(s[0]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 24
                                        : uint32_t(s[2]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[0]) << 24;
        out[c] = float(int32_t(u)) / 2147483648.0f;
        break;
      }
      case 0x16: case 0x17: case 0x18: case 0x19: {
        uint32_t u;
        std::memcpy(&u, f + 4 * c, 4);
        if (type & 1) u = __builtin_bswap32(u);
        if (type >= 0x18) std::memcpy(&out[c], &u, 4);
        else out[c] = float(int32_t(u)) / 2147483648.0f;
        break;
      }
      default: {  // 0x1a, 0x1b: double
        uint64_t u;
        std::memcpy(&u, f + 8 * c, 8);
        if (type & 1) u = __builtin_bswap64(u);
        double d;
        std::memcpy(&d, &u, 8);
        out[c] = float(d);
      }
    }
  }
}

uint32_t BytesPerSample(uint32_t type) {
  static const uint8_t kBytes[12] = {1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 8, 8};
  return kBytes[type - 0x10];
}

// Starts the oldest waveform block that has samples to play.
bool NextBlock(Voice& v) {
  while (!v.blocks.empty()) {
    const Block& b = v.blocks.front();
    const bool drop = b.skipped || b.stream || (b.skip == 0 && b.samples == 0) ||
                      (SdkVersion() < 0x01700000 && b.skip && !b.samples && b.repeats == kForever);
    if (!drop) break;
    v.decoded_bytes += b.size;
    v.blocks.erase(v.blocks.begin());
  }
  if (v.blocks.empty()) return false;
  const Block& b = v.blocks.front();
  v.have_block = true;
  v.pos = 0, v.span = 0, v.byte = 0, v.repeat = 0;
  v.end = b.samples == kForever ? ~uint64_t(0) : uint64_t(b.skip) + b.samples;
  v.block_user = b.user;
  v.read_addr = b.base + b.offset;
  return true;
}

void EndBlock(Voice& v) {
  size_t n = 1;
  while (n < v.blocks.size() && v.blocks[n].stream) n++;
  v.blocks.erase(v.blocks.begin(), v.blocks.begin() + n);
  v.have_block = false;
}

// Appends `want` decoded frames per channel to the voice's FIFO, zeros
// past the end of the waveform or while it is starved.
void Decode(Voice& v, uint32_t want) {
  const uint32_t bps = BytesPerSample(v.wtype), stride = v.wch * bps + v.frame_margin;
  float frame[kMaxChannels];
  v.starved = false;
  while (want) {
    if (!v.have_block && !NextBlock(v)) {
      if (v.terminated) v.ended = true;
      else v.starved = true;
      break;
    }
    const Block& b = v.blocks[v.span];
    const uint32_t avail = b.size > v.byte ? (b.size - v.byte) / stride : 0;
    const uint64_t left = v.end - v.pos;
    const uint32_t k = uint32_t(std::min<uint64_t>({want, avail, left}));
    const uint8_t* src = recomp::GPtr(b.base + b.offset + v.byte + v.frame_offset);
    for (uint32_t i = 0; i < k; i++) {
      ConvertFrame(int(v.wtype), v.wch, src + size_t(i) * stride, frame);
      for (uint32_t c = 0; c < v.wch; c++) v.fifo[c].push_back(frame[c]);
    }
    v.pos += k, v.byte += k * stride, want -= k;
    v.decoded_samples += k, v.decoded_bytes += uint64_t(k) * stride;
    v.read_addr = b.base + b.offset + v.byte;
    if (v.pos < v.end && k == avail) {
      // This span's bytes are used up: on into a stream continuation,
      // else wait for one (an open-ended block) or end the block.
      if (v.span + 1 < v.blocks.size() && v.blocks[v.span + 1].stream) {
        v.span++, v.byte = 0;
        continue;
      }
      if (v.end == ~uint64_t(0) && !v.terminated && v.span + 1 >= v.blocks.size()) {
        v.starved = true;
        break;
      }
    } else if (v.pos < v.end) {
      continue;
    }
    const Block& head = v.blocks.front();
    const bool empty = v.pos == 0 && k == 0;  // no whole frame: never loops
    if (!empty && (head.repeats == kForever || v.repeat < head.repeats)) {
      if (v.exit_loops) {
        v.exit_loops--;
      } else {
        v.repeat++, v.pos = 0, v.span = 0, v.byte = 0;
        continue;
      }
    }
    EndBlock(v);
  }
  for (uint32_t c = 0; c < v.wch; c++) v.fifo[c].insert(v.fifo[c].end(), want, 0.0f);
  if (v.ended) v.tail_zeros += want;
}

void RenderSampler(Voice& v, int n) {
  float* ch[kMaxChannels];
  for (int c = 0; c < kMaxChannels; c++) ch[c] = t_work[c];
  const int nch = int(v.wch);
  if (!v.wave_set) {
    std::memset(v.back, 0, sizeof v.back);
    return;
  }
  v.rflags &= ~(kPlaying | kEmpty);
  if (v.rflags & kPaused) {
    for (int c = 0; c < nch; c++) std::memset(ch[c], 0, sizeof(float) * n);
  } else {
    // Resample from the FIFO: 4 taps around each position, Q2.14 steps.
    const uint32_t total = v.frac + v.step * uint32_t(n), consume = total >> 14;
    const uint32_t need = consume + 4;
    if (v.fifo[0].size() < need) Decode(v, uint32_t(need - v.fifo[0].size()));
    for (int c = 0; c < nch; c++) {
      const float* x = v.fifo[c].data();
      if (v.step == 0x4000 && v.frac == 0) {
        std::memcpy(ch[c], x + 1, sizeof(float) * n);
      } else {
        for (int i = 0; i < n; i++) {
          const uint32_t p = v.frac + v.step * uint32_t(i);
          const float* t = ResampleTaps(p >> 5);
          const float* s = x + (p >> 14);
          ch[c][i] = t[0] * s[0] + t[1] * s[1] + t[2] * s[2] + t[3] * s[3];
        }
      }
      v.fifo[c].erase(v.fifo[c].begin(), v.fifo[c].begin() + consume);
    }
    v.frac = total & 0x3FFF;
    if (v.starved) v.rflags |= kEmpty;
    else v.rflags |= kPlaying;
    if (v.ended && v.fifo[0].size() - std::min<size_t>(v.tail_zeros, v.fifo[0].size()) <= 1)
      v.rflags = 0;
    v.tail_zeros = uint32_t(std::min<size_t>(v.tail_zeros, v.fifo[0].size()));
  }
  RunFilters(v, 0, ch, nch, n);
  if (v.env_set) {
    if ((v.rpending & kEvStop) && v.env.phase != 2) {
      v.env.Release();
      if (v.fade == 4) v.fade = 0;
    }
    if (!(v.rflags & kPaused) && !v.env.Process(ch, nch, n)) v.rflags = 0;
  }
  v.dist.Process(ch, nch, n);
  RunFilters(v, 1, ch, nch, n);
  if (!(v.rflags & kPaused)) v.meter.Process(ch, nch, n);
  Fade(v, ch, nch, n);
  SendPorts(v, ch, nch, n);

  std::memset(v.back, 0, 0x30);
  uint8_t* s = v.back;
  const uint32_t flags = v.rflags;
  const float env = v.env_set ? v.env.level : 1.0f, peak = v.meter.Height(nch);
  std::memcpy(s, &flags, 4);
  std::memcpy(s + 4, &env, 4);
  std::memcpy(s + 8, &peak, 4);
  std::memcpy(s + 0x10, &v.decoded_samples, 8);
  std::memcpy(s + 0x18, &v.decoded_bytes, 8);
  std::memcpy(s + 0x20, &v.block_user, 8);
  std::memcpy(s + 0x28, &v.read_addr, 8);
}

void RenderSubmixer(Voice& v, int n) {
  const int nch = v.in_ch;
  float* ch[kMaxChannels];
  float* side[kMaxChannels];
  for (int c = 0; c < kMaxChannels; c++) ch[c] = t_work[c], side[c] = t_side[c];
  Gather(v, 0, t_work, nch, n);
  if (v.rack->max_inputs >= 2) Gather(v, 1, t_side, nch, n);
  v.rflags = (v.rflags & ~kPlaying) | ((v.rflags & kPaused) ? 0 : kPlaying);
  RunFilters(v, 0, ch, nch, n);
  if (v.env_set) {
    if ((v.rpending & kEvStop) && v.env.phase != 2) {
      v.env.Release();
      if (v.fade == 4) v.fade = 0;
    }
    if (!(v.rflags & kPaused) && !v.env.Process(ch, nch, n)) v.rflags = 0;
  }
  if (v.dyn_set) v.dyn.Process(ch, nch, n, side, SdkVersion() >= 0x01700000);
  v.dist.Process(ch, nch, n);
  RunFilters(v, 1, ch, nch, n);
  if (!(v.rflags & kPaused)) v.meter.Process(ch, nch, n);
  Fade(v, ch, nch, n);
  SendPorts(v, ch, nch, n);
  const uint32_t flags = v.rflags;
  const float env = v.env_set ? v.env.level : 1.0f;
  const float peak = (v.rflags & kPaused) ? 0 : v.meter.Height(nch);
  const float comp = v.dyn_set ? v.dyn.Height(nch) : 1.0f;
  std::memcpy(v.back, &flags, 4);
  std::memcpy(v.back + 4, &env, 4);
  std::memcpy(v.back + 8, &peak, 4);
  std::memcpy(v.back + 0xc, &comp, 4);
}

void RenderReverb(Voice& v, int n) {
  float* in[kMaxChannels];
  float* out[kMaxChannels];
  for (int c = 0; c < kMaxChannels; c++) in[c] = t_side[c], out[c] = t_work[c];
  Gather(v, 0, t_side, v.in_ch, n);
  v.rflags = (v.rflags & ~kPlaying) | ((v.rflags & kPaused) ? 0 : kPlaying);
  v.reverb->Process(in, v.in_ch, out, v.out_ch, n);
  Fade(v, out, v.out_ch, n);
  SendPorts(v, out, v.out_ch, n);
  const uint32_t flags = v.rflags;
  std::memcpy(v.back, &flags, 4);
}

void WriteOutput(Voice& v, RenderBuffer* bufs, uint32_t count, float* const* ch, int nch, int n) {
  if (v.output >= count) return;
  RenderBuffer& b = bufs[v.output];
  const int bc = int(b.channels);
  const bool first = !b.written;
  b.written = true;
  if (b.type == 0x18 || b.type == 0x19) {
    float* out = reinterpret_cast<float*>(recomp::GPtr(b.buffer));
    for (int i = 0; i < n; i++)
      for (int c = 0; c < bc; c++) {
        const float x = c < nch ? ch[c][i] : 0;
        out[i * bc + c] = first ? x : out[i * bc + c] + x;
      }
    return;
  }
  int16_t* out = reinterpret_cast<int16_t*>(recomp::GPtr(b.buffer));
  for (int i = 0; i < n; i++)
    for (int c = 0; c < bc; c++) {
      const float x = c < nch ? ch[c][i] : 0;
      const int s = x < -1.0f ? -32767 : int(std::min(x, 1.0f) * 32767.0f);
      out[i * bc + c] = int16_t(first ? s : std::clamp(out[i * bc + c] + s, -32768, 32767));
    }
}

void RenderMastering(Voice& v, int n, RenderBuffer* bufs, uint32_t count) {
  const Rack& r = *v.rack;
  const int nch = int(r.max_channels);
  float* ch[kMaxChannels];
  for (int c = 0; c < kMaxChannels; c++) ch[c] = t_work[c];
  Gather(v, 0, t_work, nch, n);
  v.rflags = (v.rflags & ~kPlaying) | ((v.rflags & kPaused) ? 0 : kPlaying);
  std::memset(v.back, 0, sizeof v.back);
  if (v.meter_set && !(v.rflags & kPaused)) {
    v.meter.Process(ch, nch, n);
    for (int c = 0; c < nch; c++) {
      const float l = v.meter.Level(c);
      std::memcpy(v.back + 0xc + 4 * c, &l, 4);
    }
  }
  const int inl = LayoutOf(v.in_ch);
  const int outl = v.output_set && v.output < count ? LayoutOf(int(bufs[v.output].channels)) : inl;
  const int type = inl * 4 + outl;
  if (v.mmatrix_set >> type & 1) {
    float (*m)[kMaxChannels] = v.mmatrix[type];
    float (*pm)[kMaxChannels] = v.mprev[type];
    if (v.reset) std::memcpy(pm, m, sizeof v.mprev[type]);
    for (int i = 0; i < n; i++) {
      float in[kMaxChannels];
      for (int s = 0; s < nch; s++) in[s] = ch[s][i];
      const float t = float(i) / float(n);
      for (int d = 0; d < nch; d++) {
        float y = 0;
        for (int s = 0; s < nch; s++) y += (pm[s][d] + (m[s][d] - pm[s][d]) * t) * in[s];
        ch[d][i] = y;
      }
    }
    std::memcpy(pm, m, sizeof v.mprev[type]);
  }
  if (v.lfe_set && v.in_ch > 4 && nch > kLfe) v.lfe.Process(ch[kLfe], n);
  if (v.dyn_set) v.dyn.Process(ch, nch, n, nullptr, SdkVersion() >= 0x01700000);
  if (v.gain_set) {
    for (int c = 0; c < nch; c++) {
      const float g0 = v.reset ? v.gain[c] : v.gain_prev[c], g1 = v.gain[c];
      for (int i = 0; i < n; i++) ch[c][i] *= g0 + (g1 - g0) * float(i) / float(n);
      v.gain_prev[c] = g1;
    }
  }
  if (v.meter_set && !(v.rflags & kPaused)) {
    v.meter_post.Process(ch, nch, n);
    for (int c = 0; c < nch; c++) {
      const float l = v.meter_post.Level(c);
      std::memcpy(v.back + 0x2c + 4 * c, &l, 4);
    }
  }
  Fade(v, ch, nch, n);
  if (v.output_set) WriteOutput(v, bufs, count, ch, nch, n);
  const uint32_t flags = v.rflags;
  std::memcpy(v.back, &flags, 4);
}

// Events taken by this grain, before the rack renders the voice.
void EventsBefore(Voice& v) {
  const uint8_t e = v.rpending;
  if (e & kEvPause) {
    if (!(v.rflags & kPaused)) {
      if (e & kEvPlay) v.rflags |= kPaused, v.fade = 3;
      else v.fade = 1;
    }
  }
  if ((e & kEvResume) && (v.rflags & kPaused)) v.rflags &= ~kPaused, v.fade = 2;
  if (e & kEvStop) v.rflags |= kStopped, v.fade = 4;
  if (e & (kEvStopImm | kEvKill)) v.fade = 5;
}

void EventsAfter(Voice& v) {
  const uint8_t e = v.rpending;
  if ((e & kEvPause) && v.fade == 1) v.rflags = (v.rflags & ~(kPlaying | kPaused)) | kPaused, v.fade = 3;
  if ((e & kEvResume) && v.fade == 2) v.fade = 0;
  if (v.fade == 4 || v.fade == 5) v.rflags = 0;
}

void RenderVoice(Voice& v, int n, RenderBuffer* bufs, uint32_t count) {
  if (!v.in_ch || !(v.rflags & kInUse)) return;
  EventsBefore(v);
  switch (v.rack->id) {
    case kRackSampler: RenderSampler(v, n); break;
    case kRackSubmixer: RenderSubmixer(v, n); break;
    case kRackReverb: RenderReverb(v, n); break;
    default: RenderMastering(v, n, bufs, count); break;
  }
  EventsAfter(v);
  v.reset = false;
}

// Pending controls take effect at the start of a render.
void Commit(System& s, Rack& r) {
  for (auto& vp : r.voices) {
    Voice& v = *vp;
    const bool play = v.pending & kEvPlay;
    if (v.setup_changed && !play && (v.flags & kInUse || SdkVersion() < 0x01750000))
      v.pending |= kEvKill;
    if (play) {
      v.rflags = kInUse;
      v.fade = 0;
      AllocInputs(v);
      ResetModules(v);
      if (r.kind == 0) {
        if (std::find(r.active.begin(), r.active.end(), &v) == r.active.end()) r.active.push_back(&v);
      } else if (std::find(s.ordered.begin(), s.ordered.end(), &v) == s.ordered.end()) {
        s.ordered.push_back(&v);
        s.dirty = true;
      }
    }
    v.rpending = v.pending;
    v.pending = 0;
    v.setup_changed = false;
  }
}

// Orders the submixer, reverb and mastering voices so each renders after
// everything that feeds it: depth first from each mastering voice.
void Sort(System& s) {
  std::vector<Voice*> post, rest;
  std::vector<Voice*> visiting;
  auto feeds = [](const Voice& u, const Voice& v) {
    for (uint32_t i = 0; i < u.rack->max_ports; i++)
      if (u.ports[i].patched && u.ports[i].dst == &v) return true;
    return false;
  };
  auto seen = [&](Voice* v) {
    return std::find(post.begin(), post.end(), v) != post.end() ||
           std::find(visiting.begin(), visiting.end(), v) != visiting.end();
  };
  std::function<void(Voice*)> visit = [&](Voice* v) {
    visiting.push_back(v);
    for (Voice* u : s.ordered)
      if (u != v && !seen(u) && feeds(*u, *v)) visit(u);
    visiting.pop_back();
    post.push_back(v);
  };
  for (Rack* r : s.lists[2])
    for (auto& v : r->voices)
      if ((v->rflags & kInUse) && !seen(v.get()) &&
          std::find(s.ordered.begin(), s.ordered.end(), v.get()) != s.ordered.end())
        visit(v.get());
  for (Voice* v : s.ordered)
    if (std::find(post.begin(), post.end(), v) == post.end()) rest.push_back(v);
  rest.insert(rest.end(), post.begin(), post.end());
  s.ordered = std::move(rest);
  s.dirty = false;
}

void PostPass(System& s, Rack& r) {
  for (auto& vp : r.voices) {
    Voice& v = *vp;
    if (!(v.flags & kInUse) && !v.rflags) continue;
    if (!v.rflags) FreeVoice(s, v);
    v.flags = v.rflags;
    v.just_played = false;
    v.rpending = 0;
    std::memcpy(v.state, v.back, sizeof v.state);
  }
  // The input block this grain read is free for later delayed sends.
  for (auto& vp : r.voices)
    for (Input& in : vp->inputs) in.written[r.ring_cur] = false;
  r.ring_cur = (r.ring_cur + 1) % (r.max_delay_blocks + 1);
}

uint32_t Render(System& s, gaddr infos, uint32_t count) {
  RenderBuffer bufs[16];
  auto fail = [&](uint32_t e) {
    for (uint32_t i = 0; i < count; i++) {
      const gaddr b = Rd64(infos + i * 0x18);
      if (b) std::memset(recomp::GPtr(b), 0, Rd64(infos + i * 0x18 + 8));
    }
    return e;
  };
  if (!infos || count - 1 > 15) return infos ? fail(kErrInvalidBufferAddress) : kErrInvalidBufferAddress;
  for (uint32_t i = 0; i < count; i++) {
    const gaddr e = infos + i * 0x18;
    RenderBuffer& b = bufs[i];
    b = {Rd64(e), Rd64(e + 8), Rd32(e + 0x10), Rd32(e + 0x14), false};
    if (!(b.channels == 1 || b.channels == 2 || b.channels == 6 || b.channels == 8))
      return fail(kErrInvalidChannels);
    if (!(b.type == 0x12 || b.type == 0x13 || b.type == 0x18 || b.type == 0x19))
      return fail(kErrInvalidWaveformType);
    if (!b.buffer) return fail(kErrInvalidBufferAddress);
    const uint64_t need = uint64_t(b.channels) * (b.type < 0x18 ? 2 : 4) * s.grain;
    if (b.size < need) return fail(kErrInvalidBufferSize);
    b.size = need;
  }
  const int n = int(s.grain);
  for (auto& l : s.lists)
    for (Rack* r : l) Commit(s, *r);
  if (s.dirty) Sort(s);
  for (Rack* r : s.lists[0]) {
    const std::vector<Voice*> active = r->active;
    for (Voice* v : active) RenderVoice(*v, n, bufs, count);
  }
  for (Voice* v : std::vector<Voice*>(s.ordered)) RenderVoice(*v, n, bufs, count);
  for (uint32_t i = 0; i < count; i++) {
    RenderBuffer& b = bufs[i];
    uint8_t* p = recomp::GPtr(b.buffer);
    if (!b.written) {
      std::memset(p, 0, b.size);
    } else if (b.type == 0x13) {
      for (uint64_t k = 0; k < b.size; k += 2) std::swap(p[k], p[k + 1]);
    } else if (b.type == 0x19) {
      for (uint64_t k = 0; k < b.size; k += 4) std::swap(p[k], p[k + 3]), std::swap(p[k + 1], p[k + 2]);
    }
  }
  for (auto& l : s.lists)
    for (Rack* r : l) PostPass(s, *r);
  return 0;
}

}  // namespace

using recomp::hle::Arg;

// int sceNgs2SystemCreateWithAllocator(const SceNgs2SystemOption* option,
// const SceNgs2BufferAllocator* allocator {alloc, free, userData},
// SceNgs2Handle* out)
ORBIS_HLE("libSceNgs2", sceNgs2SystemCreateWithAllocator, 3) {
  const gaddr opt = Arg(c, 0), alloc = Arg(c, 1), out = Arg(c, 2);
  if (!alloc || !Rd64(alloc)) return kErrInvalidAllocator;
  if (!out) return kErrInvalidOutAddress;
  SystemConfig cfg;
  if (uint32_t e = ParseSystemOption(opt, cfg)) return e;
  uint8_t info[0x40] = {};
  const uint64_t need = SystemBufferSize(cfg);
  std::memcpy(info + 8, &need, 8);
  const uint64_t user = Rd64(alloc + 0x10);
  std::memcpy(info + 0x38, &user, 8);
  const int32_t r = CallBufferHandler(c, Rd64(alloc), info);
  if (r < 0) return uint32_t(r);
  const gaddr host = InfoU64(info, 0), free_fn = Rd64(alloc + 8);
  uint32_t e = !host ? kErrInvalidBufferAddress : InfoU64(info, 8) < need ? kErrInvalidBufferSize : 0;
  if (e) {
    if (free_fn) CallBufferHandler(c, free_fn, info);
    return e;
  }
  std::lock_guard<std::mutex> l(g_mu);
  auto s = std::make_unique<System>();
  s->handle = AlignUp(host + 0x10, 8) + 0x58;
  std::memcpy(s->info, info, 0x40);
  s->free_fn = free_fn;
  s->max_grain = cfg.max_grain, s->grain = cfg.grain, s->rate = cfg.rate;
  s->win_max = cfg.max_grain;
  Wr64(out, s->handle);
  g_systems[s->handle] = std::move(s);
  return 0;
}

// int sceNgs2SystemDestroy(SceNgs2Handle system, SceNgs2ContextBufferInfo*
// out): racks still alive are detached, not destroyed.
ORBIS_HLE("libSceNgs2", sceNgs2SystemDestroy, 2) {
  const gaddr out = Arg(c, 1);
  std::unique_ptr<System> s;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_systems.find(Arg(c, 0));
    if (it == g_systems.end()) return kErrInvalidSystemHandle;
    s = std::move(it->second);
    g_systems.erase(it);
    for (auto& list : s->lists)
      for (Rack* r : std::vector<Rack*>(list)) DetachRack(*r);
  }
  if (s->free_fn) CallBufferHandler(c, s->free_fn, s->info);
  if (out) std::memcpy(recomp::GPtr(out), s->info, 0x40);
  return 0;
}

// int sceNgs2RackQueryBufferSize(uint32_t rackId, const SceNgs2RackOption*
// option, SceNgs2ContextBufferInfo* out)
ORBIS_HLE("libSceNgs2", sceNgs2RackQueryBufferSize, 3) {
  const uint32_t id = uint32_t(Arg(c, 0));
  if (RackKind(id) < 0) return kErrInvalidRackId;
  const gaddr out = Arg(c, 2);
  if (!out) return kErrInvalidOutAddress;
  RackConfig cfg;
  if (uint32_t e = ParseRackOption(id, Arg(c, 1), cfg)) return e;
  Wr64(out, 0);
  Wr64(out + 8, RackBufferSize(cfg));
  std::memset(recomp::GPtr(out + 0x10), 0, 0x28);
  return 0;
}

namespace {

// Builds the rack in the buffer `info` describes and attaches it to the
// system.
uint32_t CreateRack(gaddr sys, uint32_t id, const RackConfig& cfg, const uint8_t* info,
                    gaddr free_fn, gaddr* handle) {
  const gaddr host = InfoU64(info, 0);
  if (!host) return kErrInvalidBufferAddress;
  if (InfoU64(info, 8) < RackBufferSize(cfg)) return kErrInvalidBufferSize;
  std::lock_guard<std::mutex> l(g_mu);
  System* s = FindSystem(sys);
  if (!s) return kErrInvalidSystemHandle;
  if (cfg.max_grain < s->win_min || s->grain > cfg.max_grain) return kErrInvalidNumGrain;
  auto r = std::make_unique<Rack>();
  r->sys = s, r->id = id, r->kind = cfg.kind;
  const gaddr obj = AlignUp(host + 0x10, 8);
  r->handle = obj + 0x70;
  std::memcpy(r->info, info, 0x40);
  r->free_fn = free_fn;
  r->flags = cfg.flags, r->max_grain = cfg.max_grain, r->max_voices = cfg.max_voices;
  r->max_delay_blocks = cfg.max_delay_blocks, r->max_matrices = cfg.max_matrices;
  r->max_ports = cfg.max_ports, r->max_channels = cfg.max_channels;
  r->max_env_points = cfg.max_env_points, r->max_filters = cfg.max_filters;
  r->max_inputs = cfg.max_inputs, r->meter_blocks = cfg.meter_blocks;
  r->max_wave_blocks = cfg.max_wave_blocks, r->reverb_size = cfg.reverb_size;
  for (uint32_t i = 0; i < cfg.max_voices; i++) {
    auto v = std::make_unique<Voice>();
    v->rack = r.get(), v->index = i, v->handle = obj + kRackObject + i * kVoiceObject;
    if (id == kRackReverb) {
      v->reverb = std::make_unique<Reverb>();
      v->reverb->Init(float(s->rate), cfg.reverb_size);
    }
    ResetModules(*v);
    g_voices[v->handle] = v.get();
    r->voices.push_back(std::move(v));
  }
  s->win_max = std::min(s->win_max, cfg.max_grain);
  s->lists[cfg.kind].push_back(r.get());
  *handle = r->handle;
  g_racks[r->handle] = std::move(r);
  return 0;
}

void DestroyRack(Rack* r) {
  DetachRack(*r);
  for (auto& v : r->voices) g_voices.erase(v->handle);
  g_racks.erase(r->handle);
}

}  // namespace

// int sceNgs2RackCreate(SceNgs2Handle system, uint32_t rackId, const
// SceNgs2RackOption* option, const SceNgs2ContextBufferInfo* buffer,
// SceNgs2Handle* out)
ORBIS_HLE("libSceNgs2", sceNgs2RackCreate, 5) {
  const uint32_t id = uint32_t(Arg(c, 1));
  if (RackKind(id) < 0) return kErrInvalidRackId;
  const gaddr buf = Arg(c, 3), out = Arg(c, 4);
  if (!buf) return kErrInvalidBufferInfo;
  if (!out) return kErrInvalidOutAddress;
  RackConfig cfg;
  if (uint32_t e = ParseRackOption(id, Arg(c, 2), cfg)) return e;
  uint8_t info[0x40];
  std::memcpy(info, recomp::GPtr(buf), 0x40);
  gaddr h = 0;
  if (uint32_t e = CreateRack(Arg(c, 0), id, cfg, info, 0, &h)) return e;
  Wr64(out, h);
  return 0;
}

// int sceNgs2RackCreateWithAllocator(SceNgs2Handle system, uint32_t rackId,
// const SceNgs2RackOption* option, const SceNgs2BufferAllocator* allocator,
// SceNgs2Handle* out)
ORBIS_HLE("libSceNgs2", sceNgs2RackCreateWithAllocator, 5) {
  const gaddr sys = Arg(c, 0);
  const uint32_t id = uint32_t(Arg(c, 1));
  if (RackKind(id) < 0) return kErrInvalidRackId;
  const gaddr alloc = Arg(c, 3), out = Arg(c, 4);
  if (!alloc || !Rd64(alloc)) return kErrInvalidAllocator;
  if (!out) return kErrInvalidOutAddress;
  RackConfig cfg;
  if (uint32_t e = ParseRackOption(id, Arg(c, 2), cfg)) return e;
  uint8_t info[0x40] = {};
  const uint64_t need = RackBufferSize(cfg), user = Rd64(alloc + 0x10);
  std::memcpy(info + 8, &need, 8);
  std::memcpy(info + 0x38, &user, 8);
  const int32_t r = CallBufferHandler(c, Rd64(alloc), info);
  if (r < 0) return uint32_t(r);
  const gaddr free_fn = Rd64(alloc + 8);
  gaddr h = 0;
  if (uint32_t e = CreateRack(sys, id, cfg, info, free_fn, &h)) {
    if (free_fn) CallBufferHandler(c, free_fn, info);
    return e;
  }
  Wr64(out, h);
  return 0;
}

// int sceNgs2RackDestroy(SceNgs2Handle rack, SceNgs2ContextBufferInfo* out)
ORBIS_HLE("libSceNgs2", sceNgs2RackDestroy, 2) {
  const gaddr out = Arg(c, 1);
  uint8_t info[0x40];
  gaddr free_fn;
  {
    std::lock_guard<std::mutex> l(g_mu);
    Rack* r = FindRack(Arg(c, 0));
    if (!r) return kErrInvalidRackHandle;
    std::memcpy(info, r->info, 0x40);
    free_fn = r->free_fn;
    DestroyRack(r);
  }
  if (free_fn) CallBufferHandler(c, free_fn, info);
  if (out) std::memcpy(recomp::GPtr(out), info, 0x40);
  return 0;
}

// int sceNgs2RackGetVoiceHandle(SceNgs2Handle rack, uint32_t index,
// SceNgs2Handle* out)
ORBIS_HLE("libSceNgs2", sceNgs2RackGetVoiceHandle, 3) {
  const gaddr out = Arg(c, 2);
  if (!out) return kErrInvalidOutAddress;
  std::lock_guard<std::mutex> l(g_mu);
  Rack* r = FindRack(Arg(c, 0));
  if (!r) {
    Wr64(out, 0);
    return kErrInvalidRackHandle;
  }
  const uint32_t i = uint32_t(Arg(c, 1));
  if (i >= r->max_voices) {
    Wr64(out, 0);
    return kErrInvalidVoiceIndex;
  }
  Wr64(out, r->voices[i]->handle);
  return 0;
}

// int sceNgs2VoiceControl(SceNgs2Handle voice, const SceNgs2VoiceParamHead*
// params): a chain of {u16 size; s16 next (byte offset, 0 ends); u32 id}.
ORBIS_HLE("libSceNgs2", sceNgs2VoiceControl, 2) {
  gaddr p = Arg(c, 1);
  if (!p) return 0;
  std::lock_guard<std::mutex> l(g_mu);
  Voice* v = FindVoice(Arg(c, 0));
  if (!v) return kErrInvalidVoiceHandle;
  auto next = [](gaddr q) -> gaddr {
    const int16_t d = int16_t(recomp::Rd16(q + 2));
    return d ? q + int64_t(d) : 0;
  };
  gaddr fast = p;
  for (;;) {
    if (uint32_t e = ApplyParam(*v, Param{p, recomp::Rd16(p), Rd32(p + 4)})) return e;
    p = next(p);
    if (fast) fast = next(fast);
    if (fast) fast = next(fast);
    if (fast && p == fast) return kErrCircularControl;
    if (!p) return 0;
  }
}

// int sceNgs2VoiceGetState(SceNgs2Handle voice, void* state, size_t size):
// the state of the last render; exactly 4 bytes give the flags alone,
// fewer than the rack's state size nothing.
ORBIS_HLE("libSceNgs2", sceNgs2VoiceGetState, 3) {
  const gaddr out = Arg(c, 1);
  const uint64_t size = Arg(c, 2);
  if (!out) return kErrInvalidOutAddress;
  if (size < 4) return kErrInvalidStateSize;
  std::lock_guard<std::mutex> l(g_mu);
  Voice* v = FindVoice(Arg(c, 0));
  if (!v) return kErrInvalidVoiceHandle;
  uint8_t s[0x4c] = {};
  const uint32_t size_of_state = v->rack->id == kRackSampler     ? 0x30
                                 : v->rack->id == kRackSubmixer  ? 0x10
                                 : v->rack->id == kRackReverb    ? 4
                                                                 : 0x4c;
  if (v->just_played) s[0] = kInUse;
  else if (v->flags) std::memcpy(s, v->state, size_of_state), s[0] = v->flags;
  if (size == 4) {
    Wr32(out, s[0]);
    return 0;
  }
  if (size >= size_of_state) std::memcpy(recomp::GPtr(out), s, size_of_state);
  return 0;
}

// int sceNgs2SystemRender(SceNgs2Handle system, const
// SceNgs2RenderBufferInfo* buffers {u64 buffer, size; u32 waveformType,
// numChannels}, uint32_t count)
ORBIS_HLE("libSceNgs2", sceNgs2SystemRender, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  System* s = FindSystem(Arg(c, 0));
  if (!s) return kErrInvalidSystemHandle;
  return Render(*s, Arg(c, 1), uint32_t(Arg(c, 2)));
}
