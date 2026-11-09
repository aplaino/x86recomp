// VC6's _output and _woutput (output.c: the engines of sprintf,
// _snprintf, _vsnprintf, _vsnwprintf and printf/fprintf), natively, for
// string streams; docs/msvc_crt.md.
//
// The engine is VC6's state machine over the format: its character
// classes and transitions, flags, width and precision (with `*`), the
// size prefixes h, l, w and I64, and the conversions c C d i o u x X e E
// f g G n p s S Z. A type character it does not know prints what the
// last conversion left (its text and length stay). Characters go out as
// write_char puts them: --_cnt, a byte at _ptr, and once _cnt is below
// zero _flsbuf, which for a string stream sets _IOERR and returns EOF;
// the count is then -1 and the engine stops at the next format
// character. The format pointer and the argument pointer are left in the
// argument slots as the original leaves them. _woutput's differences are
// at Output below.
//
// Floating point is VC6's own conversion (_cfltcvt, through the function
// pointers _cfltcvt_init sets): the double goes through fld/fstp (on the
// x87 model: a signalling NaN comes out quiet with IE, a denormal sets
// DE), then __dtold to 80 bits and $I10_OUTPUT, which scales by a power
// of ten with VC6's 12-byte arithmetic (__multtenpow12, __ld12mul and
// its tables), takes 18 digits and rounds them to 17; _fptostr rounds
// those to the precision and _cftoe/_cftof/_cftog lay them out ("e+000",
// "1#INF", "1#QNAN", "1#IND").
//
// The original runs, from the start (whatever the native code wrote is
// put back), for a stream that is not a string (_IOSTRG), a character to
// convert (wctomb, mbtowc) beyond what Wctomb/Mbtowc below take, '#'
// with %.0e/f/g in a multibyte code page (_isctype), any exception
// unmasked in the caller's control word when a float is converted, and
// conversions whose text would not fit the original's buffer (it
// overruns its stack). The locale lock of wctomb, mbtowc and tolower is
// not taken (as for the locale functions).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "runtime/log.h"
#include "win32/msvc_crt.h"
#include "win32/msvc_crt_vc6_ld12.h"
#include "x86/lifted.h"

