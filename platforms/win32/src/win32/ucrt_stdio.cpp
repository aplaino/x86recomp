// The UCRT's stdio for x64 guests: FILE streams on host files and the
// printf/scanf families. A FILE* is a guest pointer to a small opaque block
// (the UCRT's FILE is opaque too); the host stream behind it is kept here.
// Text mode is Windows': reads drop the \r of \r\n and stop at ^Z, writes
// turn \n into \r\n. Formatting follows the UCRT (Visual Studio 2015 and
// later), not the host's printf where they differ: %p is 16 uppercase hex
// digits, NaNs print as nan, nan(ind) or nan(snan).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/ucrt.h"
#include "win32/win32.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

struct Stream {
  FILE* f = nullptr;
  bool text = false;
  bool owned = true;  // fclose closes it (not stdin/out/err)
};

std::recursive_mutex g_streams_mu;
std::map<gaddr, Stream> g_streams;  // by guest FILE*

gaddr NewStream(FILE* f, bool text, bool owned) {
  gaddr p = HeapAllocProcess(16, true);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  g_streams[p] = {f, text, owned};
  return p;
}

// The three standard streams (__acrt_iob_func(0..2)).
gaddr StdStream(uint32_t i) {
  static gaddr s[3] = {NewStream(stdin, true, false), NewStream(stdout, true, false),
                       NewStream(stderr, true, false)};
  return i < 3 ? s[i] : 0;
}

Stream* Find(gaddr fp) {
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  auto it = g_streams.find(fp);
  return it == g_streams.end() ? nullptr : &it->second;
}

Stream& Must(gaddr fp, const char* api) {
  Stream* s = Find(fp);
  if (!s) FatalF("%s on an unknown FILE* %08llX", api, (unsigned long long)fp);
  return *s;
}

int GetC(Stream& s) {
  int ch = std::fgetc(s.f);
  if (!s.text) return ch;
  if (ch == 0x1A) {  // ^Z ends a text stream
    std::ungetc(ch, s.f);
    return EOF;
  }
  if (ch == '\r') {
    int nx = std::fgetc(s.f);
    if (nx == '\n') return '\n';
    if (nx != EOF) std::ungetc(nx, s.f);
  }
  return ch;
}

size_t Write(Stream& s, const char* p, size_t n) {
  if (!s.text) return std::fwrite(p, 1, n, s.f);
  size_t done = 0;
  for (; done < n; done++) {
    // The host's own stdout and stderr keep plain newlines.
    if (p[done] == '\n' && s.owned && std::fputc('\r', s.f) == EOF) break;
    if (std::fputc(p[done], s.f) == EOF) break;
  }
  return done;
}

// ---- printf ----------------------------------------------------------------

