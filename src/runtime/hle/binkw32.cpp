// binkw32 (Bink video, RAD 1.x API): the 10 entry points the game imports,
// on ffmpeg's Bink demuxer and decoders. Dungeon Siege opens its movies
// from memory (BINKFROMMEMORY, a Tank file view), decodes one frame per
// BinkWait/BinkDoFrame, and copies it into a locked DirectDraw surface
// with BinkCopyToBuffer. Audio goes to the Miles mixer, as BinkOpenMiles
// would route it.
//
// The BINK struct is a guest block: the game reads Frames (+0x08) and
// FrameNum (+0x0C) directly.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "runtime/audio/mixer.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/mss32.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kBinkFromMemory = 0x04000000;
constexpr uint32_t kBinkStructSize = 0x200;

// BINK struct offsets (bink.h, 1.x).
enum : uint32_t {
  kWidth = 0x00, kHeight = 0x04, kFrames = 0x08, kFrameNum = 0x0C,
  kLastFrameNum = 0x10, kFrameRate = 0x14, kFrameRateDiv = 0x18,
  kOpenFlags = 0x20, kSize = 0x28,
};

struct Movie {
  uint32_t guest = 0;
  std::vector<uint8_t> data;
  int64_t pos = 0;
  AVFormatContext* fmt = nullptr;
  AVIOContext* io = nullptr;
  int vs = -1, as = -1;
  AVCodecContext* vdec = nullptr;
  AVCodecContext* adec = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;
  SwsContext* sws = nullptr;
  std::vector<uint8_t> bgra;  // the current frame, w*h*4
  int w = 0, h = 0;
  uint32_t frames = 0, frame_num = 1, decoded = 0;
  double fps = 15;
  bool started = false, paused = false;
  Clock::time_point start, paused_at;
  bool video_eof = false;
  // Audio through the Miles mixer.
  std::unique_ptr<audio::Voice> voice;
  audio::QueueSource* queue = nullptr;

  ~Movie() {
    if (voice) {
      std::lock_guard<std::mutex> l(audio::GetMixer().mu());
      audio::GetMixer().Remove(voice.get());
    }
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&vdec);
    avcodec_free_context(&adec);
    avformat_close_input(&fmt);
    if (io) {
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
  }
};

std::mutex g_mu;
std::map<uint32_t, std::unique_ptr<Movie>> g_movies;  // by BINK guest ptr
uint32_t g_sound_track = 0;   // BinkSetSoundTrack; ~0 = no sound
bool g_sound_system = false;  // BinkSetSoundSystem(BinkOpenMiles, ...)

uint32_t Le32(const uint8_t* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
}

int ReadCb(void* op, uint8_t* buf, int n) {
  auto* m = static_cast<Movie*>(op);
  int64_t left = int64_t(m->data.size()) - m->pos;
  if (left <= 0) return AVERROR_EOF;
  n = int(std::min<int64_t>(n, left));
  std::memcpy(buf, m->data.data() + m->pos, n);
  m->pos += n;
  return n;
}

int64_t SeekCb(void* op, int64_t off, int whence) {
  auto* m = static_cast<Movie*>(op);
  int64_t size = int64_t(m->data.size());
  if (whence == AVSEEK_SIZE) return size;
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: break;
    case SEEK_CUR: off += m->pos; break;
    case SEEK_END: off += size; break;
    default: return -1;
  }
  if (off < 0 || off > size) return -1;
  return m->pos = off;
}

// Bink's skip blocks copy from the previous frame. RAD's decoder starts
// from black; ffmpeg's first reference is all zero bytes, which is bright
// green in YUV (the credits movie keeps its whole background as skip
// blocks). So every frame buffer starts out black: Y 0, U/V 128, A 255.
int BlackGetBuffer(AVCodecContext* ctx, AVFrame* f, int flags) {
  int r = avcodec_default_get_buffer2(ctx, f, flags);
  if (r < 0) return r;
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
  if (!d || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) || (d->flags & AV_PIX_FMT_FLAG_RGB))
    return r;
  for (int p = 0; p < 4 && f->data[p]; p++) {
    int h = f->height;
    if (p == 1 || p == 2) h = -((-h) >> d->log2_chroma_h);
    uint8_t v = (p == 1 || p == 2) ? 128 : p == 3 ? 255 : 0;
    std::memset(f->data[p], v, size_t(f->linesize[p]) * h);
  }
  return r;
}

