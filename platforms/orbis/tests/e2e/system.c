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
int sceGnmDrawIndexAuto(unsigned* cmd, unsigned dw, unsigned count, unsigned flags);
int sceGnmDrawOpaqueAuto(unsigned* cmd, unsigned dw, unsigned flags);
int sceGnmDrawIndex(unsigned* cmd, unsigned dw, unsigned count, const void* indices, unsigned flags);
int sceGnmDrawIndexOffset(unsigned* cmd, unsigned dw, unsigned first, unsigned count, unsigned flags);
int sceGnmDrawIndexMultiInstanced(unsigned* cmd, unsigned dw, unsigned a, unsigned b,
                                  const void* indices, const void* objects);
int sceGnmDispatchDirect(unsigned* cmd, unsigned dw, unsigned x, unsigned y, unsigned z,
                         unsigned flags);
int sceGnmDrawIndirect(unsigned* cmd, unsigned dw, unsigned off, unsigned stage, unsigned vtx,
                       unsigned inst, unsigned flags);
int sceGnmDrawIndexIndirect(unsigned* cmd, unsigned dw, unsigned off, unsigned stage, unsigned vtx,
                            unsigned inst, unsigned flags);
int sceGnmDrawIndirectMulti(unsigned* cmd, unsigned dw, unsigned off, unsigned count, unsigned stage,
                            unsigned vtx, unsigned inst, unsigned flags);
int sceGnmDrawIndexIndirectMulti(unsigned* cmd, unsigned dw, unsigned off, unsigned count,
                                 unsigned stage, unsigned vtx, unsigned inst, unsigned flags);
int sceGnmDrawIndirectCountMulti(unsigned* cmd, unsigned dw, unsigned off, unsigned count,
                                 const void* count_at, unsigned stage, unsigned vtx, unsigned inst,
                                 unsigned flags);
int sceGnmDrawIndexIndirectCountMulti(unsigned* cmd, unsigned dw, unsigned off, unsigned count,
                                      const void* count_at, unsigned stage, unsigned vtx,
                                      unsigned inst, unsigned flags);
int sceGnmDispatchIndirect(unsigned* cmd, unsigned dw, unsigned off, unsigned flags);
int sceGnmDispatchIndirectOnMec(unsigned* cmd, unsigned dw, const void* args, unsigned flags);
int sceGnmRequestFlipAndSubmitDone(unsigned* cmd, unsigned dw, int handle, int index, int mode,
                                   long arg);

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

int sceCommonDialogInitialize(void);
int sceSaveDataDialogInitialize(void);
int sceSaveDataDialogOpen(const void* param);
int sceSaveDataDialogUpdateStatus(void);
int sceSaveDataDialogIsReadyToDisplay(void);
int sceSaveDataDialogClose(const void* param);
int sceSaveDataDialogGetResult(void* result);
int sceSaveDataDialogTerminate(void);

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
#define USER 0x10000000  // the one signed-in user (sceUserServiceGetInitialUser)

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

// The first n dwords of dcb are want's.
static int words(const unsigned* want, int n) {
  int same = 1;
  for (int i = 0; i < n; i++) same &= dcb[i] == want[i];
  return same;
}

