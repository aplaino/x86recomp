// orbis_process_test's checks of libSceLibcInternal's and libSceRtc's HLE
// (orbis/hle/libc_internal.cpp, rtc.cpp), from lifted code: the C
// library the system's middleware imports, and the clock's ticks,
// parsers and formatters. libc_checks() returns how many failed.

#include <stdarg.h>

int test_check(int id, long got, long want);

// libSceLibcInternal
unsigned long strlen(const char* s);
int strcmp(const char* a, const char* b);
void* memmove(void* d, const void* s, unsigned long n);
int vsnprintf(char* b, unsigned long n, const char* f, va_list ap);
int memcpy_s(void* d, unsigned long dmax, const void* s, unsigned long n);
int rand(void);
void srand(unsigned s);
typedef struct {
  int sec, min, hour, mday, mon, year, wday, yday, isdst;
} Tm;
Tm* gmtime_s(const long* t, Tm* tm);
long mktime(Tm* tm);
int __cxa_guard_acquire(unsigned long* g);
void __cxa_guard_release(unsigned long* g);
int __cxa_atexit(void (*fn)(void*), void* arg, void* dso);
void __cxa_finalize(void* dso);
short _Dtest(double* x);
short _FExp(float* x, float y, long eoff);
float _FSin(float x, unsigned q);
extern float _FInf[4];
void* _Znwm(unsigned long n);
void _ZdlPv(void* p);
float powf(float x, float y);
double ldexp(double x, int e);

// libSceRtc
typedef struct {
  unsigned short year, month, day, hour, minute, second;
  unsigned microsecond;
} DateTime;
int sceRtcGetTick(const DateTime* dt, unsigned long* tick);
int sceRtcSetTick(DateTime* dt, const unsigned long* tick);
int sceRtcCheckValid(const DateTime* dt);
int sceRtcParseRFC3339(unsigned long* utc, const char* s);
int sceRtcParseDateTime(unsigned long* utc, const char* s);
int sceRtcFormatRFC3339(char* buf, const unsigned long* utc, int tz);
int sceRtcFormatRFC2822(char* buf, const unsigned long* utc, int tz);
int sceRtcGetDayOfWeek(int y, int m, int d);
int sceRtcTickAddMonths(unsigned long* out, const unsigned long* in, int months);
int sceRtcGetDosTime(const DateTime* dt, unsigned* dos);

static int same(const char* a, const char* b) {
  while (*a && *a == *b) a++, b++;
  return *a == *b;
}

static int fmt(char* b, unsigned long n, const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  int r = vsnprintf(b, n, f, ap);
  va_end(ap);
  return r;
}

static int exits;
static void on_exit_fn(void* arg) { exits = exits * 100 + (int)(long)arg; }
extern int prx_inited;  // libTestPrx's

