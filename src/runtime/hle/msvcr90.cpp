// The Visual Studio 2008 C runtime (msvcr90.dll) for 32-bit guests.
//
// For games whose own CRT is statically linked but whose lifted modules
// (FMOD's DLLs, for one) import msvcr90 dynamically: their DllMain and
// everything it reaches need this. cdecl throughout, 4-byte pointers,
// doubles returned in st(0), and 4-byte va_list slots.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <glob.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"
#include "runtime/x87.h"

namespace recomp::hle {
namespace {

// ---- guest heap ------------------------------------------------------------

gaddr Malloc(uint32_t n) { return HeapAllocProcess(n ? n : 1, false); }

gaddr Calloc(uint32_t n, uint32_t size) {
  uint64_t total = uint64_t(n) * size;
  if (size && total / size != n) return 0;
  return HeapAllocProcess(total ? total : 1, true);
}

gaddr Realloc(gaddr p, uint32_t n) {
  if (!p) return Malloc(n);
  return HeapReAllocProcess(p, n ? n : 1);
}

void Free(gaddr p) {
  if (p) HeapFreeProcess(p);
}

// ---- errno -----------------------------------------------------------------

// The calling guest thread's errno, in guest memory (an int).
gaddr ErrnoSlot() {
  static std::mutex mu;
  static std::map<uint32_t, gaddr> slots;
  std::lock_guard<std::mutex> l(mu);
  gaddr& s = slots[CurrentThread()->tid];
  if (!s) s = HeapAllocProcess(8, true);
  return s;
}

void SetErrno(int e) { Wr32(ErrnoSlot(), uint32_t(e)); }

// ---- x87 results -----------------------------------------------------------

// x86 returns a double in st(0).
uint64_t RetX87(Ctx& c, double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  x87::LoadF64(c.fpu, bits);
  return 0;
}

double DArg(const Ctx& c, int i) {
  uint64_t bits = Rd64(c.esp + 4 + 8 * uint32_t(i));
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}

// ---- varargs ---------------------------------------------------------------

// A guest va_list on x86: 4-byte slots for everything but 64-bit and
// floating-point arguments, which occupy 8 and are 8-byte aligned.
struct VaList {
  gaddr p;
  uint32_t Next() {
    uint32_t v = Rd32(p);
    p += 4;
    return v;
  }
  uint64_t Next64() {
    p = (p + 7u) & ~7u;
    uint64_t v = Rd64(p);
    p += 8;
    return v;
  }
};

std::string NanText(double v, bool upper) {
  uint64_t b;
  std::memcpy(&b, &v, 8);
  std::string s = (b >> 63) ? "-" : "";
  bool quiet = b & (1ull << 51);
  if (!quiet)
    s += "nan(snan)";
  else if ((b & 0x7FFFFFFFFFFFFull) == 0x8000000000000ull && (b >> 63))
    s += "nan(ind)";
  else
    s += "nan";
  if (upper)
    for (char& ch : s) ch = char(std::toupper(uint8_t(ch)));
  return s;
}

std::string Pad(const std::string& body, int width, bool left) {
  if (int(body.size()) >= width) return body;
  return left ? body + std::string(width - body.size(), ' ')
              : std::string(width - body.size(), ' ') + body;
}

// MSVC 2008's printf over a guest format and va_list.
std::string Format(const std::string& fmt, VaList a) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    size_t start = i++;
    std::string flags;
    while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) flags.push_back(fmt[i++]);
    int width = -1, prec = -1;
    if (i < fmt.size() && fmt[i] == '*') {
      width = int32_t(a.Next());
      if (width < 0) {
        flags.push_back('-');
        width = -width;
      }
      i++;
    } else {
      while (i < fmt.size() && std::isdigit(uint8_t(fmt[i])))
        width = (width < 0 ? 0 : width) * 10 + (fmt[i++] - '0');
    }
    if (i < fmt.size() && fmt[i] == '.') {
      i++;
      prec = 0;
      if (i < fmt.size() && fmt[i] == '*') {
        prec = int32_t(a.Next());
        i++;
      } else {
        while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) prec = prec * 10 + (fmt[i++] - '0');
      }
    }
    int bits = 32;
    bool wide = false, narrow = false;
    for (;;) {
      if (i >= fmt.size()) break;
      char ch = fmt[i];
      if (ch == 'h') {
        bits = (i + 1 < fmt.size() && fmt[i + 1] == 'h') ? (i++, 8) : 16;
        narrow = true;
        i++;
      } else if (ch == 'l') {
        if (i + 1 < fmt.size() && fmt[i + 1] == 'l') {
          bits = 64;
          i += 2;
        } else {
          wide = true;
          i++;
        }
      } else if (ch == 'L' || ch == 'j' || ch == 'z' || ch == 't') {
        bits = 64;
        i++;
      } else if (ch == 'w') {
        wide = true;
        i++;
      } else if (ch == 'I') {
        if (fmt.compare(i, 3, "I64") == 0) {
          bits = 64;
          i += 3;
        } else if (fmt.compare(i, 3, "I32") == 0) {
          bits = 32;
          i += 3;
        } else {
          bits = 32;
          i++;
        }
      } else {
        break;
      }
    }
    if (i >= fmt.size()) {
      out += fmt.substr(start);
      break;
    }
    char conv = fmt[i];
    std::string spec = "%" + flags + (width >= 0 ? std::to_string(width) : "") +
                       (prec >= 0 ? "." + std::to_string(prec) : "");
    char buf[512];
    switch (conv) {
      case '%':
        out.push_back('%');
        break;
      case 'd': case 'i': {
        uint64_t v = bits == 64 ? a.Next64() : a.Next();
        long long s = bits == 64 ? int64_t(v) : bits == 16 ? int16_t(v)
                      : bits == 8  ? int8_t(v)
                                   : int32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), s);
        out += buf;
        break;
      }
      case 'u': case 'o': case 'x': case 'X': {
        uint64_t v = bits == 64 ? a.Next64() : a.Next();
        unsigned long long u = bits == 64 ? v : bits == 16 ? uint16_t(v)
                               : bits == 8  ? uint8_t(v)
                                            : uint32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), u);
        out += buf;
        break;
      }
      case 'p': {
        std::snprintf(buf, sizeof buf, "%08X", a.Next());
        out += Pad(buf, width, flags.find('-') != std::string::npos);
        break;
      }
      case 'c': case 'C': {
        uint32_t v = a.Next();
        bool w = conv == 'C' ? !narrow : wide;
        char ch = w ? win32::Narrow(std::u16string(1, char16_t(v)))[0] : char(v);
        out += Pad(std::string(1, ch), width, flags.find('-') != std::string::npos);
        break;
      }
      case 's': case 'S': {
        gaddr p = a.Next();
        bool w = conv == 'S' ? !narrow : wide;
        std::string s = !p ? "(null)" : w ? win32::Narrow(GWStr(p)) : GStr(p);
        if (prec >= 0 && size_t(prec) < s.size()) s.resize(size_t(prec));
        out += Pad(s, width, flags.find('-') != std::string::npos);
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        double v = [&] {
          uint64_t bitsv = a.Next64();
          double d;
          std::memcpy(&d, &bitsv, 8);
          return d;
        }();
        bool upper = std::isupper(uint8_t(conv));
        if (std::isnan(v) || std::isinf(v)) {
          std::string s = std::isnan(v) ? NanText(v, upper)
                                        : std::string(std::signbit(v) ? "-" : "") +
                                              (upper ? "INF" : "inf");
          out += Pad(s, width, flags.find('-') != std::string::npos);
        } else {
          char spec2[32];
          std::snprintf(spec2, sizeof spec2, "%s%c", spec.c_str(),
                        upper ? char(std::toupper(uint8_t(conv))) : conv);
          std::snprintf(buf, sizeof buf, spec2, v);
          out += buf;
        }
        break;
      }
      default:
        out += fmt.substr(start, i - start + 1);
        break;
    }
  }
  return out;
}

