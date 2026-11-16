// The Universal CRT (ucrtbase.dll, imported through its api-ms-win-crt-*
// API sets) for x64 guests: the parts the statically linked startup code
// and the game call. Its process-wide data (argv, the environment, the
// onexit tables' arrays, errno) lives in guest memory, where the guest
// reads it through the pointers these functions return.

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/ucrt.h"
#include "win32/win32.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

// The program name: up to the closing quote or the first blank, no escapes.
const char* SkipProgramName(const char* p) {
  if (*p == '"') {
    p++;
    while (*p && *p != '"') p++;
    if (*p) p++;
  } else {
    while (*p && *p != ' ' && *p != '\t') p++;
  }
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

// Guest copies of a list of strings: a null-terminated pointer array.
gaddr GuestStrings(const std::vector<std::string>& v) {
  gaddr arr = HeapAllocProcess(8 * (v.size() + 1), true);
  for (size_t i = 0; i < v.size(); i++) {
    gaddr s = HeapAllocProcess(v[i].size() + 1, false);
    PutStr(s, v[i]);
    Wr64(arr + 8 * i, s);
  }
  return arr;
}

// The CRT's process-wide variables, in guest memory (their addresses are
// what __p___argc and friends return).
struct Globals {
  gaddr argc, argv, environ, fmode, commode, app_type;
};
Globals& G() {
  static Globals g = [] {
    Globals r;
    gaddr b = HeapAllocProcess(0x40, true);
    r.argc = b;            // int
    r.argv = b + 0x08;     // char**
    r.environ = b + 0x10;  // char**
    r.fmode = b + 0x18;    // int
    r.commode = b + 0x1C;  // int
    r.app_type = b + 0x20;  // int
    return r;
  }();
  return g;
}

// _onexit_table_t: {first, last, end} function pointer array, which the
// guest's own startup code may also walk.
std::mutex g_onexit_mu;

void RegisterOnExit(gaddr table, gaddr fn) {
  std::lock_guard<std::mutex> l(g_onexit_mu);
  gaddr first = Rd64(table), last = Rd64(table + 8), end = Rd64(table + 16);
  if (last == end) {
    uint64_t n = (end - first) / 8, cap = n ? 2 * n : 32;
    gaddr grown = HeapAllocProcess(8 * cap, true);
    if (n) std::memcpy(GPtr(grown), GPtr(first), 8 * n);
    if (first) HeapFreeProcess(first);
    first = grown;
    last = grown + 8 * n;
    end = grown + 8 * cap;
  }
  Wr64(last, fn);
  Wr64(table, first);
  Wr64(table + 8, last + 8);
  Wr64(table + 16, end);
}

// Runs the table's functions last to first and empties it. A function may
// register more while this runs; those run too.
void ExecuteOnExit(Ctx& c, gaddr table) {
  for (;;) {
    gaddr fn;
    {
      std::lock_guard<std::mutex> l(g_onexit_mu);
      gaddr first = Rd64(table), last = Rd64(table + 8);
      if (!first || last == first) break;
      last -= 8;
      fn = Rd64(last);
      Wr64(table + 8, last);
    }
    if (fn) CallGuest(c, fn, {});
  }
  std::lock_guard<std::mutex> l(g_onexit_mu);
  if (gaddr first = Rd64(table)) HeapFreeProcess(first);
  for (int i = 0; i < 3; i++) Wr64(table + 8 * i, 0);
}

// The CRT's own table (_crt_atexit, atexit): run by exit and _cexit.
gaddr AtExitTable() {
  static gaddr t = HeapAllocProcess(24, true);
  return t;
}

[[noreturn]] void ExitNow(uint32_t code) {
  std::fflush(stdout);
  std::fprintf(stderr, "exit(%d)\n", int32_t(code));
  std::fflush(stderr);
  std::_Exit(int(code));
}

// The calling thread's errno, in guest memory.
gaddr ErrnoSlot() {
  static std::mutex mu;
  static std::unordered_map<uint32_t, gaddr> slots;
  std::lock_guard<std::mutex> l(mu);
  gaddr& s = slots[CurrentThread()->tid];
  if (!s) s = HeapAllocProcess(8, true);
  return s;
}

void SetErrno(uint32_t e) { Wr32(ErrnoSlot(), e); }

}  // namespace

gaddr GuestErrno() { return ErrnoSlot(); }

// ---- startup and termination ---------------------------------------------

// _initterm(first, last): every non-null function pointer in [first, last).
HLE64("ucrtbase.dll", _initterm, kCdecl) {
  for (gaddr p = Arg(c, 0), end = Arg(c, 1); p < end; p += 8)
    if (gaddr fn = Rd64(p)) CallGuest(c, fn, {});
  return 0;
}

// _initterm_e: the same with int-returning functions; the first nonzero
// result stops it and is returned.
HLE64("ucrtbase.dll", _initterm_e, kCdecl) {
  for (gaddr p = Arg(c, 0), end = Arg(c, 1); p < end; p += 8)
    if (gaddr fn = Rd64(p))
      if (int32_t r = int32_t(CallGuest(c, fn, {}))) return uint32_t(r);
  return 0;
}

HLE64("ucrtbase.dll", _set_app_type, kCdecl) {
  Wr32(G().app_type, uint32_t(Arg(c, 0)));
  return 0;
}

HLE64("ucrtbase.dll", _configure_narrow_argv, kCdecl) {
  std::vector<std::string> args = win32::SplitCommandLine(GStr(g_win32->cmdline_a));
  Wr32(G().argc, uint32_t(args.size()));
  Wr64(G().argv, GuestStrings(args));
  return 0;
}

HLE64("ucrtbase.dll", __p___argc, kCdecl) { return G().argc; }
HLE64("ucrtbase.dll", __p___argv, kCdecl) { return G().argv; }

HLE64("ucrtbase.dll", _initialize_narrow_environment, kCdecl) {
  Wr64(G().environ, GuestStrings(EnvironmentVars()));
  return 0;
}
HLE64("ucrtbase.dll", _get_initial_narrow_environment, kCdecl) {
  return Rd64(G().environ);
}

