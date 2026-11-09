// libSceVideodec2 HLE: the video decoder. On the console the library
// sits on libSceVdecCore, which decodes on a GPU compute queue in memory
// the caller provides; the sizes and checks below are those of
// libSceVideodec2 and libSceVdecCore 11.00. The library checks each
// pointer with sceKernelVirtualQuery; here a null pointer gets the same
// error and any other pointer is taken as mapped (an unmapped one faults
// instead of returning an error). Where it checks the memory type the
// query reports, memory the process mapped itself (not recorded) is
// fatal.

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/log.h"
#include "x86/runtime.h"

namespace {

constexpr uint32_t kErrStructSize = 0x811D0101, kErrBadPointer = 0x811D0102,
                   kErrMemorySize = 0x811D0104, kErrBadMemory = 0x811D0105,
                   kErrOnionMemory = 0x811D0109, kErrBadQueue = 0x811D0110,
                   kErrConfig = 0x811D0200, kErrPipe = 0x811D0201, kErrQueue = 0x811D0202;
constexpr int kWbOnion = 0;  // SCE_KERNEL_WB_ONION

// Memory a structure points to: 0, or the error for a null pointer
// (bad) or, with check, memory of another type than want (wrong_type).
uint32_t CheckMemory(recomp::gaddr p, bool check, int want, uint32_t bad, uint32_t wrong_type) {
  if (!p) return bad;
  if (!check) return 0;
  int type;
  if (!recomp::orbis::kernel::MemoryType(p, &type))
    recomp::FatalF("libSceVideodec2: the memory type of %llX (not mapped by the game): not implemented yet",
                   (unsigned long long)p);
  return type == want ? 0 : wrong_type;
}

// libSceVdecCore's compute resource: its shader state (4120 bytes, rounded
// up to its 256-byte alignment) and 4800768 bytes of working memory.
constexpr uint64_t kComputeMemorySize = ((4120 + 255) & ~uint64_t(255)) + 4800768;
// The compute resource's object, where it lies in that memory (the
// handle): past the shader state and the queue's ring.
constexpr uint64_t kComputeQueueAt = 0x290500;

// Compute queues by handle: the decoders using each (a queue they use is
// not released).
std::mutex g_mu;
std::map<recomp::gaddr, int> g_queues;

constexpr uint32_t kErrDecoder = 0x811D0100, kErrBadDecoder = 0x811D0103,
                   kErrFrameSize = 0x811D0106, kErrNoFrameBuffer = 0x811D0107,
                   kErrFrameAlign = 0x811D0108, kErrGarlicMemory = 0x811D010A,
                   kErrReserved = 0x811D010C, kErrNoAuSize = 0x811D010D, kErrNoAu = 0x811D010E,
                   kErrPictureCount = 0x811D010F, kErrKind = 0x811D0203, kErrCodec = 0x811D0204,
                   kErrProfileLevel = 0x811D0205, kErrDepth = 0x811D0206, kErrAffinity = 0x811D0207,
                   kErrPriority = 0x811D0208, kErrDpb = 0x811D0209, kErrExtra = 0x811D020B;
constexpr int kWcGarlic = 3;  // SCE_KERNEL_WC_GARLIC
constexpr uint32_t kAvc = 1;

// What libSceVdecCore asks for a configuration, measured on the console
// (11.00) for those games use: the three memories, the largest frame
// buffer and its alignment, and how many access units it takes in before
// it hands out a picture (one per access unit after that).
struct Sizes {
  uint32_t profile, level, width, height;
  int32_t dpb, depth;
  uint8_t flag60;  // config byte 60
  uint64_t cpu, gpu, cpu_gpu, frame;
  uint32_t align, delay;
};
constexpr Sizes kMeasured[] = {
    {100, 42, 1920, 1088, -1, 4, 0, 0x334CC00, 0x48EAE00, 0x304B000, 0x331400, 0x100, 8},
};

// SceVideodec2DecoderConfigInfo (72 bytes): { thisSize; uint32_t kind (1),
// codecType (1: AVC), profile, level, maxFrameWidth, maxFrameHeight;
// int32_t maxDpbFrameCount, decodePipelineDepth; computeQueue; uint64_t
// cpuAffinityMask; int32_t cpuThreadPriority; uint8_t byte 60,
// checkMemoryType, 2 reserved (0); void* extra (0 for AVC) }. Checked in
// libSceVideodec2's order; the kinds and codecs it takes beside AVC of
// kind 1 are fatal, and so are sizes not measured.
uint32_t CheckConfig(recomp::gaddr cfg, const Sizes** out) {
  const uint32_t kind = recomp::Rd32(cfg + 8), codec = recomp::Rd32(cfg + 12);
  if (kind != 1) {
    if (kind == 74628 || kind == 46792)
      recomp::FatalF("libSceVideodec2: decoder kind %u not implemented yet", kind);
    return kErrKind;
  }
  if (codec == 974921) recomp::FatalF("libSceVideodec2: HEVC not implemented yet");
  if (codec != kAvc) return kErrCodec;
  if (recomp::Rd64(cfg + 64)) return kErrExtra;
  static const uint32_t kLevels[] = {10, 11, 12, 13, 20, 21, 22, 30, 31, 32,
                                     40, 41, 42, 50, 51, 52, 60, 61, 62, 111};
  const uint32_t profile = recomp::Rd32(cfg + 16), level = recomp::Rd32(cfg + 20);
  if ((profile != 66 && profile != 77 && profile != 100) ||
      std::find(std::begin(kLevels), std::end(kLevels), level) == std::end(kLevels))
    return kErrProfileLevel;
  const uint8_t* b = recomp::GPtr(cfg);
  if (b[62] || b[63]) return kErrConfig;
  const int32_t depth = int32_t(recomp::Rd32(cfg + 36)), dpb = int32_t(recomp::Rd32(cfg + 32));
  if (uint32_t(depth - 1) >= 8) return kErrDepth;
  if (uint32_t(dpb + 1) >= 18) return kErrDpb;
  const uint32_t prio = recomp::Rd32(cfg + 56);
  if (prio != ~0u && prio - 256 >= 512) return kErrPriority;
  // Cores 0-5, or 0-6 in the CPU mode with the seventh (not modelled).
  const uint64_t affinity = recomp::Rd64(cfg + 48);
  if (affinity & ~uint64_t(0x7F)) return kErrAffinity;
  if (affinity & 0x40) recomp::FatalF("libSceVideodec2: the seventh core in cpuAffinityMask not implemented yet");
  const uint32_t w = recomp::Rd32(cfg + 24), h = recomp::Rd32(cfg + 28);
  for (const Sizes& s : kMeasured)
    if (s.profile == profile && s.level == level && s.width == w && s.height == h && s.dpb == dpb &&
        s.depth == depth && s.flag60 == b[60]) {
      *out = &s;
      return 0;
    }
  recomp::FatalF("libSceVideodec2: sizes for AVC profile %u level %u, %ux%u, DPB %d, depth %d, byte 60 %u "
                 "not measured yet",
                 profile, level, w, h, dpb, depth, b[60]);
}

// An AVC sequence parameter set, as far as the picture info reports it.
struct Sps {
  uint32_t profile = 0, level = 0, max_num_ref = 0, width_mbs1 = 0, height_units1 = 0;
  bool frame_mbs_only = false, crop = false;
  uint32_t crop_lrtb[4] = {};
  uint32_t units_in_tick = 0, time_scale = 0, max_dec_frame_buffering = 0;
  bool operator==(const Sps&) const = default;  // not memcmp: the padding is not set
};

class Bits {
 public:
  Bits(const uint8_t* p, size_t n) {  // a NAL unit's payload, emulation prevention removed
    for (size_t i = 0, zeros = 0; i < n; i++) {
      if (zeros >= 2 && p[i] == 3) {
        zeros = 0;
        continue;
      }
      zeros = p[i] ? 0 : zeros + 1;
      b_.push_back(p[i]);
    }
  }
  uint32_t U(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, pos_++) {
      if (pos_ >= 8 * b_.size()) recomp::FatalF("libSceVideodec2: a parameter set past its end");
      v = v << 1 | ((b_[pos_ / 8] >> (7 - pos_ % 8)) & 1);
    }
    return v;
  }
  uint32_t Ue() {
    int zeros = 0;
    while (!U(1))
      if (++zeros > 31) recomp::FatalF("libSceVideodec2: a bad exp-Golomb code in a parameter set");
    return (uint32_t(1) << zeros) - 1 + U(zeros);
  }

