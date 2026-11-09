// The Windows Media Format SDK's synchronous reader (wmvcore.dll
// WMCreateSyncReader) on FFmpeg, as far as games use it to play the
// player's own music files: one audio stream, pulled as PCM.
//
// A reader opens a file (Open), offers its profile (QueryInterface for
// IWMProfile: one stream, number 1, audio) and its header attributes
// (IWMHeaderInfo3: "Duration", a QWORD in 100 ns), and its output 0's
// format (GetOutputProps -> IWMOutputMediaProps::GetMediaType): PCM,
// 16-bit, two channels, the file's sample rate. GetNextSample hands out
// the decoded audio in samples (INSSBuffer) of at most kMaxFrames frames,
// with their time and duration in 100 ns from the start of the file, and
// NS_E_NO_MORE_SAMPLES after the last one; SetRange(start, duration)
// seeks. Mono files come out as two equal channels (callers that accept
// only stereo PCM still play them); files with more channels keep the
// first two. Every reader has its own demuxer and decoder, so one file
// can be open in several at once.
//
// What a file can be is what the FFmpeg the runtime links demuxes and
// decodes (a minimal build: MP3 only). Methods not listed here are
// unimplemented and fail by name (com.h).
//
// <ID>_WMV_LOG=1 logs each Open, SetRange and the samples handed out.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using guest::GUID;

constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IWMSyncReader = {0x9397F121, 0x7705, 0x4DC9, {0xB0, 0x49, 0x98, 0xB6, 0x98, 0x18, 0x84, 0x14}};
constexpr GUID kIID_IWMProfile = {0x96406BDB, 0x2B2B, 0x11D3, {0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF}};
constexpr GUID kIID_IWMStreamConfig = {0x96406BDC, 0x2B2B, 0x11D3, {0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF}};
constexpr GUID kIID_IWMMediaProps = {0x96406BCE, 0x2B2B, 0x11D3, {0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF}};
constexpr GUID kIID_IWMOutputMediaProps = {0x96406BD7, 0x2B2B, 0x11D3, {0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF}};
constexpr GUID kIID_IWMHeaderInfo = {0x96406BDA, 0x2B2B, 0x11D3, {0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF}};
constexpr GUID kIID_IWMHeaderInfo2 = {0x15CF9781, 0x454E, 0x482E, {0xB3, 0x93, 0x85, 0xFA, 0xE4, 0x87, 0xA8, 0x10}};
constexpr GUID kIID_IWMHeaderInfo3 = {0x15CC68E3, 0x27CC, 0x4ECD, {0xB2, 0x22, 0x3F, 0x5D, 0x02, 0xD8, 0x0B, 0xD5}};
constexpr GUID kIID_INSSBuffer = {0xE1CD3524, 0x03D7, 0x11D2, {0x9E, 0xED, 0x00, 0x60, 0x97, 0xD2, 0xD7, 0xCF}};

constexpr GUID kMEDIATYPE_Audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMEDIASUBTYPE_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kFORMAT_WaveFormatEx = {0x05589F81, 0xC356, 0x11CE, {0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A}};

constexpr uint32_t S_OK = 0, E_INVALIDARG = 0x80070057, E_POINTER = 0x80004003,
                   E_NOINTERFACE = 0x80004002, NS_E_FILE_NOT_FOUND = 0xC00D001A,
                   NS_E_UNRECOGNIZED_STREAM_TYPE = 0xC00D0026,
                   NS_E_INVALID_REQUEST = 0xC00D002B, NS_E_NO_MORE_SAMPLES = 0xC00D0BCF,
                   ASF_E_BUFFERTOOSMALL = 0xC00D07D1;
constexpr uint32_t WMT_TYPE_QWORD = 4;
constexpr uint32_t WM_SF_CLEANPOINT = 1, WM_SF_DISCONTINUITY = 2;
constexpr uint16_t kStreamNumber = 1;  // the audio stream; its output is 0
constexpr int kChannels = 2, kBlockAlign = 4;  // s16 stereo
constexpr int kMaxFrames = 4096;               // per sample: 16 KiB
// WM_MEDIA_TYPE (0x48 bytes on 32-bit Windows), then its WAVEFORMATEX.
constexpr uint32_t kMediaTypeSize = 0x48, kWaveFormatSize = 18;
constexpr char16_t kDuration[] = u"Duration";

bool LogOn() {
  static const bool on = [] {
    const char* e = Env("WMV_LOG");
    return e && *e && *e != '0';
  }();
  return on;
}

void PutGuid(gaddr va, const GUID& g) {
  static_assert(sizeof(GUID) == 16);
  std::memcpy(GPtr(va), &g, sizeof g);
}

// 100 ns units of `frames` at `rate`.
int64_t FramesToTime(int64_t frames, int rate) { return frames * 10000000 / rate; }

// One audio stream of a file, decoded to interleaved s16 stereo.
class Decoder {
 public:
  ~Decoder() {
    avcodec_free_context(&ctx_);
    avformat_close_input(&fmt_);
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
  }

