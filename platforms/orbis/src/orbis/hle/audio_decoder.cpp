#include "orbis/hle/audio_decoder.h"

#include <algorithm>
#include <cstring>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>

#include "libatrac9.h"
#include "structures.h"  // the decoded samples, before LibAtrac9 rounds them
}

#include "runtime/log.h"

namespace recomp::orbis {

namespace {

// A float planar decoder of `id`; extradata (owned by ffmpeg after this)
// may be null.
AVCodecContext* Open(AVCodecID id, uint8_t* extradata, int extradata_size, int block_align) {
  const AVCodec* codec = avcodec_find_decoder(id);
  if (!codec) FatalF("the host's ffmpeg has no %s decoder", avcodec_get_name(id));
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) FatalF("audio decoder: out of memory");
  ctx->extradata = extradata;
  ctx->extradata_size = extradata_size;
  ctx->block_align = block_align;
  ctx->request_sample_fmt = AV_SAMPLE_FMT_FLTP;
  if (avcodec_open2(ctx, codec, nullptr) < 0) avcodec_free_context(&ctx);
  return ctx;
}

// LibAtrac9's initialisation rewrites its shared tables (with the same
// values for a frame size): one at a time.
std::mutex g_at9_init;

}  // namespace

std::unique_ptr<AudioDecoder> AudioDecoder::Atrac9(uint32_t config) {
  std::unique_ptr<AudioDecoder> d(new AudioDecoder);
  for (int i = 0; i < 4; i++) d->at9_config_[i] = uint8_t(config >> (24 - 8 * i));
  d->Flush();
  if (!d->at9_) return nullptr;
  Atrac9CodecInfo info;
  Atrac9GetCodecInfo(d->at9_, &info);
  d->at9_channels_ = uint32_t(info.channels);
  d->at9_samples_ = uint32_t(info.frameSamples);
  d->at9_superframe_ = uint32_t(info.superframeSize);
  d->at9_frames_ = uint32_t(info.framesInSuperframe);
  d->pcm_.resize(size_t(d->at9_channels_) * d->at9_frames_ * d->at9_samples_);
  // Room for a frame's bit reader past the superframe on any input.
  d->scratch_.resize(d->at9_superframe_ + 0x2000);
  return d;
}

std::unique_ptr<AudioDecoder> AudioDecoder::Mp3() {
  AVCodecContext* ctx = Open(AV_CODEC_ID_MP3, nullptr, 0, 0);
  if (!ctx) FatalF("the host's ffmpeg MP3 decoder does not open");
  std::unique_ptr<AudioDecoder> d(new AudioDecoder);
  d->ctx_ = ctx;
  d->pkt_ = av_packet_alloc();
  d->frame_ = av_frame_alloc();
  if (!d->pkt_ || !d->frame_) FatalF("audio decoder: out of memory");
  return d;
}

AudioDecoder::~AudioDecoder() {
  if (at9_) Atrac9ReleaseHandle(at9_);
  av_frame_free(&frame_);
  av_packet_free(&pkt_);
  avcodec_free_context(&ctx_);
}

size_t AudioDecoder::DecodeFrame(const uint8_t* data, size_t size) {
  // LibAtrac9 reads without bounds: from a copy, zeros after the input.
  const size_t n = std::min<size_t>(size, at9_superframe_);
  std::memcpy(scratch_.data(), data, n);
  std::memset(scratch_.data() + n, 0, scratch_.size() - n);
  Atrac9Handle* h = static_cast<Atrac9Handle*>(at9_);
  const int index = h->Frame.IndexInSuperframe;
  short pcm[MAX_CHANNEL_COUNT * MAX_FRAME_SAMPLES];
  int used = 0;
  if (Atrac9Decode(at9_, scratch_.data(), pcm, &used) != 0 || used <= 0) return 0;
  // Samples in LibAtrac9's 16-bit scale, at the frame's place in the
  // superframe.
  const size_t span = size_t(at9_frames_) * at9_samples_;
  for (uint32_t c = 0; c < at9_channels_; c++) {
    const double* x = h->Frame.Channels[c]->Pcm;
    float* y = pcm_.data() + c * span + size_t(index) * at9_samples_;
    for (uint32_t i = 0; i < at9_samples_; i++) y[i] = float(x[i] / 32768.0);
  }
  at9_index_ = uint32_t(index);
  samples_ = at9_samples_;
  return size_t(used);
}

bool AudioDecoder::Decode(const uint8_t* data, size_t size) {
  if (at9_) {
    // A superframe: its frames one after the other, the last one taking
    // the rest.
    size_t at = 0;
    for (uint32_t f = 0; f < at9_frames_; f++) {
      const size_t used = DecodeFrame(data + at, size - at);
      if (used == 0 || used > size - at) return false;
      at += used;
    }
    at9_index_ = 0;
    samples_ = at9_frames_ * at9_samples_;
    return true;
  }
  if (av_new_packet(pkt_, int(size)) < 0) FatalF("audio decoder: out of memory");
  std::memcpy(pkt_->data, data, size);
  const int err = avcodec_send_packet(ctx_, pkt_);
  av_packet_unref(pkt_);
  return err >= 0 && avcodec_receive_frame(ctx_, frame_) >= 0 &&
         frame_->format == AV_SAMPLE_FMT_FLTP;
}

void AudioDecoder::Flush() {
  if (!ctx_) {
    // ATRAC9: a new decoder (LibAtrac9 has no reset).
    if (at9_) Atrac9ReleaseHandle(at9_);
    at9_ = Atrac9GetHandle();
    if (!at9_) FatalF("audio decoder: out of memory");
    std::lock_guard<std::mutex> lock(g_at9_init);
    if (Atrac9InitDecoder(at9_, at9_config_) != 0) Atrac9ReleaseHandle(at9_), at9_ = nullptr;
    return;
  }
  avcodec_flush_buffers(ctx_);
}

uint32_t AudioDecoder::samples() const { return at9_ ? samples_ : uint32_t(frame_->nb_samples); }
uint32_t AudioDecoder::channels() const {
  return at9_ ? at9_channels_ : uint32_t(frame_->ch_layout.nb_channels);
}
const float* AudioDecoder::channel(uint32_t c) const {
  if (at9_)
    return pcm_.data() + (size_t(c) * at9_frames_ + at9_index_) * at9_samples_;
  return reinterpret_cast<const float*>(frame_->extended_data[c]);
}

}  // namespace recomp::orbis