// ---- stdio -----------------------------------------------------------------

struct Stream {
  FILE* f = nullptr;
  bool owned = true;
};

std::recursive_mutex g_streams_mu;
std::map<gaddr, Stream> g_streams;

gaddr NewStream(FILE* f, bool owned) {
  gaddr p = HeapAllocProcess(16, true);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  g_streams[p] = {f, owned};
  return p;
}

Stream* Find(gaddr fp) {
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  auto it = g_streams.find(fp);
  return it == g_streams.end() ? nullptr : &it->second;
}

// __iob_func: the three standard streams, as 32-byte _iobuf blocks (their
// internals stay private to us; the address is the FILE*).
gaddr StdIob(uint32_t i) {
  static gaddr base = [] {
    gaddr b = HeapAllocProcess(3 * 32, true);
    std::lock_guard<std::recursive_mutex> l(g_streams_mu);
    g_streams[b + 0] = {stdin, false};
    g_streams[b + 32] = {stdout, false};
    g_streams[b + 64] = {stderr, false};
    return b;
  }();
  return i < 3 ? base + 32 * i : 0;
}

std::string ReadCStr(gaddr p) { return p ? GStr(p) : std::string(); }

// ---- guest paths -----------------------------------------------------------

bool IsAbsoluteGuest(const std::string& p) {
  return (p.size() >= 2 && p[1] == ':') || (!p.empty() && (p[0] == '\\' || p[0] == '/'));
}