static int c_checks(void) {
  int failed = 0;
  char buf[32] = "abcdef";
  failed += test_check(300, strlen("hello"), 5);
  failed += test_check(301, strcmp("abc", "abd") < 0, 1);
  memmove(buf + 1, buf, 5);
  failed += test_check(302, buf[1] == 'a' && buf[5] == 'e', 1);
  // 303-304: vsnprintf: the whole length, n - 1 characters and a NUL; %n,
  // and an unknown conversion printed as itself.
  failed += test_check(303, fmt(buf, 6, "%d-%s", 1234, "xyz") * 100 + (buf[4] == '-') * 10 + !buf[5],
                       811);
  int n = 0;
  fmt(buf, sizeof buf, "ab%n%y", &n);
  failed += test_check(304, n == 2 && same(buf, "ab%y"), 1);
  srand(7);
  const int r1 = rand();
  failed += test_check(305, r1 == 708685117 && rand() == 854532251, 1);
  // 306-307: memcpy_s: too small clears the destination (ERANGE).
  char dst[4] = "xyz";
  failed += test_check(306, memcpy_s(dst, 2, "hello", 5) * 10 + dst[0] + dst[1], 340);
  failed += test_check(307, memcpy_s(dst, 4, "abc", 4) * 10 + (dst[1] == 'b'), 1);
  // 308-309: struct tm.
  long t = 365 * 86400L;
  Tm tm;
  gmtime_s(&t, &tm);
  failed += test_check(308, tm.year * 10000 + tm.mon * 1000 + tm.mday * 10 + tm.wday + tm.isdst,
                       710014);
  Tm m = {0, 0, 0, 32, 0, 100, 0, 0, 0};  // January 32nd, 2000
  failed += test_check(309, mktime(&m), 949363200);
  failed += test_check(310, m.mon * 100 + m.mday * 10 + m.wday, 112);
  // 311-313: a static initialisation guard; atexit handlers by module
  // (registered with it, or their function in it), newest first.
  unsigned long g = 0;
  failed += test_check(311, __cxa_guard_acquire(&g), 1);
  __cxa_guard_release(&g);
  failed += test_check(312, __cxa_guard_acquire(&g) * 10 + (g == 1), 1);
  static int dso_a, dso_b;
  __cxa_atexit(on_exit_fn, (void*)1, &dso_a);
  __cxa_atexit(on_exit_fn, (void*)10, &dso_b);
  __cxa_finalize(&prx_inited);  // another module's: neither
  __cxa_finalize(&dso_a);       // the eboot's: both
  failed += test_check(313, exits, 1001);
  // 314-317: the math internals and data.
  double one = 1, tiny = 1e-310;
  failed += test_check(314, _Dtest(&one) * 10 + _Dtest(&tiny), -12);
  float x = 1;
  const short code = _FExp(&x, 1, 0);
  failed += test_check(315, code * 10 + (*(unsigned*)&x == 0x402DF854), -9);
  failed += test_check(316, _FSin(0, 1) == 1 && *(unsigned*)_FInf == 0x7F800000, 1);
  failed += test_check(317, (long)powf(2, 10) + (long)ldexp(1.5, 3), 1036);
  void* p = _Znwm(0);
  failed += test_check(318, p != 0, 1);
  _ZdlPv(p);
  return failed;
}

static int rtc_checks(void) {
  int failed = 0;
  DateTime d = {2024, 1, 1, 0, 0, 0, 0};
  unsigned long tick = 0;
  failed += test_check(330, sceRtcGetTick(&d, &tick), 0);
  failed += test_check(331, tick == 63839664000000000ul, 1);
  tick += 86400000000ul * 59 + 3661000005ul;  // 2024-02-29 01:01:01.000005
  sceRtcSetTick(&d, &tick);
  failed += test_check(332, d.month * 1000000 + d.day * 10000 + d.hour * 100 + d.second +
                                d.microsecond * 0, 2290101);
  failed += test_check(333, d.microsecond, 5);
  DateTime bad = {2023, 2, 29, 0, 0, 0, 0};
  failed += test_check(334, (unsigned)sceRtcCheckValid(&bad), 0x80B5000A);
  failed += test_check(335, sceRtcParseRFC3339(&tick, "2024-02-29T12:34:56.789+09:00"), 0);
  failed += test_check(336, tick == 63844774496789000ul, 1);
  failed += test_check(337, (unsigned)sceRtcParseRFC3339(&tick, "2024-02-29 12:34:56Z"),
                       0x80B50007);
  unsigned long t2 = 0;
  sceRtcParseDateTime(&t2, "  Thu, 29 Feb 2024 12:34:56 GMT");
  failed += test_check(338, t2 == 63844806896000000ul, 1);
  sceRtcParseDateTime(&t2, "Thursday, 29 Feb 2024 12:34:56 -0130");
  failed += test_check(339, t2 == 63844812296000000ul, 1);
  char buf[40];
  sceRtcFormatRFC3339(buf, &tick, 540);
  failed += test_check(340, same(buf, "2024-02-29T12:34:56.78+09:00"), 1);
  sceRtcFormatRFC2822(buf, &tick, 0);
  failed += test_check(341, same(buf, "Thu, 29 Feb 2024 03:34:56 +0000"), 1);
  failed += test_check(342, sceRtcGetDayOfWeek(2024, 1, 1), 1);
  unsigned long jan31 = 63842292000000000ul, out = 0;
  sceRtcTickAddMonths(&out, &jan31, 1);
  failed += test_check(343, out == 63844797600000000ul, 1);  // clamped to the 29th
  DateTime dos_dt = {2024, 2, 29, 12, 34, 56, 0};
  unsigned dos = 0;
  sceRtcGetDosTime(&dos_dt, &dos);
  failed += test_check(344, dos, 1482515548);
  return failed;
}

int libc_checks(void) { return c_checks() + rtc_checks(); }