// Successive variadic arguments: 8-byte slots of a guest va_list (floating
// point ones as doubles).
struct Args {
  gaddr p;
  uint64_t Next() {
    uint64_t v = Rd64(p);
    p += 8;
    return v;
  }
  double NextD() {
    uint64_t b = Next();
    double d;
    std::memcpy(&d, &b, 8);
    return d;
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
    s += "nan(ind)";  // the default NaN (0/0): sign set, empty payload
  else
    s += "nan";
  if (upper)
    for (char& ch : s) ch = char(std::toupper(uint8_t(ch)));
  return s;
}

// Pads `body` to `width` (left-justified with '-'), as the host does.
std::string Pad(const std::string& body, int width, bool left) {
  if (int(body.size()) >= width) return body;
  return left ? body + std::string(width - body.size(), ' ')
              : std::string(width - body.size(), ' ') + body;
}

// The UCRT's printf (narrow functions: %s and %c are char, %S and %C
// wchar_t) over a guest format and va_list. Output the int result cannot
// count (more than INT_MAX characters) is fatal.
// wide_fn: a wide function (swprintf...), where %s and %c are wchar_t and
// %S and %C char.
std::string Format(const std::string& fmt, Args a, bool three_digit_exponents,
                   bool wide_fn = false) {
  std::string out;
  auto too_long = [&](int64_t n) {
    if (n > INT32_MAX - int64_t(out.size()))
      FatalF("guest printf: more than INT_MAX characters from \"%s\"", fmt.c_str());
  };
  // One conversion through the host's printf, whatever its length.
  auto host = [&](const std::string& f, auto v) {
    const int n = std::snprintf(nullptr, 0, f.c_str(), v);
    if (n < 0) too_long(int64_t(INT32_MAX) + 1);
    too_long(n);
    std::string r(size_t(n), '\0');
    std::snprintf(r.data(), size_t(n) + 1, f.c_str(), v);
    return r;
  };
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    size_t start = i++;
    std::string flags;
    while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) flags.push_back(fmt[i++]);
    // Width and precision: a literal one past INT_MAX is an invalid
    // format; a negative `*` width left-justifies, except INT_MIN, whose
    // negation stays negative in the UCRT and pads nothing; a negative `*`
    // precision is none.
    int64_t width = -1, prec = -1;
    auto number = [&](int64_t* v) {
      *v = 0;
      while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) {
        *v = *v * 10 + (fmt[i++] - '0');
        if (*v > INT32_MAX) FatalF("guest printf: a width or precision past INT_MAX in \"%s\"",
                                   fmt.c_str());
      }
    };
    if (i < fmt.size() && fmt[i] == '*') {
      width = int32_t(a.Next());
      if (width < 0) {
        flags.push_back('-');
        width = width == INT32_MIN ? -1 : -width;
      }
      i++;
    } else if (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) {
      number(&width);
    }
    if (i < fmt.size() && fmt[i] == '.') {
      i++;
      if (i < fmt.size() && fmt[i] == '*') {
        prec = int32_t(a.Next());
        i++;
      } else {
        number(&prec);
      }
    }
    // Length: hh h l ll L j z t I I32 I64 w.
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
          wide = true;  // long is 32-bit; %ls/%lc are wide
          i++;
        }
      } else if (ch == 'L' || ch == 'j') {
        bits = 64;
        i++;
      } else if (ch == 'z' || ch == 't') {
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
          bits = 64;  // size_t
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
    switch (conv) {
      case '%':
        out.push_back('%');
        break;
      case 'd': case 'i': {
        uint64_t v = a.Next();
        long long s = bits == 64 ? int64_t(v) : bits == 16 ? int16_t(v) : bits == 8 ? int8_t(v)
                                                                             : int32_t(v);
        out += host(spec + "lld", s);
        break;
      }
      case 'u': case 'o': case 'x': case 'X': {
        uint64_t v = a.Next();
        unsigned long long u = bits == 64 ? v : bits == 16 ? uint16_t(v) : bits == 8 ? uint8_t(v)
                                                                           : uint32_t(v);
        out += host(spec + "ll" + conv, u);
        break;
      }
      case 'p': {
        too_long(width);
        out += Pad(host("%016llX", (unsigned long long)a.Next()), int(width),
                   flags.find('-') != std::string::npos);
        break;
      }
      case 'c': case 'C': {
        uint64_t v = a.Next();
        bool w = (conv == 'C') != wide_fn ? !narrow : wide;
        char ch = w ? win32::Narrow(std::u16string(1, char16_t(v)))[0] : char(v);
        too_long(width);
        out += Pad(std::string(1, ch), int(width), flags.find('-') != std::string::npos);
        break;
      }
      case 's': case 'S': {
        // With a precision, no more of the string is read than it keeps
        // (it need not be terminated).
        gaddr p = a.Next();
        bool w = (conv == 'S') != wide_fn ? !narrow : wide;
        std::string s;
        if (!p) {
          s = "(null)";
        } else if (prec < 0) {
          s = w ? win32::Narrow(GWStr(p)) : GStr(p);
        } else if (w) {
          std::u16string u;
          for (gaddr q = p; int64_t(u.size()) < prec && Rd16(q); q += 2) u.push_back(Rd16(q));
          s = win32::Narrow(u);
        } else {
          for (gaddr q = p; int64_t(s.size()) < prec && Rd8(q); q++) s.push_back(char(Rd8(q)));
        }
        if (prec >= 0 && size_t(prec) < s.size()) s.resize(size_t(prec));
        too_long(std::max<int64_t>(width, int64_t(s.size())));
        out += Pad(s, int(width), flags.find('-') != std::string::npos);
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        double v = a.NextD();
        bool upper = std::isupper(uint8_t(conv));
        if (std::isnan(v) || std::isinf(v)) {
          std::string s = std::isnan(v) ? NanText(v, upper)
                                        : std::string(std::signbit(v) ? "-" : "") +
                                              (upper ? "INF" : "inf");
          if (!std::signbit(v) && flags.find('+') != std::string::npos) s = "+" + s;
          else if (!std::signbit(v) && flags.find(' ') != std::string::npos) s = " " + s;
          too_long(width);
          out += Pad(s, int(width), flags.find('-') != std::string::npos);
          break;
        }
        if (conv == 'a' || conv == 'A')
          FatalF("guest printf: %%%c is not supported yet", conv);
        std::string s = host(spec + conv, v);
        if (three_digit_exponents) {  // the legacy e+005: three exponent digits
          size_t e = s.find_first_of("eE");
          if (e != std::string::npos && s.size() - e == 4) s.insert(e + 2, "0");
        }
        out += s;
        break;
      }
      case 'n':
        FatalF("guest printf: %%n (disabled by default in the UCRT)");
      default:
        // An invalid conversion: the UCRT's invalid parameter handler.
        FatalF("guest printf: invalid conversion %%%c in \"%s\"", conv, fmt.c_str());
    }
  }
  return out;
}