  bool Open(const std::string& path, std::string* err) {
    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
      *err = "not a format this build reads";
      return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
      *err = "no stream information";
      return false;
    }
    const AVCodec* codec = nullptr;
    stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream_ < 0 || !codec) {
      *err = stream_ == AVERROR_DECODER_NOT_FOUND ? "no decoder for its audio" : "no audio stream";
      return false;
    }
    st_ = fmt_->streams[stream_];
    ctx_ = avcodec_alloc_context3(codec);
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!ctx_ || !pkt_ || !frame_ || avcodec_parameters_to_context(ctx_, st_->codecpar) < 0 ||
        avcodec_open2(ctx_, codec, nullptr) < 0) {
      *err = "the decoder does not start";
      return false;
    }
    rate_ = ctx_->sample_rate;
    channels_ = ctx_->ch_layout.nb_channels;
    if (rate_ <= 0 || channels_ <= 0) {
      *err = "no sample rate or channels";
      return false;
    }
    duration_ = FindDuration();
    return true;
  }

  int rate() const { return rate_; }
  int channels() const { return channels_; }
  int64_t duration() const { return duration_; }  // 100 ns
  const char* codec_name() const { return ctx_->codec->name; }

  // Delivers from `time` (100 ns) on.
  void Seek(int64_t time) {
    const int64_t target = time * rate_ / 10000000;
    pending_.clear();
    pending_at_ = 0;
    if (!started_ && target == 0) return;  // nothing read yet
    started_ = true;
    const int64_t ts = av_rescale_q(time, AVRational{1, 10000000}, st_->time_base) +
                       (st_->start_time != AV_NOPTS_VALUE ? st_->start_time : 0);
    if (avformat_seek_file(fmt_, stream_, INT64_MIN, ts, ts, 0) < 0 &&
        av_seek_frame(fmt_, stream_, st_->start_time != AV_NOPTS_VALUE ? st_->start_time : 0,
                      AVSEEK_FLAG_BACKWARD) < 0)
      WarnF("wmvcore: cannot seek to %.3f s\n", double(time) / 1e7);
    avcodec_flush_buffers(ctx_);
    draining_ = done_ = false;
    errors_ = 0;
    target_ = target;
    resync_ = true;
  }

  // The next block of at most kMaxFrames frames: false at the end. *at is
  // its first frame's position (frames from the start).
  bool Next(std::vector<int16_t>* out, int64_t* at) {
    started_ = true;
    if (pending_at_ * kChannels >= int64_t(pending_.size()) && !Decode()) return false;
    const int64_t avail = int64_t(pending_.size()) / kChannels - pending_at_;
    const int64_t n = std::min<int64_t>(avail, kMaxFrames);
    out->assign(pending_.begin() + pending_at_ * kChannels,
                pending_.begin() + (pending_at_ + n) * kChannels);
    *at = pending_pos_ + pending_at_;
    pending_at_ += n;
    return true;
  }

 private:
  // Decodes the next frame into pending_ (pending_pos_ its position).
  bool Decode() {
    pending_.clear();
    pending_at_ = 0;
    while (!done_) {
      int r = avcodec_receive_frame(ctx_, frame_);
      if (r == 0) {
        errors_ = 0;
        bool keep = Take();
        av_frame_unref(frame_);
        if (keep) return true;
        continue;
      }
      // A damaged frame is an error for that frame only (the decoder has
      // dropped its packet); many in a row end the stream.
      if (r != AVERROR(EAGAIN) && r != AVERROR_EOF && ++errors_ < 1000) continue;
      if (r != AVERROR(EAGAIN) || draining_) {
        done_ = true;
        break;
      }
      r = av_read_frame(fmt_, pkt_);
      if (r < 0) {
        draining_ = true;
        avcodec_send_packet(ctx_, nullptr);
        continue;
      }
      // (An error here is the packet's own: it is dropped.)
      if (pkt_->stream_index == stream_) avcodec_send_packet(ctx_, pkt_);
      av_packet_unref(pkt_);
    }
    return false;
  }

  // The decoded frame into pending_; false if it is all before the seek
  // target.
  bool Take() {
    const int n = frame_->nb_samples;
    if (n <= 0) return false;
    int64_t at = next_pos_;
    if (resync_) {
      const int64_t pts = frame_->best_effort_timestamp;
      if (pts != AV_NOPTS_VALUE) {
        const int64_t rel = pts - (st_->start_time != AV_NOPTS_VALUE ? st_->start_time : 0);
        at = av_rescale_q(rel, st_->time_base, AVRational{1, rate_});
      } else {
        at = target_;
      }
      resync_ = false;
    }
    int skip = 0;
    if (target_ >= 0) {
      if (at + n <= target_) {
        next_pos_ = at + n;
        resync_ = false;
        return false;
      }
      if (at < target_) skip = int(target_ - at);
      target_ = -1;
    }
    next_pos_ = at + n;
    pending_pos_ = at + skip;
    pending_.resize(size_t(n - skip) * kChannels);
    const int ch = std::max(frame_->ch_layout.nb_channels, 1);
    if (frame_->sample_rate && frame_->sample_rate != rate_ && !rate_warned_) {
      WarnF("wmvcore: the sample rate changes in the file (%d -> %d Hz): not resampled\n",
            rate_, frame_->sample_rate);
      rate_warned_ = true;
    }
    for (int i = skip; i < n; i++) {
      int16_t* o = &pending_[size_t(i - skip) * kChannels];
      o[0] = SampleAt(i, 0, ch);
      o[1] = SampleAt(i, ch > 1 ? 1 : 0, ch);
    }
    return true;
  }

  // Sample i of channel c as s16.
  int16_t SampleAt(int i, int c, int ch) const {
    const auto fmt = AVSampleFormat(frame_->format);
    const bool planar = av_sample_fmt_is_planar(fmt);
    const uint8_t* plane = frame_->extended_data[planar ? c : 0];
    const size_t idx = planar ? size_t(i) : size_t(i) * ch + c;
    auto from_float = [](double v) {
      v = std::nearbyint(v * 32768.0);
      return int16_t(std::clamp(v, -32768.0, 32767.0));
    };
    switch (av_get_packed_sample_fmt(fmt)) {
      case AV_SAMPLE_FMT_U8: return int16_t((int(plane[idx]) - 128) << 8);
      case AV_SAMPLE_FMT_S16: return reinterpret_cast<const int16_t*>(plane)[idx];
      case AV_SAMPLE_FMT_S32: return int16_t(reinterpret_cast<const int32_t*>(plane)[idx] >> 16);
      case AV_SAMPLE_FMT_S64: return int16_t(reinterpret_cast<const int64_t*>(plane)[idx] >> 48);
      case AV_SAMPLE_FMT_FLT: return from_float(reinterpret_cast<const float*>(plane)[idx]);
      case AV_SAMPLE_FMT_DBL: return from_float(reinterpret_cast<const double*>(plane)[idx]);
      default: return 0;
    }
  }

  // The container's duration; else, or when ffmpeg only estimates it
  // from the bitrate (wrong for a variable bitrate file without a header
  // that gives its length), the packets' summed up (then back to the
  // start).
  int64_t FindDuration() {
    const bool estimate = fmt_->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE;
    if (!estimate && fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0)
      return av_rescale(fmt_->duration, 10000000, AV_TIME_BASE);
    if (!estimate && st_->duration != AV_NOPTS_VALUE && st_->duration > 0)
      return av_rescale_q(st_->duration, st_->time_base, AVRational{1, 10000000});
    int64_t sum = 0;
    while (av_read_frame(fmt_, pkt_) >= 0) {
      if (pkt_->stream_index == stream_ && pkt_->duration > 0) sum += pkt_->duration;
      av_packet_unref(pkt_);
    }
    av_seek_frame(fmt_, stream_, st_->start_time != AV_NOPTS_VALUE ? st_->start_time : 0,
                  AVSEEK_FLAG_BACKWARD);
    return av_rescale_q(sum, st_->time_base, AVRational{1, 10000000});
  }

  AVFormatContext* fmt_ = nullptr;
  AVStream* st_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  int stream_ = -1, rate_ = 0, channels_ = 0;
  int errors_ = 0;  // decode errors in a row
  int64_t duration_ = 0;
  bool started_ = false, draining_ = false, done_ = false, resync_ = false;
  bool rate_warned_ = false;
  int64_t target_ = -1;   // after a seek: frames before it are dropped
  int64_t next_pos_ = 0;  // position of the next decoded frame
  std::vector<int16_t> pending_;  // decoded, not handed out yet
  int64_t pending_pos_ = 0, pending_at_ = 0;
};

