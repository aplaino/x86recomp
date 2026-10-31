// orbis_process_test's checks of the other system libraries' HLE, from
// lifted code: fibers (libSceFiber, one resumed on another thread too),
// directory reads and event queues
// (libkernel), and the GPU (libSceGnmDriver's packets and submission, the
// command processor's memory writes and end-of-pipe event).
// system_checks() returns how many failed.

int test_check(int id, long got, long want);

typedef struct {
  unsigned long words[32];  // SceFiber: 256 bytes, 8-aligned
} Fiber;
int _sceFiberInitializeImpl(Fiber* f, const char* name, void (*entry)(unsigned long, unsigned long),
                            unsigned long arg_on_init, void* context, unsigned long size,
                            const void* opt, unsigned build);
int sceFiberRun(Fiber* f, unsigned long arg, unsigned long* ret);
int sceFiberSwitch(Fiber* f, unsigned long arg, unsigned long* got);
int sceFiberReturnToThread(unsigned long ret, unsigned long* got);
int sceFiberGetSelf(Fiber** f);
int sceFiberFinalize(Fiber* f);
typedef unsigned long Handle;
int scePthreadCreate(Handle* t, const Handle* attr, void* (*entry)(void*), void* arg,
                     const char* name);
int scePthreadJoin(Handle t, void** ret);

int sceKernelOpen(const char* path, int flags, int mode);
int sceKernelClose(int fd);
int sceKernelGetdents(int fd, char* buf, int n);
int sceKernelCreateEqueue(long* eq, const char* name);
int sceKernelWaitEqueue(long eq, void* ev, int n, int* out, unsigned* timeout_us);

int sceGnmSetVsShader(unsigned* cmd, unsigned dw, const unsigned* regs, unsigned modifier);
int sceGnmInsertPushMarker(unsigned* cmd, unsigned dw, const char* s);
unsigned sceGnmDrawInitDefaultHardwareState200(unsigned* cmd, unsigned dw);
int sceGnmSubmitCommandBuffers(unsigned n, void** dcbs, unsigned* sizes, void** ccbs,
                               unsigned* ccb_sizes);
int sceGnmAddEqEvent(long eq, int id, void* udata);

int sceSaveDataInitialize(const void* reserved);
int sceSaveDataMount(const void* mount, void* result);
int sceSaveDataUmount(const char* mount_point);
int sceSaveDataDelete(const void* del);
int sceSaveDataDirNameSearch(const void* cond, void* result);
int sceSaveDataSetParam(const char* mount_point, int type, const void* buf, unsigned long size);
long sceKernelWrite(int fd, const void* buf, unsigned long n);
long sceKernelRead(int fd, void* buf, unsigned long n);

int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void* buf);
int sceAudioOutClose(int handle);
unsigned long sceKernelGetProcessTime(void);

int scePadInit(void);
int scePadOpen(int user, int type, int index, const void* param);
int scePadRead(int handle, void* data, int n);

int sceNpTrophyCreateContext(int* ctx, int user, unsigned label, unsigned long options);
int sceNpTrophyCreateHandle(int* handle);
int sceNpTrophyRegisterContext(int ctx, int handle, unsigned long options);
int sceNpTrophyUnlockTrophy(int ctx, int handle, int id, int* platinum);

typedef struct {
  void* buffer;
  unsigned long size;
  unsigned long reserved[5];
  void* user;
} Ngs2BufferInfo;
typedef struct {
  int (*alloc)(Ngs2BufferInfo*);
  int (*free)(Ngs2BufferInfo*);
  void* user;
} Ngs2Allocator;
int sceNgs2SystemCreateWithAllocator(const void* option, const Ngs2Allocator* a,
                                     unsigned long* out);
int sceNgs2SystemDestroy(unsigned long sys, Ngs2BufferInfo* out);
int sceNgs2RackQueryBufferSize(unsigned id, const void* option, Ngs2BufferInfo* out);
int sceNgs2RackCreate(unsigned long sys, unsigned id, const void* option,
                      const Ngs2BufferInfo* buf, unsigned long* out);