// A guest path with "." and ".." resolved, as _fullpath returns it.
std::string NormaliseGuest(const std::string& p) {
  std::string full = IsAbsoluteGuest(p) ? p : GuestCurrentDir() + "\\" + p;
  std::vector<std::string> out;
  size_t i = 0;
  while (i < full.size()) {
    while (i < full.size() && (full[i] == '\\' || full[i] == '/')) i++;
    size_t j = i;
    while (j < full.size() && full[j] != '\\' && full[j] != '/') j++;
    std::string c = full.substr(i, j - i);
    i = j;
    if (c.empty() || c == ".") continue;
    if (c == "..") {
      if (!out.empty()) out.pop_back();
      continue;
    }
    out.push_back(c);
  }
  std::string r = out.empty() ? "" : out[0];
  for (size_t k = 1; k < out.size(); k++) r += "\\" + out[k];
  return r;
}

// ---- directory enumeration -------------------------------------------------

struct FindState {
  std::vector<std::string> names;
  size_t next = 0;
};

std::mutex g_find_mu;
std::map<uint32_t, FindState> g_finds;
uint32_t g_next_find = 1;

void WriteFindData(gaddr data, const std::string& host, const std::string& name) {
  std::memset(GPtr(data), 0, 296);
  struct stat st;
  bool ok = ::stat(host.c_str(), &st) == 0;
  uint32_t attrib = 0;
  if (ok) attrib = S_ISDIR(st.st_mode) ? 0x10 : 0x80;  // _A_SUBDIR, _A_NORMAL
  Wr32(data + 0, attrib);
  if (ok) {
    Wr64(data + 8, uint64_t(st.st_ctime));
    Wr64(data + 16, uint64_t(st.st_atime));
    Wr64(data + 24, uint64_t(st.st_mtime));
    Wr32(data + 32, uint32_t(st.st_size));
  }
  std::string n = name.size() > 259 ? name.substr(0, 259) : name;
  std::memcpy(GPtr(data + 36), n.c_str(), n.size() + 1);
}

}  // namespace

// ---- startup and teardown --------------------------------------------------

// _initterm(first, last): every non-null function pointer in [first, last).
HLE("msvcr90.dll", _initterm, kCdecl) {
  for (gaddr p = Arg(c, 0), end = Arg(c, 1); p < end; p += 4)
    if (gaddr fn = Rd32(p)) CallGuest(c, fn, {});
  return 0;
}

// _initterm_e: the same with int-returning functions; the first nonzero
// result stops it and is returned.
HLE("msvcr90.dll", _initterm_e, kCdecl) {
  for (gaddr p = Arg(c, 0), end = Arg(c, 1); p < end; p += 4)
    if (gaddr fn = Rd32(p)) {
      uint32_t r = uint32_t(CallGuest(c, fn, {}));
      if (r) return r;
    }
  return 0;
}

// _onexit / __dllonexit: the FMOD DLLs register nothing that matters at
// exit; accept the registration and hand the pointer back.
HLE("msvcr90.dll", _onexit, kCdecl) { return uint32_t(Arg(c, 0)); }
HLE_NAMED("msvcr90.dll", "__dllonexit", _dllonexit, kCdecl) { return uint32_t(Arg(c, 0)); }

// _amsg_exit: a startup/runtime error message. Nothing to show it in.
HLE("msvcr90.dll", _amsg_exit, kCdecl) {
  RECOMP_TRACE("msvcr90: _amsg_exit(%u)\n", uint32_t(Arg(c, 0)));
  return 0;
}

HLE("msvcr90.dll", _crt_debugger_hook, kCdecl) { return 0; }
// _adjust_fdiv is a variable (the floating-point adjust flag), not a call.
DataExport g_adjust_fdiv("msvcr90.dll", "_adjust_fdiv", 4);
HLE("msvcr90.dll", _encoded_null, kCdecl) { return 0; }
HLE("msvcr90.dll", _encode_pointer, kCdecl) { return uint32_t(Arg(c, 0)); }
HLE("msvcr90.dll", _decode_pointer, kCdecl) { return uint32_t(Arg(c, 0)); }
HLE("msvcr90.dll", _lock, kCdecl) { return 0; }
HLE("msvcr90.dll", _unlock, kCdecl) { return 0; }
HLE("msvcr90.dll", _errno, kCdecl) { return ErrnoSlot(); }
HLE("msvcr90.dll", _malloc_crt, kCdecl) { return Malloc(uint32_t(Arg(c, 0))); }

