#pragma once
// The audio coprocessor's decoders (libSceAjm's codecs, which libSceNgs2's
// ATRAC9 voices use too), on the host layer's ffmpeg: a packet in,
// planar float samples out.

#include <cstddef>
#include <cstdint>
#include <memory>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace recomp::orbis {

class AudioDecoder {
 public:
  // ATRAC9 from its 4 config bytes (0xFE first, as a big-endian word);
  // null if ffmpeg refuses the config.
  static std::unique_ptr<AudioDecoder> Atrac9(uint32_t config);
  // MPEG-1/2/2.5 Layer III, a frame per packet.
  static std::unique_ptr<AudioDecoder> Mp3();
  ~AudioDecoder();

  // Decodes one packet (an ATRAC9 superframe, an MP3 frame); false if it
  // does not decode.
  bool Decode(const uint8_t* data, size_t size);
  // Forgets the stream (the next packet starts a new one).
  void Flush();

  // The last packet's samples per channel and its channels.
  uint32_t samples() const;
  uint32_t channels() const;
  const float* channel(uint32_t c) const;

 private:
  AudioDecoder() = default;
  AVCodecContext* ctx_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
};

}  // namespace recomp::orbis