// __stdio_common_v*printf options.
constexpr uint64_t kStandardSnprintf = 2, kThreeDigitExponents = 16;

// Writes s into the guest buffer the way the option set asks for; returns
// the function's result.
uint64_t PutFormatted(uint64_t options, gaddr buf, uint64_t count, const std::string& s) {
  const uint64_t n = s.size();
  if (!buf) return uint32_t(int32_t(n));
  if (options & kStandardSnprintf) {  // snprintf: truncate, always NUL
    if (count) {
      uint64_t k = std::min(n, count - 1);
      std::memcpy(GPtr(buf), s.data(), k);
      Wr8(buf + k, 0);
    }
    return uint32_t(int32_t(n));
  }
  // _vsnprintf: -1 if it does not fit; NUL only when there is room.
  uint64_t k = std::min(n, count);
  std::memcpy(GPtr(buf), s.data(), k);
  if (n < count) Wr8(buf + n, 0);
  return n <= count ? uint32_t(int32_t(n)) : uint32_t(-1);
}

}  // namespace

HLE64("ucrtbase.dll", __acrt_iob_func, kCdecl) { return StdStream(uint32_t(Arg(c, 0))); }

// __stdio_common_vsprintf(options, buffer, count, format, locale, va_list)
HLE64("ucrtbase.dll", __stdio_common_vsprintf, kCdecl) {
  uint64_t opt = Arg(c, 0);
  std::string s = Format(GStr(Arg(c, 3)), Args{Arg(c, 5)}, opt & kThreeDigitExponents);
  return PutFormatted(opt, Arg(c, 1), Arg(c, 2), s);
}

// __stdio_common_vsprintf_s: a buffer too small is an invalid parameter.
HLE64("ucrtbase.dll", __stdio_common_vsprintf_s, kCdecl) {
  uint64_t opt = Arg(c, 0);
  gaddr buf = Arg(c, 1);
  uint64_t count = Arg(c, 2);
  std::string s = Format(GStr(Arg(c, 3)), Args{Arg(c, 5)}, opt & kThreeDigitExponents);
  if (!buf || s.size() + 1 > count)
    FatalF("guest sprintf_s: %zu characters into a buffer of %llu", s.size(),
           (unsigned long long)count);
  std::memcpy(GPtr(buf), s.c_str(), s.size() + 1);
  return uint32_t(s.size());
}

// __stdio_common_vsnprintf_s(options, buffer, size, max_count, format,
// locale, va_list): max_count _TRUNCATE ((size_t)-1) truncates to the
// buffer and returns -1 when it did.
HLE64("ucrtbase.dll", __stdio_common_vsnprintf_s, kCdecl) {
  uint64_t opt = Arg(c, 0);
  gaddr buf = Arg(c, 1);
  uint64_t size = Arg(c, 2), max = Arg(c, 3);
  std::string s = Format(GStr(Arg(c, 4)), Args{Arg(c, 6)}, opt & kThreeDigitExponents);
  if (!buf || !size) FatalF("guest vsnprintf_s: no buffer");
  uint64_t limit = std::min(max == ~0ull ? size - 1 : max, size - 1);
  bool truncated = s.size() > limit;
  if (truncated && max != ~0ull && max >= size)
    FatalF("guest vsnprintf_s: %zu characters into a buffer of %llu", s.size(),
           (unsigned long long)size);
  uint64_t k = std::min<uint64_t>(s.size(), limit);
  std::memcpy(GPtr(buf), s.data(), k);
  Wr8(buf + k, 0);
  return truncated ? uint32_t(-1) : uint32_t(k);
}

