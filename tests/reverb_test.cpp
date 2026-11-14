// reverb_test: the EAX environment reverb's feedback network stays stable
// at its parameters' limits. A DC burst through the longest LF decay with
// the shortest lines (a small room, short decay; no diffusion, so each
// line feeds only itself) must keep dying away instead of growing (at an
// LF ratio of 2 the decay at DC is very long, but it is a decay).

#include <cmath>
#include <cstdio>
#include <vector>

#include "runtime/audio/reverb.h"

using namespace recomp::audio;

int main() {
  constexpr int kRate = 44100, kChunk = 1024;
  int failures = 0;
  constexpr float kDecays[] = {0.2f, 0.3f, 0.5f, 1.0f, 3.0f};
  for (int k = 0; k < 10; k++) {
    const float size = k < 5 ? 0.3f : 1.0f, decay = kDecays[k % 5];
    Reverb rv;
    rv.Open(kRate);
    ReverbParams p;
    p.size = size;
    p.decay_time = decay;
    p.decay_lf_ratio = 2.0f;
    p.diffusion = 0.0f;
    rv.Set(0, p);
    std::vector<float> send(2 * kChunk), out(2 * kChunk);
    const float* sends[1] = {send.data()};
    float mid = 0, late = 0;
    for (int chunk = 0; chunk < 40 * kRate / kChunk; chunk++) {
      std::fill(send.begin(), send.end(), chunk < 4 ? 1.0f : 0.0f);
      std::fill(out.begin(), out.end(), 0.0f);
      rv.Render(sends, out.data(), kChunk);
      float peak = 0;
      for (float v : out) peak = std::max(peak, std::fabs(v));
      if (!std::isfinite(peak)) peak = INFINITY;
      if (chunk * kChunk >= 10 * kRate && chunk * kChunk < 20 * kRate) mid = std::max(mid, peak);
      if (chunk * kChunk >= 30 * kRate) late = std::max(late, peak);
    }
    if (!(late == 0 || late < 0.9f * mid)) {
      std::fprintf(stderr, "size %.1f m, decay %.1f s: peak %g from 10 to 20 s, %g from 30 to 40 s\n",
                   double(size), double(decay), double(mid), double(late));
      failures++;
    }
  }
  if (failures) return 1;
  std::printf("reverb: ok\n");
  return 0;
}
