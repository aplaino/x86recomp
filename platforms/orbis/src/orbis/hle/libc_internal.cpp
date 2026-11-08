// libSceLibcInternal HLE: the firmware's own C library, which system
// libraries that ship with games (and the games' PRXs) import; a game's
// libc is its own PRX, lifted with it.

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "orbis/process.h"
#include "runtime/guest_alloc.h"
#include "runtime/log.h"
#include "x86/runtime.h"

// The library's version marker (a symbol that modules link against).
ORBIS_DATA("libSceLibcInternal", "Need_sceLibcInternal", need_libc_internal, 8, nullptr);
// stdout and stderr as FILEs (0x1C8 bytes): opaque blocks that the stdio
// handlers recognise.
static recomp::gaddr g_stdout = 0, g_stderr = 0;
ORBIS_DATA("libSceLibcInternal", "_Stdout", stdout_file, 0x1C8,
           [](recomp::gaddr a) { g_stdout = a; });
ORBIS_DATA("libSceLibcInternal", "_Stderr", stderr_file, 0x1C8,
           [](recomp::gaddr a) { g_stderr = a; });
// Float infinity and quiet NaN, each in a 16-byte slot.
ORBIS_DATA("libSceLibcInternal", "_FInf", f_inf, 16,
           [](recomp::gaddr a) { recomp::Wr32(a, 0x7F800000); });
ORBIS_DATA("libSceLibcInternal", "_FNan", f_nan, 16,
           [](recomp::gaddr a) { recomp::Wr32(a, 0x7FC00000); });

namespace {

// The library's heap (malloc and free): its own, in guest memory.
recomp::GuestHeap& Heap() {
  static recomp::GuestHeap* h = new recomp::GuestHeap(recomp::g_proc->vm, 1u << 20, 0);
  return *h;
}

// The arguments of a System V va_list (gp_offset, fp_offset,
// overflow_arg_area, reg_save_area): integers from the saved registers,
// then the stack; floating point likewise from the saved XMM registers.
struct VaList {
  recomp::gaddr va;
  uint64_t Int() {
    const uint32_t gp = recomp::Rd32(va);
    if (gp < 48) {
      recomp::Wr32(va, gp + 8);
      return recomp::Rd64(recomp::Rd64(va + 16) + gp);
    }
    return Overflow();
  }
  double Float() {
    const uint32_t fp = recomp::Rd32(va + 4);
    uint64_t bits;
    if (fp < 176) {
      recomp::Wr32(va + 4, fp + 16);
      bits = recomp::Rd64(recomp::Rd64(va + 16) + fp);
    } else {
      bits = Overflow();
    }
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
  }
  uint64_t Overflow() {
    const recomp::gaddr at = recomp::Rd64(va + 8);
    recomp::Wr64(va + 8, at + 8);
    return recomp::Rd64(at);
  }
};

// The variadic arguments of a call from argument `first` on (System V:
// integers in the six registers, floats in xmm0-7, then both on the
// stack in order).
struct CallArgs {
  recomp::Ctx& c;
  int gp, fp = 0, stack = 0;
  uint64_t Int() { return gp < 6 ? recomp::hle::Arg(c, gp++) : Stack(); }
  double Float() {
    if (fp < 8) return recomp::hle::ArgD(c, fp++);
    const uint64_t bits = Stack();
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
  }
  uint64_t Stack() { return recomp::hle::Arg(c, 6 + stack++); }
};

// A host snprintf's whole output.
template <typename... T>
std::string Snprintf(const std::string& spec, T... v) {
  const int n = std::snprintf(nullptr, 0, spec.c_str(), v...);
  std::string out(size_t(n > 0 ? n : 0), '\0');
  std::snprintf(out.data(), out.size() + 1, spec.c_str(), v...);
  return out;
}

// printf's conversions over a guest format and va_list, as the console's
// libc has them: flags, width and precision (also from arguments), the
// length modifiers, d i u o x X c s p n e E f F g G a A and %%, a NULL
// %s as "(null)", and an unknown conversion as itself after '%'; each
// conversion formatted by the host's. Wide characters (%lc, %ls, %S) and
// positional arguments (%N$) are not implemented.
template <typename Args>
std::string Format(const std::string& fmt, Args& a) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    std::string spec = "%";
    size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) spec += fmt[j++];
    auto number = [&] {
      if (j < fmt.size() && fmt[j] == '*') {
        spec += std::to_string(int32_t(a.Int()));
        j++;
        return;
      }
      while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') spec += fmt[j++];
    };
    number();
    if (j < fmt.size() && fmt[j] == '.') {
      spec += fmt[j++];
      number();
    }
    std::string len;
    while (j < fmt.size() && std::strchr("hljztL", fmt[j])) len += fmt[j++];
    if (j >= fmt.size()) {
      out += '%';
      break;
    }
    const char conv = fmt[j];
    i = j;
    if (conv == '$')
      recomp::FatalF("libSceLibcInternal printf: positional arguments not implemented");
    if (conv == 'S' || (len == "l" && (conv == 'c' || conv == 's')))
      recomp::FatalF("libSceLibcInternal printf: wide characters not implemented");
    if (len == "L") recomp::FatalF("libSceLibcInternal printf: long double not implemented");
    std::string buf;
    switch (conv) {
      case '%': out += '%'; continue;
      case 'd': case 'i': {
        int64_t v = int64_t(a.Int());
        if (len == "hh") v = int8_t(v);
        else if (len == "h") v = int16_t(v);
        else if (len.empty()) v = int32_t(v);
        buf = Snprintf(spec + "lld", (long long)v);
        break;
      }
      case 'u': case 'o': case 'x': case 'X': {
        uint64_t v = a.Int();
        if (len == "hh") v = uint8_t(v);
        else if (len == "h") v = uint16_t(v);
        else if (len.empty()) v = uint32_t(v);
        buf = Snprintf(spec + "ll" + conv, (unsigned long long)v);
        break;
      }
      case 'c': buf = Snprintf(spec + "c", int(a.Int())); break;
      case 's': {
        const recomp::gaddr p = a.Int();
        const std::string str = p ? recomp::GStr(p) : "(null)";
        buf = Snprintf(spec + "s", str.c_str());
        break;
      }
      case 'p': buf = Snprintf("0x%llx", (unsigned long long)a.Int()); break;
      case 'n': {  // the count so far, at the argument's size
        const recomp::gaddr p = a.Int();
        if (len == "hh") recomp::Wr8(p, uint8_t(out.size()));
        else if (len == "h") recomp::Wr16(p, uint16_t(out.size()));
        else if (len.empty()) recomp::Wr32(p, uint32_t(out.size()));
        else recomp::Wr64(p, out.size());
        continue;
      }
      case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
        buf = Snprintf(spec + conv, a.Float());
        break;
      default: buf = std::string("%") + conv; break;
    }
    out += buf;
  }
  return out;
}

