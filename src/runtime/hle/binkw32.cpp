// binkw32 (Bink video, RAD 1.x API, up to 1.8's) on ffmpeg's Bink demuxer
// and decoders. Movies may be opened from memory (BINKFROMMEMORY, e.g. a
// view of an archive file); games decode one frame per BinkWait /
// BinkDoFrame and copy it into locked memory with BinkCopyToBuffer(Rect).
// Audio goes to the software mixer whichever sound system the game picks
// (BinkOpenMiles or BinkOpenDirectSound route it there too).
//
// Sound tracks: BinkSetSoundTrack(index) (older Bink: ~0 for none) or
// BinkSetSoundTrack(count, track IDs) with BINKSNDTRACK (1.8) choose the
// audio tracks; by default the first one plays. Each track has its volume
// (BinkSetVolume by track ID).
//
// The BINK struct is a guest block: games read Width, Height, Frames
// (+0x08), FrameNum (+0x0C) and, in 1.8, NumTracks (+0xD0) and the track
// ID list (+0x250) directly.

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

constexpr uint32_t kBinkFromMemory = 0x04000000, kBinkSndTrack = 0x00004000;
constexpr uint32_t kBinkStructSize = 0x400;

// BINK struct offsets (bink.h, 1.x; the track fields as 1.8 games read
// them). The track ID list lives in the block too.
enum : uint32_t {
  kWidth = 0x00, kHeight = 0x04, kFrames = 0x08, kFrameNum = 0x0C,
  kLastFrameNum = 0x10, kFrameRate = 0x14, kFrameRateDiv = 0x18,
  kOpenFlags = 0x20, kSize = 0x28, kNumTracks = 0xD0, kTrackIDs = 0x250,
  kTrackIDList = 0x300, kMaxTracks = 64,
};

// An audio track of a movie: its stream, decoder and mixer voice.
struct Track {
  uint32_t id = 0;
  int stream = -1;
  AVCodecContext* dec = nullptr;
  std::unique_ptr<audio::Voice> voice;
  audio::QueueSource* queue = nullptr;
  float volume = 1.0f;
};

struct Movie {
  uint32_t guest = 0;
  std::vector<uint8_t> data;
  int64_t pos = 0;
  AVFormatContext* fmt = nullptr;
  AVIOContext* io = nullptr;
  int vs = -1;
  AVCodecContext* vdec = nullptr;
  std::vector<Track> tracks;  // the ones playing
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

  ~Movie() {
    {
      std::lock_guard<std::mutex> l(audio::GetMixer().mu());
      for (Track& t : tracks)
        if (t.voice) audio::GetMixer().Remove(t.voice.get());
    }
    for (Track& t : tracks) avcodec_free_context(&t.dec);
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&vdec);
    avformat_close_input(&fmt);
    if (io) {
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
  }
};

std::mutex g_mu;
std::map<uint32_t, std::unique_ptr<Movie>> g_movies;  // by BINK guest ptr
// BinkSetSoundTrack: the older form's track index (~0: no sound), or the
// 1.8 form's track IDs, used with BINKSNDTRACK.
uint32_t g_sound_track = 0;
std::vector<uint32_t> g_track_ids;
// BinkSetSoundSystem: none, Miles or DirectSound.
enum class SoundSystem { None, Miles, DirectSound } g_sound_system = SoundSystem::None;

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