int sceNgs2RackCreateWithAllocator(unsigned long sys, unsigned id, const void* option,
                                   const Ngs2Allocator* a, unsigned long* out);
int sceNgs2RackDestroy(unsigned long rack, Ngs2BufferInfo* out);
int sceNgs2RackGetVoiceHandle(unsigned long rack, unsigned index, unsigned long* out);
int sceNgs2VoiceControl(unsigned long voice, const void* params);
int sceNgs2VoiceGetState(unsigned long voice, void* state, unsigned long size);
int sceNgs2SystemRender(unsigned long sys, const void* buffers, unsigned count);

#define SCE_ETIMEDOUT 0x8002003C

// ---- fibers: a ping-pong between two, each on its own stack.

static Fiber fa, fb;
static _Alignas(16) char stack_a[0x4000], stack_b[0x4000];
static long trail;  // the order things ran in, a digit each

static void fiber_a(unsigned long init, unsigned long run) {
  trail = trail * 10 + (long)init;  // 1
  unsigned long got;
  sceFiberSwitch(&fb, run + 1, &got);  // b gets 11
  trail = trail * 10 + (long)(got - 20);  // 3: b switched back with 23
  Fiber* self;
  sceFiberGetSelf(&self);
  trail = trail * 10 + (self == &fa ? 4 : 9);
  sceFiberReturnToThread(77, &got);
  trail = trail * 10 + (long)got;  // 6: run again with 6
  sceFiberReturnToThread(88, &got);
}

static void fiber_b(unsigned long init, unsigned long run) {
  char local = 0;  // on b's own stack
  trail = trail * 10 + (run == 11 && init == 2 && &local >= stack_b &&
                                &local < stack_b + sizeof stack_b
                            ? 2
                            : 9);
  unsigned long got;
  sceFiberSwitch(&fa, 23, &got);
  sceFiberReturnToThread(0, &got);
}

// ---- a fiber that yields on one thread and is resumed on another: its
// locals survive, and it sees the thread it runs on, both through fs (the
// TCB pointer at fs:0, read again in the same function) and through TLS.

static Fiber fm;
static _Alignas(16) char stack_m[0x4000];
static __thread long thread_no;
static unsigned long tcb[2], moved_ret[2];

static unsigned long fs_tcb(void) {
  unsigned long p;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(p));
  return p;
}
static __attribute__((noinline)) long current_thread_no(void) { return thread_no; }

static void fiber_m(unsigned long init, unsigned long run) {
  volatile long kept = (long)run;  // 5, from thread 1
  unsigned long got, seen = fs_tcb() == tcb[0];
  sceFiberReturnToThread(1, &got);
  seen += 2 * (fs_tcb() == tcb[1]) + 4 * (current_thread_no() == 2);
  sceFiberReturnToThread((unsigned long)(kept * 100 + (long)got * 10) + seen, &got);
}

static void* run_moved(void* arg) {
  thread_no = (long)arg;
  tcb[thread_no - 1] = fs_tcb();
  sceFiberRun(&fm, thread_no == 1 ? 5 : 7, &moved_ret[thread_no - 1]);
  return 0;
}

static int fiber_checks(void) {
  int failed = 0;
  failed += test_check(70, _sceFiberInitializeImpl(&fa, "a", fiber_a, 1, stack_a, sizeof stack_a, 0,
                                                   0x3500000),
                       0);
  _sceFiberInitializeImpl(&fb, "b", fiber_b, 2, stack_b, sizeof stack_b, 0, 0x3500000);
  failed += test_check(71, (unsigned)_sceFiberInitializeImpl(&fb, "b", fiber_b, 2, stack_b, 100, 0,
                                                             0x3500000),
                       0x80590003);  // context too small
  unsigned long ret = 0;
  failed += test_check(72, sceFiberRun(&fa, 10, &ret), 0);
  failed += test_check(73, (long)ret, 77);
  trail = trail * 10 + 5;
  sceFiberRun(&fa, 6, &ret);
  failed += test_check(74, trail, 123456);
  failed += test_check(75, (long)ret, 88);
  Fiber* self;
  failed += test_check(76, (unsigned)sceFiberGetSelf(&self), 0x80590005);  // not in a fiber
  failed += test_check(77, sceFiberFinalize(&fa) + sceFiberFinalize(&fb), 0);
  _sceFiberInitializeImpl(&fm, "m", fiber_m, 0, stack_m, sizeof stack_m, 0, 0x3500000);
  for (long i = 1; i <= 2; i++) {
    Handle t;
    void* r;
    scePthreadCreate(&t, 0, run_moved, (void*)i, "fiber");
    scePthreadJoin(t, &r);
  }
  failed += test_check(192, (long)moved_ret[0], 1);
  failed += test_check(193, (long)moved_ret[1], 577);  // kept 5, got 7, then fs and TLS
  failed += test_check(194, sceFiberFinalize(&fm), 0);
  return failed;
}