// __stdio_common_vfprintf(options, stream, format, locale, va_list)
HLE64("ucrtbase.dll", __stdio_common_vfprintf, kCdecl) {
  uint64_t opt = Arg(c, 0);
  Stream& st = Must(Arg(c, 1), "fprintf");
  std::string s = Format(GStr(Arg(c, 2)), Args{Arg(c, 4)}, opt & kThreeDigitExponents);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(st, s.data(), s.size()) == s.size() ? uint32_t(s.size()) : uint32_t(-1);
}

// ---- streams --------------------------------------------------------------------

namespace {
// fopen's mode: r w a, +, b/t; anything else is the UCRT's invalid
// parameter. The host file, or null with the guest's errno set.
FILE* OpenHost(const std::string& path, const std::string& mode, bool* text) {
  if (mode.empty() || !std::strchr("rwa", mode[0]))
    FatalF("guest fopen(\"%s\", \"%s\"): invalid mode", path.c_str(), mode.c_str());
  bool plus = mode.find('+') != std::string::npos;
  *text = mode.find('b') == std::string::npos;  // text unless "b" (_fmode)
  PathAccess acc = mode[0] == 'r' ? (plus ? PathAccess::kUpdate : PathAccess::kRead)
                                  : PathAccess::kWrite;
  if (mode[0] == 'a') acc = PathAccess::kUpdate;
  std::string host = HostPath(path, acc);
  std::string m(1, mode[0]);
  if (plus) m += '+';
  m += 'b';
  FILE* f = std::fopen(host.c_str(), m.c_str());
  RECOMP_TRACE("  fopen(%s, %s) -> %s%s\n", path.c_str(), mode.c_str(), host.c_str(),
               f ? "" : " (failed)");
  if (!f)
    Wr32(GuestErrno(), uint32_t(errno == ENOENT ? ENOENT : errno == EACCES ? EACCES : EINVAL));
  return f;
}

gaddr Open(const std::string& path, const std::string& mode) {
  bool text;
  FILE* f = OpenHost(path, mode, &text);
  return f ? NewStream(f, text, true) : 0;
}

// freopen: the stream is closed, then the same FILE* opens `path`; on
// failure it stays closed and the result is null.
gaddr Reopen(const std::string& path, const std::string& mode, gaddr fp) {
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  Stream& s = Must(fp, "freopen");
  if (s.owned) std::fclose(s.f);
  else std::fflush(s.f);
  bool text;
  FILE* f = OpenHost(path, mode, &text);
  if (!f) {
    g_streams.erase(fp);
    return 0;
  }
  s = {f, text, true};
  return fp;
}
}  // namespace

HLE64("ucrtbase.dll", fopen, kCdecl) { return Open(GStr(Arg(c, 0)), GStr(Arg(c, 1))); }
HLE64("ucrtbase.dll", _wfopen, kCdecl) {
  return Open(NarrowPath(GWStr(Arg(c, 0))), win32::Narrow(GWStr(Arg(c, 1))));
}
// _wfreopen(path, mode, stream); a null path (changing the mode of the open
// file) is not supported.
HLE64("ucrtbase.dll", _wfreopen, kCdecl) {
  if (!Arg(c, 0)) FatalF("_wfreopen with a null path (mode change) is not supported");
  return Reopen(NarrowPath(GWStr(Arg(c, 0))), win32::Narrow(GWStr(Arg(c, 1))), Arg(c, 2));
}
// fopen_s(&file, path, mode): 0 or the errno.
HLE64("ucrtbase.dll", fopen_s, kCdecl) {
  gaddr f = Open(GStr(Arg(c, 1)), GStr(Arg(c, 2)));
  Wr64(Arg(c, 0), f);
  return f ? 0 : Rd32(GuestErrno());
}

HLE64("ucrtbase.dll", fclose, kCdecl) {
  gaddr fp = Arg(c, 0);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  auto it = g_streams.find(fp);
  if (it == g_streams.end()) return uint32_t(EOF);
  int r = it->second.owned ? std::fclose(it->second.f) : std::fflush(it->second.f);
  if (it->second.owned) {
    g_streams.erase(it);
    HeapFreeProcess(fp);
  }
  return uint32_t(r);
}