// The command line after the program name (WinMain's lpCmdLine).
HLE64("ucrtbase.dll", _get_narrow_winmain_command_line, kCdecl) {
  const char* base = g_proc->mem.ptr<const char>(g_win32->cmdline_a);
  return g_win32->cmdline_a + gaddr(SkipProgramName(base) - base);
}

HLE64("ucrtbase.dll", _set_fmode, kCdecl) {
  Wr32(G().fmode, uint32_t(Arg(c, 0)));
  return 0;
}
HLE64("ucrtbase.dll", __p__commode, kCdecl) { return G().commode; }

// The guest's floating-point environment needs no CRT setup.
HLE64("ucrtbase.dll", __setusermatherr, kCdecl) { return 0; }

HLE64("ucrtbase.dll", _initialize_onexit_table, kCdecl) {
  for (int i = 0; i < 3; i++) Wr64(Arg(c, 0) + 8 * i, 0);
  return 0;
}
HLE64("ucrtbase.dll", _register_onexit_function, kCdecl) {
  RegisterOnExit(Arg(c, 0), Arg(c, 1));
  return 0;
}
HLE64("ucrtbase.dll", _execute_onexit_table, kCdecl) {
  ExecuteOnExit(c, Arg(c, 0));
  return 0;
}
HLE64("ucrtbase.dll", _crt_atexit, kCdecl) {
  RegisterOnExit(AtExitTable(), Arg(c, 0));
  return 0;
}

// The thread_local destructor callback of the EXE: kept, never called
// (guest threads do not run TLS destructors here).
HLE64("ucrtbase.dll", _register_thread_local_exe_atexit_callback, kCdecl) { return 0; }

HLE64("ucrtbase.dll", exit, kCdecl) {
  uint32_t code = uint32_t(Arg(c, 0));
  ExecuteOnExit(c, AtExitTable());
  ExitNow(code);
}
HLE64("ucrtbase.dll", _exit, kCdecl) { ExitNow(uint32_t(Arg(c, 0))); }
HLE64("ucrtbase.dll", _cexit, kCdecl) {
  ExecuteOnExit(c, AtExitTable());
  return 0;
}
HLE64("ucrtbase.dll", _c_exit, kCdecl) { return 0; }

HLE64("ucrtbase.dll", terminate, kCdecl) { FatalF("guest called terminate()"); }
HLE64("ucrtbase.dll", _invalid_parameter_noinfo_noreturn, kCdecl) {
  FatalF("guest CRT: invalid parameter (_invalid_parameter_noinfo_noreturn)");
}

// The startup code's __except filters: no exception is handled there.
HLE64("ucrtbase.dll", _seh_filter_exe, kCdecl) { return 0; }  // CONTINUE_SEARCH
HLE64("ucrtbase.dll", _seh_filter_dll, kCdecl) { return 0; }

// errno: an int per guest thread.
HLE64("ucrtbase.dll", _errno, kCdecl) { return ErrnoSlot(); }

// ---- threads ----------------------------------------------------------------

// _beginthreadex(security, stack, start, arg, flags, &tid): a thread whose
// exit code is start's result; the handle is the caller's to close.
// Flags: CREATE_SUSPENDED, STACK_SIZE_PARAM_IS_A_RESERVATION.
HLE64("ucrtbase.dll", _beginthreadex, kCdecl) {
  uint32_t flags = uint32_t(Arg(c, 4));
  if (flags & ~0x10004u) FatalF("_beginthreadex: flags %X not supported", flags);
  uint32_t tid;
  uint32_t h = NewThread(uint32_t(Arg(c, 1)), Arg(c, 2), Arg(c, 3), flags & 4, &tid);
  if (Arg(c, 5)) Wr32(Arg(c, 5), tid);
  return HandleRet(h);
}

// ---- heap ----------------------------------------------------------------
// The UCRT allocates from the process heap since Visual Studio 2015.

namespace {
gaddr Malloc(uint64_t n) { return HeapAllocProcess(n ? n : 1, false); }
}  // namespace

HLE64("ucrtbase.dll", malloc, kCdecl) { return Malloc(Arg(c, 0)); }
HLE64("ucrtbase.dll", free, kCdecl) {
  if (gaddr p = Arg(c, 0)) HeapFreeProcess(p);
  return 0;
}
HLE64("ucrtbase.dll", calloc, kCdecl) {
  uint64_t n = Arg(c, 0), size = Arg(c, 1);
  if (size && n > UINT64_MAX / size) return 0;
  return HeapAllocProcess(n * size ? n * size : 1, true);
}
HLE64("ucrtbase.dll", realloc, kCdecl) {
  gaddr p = Arg(c, 0);
  uint64_t n = Arg(c, 1);
  if (!p) return Malloc(n);
  if (!n) {
    HeapFreeProcess(p);
    return 0;
  }
  return HeapReAllocProcess(p, n);
}
// _aligned_malloc: the block's own address is kept in the 8 bytes below the
// aligned pointer, as the UCRT does.
HLE64("ucrtbase.dll", _aligned_malloc, kCdecl) {
  uint64_t n = Arg(c, 0), align = std::max<uint64_t>(Arg(c, 1), 8);
  if (align & (align - 1)) return 0;
  gaddr block = Malloc(n + align + 8);
  if (!block) return 0;
  gaddr p = (block + 8 + align - 1) & ~(align - 1);
  Wr64(p - 8, block);
  return p;
}
HLE64("ucrtbase.dll", _aligned_free, kCdecl) {
  if (gaddr p = Arg(c, 0)) HeapFreeProcess(Rd64(p - 8));
  return 0;
}
// No new handler is ever installed (_set_new_handler is not provided).
HLE64("ucrtbase.dll", _callnewh, kCdecl) { return 0; }
HLE64("ucrtbase.dll", _set_new_mode, kCdecl) {
  static int mode = 0;
  int old = mode;
  mode = int(Arg(c, 0));
  return uint32_t(old);
}

