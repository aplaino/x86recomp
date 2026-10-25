// libSceAvPlayer HLE on ffmpeg. A player reads its source through the
// game's file callbacks (or, without them, libkernel's files) on a guest
// service thread of its own, which decodes ahead: video into NV12 frames
// in texture memory from the game's allocateTexture (numOutputVideo-
// FrameBuffers of them, at least 2), audio into 16-bit interleaved
// chunks in memory from its allocate. Video frames are handed out when
// the player's clock (time since Start, without pauses) reaches them,
// audio chunks as soon as they are decoded (the game's AudioOut paces
// them). Events go to the game's event callback: ready once the source
// is open, play on start, stop at the end. The structures (init data,
// frame info, stream info) are libSceAvPlayer 11.00's; the decoders are
// ffmpeg's, so pixels may differ from the console's (docs/hle.md).

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "orbis/hle.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace {

using recomp::gaddr;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kErrInvalidParams = 0x806A0001, kErrOperationFailed = 0x806A0002;
constexpr int32_t kStateStop = 1, kStateReady = 2, kStatePlay = 3, kStatePause = 4;
constexpr size_t kAudioChunk = 1024;  // frames
constexpr size_t kQueueAhead = 8;     // decoded video frames (and 4x audio chunks) kept ahead

struct InitData {  // SceAvPlayerInitData
  gaddr mem_obj, allocate, deallocate, allocate_texture, deallocate_texture;
  gaddr file_obj, open, close, read_offset, size;
  gaddr event_obj, event;
  uint32_t frame_buffers;
  bool auto_start;
};

struct VideoFrame {
  gaddr data;
  int64_t pts_ms;
};
struct AudioFrame {
  gaddr data;
  int64_t pts_ms;
  uint32_t bytes;
};

struct Player {
  InitData init;
  recomp::Thread* thread = nullptr;  // the decoder's guest thread
  std::mutex mu;
  std::condition_variable cv;
  // The source.
  bool opened = false, eof = false, stop = false, looping = false, playing = false, paused = false;
  int width = 0, height = 0, pitch = 0, channels = 0, rate = 0;
  float aspect = 0;
  int64_t duration_ms = 0;
  // The clock: ms since Start, without pauses.
  Clock::time_point start;
  int64_t paused_ms = 0;
  Clock::time_point pause_at;
  // Decoded, waiting to be handed out; texture ring.
  std::deque<VideoFrame> video;
  std::deque<AudioFrame> audio;
  std::vector<gaddr> textures;
  size_t next_texture = 0;
  std::vector<gaddr> audio_buffers;
  size_t next_audio = 0;
  gaddr bounce = 0;  // the decoder's read buffer
  bool done = false;  // the decoder has finished
};

std::mutex g_mu;
std::map<gaddr, std::unique_ptr<Player>> g_players;

Player* Find(gaddr h) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_players.find(h);
  return it == g_players.end() ? nullptr : it->second.get();
}

int64_t NowMs(Player& p) {
  if (!p.playing) return 0;
  const auto now = p.paused ? p.pause_at : Clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now - p.start).count() - p.paused_ms;
}

void PutF(gaddr p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  recomp::Wr32(p, v);
}

// The decoder thread's ffmpeg input: the game's readOffset(obj, buf, pos,
// len) through a guest bounce buffer.
struct Input {
  Player* p;
  std::string path;
  int fd = -1;
  int64_t pos = 0, size = 0;
  gaddr bounce = 0;
  static constexpr int kBounce = 65536;
};

int ReadCb(void* opaque, uint8_t* buf, int n) {
  Input& in = *static_cast<Input*>(opaque);
  if (in.pos >= in.size) return AVERROR_EOF;
  n = int(std::min<int64_t>({n, Input::kBounce, in.size - in.pos}));
  recomp::Ctx& c = in.p->thread->ctx;
  const int64_t r = int64_t(recomp::CallGuest(
      c, in.p->init.read_offset, {in.p->init.file_obj, in.bounce, uint64_t(in.pos), uint64_t(n)}));
  if (int32_t(r) <= 0) return AVERROR_EOF;
  std::memcpy(buf, recomp::GPtr(in.bounce), size_t(int32_t(r)));
  in.pos += int32_t(r);
  return int(int32_t(r));
}

