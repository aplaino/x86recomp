#pragma once
// The audio coprocessor's decoders (libSceAjm's codecs, which libSceNgs2's
// ATRAC9 voices use too): ATRAC9 on LibAtrac9, MP3 on the host layer's
// ffmpeg. A packet or frame in, planar float samples out.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace recomp::orbis {

class AudioDecoder {
 public:
  // ATRAC9 from its 4 config bytes (0xFE first, as a big-endian word);
  // null if the config is not valid.
  static std::unique_ptr<AudioDecoder> Atrac9(uint32_t config);
  // MPEG-1/2/2.5 Layer III, a frame per packet.
  static std::unique_ptr<AudioDecoder> Mp3();
  ~AudioDecoder();

  // Decodes one packet (an ATRAC9 superframe, an MP3 frame); false if it
  // does not decode.
  bool Decode(const uint8_t* data, size_t size);
  // ATRAC9: decodes the one frame at data (size bytes readable); returns
  // the bytes it used (byte-aligned), more than size if the frame does
  // not end in them, 0 if it does not decode.
  size_t DecodeFrame(const uint8_t* data, size_t size);
  // Forgets the stream (the next packet starts a new one).
  void Flush();

  // The last packet's samples per channel and its channels.
  uint32_t samples() const;
  uint32_t channels() const;
  const float* channel(uint32_t c) const;

 private:
  AudioDecoder() = default;
  // ATRAC9
  void* at9_ = nullptr;
  uint8_t at9_config_[4] = {};
  uint32_t at9_channels_ = 0, at9_samples_ = 0, at9_superframe_ = 0, at9_frames_ = 0;
  uint32_t at9_index_ = 0;  // the frame DecodeFrame decoded last
  uint32_t samples_ = 0;
  std::vector<float> pcm_;  // channel-major: at9_frames_ * at9_samples_ per channel
  std::vector<uint8_t> scratch_;
  // MP3
  AVCodecContext* ctx_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
};

}  // namespace recomp::orbis