HLE64("ucrtbase.dll", fread, kCdecl) {
  gaddr buf = Arg(c, 0);
  uint64_t size = Arg(c, 1), n = Arg(c, 2);
  Stream& s = Must(Arg(c, 3), "fread");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (!size || !n) return 0;
  if (!s.text) return std::fread(GPtr(buf), size_t(size), size_t(n), s.f);
  uint64_t total = size * n, got = 0;
  for (; got < total; got++) {
    int ch = GetC(s);
    if (ch == EOF) break;
    Wr8(buf + got, uint8_t(ch));
  }
  return got / size;
}

HLE64("ucrtbase.dll", fwrite, kCdecl) {
  gaddr buf = Arg(c, 0);
  uint64_t size = Arg(c, 1), n = Arg(c, 2);
  Stream& s = Must(Arg(c, 3), "fwrite");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (!size || !n) return 0;
  return Write(s, g_proc->mem.ptr<const char>(buf), size * n) / size;
}

HLE64("ucrtbase.dll", fgets, kCdecl) {
  gaddr buf = Arg(c, 0);
  int n = int32_t(Arg(c, 1));
  Stream& s = Must(Arg(c, 2), "fgets");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (n <= 0) return 0;
  int i = 0;
  while (i < n - 1) {
    int ch = GetC(s);
    if (ch == EOF) break;
    Wr8(buf + i++, uint8_t(ch));
    if (ch == '\n') break;
  }
  if (!i) return 0;
  Wr8(buf + i, 0);
  return buf;
}

HLE64("ucrtbase.dll", fputs, kCdecl) {
  std::string str = GStr(Arg(c, 0));
  Stream& s = Must(Arg(c, 1), "fputs");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(s, str.data(), str.size()) == str.size() ? 0 : uint32_t(EOF);
}

HLE64("ucrtbase.dll", fputc, kCdecl) {
  char ch = char(Arg(c, 0));
  Stream& s = Must(Arg(c, 1), "fputc");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(s, &ch, 1) == 1 ? uint8_t(ch) : uint32_t(EOF);
}

HLE64("ucrtbase.dll", fflush, kCdecl) {
  gaddr fp = Arg(c, 0);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (!fp) return uint32_t(std::fflush(nullptr));
  Stream& s = Must(fp, "fflush");
  return uint32_t(std::fflush(s.f));
}

HLE64("ucrtbase.dll", ferror, kCdecl) { return std::ferror(Must(Arg(c, 0), "ferror").f) != 0; }

HLE64("ucrtbase.dll", fseek, kCdecl) {
  Stream& s = Must(Arg(c, 0), "fseek");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(std::fseek(s.f, long(int32_t(Arg(c, 1))), int(Arg(c, 2))));
}
HLE64("ucrtbase.dll", _fseeki64, kCdecl) {
  Stream& s = Must(Arg(c, 0), "_fseeki64");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(fseeko(s.f, off_t(int64_t(Arg(c, 1))), int(Arg(c, 2))));
}
HLE64("ucrtbase.dll", ftell, kCdecl) {
  Stream& s = Must(Arg(c, 0), "ftell");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  long p = std::ftell(s.f);
  return uint32_t(int32_t(p > INT32_MAX ? -1 : p));  // long is 32-bit
}
HLE64("ucrtbase.dll", rewind, kCdecl) {
  Stream& s = Must(Arg(c, 0), "rewind");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  std::rewind(s.f);
  return 0;
}


// __stdio_common_vswprintf(options, buffer, count, format, locale, va_list):
// the narrow formatter over the format as Windows-1252, the result widened.
HLE64("ucrtbase.dll", __stdio_common_vswprintf, kCdecl) {
  uint64_t opt = Arg(c, 0);
  std::string s = Format(win32::Narrow(GWStr(Arg(c, 3))), Args{Arg(c, 5)},
                         opt & kThreeDigitExponents, true);
  std::u16string w = win32::Widen(s);
  gaddr buf = Arg(c, 1);
  uint64_t count = Arg(c, 2), n = w.size();
  if (!buf) return uint32_t(int32_t(n));
  auto put = [&](uint64_t k) {
    for (uint64_t i = 0; i < k; i++) Wr16(buf + 2 * i, w[i]);
  };
  if (opt & kStandardSnprintf) {
    if (count) {
      uint64_t k = std::min(n, count - 1);
      put(k);
      Wr16(buf + 2 * k, 0);
    }
    return uint32_t(int32_t(n));
  }
  uint64_t k = std::min(n, count);
  put(k);
  if (n < count) Wr16(buf + 2 * n, 0);
  return n <= count ? uint32_t(int32_t(n)) : uint32_t(-1);
}

