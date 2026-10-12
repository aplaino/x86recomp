#include "runtime/audio/source.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace recomp::audio {

namespace {

uint32_t Le32(const uint8_t* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
}
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// ---- WAV -------------------------------------------------------------------

class WavSource : public Source {
 public:
  WavSource(Bytes data, const WavInfo& w) : data_(std::move(data)), w_(w) {
    channels_ = w.channels;
    rate_ = int(w.rate);
    frames_ = w.data_len / w.block_align;
  }
  int Read(float* out, int n) override {
    int got = int(std::min<int64_t>(n, frames_ - pos_));
    const uint8_t* p = data_->data() + w_.data_offset + pos_ * w_.block_align;
    int samples = got * channels_;
    if (w_.bits == 16) {
      for (int i = 0; i < samples; i++)
        out[i] = int16_t(Le16(p + 2 * i)) * (1.0f / 32768.0f);
    } else {
      for (int i = 0; i < samples; i++) out[i] = (p[i] - 128) * (1.0f / 128.0f);
    }
    pos_ += got;
    return got;
  }
  bool Seek(int64_t f) override {
    pos_ = std::clamp<int64_t>(f, 0, frames_);
    return true;
  }
  int64_t Length() const override { return frames_; }
  int bytes_per_frame() const override { return w_.block_align; }

 private:
  Bytes data_;
  WavInfo w_;
  int64_t frames_ = 0, pos_ = 0;
};

// ---- MP3 -------------------------------------------------------------------

class Mp3Source : public Source {
 public:
  explicit Mp3Source(Bytes data) : data_(std::move(data)) {}
  ~Mp3Source() override {
    av_parser_close(parser_);
    avcodec_free_context(&ctx_);
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
  }

  bool Init(std::string* err) {
    codec_ = avcodec_find_decoder(AV_CODEC_ID_MP3);
    if (!codec_) return *err = "no MP3 decoder in libavcodec", false;
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!Reset()) return *err = "cannot open the MP3 decoder", false;
    // Decode the first frame to learn the format.
    if (!Decode() || pending_.empty())
      return *err = "no decodable MPEG audio frames", false;
    return true;
  }

  int Read(float* out, int n) override {
    int done = 0;
    while (done < n) {
      size_t avail = (pending_.size() - pending_pos_) / channels_;
      if (!avail) {
        if (!Decode()) break;
        continue;
      }
      size_t take = std::min<size_t>(avail, n - done);
      std::memcpy(out + done * channels_, pending_.data() + pending_pos_,
                  take * channels_ * sizeof(float));
      pending_pos_ += take * channels_;
      done += int(take);
    }
    return done;
  }

  bool Seek(int64_t f) override {
    if (f != 0) return false;
    return Reset();
  }

 private:
  bool Reset() {
    av_parser_close(parser_);
    avcodec_free_context(&ctx_);
    parser_ = av_parser_init(AV_CODEC_ID_MP3);
    ctx_ = avcodec_alloc_context3(codec_);
    if (!parser_ || !ctx_ || avcodec_open2(ctx_, codec_, nullptr) < 0)
      return false;
    off_ = 0;
    flushed_ = false;
    pending_.clear();
    pending_pos_ = 0;
    return true;
  }

  // Decodes the next frame into pending_. False at the end of the data.
  bool Decode() {
    pending_.clear();
    pending_pos_ = 0;
    for (;;) {
      int r = avcodec_receive_frame(ctx_, frame_);
      if (r == 0) {
        Convert();
        av_frame_unref(frame_);
        if (!pending_.empty()) return true;
        continue;
      }
      if (r == AVERROR_EOF) return false;
      if (r != AVERROR(EAGAIN)) return false;
      if (flushed_) return false;
      // Feed the parser until it yields a packet.
      const uint8_t* in = data_->data() + off_;
      int left = int(data_->size() - off_);
      uint8_t* out = nullptr;
      int out_size = 0;
      int used = av_parser_parse2(parser_, ctx_, &out, &out_size, left ? in : nullptr,
                                  left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (used < 0) return false;
      off_ += used;
      if (out_size) {
        pkt_->data = out;
        pkt_->size = out_size;
        if (avcodec_send_packet(ctx_, pkt_) < 0) {
          // A corrupt frame: skip it like a hardware decoder would.
          continue;
        }
      } else if (!left) {
        avcodec_send_packet(ctx_, nullptr);  // drain
        flushed_ = true;
      }
    }
  }

  void Convert() {
    if (!channels_) {
      channels_ = std::min(frame_->ch_layout.nb_channels, 2);
      rate_ = frame_->sample_rate;
    }
    pending_.resize(size_t(frame_->nb_samples) * channels_);
    ConvertFrame(frame_, channels_, pending_.data());
  }

  Bytes data_;
  const AVCodec* codec_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  size_t off_ = 0;
  bool flushed_ = false;
  std::vector<float> pending_;
  size_t pending_pos_ = 0;
};

}  // namespace