 private:
  std::vector<uint8_t> b_;
  size_t pos_ = 0;
};

// Parses an SPS NAL unit's payload (after the header byte). What the
// picture info cannot report is fatal: field coding, scaling matrices,
// picture order type 1, and VUI flags other than those of the streams
// measured (below).
Sps ParseSps(const uint8_t* p, size_t n) {
  Bits r(p, n);
  Sps s;
  s.profile = r.U(8);
  r.U(8);
  s.level = r.U(8);
  r.Ue();
  static const uint32_t kHigh[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
  if (std::find(std::begin(kHigh), std::end(kHigh), s.profile) != std::end(kHigh)) {
    if (r.Ue() == 3) r.U(1);
    r.Ue();
    r.Ue();
    r.U(1);
    if (r.U(1)) recomp::FatalF("libSceVideodec2: SPS scaling matrices not implemented yet");
  }
  r.Ue();
  const uint32_t poc = r.Ue();
  if (poc == 0) r.Ue();
  else if (poc == 1) recomp::FatalF("libSceVideodec2: picture order count type 1 not implemented yet");
  s.max_num_ref = r.Ue();
  r.U(1);
  s.width_mbs1 = r.Ue();
  s.height_units1 = r.Ue();
  s.frame_mbs_only = r.U(1);
  if (!s.frame_mbs_only) recomp::FatalF("libSceVideodec2: field coding not implemented yet");
  r.U(1);
  s.crop = r.U(1);
  if (s.crop)
    for (uint32_t& v : s.crop_lrtb) v = r.Ue();
  // The VUI the measured streams have: timing and bitstream restrictions
  // only, motion vectors over picture boundaries, a variable frame rate
  // and max_dec_frame_buffering = max_num_ref_frames (the picture info's
  // flags and that count are only known for that shape).
  bool ok = r.U(1);                              // vui_parameters_present_flag
  ok = ok && !r.U(1) && !r.U(1) && !r.U(1) && !r.U(1);  // aspect ratio, overscan, video signal, chroma loc
  ok = ok && r.U(1);                             // timing_info_present_flag
  if (ok) {
    s.units_in_tick = r.U(32);
    s.time_scale = r.U(32);
    ok = !r.U(1) && !r.U(1) && !r.U(1) && !r.U(1);  // fixed frame rate, NAL HRD, VCL HRD, pic_struct
    ok = ok && r.U(1) && r.U(1);                    // bitstream_restriction_flag, motion vectors over
  }
  if (ok) {
    r.Ue();
    r.Ue();
    r.Ue();
    r.Ue();
    r.Ue();
    s.max_dec_frame_buffering = r.Ue();
    ok = s.max_dec_frame_buffering == s.max_num_ref;
  }
  if (!ok) recomp::FatalF("libSceVideodec2: an SPS's VUI unlike the streams measured: not implemented yet");
  return s;
}

// SceVideodec2InputData (48 bytes): { thisSize; void* auData; size_t
// auSize; uint64_t pts, dts, attachedData }.
struct Au {
  uint64_t pts = 0, dts = 0, attached = 0;
};

// A picture handed out: what sceVideodec2GetPictureInfo reports of it.
// The console keeps it in the frame buffer's last KiB (here: by the
// frame buffer's address, so the next picture in the same buffer
// replaces it).
struct Picture {
  Au au;
  Sps sps;
};

struct Decoder {
  const Sizes* sizes = nullptr;
  recomp::gaddr queue = 0;
  AVCodecContext* ctx = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;
  std::map<int64_t, Au> aus;  // by the packet's sequence number (the frame's pts)
  int64_t seq = 0;
  bool have_sps = false;
  Sps sps;
  uint64_t count = 0;    // access units since creation or the last flush
  uint32_t pending = 0;  // pictures decoded and not handed out yet
  bool flushing = false;
  std::deque<std::pair<AVFrame*, Picture>> ready;  // decoded, in output order
};
std::map<recomp::gaddr, Decoder*> g_decoders;  // by handle (the CPU memory)
std::map<recomp::gaddr, Picture> g_pictures;   // by frame buffer

Decoder* Find(recomp::gaddr h) {
  auto it = g_decoders.find(h);
  return it == g_decoders.end() ? nullptr : it->second;
}

// Takes the frames the decoder has finished into d.ready.
void Receive(Decoder& d) {
  for (;;) {
    const int e = avcodec_receive_frame(d.ctx, d.frame);
    if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) return;
    if (e < 0) recomp::FatalF("libSceVideodec2: decoding failed (%d)", e);
    if (d.frame->format != AV_PIX_FMT_YUV420P)
      recomp::FatalF("libSceVideodec2: a picture in pixel format %d not implemented yet", d.frame->format);
    auto it = d.aus.find(d.frame->pts);
    if (it == d.aus.end()) recomp::FatalF("libSceVideodec2: a picture of no access unit");
    AVFrame* f = av_frame_clone(d.frame);
    d.ready.push_back({f, {it->second, d.sps}});
    d.aus.erase(it);
    av_frame_unref(d.frame);
  }
}