// ---- locale (only the "C" locale) ----------------------------------------

HLE64("ucrtbase.dll", _configthreadlocale, kCdecl) {
  // _ENABLE_PER_THREAD_LOCALE 1, _DISABLE_PER_THREAD_LOCALE 2; 0 queries,
  // -1 resets. Nothing differs between them with one locale.
  static std::mutex mu;
  static int mode = 2;
  std::lock_guard<std::mutex> l(mu);
  int old = mode, t = int(Arg(c, 0));
  if (t == 1 || t == 2) mode = t;
  if (t == -1) mode = 2;
  return uint32_t(old);
}

// localeconv: the "C" locale's struct lconv (x64 layout: ten char*, eight
// chars, six wchar_t*).
HLE64("ucrtbase.dll", localeconv, kCdecl) {
  static gaddr lc = [] {
    gaddr p = HeapAllocProcess(136, true);
    gaddr dot = HeapAllocProcess(4, true), empty = dot + 2;
    gaddr wdot = HeapAllocProcess(8, true), wempty = wdot + 4;
    PutStr(dot, ".");
    Wr16(wdot, '.');
    for (int i = 0; i < 10; i++) Wr64(p + 8 * i, i == 0 ? dot : empty);
    for (int i = 0; i < 8; i++) Wr8(p + 80 + i, CHAR_MAX);
    for (int i = 0; i < 6; i++) Wr64(p + 88 + 8 * i, i == 0 ? wdot : wempty);
    return p;
  }();
  return lc;
}

// ---- character classes and strings ("C" locale) --------------------------

namespace {
int CArg(const Ctx& c, int i) { return int32_t(Arg(c, i)); }
bool InC(int ch) { return ch >= 0 && ch < 128; }  // the classes are ASCII's
}  // namespace

HLE64("ucrtbase.dll", iscntrl, kCdecl) { return InC(CArg(c, 0)) && std::iscntrl(CArg(c, 0)); }
HLE64("ucrtbase.dll", isdigit, kCdecl) { return InC(CArg(c, 0)) && std::isdigit(CArg(c, 0)); }
HLE64("ucrtbase.dll", isspace, kCdecl) { return InC(CArg(c, 0)) && std::isspace(CArg(c, 0)); }
HLE64("ucrtbase.dll", isupper, kCdecl) { return InC(CArg(c, 0)) && std::isupper(CArg(c, 0)); }
HLE64("ucrtbase.dll", isxdigit, kCdecl) { return InC(CArg(c, 0)) && std::isxdigit(CArg(c, 0)); }
HLE64("ucrtbase.dll", tolower, kCdecl) {
  int ch = CArg(c, 0);
  return uint32_t(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
}
HLE64("ucrtbase.dll", toupper, kCdecl) {
  int ch = CArg(c, 0);
  return uint32_t(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
}

namespace {
const char* S(gaddr p) { return g_proc->mem.ptr<const char>(p); }
int Sign(int v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }
int Ci(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }  // _stricmp folds to lower
int StrICmp(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int x = Ci(uint8_t(a[i])), y = Ci(uint8_t(b[i]));
    if (x != y) return x - y;
    if (!x) break;
  }
  return 0;
}
}  // namespace

HLE64("ucrtbase.dll", strcmp, kCdecl) {
  return uint32_t(Sign(std::strcmp(S(Arg(c, 0)), S(Arg(c, 1)))));
}
HLE64("ucrtbase.dll", strncmp, kCdecl) {
  return uint32_t(Sign(std::strncmp(S(Arg(c, 0)), S(Arg(c, 1)), Arg(c, 2))));
}
HLE64("ucrtbase.dll", _stricmp, kCdecl) {
  return uint32_t(Sign(StrICmp(S(Arg(c, 0)), S(Arg(c, 1)), SIZE_MAX)));
}
HLE64("ucrtbase.dll", _strnicmp, kCdecl) {
  return uint32_t(Sign(StrICmp(S(Arg(c, 0)), S(Arg(c, 1)), Arg(c, 2))));
}
HLE64("ucrtbase.dll", strcspn, kCdecl) { return std::strcspn(S(Arg(c, 0)), S(Arg(c, 1))); }
HLE64("ucrtbase.dll", strncpy, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 1);
  uint64_t n = Arg(c, 2), i = 0;
  for (; i < n && S(src)[i]; i++) Wr8(d + i, uint8_t(S(src)[i]));
  for (; i < n; i++) Wr8(d + i, 0);
  return d;
}
// strtok: the position is per thread, as in the UCRT.
HLE64("ucrtbase.dll", strtok, kCdecl) {
  static thread_local gaddr next = 0;
  gaddr s = Arg(c, 0) ? Arg(c, 0) : next;
  const char* delim = S(Arg(c, 1));
  if (!s) return 0;
  s += std::strspn(S(s), delim);
  if (!*S(s)) {
    next = 0;
    return 0;
  }
  gaddr e = s + std::strcspn(S(s), delim);
  if (*S(e)) {
    Wr8(e, 0);
    next = e + 1;
  } else {
    next = 0;
  }
  return s;
}
// wcscat_s: 0, or EINVAL / ERANGE (and an empty destination) as the UCRT.
HLE64("ucrtbase.dll", wcscat_s, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 2);
  uint64_t size = Arg(c, 1);
  if (!d || !size) return EINVAL;
  if (!src) {
    Wr16(d, 0);
    return EINVAL;
  }
  uint64_t n = 0;
  while (n < size && Rd16(d + 2 * n)) n++;
  if (n == size) {
    Wr16(d, 0);
    return EINVAL;
  }
  for (uint64_t i = 0;; i++) {
    if (n + i >= size) {
      Wr16(d, 0);
      return ERANGE;
    }
    uint16_t ch = Rd16(src + 2 * i);
    Wr16(d + 2 * (n + i), ch);
    if (!ch) return 0;
  }
}

// ---- numeric conversion ("C" locale) -------------------------------------