// The draw and dispatch packets, as 11.00's driver writes them.
static int packet_checks(void) {
  int failed = 0;
  static unsigned short indices[8];
  static unsigned args[8], count;
  static unsigned objects[64] __attribute__((aligned(256)));
  const unsigned lo = (unsigned)(unsigned long)indices, hi = (unsigned)((unsigned long)indices >> 32);
  failed += test_check(400, sceGnmDrawIndexAuto(dcb, 7, 36, 1), 0);
  const unsigned auto_[5] = {0xC0012D01, 36, 2, 0xC0021000, 0};
  failed += test_check(401, words(auto_, 5), 1);
  failed += test_check(402, sceGnmDrawIndexAuto(dcb, 7, 36, 2), -1);  // flags past bit 0
  sceGnmDrawOpaqueAuto(dcb, 7, 0);
  const unsigned opaque[5] = {0xC0012D00, 0x10, 0x42, 0xC0021000, 0};
  failed += test_check(403, words(opaque, 5), 1);
  sceGnmDrawIndex(dcb, 10, 6, indices, 0);
  const unsigned index[8] = {0xC0042700, 6, lo, hi, 6, 0, 0xC0021000, 0};
  failed += test_check(404, words(index, 8), 1);
  failed += test_check(405, sceGnmDrawIndex(dcb, 10, 6, (char*)indices + 1, 0), -1);  // odd
  sceGnmDrawIndexOffset(dcb, 9, 10, 6, 1);
  const unsigned offset[7] = {0xC0033501, 6, 10, 6, 0, 0xC0021000, 0};
  failed += test_check(406, words(offset, 7), 1);
  failed += test_check(407, sceGnmDrawIndexMultiInstanced(dcb, 13, 6, 2, indices, objects) == 0 &&
                                dcb[0] == 0xC00B1000 && dcb[1] == 0 &&
                                sceGnmDrawIndexMultiInstanced(dcb, 13, 6, 2, indices, objects + 1) == -1,
                       1);
  sceGnmDispatchDirect(dcb, 9, 4, 2, 1, 0x19);
  const unsigned dispatch[7] = {0xC0031503, 4, 2, 1, 0x19, 0xC0021000, 0};
  failed += test_check(408, words(dispatch, 7), 1);
  failed += test_check(409, sceGnmDispatchDirect(dcb, 9, 0x80000000u, 1, 1, 0), -1);
  // Indirect draws: the stage's user data registers (VS 0x4C, ES 0xCC,
  // LS 0x14C) plus the SGPR, 0 for SGPR 0.
  sceGnmDrawIndirect(dcb, 9, 0x40, 2, 4, 5, 0);
  const unsigned indirect[7] = {0xC0032400, 0x40, 0x50, 0x51, 2, 0xC0021000, 0};
  failed += test_check(410, words(indirect, 7), 1);
  failed += test_check(411, sceGnmDrawIndirect(dcb, 9, 0x40, 0, 4, 5, 0) == -1 &&
                                sceGnmDrawIndirect(dcb, 9, 0x40, 2, 16, 5, 0) == -1,
                       1);
  sceGnmDrawIndexIndirect(dcb, 9, 0x40, 4, 0, 5, 0);
  const unsigned index_indirect[7] = {0xC0032500, 0x40, 0, 0xD1, 0, 0xC0021000, 0};
  failed += test_check(412, words(index_indirect, 7), 1);
  sceGnmDrawIndirectMulti(dcb, 11, 0x40, 3, 6, 1, 2, 1);
  const unsigned multi[9] = {0xC0052C01, 0x40, 0x14D, 0x14E, 3, 0x10, 2, 0xC0021000, 0};
  failed += test_check(413, words(multi, 9), 1);
  sceGnmDrawIndexIndirectMulti(dcb, 11, 0x40, 3, 2, 1, 2, 0);
  const unsigned index_multi[9] = {0xC0053800, 0x40, 0x4D, 0x4E, 3, 0x14, 0, 0xC0021000, 0};
  failed += test_check(414, words(index_multi, 9), 1);
  // Count multi: a NOP of 3 dwords first (its last left alone), then the
  // count's address and its enable (bit 30).
  dcb[2] = 0x12345678;
  sceGnmDrawIndirectCountMulti(dcb, 16, 0x40, 3, &count, 2, 4, 5, 0);
  const unsigned clo = (unsigned)(unsigned long)&count, chi = (unsigned)((unsigned long)&count >> 32);
  const unsigned count_multi[15] = {0xC0011000, 0,   0x12345678, 0xC0089C00, 0x40,
                                    0x50,       0x51, 1u << 30,  3,          clo,
                                    chi,        0x10, 2,         0xC0011000, 0};
  failed += test_check(415, words(count_multi, 15), 1);
  sceGnmDrawIndexIndirectCountMulti(dcb, 16, 0x40, 3, 0, 2, 4, 5, 0);
  failed += test_check(416, dcb[3] == 0xC0089D00 && dcb[7] == 0 && dcb[11] == 0x14 && dcb[12] == 0, 1);
  sceGnmDispatchIndirect(dcb, 7, 0x20, 0x8);
  const unsigned dispatch_indirect[5] = {0xC0011602, 0x20, 0x9, 0xC0021000, 0};
  failed += test_check(417, words(dispatch_indirect, 5), 1);
  sceGnmDispatchIndirectOnMec(dcb, 8, args, 0);
  const unsigned alo = (unsigned)(unsigned long)args, ahi = (unsigned)((unsigned long)args >> 32);
  const unsigned on_mec[6] = {0xC0021602, alo, ahi, 1, 0xC0021000, 0};
  failed += test_check(418, words(on_mec, 6), 1);
  failed += test_check(419, sceGnmDispatchIndirectOnMec(dcb, 8, (char*)args + 2, 0), -1);
  // A flip on its own: 256 dwords at least; the flip packet written into
  // the first 64, then the driver's checks.
  failed += test_check(420, (unsigned)sceGnmRequestFlipAndSubmitDone(dcb, 0xFF, 1, 0, 1, 0), 0x80D11001);
  failed += test_check(421, (unsigned)sceGnmRequestFlipAndSubmitDone(dcb, 0x100, 1, -1, 1, 0) == 0x80D11082 &&
                                dcb[0] == 0xC03E1000 && dcb[1] == 0x68750777,
                       1);
  return failed;
}

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
  SaveMount m = {USER, 0, 0, "SAVE0001", 0, 96, 2 | 4};  // read-write, create
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
  } cond = {USER, 0, 0, "SAVE%", 0, 0};
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
  } del = {USER, 0, 0, "SAVE0001"};
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