// The access unit's SPS (one per stream: more are fatal).
void ReadSps(Decoder& d, const uint8_t* p, size_t n) {
  for (size_t i = 0; i + 3 < n; i++) {
    if (p[i] || p[i + 1] || p[i + 2] != 1 || (p[i + 3] & 0x1F) != 7) continue;
    size_t end = i + 4;
    while (end + 2 < n && !(p[end] == 0 && p[end + 1] == 0 && (p[end + 2] == 1 || p[end + 2] == 0))) end++;
    if (end + 2 >= n) end = n;
    const Sps s = ParseSps(p + i + 4, end - i - 4);
    if (d.have_sps && s != d.sps)
      recomp::FatalF("libSceVideodec2: a second SPS not implemented yet");
    d.sps = s;
    d.have_sps = true;
  }
}

// The picture as the console leaves it (measured, 11.00): NV12, the
// luma's coded rows at a pitch of the coded width, the interleaved chroma
// right after them. A picture smaller than the decoder's maximum is not
// measured (its pitch): fatal. The console also leaves a record of the
// picture in the buffer's last KiB (GetPictureInfo's source), which is
// not written here.
void WriteFrame(const Decoder& d, recomp::gaddr fb, const AVFrame* f) {
  const int w = f->width, h = f->height;
  if (uint32_t(w) != d.sizes->width || uint32_t(h) != d.sizes->height)
    recomp::FatalF("libSceVideodec2: a %dx%d picture in a %ux%u decoder not implemented yet", w, h,
                   d.sizes->width, d.sizes->height);
  uint8_t* y = recomp::GPtr(fb);
  for (int i = 0; i < h; i++) std::memcpy(y + size_t(i) * w, f->data[0] + size_t(i) * f->linesize[0], w);
  uint8_t* uv = y + size_t(w) * h;
  for (int i = 0; i < h / 2; i++)
    for (int x = 0; x < w / 2; x++) {
      uv[size_t(i) * w + 2 * x] = f->data[1][size_t(i) * f->linesize[1] + x];
      uv[size_t(i) * w + 2 * x + 1] = f->data[2][size_t(i) * f->linesize[2] + x];
    }
}

