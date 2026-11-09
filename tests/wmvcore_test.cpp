// wmvcore_test: the synchronous reader (hle/wmvcore.cpp) driven the way
// a 32-bit guest drives it, through its import thunk and COM vtables, on
// MP3 files written here (silent frames: their structure is what is
// checked): the profile, the output format, the "Duration" attribute,
// samples in order with times and durations that add up to the file,
// NS_E_NO_MORE_SAMPLES at the end, seeking, two readers on one file at
// once, a mono file given as stereo, and files that cannot be opened.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/runtime.h"

using namespace recomp;

namespace {

int g_failures = 0;
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

// Vtable slots.
enum { kQI = 0, kRelease = 2 };
enum Reader { kOpen = 3, kClose, kSetRange, kGetNextSample = 7, kSetStreamsSelected,
              kSetReadStreamSamples = 10, kGetOutputProps = 15,
              kGetOutputNumberForStream = 19 };
enum { kGetStreamCount = 8, kGetStream };                 // IWMProfile
enum { kGetStreamType = 3, kGetStreamNumber };            // IWMStreamConfig
enum { kGetMediaType = 4 };                               // IWMOutputMediaProps
enum { kGetAttributeIndices = 18, kGetAttributeByIndexEx };  // IWMHeaderInfo3
enum { kGetBufferAndLength = 7 };                         // INSSBuffer

Ctx g_ctx{};
uint32_t g_stack_top = 0;
uint32_t g_scratch = 0;  // guest memory for arguments
uint32_t g_next = 0;

uint32_t Alloc(uint32_t size) {
  uint32_t p = g_next;
  g_next += (size + 15) & ~15u;
  std::memset(GPtr(p), 0, size);
  return p;
}

// A stdcall to `fn` with `args`, as guest code makes it.
uint32_t Call(uint32_t fn, const std::vector<uint32_t>& args) {
  uint32_t sp = g_stack_top - 4 * uint32_t(args.size());
  for (size_t i = 0; i < args.size(); i++) Wr32(sp + 4 * uint32_t(i), args[i]);
  sp -= 4;
  Wr32(sp, kReturn);
  g_ctx.esp = sp;
  hle::CallThunk(g_ctx, fn);
  CHECK(g_ctx.esp == g_stack_top);  // all arguments popped
  return g_ctx.eax;
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

// `frames` MPEG-1 Layer III frames at 128 kbit/s with no audio data
// (silence): 44.1 kHz stereo or 48 kHz mono.
void WriteMp3(const std::filesystem::path& path, int frames, bool mono48) {
  const uint8_t header[4] = {0xFF, 0xFB, uint8_t(mono48 ? 0x94 : 0x90),
                             uint8_t(mono48 ? 0xC4 : 0x04)};
  const size_t size = mono48 ? 384 : 417;  // 144 * bitrate / rate
  std::vector<uint8_t> frame(size, 0);
  std::memcpy(frame.data(), header, 4);
  FILE* f = std::fopen(path.c_str(), "wb");
  for (int i = 0; i < frames; i++) std::fwrite(frame.data(), 1, size, f);
  std::fclose(f);
}

struct Format {
  uint32_t channels = 0, rate = 0, bits = 0, block = 0;
};

// The reader's profile, output format and duration, as a guest reads them.
Format CheckHeaders(uint32_t reader, double expect_seconds) {
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
  CHECK(Rd32(mt) == 0x73647561);       // MEDIATYPE_Audio
  CHECK(Rd32(mt + 0x10) == 1);         // MEDIASUBTYPE_PCM
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
  const double seconds = double(Rd64(value)) / 1e7;
  CHECK(seconds > expect_seconds * 0.99 && seconds < expect_seconds * 1.01);
  CHECK(Method(hdr, kRelease, {}) == 0);
  return f;
}

struct Pull {
  uint32_t reader;
  uint64_t frames = 0, samples = 0;
  uint64_t next_time = 0;
  bool done = false;
};

// One GetNextSample; checks the sample against the ones before.
void PullOne(Pull& p, const Format& f) {
  uint32_t sample = Alloc(4), time = Alloc(8), dur = Alloc(8), flags = Alloc(4),
        output = Alloc(4), stream = Alloc(4);
  uint32_t hr = Method(p.reader, kGetNextSample, {1, sample, time, dur, flags, output, stream});
  if (hr == NS_E_NO_MORE_SAMPLES) {
    CHECK(Rd32(sample) == 0);
    p.done = true;
    return;
  }
  CHECK(hr == S_OK);
  if (hr != S_OK) {
    p.done = true;
    return;
  }
  CHECK(Rd16(stream) == 1 && Rd32(output) == 0);
  CHECK(Rd64(time) == p.next_time);
  CHECK(((Rd32(flags) & 2) != 0) == (p.samples == 0));  // discontinuity first
  uint32_t buf = Rd32(sample);
  uint32_t data = Alloc(4), length = Alloc(4);
  CHECK(Method(buf, kGetBufferAndLength, {data, length}) == S_OK);
  const uint32_t bytes = Rd32(length);
  CHECK(bytes > 0 && bytes < 0x20000 && bytes % f.block == 0);
  for (uint32_t i = 0; i < bytes; i += 2) CHECK(Rd16(Rd32(data) + i) == 0);  // silence
  const uint64_t frames = bytes / f.block;
  CHECK(Rd64(dur) == (p.frames + frames) * 10000000 / f.rate - p.frames * 10000000 / f.rate);
  CHECK(Method(buf, kRelease, {}) == 0);
  p.frames += frames;
  p.samples++;
  p.next_time = Rd64(time) + Rd64(dur);
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / ("wmvcore_test_" + std::to_string(::getpid()));
  fs::create_directories(dir / "data");
  ::setenv("XDG_DATA_HOME", (dir / "data").c_str(), 1);
  const int kFrames = 1000;
  WriteMp3(dir / "stereo44.mp3", kFrames, false);
  WriteMp3(dir / "mono48.mp3", 1100, true);
  {
    FILE* f = std::fopen((dir / "notes.mp3").c_str(), "wb");
    std::fputs("not an audio file\n", f);
    std::fclose(f);
  }

  // A 32-bit process with its heap, and a stack for the calls.
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
  uint32_t stack = uint32_t(g_proc->vm.Reserve(0, 0x100000));
  g_proc->vm.Commit(stack, 0x100000, Prot::RW);
  g_stack_top = stack + 0x100000 - 0x100;
  g_scratch = uint32_t(g_proc->vm.Reserve(0, 0x1000000));
  g_proc->vm.Commit(g_scratch, 0x1000000, Prot::RW);
  g_next = g_scratch;

  const uint32_t create = uint32_t(hle::ImportThunk("wmvcore.dll", "WMCreateSyncReader"));
  uint32_t out = Alloc(4);

  // Two readers on the stereo file at once, pulled in turns.
  CHECK(Call(create, {0, 1, out}) == S_OK);
  Pull a{Rd32(out)};
  CHECK(Call(create, {0, 1, out}) == S_OK);
  Pull b{Rd32(out)};
  uint32_t stereo = WStr(u"C:\\Test\\stereo44.mp3");
  CHECK(Method(a.reader, kOpen, {stereo}) == S_OK);
  CHECK(Method(a.reader, kOpen, {stereo}) == NS_E_INVALID_REQUEST);  // open already
  CHECK(Method(b.reader, kOpen, {stereo}) == S_OK);
  const double stereo_seconds = kFrames * 1152 / 44100.0;
  Format f = CheckHeaders(a.reader, stereo_seconds);
  CHECK(f.channels == 2 && f.rate == 44100 && f.bits == 16 && f.block == 4);
  CHECK(CheckHeaders(b.reader, stereo_seconds).rate == 44100);
  while (!a.done || !b.done) {
    if (!a.done) PullOne(a, f);
    if (!b.done) PullOne(b, f);
  }
  std::printf("stereo: %llu samples, %llu frames (%d MP3 frames of 1152)\n",
              (unsigned long long)a.samples, (unsigned long long)a.frames, kFrames);
  CHECK(a.frames == b.frames && a.samples == b.samples);
  CHECK(a.frames + 2 * 1152 >= uint64_t(kFrames) * 1152 && a.frames <= uint64_t(kFrames) * 1152);
  // The end stays the end.
  uint32_t s = Alloc(4), t = Alloc(8), d = Alloc(8), fl = Alloc(4), o = Alloc(4), n = Alloc(4);
  CHECK(Method(a.reader, kGetNextSample, {1, s, t, d, fl, o, n}) == NS_E_NO_MORE_SAMPLES);

  // Seeking: the first sample is at the time asked for.
  CHECK(Method(a.reader, kSetRange, {10000000 * 10u, 0, 0, 0}) == S_OK);
  Pull seek{a.reader};
  seek.next_time = 100000000;
  PullOne(seek, f);
  CHECK(seek.samples == 1);
  CHECK(Method(a.reader, kSetRange, {0, 0, 0, 0}) == S_OK);
  Pull again{a.reader};
  while (!again.done) PullOne(again, f);
  CHECK(again.frames == a.frames);

  CHECK(Method(a.reader, kClose, {}) == S_OK);
  CHECK(Method(a.reader, kClose, {}) == NS_E_INVALID_REQUEST);
  CHECK(Method(a.reader, kRelease, {}) == 0);
  CHECK(Method(b.reader, kClose, {}) == S_OK);
  CHECK(Method(b.reader, kRelease, {}) == 0);

  // Mono 48 kHz: given as stereo.
  CHECK(Call(create, {0, 1, out}) == S_OK);
  Pull m{Rd32(out)};
  CHECK(Method(m.reader, kOpen, {WStr(u"C:\\Test\\MONO48.MP3")}) == S_OK);  // any case
  Format fm = CheckHeaders(m.reader, 1100 * 1152 / 48000.0);
  CHECK(fm.channels == 2 && fm.rate == 48000 && fm.block == 4);
  while (!m.done) PullOne(m, fm);
  CHECK(m.frames + 2 * 1152 >= 1100u * 1152 && m.frames <= 1100u * 1152);

  // Files that cannot be opened.
  CHECK(Call(create, {0, 1, out}) == S_OK);
  uint32_t bad = Rd32(out);
  CHECK(Method(bad, kOpen, {WStr(u"C:\\Test\\missing.mp3")}) == NS_E_FILE_NOT_FOUND);
  uint32_t hr = Method(bad, kOpen, {WStr(u"C:\\Test\\notes.mp3")});
  CHECK(hr != S_OK && (hr & 0x80000000u));
  CHECK(Method(bad, kClose, {}) == NS_E_INVALID_REQUEST);
  CHECK(Method(bad, kRelease, {}) == 0);
  CHECK(Method(m.reader, kRelease, {}) == 0);

  fs::remove_all(dir);
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("wmvcore_test: ok\n");
  return 0;
}
