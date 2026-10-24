// The eboot of orbis_process_test: built for FreeBSD x86-64, made an SCE
// ELF (tools/orbis_sce_elf.py), lifted and run by the test. Each check
// reports to the test's HLE (libTest); _start returns how many failed.

// libTest (the test's HLE)
int test_check(int id, long got, long want);
int test_double(int id, double d, long n);  // d in xmm0, n in rsi
extern int test_value;                      // a data export: 1234

// libTestPrx (prx.c)
int prx_add(int a, int b);
int prx_tls_bump(void);
extern int prx_inited;

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
  return failed;
}