// libSceVideodec2's output step: `pictures` more decoded; with one
// pending, the next in output order goes into the frame buffer and out
// (SceVideodec2OutputInfo, 48 bytes: { thisSize; bool isValid,
// isErrorFrame; uint8_t pictureCount, byte 11; uint32_t codecType,
// frameWidth, framePitch, frameHeight; 4 bytes; void* frameBuffer;
// size_t frameBufferSize }).
uint32_t Output(Decoder& d, uint32_t pictures, recomp::gaddr fb, recomp::gaddr out) {
  d.pending += pictures;
  if (!d.pending) {
    recomp::GPtr(out)[8] = 0;
    return 0;
  }
  if (d.ready.empty()) recomp::FatalF("libSceVideodec2: a picture due and none decoded");
  auto [f, pic] = d.ready.front();
  d.ready.pop_front();
  const recomp::gaddr buf = recomp::Rd64(fb + 8);
  const uint64_t size = recomp::Rd64(fb + 16);
  WriteFrame(d, buf, f);
  g_pictures[buf] = pic;
  recomp::GPtr(fb)[24] = 1;  // isAccepted
  recomp::Wr64(out + 32, buf);
  recomp::Wr64(out + 40, size);
  d.pending--;
  uint8_t* o = recomp::GPtr(out);
  o[8] = 1;
  o[9] = 0;
  o[10] = 1;
  o[11] = 0;
  recomp::Wr32(out + 12, kAvc);
  recomp::Wr32(out + 16, uint32_t(f->width));
  recomp::Wr32(out + 20, uint32_t(f->width));
  recomp::Wr32(out + 24, uint32_t(f->height));
  av_frame_free(&f);
  return 0;
}