// ---- the pad: the user's controller, as the test's pad script holds it
// (orbis_process_test.cpp): cross and circle, the left stick left and the
// right stick down.

static int pad_checks(void) {
  int failed = 0;
  unsigned char data[2][0x80];
  failed += test_check(140, scePadInit(), 0);
  const int h = scePadOpen(USER, 0, 0, 0);
  failed += test_check(141, h, 1);
  failed += test_check(142, (unsigned)scePadOpen(USER, 0, 0, 0), 0x80920004);  // opened already
  failed += test_check(143, (unsigned)scePadRead(h, data, 0), 0x80920001);
  failed += test_check(144, scePadRead(h, data, 2), 1);
  failed += test_check(145, *(unsigned*)data[0] == 0x6000 && data[0][4] == 0 &&
                                data[0][5] == 0x80 && data[0][6] == 0x80 && data[0][7] == 0xFF &&
                                data[0][0x4C] == 1,
                       1);  // connected
  return failed;
}

// ---- the save data dialog: the system's list and messages, which take
// the controller while open. The pad script holds its buttons from the
// start, so the list stays open (a button counts only once released)
// until the game closes it.

typedef struct {
  unsigned long size;  // SceCommonDialogBaseParam: 48
  char reserved[36];
  unsigned magic;  // 0xC0D1A109 + the low 32 bits of its address
  unsigned param_size, mode, type, pad;
  void* anim;
  void* items;
  void* user_msg;
  void* sys_msg;
  void* error_code;
  void* progress;
  unsigned long user_data;
  void* option;
  void* mode67;
  char reserved2[16];
} DialogParam;
typedef struct {
  int user, pad;
  const char* title;
  const char (*dirs)[32];
  unsigned dir_count, pad2;
  void* new_item;
  unsigned focus, pad3;
  const char* focus_dir;
  unsigned style;
  char reserved[36];
} DialogItems;
typedef struct {
  unsigned type, pad;
  unsigned long value;
  char reserved[32];
} DialogSysMsg;
typedef struct {
  unsigned mode;
  int result;
  unsigned button, pad;
  char* dir;
  unsigned char* param;
  unsigned long user_data;
  char reserved[32];
} DialogResult;