int64_t SeekCb(void* opaque, int64_t off, int whence) {
  Input& in = *static_cast<Input*>(opaque);
  if (whence == AVSEEK_SIZE) return in.size;
  whence &= ~AVSEEK_FORCE;
  in.pos = whence == SEEK_SET ? off : whence == SEEK_CUR ? in.pos + off : in.size + off;
  return in.pos;
}

void Event(Player& p, int32_t id) {
  if (p.init.event) recomp::CallGuest(p.thread->ctx, p.init.event, {p.init.event_obj, uint64_t(id), 0, 0});
}

gaddr GuestAlloc(Player& p, bool texture, uint64_t align, uint64_t size) {
  const gaddr fn = texture ? p.init.allocate_texture : p.init.allocate;
  return recomp::CallGuest(p.thread->ctx, fn, {p.init.mem_obj, align, size});
}

// The decoder thread: opens the source, then decodes ahead of the clock
// until the end (or stop).
void Decode(Player& p, std::unique_ptr<Input> in) {
  recomp::Ctx& c = p.thread->ctx;
  in->bounce = recomp::CallGuest(c, p.init.allocate, {p.init.mem_obj, 64, uint64_t(Input::kBounce)});
  p.bounce = in->bounce;
  recomp::PutStr(in->bounce, in->path);  // the name, for open
  const int32_t opened = int32_t(recomp::CallGuest(c, p.init.open, {p.init.file_obj, in->bounce}));
  if (opened < 0) recomp::FatalF("sceAvPlayerAddSource(%s): the game's open failed", in->path.c_str());
  in->size = int64_t(recomp::CallGuest(c, p.init.size, {p.init.file_obj}));
  av_log_set_level(AV_LOG_ERROR);
  AVIOContext* io = avio_alloc_context(static_cast<uint8_t*>(av_malloc(Input::kBounce)),
                                       Input::kBounce, 0, in.get(), ReadCb, nullptr, SeekCb);
  AVFormatContext* fmt = avformat_alloc_context();
  fmt->pb = io;
  fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0 ||
      avformat_find_stream_info(fmt, nullptr) < 0)
    recomp::FatalF("sceAvPlayerAddSource(%s): ffmpeg cannot read it", in->path.c_str());
  const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  auto open_decoder = [&](int s) -> AVCodecContext* {
    if (s < 0) return nullptr;
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[s]->codecpar->codec_id);
    AVCodecContext* d = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!d || avcodec_parameters_to_context(d, fmt->streams[s]->codecpar) < 0 ||
        avcodec_open2(d, codec, nullptr) < 0)
      recomp::FatalF("sceAvPlayerAddSource(%s): no decoder for stream %d", in->path.c_str(), s);
    return d;
  };
  AVCodecContext* vdec = open_decoder(vs);
  AVCodecContext* adec = open_decoder(as);
  {
    std::lock_guard<std::mutex> l(p.mu);
    if (vdec) {
      p.width = vdec->width;
      p.height = vdec->height;
      p.pitch = (p.width + 63) & ~63;
      const AVRational sar = vdec->sample_aspect_ratio;
      p.aspect = float(p.width) * float(sar.num ? double(sar.num) / sar.den : 1.0) / float(p.height);
    }
    if (adec) {
      p.channels = adec->ch_layout.nb_channels;
      p.rate = adec->sample_rate;
    }
    p.duration_ms = fmt->duration > 0 ? fmt->duration / 1000 : 0;
    p.opened = true;
  }
  // The texture ring and the audio buffers.
  const uint32_t nframes = std::max<uint32_t>(p.init.frame_buffers, 2) + 2;
  for (uint32_t i = 0; vdec && i < nframes; i++)
    p.textures.push_back(GuestAlloc(p, true, 0x100, uint64_t(p.pitch) * p.height * 3 / 2));
  for (size_t i = 0; adec && i < 4 * kQueueAhead + 2; i++)
    p.audio_buffers.push_back(GuestAlloc(p, false, 0x40, kAudioChunk * 2 * uint64_t(p.channels)));
  Event(p, kStateReady);
  if (p.init.auto_start) {
    std::lock_guard<std::mutex> l(p.mu);
    p.playing = true;
    p.start = Clock::now();
  }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  std::vector<int16_t> pcm;
  int64_t audio_pts_ms = 0;
  bool announced = p.init.auto_start;
  for (;;) {
    {
      std::unique_lock<std::mutex> l(p.mu);
      p.cv.wait(l, [&] {
        return p.stop || (p.video.size() < kQueueAhead && p.audio.size() < 4 * kQueueAhead);
      });
      if (p.stop) break;
      if (p.playing && !announced) {
        announced = true;
        l.unlock();
        Event(p, kStatePlay);
      }
    }
    if (av_read_frame(fmt, pkt) < 0) {
      std::unique_lock<std::mutex> l(p.mu);
      if (p.looping) {
        l.unlock();
        av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
        continue;
      }
      p.eof = true;
      p.cv.notify_all();
      break;
    }
    AVCodecContext* d = pkt->stream_index == vs ? vdec : pkt->stream_index == as ? adec : nullptr;
    if (!d || avcodec_send_packet(d, pkt) < 0) {
      av_packet_unref(pkt);
      continue;
    }
    const AVRational tb = fmt->streams[pkt->stream_index]->time_base;
    av_packet_unref(pkt);
    while (avcodec_receive_frame(d, frame) == 0) {
      const int64_t pts_ms = frame->best_effort_timestamp == AV_NOPTS_VALUE
                                 ? 0
                                 : av_rescale_q(frame->best_effort_timestamp, tb, {1, 1000});
      if (d == vdec) {
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
          recomp::FatalF("sceAvPlayer: video format %s not implemented yet",
                         av_get_pix_fmt_name(AVPixelFormat(frame->format)));
        // NV12: the luma plane, then chroma pairs (u, v).
        const gaddr t = p.textures[p.next_texture++ % p.textures.size()];
        uint8_t* y = recomp::GPtr(t);
        uint8_t* uv = y + size_t(p.pitch) * p.height;
        for (int r = 0; r < p.height; r++)
          std::memcpy(y + size_t(r) * p.pitch, frame->data[0] + size_t(r) * frame->linesize[0],
                      size_t(p.width));
        for (int r = 0; r < p.height / 2; r++)
          for (int x = 0; x < p.width / 2; x++) {
            uv[size_t(r) * p.pitch + 2 * x] = frame->data[1][size_t(r) * frame->linesize[1] + x];
            uv[size_t(r) * p.pitch + 2 * x + 1] = frame->data[2][size_t(r) * frame->linesize[2] + x];
          }
        std::lock_guard<std::mutex> l(p.mu);
        p.video.push_back({t, pts_ms});
      } else {
        // To 16-bit interleaved.
        const int n = frame->nb_samples, ch = p.channels;
        if (pcm.empty()) audio_pts_ms = pts_ms;
        const size_t at = pcm.size();
        pcm.resize(at + size_t(n) * ch);
        for (int i = 0; i < n; i++)
          for (int k = 0; k < ch; k++) {
            float v;
            switch (frame->format) {
              case AV_SAMPLE_FMT_FLTP: v = reinterpret_cast<const float*>(frame->extended_data[k])[i]; break;
              case AV_SAMPLE_FMT_FLT: v = reinterpret_cast<const float*>(frame->data[0])[i * ch + k]; break;
              case AV_SAMPLE_FMT_S16P:
                v = reinterpret_cast<const int16_t*>(frame->extended_data[k])[i] / 32768.0f;
                break;
              case AV_SAMPLE_FMT_S16: v = reinterpret_cast<const int16_t*>(frame->data[0])[i * ch + k] / 32768.0f; break;
              default:
                recomp::FatalF("sceAvPlayer: audio format %s not implemented yet",
                               av_get_sample_fmt_name(AVSampleFormat(frame->format)));
            }
            const float sv = std::min(std::max(v * 32768.0f, -32768.0f), 32767.0f);
            pcm[at + size_t(i) * ch + k] = int16_t(std::lrintf(sv));
          }
        while (pcm.size() >= kAudioChunk * p.channels) {
          const gaddr b = p.audio_buffers[p.next_audio++ % p.audio_buffers.size()];
          const uint32_t bytes = uint32_t(kAudioChunk * 2 * p.channels);
          std::memcpy(recomp::GPtr(b), pcm.data(), bytes);
          pcm.erase(pcm.begin(), pcm.begin() + ptrdiff_t(kAudioChunk * p.channels));
          std::lock_guard<std::mutex> l(p.mu);
          p.audio.push_back({b, audio_pts_ms, bytes});
          audio_pts_ms += int64_t(kAudioChunk) * 1000 / p.rate;
        }
      }
    }
  }
  // The end: wait for the game to take what is left, then stop.
  {
    std::unique_lock<std::mutex> l(p.mu);
    p.cv.wait(l, [&] { return p.stop || (p.video.empty() && p.audio.empty()); });
    p.playing = false;
  }
  Event(p, kStateStop);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&vdec);
  avcodec_free_context(&adec);
  avformat_close_input(&fmt);
  avio_context_free(&io);
  recomp::CallGuest(c, p.init.close, {p.init.file_obj});
  std::lock_guard<std::mutex> l(p.mu);
  p.done = true;
  p.cv.notify_all();
}

}  // namespace