struct Reader final : com::Tagged<com::TagOf("wmvcore.SyncReader")> {
  std::mutex mu;
  std::unique_ptr<Decoder> dec;
  std::string name;  // for the log
  bool selected = true;
  bool discontinuity = true;  // the next sample is the first of a range
  int64_t end = -1;           // range end (100 ns), -1: the end of the file
  uint64_t samples = 0;
};
struct Profile final : com::Tagged<com::TagOf("wmvcore.Profile")> {
  std::shared_ptr<Reader> reader;
};
struct StreamConfig final : com::Tagged<com::TagOf("wmvcore.StreamConfig")> {};
struct OutputProps final : com::Tagged<com::TagOf("wmvcore.OutputProps")> {
  int rate = 0;
};
struct HeaderInfo final : com::Tagged<com::TagOf("wmvcore.HeaderInfo")> {
  std::shared_ptr<Reader> reader;
};
struct Buffer final : com::Tagged<com::TagOf("wmvcore.Buffer")> {
  gaddr data = 0;
  uint32_t length = 0;
  ~Buffer() override {
    if (data) HeapFreeProcess(data);
  }
};

uint32_t ReaderVtable() {
  return com::Vtable(
      "wmvcore.dll", "IWMSyncReader",
      {"QueryInterface", "AddRef", "Release", "Open", "Close", "SetRange",
       "SetRangeByFrame", "GetNextSample", "SetStreamsSelected", "GetStreamSelected",
       "SetReadStreamSamples", "GetReadStreamSamples", "GetOutputSetting",
       "SetOutputSetting", "GetOutputCount", "GetOutputProps", "SetOutputProps",
       "GetOutputFormatCount", "GetOutputFormat", "GetOutputNumberForStream",
       "GetStreamNumberForOutput", "GetMaxOutputSampleSize", "GetMaxStreamSampleSize",
       "OpenStream"});
}