AVCodecContext* OpenDecoder(AVStream* st) {
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec) return nullptr;
  AVCodecContext* c = avcodec_alloc_context3(codec);
  if (c && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    c->get_buffer2 = BlackGetBuffer;
  if (!c || avcodec_parameters_to_context(c, st->codecpar) < 0 ||
      avcodec_open2(c, codec, nullptr) < 0) {
    avcodec_free_context(&c);
    return nullptr;
  }
  return c;
}

void QueueAudio(Movie& m, AVFrame* f) {
  int out_ch = std::min(f->ch_layout.nb_channels, 2);
  if (!m.voice) {
    auto q = std::make_unique<audio::QueueSource>(out_ch, f->sample_rate);
    m.queue = q.get();
    m.voice = std::make_unique<audio::Voice>();
    m.voice->src = std::move(q);
    std::lock_guard<std::mutex> l(audio::GetMixer().mu());
    m.voice->state = m.paused ? audio::Voice::State::Paused
                              : audio::Voice::State::Playing;
    audio::GetMixer().Add(m.voice.get());
  }
  if (out_ch != m.queue->channels()) return;
  std::vector<float> buf(size_t(f->nb_samples) * out_ch);
  audio::ConvertFrame(f, out_ch, buf.data());
  m.queue->Push(buf.data(), f->nb_samples);
}

void ConvertVideo(Movie& m) {
  AVFrame* f = m.frame;
  m.sws = sws_getCachedContext(m.sws, f->width, f->height, AVPixelFormat(f->format),
                               m.w, m.h, AV_PIX_FMT_BGRA, SWS_POINT, nullptr,
                               nullptr, nullptr);
  if (!m.sws) return;
  m.bgra.resize(size_t(m.w) * m.h * 4);
  uint8_t* dst[4] = {m.bgra.data()};
  int stride[4] = {m.w * 4};
  sws_scale(m.sws, f->data, f->linesize, 0, f->height, dst, stride);
}

// Demuxes until the next video frame is decoded (queueing audio met on
// the way). False at the end of the movie.
bool DecodeNextFrame(Movie& m) {
  for (;;) {
    int r = avcodec_receive_frame(m.vdec, m.frame);
    if (r == 0) {
      ConvertVideo(m);
      av_frame_unref(m.frame);
      m.decoded++;
      return true;
    }
    if (r != AVERROR(EAGAIN) || m.video_eof) return false;
    if (av_read_frame(m.fmt, m.pkt) < 0) {
      avcodec_send_packet(m.vdec, nullptr);
      m.video_eof = true;
      continue;
    }
    if (m.pkt->stream_index == m.vs) {
      avcodec_send_packet(m.vdec, m.pkt);
    } else if (m.pkt->stream_index == m.as && m.adec) {
      if (avcodec_send_packet(m.adec, m.pkt) == 0) {
        while (avcodec_receive_frame(m.adec, m.frame) == 0) {
          QueueAudio(m, m.frame);
          av_frame_unref(m.frame);
        }
      }
    }
    av_packet_unref(m.pkt);
  }
}

Movie* Find(uint32_t b, const char* fn) {
  auto it = g_movies.find(b);
  if (it == g_movies.end()) {
    WarnF("binkw32: %s: unknown BINK %08X\n", fn, b);
    return nullptr;
  }
  return it->second.get();
}

void SyncGuest(const Movie& m) {
  Wr32(m.guest + kFrameNum, m.frame_num);
  Wr32(m.guest + kLastFrameNum, m.frame_num > 1 ? m.frame_num - 1 : 0);
}

HLE_NAMED("binkw32.dll", "_BinkSetSoundSystem@8", BinkSetSoundSystem, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  g_sound_system = Arg(c, 0) != 0;
  return 1;
}

HLE_NAMED("binkw32.dll", "_BinkSetSoundTrack@4", BinkSetSoundTrack, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  g_sound_track = Arg(c, 0);
  return 0;
}

// The game passes this function's address to BinkSetSoundSystem; Bink
// would call it to attach to the Miles driver. Our Bink talks to the
// mixer directly, so it is never called.
HLE_NAMED("binkw32.dll", "_BinkOpenMiles@4", BinkOpenMiles, 1) {
  FatalF("binkw32: BinkOpenMiles called directly");
}

