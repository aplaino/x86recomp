// VC6's _input (input.c: the engine of sscanf and fscanf), _fltin2 and
// atof, natively, for string streams; docs/msvc_crt.md.
//
// The engine is VC6's: whitespace in the format skips whitespace in the
// input, other characters match themselves (a DBCS lead byte with its
// trail), and a conversion has `*`, a width and the sizes h, l, L, w and
// I64 (F and N ignored) before its type: c, s and [...] (one set table,
// c accepting everything and s everything but space and \t..\r), d i o u
// x p as 32 or 64 bits (i taking a 0x or 0 prefix, x a 0x one), e f g
// through _fassign, and n. A character it does not know matches itself
// in the input (%% among them). It counts the characters it reads, the
// conversions it assigns and the directives it matched, and returns EOF
// when the input ends before any. Characters come in as _inc takes them
// (--_cnt, a byte at _ptr, and _filbuf's EOF for a string once _cnt is
// below zero) and go back through _ungetc_lk (for a string only when the
// byte before _ptr is the same). The format pointer and the argument
// pointer are left in the argument slots as the original leaves them.
//
// Numbers are VC6's own __strgtold12 (digits to a 12-byte long double:
// up to 25 significant digits, the 25th rounding the 24th up when it is
// over 4, then __mtold12 and __multtenpow12) and _ld12cvt (to a double
// or a float: VC6's _RoundMan, which rounds a half down and does not look
// at the bit after the rounding bit, and its denormals, which always
// report an underflow). _fltin2 puts the double through fld/fstp, atof
// leaves it in ST0.
//
// The original runs, from the start (whatever the native code wrote is
// put back), for a stream that is not a string (_IOSTRG), a multibyte
// __mb_cur_max (_isctype), a wide %c/%s/%[ character beyond what mbtowc
// below takes, a float field with a width past INT_MAX (the original
// overruns its buffer), a read of the last character before any was
// read (the original's is whatever its stack holds), and in _fltin2 and
// atof an exception unmasked in the caller's control word. The locale
// lock of mbtowc is not taken (as for the locale functions).

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
uint32_t R32(uint32_t a) { return Rd<uint32_t>(g_guest_base, a); }

uint16_t Ctype(int32_t ch) {
  return Rd<uint16_t>(g_guest_base, R32(GetLocale().pctype) + 2 * uint32_t(ch));
}
bool Multibyte() { return int32_t(R32(GetLocale().mb_cur_max)) > 1; }
bool IsDigit(int32_t ch) { return Ctype(ch) & 0x4; }
bool IsSpace(int32_t ch) { return Ctype(ch) & 0x8; }

// ---- __strgtold12 and _ld12cvt ------------------------------------------

// __mtold12: the digits (signed bytes) as an integer, normalized; false
// where they sum to zero (the original loops forever).
bool Mtold12(const uint8_t* man, int len, Ld12& x) {
  std::memset(x.b, 0, sizeof x.b);
  uint16_t exp = 0x404E;
  for (int i = 0; i < len; i++) {
    const Ld12 t = x;
    Shl12(x);
    Shl12(x);
    Add12(x, t);
    Shl12(x);
    Ld12 d{};
    d.SetD(0, uint32_t(int32_t(int8_t(man[i]))));
    Add12(x, d);
  }
  if (!x.D(0) && !x.D(4) && !x.D(8)) return false;
  while (!x.D(8)) {
    x.SetD(8, x.D(4) >> 16);
    x.SetD(4, x.D(0) >> 16 | x.D(4) << 16);
    x.SetD(0, x.D(0) << 16);
    exp = uint16_t(exp - 16);
  }
  while (!(x.D(8) & 0x8000)) {
    Shl12(x);
    exp--;
  }
  x.SetW(10, exp);
  return true;
}

// __strgtold12's results (SLD_*).
enum : int { kUnderflow = 1, kOverflow = 2, kNoDigits = 4 };