using recomp::hle::Arg;

// SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData* init)
ORBIS_HLE("libSceAvPlayer", sceAvPlayerInit, 1) {
  const gaddr d = Arg(c, 0);
  if (!d) return 0;
  auto p = std::make_unique<Player>();
  InitData& in = p->init;
  in.mem_obj = recomp::Rd64(d);
  in.allocate = recomp::Rd64(d + 8);
  in.deallocate = recomp::Rd64(d + 0x10);
  in.allocate_texture = recomp::Rd64(d + 0x18);
  in.deallocate_texture = recomp::Rd64(d + 0x20);
  in.file_obj = recomp::Rd64(d + 0x28);
  in.open = recomp::Rd64(d + 0x30);
  in.close = recomp::Rd64(d + 0x38);
  in.read_offset = recomp::Rd64(d + 0x40);
  in.size = recomp::Rd64(d + 0x48);
  in.event_obj = recomp::Rd64(d + 0x50);
  in.event = recomp::Rd64(d + 0x58);
  in.frame_buffers = recomp::Rd32(d + 0x68);
  in.auto_start = recomp::GPtr(d + 0x6C)[0] != 0;
  if (!in.allocate || !in.deallocate || !in.allocate_texture || !in.deallocate_texture) return 0;
  if (!in.open || !in.read_offset || !in.size)
    recomp::FatalF("sceAvPlayerInit: without file callbacks not implemented yet");
  // The handle: a block the game only passes back.
  const gaddr h = recomp::CallGuest(c, in.allocate, {in.mem_obj, 0x20, 0x260});
  if (!h) return 0;
  std::memset(recomp::GPtr(h), 0, 0x260);
  std::lock_guard<std::mutex> l(g_mu);
  g_players[h] = std::move(p);
  return h;
}