namespace {

// strto*: the host's (the same grammar since Visual Studio 2015, hex floats,
// inf and nan included) on the guest string; *end is a guest pointer.
template <typename T, typename F>
T StrTo(gaddr s, gaddr end, F f) {
  const char* p = S(s);
  char* e = nullptr;
  errno = 0;
  T v = f(p, &e);
  if (errno == ERANGE) SetErrno(ERANGE);
  if (end) Wr64(end, s + gaddr(e - p));
  return v;
}
}  // namespace

HLE64("ucrtbase.dll", strtod, kCdecl) {
  return RetD(c, StrTo<double>(Arg(c, 0), Arg(c, 1),
                               [](const char* p, char** e) { return std::strtod(p, e); }));
}
HLE64("ucrtbase.dll", strtol, kCdecl) {
  int base = int(Arg(c, 2));
  return uint32_t(int32_t(StrTo<long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    long long v = std::strtoll(p, e, base);
    if (v > INT32_MAX || v < INT32_MIN) {  // long is 32-bit in Win64
      errno = ERANGE;
      return long(v > 0 ? INT32_MAX : INT32_MIN);
    }
    return long(v);
  })));
}
HLE64("ucrtbase.dll", strtoul, kCdecl) {
  int base = int(Arg(c, 2));
  return uint32_t(StrTo<unsigned long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    const char* q = p;
    while (std::isspace(uint8_t(*q))) q++;
    bool neg = *q == '-';
    unsigned long long v = std::strtoull(p, e, base);
    // 32-bit unsigned long: out of range unless the magnitude fits.
    unsigned long long mag = neg ? 0 - v : v;
    if (errno == ERANGE || mag > UINT32_MAX) {
      errno = ERANGE;
      return (unsigned long)UINT32_MAX;
    }
    return (unsigned long)(uint32_t(v));
  }));
}
HLE64("ucrtbase.dll", strtoull, kCdecl) {
  int base = int(Arg(c, 2));
  return StrTo<unsigned long long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    return std::strtoull(p, e, base);
  });
}
HLE64("ucrtbase.dll", atof, kCdecl) { return RetD(c, std::strtod(S(Arg(c, 0)), nullptr)); }
HLE64("ucrtbase.dll", atoi, kCdecl) {
  long long v = std::strtoll(S(Arg(c, 0)), nullptr, 10);
  return uint32_t(int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v));
}
HLE64("ucrtbase.dll", atol, kCdecl) {
  long long v = std::strtoll(S(Arg(c, 0)), nullptr, 10);
  return uint32_t(int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v));
}
HLE64("ucrtbase.dll", atoll, kCdecl) { return uint64_t(std::strtoll(S(Arg(c, 0)), nullptr, 10)); }

// mbstowcs in the "C" locale: each byte is the wchar_t of the same value.
HLE64("ucrtbase.dll", mbstowcs, kCdecl) {
  gaddr d = Arg(c, 0);
  const char* src = S(Arg(c, 1));
  uint64_t n = Arg(c, 2), len = std::strlen(src);
  if (!d) return len;
  uint64_t i = 0;
  for (; i < n && i < len; i++) Wr16(d + 2 * i, uint8_t(src[i]));
  if (i < n) Wr16(d + 2 * i, 0);
  return i;
}

// ---- utility ---------------------------------------------------------------

// rand: the UCRT's per-thread linear congruential generator (seed 1;
// srand sets it).
uint32_t& RandSeed() {
  static thread_local uint32_t seed = 1;
  return seed;
}
HLE64("ucrtbase.dll", rand, kCdecl) {
  uint32_t& seed = RandSeed();
  seed = seed * 214013u + 2531011u;
  return (seed >> 16) & 0x7FFF;
}

// qsort(base, n, width, compare): the MSVC CRT's algorithm, so elements
// that compare equal end up in the order they do on Windows: quicksort on
// the median of the first, middle and last elements, an explicit stack of
// the larger partitions, and a selection sort (largest to the end) for
// partitions of up to 8 elements.
HLE64("ucrtbase.dll", qsort, kCdecl) {
  gaddr base = Arg(c, 0), cmp = Arg(c, 3);
  uint64_t n = Arg(c, 1), w = Arg(c, 2);
  if (n < 2 || !w) return 0;
  auto compare = [&](gaddr a, gaddr b) { return int32_t(CallGuest(c, cmp, {a, b})); };
  auto swap = [&](gaddr a, gaddr b) {
    if (a == b) return;
    uint8_t* pa = GPtr(a);
    uint8_t* pb = GPtr(b);
    for (uint64_t i = 0; i < w; i++) std::swap(pa[i], pb[i]);
  };
  auto shortsort = [&](gaddr lo, gaddr hi) {
    for (; hi > lo; hi -= w) {
      gaddr max = lo;
      for (gaddr p = lo + w; p <= hi; p += w)
        if (compare(p, max) > 0) max = p;
      swap(max, hi);
    }
  };
  constexpr uint64_t kCutoff = 8;
  std::vector<std::pair<gaddr, gaddr>> stack;
  gaddr lo = base, hi = base + w * (n - 1);
  for (;;) {
    uint64_t size = (hi - lo) / w + 1;
    if (size <= kCutoff) {
      shortsort(lo, hi);
    } else {
      gaddr mid = lo + (size / 2) * w;
      if (compare(lo, mid) > 0) swap(lo, mid);
      if (compare(lo, hi) > 0) swap(lo, hi);
      if (compare(mid, hi) > 0) swap(mid, hi);
      gaddr loguy = lo, higuy = hi;
      for (;;) {
        if (mid > loguy) {
          do loguy += w;
          while (loguy < mid && compare(loguy, mid) <= 0);
        }
        if (mid <= loguy) {
          do loguy += w;
          while (loguy <= hi && compare(loguy, mid) <= 0);
        }
        do higuy -= w;
        while (higuy > mid && compare(higuy, mid) > 0);
        if (higuy < loguy) break;
        swap(loguy, higuy);
        if (mid == higuy) mid = loguy;  // the pivot moved
      }
      higuy += w;
      if (mid < higuy) {
        do higuy -= w;
        while (higuy > mid && compare(higuy, mid) == 0);
      }
      if (mid >= higuy) {
        do higuy -= w;
        while (higuy > lo && compare(higuy, mid) == 0);
      }
      // [lo, higuy] and [loguy, hi] remain: the smaller one next, the
      // larger one stacked.
      if (higuy - lo >= hi - loguy) {
        if (lo < higuy) stack.push_back({lo, higuy});
        if (loguy < hi) {
          lo = loguy;
          continue;
        }
      } else {
        if (loguy < hi) stack.push_back({loguy, hi});
        if (lo < higuy) {
          hi = higuy;
          continue;
        }
      }
    }
    if (stack.empty()) return 0;
    std::tie(lo, hi) = stack.back();
    stack.pop_back();
  }
}

