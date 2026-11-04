// libSceAjm HLE: the library, the kernel's /dev/ajm driver behind it and
// the audio coprocessor (ACP) that runs the codecs. The library's checks,
// the batch words its job builders write, the driver's validation, ids,
// pools and codes are 11.00's (library and kernel). What a job does and
// the sideband records it writes are decided by the ACP's firmware, not
// measured: those are this HLE's own, on its ffmpeg decoders
// (audio_decoder.h); docs/hle.md lists them. A batch's inputs are read
// when it starts and its jobs run when it is waited for, which is when
// 11.00 copies outputs back to memory not registered with
// sceAjmMemoryRegister.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/audio_decoder.h"
#include "orbis/process.h"
#include "runtime/guest_alloc.h"
#include "runtime/log.h"
#include "x86/runtime.h"

namespace {

using recomp::gaddr;
using recomp::Rd32;
using recomp::Rd64;
using recomp::Wr32;
using recomp::Wr64;
using recomp::orbis::AudioDecoder;
using recomp::orbis::SdkVersion;

constexpr uint32_t kErrFailed = 0x80930001, kErrInvalidContext = 0x80930002,
                   kErrInvalidInstance = 0x80930003, kErrInvalidBatch = 0x80930004,
                   kErrInvalidParameter = 0x80930005, kErrOutOfMemory = 0x80930006,
                   kErrOutOfResources = 0x80930007, kErrCodecNotSupported = 0x80930008,
                   kErrCodecAlreadyRegistered = 0x80930009, kErrCodecNotRegistered = 0x8093000A,
                   kErrWrongRevision = 0x8093000B, kErrFlagNotSupported = 0x8093000C,
                   kErrBusy = 0x8093000D, kErrBadPriority = 0x8093000E,
                   kErrMalformedBatch = 0x80930011, kErrInvalidOpcode = 0x80930013,
                   kErrPriorityViolation = 0x80930014, kErrBufferTooBig = 0x80930015,
                   kErrInvalidAddress = 0x80930016, kErrCancelled = 0x80930017;

constexpr uint32_t kCodecs = 23;  // 0-21 and 22 ("Custom", never loads)
constexpr uint32_t kCodecMp3 = 0, kCodecAt9 = 1;
// The pseudo-instance every context may name (index 0).
constexpr uint32_t kSpecialInstance = 0x80000;
constexpr uint32_t kInstanceSlots = 0x3000;

// Batch words (the low dword's opcode).
enum : uint32_t { kJob = 0, kData = 1, kSideband = 2, kControl = 3, kRun = 4, kFlush = 5,
                  kDebug = 6, kInline = 7 };

// CONTROL and RUN flags.
constexpr uint64_t kRevision = 7, kSidebandCodec = 0x7F8, kGetCodecInfo = 1u << 11,
                   kMultipleFrames = 1u << 12, kReset = 1u << 13, kInitialize = 1u << 14,
                   kResample = 1u << 15, kGapless = 1ull << 45, kFormat = 1ull << 46,
                   kStream = 1ull << 47;

struct Context {
  bool registered[kCodecs] = {};
  uint32_t instances[kCodecs] = {};
};

// An instance: the driver's slot and the codec's state.
struct Instance {
  bool live = false;
  uint32_t ctx = 0, id = 0, codec = 0;
  uint64_t flags = 0;
  // Batches started and not waited for; the priority they run at and
  // whether others may run at another one (flag bit 11 when the first
  // was started).
  uint32_t pending = 0;
  int priority = 0;
  bool violation_allowed = false;