// int sceAvPlayerPostInit(SceAvPlayerHandle h, SceAvPlayerPostInitData* d):
// buffer sizes and decoder settings, which ffmpeg does not need.
ORBIS_HLE("libSceAvPlayer", sceAvPlayerPostInit, 2) {
  if (!Find(Arg(c, 0)) || !Arg(c, 1)) return kErrInvalidParams;
  return 0;
}

// int sceAvPlayerAddSource(SceAvPlayerHandle h, const char* path)
ORBIS_HLE("libSceAvPlayer", sceAvPlayerAddSource, 2) {
  Player* p = Find(Arg(c, 0));
  if (!p || !Arg(c, 1)) return kErrInvalidParams;
  if (p->thread) recomp::FatalF("sceAvPlayerAddSource: a second source not implemented yet");
  auto in = std::make_unique<Input>();
  in->p = p;
  in->path = recomp::GStr(Arg(c, 1));
  recomp::WarnF("avplayer: %s\n", in->path.c_str());
  p->thread = recomp::CreateGuestThread(1u << 20);
  Input* raw = in.release();
  recomp::StartServiceThread(p->thread, [p, raw] { Decode(*p, std::unique_ptr<Input>(raw)); });
  return 0;
}

ORBIS_HLE("libSceAvPlayer", sceAvPlayerStart, 1) {
  Player* p = Find(Arg(c, 0));
  if (!p) return kErrOperationFailed;
  std::lock_guard<std::mutex> l(p->mu);
  if (!p->playing) {
    p->playing = true;
    p->start = Clock::now();
  }
  p->cv.notify_all();
  return 0;
}