// __strgtold12(x, &end, s, 0, 0, 0, 0): the number at s (after spaces,
// tabs, CR and LF) as a 12-byte long double, *end where it stopped.
// -1 for the original's case (see Mtold12).
int Strgtold12(const uint8_t* s, Ld12& x, const uint8_t** end) {
  const uint8_t dp = R8(GetLocale().decimal_point);
  auto nonzero = [](uint8_t c) { return int8_t(c) > '0' && int8_t(c) <= '9'; };
  auto exp_char = [](uint8_t c) { return c == 'D' || c == 'E' || c == 'd' || c == 'e'; };
  uint8_t man[25];
  int manlen = 0, exp_adj = 0, pow = 0, exp_sign = 1;
  uint16_t sign = 0;
  bool found_digit = false;
  const uint8_t* p = s;
  const uint8_t* saved = s;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  // States: 0 start, 1 leading zeros, 2 after the sign, 3 integer digits,
  // 4 fraction digits, 5 a leading point, 6 after E, 7 the exponent's
  // sign, 8 its leading zeros, 9 its digits; 10 the end.
  int state = 0;
  // After the mantissa's digits: the exponent, or the end. A sign there
  // ends the number (the callers pass no implicit E).
  auto after_mantissa = [&](uint8_t c) {
    if (c == '+' || c == '-')
      p--, state = 10;
    else if (exp_char(c))
      state = 6;
    else
      p--, state = 10;
  };
  while (state != 10) {
    uint8_t c = *p++;
    switch (state) {
      case 0:
        if (nonzero(c))
          state = 3, p--;
        else if (c == dp)
          state = 5;
        else if (c == '+')
          sign = 0, state = 2;
        else if (c == '-')
          sign = 0x8000, state = 2;
        else if (c == '0')
          state = 1;
        else
          p--, state = 10;
        break;
      case 1:
        found_digit = true;
        if (nonzero(c))
          state = 3, p--;
        else if (c == dp)
          state = 4;
        else if (c == '+' || c == '-')
          p--, state = 10;
        else if (c != '0')
          after_mantissa(c);
        break;
      case 2:
        if (nonzero(c))
          state = 3, p--;
        else if (c == dp)
          state = 5;
        else if (c == '0')
          state = 1;
        else
          p = saved, state = 10;
        break;
      case 3:
        found_digit = true;
        for (; IsDigit(c); c = *p++) {
          if (manlen < 25)
            man[manlen++] = uint8_t(c - '0');
          else
            exp_adj++;
        }
        if (c == dp)
          state = 4;
        else
          after_mantissa(c);
        break;
      case 4:
        found_digit = true;
        if (!manlen)
          for (; c == '0'; c = *p++) exp_adj--;
        for (; IsDigit(c); c = *p++)
          if (manlen < 25) {
            man[manlen++] = uint8_t(c - '0');
            exp_adj--;
          }
        after_mantissa(c);
        break;
      case 5:
        if (IsDigit(c))
          state = 4, p--;
        else
          p = saved, state = 10;
        break;
      case 6:
        saved = p - 2;
        if (nonzero(c))
          state = 9, p--;
        else if (c == '+')
          state = 7;
        else if (c == '-')
          exp_sign = -1, state = 7;
        else if (c == '0')
          state = 8;
        else
          p = saved, state = 10;
        break;
      case 7:
        if (nonzero(c))
          state = 9, p--;
        else if (c == '0')
          state = 8;
        else
          p = saved, state = 10;
        break;
      case 8:
        while (c == '0') c = *p++;
        if (nonzero(c))
          state = 9, p--;
        else
          p--, state = 10;
        break;
      case 9:
        for (; IsDigit(c); c = *p++) {
          pow = int8_t(c) - '0' + pow * 10;
          if (pow > 5200) {
            pow = 5201;
            break;
          }
        }
        while (IsDigit(c)) c = *p++;
        p--, state = 10;
        break;
    }
  }
  *end = p;
  std::memset(x.b, 0, sizeof x.b);
  int flags = 0;
  if (!found_digit) {
    flags = kNoDigits;
  } else {
    uint8_t* last = man + manlen;
    if (manlen >= 25) {
      if (int8_t(man[23]) > 4) man[23]++;
      manlen = 24;
      exp_adj++;
      last = man + 24;
    }
    if (manlen) {
      while (!*--last) {
        manlen--;
        exp_adj++;
      }
      if (!Mtold12(man, manlen, x)) return -1;
      const int n = (exp_sign < 0 ? -pow : pow) + exp_adj;
      if (n > 5200) {
        std::memset(x.b, 0, sizeof x.b);
        x.SetD(6, 0x80000000);
        x.SetW(10, 0x7FFF);
        flags = kOverflow;
      } else if (n < -5200) {
        std::memset(x.b, 0, sizeof x.b);
        flags = kUnderflow;
      } else {
        MultTenPow12(x, n, false);
      }
    }
  }
  x.SetW(10, x.W(10) | sign);
  return flags;
}