// ---- scanf ---------------------------------------------------------------------

namespace {
// The UCRT's scanf over the text `in`: each conversion is matched by the
// host's sscanf (one conversion and %n at a time), and its result written
// to the guest pointer in the next va_list slot. Wide functions read their
// strings as Windows-1252 and store %s and %c as wchar_t. Returns the count
// of assignments, or EOF for input that ends before the first conversion.
int Scan(const std::string& in, const std::string& fmt, Args a, bool wide_fn) {
  size_t at = 0;
  int assigned = 0;
  bool any_conversion = false;
  for (size_t i = 0; i < fmt.size(); i++) {
    char f = fmt[i];
    if (std::isspace(uint8_t(f))) {
      while (at < in.size() && std::isspace(uint8_t(in[at]))) at++;
      continue;
    }
    if (f != '%' || (i + 1 < fmt.size() && fmt[i + 1] == '%')) {
      if (f == '%') i++;
      if (at >= in.size()) return any_conversion ? assigned : EOF;
      if (in[at] != fmt[i]) return assigned;
      at++;
      continue;
    }
    // %[*][width][length]conversion
    size_t j = i + 1;
    bool suppress = j < fmt.size() && fmt[j] == '*';
    if (suppress) j++;
    std::string width;
    while (j < fmt.size() && std::isdigit(uint8_t(fmt[j]))) width.push_back(fmt[j++]);
    int bits = 32;
    bool lng = false, shrt = false;
    for (;;) {
      if (fmt.compare(j, 3, "I64") == 0) { bits = 64; j += 3; }
      else if (fmt.compare(j, 3, "I32") == 0) { bits = 32; j += 3; }
      else if (fmt.compare(j, 2, "ll") == 0) { bits = 64; j += 2; }
      else if (fmt.compare(j, 2, "hh") == 0) { bits = 8; j += 2; }
      else if (j < fmt.size() && fmt[j] == 'h') { bits = 16; shrt = true; j++; }
      else if (j < fmt.size() && (fmt[j] == 'l' || fmt[j] == 'w')) { lng = true; j++; }
      else if (j < fmt.size() && (fmt[j] == 'L' || fmt[j] == 'z' || fmt[j] == 'j' || fmt[j] == 't' || fmt[j] == 'I')) {
        if (fmt[j] != 'L') bits = 64;
        j++;
      }
      else break;
    }
    if (j >= fmt.size()) break;
    char conv = fmt[j];
    std::string set;
    if (conv == '[') {
      size_t k = j + 1;
      if (k < fmt.size() && fmt[k] == '^') k++;
      if (k < fmt.size() && fmt[k] == ']') k++;
      while (k < fmt.size() && fmt[k] != ']') k++;
      set = fmt.substr(j, k + 1 - j);
      j = k;
    }
    i = j;
    if (conv == 'n') {
      if (!suppress) Wr32(a.Next(), uint32_t(at));
      continue;
    }
    any_conversion = true;
    if (conv != 'c' && conv != '[')
      while (at < in.size() && std::isspace(uint8_t(in[at]))) at++;
    if (at >= in.size()) return assigned ? assigned : EOF;
    const char* src = in.c_str() + at;
    int used = 0;
    bool ok = false;
    const std::string w = width;
    switch (conv) {
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
        long long v = 0;
        unsigned long long u = 0;
        const bool sgn = conv == 'd' || conv == 'i';
        std::string hf = "%" + w + (sgn ? "lld" : conv == 'u' ? "llu" : conv == 'o' ? "llo" : "llx") + "%n";
        if (conv == 'i') hf = "%" + w + "lli%n";
        ok = (sgn ? std::sscanf(src, hf.c_str(), &v, &used) : std::sscanf(src, hf.c_str(), &u, &used)) == 1;
        if (ok && !suppress) {
          uint64_t val = sgn ? uint64_t(v) : u;
          gaddr out = a.Next();
          if (conv == 'p' || bits == 64) Wr64(out, val);
          else if (bits == 16) Wr16(out, uint16_t(val));
          else if (bits == 8) Wr8(out, uint8_t(val));
          else Wr32(out, uint32_t(val));
        }
        break;
      }
      case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': case 'A': {
        double v = 0;
        ok = std::sscanf(src, ("%" + w + "lf%n").c_str(), &v, &used) == 1;
        if (ok && !suppress) {
          gaddr out = a.Next();
          if (lng) {
            uint64_t b;
            std::memcpy(&b, &v, 8);
            Wr64(out, b);
          } else {
            float fv = float(v);
            uint32_t b;
            std::memcpy(&b, &fv, 4);
            Wr32(out, b);
          }
        }
        break;
      }
      case 's': case 'c': case '[': case 'S': case 'C': {
        std::string buf(in.size() - at + 1, '\0');
        std::string hf = "%" + (w.empty() && (conv == 'c' || conv == 'C') ? std::string("1") : w) +
                         (conv == '[' ? set : std::string(1, char(std::tolower(uint8_t(conv))))) + "%n";
        if (conv == 'c' || conv == 'C') {
          size_t n = w.empty() ? 1 : size_t(std::stoul(w));
          ok = at + n <= in.size();
          if (ok) {
            buf = in.substr(at, n);
            used = int(n);
          }
        } else {
          ok = std::sscanf(src, hf.c_str(), buf.data(), &used) == 1;
          buf.resize(std::strlen(buf.c_str()));
        }
        if (ok && !suppress) {
          const bool wide = (conv == 'S' || conv == 'C') != wide_fn ? !shrt : lng;
          gaddr out = a.Next();
          const bool term = conv != 'c' && conv != 'C';
          if (wide) {
            std::u16string u = win32::Widen(buf);
            for (size_t k = 0; k < u.size(); k++) Wr16(out + 2 * k, u[k]);
            if (term) Wr16(out + 2 * u.size(), 0);
          } else {
            std::memcpy(GPtr(out), buf.data(), buf.size());
            if (term) Wr8(out + buf.size(), 0);
          }
        }
        break;
      }
      default:
        FatalF("guest scanf: conversion %%%c not supported (\"%s\")", conv, fmt.c_str());
    }
    if (!ok) return assigned;
    at += size_t(used);
    if (!suppress) assigned++;
  }
  return assigned;
}
}  // namespace