// ---- environment -----------------------------------------------------------

HLE64("ucrtbase.dll", getenv, kCdecl) {
  std::string name = S(Arg(c, 0));
  gaddr env = Rd64(G().environ);
  if (!env) {
    Wr64(G().environ, GuestStrings(EnvironmentVars()));
    env = Rd64(G().environ);
  }
  for (gaddr e; (e = Rd64(env)); env += 8) {
    const char* v = S(e);
    if (StrICmp(v, name.c_str(), name.size()) == 0 && v[name.size()] == '=')
      return e + name.size() + 1;
  }
  return 0;
}

// ---- elementary functions ------------------------------------------------
// Correctly rounded (CORE-MATH, cmake/coremath.cmake), so the same on every
// host; the UCRT's own are not correctly rounded and may differ from these
// in the last bit. Domain and range errors set the guest's errno, as the
// UCRT does. Computed in the host's round-to-nearest mode.

extern "C" {
float cr_acosf(float), cr_asinf(float), cr_atanf(float), cr_atan2f(float, float),
    cr_cosf(float), cr_logf(float), cr_log2f(float), cr_log10f(float),
    cr_powf(float, float), cr_sinf(float), cr_tanf(float), cr_expf(float), cr_coshf(float),
    cr_sinhf(float), cr_tanhf(float);
double cr_acos(double), cr_asin(double), cr_atan(double), cr_atan2(double, double),
    cr_cos(double), cr_exp(double), cr_log(double), cr_log10(double), cr_pow(double, double),
    cr_sin(double), cr_tan(double);
}

namespace {
template <typename T, typename F>
T WithErrno(F f) {
  errno = 0;
  T v = f();
  if (errno == EDOM || errno == ERANGE) SetErrno(uint32_t(errno));
  return v;
}
}  // namespace