// ---- getdents: the test's game directory has sub/ with b.txt and a.txt.

static int streq(const char* a, const char* b) {
  while (*a && *a == *b) a++, b++;
  return *a == *b;
}

static int dir_checks(void) {
  int failed = 0;
  char buf[1024];
  int fd = sceKernelOpen("/app0/sub", 0x20000, 0);  // O_DIRECTORY
  int n = sceKernelGetdents(fd, buf, sizeof buf);
  // Records: d_fileno 4, d_reclen 2, d_type 1, d_namlen 1, the name.
  int files = 0, order = 0, at = 0;
  while (at < n) {
    const char* name = buf + at + 8;
    const unsigned char type = (unsigned char)buf[at + 6];
    if (streq(name, "a.txt")) files += 1, order = order * 10 + 1, files += type == 8 ? 10 : 0;
    if (streq(name, "b.txt")) files += 1, order = order * 10 + 2;
    at += *(unsigned short*)(buf + at + 4);
  }
  failed += test_check(80, files, 12);
  failed += test_check(81, order, 12);  // sorted by name
  failed += test_check(82, at == n && n % 4 == 0, 1);
  failed += test_check(83, sceKernelGetdents(fd, buf, sizeof buf), 0);  // the end
  sceKernelClose(fd);
  return failed;
}

// ---- the GPU

static unsigned dcb[0x200];
static volatile unsigned long eop_value;
static volatile unsigned written[2];