// A pure virtual call: report it but keep the process alive (FMOD has
// abstract interfaces; this is never expected).
HLE("msvcr90.dll", _purecall, kCdecl) {
  WarnF("msvcr90: pure virtual call\n");
  return 0;
}

// ---- C++ exceptions and RTTI ----------------------------------------------
//
// No lifted x86 code here throws: these are reachable only through a
// __try/__except the FMOD DLLs do not use, so a continue-search answer is
// enough to keep the unwinder from wedging.

HLE_NAMED("msvcr90.dll", "__CxxFrameHandler3", _CxxFrameHandler3, kCdecl) { return 1; }
HLE_NAMED("msvcr90.dll", "_except_handler4_common", _except_handler4, kCdecl) { return 1; }
HLE_NAMED("msvcr90.dll", "__CppXcptFilter", _CppXcptFilter, kCdecl) { return 1; }
HLE_NAMED("msvcr90.dll", "__clean_type_info_names_internal", _clean_type_info, kCdecl) {
  return 0;
}
HLE_NAMED("msvcr90.dll", "?terminate@@YAXXZ", _terminate, kCdecl) {
  WarnF("msvcr90: terminate() called\n");
  return 0;
}
HLE_NAMED("msvcr90.dll", "?_type_info_dtor_internal_method@type_info@@QAEXXZ",
          _type_info_dtor, kCdecl) {
  return 0;
}

// operator new / operator delete
HLE_NAMED("msvcr90.dll", "??2@YAPAXI@Z", _op_new, kCdecl) { return Malloc(uint32_t(Arg(c, 0))); }
HLE_NAMED("msvcr90.dll", "??3@YAXPAX@Z", _op_delete, kCdecl) {
  Free(uint32_t(Arg(c, 0)));
  return 0;
}

// ---- heap ------------------------------------------------------------------

HLE("msvcr90.dll", malloc, kCdecl) { return Malloc(uint32_t(Arg(c, 0))); }
HLE("msvcr90.dll", calloc, kCdecl) { return Calloc(uint32_t(Arg(c, 0)), uint32_t(Arg(c, 1))); }
HLE("msvcr90.dll", realloc, kCdecl) { return Realloc(uint32_t(Arg(c, 0)), uint32_t(Arg(c, 1))); }
HLE("msvcr90.dll", free, kCdecl) {
  Free(uint32_t(Arg(c, 0)));
  return 0;
}

// ---- string and memory -----------------------------------------------------

HLE("msvcr90.dll", memcpy, kCdecl) {
  gaddr d = Arg(c, 0);
  uint32_t n = uint32_t(Arg(c, 2));
  if (n) std::memcpy(GPtr(d), GPtr(Arg(c, 1)), n);
  return d;
}

HLE("msvcr90.dll", memmove, kCdecl) {
  gaddr d = Arg(c, 0);
  uint32_t n = uint32_t(Arg(c, 2));
  if (n) std::memmove(GPtr(d), GPtr(Arg(c, 1)), n);
  return d;
}

HLE("msvcr90.dll", memset, kCdecl) {
  gaddr d = Arg(c, 0);
  std::memset(GPtr(d), int(uint8_t(Arg(c, 1))), uint32_t(Arg(c, 2)));
  return d;
}

HLE("msvcr90.dll", memcmp, kCdecl) {
  return uint32_t(int32_t(std::memcmp(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), uint32_t(Arg(c, 2)))));
}

HLE("msvcr90.dll", memchr, kCdecl) {
  gaddr d = Arg(c, 0);
  uint8_t* base = GPtr(d);
  void* p = std::memchr(base, int(uint8_t(Arg(c, 1))), uint32_t(Arg(c, 2)));
  return p ? gaddr(uint32_t(static_cast<uint8_t*>(p) - base)) : 0;
}

HLE("msvcr90.dll", strlen, kCdecl) { return uint32_t(GStr(Arg(c, 0)).size()); }

HLE("msvcr90.dll", strcpy, kCdecl) {
  gaddr d = Arg(c, 0);
  PutStr(d, GStr(Arg(c, 1)));
  return d;
}

HLE("msvcr90.dll", strncpy, kCdecl) {
  gaddr d = Arg(c, 0);
  uint32_t n = uint32_t(Arg(c, 2));
  std::string s = GStr(Arg(c, 1));
  uint32_t k = uint32_t(std::min<size_t>(s.size(), n));
  if (k) std::memcpy(GPtr(d), s.data(), k);
  for (uint32_t i = k; i < n; i++) Wr8(d + i, 0);
  return d;
}

HLE("msvcr90.dll", strcat, kCdecl) {
  gaddr d = Arg(c, 0);
  std::string a = GStr(d), b = GStr(Arg(c, 1));
  PutStr(d, a + b);
  return d;
}