#define RECOMP_CR_F1(name)                                                  \
  HLE64("ucrtbase.dll", name, kCdecl) {                                     \
    return RetF(c, WithErrno<float>([&] { return cr_##name(ArgF(c, 0)); })); \
  }
#define RECOMP_CR_D1(name)                                                   \
  HLE64("ucrtbase.dll", name, kCdecl) {                                      \
    return RetD(c, WithErrno<double>([&] { return cr_##name(ArgD(c, 0)); })); \
  }
RECOMP_CR_F1(acosf)
RECOMP_CR_F1(asinf)
RECOMP_CR_F1(atanf)
RECOMP_CR_F1(cosf)
RECOMP_CR_F1(logf)
RECOMP_CR_F1(log2f)
RECOMP_CR_F1(log10f)
RECOMP_CR_F1(sinf)
RECOMP_CR_F1(tanf)
RECOMP_CR_D1(atan)
RECOMP_CR_D1(cos)
RECOMP_CR_D1(exp)
RECOMP_CR_D1(log)
RECOMP_CR_D1(sin)
RECOMP_CR_F1(expf)
RECOMP_CR_F1(coshf)
RECOMP_CR_F1(sinhf)
RECOMP_CR_F1(tanhf)
RECOMP_CR_D1(acos)
RECOMP_CR_D1(asin)
RECOMP_CR_D1(log10)
RECOMP_CR_D1(tan)
#undef RECOMP_CR_F1
#undef RECOMP_CR_D1
HLE64("ucrtbase.dll", atan2f, kCdecl) {
  return RetF(c, WithErrno<float>([&] { return cr_atan2f(ArgF(c, 0), ArgF(c, 1)); }));
}
HLE64("ucrtbase.dll", atan2, kCdecl) {
  return RetD(c, WithErrno<double>([&] { return cr_atan2(ArgD(c, 0), ArgD(c, 1)); }));
}
HLE64("ucrtbase.dll", powf, kCdecl) {
  return RetF(c, WithErrno<float>([&] { return cr_powf(ArgF(c, 0), ArgF(c, 1)); }));
}
HLE64("ucrtbase.dll", pow, kCdecl) {
  return RetD(c, WithErrno<double>([&] { return cr_pow(ArgD(c, 0), ArgD(c, 1)); }));
}

// ---- math with exact results (the same on every IEEE host) ---------------

HLE64("ucrtbase.dll", sqrt, kCdecl) { return RetD(c, std::sqrt(ArgD(c, 0))); }
HLE64("ucrtbase.dll", sqrtf, kCdecl) { return RetF(c, std::sqrt(ArgF(c, 0))); }
HLE64("ucrtbase.dll", ceil, kCdecl) { return RetD(c, std::ceil(ArgD(c, 0))); }
HLE64("ucrtbase.dll", ceilf, kCdecl) { return RetF(c, std::ceil(ArgF(c, 0))); }
HLE64("ucrtbase.dll", floor, kCdecl) { return RetD(c, std::floor(ArgD(c, 0))); }
HLE64("ucrtbase.dll", floorf, kCdecl) { return RetF(c, std::floor(ArgF(c, 0))); }
HLE64("ucrtbase.dll", truncf, kCdecl) { return RetF(c, std::trunc(ArgF(c, 0))); }
HLE64("ucrtbase.dll", roundf, kCdecl) { return RetF(c, std::round(ArgF(c, 0))); }
HLE64("ucrtbase.dll", fmod, kCdecl) { return RetD(c, std::fmod(ArgD(c, 0), ArgD(c, 1))); }
HLE64("ucrtbase.dll", fmodf, kCdecl) { return RetF(c, std::fmod(ArgF(c, 0), ArgF(c, 1))); }
HLE64("ucrtbase.dll", ldexp, kCdecl) {
  return RetD(c, std::ldexp(ArgD(c, 0), int32_t(Arg(c, 1))));
}
HLE64("ucrtbase.dll", modf, kCdecl) {
  double ip;
  double f = std::modf(ArgD(c, 0), &ip);
  uint64_t b;
  std::memcpy(&b, &ip, 8);
  Wr64(Arg(c, 1), b);
  return RetD(c, f);
}
HLE64("ucrtbase.dll", nextafterf, kCdecl) {
  return RetF(c, std::nextafter(ArgF(c, 0), ArgF(c, 1)));
}
// _dclass/_fdclass/_ldclass: FP_INFINITE 1, FP_NAN 2, FP_NORMAL -1,
// FP_SUBNORMAL -2, FP_ZERO 0 (MSVC's values). long double is double.
namespace {
uint64_t Class(double v) {
  switch (std::fpclassify(v)) {
    case FP_INFINITE: return 1;
    case FP_NAN: return 2;
    case FP_NORMAL: return uint32_t(-1);
    case FP_SUBNORMAL: return uint32_t(-2);
    default: return 0;
  }
}
}  // namespace
HLE64("ucrtbase.dll", _dclass, kCdecl) { return Class(ArgD(c, 0)); }
HLE64("ucrtbase.dll", _ldclass, kCdecl) { return Class(ArgD(c, 0)); }
HLE64("ucrtbase.dll", _fdclass, kCdecl) { return Class(ArgF(c, 0)); }
HLE64("ucrtbase.dll", _dsign, kCdecl) { return std::signbit(ArgD(c, 0)) ? 0x8000 : 0; }


// ---- more of the CRT, "C" locale ----------------------------------------------

namespace {
const uint16_t* W16(gaddr p) { return g_proc->mem.ptr<const uint16_t>(p); }
std::u16string WStr(gaddr p) { return std::u16string(reinterpret_cast<const char16_t*>(W16(p))); }
uint16_t WLower(uint16_t ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
int WCmp(gaddr a, gaddr b, size_t n, bool fold) {
  for (size_t i = 0; i < n; i++) {
    uint16_t x = Rd16(a + 2 * i), y = Rd16(b + 2 * i);
    if (fold) x = WLower(x), y = WLower(y);
    if (x != y) return x < y ? -1 : 1;
    if (!x) break;
  }
  return 0;
}
}  // namespace

HLE64("ucrtbase.dll", isalnum, kCdecl) { return InC(CArg(c, 0)) && std::isalnum(CArg(c, 0)); }
HLE64("ucrtbase.dll", isalpha, kCdecl) { return InC(CArg(c, 0)) && std::isalpha(CArg(c, 0)); }
HLE64("ucrtbase.dll", islower, kCdecl) { return InC(CArg(c, 0)) && std::islower(CArg(c, 0)); }
HLE64("ucrtbase.dll", ispunct, kCdecl) { return InC(CArg(c, 0)) && std::ispunct(CArg(c, 0)); }
HLE64("ucrtbase.dll", towlower, kCdecl) { return WLower(uint16_t(Arg(c, 0))); }
// No multibyte code page: no byte leads a two-byte character.
HLE64("ucrtbase.dll", _ismbblead, kCdecl) { return 0; }

HLE64("ucrtbase.dll", strpbrk, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strpbrk(p, S(Arg(c, 1)));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("ucrtbase.dll", strncat, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 1);
  uint64_t n = Arg(c, 2);
  gaddr e = d + std::strlen(S(d));
  uint64_t i = 0;
  for (; i < n && Rd8(src + i); i++) Wr8(e + i, Rd8(src + i));
  Wr8(e + i, 0);
  return d;
}
// In the "C" locale strings collate by their bytes.
HLE64("ucrtbase.dll", strcoll, kCdecl) { return uint32_t(Sign(std::strcmp(S(Arg(c, 0)), S(Arg(c, 1))))); }

// strcpy_s / strcat_s / strncpy_s: 0, or EINVAL / ERANGE with the
// destination emptied (the invalid-parameter handler returns).
namespace {
uint64_t CopyS(gaddr d, uint64_t size, const std::string& src, gaddr dst_from) {
  if (!d || !size) return EINVAL;
  if (dst_from - d + src.size() + 1 > size) {
    Wr8(d, 0);
    SetErrno(ERANGE);
    return ERANGE;
  }
  std::memcpy(GPtr(dst_from), src.c_str(), src.size() + 1);
  return 0;
}
}  // namespace
HLE64("ucrtbase.dll", strcpy_s, kCdecl) {
  gaddr d = Arg(c, 0);
  if (!Arg(c, 2)) return d && Arg(c, 1) ? (Wr8(d, 0), EINVAL) : EINVAL;
  return CopyS(d, Arg(c, 1), S(Arg(c, 2)), d);
}
HLE64("ucrtbase.dll", strcat_s, kCdecl) {
  gaddr d = Arg(c, 0);
  if (!d || !Arg(c, 2)) return EINVAL;
  return CopyS(d, Arg(c, 1), S(Arg(c, 2)), d + std::strlen(S(d)));
}
// strncpy_s(dst, size, src, count): count _TRUNCATE ((size_t)-1) truncates
// to the buffer and returns STRUNCATE (80).
HLE64("ucrtbase.dll", strncpy_s, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 2);
  uint64_t size = Arg(c, 1), count = Arg(c, 3);
  if (!d || !size) return EINVAL;
  if (!src) {
    Wr8(d, 0);
    return count ? EINVAL : 0;
  }
  std::string s;
  for (gaddr q = src; s.size() < count && Rd8(q); q++) s.push_back(char(Rd8(q)));
  if (count == ~uint64_t(0) && s.size() >= size) {
    s.resize(size - 1);
    std::memcpy(GPtr(d), s.c_str(), s.size() + 1);
    return 80;
  }
  return CopyS(d, size, s, d);
}

HLE64("ucrtbase.dll", wcsncmp, kCdecl) { return uint32_t(WCmp(Arg(c, 0), Arg(c, 1), Arg(c, 2), false)); }
HLE64("ucrtbase.dll", _wcsicmp, kCdecl) { return uint32_t(WCmp(Arg(c, 0), Arg(c, 1), SIZE_MAX, true)); }
HLE64("ucrtbase.dll", _wcsnicmp, kCdecl) { return uint32_t(WCmp(Arg(c, 0), Arg(c, 1), Arg(c, 2), true)); }
HLE64("ucrtbase.dll", wcsncpy, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 1);
  uint64_t n = Arg(c, 2), i = 0;
  for (; i < n && Rd16(src + 2 * i); i++) Wr16(d + 2 * i, Rd16(src + 2 * i));
  for (; i < n; i++) Wr16(d + 2 * i, 0);
  return d;
}
HLE64("ucrtbase.dll", wcsspn, kCdecl) {
  std::u16string s = WStr(Arg(c, 0)), set = WStr(Arg(c, 1));
  size_t i = 0;
  while (i < s.size() && set.find(s[i]) != std::u16string::npos) i++;
  return i;
}
HLE64("ucrtbase.dll", wcspbrk, kCdecl) {
  std::u16string s = WStr(Arg(c, 0)), set = WStr(Arg(c, 1));
  size_t i = s.find_first_of(set);
  return i == std::u16string::npos ? 0 : Arg(c, 0) + 2 * i;
}

// The wide and 64-bit conversions, on the narrow ones (ASCII digits).
HLE64("ucrtbase.dll", _atoi64, kCdecl) { return uint64_t(std::strtoll(S(Arg(c, 0)), nullptr, 10)); }
HLE64("ucrtbase.dll", _wtoi64, kCdecl) {
  return uint64_t(std::strtoll(win32::Narrow(WStr(Arg(c, 0))).c_str(), nullptr, 10));
}
HLE64("ucrtbase.dll", _wtol, kCdecl) {
  long long v = std::strtoll(win32::Narrow(WStr(Arg(c, 0))).c_str(), nullptr, 10);
  return uint32_t(int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v));
}
HLE64("ucrtbase.dll", _wtof, kCdecl) {
  return RetD(c, std::strtod(win32::Narrow(WStr(Arg(c, 0))).c_str(), nullptr));
}
// The _l forms take a locale, which is always "C" here.
HLE64("ucrtbase.dll", _strtod_l, kCdecl) {
  return RetD(c, StrTo<double>(Arg(c, 0), Arg(c, 1),
                               [](const char* p, char** e) { return std::strtod(p, e); }));
}
HLE64("ucrtbase.dll", _strtoi64_l, kCdecl) {
  int base = int(Arg(c, 2));
  return uint64_t(StrTo<long long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    return std::strtoll(p, e, base);
  }));
}
HLE64("ucrtbase.dll", _strtoui64_l, kCdecl) {
  int base = int(Arg(c, 2));
  return StrTo<unsigned long long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    return std::strtoull(p, e, base);
  });
}
// _ui64toa_s(value, buffer, size, radix): 0, or EINVAL / ERANGE.
HLE64("ucrtbase.dll", _ui64toa_s, kCdecl) {
  uint64_t v = Arg(c, 0), size = Arg(c, 2);
  gaddr buf = Arg(c, 1);
  int radix = int(Arg(c, 3));
  if (!buf || !size || radix < 2 || radix > 36) return EINVAL;
  std::string s;
  do {
    int d = int(v % uint64_t(radix));
    s.insert(s.begin(), char(d < 10 ? '0' + d : 'a' + d - 10));
    v /= uint64_t(radix);
  } while (v);
  if (s.size() + 1 > size) {
    Wr8(buf, 0);
    return ERANGE;
  }
  PutStr(buf, s);
  return 0;
}