static int gpu_checks(void) {
  int failed = 0;
  // A vertex shader's registers: PGM_LO, HI, RSRC1, RSRC2, SPI_VS_OUT_CONFIG,
  // SPI_SHADER_POS_FORMAT, PA_CL_VS_OUT_CNTL.
  static const unsigned vs[7] = {0x1000, 0, 0x2C0, 0x14, 0x4, 0x1, 0x0};
  static const unsigned want_vs[19] = {0xC0027600, 0x48, 0x1000, 0, 0xC0027600, 0x4A, 0x2C0,
                                       0x14, 0xC0016900, 0x207, 0, 0xC0016900, 0x1B1, 4,
                                       0xC0016900, 0x1C3, 1, 0xC00A1000, 0};
  int same = 1;
  failed += test_check(90, sceGnmSetVsShader(dcb, 29, vs, 0), 0);
  for (int i = 0; i < 19; i++) same &= dcb[i] == want_vs[i];
  failed += test_check(91, same, 1);
  failed += test_check(92, sceGnmSetVsShader(dcb, 28, vs, 0), -1);  // too little room
  // A marker: a NOP of 0x68750001 and the string.
  failed += test_check(93, sceGnmInsertPushMarker(dcb, 9, "frame"), 0);
  failed += test_check(94, dcb[0] == 0xC0071000 && dcb[1] == 0x68750001 && streq((char*)&dcb[2], "frame"),
                       1);
  failed += test_check(95, (long)sceGnmDrawInitDefaultHardwareState200(dcb, 0x100), 0x100);
  failed += test_check(96, dcb[0x82] == 0xC07C1000, 1);  // the padding NOP

  // A submission: the default state, WRITE_DATA of two words, then an
  // end-of-pipe write of 64-bit data with an interrupt, on an event queue.
  long eq;
  sceKernelCreateEqueue(&eq, "gpu");
  failed += test_check(97, sceGnmAddEqEvent(eq, 0x40, (void*)0x1234), 0);
  unsigned* p = dcb + 0x100;
  *p++ = 0xC0043700;  // WRITE_DATA to memory, two words
  *p++ = 0x500;
  *p++ = (unsigned)(unsigned long)written;
  *p++ = (unsigned)((unsigned long)written >> 32);
  *p++ = 0xAAAA;
  *p++ = 0xBBBB;
  *p++ = 0xC0044700;  // EVENT_WRITE_EOP
  *p++ = 0x504;
  *p++ = (unsigned)(unsigned long)&eop_value;
  *p++ = (unsigned)((unsigned long)&eop_value >> 32) | 2u << 29 | 2u << 24;
  *p++ = 0x89ABCDEF;
  *p++ = 0x01234567;
  void* bufs[1] = {dcb};
  unsigned sizes[1] = {(unsigned)((char*)p - (char*)dcb)};
  failed += test_check(98, sceGnmSubmitCommandBuffers(1, bufs, sizes, 0, 0), 0);
  unsigned long ev[4];
  int got = 0;
  unsigned timeout = 2000000;
  failed += test_check(99, sceKernelWaitEqueue(eq, ev, 1, &got, &timeout), 0);
  failed += test_check(100, got == 1 && ev[0] == 0x40 && (short)ev[1] == -14 && ev[3] == 0x1234, 1);
  failed += test_check(101, (long)eop_value, 0x0123456789ABCDEF);
  failed += test_check(102, written[0] == 0xAAAA && written[1] == 0xBBBB, 1);
  timeout = 1000;
  failed += test_check(103, (unsigned)sceKernelWaitEqueue(eq, ev, 1, &got, &timeout), SCE_ETIMEDOUT);
  failed += test_check(104, (unsigned)sceGnmSubmitCommandBuffers(1, bufs, 0, 0, 0), 0x80D11000);
  return failed;
}

// ---- save data: one directory created, written through its mount point,
// mounted again and read, found, deleted.

typedef struct {
  int user, pad;
  const char* title;
  const char* dir;
  const void* fingerprint;
  unsigned long blocks;
  unsigned mode;
  char reserved[32];
} SaveMount;
typedef struct {
  char mount_point[16];
  unsigned long required;
  unsigned unused, status;
  char reserved[28];
} SaveMountResult;

static int save_checks(void) {
  int failed = 0;
  failed += test_check(110, sceSaveDataInitialize(0), 0);
  SaveMount m = {1, 0, 0, "SAVE0001", 0, 96, 2 | 4};  // read-write, create
  SaveMountResult r = {0};
  failed += test_check(111, sceSaveDataMount(&m, &r), 0);
  failed += test_check(112, streq(r.mount_point, "/savedata0") && r.status == 1, 1);
  failed += test_check(113, (unsigned)sceSaveDataMount(&m, &r), 0x809F0003);  // busy: mounted
  int fd = sceKernelOpen("/savedata0/slot.bin", 0x201 | 0x400, 0644);  // write, create, trunc
  failed += test_check(114, sceKernelWrite(fd, "progress", 8), 8);
  sceKernelClose(fd);
  failed += test_check(115, sceSaveDataSetParam("/savedata0", 4, "\x2a\0\0\0", 4), 0);
  failed += test_check(116, sceSaveDataUmount("/savedata0"), 0);
  failed += test_check(117, (unsigned)sceKernelOpen("/savedata0/slot.bin", 0, 0), 0x80020002);
  failed += test_check(118, (unsigned)sceSaveDataMount(&m, &r), 0x809F0007);  // exists
  m.mode = 1;  // read-only
  failed += test_check(119, sceSaveDataMount(&m, &r) == 0 && r.status == 0, 1);
  char buf[16] = {0};
  fd = sceKernelOpen("/savedata0/slot.bin", 0, 0);
  failed += test_check(120, sceKernelRead(fd, buf, sizeof buf), 8);
  sceKernelClose(fd);
  failed += test_check(121, streq(buf, "progress"), 1);
  failed += test_check(122, (unsigned)sceKernelOpen("/savedata0/new.bin", 0x201, 0644),
                       0x8002001E);  // EROFS
  sceSaveDataUmount("/savedata0");
  struct {
    int user, pad;
    const char* title;
    const char* dir;
    unsigned key, order;
    char reserved[32];
  } cond = {1, 0, 0, "SAVE%", 0, 0};
  char names[4][32];
  unsigned char params[4][0x530];
  struct {
    unsigned hits, pad;
    void* names;
    unsigned room, set;
    void* params;
    void* infos;
    char reserved[12];
  } found = {0, 0, names, 4, 0, params, 0};
  failed += test_check(123, sceSaveDataDirNameSearch(&cond, &found), 0);
  failed += test_check(124, found.hits == 1 && found.set == 1 && streq(names[0], "SAVE0001") &&
                                params[0][0x500] == 42,
                       1);
  struct {
    int user, pad;
    const char* title;
    const char* dir;
    unsigned unused;
    char reserved[32];
  } del = {1, 0, 0, "SAVE0001"};
  failed += test_check(125, sceSaveDataDelete(&del), 0);
  failed += test_check(126, (unsigned)sceSaveDataDelete(&del), 0x809F0008);  // not found
  m.mode = 2;
  failed += test_check(127, (unsigned)sceSaveDataMount(&m, &r), 0x809F0008);
  return failed;
}