static void dialog_param(DialogParam* p, unsigned mode, unsigned type, DialogItems* items) {
  char* b = (char*)p;
  for (unsigned long i = 0; i < sizeof *p; i++) b[i] = 0;
  p->size = 48;
  p->magic = 0xC0D1A109u + (unsigned)(unsigned long)p;
  p->param_size = 152;
  p->mode = mode, p->type = type, p->items = items, p->user_data = 77;
}

static int dialog_checks(void) {
  int failed = 0;
  static const char dirs[2][32] = {"NOSUCHDIR", "SAVE0002"};
  SaveMount m = {USER, 0, 0, "SAVE0002", 0, 96, 2 | 4};
  SaveMountResult mr = {0};
  sceSaveDataMount(&m, &mr);
  sceSaveDataSetParam("/savedata0", 1, "A save", 7);
  sceSaveDataUmount("/savedata0");
  DialogItems items = {USER, 0, 0, dirs, 2};
  DialogParam p;
  dialog_param(&p, 1, 2, &items);  // a list for loading
  failed += test_check(210, (unsigned)sceSaveDataDialogOpen(&p), 0x80B80003);  // not initialised
  failed += test_check(211, sceCommonDialogInitialize(), 0);
  failed += test_check(212, sceSaveDataDialogInitialize(), 0);
  failed += test_check(213, (unsigned)sceSaveDataDialogInitialize(), 0x80B80004);
  failed += test_check(214, (unsigned)sceSaveDataDialogOpen(0), 0x80B8000D);
  p.magic++;
  failed += test_check(215, (unsigned)sceSaveDataDialogOpen(&p), 0x80B8000A);
  p.magic--;
  items.focus_dir = "SAVE0002";  // a directory to focus, at the head of the list
  failed += test_check(216, (unsigned)sceSaveDataDialogOpen(&p), 0x80B8000A);
  items.focus_dir = 0;
  DialogSysMsg msg = {5};  // progress
  p.sys_msg = &msg;  // not the list's
  failed += test_check(217, (unsigned)sceSaveDataDialogOpen(&p), 0x80B8000A);
  p.sys_msg = 0;
  unsigned close[9] = {0};
  failed += test_check(218, (unsigned)sceSaveDataDialogClose(close), 0x80B8000B);  // none open
  failed += test_check(219, sceSaveDataDialogOpen(&p), 0);
  failed += test_check(220, sceSaveDataDialogUpdateStatus(), 2);  // running
  failed += test_check(221, sceSaveDataDialogIsReadyToDisplay(), 1);
  DialogResult r = {0};
  failed += test_check(222, (unsigned)sceSaveDataDialogGetResult(&r), 0x80B80005);
  unsigned char data[0x80];
  scePadRead(1, data, 1);
  failed += test_check(223, *(unsigned*)data == 0x80000000 && data[4] == 0x80, 1);  // intercepted
  failed += test_check(224, sceSaveDataDialogClose(close), 0);
  failed += test_check(225, sceSaveDataDialogUpdateStatus(), 3);  // finished
  char dir[32] = "x";
  r.dir = dir;
  failed += test_check(226, sceSaveDataDialogGetResult(&r), 0);
  failed += test_check(227, r.mode == 1 && r.result == 0 && dir[0] == 0 && r.user_data == 77, 1);
  scePadRead(1, data, 1);
  failed += test_check(228, *(unsigned*)data, 0x6000);  // the game's again
  // The progress message, about the one directory that exists.
  dialog_param(&p, 3, 2, &items);
  p.sys_msg = &msg;
  failed += test_check(229, sceSaveDataDialogOpen(&p), 0);
  failed += test_check(230, sceSaveDataDialogClose(close), 0);
  r.dir = 0;
  failed += test_check(231, sceSaveDataDialogGetResult(&r) == 0 && r.mode == 3 && r.result == 0, 1);
  failed += test_check(232, sceSaveDataDialogTerminate(), 0);
  failed += test_check(233, (unsigned)sceSaveDataDialogTerminate(), 0x80B80003);
  struct {
    int user, pad;
    const char* title;
    const char* dir;
    unsigned unused;
    char reserved[32];
  } del = {USER, 0, 0, "SAVE0002"};
  sceSaveDataDelete(&del);
  return failed;
}