// The checks Decode and Flush share: SceVideodec2FrameBuffer (32 bytes: {
// thisSize; void* frameBuffer; size_t frameBufferSize; bool isAccepted })
// and the output info (48 bytes; the 56-byte one's extra fields are not
// measured: fatal).
uint32_t CheckOutput(recomp::gaddr fb, recomp::gaddr out) {
  if (recomp::Rd64(fb) != 32 || (recomp::Rd64(out) | 8) != 0x38) return kErrStructSize;
  if (recomp::Rd64(out) == 0x38) recomp::FatalF("libSceVideodec2: the 56-byte output info not implemented yet");
  const recomp::gaddr buf = recomp::Rd64(fb + 8);
  if (!buf) return kErrNoFrameBuffer;
  recomp::GPtr(fb)[24] = 0;
  if (buf & 0xFF) return kErrFrameAlign;
  return 0;
}

}  // namespace

using recomp::hle::Arg;

// int sceVideodec2QueryComputeMemoryInfo(SceVideodec2ComputeMemoryInfo* info):
// { size_t thisSize (24); size_t cpuGpuMemorySize; void* cpuGpuMemory }.
ORBIS_HLE("libSceVideodec2", sceVideodec2QueryComputeMemoryInfo, 1) {
  const recomp::gaddr info = Arg(c, 0);
  if (!info) return kErrBadPointer;
  if (recomp::Rd64(info) != 24) return kErrStructSize;
  recomp::Wr64(info + 8, kComputeMemorySize);
  recomp::Wr64(info + 16, 0);
  return 0;
}

// int sceVideodec2AllocateComputeQueue(const SceVideodec2ComputeConfigInfo*
// cfg, const SceVideodec2ComputeMemoryInfo* mem, SceVideodec2ComputeQueue*
// queue): cfg { size_t thisSize (16); uint16_t computePipeId (below 5),
// computeQueueId (below 8); bool checkMemoryType; 3 reserved bytes (0) };
// the memory 256-byte aligned, write-back onion with checkMemoryType.
ORBIS_HLE("libSceVideodec2", sceVideodec2AllocateComputeQueue, 3) {
  const recomp::gaddr cfg = Arg(c, 0), mem = Arg(c, 1), out = Arg(c, 2);
  if (!cfg || !mem || !out) return kErrBadPointer;
  if (recomp::Rd64(cfg) != 16 || recomp::Rd64(mem) != 24) return kErrStructSize;
  if (recomp::GPtr(cfg)[13] || recomp::Rd16(cfg + 14)) return kErrConfig;
  if (recomp::Rd16(cfg + 8) >= 5) return kErrPipe;
  if (recomp::Rd16(cfg + 10) >= 8) return kErrQueue;
  const uint64_t size = recomp::Rd64(mem + 8);
  const recomp::gaddr p = recomp::Rd64(mem + 16);
  if (size < kComputeMemorySize) return kErrMemorySize;
  if (uint32_t e = CheckMemory(p, recomp::GPtr(cfg)[12] != 0, kWbOnion, kErrBadMemory, kErrOnionMemory))
    return e;
  if (p & 0xFF) return kErrConfig;
  const recomp::gaddr q = p + kComputeQueueAt;
  std::lock_guard<std::mutex> l(g_mu);
  // libSceVdecCore refuses memory that holds a live queue depending on
  // the game's SDK version: not modelled.
  if (g_queues.count(q))
    recomp::FatalF("sceVideodec2AllocateComputeQueue: memory %llX holds a queue already: not implemented yet",
                   (unsigned long long)p);
  g_queues[q] = 0;
  recomp::Wr64(out, q);
  return 0;
}

// int sceVideodec2ReleaseComputeQueue(SceVideodec2ComputeQueue queue): not
// a queue, or one a decoder still uses: kErrBadQueue.
ORBIS_HLE("libSceVideodec2", sceVideodec2ReleaseComputeQueue, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_queues.find(Arg(c, 0));
  if (it == g_queues.end() || it->second) return kErrBadQueue;
  g_queues.erase(it);
  return 0;
}