void QueueAudio(Movie& m, Track& t, AVFrame* f) {
  int out_ch = std::min(f->ch_layout.nb_channels, 2);
  if (!t.voice) {
    auto q = std::make_unique<audio::QueueSource>(out_ch, f->sample_rate);
    t.queue = q.get();
    t.voice = std::make_unique<audio::Voice>();
    t.voice->src = std::move(q);
    // Under DirectSound, Bink's buffers play at their volume, centred.
    t.voice->dsound = g_sound_system == SoundSystem::DirectSound;
    std::lock_guard<std::mutex> l(audio::GetMixer().mu());
    t.voice->volume = t.volume;
    t.voice->state = m.paused ? audio::Voice::State::Paused
                              : audio::Voice::State::Playing;
    audio::GetMixer().Add(t.voice.get());
  }
  if (out_ch != t.queue->channels()) return;
  std::vector<float> buf(size_t(f->nb_samples) * out_ch);
  audio::ConvertFrame(f, out_ch, buf.data());
  t.queue->Push(buf.data(), f->nb_samples);
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
    } else {
      for (Track& t : m.tracks) {
        if (m.pkt->stream_index != t.stream || !t.dec) continue;
        if (avcodec_send_packet(t.dec, m.pkt) == 0) {
          while (avcodec_receive_frame(t.dec, m.frame) == 0) {
            QueueAudio(m, t, m.frame);
            av_frame_unref(m.frame);
          }
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

// BinkSetSoundSystem(open, param): `open` is the address of BinkOpenMiles
// or BinkOpenDirectSound (resolved through the import table).
HLE_NAMED("binkw32.dll", "_BinkSetSoundSystem@8", BinkSetSoundSystem, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t open = Arg(c, 0);
  g_sound_system = !open ? SoundSystem::None
                   : open == ThunkFor("binkw32.dll", "_BinkOpenDirectSound@4")
                       ? SoundSystem::DirectSound
                       : SoundSystem::Miles;
  return 1;
}

// BinkSetSoundTrack(track): the one track ID BINKSNDTRACK opens (~0: no
// sound at all).
HLE_NAMED("binkw32.dll", "_BinkSetSoundTrack@4", BinkSetSoundTrack, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  g_sound_track = Arg(c, 0);
  g_track_ids.assign(1, Arg(c, 0));
  return 0;
}

// BinkSetSoundTrack(total_tracks, tracks): the IDs BINKSNDTRACK opens.
HLE_NAMED("binkw32.dll", "_BinkSetSoundTrack@8", BinkSetSoundTrack8, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t n = std::min<uint32_t>(Arg(c, 0), kMaxTracks), p = Arg(c, 1);
  g_track_ids.clear();
  for (uint32_t i = 0; p && i < n; i++) g_track_ids.push_back(Rd32(p + 4 * i));
  return 0;
}

// Games pass these functions' addresses to BinkSetSoundSystem; Bink would
// call them to attach to Miles or DirectSound. Our Bink talks to the mixer
// directly, so they are never called.
HLE_NAMED("binkw32.dll", "_BinkOpenMiles@4", BinkOpenMiles, 1) {
  FatalF("binkw32: BinkOpenMiles called directly");
}
HLE_NAMED("binkw32.dll", "_BinkOpenDirectSound@4", BinkOpenDirectSound, 1) {
  FatalF("binkw32: BinkOpenDirectSound called directly");
}

// The audio track IDs of a Bink file (header: track count at 0x28, from
// revision 'k' on 4 more bytes, then per track 4 bytes, 4 bytes of rate and
// flags, then the IDs).
std::vector<uint32_t> TrackIds(const std::vector<uint8_t>& d) {
  std::vector<uint32_t> ids;
  if (d.size() < 44) return ids;
  uint32_t n = std::min<uint32_t>(Le32(d.data() + 40), kMaxTracks);
  size_t at = 44 + 8 * size_t(n) + (d[3] >= 'k' ? 4 : 0);
  for (uint32_t i = 0; i < n && at + 4 * (i + 1) <= d.size(); i++)
    ids.push_back(Le32(d.data() + at + 4 * i));
  return ids;
}

// Opens decoders for the tracks that play.
void ChooseTracks(Movie& m, uint32_t flags, const std::vector<uint32_t>& ids) {
  bool sound;
  std::vector<uint32_t> want;
  {
    std::lock_guard<std::mutex> l(g_mu);
    switch (g_sound_system) {
      case SoundSystem::None: sound = false; break;
      case SoundSystem::Miles: sound = MilesDriverOpen(); break;
      default: sound = audio::GetMixer().rate() > 0; break;
    }
    if (g_sound_track == 0xFFFFFFFF) sound = false;
    if (flags & kBinkSndTrack) want = g_track_ids;
  }
  if (!sound) return;
  std::vector<int> streams;
  for (unsigned i = 0; i < m.fmt->nb_streams; i++)
    if (m.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) streams.push_back(int(i));
  if (streams.empty()) return;
  // Without BINKSNDTRACK, the first track.
  if (!(flags & kBinkSndTrack)) want = {ids.empty() ? 0u : ids[0]};
  for (uint32_t id : want) {
    for (size_t k = 0; k < streams.size(); k++) {
      if ((k < ids.size() ? ids[k] : uint32_t(k)) != id) continue;
      Track t;
      t.id = id;
      t.stream = streams[k];
      t.dec = OpenDecoder(m.fmt->streams[t.stream]);
      if (t.dec) m.tracks.push_back(std::move(t));
    }
  }
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
  std::vector<uint32_t> ids = TrackIds(m->data);
  ChooseTracks(*m, flags, ids);
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
  Wr32(m->guest + kNumTracks, uint32_t(ids.size()));
  Wr32(m->guest + kTrackIDs, m->guest + kTrackIDList);
  for (size_t i = 0; i < ids.size(); i++) Wr32(m->guest + kTrackIDList + 4 * uint32_t(i), ids[i]);
  SyncGuest(*m);
  WarnF("binkw32: BinkOpen(%s, %08X): %ux%u %s, %u frames at %.3f fps, %zu audio tracks, "
        "%zu playing\n",
        what.c_str(), flags, width, height, av_get_pix_fmt_name(m->vdec->pix_fmt), m->frames,
        m->fps, ids.size(), m->tracks.size());
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
    std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
    for (Track& t : m->tracks)
      if (t.voice) t.voice->state = p ? audio::Voice::State::Paused : audio::Voice::State::Playing;
  }
  return p;
}

// Copies the source rectangle sx, sy, sw x sh of the current frame to
// dest (pitch, height rows) at dx, dy, in the BINKSURFACE* format in the
// low bits of flags.
void CopyFrame(const Movie& m, uint32_t dest, uint32_t pitch, uint32_t height, uint32_t dx,
               uint32_t dy, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t flags) {
  uint32_t type = flags & 0xF;
  int bpp;
  switch (type) {
    case 1: case 2: bpp = 3; break;
    case 3: case 4: case 5: case 6: bpp = 4; break;
    case 7: case 8: case 9: case 10: bpp = 2; break;
    default:
      FatalF("binkw32: BinkCopyToBuffer: surface type %u not implemented", type);
  }
  sx = std::min<uint32_t>(sx, uint32_t(m.w));
  sy = std::min<uint32_t>(sy, uint32_t(m.h));
  sw = std::min<uint32_t>(sw, uint32_t(m.w) - sx);
  uint32_t rows = std::min<uint32_t>({sh, uint32_t(m.h) - sy, height > dy ? height - dy : 0});
  for (uint32_t y = 0; y < rows; y++) {
    const uint8_t* s = m.bgra.data() + (size_t(sy + y) * m.w + sx) * 4;
    uint8_t* d = GPtr(dest + (dy + y) * pitch + dx * bpp);
    for (uint32_t x = 0; x < sw; x++, s += 4) {
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
}

// BinkCopyToBuffer(HBINK, dest, destpitch, destheight, destx, desty, flags)
HLE_NAMED("binkw32.dll", "_BinkCopyToBuffer@28", BinkCopyToBuffer, 7) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkCopyToBuffer");
  if (!m || m->bgra.empty()) return 0;
  CopyFrame(*m, Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5), 0, 0, uint32_t(m->w),
            uint32_t(m->h), Arg(c, 6));
  return 0;
}

// BinkCopyToBufferRect(HBINK, dest, destpitch, destheight, destx, desty,
// srcx, srcy, srcw, srch, flags)
HLE_NAMED("binkw32.dll", "_BinkCopyToBufferRect@44", BinkCopyToBufferRect, 11) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkCopyToBufferRect");
  if (!m || m->bgra.empty()) return 0;
  CopyFrame(*m, Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), Arg(c, 5), Arg(c, 6), Arg(c, 7),
            Arg(c, 8), Arg(c, 9), Arg(c, 10));
  return 0;
}