// BinkOpen(name_or_memory, flags) -> HBINK.
HLE_NAMED("binkw32.dll", "_BinkOpen@8", BinkOpen, 2) {
  uint32_t src = Arg(c, 0), flags = Arg(c, 1);
  auto m = std::make_unique<Movie>();
  std::string what;
  if (flags & kBinkFromMemory) {
    if (!EnsureReadable(c, src, 8) || std::memcmp(GPtr(src), "BIK", 3) != 0) {
      WarnF("binkw32: BinkOpen: no Bink header at %08X\n", src);
      return 0;
    }
    uint32_t size = Le32(GPtr(src) + 4) + 8;
    if (!EnsureReadable(c, src, size)) {
      WarnF("binkw32: BinkOpen: cannot page in %u bytes at %08X\n", size, src);
      return 0;
    }
    m->data.assign(GPtr(src), GPtr(src) + size);
    what = "memory";
  } else {
    what = GStr(src);
    FILE* f = std::fopen(HostPath(what).c_str(), "rb");
    if (!f) {
      WarnF("binkw32: BinkOpen(%s): cannot open\n", what.c_str());
      return 0;
    }
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
      m->data.insert(m->data.end(), buf, buf + n);
    std::fclose(f);
  }
  if (m->data.size() < 44) return 0;
  const uint8_t* h = m->data.data();
  m->frames = Le32(h + 8);
  uint32_t width = Le32(h + 20), height = Le32(h + 24);
  uint32_t rate = Le32(h + 28), div = Le32(h + 32);
  if (rate && div) m->fps = double(rate) / div;

  av_log_set_level(AV_LOG_ERROR);
  m->io = avio_alloc_context(static_cast<uint8_t*>(av_malloc(32768)), 32768, 0,
                             m.get(), ReadCb, nullptr, SeekCb);
  m->fmt = avformat_alloc_context();
  m->fmt->pb = m->io;
  m->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (avformat_open_input(&m->fmt, nullptr, av_find_input_format("bink"), nullptr) < 0 ||
      avformat_find_stream_info(m->fmt, nullptr) < 0) {
    WarnF("binkw32: BinkOpen(%s): ffmpeg cannot parse the movie\n", what.c_str());
    return 0;
  }
  m->vs = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (m->vs < 0 || !(m->vdec = OpenDecoder(m->fmt->streams[m->vs]))) {
    WarnF("binkw32: BinkOpen(%s): no decodable video stream\n", what.c_str());
    return 0;
  }
  bool sound;
  {
    std::lock_guard<std::mutex> l(g_mu);
    sound = g_sound_system && g_sound_track != 0xFFFFFFFF && MilesDriverOpen();
  }
  if (sound) {
    m->as = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, m->vs, nullptr, 0);
    if (m->as >= 0) m->adec = OpenDecoder(m->fmt->streams[m->as]);
  }
  m->w = int(width);
  m->h = int(height);
  m->pkt = av_packet_alloc();
  m->frame = av_frame_alloc();

  m->guest = HeapAllocProcess(kBinkStructSize, true);
  Wr32(m->guest + kWidth, width);
  Wr32(m->guest + kHeight, height);
  Wr32(m->guest + kFrames, m->frames);
  Wr32(m->guest + kFrameRate, rate);
  Wr32(m->guest + kFrameRateDiv, div);
  Wr32(m->guest + kOpenFlags, flags);
  Wr32(m->guest + kSize, uint32_t(m->data.size()));
  SyncGuest(*m);
  WarnF("binkw32: BinkOpen(%s): %ux%u %s, %u frames at %.3f fps, %s\n",
        what.c_str(), width, height, av_get_pix_fmt_name(m->vdec->pix_fmt),
        m->frames, m->fps, m->adec ? "with audio" : "silent");
  uint32_t b = m->guest;
  std::lock_guard<std::mutex> l(g_mu);
  g_movies[b] = std::move(m);
  return b;
}

HLE_NAMED("binkw32.dll", "_BinkClose@4", BinkClose, 1) {
  std::unique_ptr<Movie> m;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_movies.find(Arg(c, 0));
    if (it == g_movies.end()) return 0;
    m = std::move(it->second);
    g_movies.erase(it);
  }
  WarnF("binkw32: BinkClose at frame %u/%u (from %08X)\n", m->frame_num,
        m->frames, Rd32(c.esp));
  return 0;  // the guest block is leaked: 512 bytes per movie
}

