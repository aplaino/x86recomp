// orbis_process_test's checks of libSceAjm's HLE (orbis/hle/ajm.cpp), from
// lifted code: contexts, codecs and instances, batches built with the
// library's job builders, their validation and error blocks, ATRAC9
// decoding (superframes made here: mono, 48 kHz, 4 frames of up to 64
// bytes; also behind a RIFF header, as a stream that configures an
// instance no initialisation job did),
// and the library's ATRAC9 config and MP3 frame parsers. ajm_checks()
// returns how many failed.

int test_check(int id, long got, long want);

int sceAjmInitialize(long reserved, unsigned* ctx);
int sceAjmFinalize(unsigned ctx);
int sceAjmMemoryRegister(unsigned ctx, void* addr, unsigned long pages);
int sceAjmModuleRegister(unsigned ctx, unsigned codec, long reserved);
int sceAjmModuleUnregister(unsigned ctx, unsigned codec);
int sceAjmInstanceCreate(unsigned ctx, unsigned codec, unsigned long flags, unsigned* id);
int sceAjmInstanceDestroy(unsigned ctx, unsigned id);
unsigned sceAjmInstanceCodecType(unsigned id);
int sceAjmBatchStartBuffer(unsigned ctx, const void* batch, unsigned long size, int priority,
                           void* err, unsigned* id);
int sceAjmBatchWait(unsigned ctx, unsigned id, unsigned timeout, void* err);
int sceAjmBatchCancel(unsigned ctx, unsigned id);
void* sceAjmBatchJobControlBufferRa(void* p, unsigned id, unsigned long flags, const void* sb_in,
                                    unsigned long sb_in_size, void* sb_out,
                                    unsigned long sb_out_size, void* ra);
void* sceAjmBatchJobRunBufferRa(void* p, unsigned id, unsigned long flags, const void* in,
                                unsigned long in_size, void* out, unsigned long out_size,
                                void* sb_out, unsigned long sb_out_size, void* ra);
void* sceAjmBatchJobRunSplitBufferRa(void* p, unsigned id, unsigned long flags, const void* ins,
                                     unsigned long num_ins, const void* outs,
                                     unsigned long num_outs, void* sb_out,
                                     unsigned long sb_out_size, void* ra);
void* sceAjmBatchJobInlineBuffer(void* p, const void* data, unsigned long size,
                                 const void** out);
int sceAjmDecAt9ParseConfigData(const unsigned char* config, unsigned* out);
int sceAjmDecMp3ParseFrame(const unsigned char* buf, unsigned long size, int parse_tag,
                           void* out);
const char* sceAjmStrError(int code);

enum {
  kRev = 1,
  kMultipleFrames = 1 << 12,
  kReset = 1 << 13,
  kInitialize = 1 << 14,
};
#define kGapless (1ul << 45)
#define kStream (1ul << 47)

typedef struct {
  int code, pad;
  unsigned long job;
  unsigned offset, pad2;
  unsigned long ra;
} BatchError;

static unsigned char superframes[3 * 256], riff[100 + 3 * 256];
static float pcm[4096];
static short pcm16[4096], split[2048];
static unsigned long batch[64];
static unsigned long sb[8], sb2[8];

// A control job and a run job on `id`, started at priority 50 and waited
// for; returns the wait's result.
static int run(unsigned ctx, unsigned id, unsigned long cflags, const void* sb_in,
               unsigned long sb_in_size, unsigned long rflags, const void* in,
               unsigned long in_size, void* out, unsigned long out_size) {
  char* p = (char*)batch;
  for (int i = 0; i < 64; i++) batch[i] = 0;
  if (cflags) p = sceAjmBatchJobControlBufferRa(p, id, cflags, sb_in, sb_in_size, sb, 64, 0);
  if (rflags) p = sceAjmBatchJobRunBufferRa(p, id, rflags, in, in_size, out, out_size, sb2, 64, 0);
  unsigned b = 0;
  const int r = sceAjmBatchStartBuffer(ctx, batch, p - (char*)batch, 50, 0, &b);
  return r ? r : sceAjmBatchWait(ctx, b, 0xFFFFFFFF, 0);
}