// int sceVideodec2QueryDecoderMemoryInfo(const SceVideodec2DecoderConfigInfo*
// cfg, SceVideodec2DecoderMemoryInfo* mem): mem (72 bytes) { thisSize;
// size_t cpuMemorySize; void* cpuMemory; size_t gpuMemorySize; void*
// gpuMemory; size_t cpuGpuMemorySize; void* cpuGpuMemory; size_t
// maxFrameBufferSize; uint32_t frameBufferAlignment, reserved }. The CPU
// memory holds libSceVideodec2's 256 KiB beside libSceVdecCore's (in the
// sizes measured). The pointers and reserved word are cleared whatever
// the config.
ORBIS_HLE("libSceVideodec2", sceVideodec2QueryDecoderMemoryInfo, 2) {
  const recomp::gaddr cfg = Arg(c, 0), mem = Arg(c, 1);
  if (!cfg || !mem) return kErrBadPointer;
  if (recomp::Rd64(cfg) != 72 || recomp::Rd64(mem) != 72) return kErrStructSize;
  const Sizes* s = nullptr;
  const uint32_t e = CheckConfig(cfg, &s);
  if (!e) {
    recomp::Wr64(mem + 8, s->cpu);
    recomp::Wr64(mem + 24, s->gpu);
    recomp::Wr64(mem + 40, s->cpu_gpu);
    recomp::Wr64(mem + 56, s->frame);
    recomp::Wr32(mem + 64, s->align);
  }
  recomp::Wr32(mem + 68, 0);
  recomp::Wr64(mem + 16, 0);
  recomp::Wr64(mem + 32, 0);
  recomp::Wr64(mem + 48, 0);
  return e;
}

// int sceVideodec2CreateDecoder(const SceVideodec2DecoderConfigInfo* cfg,
// const SceVideodec2DecoderMemoryInfo* mem, SceVideodec2Decoder* out): the
// handle is the CPU memory. With checkMemoryType the CPU (and CPU+GPU)
// memory is write-back onion, the GPU memory write-combined garlic.
ORBIS_HLE("libSceVideodec2", sceVideodec2CreateDecoder, 3) {
  const recomp::gaddr cfg = Arg(c, 0), mem = Arg(c, 1), out = Arg(c, 2);
  if (!cfg || !mem || !out) return kErrBadPointer;
  if (recomp::Rd64(cfg) != 72 || recomp::Rd64(mem) != 72) return kErrStructSize;
  const Sizes* s = nullptr;
  if (uint32_t e = CheckConfig(cfg, &s)) return e;
  const bool check = recomp::GPtr(cfg)[61] != 0;
  const recomp::gaddr cpu = recomp::Rd64(mem + 16);
  if (uint32_t e = CheckMemory(cpu, check, kWbOnion, kErrBadMemory, kErrOnionMemory)) return e;
  if (uint32_t e = CheckMemory(recomp::Rd64(mem + 32), check, kWcGarlic, kErrBadMemory, kErrGarlicMemory))
    return e;
  if (recomp::Rd64(mem + 40))
    if (uint32_t e = CheckMemory(recomp::Rd64(mem + 48), check, kWbOnion, kErrBadMemory, kErrOnionMemory))
      return e;
  if (recomp::Rd32(mem + 68)) return kErrReserved;
  if (recomp::Rd64(mem + 8) < s->cpu || recomp::Rd64(mem + 40) < s->cpu_gpu || recomp::Rd64(mem + 24) < s->gpu)
    return kErrMemorySize;
  if (recomp::Rd64(mem + 56) < s->frame) return kErrFrameSize;
  if (recomp::Rd32(mem + 64) & 0xFF) return kErrFrameAlign;
  const recomp::gaddr queue = recomp::Rd64(cfg + 40);
  if (!queue) return kErrBadQueue;
  std::lock_guard<std::mutex> l(g_mu);
  auto q = g_queues.find(queue);
  if (q == g_queues.end()) return kErrDecoder;
  if (g_decoders.count(cpu))
    recomp::FatalF("sceVideodec2CreateDecoder: memory %llX holds a decoder already: not implemented yet",
                   (unsigned long long)cpu);
  auto* d = new Decoder;
  d->sizes = s;
  d->queue = queue;
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  d->ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!d->ctx) recomp::FatalF("libSceVideodec2: no H.264 decoder");
  d->ctx->thread_count = 1;
  d->ctx->apply_cropping = 0;  // the coded picture, as the console's
  if (avcodec_open2(d->ctx, codec, nullptr) < 0) recomp::FatalF("libSceVideodec2: no H.264 decoder");
  d->pkt = av_packet_alloc();
  d->frame = av_frame_alloc();
  q->second++;
  g_decoders[cpu] = d;
  recomp::Wr64(out, cpu);
  return 0;
}