namespace recomp::native::msvc_crt::vc6 {

namespace {

using lift::Rd;
using lift::Wr;

uint8_t R8(uint32_t a) { return Rd<uint8_t>(g_guest_base, a); }
uint16_t R16(uint32_t a) { return Rd<uint16_t>(g_guest_base, a); }
uint32_t R32(uint32_t a) { return Rd<uint32_t>(g_guest_base, a); }

// $I10_OUTPUT's result (FOS): the decimal exponent, the sign (' ' or
// '-'), the digit count and the digits.
struct Fos {
  int16_t exp;
  char sign;
  uint8_t len;
  char man[24];
};

// $I10_OUTPUT(ld, 17 digits, no F flag): 0 for a NaN or an infinity.
int I10Output(uint32_t lo, uint32_t hi, uint16_t se, Fos& fos) {
  const uint32_t exp = se & 0x7FFF;
  fos.sign = se & 0x8000 ? '-' : ' ';
  auto zero = [&] {
    fos.exp = 0;
    fos.sign = ' ';
    fos.len = 1;
    fos.man[0] = '0';
    fos.man[1] = 0;
    return 1;
  };
  if (exp == 0 && !hi && !lo) return zero();
  if (exp == 0x7FFF) {
    fos.exp = 1;
    const char* s;
    if (!(hi == 0x80000000 && !lo) && !(hi & 0x40000000))
      s = "1#SNAN";
    else if ((se & 0x8000) && hi == 0xC0000000)
      s = lo ? "1#QNAN" : "1#IND";
    else
      s = hi == 0x80000000 && !lo ? "1#INF" : "1#QNAN";
    std::strcpy(fos.man, s);
    fos.len = uint8_t(std::strlen(s));
    return 0;
  }
  // The decimal exponent, estimated from the binary one and the top byte.
  const uint32_t est = ((exp >> 8) + 2 * (hi >> 24)) * 0x4D + exp * 0x4D10 - 0x134312F4;
  int16_t dexp = int16_t(int32_t(est) >> 16);
  Ld12 x{};
  x.SetD(2, lo);
  x.SetD(6, hi);
  x.SetW(10, uint16_t(exp));
  MultTenPow12(x, -dexp, true);
  if (x.W(10) >= 0x3FFF) {  // still 1 or more: times 0.1
    dexp++;
    Ld12 tenth;
    std::memset(tenth.b, 0xCC, 10);
    tenth.SetW(10, 0x3FFB);
    Ld12Mul(x, tenth);
  }
  fos.exp = dexp;
  const int ndigits = 17;
  int32_t shift = int32_t(x.W(10)) - 0x3FFE;
  x.SetW(10, 0);
  for (int i = 0; i < 8; i++) Shl12(x);
  if (shift < 0)
    for (uint32_t k = uint32_t(-shift) & 0xFF; k; k--) Shr12(x);
  char* p = fos.man;
  for (int i = ndigits + 1; i > 0; i--) {  // digit by digit: x * 10, its top byte
    const Ld12 t = x;
    Shl12(x);
    Shl12(x);
    Add12(x, t);
    Shl12(x);
    *p++ = char(x.b[11] + '0');
    x.b[11] = 0;
  }
  char* q = p - 2;
  if (p[-1] >= '5') {  // the 18th digit rounds the 17
    while (q >= fos.man && *q == '9') *q-- = '0';
    if (q < fos.man) {
      q++;
      fos.exp++;
    }
    ++*q;
  } else {
    while (q >= fos.man && *q == '0') q--;
    if (q < fos.man) return zero();
  }
  fos.len = uint8_t(q - fos.man + 1);
  fos.man[fos.len] = 0;
  return 1;
}

// _fltout2's STRFLT.
struct Strflt {
  int sign, decpt, flag;
  char mantissa[24];
};

// __dtold (double to 80 bits) and $I10_OUTPUT.
Strflt Fltout2(uint64_t bits) {
  const uint16_t top = uint16_t(bits >> 48);
  const uint32_t e = (top & 0x7FF0) >> 4;
  uint32_t lo = uint32_t(bits), hi = uint32_t(bits >> 32) & 0xFFFFF;
  uint32_t lead = 0x80000000, ie;
  uint16_t se = 0;
  if (e == 0 && !hi && !lo) {
    lo = 0;
  } else {
    if (e == 0) {
      ie = 0x3C01;
      lead = 0;
    } else {
      ie = e == 0x7FF ? 0x7FFF : e + 0x3C00;
    }
    hi = lo >> 21 | hi << 11 | lead;
    lo <<= 11;
    while (!(hi & 0x80000000)) {
      hi = lo >> 31 | hi << 1;
      lo <<= 1;
      ie += 0xFFFF;
    }
    se = uint16_t((top & 0x8000) | uint16_t(ie));
  }
  Fos fos;
  Strflt f;
  f.flag = I10Output(lo, hi, se, fos);
  f.sign = int8_t(fos.sign);
  f.decpt = fos.exp;
  std::strcpy(f.mantissa, fos.man);
  return f;
}

// _shift: the string at s moved n places up.
void Shift(char* s, int n) {
  if (n) std::memmove(s + n, s, std::strlen(s) + 1);
}

// _fptostr: `digits` digits of the mantissa (zeros after it) at buf,
// rounded on the next one; a carry into a new first digit raises decpt.
void Fptostr(char* buf, int digits, Strflt& f) {
  const char* m = f.mantissa;
  char* p = buf + 1;
  *buf = '0';
  for (int n = digits; n > 0; n--) {
    char c = *m;
    if (c)
      m++;
    else
      c = '0';
    *p++ = c;
  }
  *p = 0;
  if (digits >= 0 && *m > '4') {
    while (*--p == '9') *p = '0';
    ++*p;
  }
  if (*buf == '1')
    f.decpt++;
  else
    std::memmove(buf, buf + 1, std::strlen(buf + 1) + 1);
}

struct Float {
  char dp;  // the decimal point
};

void Cftoe2(char* buf, int ndec, bool caps, const Strflt& f, bool g, char dp) {
  const bool neg = f.sign == '-';
  if (g) Shift(buf + neg, ndec > 0);
  char* p = buf;
  if (neg) *p++ = '-';
  if (ndec > 0) {
    *p = p[1];
    *++p = dp;
  }
  char* e = p + ndec + !g;
  std::strcpy(e, "e+000");
  if (caps) *e = 'E';
  if (f.mantissa[0] != '0') {
    int x = f.decpt - 1;
    if (x < 0) {
      x = -x;
      e[1] = '-';
    }
    if (x >= 100) {
      e[2] = char(e[2] + x / 100);
      x %= 100;
    }
    if (x >= 10) {
      e[3] = char(e[3] + x / 10);
      x %= 10;
    }
    e[4] = char(e[4] + x);
  }
}

void Cftof2(char* buf, int ndec, const Strflt& f, bool g, char dp) {
  const bool neg = f.sign == '-';
  if (g && f.decpt - 1 == ndec) {
    char* q = buf + neg + f.decpt - 1;
    q[0] = '0';
    q[1] = 0;
  }
  char* p = buf;
  if (neg) *p++ = '-';
  if (f.decpt <= 0) {
    Shift(p, 1);
    *p++ = '0';
  } else {
    p += f.decpt;
  }
  if (ndec > 0) {
    Shift(p, 1);
    *p = dp;
    if (f.decpt < 0) {
      if (g || ndec >= -f.decpt) ndec = -f.decpt;
      Shift(p + 1, ndec);
      std::memset(p + 1, '0', size_t(ndec));
    }
  }
}

// _cropzeros: the fraction's trailing zeros (and a bare decimal point)
// removed, before any exponent.
void Cropzeros(char* buf, char dp) {
  char* p = buf;
  while (*p && *p != dp) p++;
  if (!*p) return;
  do p++;
  while (*p && *p != 'e' && *p != 'E');
  char* q = p;
  do q--;
  while (*q == '0');
  if (*q == dp) q--;
  char c;
  do {
    c = *p++;
    *++q = c;
  } while (c);
}

// ---- the engine --------------------------------------------------------

// VC6's character classes for ' '..'x' and its transitions
// (__lookuptable): state x class -> state.
enum : uint8_t { kOther, kPercent, kDot, kStar, kZero, kDigit, kFlag, kSize, kType };
uint8_t Class(uint8_t ch) {
  if (int8_t(ch) < ' ' || int8_t(ch) > 'x') return kOther;
  switch (ch) {
    case '%': return kPercent;
    case '.': return kDot;
    case '*': return kStar;
    case '0': return kZero;
    case ' ': case '#': case '+': case '-': return kFlag;
    case 'F': case 'I': case 'L': case 'N': case 'h': case 'l': case 'w': return kSize;
    case 'B': case 'C': case 'E': case 'G': case 'S': case 'X': case 'Z': case 'c':
    case 'd': case 'e': case 'f': case 'g': case 'i': case 'n': case 'o': case 'p':
    case 's': case 'u': case 'x':
      return kType;
    default: return ch >= '1' && ch <= '9' ? kDigit : kOther;
  }
}
// States: normal, percent, flag, width, dot, precision, size, type.
constexpr uint8_t kNext[9][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},  // other
    {1, 0, 0, 0, 0, 0, 0, 1},  // %
    {0, 4, 4, 4, 0, 0, 0, 0},  // .
    {0, 3, 3, 0, 5, 0, 0, 0},  // *
    {0, 2, 2, 3, 5, 5, 0, 0},  // 0
    {0, 3, 3, 3, 5, 5, 0, 0},  // 1-9
    {0, 2, 2, 0, 0, 0, 0, 0},  // flags
    {0, 6, 6, 6, 6, 6, 6, 0},  // sizes
    {0, 7, 7, 7, 7, 7, 7, 0},  // types
};

enum Flag : uint32_t {
  kSign = 0x1, kSpace = 0x2, kLeft = 0x4, kZeroPad = 0x8, kLong = 0x10, kShort = 0x20,
  kSigned = 0x40, kAlternate = 0x80, kNegative = 0x100, kForceOctal = 0x200, kWide = 0x800,
  kInt64 = 0x8000,
};

constexpr uint32_t kIoStrg = 0x40, kIoErr = 0x20;
constexpr int kBuffer = 0x200;  // the integers' part of the original's buffer

// The engine: _output (W false) and _woutput (W true). _woutput's
// format and output are 16-bit units: write_char is _putwc_lk's (_cnt
// less 2, a unit at _ptr; its return compared with 0xFFFF, so U+FFFF
// itself counts as a failure); a plain character marks the text wide;
// it ignores l, so %s is wide unless h, %S narrow unless w, %c and %C
// likewise; narrow text
// (digits, floats, %hs, %Z without w) goes out through mbtowc. Its
// buffer is 1 KiB (the floats' room), the integers' digits in its first
// half as in _output.
template <bool W>
class Output {
 public:
  Output(Ctx& c, uint32_t file) : c_(c), file_(file), fpu_(c.fpu) {
    std::memcpy(saved_file_, g_guest_base + file, sizeof saved_file_);
  }