// _ld12cvt's FpFormatDescriptor.
struct FpFormat {
  int max_exp, min_exp, precision, exp_width;
  bool f64;
  int bias;
};
constexpr FpFormat kDouble{1024, -1023, 53, 11, true, 1023}, kFloat{128, -127, 24, 8, false, 127};

// The significand as _ld12cvt holds it: three words, the top one first.
using Man = std::array<uint32_t, 3>;

// _ZeroTail(m, nbit): the bits after bit nbit's word position zero (bit
// nbit itself is not looked at).
bool ZeroTail(const Man& m, int nbit) {
  int nl = nbit / 32;
  if (m[nl] & ~(~0u << (31 - nbit % 32))) return false;
  for (nl++; nl < 3; nl++)
    if (m[nl]) return false;
  return true;
}

// _IncMan(m, nbit): one added at bit nbit; the carry out of the top.
bool IncMan(Man& m, int nbit) {
  int nl = nbit / 32;
  bool carry = Addl(m[nl], 1u << (31 - nbit % 32), &m[nl]);
  for (nl--; nl >= 0 && carry; nl--) carry = Addl(m[nl], 1, &m[nl]);
  return carry;
}

// _RoundMan(m, precision): up when bit `precision` is set and the tail
// after the next bit is not zero; the bits from `precision` on cleared.
bool RoundMan(Man& m, int precision) {
  const int nl = precision / 32, bit = 31 - precision % 32;
  bool carry = false;
  if ((m[nl] >> bit & 1) && !ZeroTail(m, precision + 1)) carry = IncMan(m, precision - 1);
  m[nl] &= ~0u << bit;
  for (int i = nl + 1; i < 3; i++) m[i] = 0;
  return carry;
}

// _ShrMan(m, n).
void ShrMan(Man& m, int n) {
  const int words = n / 32, bits = n % 32;
  uint32_t carry = 0;
  for (uint32_t& w : m) {
    const uint32_t v = w;
    w = v >> bits | carry;
    carry = bits ? (v & ~(~0u << bits)) << (32 - bits) : 0;
  }
  for (int i = 2; i >= 0; i--) m[i] = i < words ? 0 : m[i - words];
}