// int sceVideodec2DeleteDecoder(SceVideodec2Decoder decoder)
ORBIS_HLE("libSceVideodec2", sceVideodec2DeleteDecoder, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  Decoder* d = Find(Arg(c, 0));
  if (!d) return kErrBadDecoder;
  for (auto& [f, pic] : d->ready) av_frame_free(&f);
  av_frame_free(&d->frame);
  av_packet_free(&d->pkt);
  avcodec_free_context(&d->ctx);
  g_queues[d->queue]--;
  g_decoders.erase(Arg(c, 0));
  delete d;
  return 0;
}

// int sceVideodec2Decode(SceVideodec2Decoder decoder, const
// SceVideodec2InputData* in, SceVideodec2FrameBuffer* fb,
// SceVideodec2OutputInfo* out): one access unit in; from the delay-th on
// (Sizes), each gives out a picture, in output order, copied into fb.
// During a flush: kErrDecoder.
ORBIS_HLE("libSceVideodec2", sceVideodec2Decode, 4) {
  const recomp::gaddr in = Arg(c, 1), fb = Arg(c, 2), out = Arg(c, 3);
  std::lock_guard<std::mutex> l(g_mu);
  Decoder* d = Find(Arg(c, 0));
  if (!d) return kErrBadDecoder;
  if (!in || !fb || !out) return kErrBadPointer;
  if (recomp::Rd64(in) != 48) return kErrStructSize;
  if (uint32_t e = CheckOutput(fb, out)) return e;
  const recomp::gaddr au = recomp::Rd64(in + 8);
  const uint64_t size = recomp::Rd64(in + 16);
  if (!au) return kErrNoAu;
  if (!size) return kErrNoAuSize;
  recomp::Wr16(out + 8, 0);
  if (d->flushing) return kErrDecoder;
  ReadSps(*d, recomp::GPtr(au), size);
  d->aus[d->seq] = {recomp::Rd64(in + 24), recomp::Rd64(in + 32), recomp::Rd64(in + 40)};
  if (av_new_packet(d->pkt, int(size)) < 0) recomp::FatalF("libSceVideodec2: out of memory");
  std::memcpy(d->pkt->data, recomp::GPtr(au), size);
  d->pkt->pts = d->seq++;
  if (avcodec_send_packet(d->ctx, d->pkt) < 0) recomp::FatalF("libSceVideodec2: decoding failed");
  av_packet_unref(d->pkt);
  Receive(*d);
  // The pipeline holds delay - 1 access units; the oldest one's picture
  // is due once it is full.
  const bool due = d->count + 1 >= d->sizes->delay;
  d->count++;
  return due ? Output(*d, 1, fb, out) : 0;
}

// int sceVideodec2Flush(SceVideodec2Decoder decoder, SceVideodec2FrameBuffer*
// fb, SceVideodec2OutputInfo* out): the first call decodes what the
// pipeline holds; each gives out one picture, until none is left (isValid
// 0), which ends the flush.
ORBIS_HLE("libSceVideodec2", sceVideodec2Flush, 3) {
  const recomp::gaddr fb = Arg(c, 1), out = Arg(c, 2);
  std::lock_guard<std::mutex> l(g_mu);
  Decoder* d = Find(Arg(c, 0));
  if (!d) return kErrBadDecoder;
  if (!fb || !out) return kErrBadPointer;
  if (uint32_t e = CheckOutput(fb, out)) return e;
  if (!d->flushing) {
    if (!d->count && !d->pending) {
      recomp::GPtr(out)[8] = 0;
      return 0;
    }
    d->flushing = true;
  }
  uint32_t pictures = 0;
  if (d->count) {
    pictures = uint32_t(std::min<uint64_t>(d->count, d->sizes->delay - 1));
    if (avcodec_send_packet(d->ctx, nullptr) < 0) recomp::FatalF("libSceVideodec2: decoding failed");
    Receive(*d);
    avcodec_flush_buffers(d->ctx);
    d->count = 0;
  }
  const uint32_t e = Output(*d, pictures, fb, out);
  if (!d->count && !d->pending) d->flushing = false;
  return e;
}