HLE64("ucrtbase.dll", frexp, kCdecl) {
  int e = 0;
  double m = std::frexp(ArgD(c, 0), &e);
  Wr32(Arg(c, 1), uint32_t(e));
  return RetD(c, m);
}
HLE64("ucrtbase.dll", _finite, kCdecl) { return std::isfinite(ArgD(c, 0)) ? 1 : 0; }
HLE64("ucrtbase.dll", _isnan, kCdecl) { return std::isnan(ArgD(c, 0)) ? 1 : 0; }
// _fdtest(float*): _NANCODE 2, _INFCODE 1, 0 for zero, _DENORM -2,
// _FINITE -1.
HLE64("ucrtbase.dll", _fdtest, kCdecl) {
  float v;
  uint32_t b = Rd32(Arg(c, 0));
  std::memcpy(&v, &b, 4);
  int r = std::isnan(v) ? 2 : std::isinf(v) ? 1 : v == 0 ? 0 : std::fpclassify(v) == FP_SUBNORMAL ? -2 : -1;
  return uint32_t(int16_t(r)) & 0xFFFF;
}
// ldiv(numer, denom): ldiv_t {quot, rem} of 32-bit longs, returned in rax.
HLE64("ucrtbase.dll", ldiv, kCdecl) {
  int32_t n = int32_t(Arg(c, 0)), d = int32_t(Arg(c, 1));
  return uint64_t(uint32_t(n / d)) | uint64_t(uint32_t(n % d)) << 32;
}
// rand's seed (the generator is the UCRT's: see rand).
HLE64("ucrtbase.dll", srand, kCdecl) {
  RandSeed() = uint32_t(Arg(c, 0));
  return 0;
}