// ---- audio out: a port's buffers are taken one per 256 samples at 48 kHz.

static short pcm[256 * 2];

static int audio_checks(void) {
  int failed = 0;
  failed += test_check(130, sceAudioOutInit(), 0);
  failed += test_check(131, (unsigned)sceAudioOutOpen(0xFF, 0, 0, 100, 48000, 1),
                       0x80260006);  // not a multiple of 256
  failed += test_check(132, (unsigned)sceAudioOutOpen(0xFF, 0, 0, 256, 44100, 1), 0x80260008);
  int h = sceAudioOutOpen(0xFF, 0, 0, 256, 48000, 1);  // S16 stereo
  failed += test_check(133, h, 0x20000007);
  unsigned long t0 = sceKernelGetProcessTime();
  for (int i = 0; i < 21; i++) sceAudioOutOutput(h, pcm);
  // 20 buffers must have been taken: 20 x 5.33 ms.
  failed += test_check(134, sceKernelGetProcessTime() - t0 >= 100000, 1);
  failed += test_check(135, sceAudioOutClose(h), 0);
  failed += test_check(136, (unsigned)sceAudioOutOutput(h, pcm), 0x80260001);  // not open
  return failed;
}

// ---- the pad: the user's controller, at rest with no host input.

static int pad_checks(void) {
  int failed = 0;
  unsigned char data[2][0x80];
  failed += test_check(140, scePadInit(), 0);
  const int h = scePadOpen(1, 0, 0, 0);
  failed += test_check(141, h, 1);
  failed += test_check(142, (unsigned)scePadOpen(1, 0, 0, 0), 0x80920004);  // opened already
  failed += test_check(143, (unsigned)scePadRead(h, data, 0), 0x80920001);
  failed += test_check(144, scePadRead(h, data, 2), 1);
  failed += test_check(145, *(unsigned*)data[0] == 0 && data[0][4] == 0x80 && data[0][7] == 0x80 &&
                                data[0][0x4C] == 1,
                       1);  // no buttons, sticks centred, connected
  return failed;
}

// ---- trophies, kept per service label.

static int trophy_checks(void) {
  int failed = 0, ctx = 0, h = 0, platinum = 0;
  failed += test_check(150, sceNpTrophyCreateContext(&ctx, 1, 0, 0), 0);
  failed += test_check(151, sceNpTrophyCreateHandle(&h), 0);
  failed += test_check(152, sceNpTrophyRegisterContext(ctx, h, 0), 0);
  failed += test_check(153, sceNpTrophyUnlockTrophy(ctx, h, 3, &platinum), 0);
  failed += test_check(154, platinum, -1);
  failed += test_check(155, (unsigned)sceNpTrophyUnlockTrophy(ctx, h, 3, &platinum), 0x8055160C);
  failed += test_check(156, (unsigned)sceNpTrophyUnlockTrophy(ctx, h, 200, &platinum), 0x8055160A);
  return failed;
}