HLE("msvcr90.dll", strcmp, kCdecl) {
  return uint32_t(int32_t(std::strcmp(reinterpret_cast<const char*>(GPtr(Arg(c, 0))), reinterpret_cast<const char*>(GPtr(Arg(c, 1))))));
}

HLE("msvcr90.dll", atoi, kCdecl) { return uint32_t(int32_t(std::atoi(reinterpret_cast<const char*>(GPtr(Arg(c, 0)))))); }

HLE("msvcr90.dll", getenv, kCdecl) {
  std::string want = GStr(Arg(c, 0)) + "=";
  gaddr p = g_proc->environ_a;
  if (!p) return 0;
  const char* s = g_proc->mem.ptr<const char>(p);
  while (s && *s) {
    size_t len = std::strlen(s);
    if (std::strncmp(s, want.c_str(), want.size()) == 0) return p + uint32_t(want.size());
    p += uint32_t(len) + 1;
    s += len + 1;
  }
  return 0;
}

HLE("msvcr90.dll", qsort, kCdecl) {
  gaddr base = Arg(c, 0);
  uint32_t n = uint32_t(Arg(c, 1)), width = uint32_t(Arg(c, 2));
  gaddr cmp = Arg(c, 3);
  if (n < 2 || !width || !cmp) return 0;
  std::vector<uint8_t> tmp(width);
  // An insertion sort: the arrays FMOD sorts are short.
  for (uint32_t i = 1; i < n; i++) {
    std::memcpy(tmp.data(), GPtr(base + i * width), width);
    uint32_t j = i;
    while (j > 0) {
      int32_t r = int32_t(CallGuest(c, cmp, {base + (j - 1) * width, gaddr(tmp.data())}));
      if (r <= 0) break;
      std::memcpy(GPtr(base + j * width), GPtr(base + (j - 1) * width), width);
      j--;
    }
    std::memcpy(GPtr(base + j * width), tmp.data(), width);
  }
  return 0;
}

// ---- rand ------------------------------------------------------------------

// MSVC's rand: state = state * 214013 + 2531011, the top 15 bits returned.
uint32_t g_rand_state = 1;

HLE("msvcr90.dll", srand, kCdecl) {
  g_rand_state = uint32_t(Arg(c, 0));
  return 0;
}

HLE("msvcr90.dll", rand, kCdecl) {
  g_rand_state = g_rand_state * 214013u + 2531011u;
  return (g_rand_state >> 16) & 0x7FFF;
}

// ---- math (doubles in st(0)) ----------------------------------------------

HLE("msvcr90.dll", sqrt, kCdecl) { return RetX87(c, std::sqrt(DArg(c, 0))); }
HLE("msvcr90.dll", fabs, kCdecl) { return RetX87(c, std::fabs(DArg(c, 0))); }
HLE("msvcr90.dll", floor, kCdecl) { return RetX87(c, std::floor(DArg(c, 0))); }
HLE("msvcr90.dll", sin, kCdecl) { return RetX87(c, std::sin(DArg(c, 0))); }
HLE("msvcr90.dll", cos, kCdecl) { return RetX87(c, std::cos(DArg(c, 0))); }
HLE("msvcr90.dll", tan, kCdecl) { return RetX87(c, std::tan(DArg(c, 0))); }
HLE("msvcr90.dll", atan, kCdecl) { return RetX87(c, std::atan(DArg(c, 0))); }
HLE("msvcr90.dll", atan2, kCdecl) { return RetX87(c, std::atan2(DArg(c, 0), DArg(c, 1))); }
HLE("msvcr90.dll", acos, kCdecl) { return RetX87(c, std::acos(DArg(c, 0))); }
HLE("msvcr90.dll", exp, kCdecl) { return RetX87(c, std::exp(DArg(c, 0))); }
HLE("msvcr90.dll", log, kCdecl) { return RetX87(c, std::log(DArg(c, 0))); }
HLE("msvcr90.dll", log10, kCdecl) { return RetX87(c, std::log10(DArg(c, 0))); }
HLE("msvcr90.dll", pow, kCdecl) { return RetX87(c, std::pow(DArg(c, 0), DArg(c, 1))); }

HLE("msvcr90.dll", ldexp, kCdecl) {
  return RetX87(c, std::ldexp(DArg(c, 0), int32_t(Arg(c, 2))));
}

HLE("msvcr90.dll", frexp, kCdecl) {
  int e = 0;
  double m = std::frexp(DArg(c, 0), &e);
  gaddr pe = Arg(c, 1);
  if (pe) Wr32(pe, uint32_t(e));
  return RetX87(c, m);
}

