#include "orbis/hle/audio_decoder.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
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

}  // namespace

std::unique_ptr<AudioDecoder> AudioDecoder::Atrac9(uint32_t config) {
  // ffmpeg's extradata: a version word, then the config bytes as an
  // ATRAC9 file stores them.
  uint8_t* x = static_cast<uint8_t*>(av_mallocz(12 + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!x) FatalF("audio decoder: out of memory");
  x[0] = 2;
  for (int i = 0; i < 4; i++) x[4 + i] = uint8_t(config >> (24 - 8 * i));
  const int superframe = int((((config & 0xFFFF) >> 5) + 1) << (config >> 3 & 3));
  AVCodecContext* ctx = Open(AV_CODEC_ID_ATRAC9, x, 12, superframe);
  if (!ctx) return nullptr;
  std::unique_ptr<AudioDecoder> d(new AudioDecoder);
  d->ctx_ = ctx;
  d->pkt_ = av_packet_alloc();
  d->frame_ = av_frame_alloc();
  if (!d->pkt_ || !d->frame_) FatalF("audio decoder: out of memory");
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
  av_frame_free(&frame_);
  av_packet_free(&pkt_);
  avcodec_free_context(&ctx_);
}

bool AudioDecoder::Decode(const uint8_t* data, size_t size) {
  if (av_new_packet(pkt_, int(size)) < 0) FatalF("audio decoder: out of memory");
  std::memcpy(pkt_->data, data, size);
  const int err = avcodec_send_packet(ctx_, pkt_);
  av_packet_unref(pkt_);
  return err >= 0 && avcodec_receive_frame(ctx_, frame_) >= 0 &&
         frame_->format == AV_SAMPLE_FMT_FLTP;
}

void AudioDecoder::Flush() { avcodec_flush_buffers(ctx_); }

uint32_t AudioDecoder::samples() const { return uint32_t(frame_->nb_samples); }
uint32_t AudioDecoder::channels() const { return uint32_t(frame_->ch_layout.nb_channels); }
const float* AudioDecoder::channel(uint32_t c) const {
  return reinterpret_cast<const float*>(frame_->extended_data[c]);
}

}  // namespace recomp::orbis