// ---- trophies, kept per service label.

static int trophy_checks(void) {
  int failed = 0, ctx = 0, h = 0, platinum = 0;
  failed += test_check(150, sceNpTrophyCreateContext(&ctx, USER, 0, 0), 0);
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

  // ATRAC9 setups on a sampler rack with one ATRAC9 decoder of its own and
  // no AJM ones (flag 1 asks for those).
  unsigned opt[0xA8 / 4] = {0xA8};
  opt[0x1c / 4] = 256, opt[0x20 / 4] = 1, opt[0x28 / 4] = 1, opt[0x2c / 4] = 1;
  opt[0x80 / 4] = 1, opt[0x88 / 4] = 4, opt[0x94 / 4] = 1, opt[0x98 / 4] = 2, opt[0xa0 / 4] = 8;
  unsigned long a9rack = 0, a9v = 0;
  failed += test_check(195, sceNgs2RackCreateWithAllocator(sys, 0x1000, opt, &a, &a9rack), 0);
  sceNgs2RackGetVoiceHandle(a9rack, 0, &a9v);
  // Stereo (channel config 2), 48 kHz, 4 frames of 0x100 bytes per superframe.
  Ngs2Setup a9 = {{40, 0, 0x10000000}, 0x40, 2, 48000, 0xFE741FF0, 0, 0, 1, 0};
  failed += test_check(196, (unsigned)sceNgs2VoiceControl(a9v, &a9), 0x804A0402);
  a9.flags = 0, a9.config = 0x12345678;
  failed += test_check(197, (unsigned)sceNgs2VoiceControl(a9v, &a9), 0x804A0403);
  a9.config = 0xFE741FF0, a9.channels = 1;
  failed += test_check(198, (unsigned)sceNgs2VoiceControl(a9v, &a9), 0x804A0052);
  a9.channels = 2;
  failed += test_check(199, sceNgs2VoiceControl(a9v, &a9), 0);
  failed += test_check(200, sceNgs2RackDestroy(a9rack, 0), 0);

  failed += test_check(188, sceNgs2RackDestroy(sampler, 0), 0);
  failed += test_check(189, sceNgs2RackDestroy(master, 0), 0);
  failed += test_check(190, sceNgs2SystemDestroy(sys, 0), 0);
  failed += test_check(191, ngs2_frees, 4);
  return failed;
}

// ---- the message dialog (the test's ELFs carry no SDK version: the
// library's checks and answers for SDKs before 1.50)

typedef struct {
  unsigned long size;  // SceCommonDialogBaseParam
  char reserved[36];
  unsigned magic;
  unsigned long param_size;  // 0x88
  int mode, pad;
  void* user;
  void* bar;
  void* sys;
  int user_id;
  char reserved2[40];
  int pad2;
} MsgParam;
typedef struct {
  unsigned type, pad;
  const char* msg;
  void* buttons;
  char reserved[24];
} MsgUser;
typedef struct {
  unsigned type, pad;
  const char* msg;
  char reserved[64];
} MsgBar;
typedef struct {
  int mode, result, button;
  char reserved[32];
} MsgResult;
int sceMsgDialogInitialize(void);
int sceMsgDialogOpen(const MsgParam* p);
int sceMsgDialogUpdateStatus(void);
int sceMsgDialogGetStatus(void);
int sceMsgDialogGetResult(MsgResult* r);
int sceMsgDialogClose(void);
int sceMsgDialogTerminate(void);
int sceMsgDialogProgressBarSetValue(int target, unsigned rate);
int sceMsgDialogProgressBarInc(int target, unsigned delta);

static void msg_param(MsgParam* p, int mode, void* user, void* bar) {
  char* b = (char*)p;
  for (unsigned long i = 0; i < sizeof *p; i++) b[i] = 0;
  p->size = 48;
  p->magic = 0xC0D1A109u + (unsigned)(unsigned long)p;
  p->param_size = 0x88;
  p->mode = mode, p->user = user, p->bar = bar;
}

