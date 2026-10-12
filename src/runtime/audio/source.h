#pragma once
// Audio sources for the mixer: decoders that produce interleaved float
// frames on demand. WAV PCM is parsed here; MP3 goes through libavcodec.
// All data lives in host memory: HLE code copies guest file images before
// handing them over, so the mixer thread never reads guest memory.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AVFrame;

namespace recomp::audio {

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

class Source {
 public:
  virtual ~Source() = default;
  int channels() const { return channels_; }  // 1 or 2
  int rate() const { return rate_; }          // frames per second
  // Fills up to n frames; returns fewer only at the end of the data.
  virtual int Read(float* out, int n) = 0;
  // Continues from frame f. Compressed sources only support f == 0.
  virtual bool Seek(int64_t f) = 0;
  // Total frames, or -1 if unknown without decoding everything.
  virtual int64_t Length() const { return -1; }
  // Bytes of the original encoding per frame (WAV block align), for
  // Miles' byte-based positions; 0 for compressed sources.
  virtual int bytes_per_frame() const { return 0; }

 protected:
  int channels_ = 0, rate_ = 0;
};

struct WavInfo {
  uint16_t format = 0, channels = 0, bits = 0, block_align = 0;
  uint32_t rate = 0, data_offset = 0, data_len = 0;
};

// Total size of a RIFF image from its first 8 bytes, or 0 if not RIFF.
uint32_t RiffSize(const uint8_t* first8);
// Parses a RIFF WAVE image of `len` bytes.
bool ParseWav(const uint8_t* p, size_t len, WavInfo* out);

// 8/16-bit PCM WAV. Returns null and sets *err for anything else.
std::unique_ptr<Source> MakeWavSource(Bytes data, std::string* err);
// MPEG audio (MP3) through libavcodec.
std::unique_ptr<Source> MakeMp3Source(Bytes data, std::string* err);
// WAV if the data is RIFF, else MP3.
std::unique_ptr<Source> MakeSource(Bytes data, std::string* err);

// Interleaved float frames from a decoded libavcodec frame (float or s16,
// planar or packed), keeping the first `out_ch` channels.
void ConvertFrame(const AVFrame* f, int out_ch, float* out);

// A source fed from another thread (Bink audio). Reads pad with silence
// while the queue is empty, so the voice keeps playing until Finish().
class QueueSource : public Source {
 public:
  QueueSource(int channels, int rate);
  void Push(const float* frames, int n);
  void Finish();
  int Read(float* out, int n) override;
  bool Seek(int64_t f) override { return f == 0; }

 private:
  std::mutex mu_;
  std::vector<float> q_;
  size_t head_ = 0;  // index into q_ of the next sample to play
  bool finished_ = false;
};

}  // namespace recomp::audio