// __stdio_common_vsscanf(options, buffer, count, format, locale, va_list)
HLE64("ucrtbase.dll", __stdio_common_vsscanf, kCdecl) {
  gaddr buf = Arg(c, 1);
  uint64_t count = Arg(c, 2);
  std::string in;
  for (gaddr p = buf; (count == ~uint64_t(0) || p - buf < count) && Rd8(p); p++)
    in.push_back(char(Rd8(p)));
  return uint32_t(Scan(in, GStr(Arg(c, 3)), Args{Arg(c, 5)}, false));
}
HLE64("ucrtbase.dll", __stdio_common_vswscanf, kCdecl) {
  gaddr buf = Arg(c, 1);
  uint64_t count = Arg(c, 2);
  std::u16string in;
  for (gaddr p = buf; (count == ~uint64_t(0) || (p - buf) / 2 < count) && Rd16(p); p += 2)
    in.push_back(char16_t(Rd16(p)));
  return uint32_t(Scan(win32::Narrow(in), win32::Narrow(GWStr(Arg(c, 3))), Args{Arg(c, 5)}, true));
}
// __stdio_common_vfscanf(options, stream, format, locale, va_list): the
// stream's next line, scanned. Reading past one line in one call (a format
// that spans lines) is not supported.
HLE64("ucrtbase.dll", __stdio_common_vfscanf, kCdecl) {
  Stream& s = Must(Arg(c, 1), "fscanf");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  std::string line;
  for (int ch; (ch = GetC(s)) != EOF;) {
    line.push_back(char(ch));
    if (ch == '\n') break;
  }
  if (line.empty()) return uint32_t(EOF);
  return uint32_t(Scan(line, GStr(Arg(c, 2)), Args{Arg(c, 4)}, false));
}

// ---- more streams ------------------------------------------------------------

