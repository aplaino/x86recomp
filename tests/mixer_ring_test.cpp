// FrameRing (runtime/audio/ring.h): the ring the mixer thread fills a
// period at a time and the device callback drains, without locks. Capacity
// rounds up to a power of two, a full ring leaves the producer nothing,
// reads past the slot wrap keep their order, and one thread writing while
// another reads neither loses, repeats nor reorders a frame.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "runtime/audio/ring.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

// Frame k carries k (exactly representable: the test stops below 2^23).
void Put(std::vector<float>& buf, int64_t k, int n) {
  for (int i = 0; i < n; i++) buf[2 * size_t(i)] = buf[2 * size_t(i) + 1] = float(k + i);
}

bool Got(const std::vector<float>& buf, int i, int64_t k) {
  return buf[2 * size_t(i)] == float(k) && buf[2 * size_t(i) + 1] == float(k);
}

void Basics() {
  recomp::audio::FrameRing r;
  r.Reset(1000);
  Check(r.Capacity() == 1024, "capacity rounds up to a power of two");
  Check(r.Free() == 1024 && r.Filled() == 0, "an empty ring is all free");

  std::vector<float> in(2 * 1024), out(2 * 1024);
  Put(in, 0, 1024);
  r.Write(in.data(), 1024);
  Check(r.Free() == 0 && r.Filled() == 1024, "a full ring is all filled");
  // A reader asking for more than it holds gets what there is.
  Check(r.Read(out.data(), 4096) == 1024, "a read stops at the frames there");
  Check(Got(out, 0, 0) && Got(out, 1023, 1023), "the frames come back in order");
  Check(r.Free() == 1024 && r.Filled() == 0, "a drained ring is all free");
  Check(r.Read(out.data(), 64) == 0, "an empty ring gives the reader nothing");

  // Nothing is written unless there is room: the producer checks Free().
  Put(in, 0, 512);
  r.Write(in.data(), 512);
  Check(r.Filled() == 512, "a partial fill is counted");
  Check(r.Read(out.data(), 100) == 100 && Got(out, 99, 99), "a partial read takes the head");
  Check(r.Filled() == 412, "a partial read leaves the rest");
}

// The slot wrap: write until the write counter passes the end of the ring,
// read until the read counter follows it, and the frames after the wrap
// are the ones written there.
void Wrap() {
  recomp::audio::FrameRing r;
  r.Reset(8);
  std::vector<float> in(2 * 16), out(2 * 16);
  Put(in, 0, 6);
  r.Write(in.data(), 6);          // slots 0..5
  Check(r.Read(out.data(), 4) == 4 && Got(out, 3, 3), "the first four");
  Put(in, 6, 6);
  r.Write(in.data(), 6);          // slots 6,7 and 0..3 (wrapped)
  Check(r.Filled() == 8, "the ring fills across the wrap");
  Check(r.Read(out.data(), 8) == 8, "all eight after the wrap");
  for (int i = 0; i < 8; i++) Check(Got(out, i, 4 + i), "frames across the wrap keep their order");
}

// One producer, one consumer: frames 1..63 and the mixer's own sizes, with
// the consumer's reads landing on the producer's boundaries.
void Threads(int capacity, int period, int request, int64_t total) {
  recomp::audio::FrameRing r;
  r.Reset(capacity);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  int64_t produced = 0, consumed = 0;
  std::atomic<int64_t> bad{0};

  std::thread consumer([&] {
    std::vector<float> buf(2 * size_t(request));
    while (consumed < total) {
      const int got = r.Read(buf.data(), request);
      if (got <= 0) {
        if (std::chrono::steady_clock::now() > deadline) return;
        std::this_thread::yield();
        continue;
      }
      for (int i = 0; i < got; i++)
        if (!Got(buf, i, consumed + i)) bad.fetch_add(1, std::memory_order_relaxed);
      consumed += got;
    }
  });
  std::thread producer([&] {
    std::vector<float> buf(2 * size_t(period));
    while (produced < total) {
      const int free = r.Free();
      if (free <= 0) {
        if (std::chrono::steady_clock::now() > deadline) return;
        std::this_thread::yield();
        continue;
      }
      // A period, or what fits: the mixer skips a period the ring has no
      // room for.
      const int n = int(std::min<int64_t>(std::min(period, free), total - produced));
      Put(buf, produced, n);
      r.Write(buf.data(), n);
      produced += n;
    }
  });
  producer.join();
  consumer.join();
  std::printf("ring %d of %d frames: %lld frames through %d-frame periods and %d-frame reads, "
              "%lld out of order\n",
              r.Capacity(), capacity, (long long)consumed, period, request, (long long)bad.load());
  Check(produced == total && consumed == total, "every frame went through");
  Check(bad.load() == 0, "no frame out of order");
  Check(r.Filled() == 0, "the ring ends empty");
}

}  // namespace

int main() {
  Basics();
  Wrap();
  Threads(256, 8, 8, 1 << 20);      // slots wrapped many times
  Threads(2048, 441, 941, 1 << 21);  // 44.1 kHz: a 10 ms period, a device request
  Threads(2048, 480, 1024, 1 << 21);  // 48 kHz without conversion
  if (g_failures) return 1;
  std::printf("ok\n");
  return 0;
}