// _ld12cvt: x as a double or a float (*out); 0, 1 for an overflow
// (infinity), 2 for an underflow (a denormal or zero).
int Ld12cvt(const Ld12& x, const FpFormat& f, uint64_t* out) {
  const uint16_t se = x.W(10);
  Man m = {x.D(6), x.D(2), uint32_t(x.W(0)) << 16};
  int e = (se & 0x7FFF) - 0x3FFF;
  int ret;
  if (e == -0x3FFF) {
    e = 0;
    if (!m[0] && !m[1] && !m[2]) {
      ret = 0;
    } else {
      m = {};
      ret = 2;
    }
  } else {
    const Man saved = m;
    if (RoundMan(m, f.precision)) e++;
    if (e < f.min_exp - f.precision) {
      m = {};
      e = 0;
      ret = 2;
    } else if (e > f.min_exp) {
      if (e >= f.max_exp) {
        m = {0x80000000, 0, 0};
        ShrMan(m, f.exp_width);
        e = f.bias + f.max_exp;
        ret = 1;
      } else {
        m[0] &= 0x7FFFFFFF;
        e += f.bias;
        ShrMan(m, f.exp_width);
        ret = 0;
      }
    } else {
      m = saved;
      ShrMan(m, f.min_exp - e);
      RoundMan(m, f.precision);
      ShrMan(m, f.exp_width + 1);
      e = 0;
      ret = 2;
    }
  }
  m[0] |= uint32_t(e) << (31 - f.exp_width) | (se & 0x8000 ? 0x80000000 : 0);
  *out = f.f64 ? uint64_t(m[0]) << 32 | m[1] : m[0];
  return ret;
}

// _fltin2's FLT: flags, the bytes it took, and the double.
struct Flt {
  uint32_t flags, nbytes;
  uint64_t dval;
};

// _fltin2's work: false for the original's case (see Mtold12).
bool Fltin2(uint32_t s, Flt& flt) {
  Ld12 x;
  const uint8_t* end;
  const int f = Strgtold12(g_guest_base + s, x, &end);
  if (f < 0) return false;
  flt.flags = 0;
  flt.dval = 0;
  if (f & kNoDigits) {
    flt.flags = 0x200;
  } else {
    const int r = Ld12cvt(x, kDouble, &flt.dval);
    if ((f & kOverflow) || r == 1) flt.flags = 0x80;
    if ((f & kUnderflow) || r == 2) flt.flags |= 0x100;
  }
  flt.nbytes = uint32_t(end - (g_guest_base + s));
  return true;
}

// ---- the engine -------------------------------------------------------

constexpr uint32_t kIoRead = 0x1, kIoRw = 0x80, kIoWrt = 0x2, kIoEof = 0x10, kIoStrg = 0x40;
constexpr int32_t kEof = -1;

class Input {
 public:
  explicit Input(uint32_t file) : file_(file) {}