HLE64("ucrtbase.dll", feof, kCdecl) { return std::feof(Must(Arg(c, 0), "feof").f) != 0; }
uint64_t GetCImpl(gaddr fp, const char* api) {
  Stream& s = Must(fp, api);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  int ch = GetC(s);
  return ch == EOF ? uint32_t(EOF) : uint32_t(uint8_t(ch));
}
HLE64("ucrtbase.dll", fgetc, kCdecl) { return GetCImpl(Arg(c, 0), "fgetc"); }
HLE64("ucrtbase.dll", getc, kCdecl) { return GetCImpl(Arg(c, 0), "getc"); }
HLE64("ucrtbase.dll", ungetc, kCdecl) {
  Stream& s = Must(Arg(c, 1), "ungetc");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  int ch = int32_t(Arg(c, 0));
  return ch == EOF ? uint32_t(EOF) : uint32_t(std::ungetc(ch, s.f));
}
// fgetws: a text stream's bytes as Windows-1252 characters (no BOM or
// UTF-16 handling: files are opened without a ccs).
HLE64("ucrtbase.dll", fgetws, kCdecl) {
  gaddr buf = Arg(c, 0);
  int n = int32_t(Arg(c, 1));
  Stream& s = Must(Arg(c, 2), "fgetws");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (n <= 0) return 0;
  std::string line;
  while (int(line.size()) < n - 1) {
    int ch = GetC(s);
    if (ch == EOF) break;
    line.push_back(char(ch));
    if (ch == '\n') break;
  }
  if (line.empty()) return 0;
  std::u16string w = win32::Widen(line);
  for (size_t i = 0; i < w.size(); i++) Wr16(buf + 2 * i, w[i]);
  Wr16(buf + 2 * w.size(), 0);
  return buf;
}
// fpos_t is a 64-bit offset.
HLE64("ucrtbase.dll", fgetpos, kCdecl) {
  Stream& s = Must(Arg(c, 0), "fgetpos");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  off_t p = ftello(s.f);
  if (p < 0) return uint32_t(-1);
  Wr64(Arg(c, 1), uint64_t(p));
  return 0;
}
HLE64("ucrtbase.dll", fsetpos, kCdecl) {
  Stream& s = Must(Arg(c, 0), "fsetpos");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(fseeko(s.f, off_t(Rd64(Arg(c, 1))), SEEK_SET));
}
// Buffering is the host stream's own; the guest's buffer is not used.
HLE64("ucrtbase.dll", setvbuf, kCdecl) {
  Must(Arg(c, 0), "setvbuf");
  return 0;
}
// The lock is the streams' lock, held by every operation already.
HLE64("ucrtbase.dll", _lock_file, kCdecl) { return 0; }
HLE64("ucrtbase.dll", _unlock_file, kCdecl) { return 0; }
HLE64("ucrtbase.dll", tmpfile, kCdecl) {
  FILE* f = std::tmpfile();
  return f ? NewStream(f, false, true) : 0;
}
namespace {
// Unique names in C:\TEMP (the overlay's TEMP directory).
std::string TempName(const std::string& dir, const std::string& prefix) {
  static std::atomic<uint32_t> n{0};
  return dir + (dir.empty() || dir.back() == '\\' ? "" : "\\") + prefix +
         std::to_string(++n) + ".tmp";
}
}  // namespace
HLE64("ucrtbase.dll", tmpnam, kCdecl) {
  static gaddr buf = HeapAllocProcess(260, true);
  gaddr out = Arg(c, 0) ? Arg(c, 0) : buf;
  PutStr(out, TempName("C:\\TEMP", "s"));
  return out;
}
HLE64("ucrtbase.dll", _tempnam, kCdecl) {
  std::string dir = Arg(c, 0) ? GStr(Arg(c, 0)) : "C:\\TEMP";
  std::string name = TempName(dir, Arg(c, 1) ? GStr(Arg(c, 1)) : "");
  gaddr p = HeapAllocProcess(name.size() + 1, false);
  PutStr(p, name);
  return p;
}
// _get_stream_buffer_pointers(stream, &base, &ptr, &count): the guest
// cannot see the host stream's buffer; all three point at an empty one.
HLE64("ucrtbase.dll", _get_stream_buffer_pointers, kCdecl) {
  Must(Arg(c, 0), "_get_stream_buffer_pointers");
  static gaddr empty = HeapAllocProcess(16, true);
  for (int i = 1; i <= 3; i++)
    if (gaddr out = Arg(c, i)) Wr64(out, i == 3 ? empty + 8 : empty);
  return 0;
}

}  // namespace recomp::hle