ORBIS_HLE("libSceAvPlayer", sceAvPlayerStop, 1) {
  Player* p = Find(Arg(c, 0));
  if (!p) return kErrOperationFailed;
  std::lock_guard<std::mutex> l(p->mu);
  p->stop = true;
  p->cv.notify_all();
  return 0;
}

ORBIS_HLE("libSceAvPlayer", sceAvPlayerPause, 1) {
  Player* p = Find(Arg(c, 0));
  if (!p) return kErrOperationFailed;
  std::lock_guard<std::mutex> l(p->mu);
  if (!p->paused) {
    p->paused = true;
    p->pause_at = Clock::now();
  }
  return 0;
}

ORBIS_HLE("libSceAvPlayer", sceAvPlayerResume, 1) {
  Player* p = Find(Arg(c, 0));
  if (!p) return kErrOperationFailed;
  std::lock_guard<std::mutex> l(p->mu);
  if (p->paused) {
    p->paused = false;
    p->paused_ms += std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - p->pause_at).count();
  }
  return 0;
}

ORBIS_HLE("libSceAvPlayer", sceAvPlayerSetLooping, 2) {
  Player* p = Find(Arg(c, 0));
  if (!p) return kErrOperationFailed;
  std::lock_guard<std::mutex> l(p->mu);
  p->looping = Arg(c, 1) & 1;
  return 0;
}

// bool sceAvPlayerIsActive(SceAvPlayerHandle h): until the source has
// been played to its end (or stopped).
ORBIS_HLE("libSceAvPlayer", sceAvPlayerIsActive, 1) {
  Player* p = Find(Arg(c, 0));
  if (!p) return 0;
  std::lock_guard<std::mutex> l(p->mu);
  return !(p->stop || (p->eof && p->video.empty() && p->audio.empty()));
}

// bool sceAvPlayerGetVideoDataEx(SceAvPlayerHandle h,
// SceAvPlayerFrameInfoEx* info): the newest frame the clock has reached.
ORBIS_HLE("libSceAvPlayer", sceAvPlayerGetVideoDataEx, 2) {
  Player* p = Find(Arg(c, 0));
  const gaddr info = Arg(c, 1);
  if (!p || !info) return 0;
  std::lock_guard<std::mutex> l(p->mu);
  const int64_t now = NowMs(*p);
  if (p->video.empty() || p->video.front().pts_ms > now) return 0;
  VideoFrame f = p->video.front();
  p->video.pop_front();
  while (!p->video.empty() && p->video.front().pts_ms <= now) {  // late: skip to the newest
    f = p->video.front();
    p->video.pop_front();
  }
  p->cv.notify_all();
  std::memset(recomp::GPtr(info), 0, 0x68);
  recomp::Wr64(info, f.data);
  recomp::Wr64(info + 0x10, uint64_t(f.pts_ms));
  recomp::Wr32(info + 0x18, uint32_t(p->width));
  recomp::Wr32(info + 0x1C, uint32_t(p->height));
  PutF(info + 0x20, p->aspect);
  recomp::Wr32(info + 0x3C, uint32_t(p->pitch));
  recomp::GPtr(info)[0x40] = 8;  // luma and chroma bit depths
  recomp::GPtr(info)[0x41] = 8;
  return 1;
}

