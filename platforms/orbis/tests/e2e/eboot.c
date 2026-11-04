// The eboot of orbis_process_test: built for FreeBSD x86-64 with kernel.c,
// made an SCE ELF (tools/orbis_sce_elf.py), lifted and run by the test.
// Each check reports to the test's HLE (libTest); _start returns how many
// failed.

// libTest (the test's HLE)
int test_check(int id, long got, long want);
int test_double(int id, double d, long n);  // d in xmm0, n in rsi
extern int test_value;                      // a data export: 1234

// kernel.c: the libkernel HLE's checks (40 on); system.c: the other
// libraries' (70 on).
int kernel_checks(void);
int system_checks(void);

// libTestPrx (prx.c)
int prx_add(int a, int b);
int prx_tls_bump(void);
extern int prx_inited;

// libkernel's module loading (the eboot loads libTestLoad, load.c).
int sceKernelLoadStartModule(const char* path, unsigned long args, const void* argp,
                             unsigned flags, const void* opt, int* result);
int sceKernelDlsym(int handle, const char* symbol, void** address);

static __thread int tls_counter = 5;
static __thread long tls_zero;

// Distinct calls per case keep this a jump table.
__attribute__((noinline)) static long op(int k, long x) { return x * 10 + k; }
__attribute__((noinline)) static long sw(int k, long x) {
  switch (k) {
    case 0: return op(1, x);
    case 1: return op(2, x) + 1;
    case 2: return op(3, x) - 1;
    case 3: return op(4, x) * 2;
    case 4: return op(5, x) ^ 3;
    case 5: return op(6, x) + 7;
    case 6: return op(7, x) << 1;
    default: return -1;
  }
}

// libTestPrx's setjmp and longjmp (prx.c), found by the lifter by NID.
int setjmp(long* b) __attribute__((returns_twice));
__attribute__((noreturn)) void longjmp(long* b, int v);

// longjmp from `depth` frames below the setjmp caller.
__attribute__((noinline)) static int thrower(long* b, int v, int depth) {
  if (depth) return thrower(b, v, depth - 1) + 1;
  longjmp(b, v);
}

// setjmp, then a call that longjmps back with v (0: no longjmp, and an
// inner protected call that returns normally first when nest is set).
__attribute__((noinline)) static int protected_call(int v, int depth, int nest) {
  long b[8];
  int r = setjmp(b);
  if (r) return r;
  if (nest && protected_call(0, 0, 0) != -1) return -2;
  if (v) thrower(b, v, depth);
  return -1;
}

struct args {
  long argc;
  char* argv[];
};

static int streq(const char* a, const char* b) {
  while (*a && *a == *b) a++, b++;
  return *a == *b;
}

long _start(struct args* a, void (*cleanup)(void)) {
  int failed = 0;
  failed += test_check(1, a->argc, 2);
  failed += test_check(2, streq(a->argv[1], "hello"), 1);
  failed += test_check(3, a->argv[2] == 0, 1);
  failed += test_check(4, cleanup != 0, 1);
  cleanup();

  void* tp;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(tp));
  failed += test_check(5, *(void**)tp == tp, 1);  // tcb_self
  failed += test_check(6, (char*)&tls_counter < (char*)tp, 1);
  failed += test_check(7, tls_counter, 5);  // from the template
  failed += test_check(8, tls_zero, 0);
  tls_counter += 2;
  failed += test_check(9, tls_counter, 7);

  failed += test_check(10, test_value, 1234);
  failed += test_check(11, prx_inited, 1);  // the PRX's _init ran first
  failed += test_check(12, prx_add(40, 2), 42);
  failed += test_check(13, prx_tls_bump(), 101);  // general-dynamic TLS
  failed += test_check(14, prx_tls_bump(), 102);
  failed += test_double(15, 2.5, 3);

  static const long want[] = {11, 13, 12, 28, 12, 23, 34, -1};
  for (int k = 0; k < 8; k++) failed += test_check(16 + k, sw(k, 1), want[k]);

  failed += test_check(24, protected_call(7, 3, 0), 7);   // back past three frames
  failed += test_check(25, protected_call(0, 0, 0), -1);  // no longjmp
  failed += test_check(26, protected_call(9, 0, 1), 9);   // after an inner one returned
  failed += test_check(27, protected_call(0x80000000, 1, 0), (int)0x80000000);

  // 28-31: a PRX loaded and started here, and a function found in it.
  int arg = 7, started = 0;
  int h = sceKernelLoadStartModule("/app0/sce_module/libTestLoad.prx", 4, &arg, 0, 0, &started);
  failed += test_check(28, h > 1, 1);  // after the eboot and libTestPrx
  failed += test_check(29, started, 11);  // its _init's result
  int (*mul)(int, int) = 0;
  failed += test_check(30, sceKernelDlsym(h, "load_mul", (void**)&mul), 0);
  failed += test_check(31, mul(6, 7), 46);
  return failed + kernel_checks() + system_checks();
}