  // false: the original's work (nothing the native code wrote is left).
  bool Run(uint32_t fmt, uint32_t ap);
  void Undo() {
    for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) g_guest_base[it->first] = it->second;
    std::memcpy(g_guest_base + file_, saved_file_, sizeof saved_file_);
    c_.fpu = fpu_;
  }
  int32_t charsout() const { return charsout_; }
  uint32_t fmt() const { return fmt_; }
  uint32_t ap() const { return ap_; }

 private:
  // The text of a conversion: guest memory, or host bytes (the buffer,
  // "(null)").
  struct Text {
    const char* host = nullptr;
    uint32_t guest = 0;
    uint8_t At(uint32_t i) const { return host ? uint8_t(host[i]) : R8(guest + i); }
    uint16_t WideAt(uint32_t i) const {
      if (!host) return R16(guest + 2 * i);
      uint16_t v;
      std::memcpy(&v, host + 2 * i, 2);
      return v;
    }
    // A unit of the engine's own width.
    uint32_t Unit(uint32_t i) const { return W ? WideAt(i) : uint32_t(int8_t(At(i))); }
  };
  static constexpr int kFloatBuffer = W ? 2 * kBuffer : kBuffer;

  void Poke(uint32_t a, uint8_t v) {
    undo_.emplace_back(a, R8(a));
    g_guest_base[a] = v;
  }
  uint32_t ReadFmt() {
    const uint32_t v = W ? R16(fmt_) : R8(fmt_);
    fmt_ += W ? 2 : 1;
    return v;
  }
  void WriteChar(uint32_t ch) {
    const int32_t cnt = int32_t(R32(file_ + 4)) - (W ? 2 : 1);
    Wr<uint32_t>(g_guest_base, file_ + 4, uint32_t(cnt));
    if (cnt < 0) {  // _flsbuf/_flswbuf on a string: _IOERR, EOF
      Wr<uint32_t>(g_guest_base, file_ + 0xC, R32(file_ + 0xC) | kIoErr);
      charsout_ = -1;
      return;
    }
    const uint32_t p = R32(file_);
    Poke(p, uint8_t(ch));
    if (W) Poke(p + 1, uint8_t(ch >> 8));
    Wr<uint32_t>(g_guest_base, file_, p + (W ? 2 : 1));
    if (W && uint16_t(ch) == 0xFFFF)
      charsout_ = -1;
    else
      charsout_++;
  }
  void MultiChar(uint32_t ch, int32_t n) {
    for (;;) {
      if (n < 1) return;
      n--;
      WriteChar(ch);
      if (charsout_ == -1) return;
    }
  }
  void String(const Text& t, int32_t n) {
    for (uint32_t i = 0; n > 0; i++) {
      n--;
      WriteChar(t.Unit(i));
      if (charsout_ == -1) return;
    }
  }
  uint32_t Int() {
    const uint32_t v = R32(ap_);
    ap_ += 4;
    return v;
  }
  bool CLocale() const { return !R32(GetLocale().ctype_handle); }
  uint16_t Ctype(int32_t ch) const { return R16(R32(GetLocale().pctype) + 2 * uint32_t(ch)); }
  // wctomb: a byte for wc; false for the original's cases. In the C locale
  // up to 0xFF; in a code page ASCII, which every Windows ANSI code page
  // maps to itself (WideCharToMultiByte otherwise).
  bool Wctomb(uint16_t wc, char* out) const {
    if (wc > (CLocale() ? 0xFF : 0x7F)) return false;
    *out = char(wc);
    return true;
  }
  // mbtowc of the byte at text[i]: 1 and its character, 0 for a NUL, -1
  // for the original's cases (outside the C locale, a lead byte or one
  // over 0x7F).
  int Mbtowc(uint8_t b, uint16_t* wc) const {
    if (!b) return 0;
    if (!CLocale() && (b > 0x7F || (Ctype(b) & 0x8000))) return -1;
    *wc = b;
    return 1;
  }
  bool Float(uint8_t type, int precision, bool caps);