// BinkSetVolume(HBINK, trackid, volume): 32768 is full volume (Bink
// allows up to 65536).
HLE_NAMED("binkw32.dll", "_BinkSetVolume@12", BinkSetVolume, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkSetVolume");
  if (!m) return 0;
  float v = float(std::min<uint32_t>(Arg(c, 2), 65536)) / 32768.0f;
  std::lock_guard<std::mutex> ml(audio::GetMixer().mu());
  for (Track& t : m->tracks) {
    if (t.id != Arg(c, 1)) continue;
    t.volume = v;
    if (t.voice) t.voice->volume = v;
  }
  return 0;
}

// BinkGetRealtime(HBINK, BINKREALTIME*, frames): the frame position and
// rate; the timing statistics are zero.
HLE_NAMED("binkw32.dll", "_BinkGetRealtime@12", BinkGetRealtime, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkGetRealtime");
  uint32_t rt = Arg(c, 1);
  if (!m || !rt) return 0;
  std::memset(GPtr(rt), 0, 14 * 4);
  Wr32(rt + 0, m->frame_num);
  Wr32(rt + 4, Rd32(m->guest + kFrameRate));
  Wr32(rt + 8, Rd32(m->guest + kFrameRateDiv));
  Wr32(rt + 12, Arg(c, 2));
  return 0;
}

// BinkGoto(HBINK, frame, flags): back to the start by seeking, forward by
// decoding; playback time continues from the new frame.
HLE_NAMED("binkw32.dll", "_BinkGoto@12", BinkGoto, 3) {
  std::lock_guard<std::mutex> l(g_mu);
  Movie* m = Find(Arg(c, 0), "BinkGoto");
  if (!m) return 0;
  uint32_t target = std::clamp<uint32_t>(Arg(c, 1), 1, std::max(m->frames, 1u));
  if (target < m->decoded) {
    if (av_seek_frame(m->fmt, m->vs, 0, AVSEEK_FLAG_BACKWARD) < 0)
      FatalF("binkw32: BinkGoto(%u): cannot rewind the movie", target);
    avcodec_flush_buffers(m->vdec);
    for (Track& t : m->tracks) {
      avcodec_flush_buffers(t.dec);
      if (t.queue) t.queue->Clear();
    }
    m->decoded = 0;
    m->video_eof = false;
  }
  while (m->decoded < target)
    if (!DecodeNextFrame(*m)) break;
  m->frame_num = target;
  m->start = Clock::now() - std::chrono::duration_cast<Clock::duration>(
                                std::chrono::duration<double>((target - 1) / m->fps));
  SyncGuest(*m);
  return 0;
}

}  // namespace
}  // namespace recomp::hle