// ---- NGS2: a stereo 16-bit one-shot through a sampler voice into a
// mastering voice, rendered into a float buffer.

static _Alignas(16) char ngs2_pool[0x20000];
static unsigned long ngs2_used, ngs2_frees;

static int ngs2_alloc(Ngs2BufferInfo* i) {
  i->buffer = ngs2_pool + ngs2_used;
  ngs2_used += (i->size + 15) & ~15ul;
  return 0;
}
static int ngs2_free(Ngs2BufferInfo* i) {
  ngs2_frees++;
  return 0;
}

typedef struct {
  unsigned short size;
  short next;
  unsigned id;
} Ngs2Head;
typedef struct {
  Ngs2Head h;
  unsigned type, channels, rate, config, offset, margin, flags, reserved;
} Ngs2Setup;
typedef struct {
  Ngs2Head h;
  const void* data;
  unsigned flags, count;
  const void* blocks;
} Ngs2Blocks;
typedef struct {
  unsigned offset, size, repeats, skip, samples, reserved;
  unsigned long user;
} Ngs2Block;
typedef struct {
  Ngs2Head h;
  unsigned port, input;
  unsigned long dest;
} Ngs2Patch;
typedef struct {
  Ngs2Head h;
  unsigned a, b;
} Ngs2Pair;
typedef struct {
  Ngs2Head h;
  unsigned event;
} Ngs2Event;
typedef struct {
  void* buffer;
  unsigned long size;
  unsigned type, channels;
} Ngs2Render;

static short ngs2_pcm[512 * 2];
static float ngs2_out[256 * 2];

static unsigned bits(float f) { return *(unsigned*)&f; }