uint32_t RiffSize(const uint8_t* p) {
  if (std::memcmp(p, "RIFF", 4) != 0) return 0;
  return Le32(p + 4) + 8;
}

bool ParseWav(const uint8_t* p, size_t len, WavInfo* w) {
  if (len < 12 || std::memcmp(p, "RIFF", 4) || std::memcmp(p + 8, "WAVE", 4))
    return false;
  bool have_fmt = false;
  size_t off = 12;
  while (off + 8 <= len) {
    uint32_t id = Le32(p + off), size = Le32(p + off + 4);
    size_t body = off + 8;
    if (id == 0x20746D66 && size >= 16 && body + 16 <= len) {  // "fmt "
      w->format = Le16(p + body);
      w->channels = Le16(p + body + 2);
      w->rate = Le32(p + body + 4);
      w->block_align = Le16(p + body + 12);
      w->bits = Le16(p + body + 14);
      have_fmt = true;
    } else if (id == 0x61746164) {  // "data"
      if (!have_fmt) return false;
      w->data_offset = uint32_t(body);
      // Some writers put a bogus size on the last chunk; clamp to the file.
      w->data_len = uint32_t(std::min<size_t>(size, len - body));
      return true;
    }
    off = body + size + (size & 1);
  }
  return false;
}

std::unique_ptr<Source> MakeWavSource(Bytes data, std::string* err) {
  WavInfo w;
  if (!ParseWav(data->data(), data->size(), &w)) {
    *err = "malformed WAV";
    return nullptr;
  }
  if (w.format != 1 || (w.bits != 8 && w.bits != 16) || w.channels < 1 ||
      w.channels > 2 || w.block_align != w.channels * w.bits / 8 || !w.rate) {
    *err = "unsupported WAV format " + std::to_string(w.format) + " (" +
           std::to_string(w.channels) + " ch, " + std::to_string(w.bits) + " bit)";
    return nullptr;
  }
  return std::make_unique<WavSource>(std::move(data), w);
}

std::unique_ptr<Source> MakeMp3Source(Bytes data, std::string* err) {
  auto s = std::make_unique<Mp3Source>(std::move(data));
  if (!s->Init(err)) return nullptr;
  return s;
}

void ConvertFrame(const AVFrame* f, int out_ch, float* out) {
  int ch = f->ch_layout.nb_channels;
  auto fmt = AVSampleFormat(f->format);
  bool planar = av_sample_fmt_is_planar(fmt);
  auto packed = av_get_packed_sample_fmt(fmt);
  for (int i = 0; i < f->nb_samples; i++)
    for (int c = 0; c < out_ch; c++) {
      int sc = std::min(c, ch - 1);
      float v = 0;
      if (packed == AV_SAMPLE_FMT_FLT)
        v = planar ? reinterpret_cast<const float*>(f->data[sc])[i]
                   : reinterpret_cast<const float*>(f->data[0])[i * ch + sc];
      else if (packed == AV_SAMPLE_FMT_S16)
        v = (planar ? reinterpret_cast<const int16_t*>(f->data[sc])[i]
                    : reinterpret_cast<const int16_t*>(f->data[0])[i * ch + sc]) /
            32768.0f;
      out[size_t(i) * out_ch + c] = v;
    }
}

std::unique_ptr<Source> MakeSource(Bytes data, std::string* err) {
  if (data->size() >= 12 && RiffSize(data->data()))
    return MakeWavSource(std::move(data), err);
  return MakeMp3Source(std::move(data), err);
}

// ---- QueueSource -------------------------------------------------------------

QueueSource::QueueSource(int channels, int rate) {
  channels_ = channels;
  rate_ = rate;
}

void QueueSource::Push(const float* frames, int n) {
  std::lock_guard<std::mutex> l(mu_);
  if (head_ > (1u << 16) && head_ * 2 > q_.size()) {
    q_.erase(q_.begin(), q_.begin() + head_);
    head_ = 0;
  }
  q_.insert(q_.end(), frames, frames + size_t(n) * channels_);
}

void QueueSource::Finish() {
  std::lock_guard<std::mutex> l(mu_);
  finished_ = true;
}

int QueueSource::Read(float* out, int n) {
  std::lock_guard<std::mutex> l(mu_);
  size_t avail = (q_.size() - head_) / channels_;
  size_t take = std::min<size_t>(avail, n);
  std::memcpy(out, q_.data() + head_, take * channels_ * sizeof(float));
  head_ += take * channels_;
  if (finished_) return int(take);
  // Underrun: keep the voice alive with silence.
  std::fill(out + take * channels_, out + size_t(n) * channels_, 0.0f);
  return n;
}

}  // namespace recomp::audio