  // false: the original's work (nothing the native code wrote is left).
  bool Run(uint32_t fmt, uint32_t ap);
  void Undo() {
    for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) g_guest_base[it->first] = it->second;
  }
  int32_t result() const { return result_; }
  uint32_t fmt_slot() const { return fmt_slot_; }
  uint32_t ap_slot() const { return ap_; }

 private:
  void Poke(uint32_t a, uint8_t v) {
    undo_.emplace_back(a, R8(a));
    g_guest_base[a] = v;
  }
  void Poke16(uint32_t a, uint16_t v) {
    Poke(a, uint8_t(v));
    Poke(a + 1, uint8_t(v >> 8));
  }
  void Poke32(uint32_t a, uint32_t v) {
    Poke16(a, uint16_t(v));
    Poke16(a + 2, uint16_t(v >> 16));
  }
  // _inc: --_cnt, then a byte, or _filbuf's EOF for a string.
  int32_t Inc() {
    charcount_++;
    const int32_t cnt = int32_t(R32(file_ + 4)) - 1;
    Poke32(file_ + 4, uint32_t(cnt));
    if (cnt < 0) return kEof;
    const uint32_t p = R32(file_);
    Poke32(file_, p + 1);
    return R8(p);
  }
  // _un_inc: _ungetc_lk for anything but EOF.
  void UnInc(int32_t ch) {
    charcount_--;
    if (ch == kEof) return;
    const uint32_t flag = R32(file_ + 0xC);
    if (!(flag & kIoRead) && (!(flag & kIoRw) || (flag & kIoWrt))) return;
    if (!R32(file_ + 8)) return Fail();  // _getbuf
    uint32_t p = R32(file_);
    if (p == R32(file_ + 8)) {
      if (R32(file_ + 4)) return;
      Poke32(file_, ++p);
    }
    Poke32(file_, --p);
    if (int8_t(R8(p)) != int8_t(ch)) return Poke32(file_, p + 1);
    Poke32(file_ + 4, R32(file_ + 4) + 1);
    Poke32(file_ + 0xC, (flag & ~kIoEof) | kIoRead);
  }
  // _whiteout.
  int32_t EatWhite() {
    int32_t ch;
    do ch = Inc();
    while (IsSpace(ch));
    return ch;
  }
  // The last character read, which the original keeps across directives.
  int32_t Ch() {
    if (!ch_read_) Fail();
    return ch_;
  }
  void SetCh(int32_t ch) {
    ch_ = ch;
    ch_read_ = true;
  }
  void Fail() { failed_ = true; }
  bool CLocale() const { return !R32(GetLocale().ctype_handle); }
  // mbtowc of a byte that is not a lead byte: the byte in the C locale,
  // and in a code page for ASCII, which every Windows ANSI code page maps
  // to itself (MultiByteToWideChar otherwise); false for the original's
  // cases.
  bool Mbtowc(uint8_t b, uint16_t* wc) const {
    if (!CLocale() && b > 0x7F) return false;
    *wc = b;
    return true;
  }
  // The end of the input or of a directive: count, or EOF when the input
  // ended before anything matched.
  void Return(int32_t ch) { result_ = ch == kEof && !count_ && !match_ ? -1 : count_; }

  uint32_t file_;
  std::vector<std::pair<uint32_t, uint8_t>> undo_;
  int32_t charcount_ = 0, count_ = 0, result_ = 0;
  int8_t match_ = 0;
  int32_t ch_ = 0;
  bool ch_read_ = false, failed_ = false;
  uint32_t fmt_slot_ = 0, ap_ = 0;
};