  Ctx& c_;
  uint32_t file_;
  const x87::State fpu_;
  uint8_t saved_file_[16];
  std::vector<std::pair<uint32_t, uint8_t>> undo_;
  int32_t charsout_ = 0;
  uint32_t fmt_ = 0, ap_ = 0;
  // The original's buffer (room past it for the bounds below).
  std::array<char, 4 * kBuffer> buffer_{};
};

// _cfltcvt and its follow-ups into buffer_; false for the original's
// cases.
template <bool W>
bool Output<W>::Float(uint8_t type, int precision, bool caps) {
  x87::State& s = c_.fpu;
  if ((x87::ControlWord(s) & 0x3F) != 0x3F) return false;
  uint64_t bits = uint64_t(R32(ap_ + 4)) << 32 | R32(ap_);
  ap_ += 8;
  if (precision > kFloatBuffer) return false;
  // _cftoe/_cftof/_cftog: fld/fstp of the argument.
  x87::LoadF64(s, bits);
  bits = x87::StoreF64(s);
  x87::Pop(s);
  Strflt f = Fltout2(bits);
  const int room = type == 'f' ? 4 + std::max(f.decpt, 1) + precision : precision + 10;
  if (room >= kFloatBuffer) return false;
  const char dp = char(R8(GetLocale().decimal_point));
  char* buf = buffer_.data();
  const bool neg = f.sign == '-';
  if (type == 'e') {
    Fptostr(buf + (precision > 0) + neg, precision + 1, f);
    Cftoe2(buf, precision, caps, f, false, dp);
  } else if (type == 'f') {
    Fptostr(buf + neg, f.decpt + precision, f);
    Cftof2(buf, precision, f, false, dp);
  } else {
    const int mag = f.decpt - 1;
    char* digits = buf + neg;
    Fptostr(digits, precision, f);
    const int mag2 = f.decpt - 1;
    if (mag2 < -4 || mag2 >= precision) {
      Cftoe2(buf, precision, caps, f, true, dp);
    } else {
      if (mag < mag2) digits[std::strlen(digits) - 1] = 0;  // the rounding added a digit
      Cftof2(buf, precision, f, true, dp);
    }
  }
  return true;
}