uint32_t ProfileVtable() {
  return com::Vtable(
      "wmvcore.dll", "IWMProfile",
      {"QueryInterface", "AddRef", "Release", "GetVersion", "GetName", "SetName",
       "GetDescription", "SetDescription", "GetStreamCount", "GetStream",
       "GetStreamByNumber", "RemoveStream", "RemoveStreamByNumber", "AddStream",
       "ReconfigStream", "CreateNewStream", "GetMutualExclusionCount",
       "GetMutualExclusion", "RemoveMutualExclusion", "AddMutualExclusion",
       "CreateNewMutualExclusion"});
}

uint32_t StreamConfigVtable() {
  return com::Vtable(
      "wmvcore.dll", "IWMStreamConfig",
      {"QueryInterface", "AddRef", "Release", "GetStreamType", "GetStreamNumber",
       "SetStreamNumber", "GetStreamName", "SetStreamName", "GetConnectionName",
       "SetConnectionName", "GetBitrate", "SetBitrate", "GetBufferWindow",
       "SetBufferWindow"});
}

uint32_t OutputPropsVtable() {
  return com::Vtable("wmvcore.dll", "IWMOutputMediaProps",
                     {"QueryInterface", "AddRef", "Release", "GetType", "GetMediaType",
                      "SetMediaType", "GetStreamGroupName", "GetConnectionName"});
}

uint32_t HeaderInfoVtable() {
  return com::Vtable(
      "wmvcore.dll", "IWMHeaderInfo3",
      {"QueryInterface", "AddRef", "Release", "GetAttributeCount", "GetAttributeByIndex",
       "GetAttributeByName", "SetAttribute", "GetMarkerCount", "GetMarker", "AddMarker",
       "RemoveMarker", "GetScriptCount", "GetScript", "AddScript", "RemoveScript",
       "GetCodecInfoCount", "GetCodecInfo", "GetAttributeCountEx", "GetAttributeIndices",
       "GetAttributeByIndexEx", "ModifyAttribute", "AddAttribute", "DeleteAttribute",
       "AddCodecInfo"});
}

uint32_t BufferVtable() {
  return com::Vtable("wmvcore.dll", "INSSBuffer",
                     {"QueryInterface", "AddRef", "Release", "GetLength", "SetLength",
                      "GetMaxLength", "GetBuffer", "GetBufferAndLength"});
}

template <typename T>
T* Self(Ctx& c, const char* iface) {
  T* o = com::As<T>(Arg(c, 0));
  if (!o) FatalF("%s call on unknown object %08X", iface, uint32_t(Arg(c, 0)));
  return o;
}

// The host object behind `this` as an owning pointer (for objects that
// keep their reader alive).
template <typename T>
std::shared_ptr<T> Shared(Ctx& c, const char* iface) {
  Self<T>(c, iface);
  return std::static_pointer_cast<T>(com::Lookup(Arg(c, 0)));
}

// QueryInterface answered with the object itself for any of `iids` (and
// IUnknown).
uint32_t QueryInterfaceFor(Ctx& c, std::initializer_list<const GUID*> iids) {
  uint32_t out = Arg(c, 2);
  if (!out) return E_POINTER;
  bool ok = com::GuidEq(Arg(c, 1), kIID_IUnknown);
  for (const GUID* g : iids) ok = ok || com::GuidEq(Arg(c, 1), *g);
  if (!ok) {
    Wr32(out, 0);
    return E_NOINTERFACE;
  }
  com::AddRef(Arg(c, 0));
  Wr32(out, Arg(c, 0));
  return S_OK;
}