// BinkWait(HBINK) -> nonzero while it is too early for the next frame.
HLE_NAMED("binkw32.dll", "_BinkWait@4", BinkWait, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkWait");
  if (!m) return 0;
  if (m->paused) return 1;
  auto now = Clock::now();
  if (!m->started) {
    m->started = true;
    m->start = now;
  }
  auto due = m->start + std::chrono::duration_cast<Clock::duration>(
                            std::chrono::duration<double>((m->frame_num - 1) / m->fps));
  return now < due ? 1 : 0;
}

// BinkDoFrame(HBINK): decodes the frame FrameNum.
HLE_NAMED("binkw32.dll", "_BinkDoFrame@4", BinkDoFrame, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkDoFrame");
  if (!m) return 0;
  while (m->decoded < m->frame_num)
    if (!DecodeNextFrame(*m)) break;
  return 0;
}

HLE_NAMED("binkw32.dll", "_BinkNextFrame@4", BinkNextFrame, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkNextFrame");
  if (!m) return 0;
  if (m->frame_num < m->frames) m->frame_num++;
  SyncGuest(*m);
  return 0;
}

// BinkPause(HBINK, pause) -> the new pause state.
HLE_NAMED("binkw32.dll", "_BinkPause@8", BinkPause, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkPause");
  if (!m) return 0;
  bool p = Arg(c, 1) != 0;
  if (p != m->paused) {
    auto now = Clock::now();
    if (p)
      m->paused_at = now;
    else if (m->started)
      m->start += now - m->paused_at;
    m->paused = p;
    if (m->voice) {
      std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
      m->voice->state = p ? audio::Voice::State::Paused : audio::Voice::State::Playing;
    }
  }
  return p;
}

// BinkCopyToBuffer(HBINK, dest, destpitch, destheight, destx, desty, flags)
// with the BINKSURFACE* format in the low bits of flags.
HLE_NAMED("binkw32.dll", "_BinkCopyToBuffer@28", BinkCopyToBuffer, 7) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkCopyToBuffer");
  if (!m || m->bgra.empty()) return 0;
  uint32_t dest = Arg(c, 1), pitch = Arg(c, 2), height = Arg(c, 3);
  uint32_t dx = Arg(c, 4), dy = Arg(c, 5), type = Arg(c, 6) & 0xF;
  int bpp;
  switch (type) {
    case 1: case 2: bpp = 3; break;
    case 3: case 4: case 5: case 6: bpp = 4; break;
    case 7: case 8: case 9: case 10: bpp = 2; break;
    default:
      FatalF("binkw32: BinkCopyToBuffer: surface type %u not implemented", type);
  }
  uint32_t rows = std::min<uint32_t>(m->h, height > dy ? height - dy : 0);
  for (uint32_t y = 0; y < rows; y++) {
    const uint8_t* s = m->bgra.data() + size_t(y) * m->w * 4;
    uint8_t* d = GPtr(dest + (dy + y) * pitch + dx * bpp);
    for (int x = 0; x < m->w; x++, s += 4) {
      uint8_t b = s[0], g = s[1], r = s[2], a = s[3];
      switch (type) {
        case 1: d[0] = b; d[1] = g; d[2] = r; break;
        case 2: d[0] = r; d[1] = g; d[2] = b; break;
        case 3: d[0] = b; d[1] = g; d[2] = r; d[3] = 0xFF; break;
        case 4: d[0] = r; d[1] = g; d[2] = b; d[3] = 0xFF; break;
        case 5: d[0] = b; d[1] = g; d[2] = r; d[3] = a; break;
        case 6: d[0] = r; d[1] = g; d[2] = b; d[3] = a; break;
        default: {
          uint16_t v;
          if (type == 7)
            v = uint16_t((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4);
          else if (type == 8)
            v = uint16_t((a >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
          else if (type == 9)
            v = uint16_t((r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
          else
            v = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
          d[0] = uint8_t(v);
          d[1] = uint8_t(v >> 8);
        }
      }
      d += bpp;
    }
  }
  return 0;
}

}  // namespace
}  // namespace recomp::hle