bool Input::Run(uint32_t fmt, uint32_t ap) {
  fmt_slot_ = fmt;
  ap_ = ap;
  uint32_t f = fmt;  // the original's register copy of the format pointer
  while (R8(f)) {
    if (IsSpace(R8(f))) {
      UnInc(EatWhite());
      while (IsSpace(R8(f + 1))) f++;
      f++;
    }
    if (R8(f) == '%') {
      int32_t width = 0, widthset = 0;
      int8_t suppress = 0, longone = 1, widechar = 0;
      bool done = false, int64 = false, negative = false, wide_arg = false;
      uint32_t number = 0;
      uint64_t num64 = 0;
      for (;;) {
        const uint8_t comchr = R8(++f);
        if (IsDigit(comchr)) {
          widthset++;
          width = int32_t(uint32_t(comchr) + uint32_t(width) * 10 - '0');
          continue;
        }
        if (comchr == 'N' || comchr == 'F') continue;
        if (comchr == '*') {
          suppress++;
        } else if (comchr == 'I' && R8(f + 1) == '6' && R8(f + 2) == '4') {
          int64 = true;
          num64 = 0;
          f += 2;
        } else if (comchr == 'L') {
          longone++;
        } else if (comchr == 'h') {
          longone--;
          widechar--;
        } else if (comchr == 'l') {
          longone++;
          widechar++;
        } else if (comchr == 'w') {
          widechar++;
        } else {
          break;
        }
      }
      fmt_slot_ = f;
      uint32_t pointer = 0, saved_ap = 0;
      if (!suppress) {
        saved_ap = ap_;
        ap_ += 4;
        pointer = R32(ap_ - 4);
      }
      if (!widechar) widechar = R8(f) == 'S' || R8(f) == 'C' ? 1 : -1;
      uint32_t comchr = R8(f) | 0x20;
      if (comchr != 'n') SetCh(comchr == 'c' || comchr == '{' ? Inc() : EatWhite());
      if (failed_) return false;
      if (widthset && !width) {
        UnInc(Ch());
        break;
      }
      auto assign = [&] {
        if (int64) {
          Poke32(pointer, uint32_t(num64));
          Poke32(pointer + 4, uint32_t(num64 >> 32));
        } else if (longone) {
          Poke32(pointer, number);
        } else {
          Poke16(pointer, uint16_t(number));
        }
      };
      bool ok = true;  // false: the input did not match (the error return)
      switch (comchr) {
        case 'c':
        case 's':
        case '{': {
          // The set: c's empty and s's whitespace, both rejected, or the
          // format's.
          uint32_t scan;
          int8_t reject = 0;
          if (widechar > 0) wide_arg = true;
          if (comchr == 'c') {
            if (!widthset) widthset = 1, width++;
            scan = 0, reject = -1;
          } else if (comchr == 's') {
            scan = 0, reject = -1;
          } else {
            scan = ++f;
            if (R8(scan) == '^') scan++, reject = -1;
          }
          uint8_t table[32] = {};
          static const uint8_t kSpaces[] = " \t-\r]";
          auto set_char = [&](uint32_t i) { return scan ? R8(scan + i) : comchr == 's' ? kSpaces[i] : ']'; };
          uint32_t i = 0;
          uint8_t prevchar = 0;
          if (comchr == '{' && set_char(0) == ']') {
            prevchar = ']';
            table[']' >> 3] = 1 << (']' & 7);
            i++;
          }
          while (set_char(i) != ']') {
            uint8_t rngch = set_char(i);
            if (rngch == '-' && prevchar && set_char(i + 1) != ']') {
              uint8_t last = set_char(i + 1);
              if (prevchar < last) {
                rngch = prevchar;
              } else {
                rngch = last;
                last = prevchar;
              }
              for (int c = rngch; c <= last; c++) table[c >> 3] |= uint8_t(1 << (c & 7));
              prevchar = 0;
              i += 2;
            } else {
              prevchar = rngch;
              table[rngch >> 3] |= uint8_t(1 << (rngch & 7));
              i++;
            }
          }
          // (The original's check for a NUL after it is never reached:
          // the set runs to a ']'.)
          if (comchr == '{') f = fmt_slot_ = scan + i;
          uint32_t start = pointer, p = pointer;
          UnInc(Ch());
          for (;;) {
            if (widthset && !width--) break;
            SetCh(Inc());
            const int32_t ch = Ch();
            if (ch == kEof || !((table[ch >> 3] ^ uint8_t(reject)) & (1 << (ch & 7)))) {
              UnInc(ch);
              break;
            }
            if (suppress) {
              start++;
              continue;
            }
            if (!wide_arg) {
              Poke(p++, uint8_t(ch));
              continue;
            }
            uint16_t wc;
            if ((Ctype(ch & 0xFF) & 0x8000) || !Mbtowc(uint8_t(ch), &wc)) return false;
            Poke16(p, wc);
            p += 2;
          }
          if (start == p) {
            ok = false;
            break;
          }
          if (!suppress) {
            count_++;
            if (comchr != 'c') {
              if (wide_arg)
                Poke16(p, 0);
              else
                Poke(p, 0);
            }
          }
          break;
        }
        case 'e':
        case 'f':
        case 'g': {
          char buf[352];
          char* s = buf;
          int started = 0;
          int32_t ch = Ch();
          auto take = [&] {
            SetCh(Inc());
            ch = Ch();
          };
          if (ch == '-' || ch == '+') {
            if (ch == '-') *s++ = '-';
            width--;
            take();
          }
          if (!widthset || width > 349) width = 349;
          if (width < 0) return false;
          const uint8_t dp = R8(GetLocale().decimal_point);
          while (IsDigit(ch) && width-- != 0) {
            started++;
            *s++ = char(ch);
            take();
          }
          if (uint8_t(ch) == dp && width-- != 0) {
            take();
            *s++ = char(dp);
            while (IsDigit(ch) && width-- != 0) {
              started++;
              *s++ = char(ch);
              take();
            }
          }
          if (started && (ch == 'e' || ch == 'E') && width-- != 0) {
            *s++ = 'e';
            take();
            if (ch == '-' || ch == '+') {
              if (ch == '-') *s++ = '-';
              if (width-- == 0)
                width = 0;
              else
                take();
            }
            while (IsDigit(ch) && width-- != 0) {
              started++;
              *s++ = char(ch);
              take();
            }
          }
          UnInc(ch);
          if (!started) {
            ok = false;
            break;
          }
          if (!suppress) {
            count_++;
            *s = 0;
            // _fassign: _atodbl or _atoflt.
            Ld12 x;
            const uint8_t* end;
            if (Strgtold12(reinterpret_cast<const uint8_t*>(buf), x, &end) < 0) return false;
            uint64_t v;
            if (int8_t(longone - 1)) {
              Ld12cvt(x, kDouble, &v);
              Poke32(pointer, uint32_t(v));
              Poke32(pointer + 4, uint32_t(v >> 32));
            } else {
              Ld12cvt(x, kFloat, &v);
              Poke32(pointer, uint32_t(v));
            }
          }
          break;
        }
        case 'n':
          if (suppress) break;
          number = uint32_t(charcount_);
          assign();
          break;
        case 'd':
        case 'i':
        case 'o':
        case 'p':
        case 'u':
        case 'x': {
          int32_t ch = Ch();
          int started = 0;
          if (comchr == 'p') longone = 1;
          if (comchr == 'i') comchr = 'd';
          if (ch == '-' || ch == '+') {
            if (ch == '-') negative = true;
            if (--width == 0 && widthset) {
              done = true;
            } else {
              SetCh(Inc());
              ch = Ch();
            }
          }
          if ((R8(f) | 0x20) == 'i' || comchr == 'x') {
            if (ch == '0') {
              SetCh(Inc());
              ch = Ch();
              if (int8_t(ch) == 'x' || int8_t(ch) == 'X') {
                SetCh(Inc());
                ch = Ch();
                comchr = 'x';
              } else {
                started = 1;
                if (comchr == 'x') {
                  UnInc(ch);
                  SetCh('0');
                  ch = '0';
                } else {
                  comchr = 'o';
                }
              }
            }
          }
          const bool hex = comchr == 'x' || (!int64 && comchr == 'p');
          while (!done) {
            uint32_t digit = uint32_t(ch);
            if (hex) {
              if (!(Ctype(ch) & 0x80)) break;
              if (int64)
                num64 <<= 4;
              else
                number <<= 4;
              if (!IsDigit(ch)) digit = (digit & ~0x20u) - 7;
              SetCh(int32_t(digit));
            } else {
              if (!IsDigit(ch)) break;
              if (comchr == 'o') {
                if (ch > '7') break;
                if (int64)
                  num64 <<= 3;
                else
                  number <<= 3;
              } else if (int64) {
                num64 *= 10;
              } else {
                number *= 10;
              }
            }
            started++;
            if (int64)
              num64 += uint64_t(int64_t(int32_t(digit - '0')));
            else
              number += digit - '0';
            if (widthset && --width == 0) {
              done = true;
              break;
            }
            SetCh(Inc());
            ch = Ch();
          }
          if (!done) UnInc(Ch());
          if (negative) {
            number = 0 - number;
            num64 = 0 - num64;
          }
          if (!started) {
            ok = false;
            break;
          }
          if (!suppress) {
            count_++;
            assign();
          }
          break;
        }
        default:
          // Anything else matches itself.
          if (R8(f) != uint32_t(Ch())) {
            UnInc(Ch());
            ok = false;
            break;
          }
          match_--;
          if (!suppress) ap_ = saved_ap;
          break;
      }
      if (failed_) return false;
      if (!ok) break;
      match_++;
      f = ++fmt_slot_;
    } else {
      SetCh(Inc());
      const int32_t ch = Ch();
      fmt_slot_ = ++f;
      if (R8(f - 1) != uint32_t(ch)) {
        UnInc(ch);
        break;
      }
      if (Ctype(ch & 0xFF) & 0x8000) {  // a lead byte: its trail too
        const int32_t ch2 = Inc();
        fmt_slot_ = ++f;
        if (R8(f - 1) != uint32_t(ch2)) {
          UnInc(ch2);
          UnInc(ch);
          break;
        }
        charcount_--;
      }
    }
    if (failed_) return false;
    if (Ch() == kEof && (R8(f) != '%' || R8(fmt_slot_ + 1) != 'n')) break;
  }
  Return(Ch());
  return !failed_;
}

}  // namespace