void WarnNoInterface(const char* iface, uint32_t iid) {
  GUID g = com::ReadGuid(iid);
  WarnF("wmvcore: %s::QueryInterface({%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}): "
        "not available\n",
        iface, g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// (pUnkCert, dwRights, ppSyncReader)
HLE("wmvcore.dll", WMCreateSyncReader, 3) {
  uint32_t out = Arg(c, 2);
  if (!out) return E_POINTER;
  Wr32(out, com::Publish(std::make_shared<Reader>(), ReaderVtable()));
  return S_OK;
}

// ---- IWMSyncReader ----------------------------------------------------------

// IWMProfile and IWMHeaderInfo(2, 3) are objects of their own that keep
// the reader alive.
HLE_NAMED("wmvcore.dll", "IWMSyncReader::QueryInterface", Rd_QueryInterface, 3) {
  uint32_t iid = Arg(c, 1), out = Arg(c, 2);
  if (!out) return E_POINTER;
  if (com::GuidEq(iid, kIID_IWMProfile)) {
    auto p = std::make_shared<Profile>();
    p->reader = Shared<Reader>(c, "IWMSyncReader");
    Wr32(out, com::Publish(p, ProfileVtable()));
    return S_OK;
  }
  if (com::GuidEq(iid, kIID_IWMHeaderInfo) || com::GuidEq(iid, kIID_IWMHeaderInfo2) ||
      com::GuidEq(iid, kIID_IWMHeaderInfo3)) {
    auto h = std::make_shared<HeaderInfo>();
    h->reader = Shared<Reader>(c, "IWMSyncReader");
    Wr32(out, com::Publish(h, HeaderInfoVtable()));
    return S_OK;
  }
  uint32_t r = QueryInterfaceFor(c, {&kIID_IWMSyncReader});
  if (r != S_OK) WarnNoInterface("IWMSyncReader", iid);
  return r;
}
HLE_NAMED("wmvcore.dll", "IWMSyncReader::AddRef", Rd_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wmvcore.dll", "IWMSyncReader::Release", Rd_Release, 1) { return com::Release(Arg(c, 0)); }

// (pwszFilename)
HLE_NAMED("wmvcore.dll", "IWMSyncReader::Open", Rd_Open, 2) {
  Reader* r = Self<Reader>(c, "IWMSyncReader");
  if (!Arg(c, 1)) return E_INVALIDARG;
  std::lock_guard<std::mutex> l(r->mu);
  if (r->dec) return NS_E_INVALID_REQUEST;
  std::u16string wide = GWStr(Arg(c, 1));
  for (char16_t ch : wide) {
    bool ok;
    win32::UnicodeToCp1252(ch, &ok);
    if (!ok) {
      WarnF("wmvcore: Open: a name with a character outside Windows-1252 (U+%04X)\n",
            unsigned(ch));
      return NS_E_FILE_NOT_FOUND;
    }
  }
  std::string guest = NarrowPath(wide), host = HostPath(guest);
  struct stat st;
  if (stat(host.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    WarnF("wmvcore: Open(%s): no such file\n", guest.c_str());
    return NS_E_FILE_NOT_FOUND;
  }
  auto dec = std::make_unique<Decoder>();
  std::string err;
  if (!dec->Open(host, &err)) {
    WarnF("wmvcore: Open(%s): %s\n", guest.c_str(), err.c_str());
    return NS_E_UNRECOGNIZED_STREAM_TYPE;
  }
  if (LogOn())
    std::fprintf(stderr, "wmvcore: reader %08X: Open(%s): %s, %d Hz, %d channel(s), %.3f s\n",
                 r->guest, guest.c_str(), dec->codec_name(), dec->rate(), dec->channels(),
                 double(dec->duration()) / 1e7);
  r->dec = std::move(dec);
  r->name = guest;
  r->selected = true;
  r->discontinuity = true;
  r->end = -1;
  r->samples = 0;
  return S_OK;
}

HLE_NAMED("wmvcore.dll", "IWMSyncReader::Close", Rd_Close, 1) {
  Reader* r = Self<Reader>(c, "IWMSyncReader");
  std::lock_guard<std::mutex> l(r->mu);
  if (!r->dec) return NS_E_INVALID_REQUEST;
  if (LogOn())
    std::fprintf(stderr, "wmvcore: reader %08X: Close after %llu sample(s)\n", r->guest,
                 (unsigned long long)r->samples);
  r->dec.reset();
  return S_OK;
}

// (QWORD cnsStartTime, LONGLONG cnsDuration): 0 = to the end.
HLE_NAMED("wmvcore.dll", "IWMSyncReader::SetRange", Rd_SetRange, 5) {
  Reader* r = Self<Reader>(c, "IWMSyncReader");
  const int64_t start = int64_t(uint64_t(Arg(c, 1)) | uint64_t(Arg(c, 2)) << 32);
  const int64_t length = int64_t(uint64_t(Arg(c, 3)) | uint64_t(Arg(c, 4)) << 32);
  std::lock_guard<std::mutex> l(r->mu);
  if (!r->dec) return NS_E_INVALID_REQUEST;
  if (start < 0) return E_INVALIDARG;
  if (LogOn())
    std::fprintf(stderr, "wmvcore: reader %08X: SetRange(%.3f s, %.3f s)\n", r->guest,
                 double(start) / 1e7, double(length) / 1e7);
  r->dec->Seek(start);
  r->end = length > 0 ? start + length : -1;
  r->discontinuity = true;
  return S_OK;
}

// (wStreamNum, ppSample, pcnsSampleTime, pcnsDuration, pdwFlags,
// pdwOutputNum, pwStreamNum): stream 0 is any stream.
HLE_NAMED("wmvcore.dll", "IWMSyncReader::GetNextSample", Rd_GetNextSample, 8) {
  Reader* r = Self<Reader>(c, "IWMSyncReader");
  const uint16_t stream = uint16_t(Arg(c, 1));
  const uint32_t sample = Arg(c, 2), time = Arg(c, 3), duration = Arg(c, 4),
                 flags = Arg(c, 5), output = Arg(c, 6), stream_out = Arg(c, 7);
  if (!sample) return E_INVALIDARG;
  Wr32(sample, 0);
  std::lock_guard<std::mutex> l(r->mu);
  if (!r->dec) return NS_E_INVALID_REQUEST;
  if (stream != 0 && stream != kStreamNumber) return E_INVALIDARG;
  if (!r->selected) return NS_E_INVALID_REQUEST;
  std::vector<int16_t> pcm;
  int64_t at = 0;
  const int rate = r->dec->rate();
  if (!r->dec->Next(&pcm, &at) || (r->end >= 0 && FramesToTime(at, rate) >= r->end)) {
    if (LogOn())
      std::fprintf(stderr, "wmvcore: reader %08X: no more samples after %llu\n", r->guest,
                   (unsigned long long)r->samples);
    return NS_E_NO_MORE_SAMPLES;
  }
  const int64_t frames = int64_t(pcm.size()) / kChannels;
  auto b = std::make_shared<Buffer>();
  b->length = uint32_t(pcm.size() * sizeof(int16_t));
  b->data = HeapAllocProcess(b->length, false);
  if (!b->data) FatalF("wmvcore: no guest memory for a sample of %u bytes", b->length);
  std::memcpy(GPtr(b->data), pcm.data(), b->length);
  const int64_t t0 = FramesToTime(at, rate), t1 = FramesToTime(at + frames, rate);
  Wr32(sample, com::Publish(b, BufferVtable()));
  if (time) Wr64(time, uint64_t(t0));
  if (duration) Wr64(duration, uint64_t(t1 - t0));
  if (flags) Wr32(flags, WM_SF_CLEANPOINT | (r->discontinuity ? WM_SF_DISCONTINUITY : 0));
  if (output) Wr32(output, 0);
  if (stream_out) Wr16(stream_out, kStreamNumber);
  r->discontinuity = false;
  r->samples++;
  if (LogOn() && (r->samples == 1 || r->samples % 500 == 0))
    std::fprintf(stderr, "wmvcore: reader %08X: sample %llu at %.3f s, %lld frames\n",
                 r->guest, (unsigned long long)r->samples, double(t0) / 1e7,
                 (long long)frames);
  return S_OK;
}

// (cStreamCount, pwStreamNumbers, pSelections): WMT_OFF 0, WMT_CLEANPOINT_ONLY
// 1, WMT_ON 2.
HLE_NAMED("wmvcore.dll", "IWMSyncReader::SetStreamsSelected", Rd_SetStreamsSelected, 4) {
  Reader* r = Self<Reader>(c, "IWMSyncReader");
  const uint16_t count = uint16_t(Arg(c, 1));
  const uint32_t numbers = Arg(c, 2), selections = Arg(c, 3);
  if (!count || !numbers || !selections) return E_INVALIDARG;
  std::lock_guard<std::mutex> l(r->mu);
  if (!r->dec) return NS_E_INVALID_REQUEST;
  for (uint16_t i = 0; i < count; i++)
    if (Rd16(numbers + 2 * i) != kStreamNumber) return NS_E_INVALID_REQUEST;
  for (uint16_t i = 0; i < count; i++) r->selected = Rd32(selections + 4 * i) != 0;
  return S_OK;
}

// (wStreamNum, fCompressed): decoded samples only.
HLE_NAMED("wmvcore.dll", "IWMSyncReader::SetReadStreamSamples", Rd_SetReadStreamSamples, 3) {
  Reader* r = Self<Reader>(c, "IWMSyncReader");
  std::lock_guard<std::mutex> l(r->mu);
  if (!r->dec) return NS_E_INVALID_REQUEST;
  if (uint16_t(Arg(c, 1)) != kStreamNumber) return NS_E_INVALID_REQUEST;
  if (Arg(c, 2)) FatalF("wmvcore: IWMSyncReader::SetReadStreamSamples: compressed samples are not supported");
  return S_OK;
}

// (wStreamNum, pdwOutputNum)
HLE_NAMED("wmvcore.dll", "IWMSyncReader::GetOutputNumberForStream", Rd_GetOutputNumberForStream, 3) {
  Reader* r = Self<Reader>(c, "IWMSyncReader");
  if (!Arg(c, 2)) return E_POINTER;
  std::lock_guard<std::mutex> l(r->mu);
  if (!r->dec) return NS_E_INVALID_REQUEST;
  if (uint16_t(Arg(c, 1)) != kStreamNumber) return E_INVALIDARG;
  Wr32(Arg(c, 2), 0);
  return S_OK;
}

// (dwOutputNum, ppOutput)
HLE_NAMED("wmvcore.dll", "IWMSyncReader::GetOutputProps", Rd_GetOutputProps, 3) {
  Reader* r = Self<Reader>(c, "IWMSyncReader");
  if (!Arg(c, 2)) return E_POINTER;
  std::lock_guard<std::mutex> l(r->mu);
  if (!r->dec) return NS_E_INVALID_REQUEST;
  if (Arg(c, 1) != 0) return E_INVALIDARG;
  auto p = std::make_shared<OutputProps>();
  p->rate = r->dec->rate();
  Wr32(Arg(c, 2), com::Publish(p, OutputPropsVtable()));
  return S_OK;
}

// ---- IWMProfile -------------------------------------------------------------

HLE_NAMED("wmvcore.dll", "IWMProfile::QueryInterface", Prof_QueryInterface, 3) {
  uint32_t r = QueryInterfaceFor(c, {&kIID_IWMProfile});
  if (r != S_OK) WarnNoInterface("IWMProfile", Arg(c, 1));
  return r;
}
HLE_NAMED("wmvcore.dll", "IWMProfile::AddRef", Prof_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wmvcore.dll", "IWMProfile::Release", Prof_Release, 1) { return com::Release(Arg(c, 0)); }

// (pcStreams)
HLE_NAMED("wmvcore.dll", "IWMProfile::GetStreamCount", Prof_GetStreamCount, 2) {
  Profile* p = Self<Profile>(c, "IWMProfile");
  if (!Arg(c, 1)) return E_POINTER;
  std::lock_guard<std::mutex> l(p->reader->mu);
  if (!p->reader->dec) return NS_E_INVALID_REQUEST;
  Wr32(Arg(c, 1), 1);
  return S_OK;
}

// (dwStreamIndex, ppConfig)
HLE_NAMED("wmvcore.dll", "IWMProfile::GetStream", Prof_GetStream, 3) {
  Profile* p = Self<Profile>(c, "IWMProfile");
  if (!Arg(c, 2)) return E_POINTER;
  std::lock_guard<std::mutex> l(p->reader->mu);
  if (!p->reader->dec) return NS_E_INVALID_REQUEST;
  if (Arg(c, 1) != 0) return E_INVALIDARG;
  Wr32(Arg(c, 2), com::Publish(std::make_shared<StreamConfig>(), StreamConfigVtable()));
  return S_OK;
}

// ---- IWMStreamConfig --------------------------------------------------------

HLE_NAMED("wmvcore.dll", "IWMStreamConfig::QueryInterface", Cfg_QueryInterface, 3) {
  uint32_t r = QueryInterfaceFor(c, {&kIID_IWMStreamConfig});
  if (r != S_OK) WarnNoInterface("IWMStreamConfig", Arg(c, 1));
  return r;
}
HLE_NAMED("wmvcore.dll", "IWMStreamConfig::AddRef", Cfg_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wmvcore.dll", "IWMStreamConfig::Release", Cfg_Release, 1) { return com::Release(Arg(c, 0)); }

// (pguidStreamType)
HLE_NAMED("wmvcore.dll", "IWMStreamConfig::GetStreamType", Cfg_GetStreamType, 2) {
  Self<StreamConfig>(c, "IWMStreamConfig");
  if (!Arg(c, 1)) return E_POINTER;
  PutGuid(Arg(c, 1), kMEDIATYPE_Audio);
  return S_OK;
}

// (pwStreamNum)
HLE_NAMED("wmvcore.dll", "IWMStreamConfig::GetStreamNumber", Cfg_GetStreamNumber, 2) {
  Self<StreamConfig>(c, "IWMStreamConfig");
  if (!Arg(c, 1)) return E_POINTER;
  Wr16(Arg(c, 1), kStreamNumber);
  return S_OK;
}

// ---- IWMOutputMediaProps ----------------------------------------------------

HLE_NAMED("wmvcore.dll", "IWMOutputMediaProps::QueryInterface", Out_QueryInterface, 3) {
  uint32_t r = QueryInterfaceFor(c, {&kIID_IWMOutputMediaProps, &kIID_IWMMediaProps});
  if (r != S_OK) WarnNoInterface("IWMOutputMediaProps", Arg(c, 1));
  return r;
}
HLE_NAMED("wmvcore.dll", "IWMOutputMediaProps::AddRef", Out_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wmvcore.dll", "IWMOutputMediaProps::Release", Out_Release, 1) { return com::Release(Arg(c, 0)); }

// (pType, pcbType): pType 0 asks for the size. WM_MEDIA_TYPE: majortype
// +0, subtype +0x10, bFixedSizeSamples +0x20, bTemporalCompression +0x24,
// lSampleSize +0x28, formattype +0x2C, pUnk +0x3C, cbFormat +0x40,
// pbFormat +0x44, then the WAVEFORMATEX pbFormat points at.
HLE_NAMED("wmvcore.dll", "IWMOutputMediaProps::GetMediaType", Out_GetMediaType, 3) {
  OutputProps* p = Self<OutputProps>(c, "IWMOutputMediaProps");
  const uint32_t mt = Arg(c, 1), size = Arg(c, 2);
  if (!size) return E_POINTER;
  constexpr uint32_t need = kMediaTypeSize + kWaveFormatSize;
  if (!mt) {
    Wr32(size, need);
    return S_OK;
  }
  if (Rd32(size) < need) {
    Wr32(size, need);
    return ASF_E_BUFFERTOOSMALL;
  }
  std::memset(GPtr(mt), 0, need);
  PutGuid(mt, kMEDIATYPE_Audio);
  PutGuid(mt + 0x10, kMEDIASUBTYPE_PCM);
  Wr32(mt + 0x20, 1);  // fixed size samples
  Wr32(mt + 0x28, kBlockAlign);
  PutGuid(mt + 0x2C, kFORMAT_WaveFormatEx);
  Wr32(mt + 0x40, kWaveFormatSize);
  const uint32_t wf = mt + kMediaTypeSize;
  Wr32(mt + 0x44, wf);
  Wr16(wf, 1);  // WAVE_FORMAT_PCM
  Wr16(wf + 2, kChannels);
  Wr32(wf + 4, uint32_t(p->rate));
  Wr32(wf + 8, uint32_t(p->rate) * kBlockAlign);
  Wr16(wf + 12, kBlockAlign);
  Wr16(wf + 14, 16);
  Wr16(wf + 16, 0);
  Wr32(size, need);
  return S_OK;
}

// ---- IWMHeaderInfo3 ---------------------------------------------------------
// One attribute: index 0, "Duration" (file level, stream 0), WMT_TYPE_QWORD
// in 100 ns.

HLE_NAMED("wmvcore.dll", "IWMHeaderInfo3::QueryInterface", Hdr_QueryInterface, 3) {
  uint32_t r = QueryInterfaceFor(c, {&kIID_IWMHeaderInfo3, &kIID_IWMHeaderInfo2, &kIID_IWMHeaderInfo});
  if (r != S_OK) WarnNoInterface("IWMHeaderInfo3", Arg(c, 1));
  return r;
}
HLE_NAMED("wmvcore.dll", "IWMHeaderInfo3::AddRef", Hdr_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wmvcore.dll", "IWMHeaderInfo3::Release", Hdr_Release, 1) { return com::Release(Arg(c, 0)); }

// (wStreamNum, pwszName, pwLangIndex, pwIndices, pwCount): pwIndices 0
// asks for the count.
HLE_NAMED("wmvcore.dll", "IWMHeaderInfo3::GetAttributeIndices", Hdr_GetAttributeIndices, 6) {
  HeaderInfo* h = Self<HeaderInfo>(c, "IWMHeaderInfo3");
  const uint16_t stream = uint16_t(Arg(c, 1));
  const uint32_t name = Arg(c, 2), indices = Arg(c, 4), count = Arg(c, 5);
  if (!name || !count) return E_INVALIDARG;
  {
    std::lock_guard<std::mutex> l(h->reader->mu);
    if (!h->reader->dec) return NS_E_INVALID_REQUEST;
  }
  const bool found = stream == 0 && GWStr(name) == kDuration;
  if (!found) {
    WarnF("wmvcore: attribute %s of stream %u not available\n",
          win32::Narrow(GWStr(name)).c_str(), unsigned(stream));
    Wr16(count, 0);
    return S_OK;
  }
  if (indices) {
    if (Rd16(count) < 1) {
      Wr16(count, 1);
      return ASF_E_BUFFERTOOSMALL;
    }
    Wr16(indices, 0);
  }
  Wr16(count, 1);
  return S_OK;
}

// (wStreamNum, wIndex, pwszName, pwNameLen, pType, pwLangIndex, pValue,
// pdwDataLength): pwszName 0 asks for the name's length (with its NUL),
// pValue 0 for the value's.
HLE_NAMED("wmvcore.dll", "IWMHeaderInfo3::GetAttributeByIndexEx", Hdr_GetAttributeByIndexEx, 9) {
  HeaderInfo* h = Self<HeaderInfo>(c, "IWMHeaderInfo3");
  const uint16_t stream = uint16_t(Arg(c, 1)), index = uint16_t(Arg(c, 2));
  const uint32_t name = Arg(c, 3), name_len = Arg(c, 4), type = Arg(c, 5), lang = Arg(c, 6),
                 value = Arg(c, 7), length = Arg(c, 8);
  int64_t duration;
  {
    std::lock_guard<std::mutex> l(h->reader->mu);
    if (!h->reader->dec) return NS_E_INVALID_REQUEST;
    duration = h->reader->dec->duration();
  }
  if (stream != 0 || index != 0 || !name_len || !length) return E_INVALIDARG;
  constexpr uint16_t chars = sizeof kDuration / sizeof kDuration[0];  // with the NUL
  if (name) {
    if (Rd16(name_len) < chars) {
      Wr16(name_len, chars);
      return ASF_E_BUFFERTOOSMALL;
    }
    std::memcpy(GPtr(name), kDuration, sizeof kDuration);
  }
  Wr16(name_len, chars);
  if (type) Wr32(type, WMT_TYPE_QWORD);
  if (lang) Wr16(lang, 0);
  if (value) {
    if (Rd32(length) < 8) {
      Wr32(length, 8);
      return ASF_E_BUFFERTOOSMALL;
    }
    Wr64(value, uint64_t(duration));
  }
  Wr32(length, 8);
  return S_OK;
}

// ---- INSSBuffer -------------------------------------------------------------

HLE_NAMED("wmvcore.dll", "INSSBuffer::QueryInterface", Buf_QueryInterface, 3) {
  uint32_t r = QueryInterfaceFor(c, {&kIID_INSSBuffer});
  if (r != S_OK) WarnNoInterface("INSSBuffer", Arg(c, 1));
  return r;
}
HLE_NAMED("wmvcore.dll", "INSSBuffer::AddRef", Buf_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("wmvcore.dll", "INSSBuffer::Release", Buf_Release, 1) { return com::Release(Arg(c, 0)); }

// (ppdwBuffer, pdwLength)
HLE_NAMED("wmvcore.dll", "INSSBuffer::GetBufferAndLength", Buf_GetBufferAndLength, 3) {
  Buffer* b = Self<Buffer>(c, "INSSBuffer");
  if (!Arg(c, 1) || !Arg(c, 2)) return E_INVALIDARG;
  Wr32(Arg(c, 1), b->data);
  Wr32(Arg(c, 2), b->length);
  return S_OK;
}

}  // namespace
}  // namespace recomp::hle