// Whether s holds the n float samples at ref as S16.
static int same16(const short* s, const float* ref, int n) {
  for (int i = 0; i < n; i++) {
    // Rounded to nearest; a tie may go either way here (to even there).
    const float x = ref[i] * 32768.0f, t = x - (long)x;
    long v = (long)(x < 0 ? x - 0.5f : x + 0.5f);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    const long d = s[i] - v;
    if (d && !((t == 0.5f && d == -1) || (t == -0.5f && d == 1))) return 0;
  }
  return 1;
}

static unsigned crc8(const unsigned char* p, int n) {
  unsigned crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc & 0x80 ? crc << 1 ^ 0x45 : crc << 1) & 0xFF;
  }
  return crc;
}

static int parser_checks(void) {
  int failed = 0;
  unsigned out[5] = {0};
  const unsigned char config[4] = {0xFE, 0x70, 0x07, 0xF0};
  failed += test_check(358, sceAjmDecAt9ParseConfigData(config, out), 0);
  failed += test_check(359, out[0] == 1 && out[1] == 48000 && out[2] == 256 && out[3] == 1024 &&
                                out[4] == 256,
                       1);
  const unsigned char odd[4] = {0xFE, 0x70, 0x07, 0xE8};  // superframe index 1
  failed += test_check(360, (unsigned)sceAjmDecAt9ParseConfigData(odd, out), 0x80930005);

  // 361-363: an MPEG-1 Layer III frame, 128 kbit/s, 44.1 kHz, joint stereo,
  // with a Xing header (100 frames) and LAME's delay and padding.
  static unsigned char f[417];
  f[0] = 0xFF, f[1] = 0xFB, f[2] = 0x90, f[3] = 0x64;
  const char* xing = "Xing";
  for (int i = 0; i < 4; i++) f[36 + i] = xing[i], f[48 + i] = "LAME"[i];
  f[43] = 1, f[47] = 100;
  f[48 + 21] = 0x24, f[48 + 22] = 0x01, f[48 + 23] = 0x23;  // delay 576, padding 291
  unsigned m[10] = {0};
  failed += test_check(361, sceAjmDecMp3ParseFrame(f, sizeof f, 0, m), 0);
  failed += test_check(362, m[0] + m[2] * 10000 + m[3] + m[4] / 1000 + m[5], 417 + 20000 + 1152 + 128 + 44100);
  sceAjmDecMp3ParseFrame(f, sizeof f, 1, m);
  failed += test_check(363, m[6] == 1152 + 576 + 529 && m[7] == 100 &&
                                m[8] == 1152 * 100 - 867 && m[9] == 1,
                       1);
  // 364: the 0xB4 tag (with its CRC) instead.
  for (int i = 36; i < 60; i++) f[i] = 0;
  const unsigned char tag[9] = {0xB4, 0x00, 0x10, 0x00, 0x00, 0x12, 0x34, 0x00, 0x00};
  for (int i = 0; i < 9; i++) f[40 + i] = tag[i];
  f[49] = crc8(tag, 9);
  sceAjmDecMp3ParseFrame(f, sizeof f, 1, m);
  failed += test_check(364, m[6] == 16 && m[8] == 0x1234 && m[9] == 3, 1);
  f[1] = 0xFD;  // Layer II
  failed += test_check(365, (unsigned)sceAjmDecMp3ParseFrame(f, sizeof f, 0, m), 0x80930005);
  return failed;
}