// _input(FILE*, format, va_list).
void _input(Ctx& c, GuestFn original) {
  const uint32_t file = R32(c.esp + 4), fmt = R32(c.esp + 8), ap = R32(c.esp + 12);
  if (!GetLocale().pctype || !GetLocale().decimal_point)
    FatalF("msvc_crt::vc6: _input ran before SetLocale (with decimal_point)");
  if (!(R32(file + 0xC) & kIoStrg) || Multibyte()) return original(c);
  Input in(file);
  if (!in.Run(fmt, ap)) {
    in.Undo();
    return original(c);
  }
  Wr<uint32_t>(g_guest_base, c.esp + 8, in.fmt_slot());
  Wr<uint32_t>(g_guest_base, c.esp + 12, in.ap_slot());
  c.eax = uint32_t(in.result());
  c.esp += 4;
}

// _fltin2(FLT*, string, length, scale, decpt) -> the FLT.
void _fltin2(Ctx& c, GuestFn original) {
  const uint32_t flt = R32(c.esp + 4), s = R32(c.esp + 8);
  if (!GetLocale().pctype || !GetLocale().decimal_point)
    FatalF("msvc_crt::vc6: _fltin2 ran before SetLocale (with decimal_point)");
  if (Multibyte() || (x87::ControlWord(c.fpu) & 0x3F) != 0x3F) return original(c);
  Flt f;
  if (!Fltin2(s, f)) return original(c);
  x87::LoadF64(c.fpu, f.dval);
  const uint64_t d = x87::StoreF64(c.fpu);
  x87::Pop(c.fpu);
  Wr<uint32_t>(g_guest_base, flt, f.flags);
  Wr<uint32_t>(g_guest_base, flt + 4, f.nbytes);
  Wr<uint64_t>(g_guest_base, flt + 0x10, d);
  c.eax = flt;
  c.esp += 4;
}

// atof(string): _fltin2 after the spaces (isspace), the double in ST0. EAX
// is left at atof's own FLT, as the original leaves it.
void atof(Ctx& c, GuestFn original) {
  uint32_t s = R32(c.esp + 4);
  if (!GetLocale().pctype || !GetLocale().decimal_point)
    FatalF("msvc_crt::vc6: atof ran before SetLocale (with decimal_point)");
  if (Multibyte() || (x87::ControlWord(c.fpu) & 0x3F) != 0x3F) return original(c);
  while (IsSpace(R8(s))) s++;
  Flt f;
  if (!Fltin2(s, f)) return original(c);
  x87::LoadF64(c.fpu, f.dval);
  const uint64_t d = x87::StoreF64(c.fpu);
  x87::Pop(c.fpu);
  x87::LoadF64(c.fpu, d);
  c.eax = c.esp - 0x1C;
  c.esp += 4;
}

}  // namespace recomp::native::msvc_crt::vc6
