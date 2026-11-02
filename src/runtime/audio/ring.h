#pragma once
// A lock-free ring of stereo float frames from the mixer thread (the only
// writer) to the device callback (the only reader): the mixer fills it a
// period at a time, the callback copies it out, and neither takes a lock
// or touches guest state. See mixer.h for why.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace recomp::audio {

// Sizes and counts are in frames (two floats), not floats or bytes.
class FrameRing {
 public:
  // Makes room for `frames` frames, rounded up to a power of two, and
  // empties it. Only while no producer or consumer runs.
  void Reset(int frames);
  int Capacity() const { return capacity_; }

  // Frames the producer may append now: the producer's side only.
  int Free() const {
    const uint32_t w = write_.load(std::memory_order_relaxed);
    const uint32_t r = read_.load(std::memory_order_acquire);
    return capacity_ - int(w - r);
  }
  // Frames the consumer may take now: the consumer's side only.
  int Filled() const {
    const uint32_t w = write_.load(std::memory_order_acquire);
    const uint32_t r = read_.load(std::memory_order_relaxed);
    return int(w - r);
  }

  // Producer: appends n frames (at most Free()).
  void Write(const float* src, int n);
  // Consumer: takes min(n, Filled()) frames into dst; returns how many.
  int Read(float* dst, int n);

 private:
  // A frame's slot: the counter masked, the capacity being a power of two
  // (no modulo, and the counters compare right across the uint32_t wrap).
  float* At(uint32_t frame) {
    return &data_[2 * size_t(frame & uint32_t(capacity_ - 1))];
  }
  const float* At(uint32_t frame) const {
    return &data_[2 * size_t(frame & uint32_t(capacity_ - 1))];
  }

  std::vector<float> data_;  // interleaved stereo
  int capacity_ = 0;
  // Written by one side each: the producer reads read_ and publishes
  // write_ (release), the consumer the other way round, so a frame is
  // whole before the other side sees it.
  std::atomic<uint32_t> read_{0}, write_{0};
};

inline void FrameRing::Reset(int frames) {
  int capacity = 1;
  while (capacity < frames) capacity *= 2;
  capacity_ = frames > 0 ? capacity : 0;
  data_.assign(size_t(2) * size_t(capacity_), 0.0f);
  read_.store(0, std::memory_order_relaxed);
  write_.store(0, std::memory_order_relaxed);
}

inline void FrameRing::Write(const float* src, int n) {
  if (!capacity_ || n <= 0) return;
  const uint32_t w = write_.load(std::memory_order_relaxed);
  const int first = std::min(n, capacity_ - int(w & uint32_t(capacity_ - 1)));
  std::memcpy(At(w), src, sizeof(float) * 2 * size_t(first));
  if (n > first)
    std::memcpy(At(w + uint32_t(first)), src + 2 * size_t(first),
                sizeof(float) * 2 * size_t(n - first));
  write_.store(w + uint32_t(n), std::memory_order_release);
}

inline int FrameRing::Read(float* dst, int n) {
  if (!capacity_ || n <= 0) return 0;
  const uint32_t r = read_.load(std::memory_order_relaxed);
  const uint32_t w = write_.load(std::memory_order_acquire);
  n = std::min(n, int(w - r));
  if (n <= 0) return 0;
  const int first = std::min(n, capacity_ - int(r & uint32_t(capacity_ - 1)));
  std::memcpy(dst, At(r), sizeof(float) * 2 * size_t(first));
  if (n > first)
    std::memcpy(dst + 2 * size_t(first), At(r + uint32_t(first)),
                sizeof(float) * 2 * size_t(n - first));
  read_.store(r + uint32_t(n), std::memory_order_release);
  return n;
}

}  // namespace recomp::audio