  // The stream: the decoder and, for ATRAC9, its config and the frames of
  // the superframe decoded last not output yet.
  std::unique_ptr<AudioDecoder> dec;
  uint32_t at9_config = 0;
  uint32_t next_frame = 0, frames = 0;
  uint64_t total_samples = 0;
  uint32_t mp3_rate = 0, mp3_bitrate = 0;  // the last MP3 frame's header
  // Gapless decoding: samples to output (0: all), to skip at the start,
  // skipped so far.
  uint32_t gapless_total = 0;
  uint16_t gapless_skip = 0, gapless_skipped = 0;
};

struct Buffer {
  gaddr addr = 0;
  uint32_t size = 0;
};

// A job as the ACP runs it: its flags, its inputs (read at the start) and
// where its outputs go.
struct Job {
  uint32_t instance = 0;
  bool control = false;
  uint64_t flags = 0;
  std::vector<uint8_t> in, sb_in;
  std::vector<Buffer> out;
  Buffer sb_out;
};

struct Batch {
  bool live = false, cancelled = false;
  uint32_t ctx = 0;
  std::vector<Job> jobs;
  std::vector<uint32_t> instances;  // slots holding a pending reference
};

// Registered memory: one slot per 2 MiB page.
struct Region {
  uint32_t ctx;
  gaddr base, page;
};

std::mutex g_mu;
std::map<uint32_t, Context> g_contexts;
uint32_t g_next_context = 1;
std::array<Instance, kInstanceSlots> g_instances;
std::deque<uint32_t> g_free_instances;  // FIFO: 1..12286 at boot
std::array<Batch, 256> g_batches;
std::deque<uint32_t> g_free_batches;  // the games' pool, FIFO: 0..190
std::vector<Region> g_regions;        // at most 32 pages, 16 registered at once

void InitPools() {
  if (!g_free_instances.empty() || g_instances[0].live) return;
  for (uint32_t i = 1; i <= 12286; i++) g_free_instances.push_back(i);
  for (uint32_t i = 0; i <= 190; i++) g_free_batches.push_back(i);
  Instance& s = g_instances[0];
  s.live = true, s.id = kSpecialInstance;
}

Context* FindContext(uint32_t ctx) {
  auto it = g_contexts.find(ctx);
  return it == g_contexts.end() ? nullptr : &it->second;
}

// A live instance of ctx with exactly this id, or null.
Instance* FindInstance(uint32_t ctx, uint32_t id) {
  const uint32_t index = id & 0x3FFF;
  if (index >= kInstanceSlots || index == 0) return nullptr;
  Instance& in = g_instances[index];
  return in.live && in.id == id && in.ctx == ctx ? &in : nullptr;
}

void Release(Instance& in) {
  if (--in.pending == 0) in.priority = 0;
}

// ---- ATRAC9 config and MP3 headers (library code in 11.00)

struct At9Config {
  uint32_t channels, rate, frame_samples, frames, frame_bytes;
};

// The 4 config bytes (sync byte first), or false.
bool ParseAt9(const uint8_t b[4], At9Config& c) {
  if (b[0] != 0xFE || b[1] & 0x80 || ((b[1] >> 1) & 7) > 5 || b[1] & 1) return false;
  const uint32_t sf = (b[3] >> 3) & 3;
  if (sf == 1 || sf == 3 || (b[3] & 7)) return false;
  static const uint32_t kRates[] = {11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
  static const uint32_t kSamples[] = {64, 64, 128, 128, 128, 256, 256, 256};
  static const uint32_t kChannels[] = {1, 2, 2, 6, 8, 4};
  c.rate = kRates[b[1] >> 4];
  c.frame_samples = kSamples[b[1] >> 4];
  c.channels = kChannels[(b[1] >> 1) & 7];
  c.frames = sf == 0 ? 1 : 4;
  c.frame_bytes = (uint32_t(b[2]) << 3 | b[3] >> 5) + 1;
  return true;
}

struct Mp3Header {
  uint32_t frame_size, channels, samples, bitrate, rate;
};

bool ParseMp3(const uint8_t* b, size_t size, Mp3Header& h) {
  if (size < 4 || (uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16) < 0xFFE00000) return false;
  const uint32_t version = (b[1] >> 3) & 3, rate_index = (b[2] >> 2) & 3;
  if (((b[1] >> 1) & 3) != 1 || version == 1 || b[2] >> 4 == 15 || rate_index == 3) return false;
  static const uint32_t kRates[3][3] = {
      {44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
  static const uint32_t kKbps[2][15] = {
      {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
  const bool mpeg1 = version == 3;
  h.rate = kRates[mpeg1 ? 0 : version == 2 ? 1 : 2][rate_index];
  h.bitrate = kKbps[mpeg1 ? 0 : 1][b[2] >> 4] * 1000;
  const int32_t coef = mpeg1 ? 144 : 72;
  h.frame_size = ((b[2] >> 1) & 1) + uint32_t(int32_t(h.bitrate) * coef / int32_t(h.rate));
  h.channels = b[3] >> 6 == 3 ? 1 : 2;
  h.samples = uint32_t(8 * coef);
  return true;
}

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t Be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

// CRC-8, polynomial 0x45, initial value 0xFF, most significant bit first.
uint8_t Crc8(const uint8_t* p, size_t n) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = uint8_t(crc & 0x80 ? crc << 1 ^ 0x45 : crc << 1);
  }
  return crc;
}

// sceAjmDecMp3ParseFrame's 40-byte result.
struct Mp3Frame {
  uint64_t frame_size = 0;
  uint32_t channels = 0, samples = 0, bitrate = 0, rate = 0;
  uint32_t delay = 0, num_frames = 0, total_samples = 0, tag = 0;
};

// The frame's header and, with parse_tag, the encoder's tag in it (Xing or
// Info with LAME's, VBRI, or the 0xB4 tag), as 11.00's library reads them.
bool ParseMp3Frame(const uint8_t* b, size_t size, bool parse_tag, Mp3Frame& f) {
  Mp3Header h;
  if (!ParseMp3(b, size, h)) return false;
  f = Mp3Frame{};
  f.frame_size = h.frame_size, f.channels = h.channels, f.samples = h.samples;
  f.bitrate = h.bitrate, f.rate = h.rate;
  if (!parse_tag) return true;
  const bool mpeg1 = ((b[1] >> 3) & 3) == 3, mono = b[3] >> 6 == 3;
  const size_t side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
  if (4 + side > size) return true;
  // The granules' part2_3_length (12 bits each), from the first one's bit
  // position on, a block per granule and channel.
  const uint32_t channels = mono ? 1 : 2, granules = mpeg1 ? 2 : 1;
  uint32_t bit = mpeg1 ? 32 + 9 + (mono ? 5 : 3) + 4 * channels : 32 + 8 + (mono ? 1 : 2);
  const uint32_t block = mpeg1 ? 59 : 63;
  uint32_t sum = 0;
  for (uint32_t g = 0; g < granules * channels; g++, bit += block) {
    uint32_t v = 0;
    for (uint32_t k = 0; k < 12; k++) v = v << 1 | (b[(bit + k) / 8] >> (7 - (bit + k) % 8) & 1);
    sum += v;
  }
  const size_t pos = (32 + 8 * side + sum + 7) / 8;
  if (pos == 0) return true;
  const uint8_t* end = b + size;
  const uint8_t* p = b + pos;
  if (p + 8 < end && (!std::memcmp(p, "Xing", 4) || !std::memcmp(p, "Info", 4))) {
    const uint8_t flags = p[7];
    const uint8_t* q = p + 8;
    bool ok = true;
    if (flags & 1) {
      if (p + 12 < end) {
        f.num_frames = Be32(p + 8);
        q = p + 12;
      } else {
        ok = false;
      }
    }
    for (auto [bit_mask, n] : {std::pair<int, int>{2, 4}, {4, 100}, {8, 4}})
      if (ok && flags & bit_mask) {
        q += n;
        ok = q < end;
      }
    if (ok && q + 4 < end && q[0] == 'L' && q[1] == 'A' && q[2] == 'M' && q + 24 < end &&
        q[3] == 'E') {
      const uint32_t delay = uint32_t(q[21]) << 4 | q[22] >> 4;
      const uint32_t padding = uint32_t(q[22] & 15) << 8 | q[23];
      f.delay = delay;
      if (flags & 1) f.total_samples = f.samples * f.num_frames - (delay + padding);
      f.delay = f.samples + delay + 529;
      f.tag = 1;
      return true;
    }
  }
  if (f.frame_size <= size && f.frame_size >= pos + 26 && !std::memcmp(p, "VBRI", 4)) {
    f.delay = Be16(p + 6);
    f.tag = 2;
    Mp3Frame next;
    if (ParseMp3Frame(b + f.frame_size, size - f.frame_size, true, next) && next.tag == 3) {
      f.delay += next.delay;
      f.total_samples = next.total_samples;
      f.tag = 4;
    }
    return true;
  }
  const uint8_t* limit = b + std::min<uint64_t>(f.frame_size, size);
  for (const uint8_t* q = p; q + 10 <= limit; q++)
    if (q[0] == 0xB4 && Crc8(q, 9) == q[9]) {
      f.delay = Be16(q + 1);
      f.total_samples = Be32(q + 3);
      f.tag = 3;
      break;
    }
  return true;
}

// ---- the ACP: running a job (this HLE's own; docs/hle.md)

// Writes the decoded samples to a job's output buffers, one after the
// other, in the instance's format (flags bits 7-9: S16, S32, float).
struct Output {
  const std::vector<Buffer>& bufs;
  uint32_t format;
  size_t index = 0, offset = 0;
  uint64_t written = 0;

  uint64_t Room() const {
    uint64_t n = 0;
    for (size_t i = index; i < bufs.size(); i++) n += bufs[i].size - (i == index ? offset : 0);
    return n;
  }
  uint32_t SampleBytes() const { return format == 0 ? 2 : 4; }
  void Put(const void* src, size_t n) {
    const uint8_t* s = static_cast<const uint8_t*>(src);
    while (n) {
      while (bufs[index].size == offset) index++, offset = 0;
      const size_t k = std::min<size_t>(n, bufs[index].size - offset);
      std::memcpy(recomp::GPtr(bufs[index].addr + offset), s, k);
      s += k, n -= k, offset += k, written += k;
    }
  }
  void Sample(float x) {
    if (format == 2) {
      Put(&x, 4);
    } else if (format == 1) {
      const double v = std::clamp(double(x) * 2147483648.0, -2147483648.0, 2147483647.0);
      const int32_t s = int32_t(std::lrint(v));
      Put(&s, 4);
    } else {
      const int16_t s = int16_t(std::lrint(std::clamp(x * 32768.0f, -32768.0f, 32767.0f)));
      Put(&s, 2);
    }
  }
};

// The samples of the next frame to output after the gapless skip and
// total, and how many of its first ones are skipped.
uint32_t GaplessCut(Instance& in, uint32_t frame_samples, uint32_t& skip) {
  skip = std::min<uint32_t>(frame_samples, in.gapless_skip - std::min(in.gapless_skip, in.gapless_skipped));
  uint32_t n = frame_samples - skip;
  if (in.gapless_total) {
    const uint64_t left = in.total_samples < in.gapless_total ? in.gapless_total - in.total_samples : 0;
    n = uint32_t(std::min<uint64_t>(n, left));
  }
  return n;
}

void Emit(Instance& in, Output& out, uint32_t first, uint32_t n) {
  const uint32_t channels = in.dec->channels();
  for (uint32_t i = first; i < first + n; i++)
    for (uint32_t c = 0; c < channels; c++) out.Sample(in.dec->channel(c)[i]);
}

[[noreturn]] void NotMeasured(const char* what, uint32_t id) {
  recomp::FatalF("libSceAjm: instance %X: %s is decided by the ACP's firmware, not measured; not "
                 "implemented",
                 id, what);
}

// Decodes frames from `in` into `out` (one, or as many as fit with
// MULTIPLE_FRAMES); returns the frames, sets the input bytes consumed.
uint32_t Decode(Instance& in, const Job& job, Output& out, uint32_t& consumed) {
  consumed = 0;
  uint32_t frames = 0;
  const bool many = job.flags & kMultipleFrames;
  const uint8_t* src = job.in.data();
  const size_t size = job.in.size();
  while (frames == 0 || many) {
    uint32_t skip = 0, n = 0, unit = 0, first = 0;
    if (in.codec == kCodecAt9) {
      if (!in.dec) NotMeasured("running ATRAC9 before its initialisation", in.id);
      At9Config c;
      const uint8_t b[4] = {uint8_t(in.at9_config >> 24), uint8_t(in.at9_config >> 16),
                            uint8_t(in.at9_config >> 8), uint8_t(in.at9_config)};
      ParseAt9(b, c);
      // A frame consumes its share of the superframe; the superframe
      // decodes at its first frame, which needs all of it.
      unit = c.frame_bytes;
      const bool start = in.next_frame == in.frames;
      if (size - consumed < (start ? unit * c.frames : unit)) break;
      n = GaplessCut(in, c.frame_samples, skip);
      if (out.Room() < uint64_t(n) * c.channels * out.SampleBytes()) break;
      if (start) {
        if (!in.dec->Decode(src + consumed, unit * c.frames) || in.dec->channels() != c.channels)
          recomp::FatalF("libSceAjm: instance %X: an ATRAC9 superframe did not decode (the "
                         "ACP's result is not measured)",
                         in.id);
        in.next_frame = 0, in.frames = c.frames;
      }
      first = in.next_frame++ * c.frame_samples;
    } else {
      Mp3Header h;
      if (size - consumed < 4) break;
      if (!ParseMp3(src + consumed, size - consumed, h) || h.bitrate == 0)
        NotMeasured("an MP3 frame header it does not take (or free format)", in.id);
      unit = h.frame_size;
      if (size - consumed < unit) break;
      n = GaplessCut(in, h.samples, skip);
      if (out.Room() < uint64_t(n) * h.channels * out.SampleBytes()) break;
      if (!in.dec->Decode(src + consumed, unit) || in.dec->samples() != h.samples ||
          in.dec->channels() != h.channels)
        recomp::FatalF("libSceAjm: instance %X: an MP3 frame did not decode (the ACP's result "
                       "is not measured)",
                       in.id);
      in.mp3_rate = h.rate, in.mp3_bitrate = h.bitrate;
    }
    Emit(in, out, first + skip, n);
    in.gapless_skipped = uint16_t(in.gapless_skipped + skip);
    in.total_samples += n;
    consumed += unit;
    frames++;
  }
  if (frames == 0 && (size || out.Room()))
    NotMeasured("a run that decodes no frame (input or output too short)", in.id);
  return frames;
}

void Reset(Instance& in) {
  if (in.dec) in.dec->Flush();
  in.next_frame = in.frames = 0;
  in.total_samples = 0;
  in.gapless_skipped = 0;
}

// The job's sideband records, in order, as one block.
struct Sideband {
  std::vector<uint8_t> bytes;
  void U32(uint32_t v) { bytes.insert(bytes.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}); }
  void U64(uint64_t v) { U32(uint32_t(v)), U32(uint32_t(v >> 32)); }
  void Gapless(const Instance& in) {
    U32(in.gapless_total);
    U32(uint32_t(in.gapless_skip) | uint32_t(in.gapless_skipped) << 16);
  }
};

// WAVE speaker masks for the channel counts the codecs give.
uint32_t ChannelMask(uint32_t channels) {
  switch (channels) {
    case 1: return 0x4;
    case 2: return 0x3;
    case 4: return 0x33;
    case 6: return 0x3F;
    case 8: return 0x63F;
  }
  recomp::FatalF("libSceAjm: %u channels: no speaker mask", channels);
}

void RunControl(Instance& in, const Job& job, Sideband& sb) {
  const uint64_t f = job.flags;
  if (f & (kSidebandCodec | kResample)) NotMeasured("a control job's codec or resample flags", in.id);
  if (f & kInitialize) {
    if (f & kGapless) NotMeasured("initialising with gapless decoding", in.id);
    if (in.codec == kCodecAt9) {
      // The init parameters: the 4 config bytes (then a reserved word).
      At9Config c;
      if (job.sb_in.size() < 4 || !ParseAt9(job.sb_in.data(), c))
        NotMeasured("an ATRAC9 initialisation without a valid config", in.id);
      in.at9_config = Be32(job.sb_in.data());
      in.dec = AudioDecoder::Atrac9(in.at9_config);
      if (!in.dec)
        recomp::FatalF("libSceAjm: ATRAC9 config %08X is not implemented yet (ffmpeg refuses it)",
                       in.at9_config);
    } else {
      if (!job.sb_in.empty()) NotMeasured("MP3 initialisation parameters", in.id);
      if (!in.dec) in.dec = AudioDecoder::Mp3();
    }
  }
  if (f & (kInitialize | kReset)) Reset(in);
  if (f & kGapless && !job.sb_in.empty()) {
    if (job.sb_in.size() < 8) NotMeasured("a short gapless decoding record", in.id);
    in.gapless_total = uint32_t(job.sb_in[0] | job.sb_in[1] << 8 | job.sb_in[2] << 16 |
                                uint32_t(job.sb_in[3]) << 24);
    in.gapless_skip = uint16_t(job.sb_in[4] | job.sb_in[5] << 8);
    if (!(f & kReset)) in.gapless_skipped = uint16_t(job.sb_in[6] | job.sb_in[7] << 8);
  }
  sb.U64(0);  // the result
  if (f & kGapless) sb.Gapless(in);
}

void RunRun(Instance& in, const Job& job, Sideband& sb) {
  const uint64_t f = job.flags;
  if (f & kSidebandCodec) NotMeasured("codec-specific sideband records", in.id);
  if (f & kGetCodecInfo) NotMeasured("the codec info record", in.id);
  if (f & kMultipleFrames && !(f & kStream)) NotMeasured("MULTIPLE_FRAMES without STREAM", in.id);
  Output out{job.out, uint32_t(in.flags >> 7 & 7)};
  uint32_t frames = 0, consumed = 0;
  if (!job.in.empty() || out.Room()) {
    if (!in.dec) NotMeasured("running before the codec's initialisation", in.id);
    frames = Decode(in, job, out, consumed);
  }
  sb.U64(0);  // the result
  if (f & kStream) {
    sb.U32(consumed);
    sb.U32(uint32_t(out.written));
    sb.U64(in.total_samples);
  }
  if (f & kMultipleFrames) sb.U64(frames);
  if (f & kFormat) {
    uint32_t channels, rate, bitrate;
    if (in.codec == kCodecAt9) {
      if (!in.dec) NotMeasured("the format before initialisation", in.id);
      At9Config c;
      const uint8_t b[4] = {uint8_t(in.at9_config >> 24), uint8_t(in.at9_config >> 16),
                            uint8_t(in.at9_config >> 8), uint8_t(in.at9_config)};
      ParseAt9(b, c);
      channels = c.channels, rate = c.rate;
      bitrate = uint32_t(uint64_t(c.frame_bytes) * 8 * c.rate / c.frame_samples);
    } else {
      if (!in.mp3_rate) NotMeasured("the MP3 format before a frame decoded", in.id);
      channels = in.dec->channels(), rate = in.mp3_rate, bitrate = in.mp3_bitrate;
    }
    sb.U32(channels);
    sb.U32(ChannelMask(channels));
    sb.U32(rate);
    sb.U32(uint32_t(in.flags >> 7 & 7));
    sb.U32(bitrate);
    sb.U32(0);
  }
  if (f & kGapless) sb.Gapless(in);
}

void RunJob(const Job& job) {
  if (job.instance == kSpecialInstance)
    recomp::FatalF("libSceAjm: jobs on instance 80000 (the driver's memory statistics) not "
                   "implemented");
  Instance& in = g_instances[job.instance & 0x3FFF];
  Sideband sb;
  if (job.control) RunControl(in, job, sb);
  else RunRun(in, job, sb);
  if (sb.bytes.size() > job.sb_out.size)
    NotMeasured("a sideband buffer too small for its records", in.id);
  if (!sb.bytes.empty()) std::memcpy(recomp::GPtr(job.sb_out.addr), sb.bytes.data(), sb.bytes.size());
}

// ---- the driver's batch start

bool Registered(uint32_t ctx, gaddr addr) {
  for (const Region& r : g_regions)
    if (r.ctx == ctx && addr >= r.page && addr - r.page < (1u << 21)) return true;
  return false;
}

// The 32-byte error block.
void PutError(gaddr err, uint32_t code, gaddr job, uint32_t offset, uint64_t ra) {
  if (!err) return;
  Wr32(err, code);
  Wr32(err + 4, 0);
  Wr64(err + 8, job);
  Wr32(err + 16, offset);
  Wr32(err + 20, 0);
  Wr64(err + 24, ra);
}

// The job's DEBUG return address, as the driver finds it for the error
// block: the first DEBUG word of the body (0 if none).
uint64_t FindRa(gaddr job, gaddr batch_end) {
  const gaddr end = std::min<gaddr>(batch_end, job + Rd32(job + 4) + 8);
  for (gaddr p = job + 8; p + 8 <= end;) {
    const uint32_t op = Rd32(p) & 0xF;
    if (op == kDebug) return p + 16 <= end ? Rd64(p + 8) : 0;
    if (op >= 7) break;
    p += op == kData || op == kSideband ? 16 : 8;
  }
  return 0;
}

uint64_t BatchStart(uint32_t ctx, gaddr batch, uint64_t size, int priority, gaddr err,
                    gaddr out_id) {
  if (priority < 10 || priority > 90) return kErrBadPriority;
  if (err && SdkVersion() >= 0x05000000)
    for (int i = 0; i < 32; i += 8) Wr64(err + i, 0);
  if (batch >> 47 || 0x800000000000ull - batch < size) return kErrInvalidAddress;
  const gaddr end = batch + size;

  // Pass 1: the jobs and their instances.
  struct Entry {
    uint32_t slot;
    gaddr header;
  };
  std::vector<Entry> jobs;
  std::vector<uint32_t> taken;
  auto fail = [&](uint32_t code, gaddr at, uint32_t offset, gaddr ra_job) -> uint64_t {
    for (uint32_t s : taken) Release(g_instances[s]);
    if (code == kErrOutOfMemory) return code;
    PutError(err, code, at, offset,
             code == kErrMalformedBatch || code == kErrInvalidOpcode || !ra_job ? 0 : FindRa(ra_job, end));
    return kErrMalformedBatch;
  };
  if (size & 7) return fail(kErrMalformedBatch, batch, 0, 0);
  gaddr p = batch;
  while (jobs.size() < 1000 && p + 8 <= end) {
    const uint32_t w0 = Rd32(p), len = Rd32(p + 4), op = w0 & 0xF;
    if (op != kJob && op != kInline) return fail(kErrInvalidOpcode, p, 0, p);
    if (len & 7) return fail(kErrMalformedBatch, p, 0, p);
    if (op == kJob) {
      uint32_t slot = (w0 >> 6) & 0x3FFF;
      const uint32_t id = (w0 >> 6) & 0xFFFFF;
      if (slot >= kInstanceSlots) {
        slot = 0;
        if (id != kSpecialInstance) return fail(kErrInvalidInstance, p, 0, p);
      }
      if (std::find(taken.begin(), taken.end(), slot) == taken.end()) {
        Instance& in = g_instances[slot];
        if (!in.live || in.id != id || (id != kSpecialInstance && in.ctx != ctx))
          return fail(kErrInvalidInstance, p, 0, p);
        if (in.pending && !in.violation_allowed) {
          if (id != kSpecialInstance && in.priority != priority)
            return fail(kErrPriorityViolation, p, 0, p);
        } else {
          in.priority = priority;
          in.violation_allowed = in.flags >> 11 & 1;
        }
        in.pending++;
        taken.push_back(slot);
      }
      jobs.push_back({slot, p});
    }
    const gaddr next = p + 8 + len;
    if (next > end) return fail(kErrMalformedBatch, p, 0, p);
    p = next;
  }
  if (p != end) return fail(kErrOutOfMemory, 0, 0, 0);

  // Pass 2: each job's body, in instance order (11.00 sorts by its internal
  // routing word, then by position).
  std::vector<Entry> order = jobs;
  std::stable_sort(order.begin(), order.end(), [](const Entry& a, const Entry& b) {
    return g_instances[a.slot].id < g_instances[b.slot].id;
  });
  std::map<gaddr, Job> built;
  for (const Entry& e : order) {
    const gaddr body_end = e.header + 8 + Rd32(e.header + 4);
    Job job;
    job.instance = g_instances[e.slot].id;
    int commands = 0;
    for (gaddr q = e.header + 8; q < body_end;) {
      const uint32_t w0 = Rd32(q), w1 = Rd32(q + 4), op = w0 & 0xF;
      const uint32_t at = uint32_t(q - e.header);
      const bool wide = op == kData || op == kSideband || op == kDebug;
      if (wide && q + 16 > body_end) return fail(kErrMalformedBatch, e.header, at, e.header);
      switch (op) {
        case kData:
        case kSideband: {
          const gaddr addr = Rd64(q + 8);
          const bool out = w0 & 0x10;
          if (w1 && !Registered(ctx, addr) && ((addr & 0x3FF) + uint64_t(w1) + 1023) / 1024 > 254)
            return fail(kErrBufferTooBig, e.header, at, e.header);
          if (op == kData && out) job.out.push_back({addr, w1});
          else if (op == kSideband && out) job.sb_out = {addr, w1};
          else if (w1) {
            std::vector<uint8_t>& dst = op == kData ? job.in : job.sb_in;
            const uint8_t* src = recomp::GPtr(addr);
            dst.insert(dst.end(), src, src + w1);
          }
          break;
        }
        case kControl:
        case kRun:
          if ((w1 & 7) != 1) return fail(kErrWrongRevision, e.header, at, e.header);
          job.control = op == kControl;
          job.flags = uint64_t(w1) | uint64_t((w0 >> 6) & 0xFFFFF) << 32;
          commands++;
          break;
        case kFlush:
          recomp::FatalF("libSceAjm: FLUSH commands not implemented");
      }
      q += wide ? 16 : 8;
    }
    if (commands != 1)
      recomp::FatalF("libSceAjm: a job with %d CONTROL/RUN commands not implemented", commands);
    built[e.header] = std::move(job);
  }

  if (g_free_batches.empty()) {
    for (uint32_t s : taken) Release(g_instances[s]);
    return kErrOutOfResources;
  }
  const uint32_t id = g_free_batches.front();
  g_free_batches.pop_front();
  Batch& b = g_batches[id];
  b = Batch{};
  b.live = true, b.ctx = ctx;
  for (const Entry& e : jobs) b.jobs.push_back(std::move(built[e.header]));
  b.instances = taken;
  Wr32(out_id, id);
  return 0;
}

// The batch's end: its jobs run (unless cancelled), its references and id
// are released.
uint64_t Finish(Batch& b, uint32_t id) {
  if (!b.cancelled)
    for (const Job& job : b.jobs) RunJob(job);
  for (uint32_t s : b.instances) Release(g_instances[s]);
  const bool cancelled = b.cancelled;
  b = Batch{};
  g_free_batches.push_back(id);
  return cancelled ? kErrCancelled : 0;
}

// A context's descriptor closed: its batches end without running (11.00
// waits for them and does not copy their outputs back), then its
// instances, registrations and memory go.
void CloseContext(uint32_t ctx) {
  for (uint32_t id = 0; id < g_batches.size(); id++)
    if (g_batches[id].live && g_batches[id].ctx == ctx) {
      g_batches[id].cancelled = true;
      Finish(g_batches[id], id);
    }
  for (uint32_t i = 1; i < kInstanceSlots; i++)
    if (g_instances[i].live && g_instances[i].ctx == ctx) {
      g_instances[i] = Instance{};
      g_free_instances.push_back(i);
    }
  g_regions.erase(std::remove_if(g_regions.begin(), g_regions.end(),
                                 [&](const Region& r) { return r.ctx == ctx; }),
                  g_regions.end());
  g_contexts.erase(ctx);
}

}  // namespace

using recomp::hle::Arg;

// int sceAjmInitialize(int64_t reserved, SceAjmContextId* ctx): 11.00's
// context is the descriptor of /dev/ajm; ours is a small integer of its own.
ORBIS_HLE("libSceAjm", sceAjmInitialize, 2) {
  if (!Arg(c, 1)) return kErrInvalidParameter;
  Wr32(Arg(c, 1), 0xFFFFFFFF);
  if (Arg(c, 0)) return kErrInvalidParameter;
  std::lock_guard<std::mutex> l(g_mu);
  InitPools();
  const uint32_t ctx = g_next_context++;
  g_contexts[ctx];
  Wr32(Arg(c, 1), ctx);
  return 0;
}

ORBIS_HLE("libSceAjm", sceAjmFinalize, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t ctx = uint32_t(Arg(c, 0));
  if (!FindContext(ctx)) return kErrInvalidContext;
  CloseContext(ctx);
  return 0;
}

// int sceAjmMemoryRegister(ctx, void* addr, uint64_t pages): 2 MiB pages
// the ACP uses in place. 11.00 also requires direct memory in one piece;
// ours takes any memory.
ORBIS_HLE("libSceAjm", sceAjmMemoryRegister, 3) {
  const gaddr addr = Arg(c, 1);
  const uint64_t pages = Arg(c, 2);
  if (!addr || !pages) return kErrInvalidParameter;
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t ctx = uint32_t(Arg(c, 0));
  if (!FindContext(ctx)) return kErrInvalidContext;
  if (addr & 0x3FFF || g_regions.size() + pages > 16) return kErrInvalidParameter;
  for (const Region& r : g_regions)
    if (r.page < addr + (1u << 21) && addr < r.page + (1u << 21)) return kErrInvalidParameter;
  if (32 - g_regions.size() < pages) return kErrOutOfResources;
  for (uint64_t i = 0; i < pages; i++) g_regions.push_back({ctx, addr, addr + (i << 21)});
  return 0;
}

ORBIS_HLE("libSceAjm", sceAjmMemoryUnregister, 2) {
  const gaddr addr = Arg(c, 1);
  if (!addr) return kErrInvalidParameter;
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t ctx = uint32_t(Arg(c, 0));
  if (!FindContext(ctx)) return kErrInvalidContext;
  const size_t before = g_regions.size();
  g_regions.erase(std::remove_if(g_regions.begin(), g_regions.end(),
                                 [&](const Region& r) { return r.ctx == ctx && r.base == addr; }),
                  g_regions.end());
  return g_regions.size() == before ? kErrFailed : 0;
}

// int sceAjmModuleRegister(ctx, SceAjmCodecType codec, int64_t reserved):
// games may not register AC3, CELP(16) decoding, Vorbis, SRC or FLAC.
ORBIS_HLE("libSceAjm", sceAjmModuleRegister, 3) {
  const uint32_t codec = uint32_t(Arg(c, 1));
  if (codec > 22 || Arg(c, 2)) return kErrInvalidParameter;
  std::lock_guard<std::mutex> l(g_mu);
  Context* x = FindContext(uint32_t(Arg(c, 0)));
  if (!x) return kErrInvalidContext;
  if (codec < 21 && 0x1850C0 >> codec & 1) return kErrCodecNotSupported;
  if (x->registered[codec]) return kErrCodecAlreadyRegistered;
  if (codec == 22) return kErrCodecNotSupported;
  x->registered[codec] = true;
  return 0;
}

ORBIS_HLE("libSceAjm", sceAjmModuleUnregister, 2) {
  const uint32_t codec = uint32_t(Arg(c, 1));
  if (codec > 22) return kErrInvalidParameter;
  std::lock_guard<std::mutex> l(g_mu);
  Context* x = FindContext(uint32_t(Arg(c, 0)));
  if (!x) return kErrInvalidContext;
  if (!x->registered[codec]) return kErrCodecNotRegistered;
  if (x->instances[codec]) return kErrBusy;
  x->registered[codec] = false;
  return 0;
}

// int sceAjmInstanceCreate(ctx, codec, uint64_t flags, SceAjmInstanceId*
// out): id = slot | codec << 14, slots from a FIFO that starts at 1. The
// flags are kept but for bits 17-35; bits 7-9 are the output format.
ORBIS_HLE("libSceAjm", sceAjmInstanceCreate, 4) {
  const uint32_t codec = uint32_t(Arg(c, 1));
  const uint64_t flags = Arg(c, 2) & 0xFFFFFFF00001FFFFull;
  const gaddr out = Arg(c, 3);
  if (!out) return kErrInvalidParameter;
  Wr32(out, 0xFFFFFFFF);
  if (codec > 22) return kErrInvalidParameter;
  if ((Arg(c, 2) >> 7 & 7) > 2) return kErrFlagNotSupported;
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t ctx = uint32_t(Arg(c, 0));
  Context* x = FindContext(ctx);
  if (!x) return kErrInvalidContext;
  if ((flags & 7) != 1) return kErrWrongRevision;
  if (!x->registered[codec]) return kErrCodecNotRegistered;
  if (codec != kCodecMp3 && codec != kCodecAt9)
    recomp::FatalF("libSceAjm: codec %u is not implemented yet", codec);
  if (g_free_instances.empty()) return kErrOutOfResources;
  const uint32_t slot = g_free_instances.front();
  g_free_instances.pop_front();
  Instance& in = g_instances[slot];
  in = Instance{};
  in.live = true, in.ctx = ctx, in.codec = codec, in.flags = flags;
  in.id = slot | codec << 14;
  x->instances[codec]++;
  Wr32(out, in.id);
  return 0;
}

// Multi-codec instances (Extend) and switching between their codecs.
static uint64_t MultiCodec(const recomp::Ctx& c, const char* name) {
  if (uint32_t(Arg(c, 1)) > 22) return kErrInvalidParameter;
  if ((Arg(c, 2) >> 7 & 7) > 2) return kErrFlagNotSupported;
  recomp::FatalF("libSceAjm: %s (multi-codec instances) not implemented", name);
}
ORBIS_HLE("libSceAjm", sceAjmInstanceExtend, 4) { return MultiCodec(c, "sceAjmInstanceExtend"); }
ORBIS_HLE("libSceAjm", sceAjmInstanceSwitch, 4) { return MultiCodec(c, "sceAjmInstanceSwitch"); }

ORBIS_HLE("libSceAjm", sceAjmInstanceDestroy, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t ctx = uint32_t(Arg(c, 0));
  Context* x = FindContext(ctx);
  if (!x) return kErrInvalidContext;
  Instance* in = FindInstance(ctx, uint32_t(Arg(c, 1)));
  if (!in) return kErrInvalidInstance;
  if (in->pending) return kErrBusy;
  x->instances[in->codec]--;
  const uint32_t slot = in->id & 0x3FFF;
  *in = Instance{};
  g_free_instances.push_back(slot);
  return 0;
}

ORBIS_HLE("libSceAjm", sceAjmInstanceCodecType, 1) { return uint32_t(Arg(c, 0)) >> 14; }

// int sceAjmBatchStartBuffer(ctx, const void* batch, uint64_t size, int
// priority, SceAjmBatchError* err, SceAjmBatchId* out): games' priorities
// are 10-90, their batch ids 0-190 from a FIFO shared by all contexts.
ORBIS_HLE("libSceAjm", sceAjmBatchStartBuffer, 6) {
  const gaddr out = Arg(c, 5);
  if (!out) return kErrInvalidParameter;
  Wr32(out, 0xFFFFFFFF);
  if (!Arg(c, 1) || !Arg(c, 2)) return kErrInvalidParameter;
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t ctx = uint32_t(Arg(c, 0));
  if (!FindContext(ctx)) return kErrInvalidContext;
  return BatchStart(ctx, Arg(c, 1), uint32_t(Arg(c, 2)), int(Arg(c, 3)), Arg(c, 4), out);
}

// int sceAjmBatchWait(ctx, SceAjmBatchId batch, uint32_t timeout,
// SceAjmBatchError* err): ours never has a batch in progress.
ORBIS_HLE("libSceAjm", sceAjmBatchWait, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t ctx = uint32_t(Arg(c, 0)), id = uint32_t(Arg(c, 1));
  if (!FindContext(ctx)) return kErrInvalidContext;
  if (id >= g_batches.size() || !g_batches[id].live || g_batches[id].ctx != ctx)
    return kErrInvalidBatch;
  return Finish(g_batches[id], id);
}

ORBIS_HLE("libSceAjm", sceAjmBatchCancel, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  const uint32_t ctx = uint32_t(Arg(c, 0)), id = uint32_t(Arg(c, 1));
  if (!FindContext(ctx)) return kErrInvalidContext;
  if (id >= g_batches.size() || !g_batches[id].live || g_batches[id].ctx != ctx)
    return kErrInvalidBatch;
  g_batches[id].cancelled = true;
  return 0;
}

// The job builders: they read the words they write and keep some bits of
// them, as 11.00's do.
static void Header(gaddr p, uint32_t keep, uint32_t bits, uint32_t dword1) {
  Wr32(p, (Rd32(p) & keep) | bits);
  Wr32(p + 4, dword1);
}
static gaddr BufferWord(gaddr p, uint32_t op, bool out, uint32_t size, gaddr addr) {
  Header(p, 0xFFFFFFE0, op | (out ? 0x10 : 0), size);
  Wr64(p + 8, addr);
  return p + 16;
}
static gaddr FlagsWord(gaddr p, uint32_t op, uint64_t flags) {
  Header(p, 0xFC000030, op | uint32_t(flags >> 26 & 0x3FFFFC0), uint32_t(flags));
  return p + 8;
}
// The JOB word and the DEBUG one when ra is set; returns the body.
static gaddr JobWord(gaddr p, uint32_t instance, uint32_t body, gaddr ra) {
  Header(p, 0xFC000030, (instance << 6) & 0x3FFFFC0, body + (ra ? 16 : 0));
  if (!ra) return p + 8;
  Header(p + 8, 0xFFFFFFF0, kDebug, 0);
  Wr64(p + 16, ra);
  return p + 24;
}

// void* sceAjmBatchJobControlBufferRa(void* p, instance, uint64_t flags,
// const void* sb_in, size_t sb_in_size, void* sb_out, size_t sb_out_size,
// void* ra)
ORBIS_HLE("libSceAjm", sceAjmBatchJobControlBufferRa, 8) {
  const uint32_t instance = uint32_t(Arg(c, 1));
  const uint64_t flags = Arg(c, 2) & (instance == kSpecialInstance ? 0xC0018007ull : 0x60000000E7FFull);
  gaddr p = JobWord(Arg(c, 0), instance, 40, Arg(c, 7));
  p = BufferWord(p, kSideband, false, uint32_t(Arg(c, 4)), Arg(c, 3));
  p = FlagsWord(p, kControl, flags);
  return BufferWord(p, kSideband, true, uint32_t(Arg(c, 6)), Arg(c, 5));
}

// void* sceAjmBatchJobRunBufferRa(void* p, instance, uint64_t flags, const
// void* in, size_t in_size, void* out, size_t out_size, void* sb_out,
// size_t sb_out_size, void* ra)
ORBIS_HLE("libSceAjm", sceAjmBatchJobRunBufferRa, 10) {
  gaddr p = JobWord(Arg(c, 0), uint32_t(Arg(c, 1)), 56, Arg(c, 9));
  p = BufferWord(p, kData, false, uint32_t(Arg(c, 4)), Arg(c, 3));
  p = FlagsWord(p, kRun, Arg(c, 2) & 0xE00000001FFFull);
  p = BufferWord(p, kData, true, uint32_t(Arg(c, 6)), Arg(c, 5));
  return BufferWord(p, kSideband, true, uint32_t(Arg(c, 8)), Arg(c, 7));
}

// void* sceAjmBatchJobRunSplitBufferRa(void* p, instance, uint64_t flags,
// const SceAjmBuffer* ins, size_t num_ins, const SceAjmBuffer* outs,
// size_t num_outs, void* sb_out, size_t sb_out_size, void* ra);
// SceAjmBuffer: {void* addr; size_t size}.
ORBIS_HLE("libSceAjm", sceAjmBatchJobRunSplitBufferRa, 10) {
  const gaddr ins = Arg(c, 3), outs = Arg(c, 5);
  const uint64_t nin = Arg(c, 4), nout = Arg(c, 6);
  gaddr p = JobWord(Arg(c, 0), uint32_t(Arg(c, 1)), uint32_t(16 * nin + 16 * nout + 24), Arg(c, 9));
  for (uint64_t i = 0; i < nin; i++)
    p = BufferWord(p, kData, false, Rd32(ins + 16 * i + 8), Rd64(ins + 16 * i));
  p = FlagsWord(p, kRun, Arg(c, 2) & 0xE00000001FFFull);
  for (uint64_t i = 0; i < nout; i++)
    p = BufferWord(p, kData, true, Rd32(outs + 16 * i + 8), Rd64(outs + 16 * i));
  return BufferWord(p, kSideband, true, uint32_t(Arg(c, 8)), Arg(c, 7));
}

// void* sceAjmBatchJobInlineBuffer(void* p, const void* data, size_t size,
// const void** out): the data in the batch (its padding left as it was).
ORBIS_HLE("libSceAjm", sceAjmBatchJobInlineBuffer, 4) {
  const gaddr p = Arg(c, 0);
  const uint64_t size = Arg(c, 2), padded = (size + 7) & ~7ull;
  Header(p, 0xFFFFFFE0, kInline, uint32_t(padded));
  std::memmove(recomp::GPtr(p + 8), recomp::GPtr(Arg(c, 1)), size);
  Wr64(Arg(c, 3), p + 8);
  return p + 8 + padded;
}

// void sceAjmBatchErrorDump(const SceAjmBatchInfo* info, const
// SceAjmBatchError* err): 11.00 prints the batch's words around the error
// to stderr; ours logs the error block.
ORBIS_HLE("libSceAjm", sceAjmBatchErrorDump, 2) {
  const gaddr err = Arg(c, 1);
  if (!Arg(c, 0) || !err) return 0;
  recomp::WarnF("libSceAjm: batch error %08X at job %llX + %u (ra %llX)", Rd32(err),
                (unsigned long long)Rd64(err + 8), Rd32(err + 16),
                (unsigned long long)Rd64(err + 24));
  return 0;
}

// int sceAjmDecAt9ParseConfigData(const uint8_t config[4], uint32_t out[5]):
// channels, sample rate, samples per frame and per superframe, bytes per
// superframe.
ORBIS_HLE("libSceAjm", sceAjmDecAt9ParseConfigData, 2) {
  const gaddr cfg = Arg(c, 0), out = Arg(c, 1);
  At9Config a;
  if (!cfg || !out || !ParseAt9(recomp::GPtr(cfg), a)) return kErrInvalidParameter;
  Wr32(out, a.channels);
  Wr32(out + 4, a.rate);
  Wr32(out + 8, a.frame_samples);
  Wr32(out + 12, a.frame_samples * a.frames);
  Wr32(out + 16, a.frame_bytes * a.frames);
  return 0;
}

// int sceAjmDecMp3ParseFrame(const uint8_t* buf, size_t size, int
// parse_tag, SceAjmDecMp3ParseFrame* out)
ORBIS_HLE("libSceAjm", sceAjmDecMp3ParseFrame, 4) {
  const gaddr buf = Arg(c, 0), out = Arg(c, 3);
  const uint64_t size = Arg(c, 1);
  Mp3Frame f;
  if (!buf || !out || !ParseMp3Frame(recomp::GPtr(buf), size, int(Arg(c, 2)) != 0, f))
    return kErrInvalidParameter;
  Wr64(out, f.frame_size);
  const uint32_t words[] = {f.channels, f.samples, f.bitrate, f.rate, f.delay, f.num_frames,
                            f.total_samples, f.tag};
  for (int i = 0; i < 8; i++) Wr32(out + 8 + 4 * i, words[i]);
  return 0;
}

// const char* sceAjmStrError(int code): the code's name (11.00 has a text
// of its own for each).
ORBIS_HLE("libSceAjm", sceAjmStrError, 1) {
  static const char* const kNames[] = {
      "SCE_AJM_ERROR_INVALID_CONTEXT", "SCE_AJM_ERROR_INVALID_INSTANCE",
      "SCE_AJM_ERROR_INVALID_BATCH", "SCE_AJM_ERROR_INVALID_PARAMETER",
      "SCE_AJM_ERROR_OUT_OF_MEMORY", "SCE_AJM_ERROR_OUT_OF_RESOURCES",
      "SCE_AJM_ERROR_CODEC_NOT_SUPPORTED", "SCE_AJM_ERROR_CODEC_ALREADY_REGISTERED",
      "SCE_AJM_ERROR_CODEC_NOT_REGISTERED", "SCE_AJM_ERROR_WRONG_REVISION_FLAG",
      "SCE_AJM_ERROR_FLAG_NOT_SUPPORTED", "SCE_AJM_ERROR_BUSY",
      "SCE_AJM_ERROR_BAD_PRIORITY", "SCE_AJM_ERROR_IN_PROGRESS", "SCE_AJM_ERROR_RETRY",
      "SCE_AJM_ERROR_MALFORMED_BATCH", "SCE_AJM_ERROR_JOB_CREATION",
      "SCE_AJM_ERROR_INVALID_OPCODE", "SCE_AJM_ERROR_PRIORITY_VIOLATION",
      "SCE_AJM_ERROR_BUFFER_TOO_BIG", "SCE_AJM_ERROR_INVALID_ADDRESS", "SCE_AJM_ERROR_CANCELLED"};
  constexpr size_t kCount = sizeof kNames / sizeof *kNames;
  static gaddr strings[kCount + 2];
  static std::once_flag once;
  std::call_once(once, [] {
    static recomp::GuestHeap heap(recomp::g_proc->vm, 4096, 0);
    auto put = [](const char* s) {
      const gaddr p = heap.Alloc(std::strlen(s) + 1, false);
      recomp::PutStr(p, s);
      return p;
    };
    strings[0] = put("SCE_OK");
    for (size_t i = 0; i < kCount; i++) strings[i + 1] = put(kNames[i]);
    strings[kCount + 1] = put("(unknown)");
  });
  const uint32_t code = uint32_t(Arg(c, 0));
  if (code == 0) return strings[0];
  if (code >= kErrInvalidContext && code <= kErrCancelled) return strings[code - kErrInvalidContext + 1];
  return strings[kCount + 1];
}