template <bool W>
bool Output<W>::Run(uint32_t fmt, uint32_t ap) {
  fmt_ = fmt;
  ap_ = ap;
  static const char kNull[] = "(null)";
  static const char16_t kWideNull[] = u"(null)";
  // A narrow and a wide string's flags: the narrow engine's %s is wide
  // with l or w, the wide engine's narrow with h.
  const uint32_t narrow_flags = W ? kShort : 0, wide_flags = W ? 0 : kLong | kWide;
  int state = 0, precision = 0, fldwidth = 0, prefixlen = 0, hexadd = 0;
  bool capexp = false, no_output = false, wide = false;
  uint32_t flags = 0;
  int32_t textlen = 0;
  Text text;
  uint16_t prefix[2] = {};
  uint32_t ch = ReadFmt();
  while (ch && charsout_ >= 0) {
    // Classes by the character's value (the narrow engine's as a signed
    // char, the wide one's unsigned).
    const int32_t v = W ? int32_t(ch) : int8_t(ch);
    const uint8_t cls = v >= 0x20 && v <= 0x78 ? Class(uint8_t(v)) : kOther;
    state = kNext[cls][state];
    switch (state) {
      case 6:
        if (ch == 'I') {
          const uint32_t a = W ? R16(fmt_) : R8(fmt_), b = W ? R16(fmt_ + 2) : R8(fmt_ + 1);
          if (a != '6' || b != '4') {
            state = 0;
            goto normal;
          }
          fmt_ += W ? 4 : 2;
          flags |= kInt64;
        } else if (ch == 'h') {
          flags |= kShort;
        } else if (ch == 'l' && !W) {  // _woutput has no l
          flags |= kLong;
        } else if (ch == 'w') {
          flags |= kWide;
        }
        break;
      case 0:
      normal:
        if (W) {
          wide = true;
          WriteChar(ch);
        } else {
          wide = false;
          if (Ctype(int32_t(ch)) & 0x8000) {  // a lead byte: it and the next go out
            WriteChar(uint32_t(int8_t(ch)));
            ch = ReadFmt();
          }
          WriteChar(uint32_t(int8_t(ch)));
        }
        break;
      case 1:
        precision = -1;
        capexp = no_output = wide = false;
        fldwidth = prefixlen = 0;
        flags = 0;
        break;
      case 2:
        flags |= ch == ' ' ? kSpace : ch == '#' ? kAlternate : ch == '+' ? kSign
               : ch == '-' ? kLeft : ch == '0' ? kZeroPad : 0;
        break;
      case 3:
        if (ch == '*') {
          fldwidth = int32_t(Int());
          if (fldwidth < 0) {
            flags |= kLeft;
            fldwidth = int32_t(0u - uint32_t(fldwidth));
          }
        } else {
          fldwidth = int32_t(uint32_t(fldwidth) * 10 + (ch - '0'));
        }
        break;
      case 4:
        precision = 0;
        break;
      case 5:
        if (ch == '*') {
          precision = int32_t(Int());
          if (precision < 0) precision = -1;
        } else {
          precision = int32_t(uint32_t(precision) * 10 + (ch - '0'));
        }
        break;
      case 7: {
        int radix = 0;
        switch (ch) {
          case 'C':
            if (!(flags & (kShort | kLong | kWide))) flags |= W ? kShort : kWide;
            [[fallthrough]];
          case 'c':
            if (W) {
              wide = true;
              const uint32_t v = Int();
              uint16_t wc = uint16_t(v);
              if (flags & kShort) {
                const int r = Mbtowc(uint8_t(v), &wc);
                if (r < 0) return false;
                if (r == 0) wc = 0;
              }
              std::memcpy(buffer_.data(), &wc, 2);
            } else if (flags & (kLong | kWide)) {
              if (!Wctomb(uint16_t(Int()), buffer_.data())) return false;
            } else {
              buffer_[0] = char(Int());
            }
            textlen = 1;
            text = {buffer_.data(), 0};
            break;
          case 'Z': {
            const uint32_t s = Int(), b = s ? R32(s + 4) : 0;
            if (!b) {
              text = {kNull, 0};
              textlen = int32_t(std::strlen(kNull));
            } else if (flags & kWide) {
              text = {nullptr, b};
              textlen = int32_t(uint32_t(int32_t(int16_t(R16(s)))) >> 1);
              wide = true;
            } else {
              text = {nullptr, b};
              textlen = int16_t(R16(s));
              wide = false;
            }
            break;
          }
          case 'E':
          case 'G':
            capexp = true;
            ch += 0x20;
            [[fallthrough]];
          case 'e':
          case 'f':
          case 'g':
            flags |= kSigned;
            if (precision < 0)
              precision = 6;
            else if (precision == 0 && ch == 'g')
              precision = 1;
            if (!Float(uint8_t(ch), precision, capexp)) return false;
            if ((flags & kAlternate) && precision == 0) {
              // _forcdecpt: the decimal point after the leading digits.
              if (R32(GetLocale().mb_cur_max) >= 2) return false;
              char* p = buffer_.data();
              if (*p != 'e' && *p != 'E')
                do p++;
                while (Ctype(int8_t(*p)) & 4);
              char cur = *p;
              *p = char(R8(GetLocale().decimal_point));
              do {
                p++;
                const char next = *p;
                *p = cur;
                cur = next;
              } while (*p);
            }
            if (ch == 'g' && !(flags & kAlternate))
              Cropzeros(buffer_.data(), char(R8(GetLocale().decimal_point)));
            text = {buffer_.data(), 0};
            if (buffer_[0] == '-') {
              flags |= kNegative;
              text.host++;
            }
            textlen = int32_t(std::strlen(text.host));
            break;
          case 'd':
          case 'i':
            flags |= kSigned;
            radix = 10;
            break;
          case 'u':
            radix = 10;
            break;
          case 'o':
            radix = 8;
            if (flags & kAlternate) flags |= kForceOctal;
            break;
          case 'p':
            precision = 8;
            [[fallthrough]];
          case 'X':
            hexadd = 7;
            radix = 16;
            break;
          case 'x':
            hexadd = 0x27;
            radix = 16;
            break;
          case 'n': {
            const uint32_t p = Int();
            const uint32_t v = uint32_t(charsout_);
            for (int i = 0; i < (flags & kShort ? 2 : 4); i++) Poke(p + i, uint8_t(v >> 8 * i));
            no_output = true;
            goto next;
          }
          case 'S':
            if (!(flags & (kShort | kLong | kWide))) flags |= W ? kShort : kWide;
            [[fallthrough]];
          case 's': {
            uint32_t left = precision == -1 ? 0x7FFFFFFF : uint32_t(precision);
            const uint32_t s = Int();
            const bool narrow = W ? (flags & narrow_flags) != 0 : !(flags & wide_flags);
            if (narrow && W) {
              // Characters, a lead byte taking the next with it.
              text = s ? Text{nullptr, s} : Text{kNull, 0};
              uint32_t n = 0, at = 0;
              if (int32_t(left) > 0) do {
                  const uint8_t b = text.At(at);
                  if (!b) break;
                  if (Ctype(b) & 0x8000) at++;
                  at++;
                  n++;
                } while (int32_t(n) < int32_t(left));
              textlen = int32_t(n);
            } else if (narrow) {
              text = s ? Text{nullptr, s} : Text{kNull, 0};
              uint32_t n = 0;
              while (left && text.At(n)) left--, n++;
              textlen = int32_t(n);
            } else {
              text = s ? Text{nullptr, s} : Text{reinterpret_cast<const char*>(kWideNull), 0};
              wide = true;
              uint32_t n = 0;
              while (left && text.WideAt(n)) left--, n++;
              textlen = int32_t(n);
            }
            break;
          }
          default:  // an unknown type: the last conversion's text again
            break;
        }
        if (radix) {
          if (radix == 16 && (flags & kAlternate)) {
            prefix[0] = '0';
            prefix[1] = uint16_t(hexadd + 'Q');
            prefixlen = 2;
          }
          uint64_t num;
          if (flags & kInt64) {
            num = Int();
            num |= uint64_t(Int()) << 32;
          } else if (flags & kShort) {
            const uint32_t v = Int();
            num = flags & kSigned ? uint64_t(int64_t(int16_t(v))) : uint64_t(v & 0xFFFF);
          } else {
            const uint32_t v = Int();
            num = flags & kSigned ? uint64_t(int64_t(int32_t(v))) : uint64_t(v);
          }
          if ((flags & kSigned) && int64_t(num) < 0) {
            flags |= kNegative;
            num = 0 - num;
          }
          if (!(flags & kInt64)) num &= 0xFFFFFFFF;
          if (precision < 0)
            precision = 1;
          else
            flags &= ~kZeroPad;
          if (!num) prefixlen = 0;
          if (precision > kBuffer - 2) return false;
          int at = kBuffer - 1;
          while (precision-- > 0 || num) {
            int digit = int(num % uint64_t(radix)) + '0';
            num /= uint64_t(radix);
            if (digit > '9') digit += hexadd;
            buffer_[at--] = char(digit);
          }
          textlen = kBuffer - 1 - at;
          text = {buffer_.data() + at + 1, 0};
          if ((flags & kForceOctal) && (text.host[0] != '0' || !textlen)) {
            buffer_[at] = '0';
            text.host--;
            textlen++;
          }
        }
        if (no_output) break;
        if (flags & kSigned) {
          if (flags & kNegative) {
            prefix[0] = '-';
            prefixlen = 1;
          } else if (flags & kSign) {
            prefix[0] = '+';
            prefixlen = 1;
          } else if (flags & kSpace) {
            prefix[0] = ' ';
            prefixlen = 1;
          }
        }
        // The prefix as units of the engine's width.
        char units[4];
        for (int k = 0; k < 2; k++) {
          if (W)
            std::memcpy(units + 2 * k, &prefix[k], 2);
          else
            units[k] = char(prefix[k]);
        }
        const int32_t padding = fldwidth - prefixlen - textlen;
        if (!(flags & (kLeft | kZeroPad))) MultiChar(' ', padding);
        String(Text{units, 0}, prefixlen);
        if ((flags & kZeroPad) && !(flags & kLeft)) MultiChar('0', padding);
        if (!W && wide && textlen > 0) {
          uint32_t n = uint32_t(textlen), i = 0;
          do {
            n--;
            char mb;
            if (!Wctomb(text.WideAt(i++), &mb)) return false;
            String(Text{&mb, 0}, 1);
          } while (n);
        } else if (W && !wide && textlen > 0) {
          // Narrow text, a character at a time through mbtowc.
          int32_t n = textlen - 1;
          uint32_t at = 0;
          bool more;
          do {
            uint16_t wc;
            const int r = Mbtowc(text.At(at), &wc);
            if (r < 0) return false;
            if (r < 1) break;
            WriteChar(wc);
            at += uint32_t(r);
            more = n > 0;
            n--;
          } while (more);
        } else {
          String(text, textlen);
        }
        if (flags & kLeft) MultiChar(' ', padding);
        break;
      }
    }
  next:
    ch = ReadFmt();
  }
  return true;
}

// _output and _woutput(FILE*, format, va_list).
template <bool W>
void Engine(Ctx& c, GuestFn original) {
  const uint32_t file = R32(c.esp + 4), fmt = R32(c.esp + 8), ap = R32(c.esp + 12);
  if (!GetLocale().pctype || !GetLocale().decimal_point)
    FatalF("msvc_crt::vc6: _output ran before SetLocale (with decimal_point)");
  if (!(R32(file + 0xC) & kIoStrg)) return original(c);
  Output<W> o(c, file);
  if (!o.Run(fmt, ap)) {
    o.Undo();
    return original(c);
  }
  Wr<uint32_t>(g_guest_base, c.esp + 8, o.fmt());
  Wr<uint32_t>(g_guest_base, c.esp + 12, o.ap());
  c.eax = uint32_t(o.charsout());
  c.esp += 4;
}

}  // namespace

void _output(Ctx& c, GuestFn original) { Engine<false>(c, original); }
void _woutput(Ctx& c, GuestFn original) { Engine<true>(c, original); }

}  // namespace recomp::native::msvc_crt::vc6