HLE("msvcr90.dll", abs, kCdecl) {
  return uint32_t(int32_t(std::abs(int32_t(Arg(c, 0)))));
}

HLE("msvcr90.dll", _CIacos, kCdecl) { return RetX87(c, std::acos(DArg(c, 0))); }
HLE("msvcr90.dll", _CIatan2, kCdecl) { return RetX87(c, std::atan2(DArg(c, 0), DArg(c, 1))); }
HLE("msvcr90.dll", _CIexp, kCdecl) { return RetX87(c, std::exp(DArg(c, 0))); }
HLE("msvcr90.dll", _CIlog, kCdecl) { return RetX87(c, std::log(DArg(c, 0))); }
HLE("msvcr90.dll", _CIlog10, kCdecl) { return RetX87(c, std::log10(DArg(c, 0))); }
HLE("msvcr90.dll", _CIpow, kCdecl) { return RetX87(c, std::pow(DArg(c, 0), DArg(c, 1))); }
HLE("msvcr90.dll", _CIsqrt, kCdecl) { return RetX87(c, std::sqrt(DArg(c, 0))); }

// ---- stdio -----------------------------------------------------------------

// __iob_func: the address of the three standard _iobufs.
HLE_NAMED("msvcr90.dll", "__iob_func", _iob_func, kCdecl) { return StdIob(0); }

HLE("msvcr90.dll", fopen, kCdecl) {
  std::string path = GStr(Arg(c, 0)), mode = GStr(Arg(c, 1));
  bool writing = mode.find('w') != std::string::npos || mode.find('a') != std::string::npos ||
                 mode.find('+') != std::string::npos;
  std::string host = HostPath(path, writing ? PathAccess::kWrite : PathAccess::kRead);
  FILE* f = std::fopen(host.c_str(), mode.c_str());
  if (!f) {
    SetErrno(ENOENT);
    return 0;
  }
  return NewStream(f, true);
}

HLE("msvcr90.dll", fclose, kCdecl) {
  gaddr fp = Arg(c, 0);
  Stream* s = Find(fp);
  if (!s) return uint32_t(-1);
  int r = s->owned ? std::fclose(s->f) : 0;
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  g_streams.erase(fp);
  HeapFreeProcess(fp);
  return uint32_t(int32_t(r));
}

HLE("msvcr90.dll", fread, kCdecl) {
  gaddr buf = Arg(c, 0);
  uint32_t size = uint32_t(Arg(c, 1)), count = uint32_t(Arg(c, 2));
  Stream* s = Find(Arg(c, 3));
  if (!s) return 0;
  size_t n = std::fread(GPtr(buf), size, count, s->f);
  return uint32_t(n);
}

HLE("msvcr90.dll", fwrite, kCdecl) {
  gaddr buf = Arg(c, 0);
  uint32_t size = uint32_t(Arg(c, 1)), count = uint32_t(Arg(c, 2));
  Stream* s = Find(Arg(c, 3));
  if (!s) return 0;
  size_t n = std::fwrite(GPtr(buf), size, count, s->f);
  return uint32_t(n);
}

HLE("msvcr90.dll", fseek, kCdecl) {
  Stream* s = Find(Arg(c, 0));
  if (!s) return uint32_t(-1);
  return uint32_t(int32_t(std::fseek(s->f, int32_t(Arg(c, 1)), int32_t(Arg(c, 2)))));
}

HLE("msvcr90.dll", ftell, kCdecl) {
  Stream* s = Find(Arg(c, 0));
  if (!s) return uint32_t(-1);
  return uint32_t(int32_t(std::ftell(s->f)));
}

HLE("msvcr90.dll", printf, kCdecl) {
  std::string s = Format(GStr(Arg(c, 0)), VaList{c.esp + 8});
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
  return uint32_t(s.size());
}

HLE("msvcr90.dll", sprintf, kCdecl) {
  gaddr buf = Arg(c, 0);
  std::string s = Format(GStr(Arg(c, 1)), VaList{c.esp + 12});
  std::memcpy(GPtr(buf), s.c_str(), s.size() + 1);
  return uint32_t(s.size());
}

