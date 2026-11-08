// orbis_process_test's checks of libSceVideodec2's HLE
// (orbis/hle/videodec2.cpp), from lifted code: the compute queue, the
// decoder's memory (the configuration measured on the console), creation
// and its checks, then h264.py's 12 access units: pictures from the 8th
// on, in display order, each in the frame buffer passed (NV12 at a pitch
// of 1920, the coded rows past the crop too) with its picture info; a
// flush for the rest, reset and deletion. videodec2_checks() returns how
// many failed.

int test_check(int id, long got, long want);

int sceKernelAllocateDirectMemory(long start, long end, unsigned long len, unsigned long align,
                                  int type, long* out);
int sceKernelMapDirectMemory(void** addr, unsigned long len, int prot, int flags, long off,
                             unsigned long align);
int sceVideodec2QueryComputeMemoryInfo(void* info);
int sceVideodec2AllocateComputeQueue(const void* cfg, const void* mem, unsigned long* queue);
int sceVideodec2ReleaseComputeQueue(unsigned long queue);
int sceVideodec2QueryDecoderMemoryInfo(const void* cfg, void* mem);
int sceVideodec2CreateDecoder(const void* cfg, const void* mem, unsigned long* dec);
int sceVideodec2DeleteDecoder(unsigned long dec);
int sceVideodec2Decode(unsigned long dec, const void* in, void* fb, void* out);
int sceVideodec2Flush(unsigned long dec, void* fb, void* out);
int sceVideodec2Reset(unsigned long dec);
int sceVideodec2GetPictureInfo(const void* out, void* p0, void* p1);

extern const unsigned char* const vd_aus[12];
extern const unsigned long vd_au_sizes[12];
extern const int vd_display[12];

static unsigned char* Map(unsigned long len, int type) {
  long off = 0;
  void* p = 0;
  len = (len + 0x3FFF) & ~0x3FFFul;
  sceKernelAllocateDirectMemory(0, 0x100000000, len, 0x10000, type, &off);
  sceKernelMapDirectMemory(&p, len, 0x33, 0, off, 0x10000);
  return p;
}

// h264.py's pattern: the last macroblock row of display picture d.
static int Pattern(int d, int plane, int x, int y) {
  if (plane == 0) return 32 + 8 * d + (x + 3 * y) % 16;
  if (plane == 1) return 64 + 4 * d + x % 8;
  return 160 - 4 * d + y % 4;
}

// Display picture d in fb: flat but for its last row, which
// includes the coded rows 1080-1087 and chroma rows 536-543.
static int PictureIs(const unsigned char* fb, int d) {
  const unsigned char* uv = fb + 1920 * 1088;
  int ok = fb[0] == 128 && fb[1071 * 1920 + 700] == 128 && uv[535 * 1920 + 10] == 128;
  for (int x = 0; x < 1920; x += 37)
    ok &= fb[1072 * 1920 + x] == Pattern(d, 0, x, 1072) && fb[1087 * 1920 + x] == Pattern(d, 0, x, 1087);
  for (int x = 0; x < 960; x += 29)
    ok &= uv[536 * 1920 + 2 * x] == Pattern(d, 1, x, 536) && uv[543 * 1920 + 2 * x + 1] == Pattern(d, 2, x, 543);
  return ok;
}

typedef struct {
  unsigned long size;
  unsigned char valid, error, pictures, b11;
  unsigned codec, width, pitch, height, pad;
  unsigned char* buffer;
  unsigned long buffer_size;
} Output;

typedef struct {
  unsigned long size;
  unsigned char* buffer;
  unsigned long buffer_size;
  unsigned char accepted, pad[7];
} FrameBuffer;

static unsigned char pic[104];