static int ngs2_checks(void) {
  int failed = 0;
  Ngs2Allocator a = {ngs2_alloc, ngs2_free, 0};
  unsigned long sys = 0, sampler = 0, master = 0, sv = 0, mv = 0;
  failed += test_check(160, (unsigned)sceNgs2SystemCreateWithAllocator(0, 0, &sys), 0x804A020A);
  failed += test_check(161, sceNgs2SystemCreateWithAllocator(0, &a, &sys), 0);
  failed += test_check(162, sys, (long)(ngs2_pool + 0x68));  // the handle: buffer + 0x68
  failed += test_check(163, ngs2_used, 0x10410);              // 0x410 + 512 x 0x80
  failed += test_check(164, (unsigned)sceNgs2RackCreateWithAllocator(sys, 0x1234, 0, &a, &sampler),
                       0x804A0260);
  failed += test_check(165, sceNgs2RackCreateWithAllocator(sys, 0x1000, 0, &a, &sampler), 0);
  failed += test_check(166, sceNgs2RackCreateWithAllocator(sys, 0x3000, 0, &a, &master), 0);
  failed += test_check(167, (unsigned)sceNgs2RackGetVoiceHandle(sampler, 256, &sv), 0x804A0302);
  sceNgs2RackGetVoiceHandle(sampler, 0, &sv);
  sceNgs2RackGetVoiceHandle(master, 0, &mv);

  // Mastering voice 0: stereo, to render buffer 0, playing.
  struct {
    Ngs2Pair setup;
    Ngs2Event play;
  } m = {{{16, 16, 0x30000000}, 2, 0}, {{12, 0, 6}, 0}};
  failed += test_check(168, sceNgs2VoiceControl(mv, &m), 0);

  // The sampler voice: 512 stereo frames of (0.5, -0.25), once, patched to
  // the mastering rack (its voice 0).
  for (int i = 0; i < 512; i++) ngs2_pcm[2 * i] = 0x4000, ngs2_pcm[2 * i + 1] = -0x2000;
  Ngs2Block block = {0, sizeof ngs2_pcm, 0, 0, 512, 0, 7};
  struct {
    Ngs2Setup setup;
    Ngs2Blocks blocks;
    Ngs2Patch patch;
    Ngs2Event play;
  } v = {{{40, 40, 0x10000000}, 0x12, 2, 48000, 0, 0, 0, 0, 0},
         {{32, 32, 0x10000001}, ngs2_pcm, 0, 1, &block},
         {{24, 24, 5}, 0, 0, master},
         {{12, 0, 6}, 0}};
  failed += test_check(169, sceNgs2VoiceControl(sv, &v), 0);
  Ngs2Pair bad = {{12, 0, 2}, 0, 0};  // port volume is 16 bytes
  failed += test_check(170, (unsigned)sceNgs2VoiceControl(sv, &bad), 0x804A030A);
  Ngs2Patch to_sampler = {{24, 0, 5}, 0, 0, sampler};  // samplers have no inputs
  failed += test_check(171, (unsigned)sceNgs2VoiceControl(sv, &to_sampler), 0x804A0305);
  Ngs2Pair loop[2] = {{{16, 16, 2}, 0, 0x3F800000}, {{16, -16, 2}, 0, 0x3F800000}};
  failed += test_check(172, (unsigned)sceNgs2VoiceControl(sv, loop), 0x804A030B);

  unsigned st[12];
  failed += test_check(173, sceNgs2VoiceGetState(sv, st, 4), 0);
  failed += test_check(174, st[0], 1);  // played, not rendered yet

  Ngs2Render out = {ngs2_out, sizeof ngs2_out, 0x18, 2};
  Ngs2Render three = {ngs2_out, sizeof ngs2_out, 0x18, 3};
  failed += test_check(175, (unsigned)sceNgs2SystemRender(sys, &three, 1), 0x804A0052);
  failed += test_check(176, sceNgs2SystemRender(sys, &out, 1), 0);
  // The resampler starts 3 samples late (its history), then copies.
  failed += test_check(177, bits(ngs2_out[2 * 2]) == 0 && bits(ngs2_out[3 * 2]) == 0x3F000000 &&
                                bits(ngs2_out[3 * 2 + 1]) == 0xBE800000 &&
                                bits(ngs2_out[255 * 2]) == 0x3F000000,
                       1);
  for (int i = 0; i < 12; i++) st[i] = 0xAA;
  failed += test_check(178, sceNgs2VoiceGetState(sv, st, 8), 0);
  failed += test_check(179, st[0], 0xAA);  // 4 < size < 0x30: nothing written
  sceNgs2VoiceGetState(sv, st, 0x30);
  failed += test_check(180, st[0], 3);  // in use, playing
  failed += test_check(181, st[1], 0x3F800000);  // no envelope: 1.0
  failed += test_check(182, st[4] | (unsigned long)st[5] << 32, 256);  // decoded samples
  failed += test_check(183, st[6], 1024);                              // and bytes
  failed += test_check(184, st[8], 7);                                 // the block's userData
  sceNgs2SystemRender(sys, &out, 1);
  sceNgs2VoiceGetState(sv, st, 0x30);
  failed += test_check(185, st[0], 3);
  sceNgs2SystemRender(sys, &out, 1);  // the last 3 samples, then the voice ends
  failed += test_check(186, bits(ngs2_out[2 * 2]) == 0x3F000000 && bits(ngs2_out[3 * 2]) == 0, 1);
  sceNgs2VoiceGetState(sv, st, 0x30);
  failed += test_check(187, st[0], 0);

  failed += test_check(188, sceNgs2RackDestroy(sampler, 0), 0);
  failed += test_check(189, sceNgs2RackDestroy(master, 0), 0);
  failed += test_check(190, sceNgs2SystemDestroy(sys, 0), 0);
  failed += test_check(191, ngs2_frees, 3);
  return failed;
}

int system_checks(void) {
  return fiber_checks() + dir_checks() + gpu_checks() + save_checks() + audio_checks() +
         pad_checks() + trophy_checks() + ngs2_checks();
}