// setlocale: only "C" (and "", which is "C" too) exists; the result is
// the locale's name. A query (null) answers "C".
HLE64("ucrtbase.dll", setlocale, kCdecl) {
  static gaddr name = [] {
    gaddr p = HeapAllocProcess(2, true);
    PutStr(p, "C");
    return p;
  }();
  gaddr l = Arg(c, 1);
  if (!l) return name;
  std::string want = GStr(l);
  return want.empty() || want == "C" ? name : 0;
}
// _create_locale(category, name): a _locale_t the _l functions take and
// ignore; the "C" locale's only.
HLE64("ucrtbase.dll", _create_locale, kCdecl) {
  std::string want = Arg(c, 1) ? GStr(Arg(c, 1)) : "";
  if (!want.empty() && want != "C") return 0;
  static gaddr loc = HeapAllocProcess(16, true);
  return loc;
}

HLE64("ucrtbase.dll", _aligned_realloc, kCdecl) {
  gaddr p = Arg(c, 0);
  uint64_t n = Arg(c, 1), align = std::max<uint64_t>(Arg(c, 2), 8);
  if (align & (align - 1)) return 0;
  if (p && !n) {
    HeapFreeProcess(Rd64(p - 8));
    return 0;
  }
  gaddr block = HeapAllocProcess(n + align + 8, false);
  if (!block) return 0;
  gaddr q = (block + 8 + align - 1) & ~(align - 1);
  Wr64(q - 8, block);
  if (p) {
    gaddr old = Rd64(p - 8);
    uint64_t have = HeapSizeProcess(old) - (p - old);
    std::memcpy(GPtr(q), GPtr(p), std::min(have, n));
    HeapFreeProcess(old);
  }
  return q;
}

// at_quick_exit handlers never run: quick_exit is not provided.
HLE64("ucrtbase.dll", _crt_at_quick_exit, kCdecl) { return 0; }
HLE64("ucrtbase.dll", _endthreadex, kCdecl) {
  GuestSyncFence();
  throw GuestThreadExit{uint32_t(Arg(c, 0))};
}
HLE64("ucrtbase.dll", _invalid_parameter_noinfo, kCdecl) {
  FatalF("guest CRT: invalid parameter (return address %08llX)", (unsigned long long)Rd64(c.rsp));
}
HLE64("ucrtbase.dll", abort, kCdecl) { FatalF("guest called abort()"); }
HLE64("ucrtbase.dll", raise, kCdecl) { FatalF("guest raised signal %d", int(int32_t(Arg(c, 0)))); }
// No command processor: system(NULL) says so, and a command fails.
HLE64("ucrtbase.dll", system, kCdecl) {
  if (Arg(c, 0)) SetErrno(ENOENT);
  return Arg(c, 0) ? uint32_t(-1) : 0;
}
HLE64("ucrtbase.dll", strerror, kCdecl) {
  static gaddr buf = HeapAllocProcess(96, true);
  PutStr(buf, std::string(std::strerror(int(int32_t(Arg(c, 0))))).substr(0, 94));
  return buf;
}

// ---- paths ----------------------------------------------------------------------

// _fullpath(buffer, path, size): with a null buffer, a malloc'd one.
HLE64("ucrtbase.dll", _fullpath, kCdecl) {
  std::string full = GuestFullPath(GStr(Arg(c, 1)));
  gaddr buf = Arg(c, 0);
  if (!buf) buf = HeapAllocProcess(full.size() + 1, false);
  else if (full.size() + 1 > Arg(c, 2)) {
    SetErrno(ERANGE);
    return 0;
  }
  PutStr(buf, full);
  return buf;
}
HLE64("ucrtbase.dll", _wgetcwd, kCdecl) {
  std::string d = GuestCurrentDir();
  gaddr buf = Arg(c, 0);
  if (!buf) buf = HeapAllocProcess(2 * (d.size() + 1), false);
  else if (d.size() + 1 > uint64_t(int32_t(Arg(c, 1)))) {
    SetErrno(ERANGE);
    return 0;
  }
  PutWStr(buf, d);
  return buf;
}
namespace {
// _splitpath's parts of p: drive "C:", directory up to its last
// separator, file name, extension from the last dot.
void SplitPath(const std::string& p, std::string* drive, std::string* dir, std::string* name,
               std::string* ext) {
  size_t at = 0;
  *drive = p.size() >= 2 && p[1] == ':' ? p.substr(0, 2) : "";
  at = drive->size();
  size_t slash = p.find_last_of("\\/");
  *dir = slash == std::string::npos || slash < at ? "" : p.substr(at, slash + 1 - at);
  std::string file = p.substr(slash == std::string::npos ? at : std::max(slash + 1, at));
  size_t dot = file.find_last_of('.');
  *name = dot == std::string::npos ? file : file.substr(0, dot);
  *ext = dot == std::string::npos ? "" : file.substr(dot);
}
}  // namespace
HLE64("ucrtbase.dll", _splitpath, kCdecl) {
  std::string parts[4];
  SplitPath(GStr(Arg(c, 0)), &parts[0], &parts[1], &parts[2], &parts[3]);
  for (int i = 0; i < 4; i++)
    if (gaddr out = Arg(c, 1 + i)) PutStr(out, parts[i]);
  return 0;
}
HLE64("ucrtbase.dll", _wsplitpath, kCdecl) {
  std::string parts[4];
  SplitPath(win32::Narrow(WStr(Arg(c, 0))), &parts[0], &parts[1], &parts[2], &parts[3]);
  for (int i = 0; i < 4; i++)
    if (gaddr out = Arg(c, 1 + i)) PutWStr(out, parts[i]);
  return 0;
}
HLE64("ucrtbase.dll", remove, kCdecl) {
  if (std::remove(HostPath(GStr(Arg(c, 0)), PathAccess::kWrite).c_str()) != 0) {
    SetErrno(uint32_t(errno));
    return uint32_t(-1);
  }
  return 0;
}
HLE64("ucrtbase.dll", rename, kCdecl) {
  if (std::rename(HostPath(GStr(Arg(c, 0)), PathAccess::kUpdate).c_str(),
                  HostPath(GStr(Arg(c, 1)), PathAccess::kWrite).c_str()) != 0) {
    SetErrno(uint32_t(errno));
    return uint32_t(-1);
  }
  return 0;
}

}  // namespace recomp::hle