// int sceVideodec2Reset(SceVideodec2Decoder decoder): drops what the
// pipeline holds and what was not handed out.
ORBIS_HLE("libSceVideodec2", sceVideodec2Reset, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  Decoder* d = Find(Arg(c, 0));
  if (!d) return kErrBadDecoder;
  avcodec_flush_buffers(d->ctx);
  for (auto& [f, pic] : d->ready) av_frame_free(&f);
  d->ready.clear();
  d->aus.clear();
  d->count = d->pending = 0;
  d->flushing = false;
  return 0;
}

// int sceVideodec2GetPictureInfo(const SceVideodec2OutputInfo* out, void*
// p0, void* p1): the picture's SceVideodec2AvcPictureInfo (104 bytes;
// the 120-byte one is not measured: fatal) into p0 (p1: a second field,
// never here). Its fields beside the access unit's pts, dts and attached
// data are its SPS's: profile, level, size in macroblocks less one, frame
// coding and cropping (offsets doubled), and its VUI as the measured
// streams have it (video format 5, colour 2/2/2, timing, bitstream
// restriction and max_dec_frame_buffering). Bytes 12-15, 40, 43, 54, 55,
// 86, 87 and 103 are left as they are.
ORBIS_HLE("libSceVideodec2", sceVideodec2GetPictureInfo, 3) {
  const recomp::gaddr out = Arg(c, 0), p0 = Arg(c, 1), p1 = Arg(c, 2);
  if (!p0) return kErrBadPointer;
  recomp::Wr32(p0 + 8, 0);
  if (p1) recomp::Wr32(p1 + 8, 0);
  if (!out) return kErrBadPointer;
  if ((recomp::Rd64(out) | 8) != 0x38) return kErrStructSize;
  const recomp::gaddr fb = recomp::Rd64(out + 32);
  if (fb & 0xFF) return kErrFrameAlign;
  if (!fb) return kErrNoFrameBuffer;
  const uint8_t count = recomp::GPtr(out)[10];
  if (uint8_t(count - 1) >= 2) return kErrPictureCount;
  if (recomp::Rd64(out + 40) <= uint64_t(count) << 10) return kErrFrameSize;
  if (count != 1) recomp::FatalF("libSceVideodec2: a picture of two fields not implemented yet");
  const uint64_t size = recomp::Rd64(p0);
  if (size != 0x68 && size != 0x78) return kErrStructSize;
  if (size == 0x78) recomp::FatalF("libSceVideodec2: the 120-byte AVC picture info not implemented yet");
  if (recomp::Rd32(out + 12) != kAvc) recomp::FatalF("libSceVideodec2: picture info of codec %u not implemented yet",
                                                     recomp::Rd32(out + 12));
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_pictures.find(fb);
  if (it == g_pictures.end())
    recomp::FatalF("sceVideodec2GetPictureInfo: frame buffer %llX holds no picture of a decoder's: not "
                   "implemented yet", (unsigned long long)fb);
  const Picture& p = it->second;
  const Sps& s = p.sps;
  uint8_t* o = recomp::GPtr(p0);
  o[8] = 1;
  recomp::Wr64(p0 + 16, p.au.pts);
  recomp::Wr64(p0 + 24, p.au.dts);
  recomp::Wr64(p0 + 32, p.au.attached);
  o[41] = uint8_t(s.profile);
  o[42] = uint8_t(s.level);
  recomp::Wr32(p0 + 44, s.width_mbs1);
  recomp::Wr32(p0 + 48, s.height_units1);
  o[52] = s.frame_mbs_only;
  o[53] = s.crop;
  for (int i = 0; i < 4; i++) recomp::Wr32(p0 + 56 + 4 * i, s.crop ? 2 * s.crop_lrtb[i] : 0);
  o[72] = o[73] = 0;
  recomp::Wr16(p0 + 74, 0);
  recomp::Wr16(p0 + 76, 0);
  o[78] = 0;
  o[79] = 5;
  o[80] = o[81] = 0;
  o[82] = o[83] = o[84] = 2;
  o[85] = 1;
  recomp::Wr32(p0 + 88, s.units_in_tick);
  recomp::Wr32(p0 + 92, s.time_scale);
  o[96] = 0;
  o[97] = 1;
  o[98] = uint8_t(s.max_dec_frame_buffering);
  o[99] = o[100] = o[101] = o[102] = 0;
  return 0;
}