int ajm_checks(void) {
  int failed = parser_checks();
  unsigned ctx = 0, bad = 0;
  failed += test_check(350, sceAjmInitialize(0, &ctx), 0);
  failed += test_check(351, (unsigned)sceAjmInitialize(1, &bad) + (bad == 0xFFFFFFFF), 0x80930006);
  failed += test_check(352, (unsigned)sceAjmModuleRegister(ctx, 14, 0), 0x80930008);  // Vorbis
  failed += test_check(353, sceAjmModuleRegister(ctx, 1, 0) * 2 +
                                ((unsigned)sceAjmModuleRegister(ctx, 1, 0) == 0x80930009),
                       1);
  unsigned id = 0, id16 = 0;
  failed += test_check(354, (unsigned)sceAjmInstanceCreate(ctx, 2, 1, &id), 0x8093000A);
  failed += test_check(355, (unsigned)sceAjmInstanceCreate(ctx, 1, 1 | 3 << 7, &id), 0x8093000C);
  failed += test_check(356, (unsigned)sceAjmInstanceCreate(ctx, 1, 0, &id), 0x8093000B);
  // Float, then S16: the first instances' slots are 1 and 2.
  sceAjmInstanceCreate(ctx, 1, 1 | 2 << 7, &id);
  sceAjmInstanceCreate(ctx, 1, 1, &id16);
  failed += test_check(357, id == 0x4001 && id16 == 0x4002 && sceAjmInstanceCodecType(id) == 1, 1);

  // The superframes: frame 0 starts a packet, the others continue it.
  // The middle superframe's frames are another pattern, 59 bytes long:
  // each starts where the one before ended, the last one taking the
  // superframe's rest.
  static const unsigned char kFrame[2][4] = {{0x3C, 0x83, 0x96, 0x1F}, {0x35, 0x20, 0xB4, 0x73}};
  for (int s = 0; s < 3; s++)
    for (int k = 0; k < 4; k++) {
      unsigned char* fr = superframes + 256 * s + (s == 1 ? 59 : 64) * k;
      for (int i = 0; i < 4; i++) fr[i] = kFrame[s == 1][i];
      fr[0] |= k ? 0x80 : 0;
    }

  // 366-369: the job builders' words, an init parameter inline in the batch.
  const unsigned char init[8] = {0xFE, 0x70, 0x07, 0xF0};
  const void* init_at = 0;
  char* p = sceAjmBatchJobInlineBuffer(batch, init, 5, &init_at);
  failed += test_check(366, (p == (char*)batch + 16) * 10 + (init_at == batch + 1) +
                                (batch[0] == (8ul << 32 | 7)) * 100,
                       111);
  batch[1] = 0xFE | 0x70 << 8 | 0x07 << 16 | 0xF0u << 24;
  char* q = sceAjmBatchJobControlBufferRa(p, id, kRev | kInitialize | kReset | 1ul << 63, init_at,
                                          8, sb, 8, 0);
  const unsigned* w = (const unsigned*)p;
  failed += test_check(367, (q - p) * 1000 + (w[0] == id << 6) * 100 + (w[1] == 40) * 10 +
                                (w[7] == (kRev | kInitialize | kReset) && (w[6] & 0xF) == 3),
                       48111);
  char* end = sceAjmBatchJobRunBufferRa(q, id, kRev | kStream | kMultipleFrames, superframes, 512,
                                        pcm, sizeof pcm, sb2, 0x20, 0);
  failed += test_check(368, end - q, 64);
  unsigned b = 0;
  failed += test_check(369, sceAjmBatchStartBuffer(ctx, batch, end - (char*)batch, 50, 0, &b) * 10 +
                                b,
                       0);
  // 370-372: two superframes decoded at once: 512 bytes in, 2048 samples,
  // the input used up.
  failed += test_check(370, sceAjmBatchWait(ctx, b, 0xFFFFFFFF, 0) + (int)sb[0], 0);
  const unsigned* s2 = (const unsigned*)sb2;
  failed += test_check(371, s2[0] == 0x8 && s2[2] == 512 && s2[3] == 2048 * 4 && sb2[2] == 2048 &&
                                s2[6] == 8,
                       1);
  int loud = 0, bounded = 1;
  for (int i = 0; i < 2048; i++) {
    if (pcm[i] > 0.01f || pcm[i] < -0.01f) loud++;
    if (pcm[i] > 1.5f || pcm[i] < -1.5f) bounded = 0;
  }
  failed += test_check(372, loud > 100 && bounded, 1);
  failed += test_check(373, (unsigned)sceAjmBatchWait(ctx, b, 0, 0), 0x80930004);

  // 374-375: the same superframes as S16, a frame at a time: 64 bytes
  // each, the same samples.
  run(ctx, id16, kRev | kInitialize | kReset, init, 8, 0, 0, 0, 0, 0);
  int r = run(ctx, id16, 0, 0, 0, kRev | kStream, superframes, 512, pcm16, sizeof pcm16);
  failed += test_check(374, r * 10 + (s2[2] == 64 && s2[3] == 512 && sb2[2] == 256), 1);
  failed += test_check(375, same16(pcm16, pcm, 256), 1);

  // 376-377: gapless: skip 100 samples, then 1000 at most: the samples
  // of the first decode from the 100th on.
  const unsigned gapless[2] = {1000, 100};
  r = run(ctx, id, kRev | kGapless | kReset, gapless, 8, kRev | kStream | kMultipleFrames | kGapless,
          superframes, 768, pcm + 2048, 4 * 2048);
  int shifted = 1;
  for (int i = 0; i < 1000; i++) shifted &= pcm[2048 + i] == pcm[100 + i];
  failed += test_check(376, r * 10 + (s2[2] == 768 && s2[3] == 1000 * 4 && sb2[2] == 1000 && shifted),
                       1);
  failed += test_check(377, s2[8] == 1000 && s2[9] == (100 | 100 << 16), 1);

  // 390: a run over two input and two output buffers: the same samples
  // as one buffer gives (the float run's), the outputs filled one after the other.
  run(ctx, id16, kRev | kInitialize | kReset, init, 8, 0, 0, 0, 0, 0);
  const struct {
    const void* addr;
    unsigned long size;
  } ins[2] = {{superframes, 256}, {superframes + 256, 256}},
    outs[2] = {{split, 1000}, {split + 500, 4096 - 1000}};
  for (int i = 0; i < 64; i++) batch[i] = 0;
  end = sceAjmBatchJobRunSplitBufferRa(batch, id16, kRev | kStream | kMultipleFrames, ins, 2, outs, 2,
                                       sb2, 0x20, 0);
  sceAjmBatchStartBuffer(ctx, batch, end - (char*)batch, 50, 0, &b);
  r = sceAjmBatchWait(ctx, b, 0xFFFFFFFF, 0);
  failed += test_check(390, r * 10 + (end - (char*)batch == 8 + 4 * 16 + 8 + 16 && same16(split, pcm, 2048) &&
                                     s2[2] == 512 && s2[3] == 4096 && s2[6] == 8),
                       1);
  failed += test_check(391, s2[0], 0x10);  // the output is full

  // 392-396: an instance no initialisation job configured, given a RIFF
  // header: fact's 1000 samples after a delay of 100.
  unsigned id3 = 0;
  sceAjmInstanceCreate(ctx, 1, 1, &id3);
  static const unsigned char kGuid[16] = {0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
                                          0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C};
  unsigned* h = (unsigned*)riff;
  h[0] = 'R' | 'I' << 8 | 'F' << 16 | 'F' << 24, h[1] = sizeof riff - 8;
  h[2] = 'W' | 'A' << 8 | 'V' << 16 | 'E' << 24;
  h[3] = 'f' | 'm' << 8 | 't' << 16 | ' ' << 24, h[4] = 52;
  h[5] = 0xFFFE | 1 << 16, h[6] = 48000, h[7] = 12000, h[8] = 256;  // block: the superframe
  h[9] = 34 | 256 << 16, h[10] = 4;  // extension size, samples per block, speaker mask
  for (int i = 0; i < 16; i++) riff[44 + i] = kGuid[i];
  h[15] = 1, h[16] = 0xF00770FE, h[17] = 0;  // version, config
  h[18] = 'f' | 'a' << 8 | 'c' << 16 | 't' << 24, h[19] = 12, h[20] = 1000, h[21] = h[22] = 100;
  h[23] = 'd' | 'a' << 8 | 't' << 16 | 'a' << 24, h[24] = 3 * 256;
  for (int i = 0; i < 3 * 256; i++) riff[100 + i] = superframes[i];
  // Room for 500 samples: frame 0 gives 156 after the delay, frame 1 256,
  // frame 2 does not fit; the header and two frames consumed.
  r = run(ctx, id3, 0, 0, 0, kRev | kStream | kMultipleFrames, riff, sizeof riff, pcm16, 1000);
  failed += test_check(392, r * 10 + (s2[0] == 0x10 && s2[2] == 100 + 128 && s2[3] == 412 * 2 &&
                                      sb2[2] == 412 && s2[6] == 2),
                       1);
  failed += test_check(393, same16(pcm16, pcm + 100, 412), 1);
  // The rest: output stops at the fact's samples, the input is used up.
  r = run(ctx, id3, 0, 0, 0, kRev | kStream | kMultipleFrames, riff + 228, sizeof riff - 228,
          pcm16, 4096);
  failed += test_check(394, r * 10 + (s2[0] == 0x8 && s2[2] == sizeof riff - 228 &&
                                      s2[3] == 588 * 2 && sb2[2] == 1000 && s2[6] == 10),
                       1);
  failed += test_check(395, same16(pcm16, pcm + 512, 588), 1);
  // A new header starts the stream again.
  r = run(ctx, id3, 0, 0, 0, kRev | kStream | kMultipleFrames, riff, sizeof riff, pcm16, 1000);
  failed += test_check(396, r * 10 + (s2[2] == 228 && sb2[2] == 412 && same16(pcm16, pcm + 100, 412)),
                       1);
  sceAjmInstanceDestroy(ctx, id3);

  // 378-380: errors: the error block names the job, the command and the
  // job's return address.
  BatchError e = {0};
  for (int i = 0; i < 64; i++) batch[i] = 0;
  end = sceAjmBatchJobRunBufferRa(batch, id, kRev, 0, 0, 0, 0, sb2, 8, (void*)0x1234);
  ((unsigned*)batch)[11] = 2;  // the RUN word's revision
  failed += test_check(378, (unsigned)sceAjmBatchStartBuffer(ctx, batch, end - (char*)batch, 50, &e, &b),
                       0x80930011);
  failed += test_check(379, (unsigned)e.code == 0x8093000B && e.job == (unsigned long)batch &&
                                e.offset == 40 && e.ra == 0x1234 && b == 0xFFFFFFFF,
                       1);
  failed += test_check(380, (unsigned)sceAjmBatchStartBuffer(ctx, batch, end - (char*)batch, 5, &e, &b),
                       0x8093000E);
  ((unsigned*)batch)[11] = 1;
  ((unsigned*)batch)[0] = 0x4009 << 6;  // no such instance
  sceAjmBatchStartBuffer(ctx, batch, end - (char*)batch, 50, &e, &b);
  failed += test_check(381, (unsigned)e.code == 0x80930003 && e.offset == 0 && e.ra == 0x1234, 1);

  // 382-385: an instance in a started batch is busy until the batch is
  // waited for, cancelled or not; batch ids come from a FIFO.
  ((unsigned*)batch)[0] = id << 6;
  sceAjmBatchStartBuffer(ctx, batch, end - (char*)batch, 50, 0, &b);
  failed += test_check(382, (unsigned)sceAjmInstanceDestroy(ctx, id), 0x8093000D);
  failed += test_check(383, sceAjmBatchCancel(ctx, b) * 10 + (b > 1), 1);
  failed += test_check(384, (unsigned)sceAjmBatchWait(ctx, b, 0, 0), 0x80930017);
  failed += test_check(385, (unsigned)sceAjmModuleUnregister(ctx, 1), 0x8093000D);
  failed += test_check(386, sceAjmInstanceDestroy(ctx, id) + sceAjmInstanceDestroy(ctx, id16) +
                                sceAjmModuleUnregister(ctx, 1),
                       0);
  failed += test_check(387, (unsigned)sceAjmMemoryRegister(ctx, pcm + 1, 1), 0x80930005);
  failed += test_check(388, sceAjmStrError(0x80930011) != sceAjmStrError(0) &&
                                sceAjmStrError(0x80930001) == sceAjmStrError(0x12345678),
                       1);
  failed += test_check(389, sceAjmFinalize(ctx) * 10 + ((unsigned)sceAjmFinalize(ctx) == 0x80930002),
                       1);
  return failed;
}