// The picture info of out: display picture d, decoded as access unit i.
static int InfoIs(const Output* out, int i) {
  for (int k = 0; k < 104; k++) pic[k] = 0xEE;
  *(unsigned long*)pic = 104;
  if (sceVideodec2GetPictureInfo(out, pic, 0) != 0) return 0;
  const unsigned* w = (const unsigned*)pic;
  const unsigned long* q = (const unsigned long*)pic;
  return pic[8] == 1 && q[2] == 1000ul + i && q[3] == 2000ul + i && q[4] == 0x100ul + i && pic[40] == 0xEE &&
         pic[41] == 100 && pic[42] == 42 && w[11] == 119 && w[12] == 67 && pic[52] == 1 && pic[53] == 1 &&
         w[14] == 0 && w[15] == 0 && w[16] == 0 && w[17] == 8 && pic[79] == 5 && pic[82] == 2 && pic[85] == 1 &&
         w[22] == 1 && w[23] == 120 && pic[97] == 1 && pic[98] == 4 && pic[103] == 0xEE;
}

int videodec2_checks(void) {
  int failed = 0;
  unsigned long cinfo[3] = {24, 0, 1};
  int r = sceVideodec2QueryComputeMemoryInfo(cinfo);
  cinfo[0] = 16;
  failed += test_check(440, r == 0 && cinfo[1] == 0x495200 && cinfo[2] == 0 &&
                                (unsigned)sceVideodec2QueryComputeMemoryInfo(cinfo) == 0x811D0101,
                       1);

  // Pipe 4, queue 7, memory types checked.
  unsigned char ccfg[16] = {16, 0, 0, 0, 0, 0, 0, 0, 4, 0, 7, 0, 1};
  unsigned char* onion = Map(0x495200, 0);
  unsigned char* garlic = Map(0x495200, 3);
  unsigned long cmem[3] = {24, 0x495200, (unsigned long)garlic}, queue = 0;
  unsigned bad = (unsigned)sceVideodec2AllocateComputeQueue(ccfg, cmem, &queue);
  cmem[2] = (unsigned long)onion;
  cmem[1] = 0x495100;
  bad ^= (unsigned)sceVideodec2AllocateComputeQueue(ccfg, cmem, &queue) << 4;
  cmem[1] = 0x495200;
  ccfg[8] = 5;
  bad ^= (unsigned)sceVideodec2AllocateComputeQueue(ccfg, cmem, &queue) << 8;
  ccfg[8] = 4;
  failed += test_check(441, bad, 0x811D0109 ^ 0x811D0104 << 4 ^ 0x811D0201 << 8);
  r = sceVideodec2AllocateComputeQueue(ccfg, cmem, &queue);
  failed += test_check(442, r == 0 && queue == (unsigned long)onion + 0x290500, 1);

  // AVC (kind 1), High 4.2, 1920x1088, DPB -1, depth 4, queue, cores 0-5,
  // priority -1, memory types checked.
  unsigned long cfg[9] = {72, 1ul | 1ul << 32, 100ul | 42ul << 32, 1920ul | 1088ul << 32,
                          0xFFFFFFFFul | 4ul << 32, queue, 0x3F, 0xFFFFFFFFul | 0x100ul << 32, 0};
  unsigned long mem[9] = {72, 0, 1, 0, 1, 0, 1, 0, 0xFFFFFFFFFFFFFFFF};
  r = sceVideodec2QueryDecoderMemoryInfo(cfg, mem);
  failed += test_check(443, r == 0 && mem[1] == 0x334CC00 && mem[3] == 0x48EAE00 && mem[5] == 0x304B000 &&
                                mem[7] == 0x331400 && mem[8] == 0x100 && !mem[2] && !mem[4] && !mem[6],
                       1);
  cfg[2] = 99ul | 42ul << 32;
  bad = (unsigned)sceVideodec2QueryDecoderMemoryInfo(cfg, mem);
  cfg[2] = 100ul | 42ul << 32;
  cfg[4] = 0xFFFFFFFFul;
  bad ^= (unsigned)sceVideodec2QueryDecoderMemoryInfo(cfg, mem) << 4;
  cfg[4] = 0xFFFFFFFFul | 4ul << 32;
  failed += test_check(444, bad, 0x811D0205 ^ 0x811D0206 << 4);

  unsigned char* cpu = Map(mem[1], 0);
  unsigned char* gpu = Map(mem[3], 3);
  unsigned char* cpu_gpu = Map(mem[5], 0);
  mem[2] = (unsigned long)cpu;
  mem[4] = (unsigned long)cpu;  // not garlic
  mem[6] = (unsigned long)cpu_gpu;
  unsigned long dec = 0;
  failed += test_check(445, (unsigned)sceVideodec2CreateDecoder(cfg, mem, &dec), 0x811D010A);
  mem[4] = (unsigned long)gpu;
  r = sceVideodec2CreateDecoder(cfg, mem, &dec);
  failed += test_check(446, r == 0 && dec == (unsigned long)cpu, 1);
  failed += test_check(447, (unsigned)sceVideodec2ReleaseComputeQueue(queue), 0x811D0110);

  // Two frame buffers in turn.
  unsigned char* fbs[2] = {Map(0x400000, 3), Map(0x400000, 3)};
  FrameBuffer fb = {32, 0, 0x400000};
  Output out;
  unsigned long in[6] = {48};
  int shown = 0, quiet = 0, ok = 1;
  for (int i = 0; i < 12; i++) {
    in[1] = (unsigned long)vd_aus[i];
    in[2] = vd_au_sizes[i];
    in[3] = 1000 + i;
    in[4] = 2000 + i;
    in[5] = 0x100 + i;
    fb.buffer = fbs[i & 1];
    fb.accepted = 0xEE;
    out.size = 48;
    out.valid = 0xEE;
    r = sceVideodec2Decode(dec, in, &fb, &out);
    if (r != 0) ok = 0;
    if (!out.valid) {
      quiet += fb.accepted == 0;
      continue;
    }
    // Display picture `shown`: decoded as access unit j.
    int j = 0;
    while (vd_display[j] != shown) j++;
    ok &= fb.accepted == 1 && out.error == 0 && out.pictures == 1 && out.codec == 1 && out.width == 1920 &&
          out.pitch == 1920 && out.height == 1088 && out.buffer == fbs[i & 1] && out.buffer_size == 0x400000 &&
          PictureIs(out.buffer, shown) && InfoIs(&out, j);
    shown++;
  }
  failed += test_check(448, quiet, 7);  // access units 0-6 give nothing
  failed += test_check(449, ok * 100 + shown, 105);

  // The flush: the other 7, then nothing (which ends it).
  in[1] = (unsigned long)vd_aus[0];
  in[2] = vd_au_sizes[0];
  int during = 0;
  for (int k = 0; k < 9; k++) {
    fb.buffer = fbs[k & 1];
    r = sceVideodec2Flush(dec, &fb, &out);
    if (r != 0) ok = 0;
    if (k == 0) {
      Output other = {48};
      FrameBuffer fb2 = {32, fbs[1], 0x400000};
      during = sceVideodec2Decode(dec, in, &fb2, &other);
    }
    if (!out.valid) break;
    int j = 0;
    while (vd_display[j] != shown) j++;
    ok &= fb.accepted == 1 && PictureIs(out.buffer, shown) && InfoIs(&out, j);
    shown++;
  }
  failed += test_check(450, (unsigned)during, 0x811D0100);
  failed += test_check(451, ok * 100 + shown, 112);
  // After the flush, decoding starts over.
  r = sceVideodec2Decode(dec, in, &fb, &out);
  failed += test_check(452, r * 2 + out.valid, 0);
  failed += test_check(453, sceVideodec2Reset(dec), 0);
  r = sceVideodec2DeleteDecoder(dec);
  failed += test_check(454, r * 2 + ((unsigned)sceVideodec2DeleteDecoder(dec) == 0x811D0103), 1);
  r = sceVideodec2ReleaseComputeQueue(queue);
  failed += test_check(455, r * 2 + ((unsigned)sceVideodec2ReleaseComputeQueue(queue) == 0x811D0110), 1);
  return failed;
}
