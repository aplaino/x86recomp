// orbis_process_test's checks of the other system libraries' HLE, from
// lifted code: fibers (libSceFiber), directory reads and event queues
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

int system_checks(void) { return fiber_checks() + dir_checks() + gpu_checks(); }