// sscanf: the conversions FMOD uses (integers, floats, strings, characters).
HLE("msvcr90.dll", sscanf, kCdecl) {
  std::string in = GStr(Arg(c, 0));
  std::string fmt = GStr(Arg(c, 1));
  VaList va{c.esp + 12};
  size_t i = 0, f = 0;
  int assigned = 0;
  auto skip_ws = [&] {
    while (i < in.size() && std::isspace(uint8_t(in[i]))) i++;
  };
  while (f < fmt.size()) {
    if (std::isspace(uint8_t(fmt[f]))) {
      skip_ws();
      f++;
      continue;
    }
    if (fmt[f] != '%') {
      if (i >= in.size() || in[i] != fmt[f]) return assigned ? uint32_t(assigned) : uint32_t(-1);
      i++;
      f++;
      continue;
    }
    f++;
    if (f < fmt.size() && fmt[f] == '%') {
      if (i >= in.size() || in[i] != '%') return assigned ? uint32_t(assigned) : uint32_t(-1);
      i++;
      f++;
      continue;
    }
    bool suppress = false;
    if (f < fmt.size() && fmt[f] == '*') {
      suppress = true;
      f++;
    }
    int width = 0;
    while (f < fmt.size() && std::isdigit(uint8_t(fmt[f]))) width = width * 10 + (fmt[f++] - '0');
    int bits = 32;
    while (f < fmt.size() && (fmt[f] == 'h' || fmt[f] == 'l' || fmt[f] == 'L' || fmt[f] == 'I')) {
      if (fmt[f] == 'h') {
        bits = 16;
        f++;
      } else if (fmt[f] == 'l') {
        bits = 64;
        f++;
        if (f < fmt.size() && fmt[f] == 'l') f++;
      } else if (fmt[f] == 'L') {
        bits = 64;
        f++;
      } else {
        if (fmt.compare(f, 3, "I64") == 0) {
          bits = 64;
          f += 3;
        } else {
          f++;
        }
      }
    }
    if (f >= fmt.size()) break;
    char conv = fmt[f++];
    if (conv != 'c' && conv != '[') skip_ws();
    if (i >= in.size()) break;
    gaddr out = suppress ? 0 : va.Next();
    switch (conv) {
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
        size_t start = i;
        size_t lim = width ? std::min(in.size(), i + size_t(width)) : in.size();
        if (i < lim && (in[i] == '-' || in[i] == '+')) i++;
        int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : 10;
        size_t digits = i;
        while (i < lim && (std::isxdigit(uint8_t(in[i])) || in[i] == 'x' || in[i] == 'X')) i++;
        if (i == digits) {
          i = start;
          return assigned ? uint32_t(assigned) : uint32_t(-1);
        }
        std::string tok = in.substr(start, i - start);
        long long v = std::strtoll(tok.c_str(), nullptr, base);
        if (!suppress) {
          if (bits == 64)
            Wr64(out, uint64_t(v));
          else if (bits == 16)
            Wr16(out, uint16_t(v));
          else
            Wr32(out, uint32_t(v));
        }
        assigned++;
        break;
      }
      case 'f': case 'e': case 'g': case 'F': case 'E': case 'G': {
        size_t start = i;
        size_t lim = width ? std::min(in.size(), i + size_t(width)) : in.size();
        while (i < lim && (std::isdigit(uint8_t(in[i])) || std::strchr("+-.eE", in[i]))) i++;
        if (i == start) return assigned ? uint32_t(assigned) : uint32_t(-1);
        std::string tok = in.substr(start, i - start);
        double d = std::strtod(tok.c_str(), nullptr);
        if (!suppress) {
          if (bits == 64) {
            uint64_t b;
            std::memcpy(&b, &d, 8);
            Wr64(out, b);
          } else {
            float fl = float(d);
            uint32_t b;
            std::memcpy(&b, &fl, 4);
            Wr32(out, b);
          }
        }
        assigned++;
        break;
      }
      case 's': {
        size_t start = i;
        size_t lim = width ? std::min(in.size(), i + size_t(width)) : in.size();
        while (i < lim && !std::isspace(uint8_t(in[i]))) i++;
        if (i == start) return assigned ? uint32_t(assigned) : uint32_t(-1);
        if (!suppress) {
          std::string tok = in.substr(start, i - start);
          std::memcpy(GPtr(out), tok.c_str(), tok.size() + 1);
        }
        assigned++;
        break;
      }
      case 'c': {
        uint32_t n = width ? uint32_t(width) : 1;
        if (i + n > in.size()) return assigned ? uint32_t(assigned) : uint32_t(-1);
        if (!suppress) std::memcpy(GPtr(out), in.data() + i, n);
        i += n;
        assigned++;
        break;
      }
      default:
        return assigned ? uint32_t(assigned) : uint32_t(-1);
    }
  }
  return uint32_t(assigned);
}

// ---- files and directories -------------------------------------------------