static int msg_dialog_checks(void) {
  int failed = 0;
  MsgUser u = {0, 0, "Saved."};  // OK
  MsgParam p;
  msg_param(&p, 1, &u, 0);
  failed += test_check(240, (unsigned)sceMsgDialogOpen(0), 0x80B8000D);
  failed += test_check(241, (unsigned)sceMsgDialogOpen(&p), 0x80B80003);  // params first
  failed += test_check(242, sceMsgDialogInitialize(), 0);
  failed += test_check(243, (unsigned)sceMsgDialogInitialize(), 0x80B80004);
  p.magic++;
  failed += test_check(244, (unsigned)sceMsgDialogOpen(&p), 0x80B8000A);
  p.magic--;
  MsgBar bar = {0, 0, "Saving"};
  p.bar = &bar;  // not the mode's
  failed += test_check(245, (unsigned)sceMsgDialogOpen(&p), 0x80B8000A);
  p.bar = 0;
  failed += test_check(246, sceMsgDialogOpen(&p), 0);
  failed += test_check(247, (unsigned)sceMsgDialogOpen(&p), 0x80B80006);  // running
  failed += test_check(248, sceMsgDialogUpdateStatus(), 2);
  failed += test_check(249, (unsigned)sceMsgDialogProgressBarSetValue(0, 50), 0x80B8000F);
  MsgResult r = {0};
  failed += test_check(250, (unsigned)sceMsgDialogGetResult(&r), 0x80B80005);
  failed += test_check(251, sceMsgDialogClose(), 0);
  failed += test_check(252, (unsigned)sceMsgDialogClose(), 0x80B8000C);
  failed += test_check(253, sceMsgDialogGetStatus(), 2);  // until UpdateStatus sees it
  failed += test_check(254, sceMsgDialogUpdateStatus(), 3);
  // Closed by the game: backed out of (no SDK 3.00 fix-up here).
  failed += test_check(255, sceMsgDialogGetResult(&r) == 0 && r.mode == 1 && r.result == 1 &&
                                r.button == 0, 1);
  // A progress bar, opened again from finished.
  msg_param(&p, 2, 0, &bar);
  failed += test_check(256, sceMsgDialogOpen(&p), 0);
  failed += test_check(257, sceMsgDialogProgressBarInc(0, 250) * 10 +
                                (unsigned)sceMsgDialogProgressBarSetValue(1, 5), 0x80B8000A);
  failed += test_check(258, sceMsgDialogTerminate(), 0);  // also while open
  failed += test_check(259, (unsigned)sceMsgDialogTerminate(), 0x80B80003);
  return failed;
}

// ---- add-on content and temporary data: a game installed in full, with
// none.

int sceAppContentInitialize(const void* init, void* boot);
int sceAppContentAppParamGetInt(unsigned id, int* value);
int sceAppContentGetAddcontInfoList(unsigned label, void* list, unsigned num, unsigned* hits);
int sceAppContentTemporaryDataMount2(unsigned option, char* mount_point);

static int appcontent_checks(void) {
  int failed = 0;
  int v = 0;
  failed += test_check(270, (unsigned)sceAppContentAppParamGetInt(0, &v), 0x80D90001);
  char init[32] = {0}, boot[40];
  for (int i = 0; i < 40; i++) boot[i] = 1;
  failed += test_check(271, sceAppContentInitialize(init, boot) * 10 + boot[4], 0);  // attr 0
  failed += test_check(272, sceAppContentAppParamGetInt(0, &v) * 10 + v, 3);  // the full SKU
  unsigned hits = 9;
  failed += test_check(273, sceAppContentGetAddcontInfoList(0, 0, 0, &hits) * 10 + hits, 0);
  char mp[16];
  failed += test_check(274, sceAppContentTemporaryDataMount2(0, mp), 0);
  failed += test_check(275, streq(mp, "/temp0"), 1);
  return failed;
}

int system_checks(void) {
  return fiber_checks() + dir_checks() + gpu_checks() + packet_checks() + save_checks() + audio_checks() +
         pad_checks() + dialog_checks() + msg_dialog_checks() + trophy_checks() + ngs2_checks() +
         appcontent_checks();
}
