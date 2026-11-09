// wmvcore_test: the synchronous reader (hle/wmvcore.cpp) driven the way
// a 32-bit guest drives it, through its import thunk and COM vtables, on
// the MP3 files in tests/wmvcore (encoded from the signals Reference
// computes): the profile, the output format, the "Duration" attribute,
// samples in order whose times and durations add up, the decoded PCM
// against the signal (each channel in its place), a seek checked by what
// it plays, two readers on one file on two threads at once, a mono file
// given as stereo, a variable bitrate file without a length header, and
// files that cannot be opened.
//
//   stereo.mp3  44.1 kHz, 4 s: left 440 Hz, right a sweep from 200 Hz
//               (unique at every instant: a misplaced seek shows)
//   mono.mp3    48 kHz, 3 s: 660 Hz
//   vbr.mp3     44.1 kHz stereo, variable bitrate, no Xing/LAME header:
//               2 s of silence, then 3 s of tones (ffmpeg's estimate from
//               the first frames' bitrate is half as long again)

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/runtime.h"

using namespace recomp;

namespace {

std::atomic<int> g_failures{0};
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

constexpr uint32_t S_OK = 0, NS_E_FILE_NOT_FOUND = 0xC00D001A,
                   NS_E_INVALID_REQUEST = 0xC00D002B, NS_E_NO_MORE_SAMPLES = 0xC00D0BCF;
constexpr uint32_t kReturn = 0x00401000;  // never run: the handlers return here
constexpr double kPi = 3.14159265358979323846;
// Relative RMS difference between the decoded PCM and the signal: the
// encoding leaves about 0.05; one frame off is 0.08 for 440 Hz, more for
// the sweep; the wrong channel, or a wrong place, about 1.4.
constexpr double kTolerance = 0.065;
// What the first MP3 frame after a seek decodes to differs (the bit
// reservoir of the frame before is missing): left out of the comparison.
constexpr int64_t kAfterSeek = 2 * 1152;

// Vtable slots.
enum { kQI = 0, kRelease = 2 };
enum { kOpen = 3, kClose, kSetRange, kGetNextSample = 7, kSetStreamsSelected,
       kSetReadStreamSamples = 10, kGetOutputProps = 15, kGetOutputNumberForStream = 19 };
enum { kGetStreamCount = 8, kGetStream };                    // IWMProfile
enum { kGetStreamType = 3, kGetStreamNumber };               // IWMStreamConfig
enum { kGetMediaType = 4 };                                  // IWMOutputMediaProps
enum { kGetAttributeIndices = 18, kGetAttributeByIndexEx };  // IWMHeaderInfo3
enum { kGetBufferAndLength = 7 };                            // INSSBuffer

// A guest thread's registers, stack and memory for arguments. Guest
// addresses are 32-bit.
struct Guest {
  Ctx ctx{};
  uint32_t stack_top = 0, scratch = 0, next = 0;
};
thread_local Guest* t_guest = nullptr;

Guest* NewGuest() {
  auto* g = new Guest;
  uint32_t stack = uint32_t(g_proc->vm.Reserve(0, 0x100000));
  g_proc->vm.Commit(stack, 0x100000, Prot::RW);
  g->stack_top = stack + 0x100000 - 0x100;
  g->scratch = uint32_t(g_proc->vm.Reserve(0, 0x1000000));
  g_proc->vm.Commit(g->scratch, 0x1000000, Prot::RW);
  g->next = g->scratch;
  return g;
}

uint32_t Alloc(uint32_t size) {
  uint32_t p = t_guest->next;
  t_guest->next += (size + 15) & ~15u;
  if (t_guest->next > t_guest->scratch + 0x1000000) std::abort();
  std::memset(GPtr(p), 0, size);
  return p;
}

// A stdcall to `fn` with `args`, as guest code makes it.
uint32_t Call(uint32_t fn, const std::vector<uint32_t>& args) {
  Guest& g = *t_guest;
  uint32_t sp = g.stack_top - 4 * uint32_t(args.size());
  for (size_t i = 0; i < args.size(); i++) Wr32(sp + 4 * uint32_t(i), args[i]);
  sp -= 4;
  Wr32(sp, kReturn);
  g.ctx.esp = sp;
  hle::CallThunk(g.ctx, fn);
  CHECK(g.ctx.esp == g.stack_top);  // all arguments popped
  return g.ctx.eax;
}

// Method `slot` of the interface pointer `obj`.
uint32_t Method(uint32_t obj, int slot, std::initializer_list<uint32_t> args) {
  std::vector<uint32_t> all{obj};
  all.insert(all.end(), args);
  return Call(Rd32(Rd32(obj) + 4 * uint32_t(slot)), all);
}

uint32_t WStr(const std::u16string& s) {
  uint32_t p = Alloc(uint32_t(s.size() * 2 + 2));
  std::memcpy(GPtr(p), s.data(), s.size() * 2);
  return p;
}

uint32_t Guid(uint32_t d1, uint16_t d2, uint16_t d3, std::initializer_list<uint8_t> d4) {
  uint32_t p = Alloc(16);
  Wr32(p, d1);
  Wr16(p + 4, d2);
  Wr16(p + 6, d3);
  int i = 0;
  for (uint8_t b : d4) Wr8(p + 8 + i++, b);
  return p;
}

uint32_t NewReader() {
  static const uint32_t create =
      uint32_t(hle::ImportThunk("wmvcore.dll", "WMCreateSyncReader"));
  uint32_t out = Alloc(4);
  CHECK(Call(create, {0, 1, out}) == S_OK);
  return Rd32(out);
}

uint32_t OpenReader(const char16_t* name) {
  uint32_t r = NewReader();
  CHECK(Method(r, kOpen, {WStr(std::u16string(u"C:\\Test\\") + name)}) == S_OK);
  return r;
}

struct Format {
  uint32_t channels = 0, rate = 0, bits = 0, block = 0;
  double seconds = 0;  // the "Duration" attribute
};

// The reader's profile, output format and duration, as a guest reads them.
Format ReadHeaders(uint32_t reader) {
  uint32_t out = Alloc(4), out2 = Alloc(4);
  uint32_t iid_profile = Guid(0x96406BDB, 0x2B2B, 0x11D3, {0xB3, 0x6B, 0x00, 0xC0, 0x4F, 0x61, 0x08, 0xFF});
  CHECK(Method(reader, kQI, {iid_profile, out}) == S_OK);
  uint32_t profile = Rd32(out);
  CHECK(Method(profile, kGetStreamCount, {out2}) == S_OK && Rd32(out2) == 1);
  CHECK(Method(profile, kGetStream, {0, out}) == S_OK);
  uint32_t cfg = Rd32(out);
  uint32_t num = Alloc(4), guid = Alloc(16);
  CHECK(Method(cfg, kGetStreamNumber, {num}) == S_OK && Rd16(num) == 1);
  CHECK(Method(cfg, kGetStreamType, {guid}) == S_OK && Rd32(guid) == 0x73647561);
  CHECK(Method(cfg, kRelease, {}) == 0);
  CHECK(Method(profile, kRelease, {}) == 0);

  uint32_t sel = Alloc(4);
  Wr16(num, 1);
  Wr32(sel, 2);  // WMT_ON
  CHECK(Method(reader, kSetStreamsSelected, {1, num, sel}) == S_OK);
  CHECK(Method(reader, kSetReadStreamSamples, {1, 0}) == S_OK);
  CHECK(Method(reader, kSetRange, {0, 0, 0, 0}) == S_OK);
  CHECK(Method(reader, kGetOutputNumberForStream, {1, out2}) == S_OK && Rd32(out2) == 0);
  CHECK(Method(reader, kGetOutputProps, {0, out}) == S_OK);
  uint32_t props = Rd32(out);
  uint32_t size = Alloc(4);
  CHECK(Method(props, kGetMediaType, {0, size}) == S_OK);
  CHECK(Rd32(size) == 0x48 + 18);
  uint32_t mt = Alloc(Rd32(size));
  CHECK(Method(props, kGetMediaType, {mt, size}) == S_OK);
  CHECK(Rd32(mt) == 0x73647561);         // MEDIATYPE_Audio
  CHECK(Rd32(mt + 0x10) == 1);           // MEDIASUBTYPE_PCM
  CHECK(Rd32(mt + 0x2C) == 0x05589F81);  // FORMAT_WaveFormatEx
  CHECK(Rd32(mt + 0x40) == 18);
  CHECK(Rd32(mt + 0x44) == mt + 0x48);
  uint32_t wf = Rd32(mt + 0x44);
  Format f{Rd16(wf + 2), Rd32(wf + 4), Rd16(wf + 14), Rd16(wf + 12)};
  CHECK(Rd16(wf) == 1);  // WAVE_FORMAT_PCM
  CHECK(Rd32(wf + 8) == f.rate * f.block);
  CHECK(Method(props, kRelease, {}) == 0);

  uint32_t iid_hdr = Guid(0x15CC68E3, 0x27CC, 0x4ECD, {0xB2, 0x22, 0x3F, 0x5D, 0x02, 0xD8, 0x0B, 0xD5});
  CHECK(Method(reader, kQI, {iid_hdr, out}) == S_OK);
  uint32_t hdr = Rd32(out);
  uint32_t name = WStr(u"Duration"), count = Alloc(4);
  CHECK(Method(hdr, kGetAttributeIndices, {0, name, 0, 0, count}) == S_OK && Rd16(count) == 1);
  uint32_t indices = Alloc(2 * Rd16(count));
  Wr16(indices, 0xFFFF);
  CHECK(Method(hdr, kGetAttributeIndices, {0, name, 0, indices, count}) == S_OK);
  uint32_t name_len = Alloc(4), type = Alloc(4), value = Alloc(8), len = Alloc(4);
  Wr32(len, 8);
  CHECK(Method(hdr, kGetAttributeByIndexEx,
               {0, Rd16(indices), 0, name_len, type, 0, value, len}) == S_OK);
  CHECK(Rd32(type) == 4);  // WMT_TYPE_QWORD
  CHECK(Rd16(name_len) == 9);
  f.seconds = double(Rd64(value)) / 1e7;
  CHECK(Method(hdr, kRelease, {}) == 0);
  CHECK(f.channels == 2 && f.bits == 16 && f.block == 4);
  return f;
}

// Every sample from the reader's position on, checked against the ones
// before: times that follow on, durations that add up, the
// discontinuity flag on the first. `start` is the first one's time.
std::vector<int16_t> PullAll(uint32_t reader, const Format& f, uint64_t start) {
  std::vector<int16_t> pcm;
  uint64_t frames = 0, next_time = start;
  const int64_t first_frame = int64_t(start) * f.rate / 10000000;
  const uint32_t mark = t_guest->next;
  for (;;) {
    t_guest->next = mark;
    uint32_t sample = Alloc(4), time = Alloc(8), dur = Alloc(8), flags = Alloc(4),
             output = Alloc(4), stream = Alloc(4);
    uint32_t hr = Method(reader, kGetNextSample, {1, sample, time, dur, flags, output, stream});
    if (hr == NS_E_NO_MORE_SAMPLES) {
      CHECK(Rd32(sample) == 0);
      break;
    }
    CHECK(hr == S_OK);
    if (hr != S_OK) break;
    CHECK(Rd16(stream) == 1 && Rd32(output) == 0);
    CHECK(Rd64(time) == next_time);
    CHECK(((Rd32(flags) & 2) != 0) == (frames == 0));  // discontinuity first
    uint32_t buf = Rd32(sample);
    uint32_t data = Alloc(4), length = Alloc(4);
    CHECK(Method(buf, kGetBufferAndLength, {data, length}) == S_OK);
    const uint32_t bytes = Rd32(length);
    CHECK(bytes > 0 && bytes < 0x20000 && bytes % f.block == 0);
    const auto* p = reinterpret_cast<const int16_t*>(GPtr(Rd32(data)));
    pcm.insert(pcm.end(), p, p + bytes / 2);
    const uint64_t n = bytes / f.block, at = uint64_t(first_frame) + frames;
    CHECK(Rd64(dur) == (at + n) * 10000000 / f.rate - at * 10000000 / f.rate);
    CHECK(Method(buf, kRelease, {}) == 0);
    frames += n;
    next_time = Rd64(time) + Rd64(dur);
  }
  t_guest->next = mark;
  // The end stays the end.
  uint32_t s = Alloc(4), t = Alloc(8), d = Alloc(8), fl = Alloc(4), o = Alloc(4), n = Alloc(4);
  CHECK(Method(reader, kGetNextSample, {1, s, t, d, fl, o, n}) == NS_E_NO_MORE_SAMPLES);
  t_guest->next = mark;
  return pcm;
}

// The signal a fixture was encoded from, channel c at time t (full scale 1).
using Signal = std::function<double(int c, double t)>;
double StereoSignal(int c, double t) {
  return c == 0 ? 0.5 * std::sin(2 * kPi * 440 * t)
                : 0.5 * std::sin(2 * kPi * (200 * t + 150 * t * t));
}
double MonoSignal(int, double t) { return 0.5 * std::sin(2 * kPi * 660 * t); }

// Relative RMS difference between channel c of `pcm` (whose frame 0 is
// at frame `first` of the file) and channel sc of the signal, over the
// frames [from, size - 2000) of pcm.
double Difference(const std::vector<int16_t>& pcm, int c, const Signal& signal, int sc,
                  int rate, int64_t first, int64_t from) {
  double err = 0, ref = 0;
  const int64_t frames = int64_t(pcm.size()) / 2;
  for (int64_t i = from; i < frames - 2000; i++) {
    double r = signal(sc, double(first + i) / rate) * 32768;
    double d = pcm[size_t(2 * i + c)] - r;
    err += d * d;
    ref += r * r;
  }
  return ref > 0 ? std::sqrt(err / ref) : 1e9;
}

}  // namespace

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  if (argc < 2) {
    std::fprintf(stderr, "usage: wmvcore_test <tests/wmvcore>\n");
    return 2;
  }
  fs::path dir = fs::temp_directory_path() / ("wmvcore_test_" + std::to_string(::getpid()));
  fs::create_directories(dir / "data");
  ::setenv("XDG_DATA_HOME", (dir / "data").c_str(), 1);
  for (const char* f : {"stereo.mp3", "mono.mp3", "vbr.mp3"})
    fs::copy_file(fs::path(argv[1]) / f, dir / f);
  {
    FILE* f = std::fopen((dir / "notes.mp3").c_str(), "wb");
    std::fputs("not an audio file\n", f);
    std::fclose(f);
  }

  // A 32-bit process with its heap.
  g_proc = new Process;
  g_proc->game.id = "wmvtest";
  g_proc->game.guest_dir = "C:\\Test";
  g_proc->game.exe_name = "test.exe";
  g_proc->game_dir = dir.string();
  if (!g_proc->mem.Reserve(32)) return 1;
  g_proc->mem.Commit(kThunkBase, kThunkRegionSize, Prot::R);
  g_proc->mem.Commit(kPebVa, 0x1000, Prot::RW);
  g_proc->vm.Init(&g_proc->mem, 0x00010000, kThunkBase);
  InitKernel32State();
  t_guest = NewGuest();

  // Stereo: each channel in its place, no frame lost or added.
  uint32_t a = OpenReader(u"stereo.mp3");
  CHECK(Method(a, kOpen, {WStr(u"C:\\Test\\stereo.mp3")}) == NS_E_INVALID_REQUEST);  // open
  Format f = ReadHeaders(a);
  CHECK(f.rate == 44100);
  CHECK(std::fabs(f.seconds - 4.0) < 0.01);
  std::vector<int16_t> stereo = PullAll(a, f, 0);
  const double left = Difference(stereo, 0, StereoSignal, 0, 44100, 0, 2000),
               right = Difference(stereo, 1, StereoSignal, 1, 44100, 0, 2000),
               swapped = Difference(stereo, 0, StereoSignal, 1, 44100, 0, 2000);
  std::printf("stereo: %zu frames; difference left %.4f, right %.4f, left as right %.4f\n",
              stereo.size() / 2, left, right, swapped);
  CHECK(stereo.size() / 2 == 4 * 44100);
  CHECK(left < kTolerance && right < kTolerance && swapped > 0.5);

  // A seek: from 2.5 s on, by what it plays.
  CHECK(Method(a, kSetRange, {25000000, 0, 0, 0}) == S_OK);
  std::vector<int16_t> seek = PullAll(a, f, 25000000);
  const int64_t at = 110250;  // 2.5 s
  const double seek_left = Difference(seek, 0, StereoSignal, 0, 44100, at, kAfterSeek),
               seek_right = Difference(seek, 1, StereoSignal, 1, 44100, at, kAfterSeek);
  std::printf("seek to 2.5 s: %zu frames; difference left %.4f, right %.4f\n",
              seek.size() / 2, seek_left, seek_right);
  CHECK(int64_t(seek.size() / 2) == 4 * 44100 - at);
  CHECK(seek_left < kTolerance && seek_right < kTolerance);
  // ... and what it plays is what the whole file played there.
  bool same_tail = true;
  for (size_t i = 2 * kAfterSeek; i < seek.size(); i++)
    same_tail = same_tail && std::abs(seek[i] - stereo[2 * at + i]) <= 1;
  CHECK(same_tail);
  CHECK(Method(a, kClose, {}) == S_OK);
  CHECK(Method(a, kClose, {}) == NS_E_INVALID_REQUEST);
  CHECK(Method(a, kRelease, {}) == 0);

  // Two readers on one file, on two threads at once.
  std::vector<int16_t> results[2];
  Guest* guests[2] = {NewGuest(), NewGuest()};
  std::thread threads[2];
  for (int i = 0; i < 2; i++)
    threads[i] = std::thread([&, i] {
      t_guest = guests[i];
      uint32_t r = OpenReader(u"STEREO.MP3");  // any case
      Format tf = ReadHeaders(r);
      results[i] = PullAll(r, tf, 0);
      CHECK(Method(r, kClose, {}) == S_OK);
      CHECK(Method(r, kRelease, {}) == 0);
    });
  for (auto& t : threads) t.join();
  CHECK(results[0] == stereo && results[1] == stereo);

  // Mono: given as stereo, the same on both channels.
  uint32_t m = OpenReader(u"mono.mp3");
  Format fm = ReadHeaders(m);
  CHECK(fm.rate == 48000);
  CHECK(std::fabs(fm.seconds - 3.0) < 0.01);
  std::vector<int16_t> mono = PullAll(m, fm, 0);
  bool equal = true;
  for (size_t i = 0; i < mono.size(); i += 2) equal = equal && mono[i] == mono[i + 1];
  const double mono_diff = Difference(mono, 0, MonoSignal, 0, 48000, 0, 2000);
  std::printf("mono: %zu frames; difference %.4f\n", mono.size() / 2, mono_diff);
  CHECK(mono.size() / 2 == 3 * 48000);
  CHECK(equal && mono_diff < kTolerance);
  CHECK(Method(m, kRelease, {}) == 0);  // released open: no Close needed

  // Variable bitrate without a length header: the duration is the
  // packets', not the estimate from the bitrate.
  uint32_t v = OpenReader(u"vbr.mp3");
  Format fv = ReadHeaders(v);
  std::vector<int16_t> vbr = PullAll(v, fv, 0);
  const double decoded = double(vbr.size() / 2) / fv.rate;
  std::printf("vbr: duration %.3f s, decoded %.3f s\n", fv.seconds, decoded);
  CHECK(std::fabs(fv.seconds - decoded) < 0.03);
  CHECK(Method(v, kRelease, {}) == 0);

  // Files that cannot be opened.
  uint32_t bad = NewReader();
  CHECK(Method(bad, kOpen, {WStr(u"C:\\Test\\missing.mp3")}) == NS_E_FILE_NOT_FOUND);
  uint32_t hr = Method(bad, kOpen, {WStr(u"C:\\Test\\notes.mp3")});
  CHECK(hr != S_OK && (hr & 0x80000000u));
  CHECK(Method(bad, kClose, {}) == NS_E_INVALID_REQUEST);
  CHECK(Method(bad, kRelease, {}) == 0);

  fs::remove_all(dir);
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures.load());
    return 1;
  }
  std::printf("wmvcore_test: ok\n");
  return 0;
}