// _stat64i32(path, struct _stat64i32*)
HLE_NAMED("msvcr90.dll", "_stat64i32", _stat64i32, kCdecl) {
  std::string host = HostPath(GStr(Arg(c, 0)));
  gaddr out = Arg(c, 1);
  struct stat st;
  if (::stat(host.c_str(), &st) != 0) {
    SetErrno(ENOENT);
    return uint32_t(-1);
  }
  if (out) {
    std::memset(GPtr(out), 0, 56);
    Wr32(out + 0, uint32_t(st.st_dev));
    Wr16(out + 4, uint16_t(st.st_ino));
    uint16_t mode = S_ISDIR(st.st_mode) ? 0x4000 : 0x8000;
    if (st.st_mode & S_IREAD) mode |= 0x100;
    if (st.st_mode & S_IWRITE) mode |= 0x80;
    Wr16(out + 6, mode);
    Wr16(out + 8, 1);
    Wr16(out + 10, 0);
    Wr16(out + 12, 0);
    Wr32(out + 16, 0);
    Wr64(out + 24, uint64_t(st.st_size));
    Wr64(out + 32, uint64_t(st.st_atime));
    Wr64(out + 40, uint64_t(st.st_mtime));
    Wr64(out + 48, uint64_t(st.st_ctime));
  }
  return 0;
}

// _findfirst64i32(pattern, struct _finddata64i32_t*): the first match's
// handle, or -1.
HLE_NAMED("msvcr90.dll", "_findfirst64i32", _findfirst64i32, kCdecl) {
  std::string pattern = GStr(Arg(c, 0));
  gaddr data = Arg(c, 1);
  std::string host = HostPath(pattern);
  glob_t g{};
  if (::glob(host.c_str(), 0, nullptr, &g) != 0 || g.gl_pathc == 0) {
    SetErrno(ENOENT);
    if (g.gl_pathc) ::globfree(&g);
    return uint32_t(-1);
  }
  FindState st;
  for (size_t k = 0; k < g.gl_pathc; k++) st.names.push_back(g.gl_pathv[k]);
  ::globfree(&g);
  std::string first = st.names[0];
  st.next = 1;
  std::lock_guard<std::mutex> l(g_find_mu);
  uint32_t h = g_next_find++;
  g_finds[h] = std::move(st);
  std::string name = first.substr(first.find_last_of('/') + 1);
  WriteFindData(data, first, name);
  return h;
}

HLE_NAMED("msvcr90.dll", "_findnext64i32", _findnext64i32, kCdecl) {
  uint32_t h = uint32_t(Arg(c, 0));
  gaddr data = Arg(c, 1);
  std::lock_guard<std::mutex> l(g_find_mu);
  auto it = g_finds.find(h);
  if (it == g_finds.end() || it->second.next >= it->second.names.size()) {
    SetErrno(ENOENT);
    return uint32_t(-1);
  }
  std::string p = it->second.names[it->second.next++];
  WriteFindData(data, p, p.substr(p.find_last_of('/') + 1));
  return 0;
}

HLE("msvcr90.dll", _mkdir, kCdecl) {
  std::string host = HostPath(GStr(Arg(c, 0)), PathAccess::kWrite);
  if (::mkdir(host.c_str(), 0755) != 0) {
    SetErrno(errno);
    return uint32_t(-1);
  }
  return 0;
}

// _fullpath(buffer, path, maxlen): the path made absolute, ".." resolved.
HLE("msvcr90.dll", _fullpath, kCdecl) {
  gaddr buf = Arg(c, 0);
  std::string path = NormaliseGuest(GStr(Arg(c, 1)));
  uint32_t maxlen = uint32_t(Arg(c, 2));
  if (maxlen && path.size() + 1 > maxlen) {
    SetErrno(ERANGE);
    return 0;
  }
  if (!buf) return 0;
  PutStr(buf, path);
  return buf;
}

// ---- wide-character helpers -----------------------------------------------

HLE("msvcr90.dll", wcstombs, kCdecl) {
  gaddr dst = Arg(c, 0);
  std::u16string w = GWStr(Arg(c, 1));
  uint32_t n = uint32_t(Arg(c, 2));
  std::string s = win32::Narrow(w);
  if (!dst) return uint32_t(s.size());
  uint32_t k = uint32_t(std::min<size_t>(s.size(), n));
  if (k) std::memcpy(GPtr(dst), s.data(), k);
  return k;
}

HLE("msvcr90.dll", wcstoul, kCdecl) {
  gaddr src = Arg(c, 0);
  gaddr endp = Arg(c, 1);
  int base = int32_t(Arg(c, 2));
  std::u16string w = GWStr(src);
  std::string s = win32::Narrow(w);
  char* end = nullptr;
  unsigned long v = std::strtoul(s.c_str(), &end, base ? base : 10);
  if (endp) {
    // The end pointer the caller sees is a WCHAR offset into src.
    uint32_t off = uint32_t(end ? end - s.c_str() : 0);
    Wr32(endp, src + 2 * off);
  }
  return uint32_t(v);
}

}  // namespace recomp::hle