// bool sceAvPlayerGetAudioData(SceAvPlayerHandle h, SceAvPlayerFrameInfo*
// info): the next chunk of 1024 16-bit frames.
ORBIS_HLE("libSceAvPlayer", sceAvPlayerGetAudioData, 2) {
  Player* p = Find(Arg(c, 0));
  const gaddr info = Arg(c, 1);
  if (!p || !info) return 0;
  std::lock_guard<std::mutex> l(p->mu);
  if (!p->playing || p->paused || p->audio.empty()) return 0;
  const AudioFrame f = p->audio.front();
  p->audio.pop_front();
  p->cv.notify_all();
  std::memset(recomp::GPtr(info), 0, 0x28);
  recomp::Wr64(info, f.data);
  recomp::Wr64(info + 0x10, uint64_t(f.pts_ms));
  recomp::Wr16(info + 0x18, uint16_t(p->channels));
  recomp::Wr32(info + 0x1C, uint32_t(p->rate));
  recomp::Wr32(info + 0x20, f.bytes);
  return 1;
}

// int32_t sceAvPlayerStreamCount(SceAvPlayerHandle h): once the source is
// open.
ORBIS_HLE("libSceAvPlayer", sceAvPlayerStreamCount, 1) {
  Player* p = Find(Arg(c, 0));
  if (!p) return kErrOperationFailed;
  std::unique_lock<std::mutex> l(p->mu);
  if (!p->opened) return kErrOperationFailed;
  return uint32_t((p->width ? 1 : 0) + (p->channels ? 1 : 0));
}

// int sceAvPlayerGetStreamInfo(SceAvPlayerHandle h, uint32_t index,
// SceAvPlayerStreamInfo* info): {type (0 video, 1 audio), details,
// duration, start time}; video first.
ORBIS_HLE("libSceAvPlayer", sceAvPlayerGetStreamInfo, 3) {
  Player* p = Find(Arg(c, 0));
  const gaddr info = Arg(c, 2);
  if (!p || !info) return kErrInvalidParams;
  std::lock_guard<std::mutex> l(p->mu);
  if (!p->opened) return kErrOperationFailed;
  uint32_t i = uint32_t(Arg(c, 1));
  if (!p->width) i++;
  std::memset(recomp::GPtr(info), 0, 0x28);
  if (i == 0) {
    recomp::Wr32(info, 0);
    recomp::Wr32(info + 8, uint32_t(p->width));
    recomp::Wr32(info + 12, uint32_t(p->height));
    PutF(info + 16, p->aspect);
  } else if (i == 1 && p->channels) {
    recomp::Wr32(info, 1);
    recomp::Wr16(info + 8, uint16_t(p->channels));
    recomp::Wr32(info + 12, uint32_t(p->rate));
  } else {
    return kErrInvalidParams;
  }
  recomp::Wr64(info + 0x18, uint64_t(p->duration_ms));
  return 0;
}

// Every stream plays.
ORBIS_HLE("libSceAvPlayer", sceAvPlayerEnableStream, 2) {
  return Find(Arg(c, 0)) ? 0 : kErrOperationFailed;
}

// Stops the player, waits for its decoder and gives its memory back.
ORBIS_HLE("libSceAvPlayer", sceAvPlayerClose, 1) {
  Player* p = Find(Arg(c, 0));
  if (!p) return kErrInvalidParams;
  {
    std::unique_lock<std::mutex> l(p->mu);
    p->stop = true;
    p->cv.notify_all();
    if (p->thread) p->cv.wait(l, [&] { return p->done; });
  }
  const InitData& in = p->init;
  for (gaddr t : p->textures) recomp::CallGuest(c, in.deallocate_texture, {in.mem_obj, t});
  for (gaddr b : p->audio_buffers) recomp::CallGuest(c, in.deallocate, {in.mem_obj, b});
  if (p->bounce) recomp::CallGuest(c, in.deallocate, {in.mem_obj, p->bounce});
  const gaddr h = Arg(c, 0);
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_players.erase(h);
  }
  recomp::CallGuest(c, in.deallocate, {in.mem_obj, h});
  return 0;
}