// The host stream for a guest FILE: stdout and stderr only.
std::FILE* Stream(recomp::gaddr f) {
  if (f && f == g_stdout) return stdout;
  if (f && f == g_stderr) return stderr;
  recomp::FatalF("libSceLibcInternal: a stream other than stdout and stderr not implemented");
}

int64_t Write(std::FILE* f, const std::string& s) {
  std::fwrite(s.data(), 1, s.size(), f);
  std::fflush(f);
  return int64_t(s.size());
}

}  // namespace

using recomp::hle::Arg;

// int fflush(FILE* stream): stdout and stderr (or every stream: null) are
// the log's, written as they go.
ORBIS_HLE("libSceLibcInternal", fflush, 1) {
  const recomp::gaddr f = Arg(c, 0);
  if (f) {
    std::fflush(Stream(f));
  } else {
    std::fflush(stdout);
    std::fflush(stderr);
  }
  return 0;
}

// int vprintf(const char* format, va_list ap): to the log's stdout.
ORBIS_HLE("libSceLibcInternal", vprintf, 2) {
  VaList a{Arg(c, 1)};
  return Write(stdout, Format(recomp::GStr(Arg(c, 0)), a));
}

ORBIS_HLE("libSceLibcInternal", memcpy, 3) {
  std::memcpy(recomp::GPtr(Arg(c, 0)), recomp::GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
ORBIS_HLE("libSceLibcInternal", memset, 3) {
  std::memset(recomp::GPtr(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
ORBIS_HLE("libSceLibcInternal", malloc, 1) {
  const recomp::gaddr p = Heap().Alloc(Arg(c, 0) ? Arg(c, 0) : 1, false);
  if (!p) recomp::orbis::kernel::SetErrno(c, recomp::orbis::kernel::kENOMEM);
  return p;
}
ORBIS_HLE("libSceLibcInternal", free, 1) {
  if (Arg(c, 0) && !Heap().Free(Arg(c, 0)))
    recomp::FatalF("libSceLibcInternal free(%llX): not a block of its heap",
                   (unsigned long long)Arg(c, 0));
  return 0;
}

ORBIS_HLE("libSceLibcInternal", memmove, 3) {
  std::memmove(recomp::GPtr(Arg(c, 0)), recomp::GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
ORBIS_HLE("libSceLibcInternal", strlen, 1) {
  return std::strlen(reinterpret_cast<const char*>(recomp::GPtr(Arg(c, 0))));
}
ORBIS_HLE("libSceLibcInternal", strcmp, 2) {
  return uint64_t(int64_t(std::strcmp(reinterpret_cast<const char*>(recomp::GPtr(Arg(c, 0))),
                                      reinterpret_cast<const char*>(recomp::GPtr(Arg(c, 1))))));
}

// The C math functions, by the host's libm (results may differ from the
// console's in the last bit).
#define LIBC_MATH_F1(name) \
  ORBIS_HLE("libSceLibcInternal", name, 0) { return recomp::hle::RetF(c, ::name(recomp::hle::ArgF(c, 0))); }
#define LIBC_MATH_D1(name) \
  ORBIS_HLE("libSceLibcInternal", name, 0) { return recomp::hle::RetD(c, ::name(recomp::hle::ArgD(c, 0))); }
#define LIBC_MATH_F2(name)                                                                  \
  ORBIS_HLE("libSceLibcInternal", name, 0) {                                                \
    return recomp::hle::RetF(c, ::name(recomp::hle::ArgF(c, 0), recomp::hle::ArgF(c, 1))); \
  }
#define LIBC_MATH_D2(name)                                                                  \
  ORBIS_HLE("libSceLibcInternal", name, 0) {                                                \
    return recomp::hle::RetD(c, ::name(recomp::hle::ArgD(c, 0), recomp::hle::ArgD(c, 1))); \
  }
LIBC_MATH_F1(acosf)
LIBC_MATH_F1(atanf)
LIBC_MATH_F1(expf)
LIBC_MATH_F1(logbf)
LIBC_MATH_F1(roundf)
LIBC_MATH_F1(tanf)
LIBC_MATH_D1(atan)
LIBC_MATH_D1(log)
LIBC_MATH_D1(tan)
LIBC_MATH_F2(atan2f)
LIBC_MATH_F2(fmaxf)
LIBC_MATH_F2(hypotf)
LIBC_MATH_F2(powf)
LIBC_MATH_D2(pow)
// double ldexp(double x, int e), float ldexpf(float, int), scalbnf likewise.
ORBIS_HLE("libSceLibcInternal", ldexp, 1) {
  return recomp::hle::RetD(c, ::ldexp(recomp::hle::ArgD(c, 0), int(Arg(c, 0))));
}
ORBIS_HLE("libSceLibcInternal", ldexpf, 1) {
  return recomp::hle::RetF(c, ::ldexpf(recomp::hle::ArgF(c, 0), int(Arg(c, 0))));
}
ORBIS_HLE("libSceLibcInternal", scalbnf, 1) {
  return recomp::hle::RetF(c, ::scalbnf(recomp::hle::ArgF(c, 0), int(Arg(c, 0))));
}

// ---- stdio: stdout and stderr (the log's)

// int printf(const char* format, ...)
ORBIS_HLE("libSceLibcInternal", printf, 1) {
  CallArgs a{c, 1};
  return Write(stdout, Format(recomp::GStr(Arg(c, 0)), a));
}
// int fprintf(FILE* stream, const char* format, ...)
ORBIS_HLE("libSceLibcInternal", fprintf, 2) {
  std::FILE* f = Stream(Arg(c, 0));
  CallArgs a{c, 2};
  return Write(f, Format(recomp::GStr(Arg(c, 1)), a));
}
// int puts(const char* s): 0, not a count.
ORBIS_HLE("libSceLibcInternal", puts, 1) {
  Write(stdout, recomp::GStr(Arg(c, 0)) + "\n");
  return 0;
}
ORBIS_HLE("libSceLibcInternal", putchar, 1) {
  Write(stdout, std::string(1, char(Arg(c, 0))));
  return uint8_t(Arg(c, 0));
}
// int vsnprintf(char* buf, size_t n, const char* format, va_list ap): the
// whole length; at most n - 1 characters and a NUL stored (none for n 0).
ORBIS_HLE("libSceLibcInternal", vsnprintf, 4) {
  VaList a{Arg(c, 3)};
  const std::string s = Format(recomp::GStr(Arg(c, 2)), a);
  if (const uint64_t n = Arg(c, 1)) {
    const size_t k = std::min<uint64_t>(s.size(), n - 1);
    std::memcpy(recomp::GPtr(Arg(c, 0)), s.data(), k);
    recomp::Wr8(Arg(c, 0) + k, 0);
  }
  return s.size();
}

// ---- the C library's checked, random, process and time functions

// errno_t memcpy_s(void* dst, rsize_t dmax, const void* src, rsize_t n):
// on a violation, dst cleared (but for a bad dst), the default constraint
// handler's message on stderr, and the errno returned.
ORBIS_HLE("libSceLibcInternal", memcpy_s, 4) {
  using namespace recomp::orbis::kernel;
  const recomp::gaddr dst = Arg(c, 0), src = Arg(c, 2);
  const uint64_t dmax = Arg(c, 1), n = Arg(c, 3);
  auto fail = [&](const char* msg, int err, bool clear) {
    if (clear) std::memset(recomp::GPtr(dst), 0, dmax);
    Write(stderr, std::string(msg) + "\n");
    return uint64_t(err);
  };
  if (!dst || dmax >> 63) return fail("memcpy_s: bad destination argument", kEINVAL, false);
  if (!src || n >> 63) return fail("memcpy_s: bad argument", kEINVAL, true);
  if (n > dmax) return fail("memcpy_s: buffer too small", kERANGE, true);
  if (n && dst < src + n && src < dst + n) return fail("memcpy_s: buffer overlap", kEINVAL, true);
  std::memcpy(recomp::GPtr(dst), recomp::GPtr(src), n);
  return 0;
}

namespace {

// rand's state: one 64-bit seed for the process, unlocked.
uint64_t g_rand_seed = 1;

}  // namespace

// int rand(void): the seed's LCG step; 30 bits of its upper half.
ORBIS_HLE("libSceLibcInternal", rand, 0) {
  g_rand_seed = g_rand_seed * 0x5851F42D4C957F2Dull + 1;
  return (g_rand_seed >> 32) & 0x3FFFFFFF;
}
ORBIS_HLE("libSceLibcInternal", srand, 1) {
  g_rand_seed = uint32_t(Arg(c, 0));
  return 0;
}

// abort and a pure virtual call end the process at once (the console's
// fatal trap): nothing flushed, no atexit handlers.
ORBIS_HLE("libSceLibcInternal", abort, 0) { recomp::FatalF("the guest called abort()"); }
ORBIS_HLE("libSceLibcInternal", __cxa_pure_virtual, 0) {
  recomp::FatalF("the guest called a pure virtual function");
}

namespace {

// struct tm: nine ints (sec, min, hour, mday, mon, year since 1900, wday,
// yday, isdst).
constexpr uint64_t kTmSize = 36, k1900To1970 = 2208988800ull;

// The library's time-to-tm: secs (UTC or local) from 1900 on (earlier
// ones wrap, unchecked), isdst as given.
recomp::gaddr ToTm(recomp::gaddr tm, int64_t secs, int32_t isdst) {
  const uint64_t s = uint64_t(secs) + k1900To1970;
  uint64_t days = s / 86400;
  const uint32_t sod = uint32_t(s % 86400);
  int32_t f[9];
  f[0] = int32_t(sod % 60), f[1] = int32_t(sod / 60 % 60), f[2] = int32_t(sod / 3600);
  f[6] = int32_t((days + 1) % 7);  // 1900-01-01 was a Monday
  auto leap = [](uint64_t y) { return y % 400 == 0 || (y % 100 != 0 && y % 4 == 0); };
  uint64_t y = 1900 + days / 146097 * 400;  // whole 400-year cycles first
  days %= 146097;
  while (days >= (leap(y) ? 366u : 365u)) days -= leap(y) ? 366 : 365, y++;
  f[7] = int32_t(days);
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int m = 0;
  while (days >= uint64_t(kDays[m] + (m == 1 && leap(y)))) days -= kDays[m] + (m == 1 && leap(y)), m++;
  f[3] = int32_t(days + 1), f[4] = m, f[5] = int32_t(y - 1900), f[8] = isdst;
  for (int i = 0; i < 9; i++) recomp::Wr32(tm + 4 * i, uint32_t(f[i]));
  return tm;
}

// gmtime's and localtime's result: one per thread.
recomp::gaddr ThreadTm() {
  thread_local recomp::gaddr tm = 0;
  if (!tm) tm = Heap().Alloc(kTmSize, true);
  return tm;
}

}  // namespace

// time_t time(time_t* t): the seconds clock's (the host's).
ORBIS_HLE("libSceLibcInternal", time, 1) {
  const int64_t t = int64_t(::time(nullptr));
  if (Arg(c, 0)) recomp::Wr64(Arg(c, 0), uint64_t(t));
  return uint64_t(t);
}
ORBIS_HLE("libSceLibcInternal", gmtime, 1) {
  return ToTm(ThreadTm(), int64_t(recomp::Rd64(Arg(c, 0))), 0);
}
// struct tm* gmtime_s(const time_t* t, struct tm* result): isdst -1; a
// NULL argument goes to the constraint handler (its message on stderr).
ORBIS_HLE("libSceLibcInternal", gmtime_s, 2) {
  if (!Arg(c, 0) || !Arg(c, 1)) {
    Write(stderr, "gmtime_s: bad argument\n");
    return 0;
  }
  return ToTm(Arg(c, 1), int64_t(recomp::Rd64(Arg(c, 0))), -1);
}
// localtime: the platform's zone is UTC, without daylight saving time
// (kernel_time.cpp), so local time is gmtime's.
ORBIS_HLE("libSceLibcInternal", localtime, 1) {
  return ToTm(ThreadTm(), int64_t(recomp::Rd64(Arg(c, 0))), 0);
}
// time_t mktime(struct tm* tm): the fields normalised (any may be out of
// range), as local time in the platform's zone (UTC, no daylight saving
// time: isdst comes back 0); before 1900 -1. The tm is rewritten in full.
ORBIS_HLE("libSceLibcInternal", mktime, 1) {
  const recomp::gaddr tm = Arg(c, 0);
  int32_t f[6];
  for (int i = 0; i < 6; i++) f[i] = int32_t(recomp::Rd32(tm + 4 * i));
  int64_t year = f[5], mon = f[4];
  year += mon >= 0 ? mon / 12 : -((11 - mon) / 12);
  mon -= (mon >= 0 ? mon / 12 : -((11 - mon) / 12)) * 12;
  if (year > INT32_MAX || year < INT32_MIN) return ~uint64_t(0);
  // Days from 0001-01-01 to January 1 of year y (proleptic Gregorian,
  // floor division for years before 1).
  auto fdiv = [](int64_t a, int64_t b) { return a / b - (a % b < 0); };
  auto days_before = [&](int64_t y) {
    const int64_t p = y - 1;
    return 365 * p + fdiv(p, 4) - fdiv(p, 100) + fdiv(p, 400);
  };
  auto leap = [](int64_t y) { return y % 400 == 0 || (y % 100 != 0 && y % 4 == 0); };
  static const int kCum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  double days = double(days_before(1900 + year) - days_before(1900));
  days += kCum[mon] + (mon > 1 && leap(1900 + year));
  const double total = (days + f[3] - 1) * 86400.0 + f[2] * 3600.0 + f[1] * 60.0 + f[0];
  if (total < 0) return ~uint64_t(0);
  const int64_t t = int64_t(total - double(k1900To1970));
  ToTm(tm, t, 0);
  return uint64_t(t);
}

// ---- the C++ runtime

namespace {

// __cxa_atexit's handlers: one list for every module, newest first.
struct AtExit {
  recomp::gaddr fn, arg, dso;
};
std::mutex g_atexit_mu;
std::vector<AtExit> g_atexit;  // oldest first

// Static initialisation guards: one lock and condition for all of them.
std::mutex g_guard_mu;
std::condition_variable g_guard_cv;

}  // namespace

// int __cxa_atexit(void (*fn)(void*), void* arg, void* dso)
ORBIS_HLE("libSceLibcInternal", __cxa_atexit, 3) {
  std::lock_guard<std::mutex> l(g_atexit_mu);
  g_atexit.push_back({Arg(c, 0), Arg(c, 1), Arg(c, 2)});
  return 0;
}
// void __cxa_finalize(void* dso): runs, newest first, the handlers
// registered with dso or whose function lies in dso's module (every one
// for null), each once; the list unlocked while one runs.
ORBIS_HLE("libSceLibcInternal", __cxa_finalize, 1) {
  const recomp::gaddr dso = Arg(c, 0);
  for (;;) {
    AtExit e;
    {
      std::lock_guard<std::mutex> l(g_atexit_mu);
      auto it = std::find_if(g_atexit.rbegin(), g_atexit.rend(), [&](const AtExit& a) {
        return !dso || a.dso == dso || recomp::orbis::SameModule(a.fn, dso);
      });
      if (it == g_atexit.rend()) return 0;
      e = *it;
      g_atexit.erase(std::next(it).base());
    }
    recomp::CallGuest(c, e.fn, {e.arg});
  }
}

// int __cxa_guard_acquire(uint64_t* g): 0 if done (byte 0), else waits
// while another thread initialises (byte 1), then marks it in progress
// and returns 1. __cxa_guard_release marks it done.
ORBIS_HLE("libSceLibcInternal", __cxa_guard_acquire, 1) {
  const recomp::gaddr g = Arg(c, 0);
  std::unique_lock<std::mutex> l(g_guard_mu);
  for (;;) {
    if (recomp::Rd8(g)) return 0;
    if (!recomp::Rd8(g + 1)) {
      recomp::Wr64(g, 0x100);
      return 1;
    }
    g_guard_cv.wait(l);
  }
}
ORBIS_HLE("libSceLibcInternal", __cxa_guard_release, 1) {
  {
    std::lock_guard<std::mutex> l(g_guard_mu);
    recomp::Wr64(Arg(c, 0), 1);
  }
  g_guard_cv.notify_all();
  return 0;
}

// operator new and new[]: the library's heap, size 0 taken as 1. Its
// new_handler and bad_alloc (a C++ exception into guest code) are not
// implemented: running out of memory is fatal.
static uint64_t New(uint64_t size) {
  const recomp::gaddr p = Heap().Alloc(size ? size : 1, false);
  if (!p) recomp::FatalF("libSceLibcInternal operator new(%llu): out of memory", (unsigned long long)size);
  return p;
}
ORBIS_HLE_NAMED("libSceLibcInternal", "_Znwm", Znwm, 1) { return New(Arg(c, 0)); }
ORBIS_HLE_NAMED("libSceLibcInternal", "_Znam", Znam, 1) { return New(Arg(c, 0)); }
static uint64_t Delete(recomp::gaddr p) {
  if (p && !Heap().Free(p))
    recomp::FatalF("libSceLibcInternal operator delete(%llX): not a block of its heap",
                   (unsigned long long)p);
  return 0;
}
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZdlPv", ZdlPv, 1) { return Delete(Arg(c, 0)); }
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZdaPv", ZdaPv, 1) { return Delete(Arg(c, 0)); }

// The standard library's throws: C++ exceptions into guest code are not
// implemented.
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZSt11_Xbad_allocv", Xbad_alloc, 0) {
  recomp::FatalF("the guest threw std::bad_alloc (C++ exceptions not implemented)");
}
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZSt14_Xlength_errorPKc", Xlength_error, 1) {
  recomp::FatalF("the guest threw std::length_error(\"%s\") (C++ exceptions not implemented)",
                 recomp::GStr(Arg(c, 0)).c_str());
}
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZSt14_Xout_of_rangePKc", Xout_of_range, 1) {
  recomp::FatalF("the guest threw std::out_of_range(\"%s\") (C++ exceptions not implemented)",
                 recomp::GStr(Arg(c, 0)).c_str());
}
// const char* std::_Syserror_map(int): the library's own texts for errno
// values, which are not copied.
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZSt13_Syserror_mapi", Syserror_map, 1) {
  recomp::FatalF("std::_Syserror_map(%d) not implemented", int(Arg(c, 0)));
}

// std::ios_base::Init and std::_Winit: the first constructs the standard
// streams (narrow or wide), the last destructor flushes them. The streams
// are the library's data, which the HLE does not provide (importing them
// is fatal at load), so these only count.
namespace {
std::mutex g_init_mu;
int g_ios_init = -1, g_winit = -1;
uint64_t Count(int& n, int by) {
  std::lock_guard<std::mutex> l(g_init_mu);
  n = by > 0 && n < 0 ? 1 : n + by;
  return 0;
}
}  // namespace
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZNSt8ios_base4InitC1Ev", ios_init_ctor, 1) {
  return Count(g_ios_init, 1);
}
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZNSt8ios_base4InitD1Ev", ios_init_dtor, 1) {
  return Count(g_ios_init, -1);
}
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZNSt6_WinitC1Ev", winit_ctor, 1) { return Count(g_winit, 1); }
ORBIS_HLE_NAMED("libSceLibcInternal", "_ZNSt6_WinitD1Ev", winit_dtor, 1) { return Count(g_winit, -1); }

// ---- the math library's internals (Dinkumware's)

namespace {

// Their classification: 0 zero, -1 finite, -2 subnormal, 1 infinite, 2 NaN.
int16_t Classify(double v) {
  switch (std::fpclassify(v)) {
    case FP_ZERO: return 0;
    case FP_SUBNORMAL: return -2;
    case FP_INFINITE: return 1;
    case FP_NAN: return 2;
    default: return -1;
  }
}
uint64_t Short(int16_t v) { return uint64_t(int64_t(v)); }

}  // namespace

// short _Dtest(double* x), _FDtest(float* x)
ORBIS_HLE("libSceLibcInternal", _Dtest, 1) {
  const uint64_t b = recomp::Rd64(Arg(c, 0));
  double d;
  std::memcpy(&d, &b, 8);
  return Short(Classify(d));
}
ORBIS_HLE("libSceLibcInternal", _FDtest, 1) {
  const uint32_t b = recomp::Rd32(Arg(c, 0));
  float f;
  std::memcpy(&f, &b, 4);
  return Short(Classify(f));
}
// short _FExp(float* x, float y, long eoff): *x = y * e^*x * 2^eoff and
// its classification, with the library's special cases (an infinite or
// zero y, x beyond +-229, e itself). The floating-point exception flags
// it raises are not modelled.
ORBIS_HLE("libSceLibcInternal", _FExp, 2) {
  const recomp::gaddr px = Arg(c, 0);
  const int64_t eoff = int64_t(Arg(c, 1));
  const float y = recomp::hle::ArgF(c, 0);
  float x;
  const uint32_t xb = recomp::Rd32(px);
  std::memcpy(&x, &xb, 4);
  auto store = [&](float v, int16_t code) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    recomp::Wr32(px, b);
    return Short(code);
  };
  if (std::isnan(x)) return Short(2);
  if (std::isnan(y)) return store(y, 2);
  if (y == 0) return x == INFINITY ? store(NAN, 2) : store(y, 0);
  if (std::isinf(y)) return x == -INFINITY ? store(NAN, 2) : store(y, 1);
  if (x == INFINITY || x > 229) return store(INFINITY * y, 1);
  if (x == -INFINITY || x < -229) return store(0 * y, 0);
  if (x == 1 && y == 1 && eoff == 0) {
    float e;
    const uint32_t eb = 0x402DF854;  // e, rounded to float
    std::memcpy(&e, &eb, 4);
    return store(e, -1);
  }
  const float r = float(std::ldexp(double(y) * std::exp(double(x)), int(std::clamp<int64_t>(eoff, -100000, 100000))));
  return store(r, Classify(r));
}
// float _FLog(float x, int base) and double _Log(double, int): base 0 the
// natural logarithm, above 0 base 10, below 0 base 2.
ORBIS_HLE("libSceLibcInternal", _FLog, 1) {
  const float x = recomp::hle::ArgF(c, 0);
  const int b = int(Arg(c, 0));
  return recomp::hle::RetF(c, b == 0 ? logf(x) : b > 0 ? log10f(x) : log2f(x));
}
ORBIS_HLE("libSceLibcInternal", _Log, 1) {
  const double x = recomp::hle::ArgD(c, 0);
  const int b = int(Arg(c, 0));
  return recomp::hle::RetD(c, b == 0 ? ::log(x) : b > 0 ? ::log10(x) : ::log2(x));
}
// float _FSin(float x, unsigned q) and double _Sin(double, unsigned): the
// sine for q 0, the cosine for any other q (not a quadrant offset here).
ORBIS_HLE("libSceLibcInternal", _FSin, 1) {
  const float x = recomp::hle::ArgF(c, 0);
  return recomp::hle::RetF(c, uint32_t(Arg(c, 0)) ? cosf(x) : sinf(x));
}
ORBIS_HLE("libSceLibcInternal", _Sin, 1) {
  const double x = recomp::hle::ArgD(c, 0);
  return recomp::hle::RetD(c, uint32_t(Arg(c, 0)) ? ::cos(x) : ::sin(x));
}
