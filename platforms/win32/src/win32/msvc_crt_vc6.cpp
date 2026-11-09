// Functions of a VC6 CRT whose behaviour is that version's own, natively
// (msvc_crt.cpp has those common to the versions; docs/msvc_crt.md):
// the locale-dependent ones, on VC6's locale globals, and floor/ceil and
// the intrinsics _CIpow/_CIfmod, with the x87 state VC6's code leaves.
//
// The locale-dependent functions: after setlocale(LC_CTYPE, ".<code
// page>") these take the CRT's locale path: the character classes come
// from the ctype table setlocale built (GetStringTypeA), and case mapping
// from LCMapString. Natively: the table is read where the original reads
// it; 'A'-'Z' and 'a'-'z' map to each other, which is what LCMapString
// does for them in every code page; any other character the table marks
// for mapping, a multibyte code page (__mb_cur_max > 1) or a value of 256
// or more is the original's work, which then runs from the start. The
// originals' lock on the locale (__unguarded_readlc_active, or the
// setlocale lock while setlocale runs) is not taken: programs bound to
// these call setlocale before they start other threads, and never again.

#include <cstdint>

#include "runtime/log.h"
#include "win32/msvc_crt.h"
#include "x86/lifted.h"

namespace recomp::native::msvc_crt::vc6 {

namespace {

using lift::Rd;
using lift::Wr;

Locale g_locale;
Math g_math;

uint32_t Arg(const Ctx& c, int n) { return Rd<uint32_t>(g_guest_base, c.esp + 4 + 4 * n); }
uint8_t* P(uint32_t va) { return g_guest_base + va; }

void Ret(Ctx& c, uint32_t v) {
  c.eax = v;
  c.esp += 4;
}

constexpr uint16_t kUpper = 0x1, kLower = 0x2;

const Locale& L() {
  if (!g_locale.pctype) FatalF("msvc_crt::vc6: a locale function ran before SetLocale");
  return g_locale;
}
bool CLocale() { return !Rd<uint32_t>(g_guest_base, L().ctype_handle); }
bool Multibyte() { return int32_t(Rd<uint32_t>(g_guest_base, L().mb_cur_max)) > 1; }
uint16_t Ctype(int32_t ch) {
  return Rd<uint16_t>(g_guest_base, Rd<uint32_t>(g_guest_base, L().pctype) + 2 * uint32_t(ch));
}

// _tolower_lk or, with `upper`, _toupper_lk into *out; false where the
// original's work is needed (above).
bool MapCase(int32_t ch, bool upper, int32_t* out) {
  const int32_t first = upper ? 'a' : 'A', delta = upper ? -0x20 : 0x20;
  const bool letter = ch >= first && ch <= first + 25;
  if (CLocale()) {
    *out = letter ? ch + delta : ch;
    return true;
  }
  if (ch >= 0x100 || Multibyte()) return false;
  if (!(Ctype(ch) & (upper ? kLower : kUpper))) {
    *out = ch;
    return true;
  }
  if (!letter) return false;
  *out = ch + delta;
  return true;
}

// tolower/toupper: in the locale path the original leaves the result in
// its argument's stack slot too.
void ToCase(Ctx& c, bool upper, GuestFn original) {
  int32_t r;
  if (!MapCase(int32_t(Arg(c, 0)), upper, &r)) return original(c);
  if (!CLocale()) Wr<uint32_t>(g_guest_base, c.esp + 4, uint32_t(r));
  Ret(c, uint32_t(r));
}

// _strlwr/_strupr: in place. The original's locale path maps the string
// with LCMapString into a buffer from the program's allocator and copies
// it back; here no buffer is made.
void CaseString(Ctx& c, bool upper, GuestFn original) {
  const uint32_t s = Arg(c, 0);
  if (!CLocale())
    for (const uint8_t* p = P(s); *p; p++)
      if (*p >= 0x80) return original(c);
  const uint8_t first = upper ? 'a' : 'A';
  for (uint8_t* p = P(s); *p; p++)
    if (uint8_t(*p - first) < 26) *p ^= 0x20;
  Ret(c, s);
}

// isalpha and the others: the table's classes for ch, masked.
void IsClass(Ctx& c, uint16_t mask, GuestFn original) {
  if (Multibyte()) return original(c);
  Ret(c, Ctype(int32_t(Arg(c, 0))) & mask);
}

}  // namespace

void SetLocale(const Locale& l) { g_locale = l; }
const Locale& GetLocale() { return g_locale; }
void SetMath(const Math& m) { g_math = m; }
const Math& GetMath() { return g_math; }

// tolower(ch) and toupper(ch).
void tolower(Ctx& c, GuestFn original) { ToCase(c, false, original); }
void toupper(Ctx& c, GuestFn original) { ToCase(c, true, original); }

// _strcmpi(a, b) -> -1, 0 or 1: bytes that differ are compared lowered
// (as bytes).
void _strcmpi(Ctx& c, GuestFn original) {
  const uint8_t *a = P(Arg(c, 0)), *b = P(Arg(c, 1));
  for (;; a++, b++) {
    if (*a == *b) {
      if (!*a) return Ret(c, 0);
      continue;
    }
    int32_t x, y;
    if (!MapCase(*a, false, &x) || !MapCase(*b, false, &y)) return original(c);
    if (uint8_t(x) != uint8_t(y)) return Ret(c, uint8_t(x) < uint8_t(y) ? ~0u : 1);
  }
}

// _strnicmp(a, b, n) -> -1, 0 or 1: at most n bytes, lowered; at a NUL in
// either string the bytes themselves decide.
void _strnicmp(Ctx& c, GuestFn original) {
  const uint8_t *a = P(Arg(c, 0)), *b = P(Arg(c, 1));
  for (uint32_t n = Arg(c, 2); n; n--, a++, b++) {
    if (!*a || !*b) return Ret(c, *a == *b ? 0 : *a < *b ? ~0u : 1);
    int32_t x, y;
    if (!MapCase(*a, false, &x) || !MapCase(*b, false, &y)) return original(c);
    if (x != y) return Ret(c, uint32_t(x) < uint32_t(y) ? ~0u : 1);
  }
  Ret(c, 0);
}

// _memicmp(a, b, n) -> -1, 0 or 1: n bytes, NULs included, those that
// differ compared lowered (as bytes).
void _memicmp(Ctx& c, GuestFn original) {
  const uint8_t *a = P(Arg(c, 0)), *b = P(Arg(c, 1));
  for (uint32_t n = Arg(c, 2); n; n--, a++, b++) {
    if (*a == *b) continue;
    int32_t x, y;
    if (!MapCase(*a, false, &x) || !MapCase(*b, false, &y)) return original(c);
    if (uint8_t(x) != uint8_t(y)) return Ret(c, uint8_t(x) < uint8_t(y) ? ~0u : 1);
  }
  Ret(c, 0);
}

// _strlwr(s) -> s and _strupr(s) -> s.
void _strlwr(Ctx& c, GuestFn original) { CaseString(c, false, original); }
void _strupr(Ctx& c, GuestFn original) { CaseString(c, true, original); }

// isalpha, isdigit, isspace, isalnum, iscntrl: _ALPHA = 0x103, _DIGIT = 4,
// _SPACE = 8, _CONTROL = 0x20.
void isalpha(Ctx& c, GuestFn original) { IsClass(c, 0x103, original); }
void isdigit(Ctx& c, GuestFn original) { IsClass(c, 0x004, original); }
void isspace(Ctx& c, GuestFn original) { IsClass(c, 0x008, original); }
void isalnum(Ctx& c, GuestFn original) { IsClass(c, 0x107, original); }
void iscntrl(Ctx& c, GuestFn original) { IsClass(c, 0x020, original); }

// _isctype(ch, mask): the table for -1..255 (in any code page); the
// original's GetStringTypeA for the rest.
void _isctype(Ctx& c, GuestFn original) {
  const int32_t ch = int32_t(Arg(c, 0));
  if (uint32_t(ch + 1) > 0x100) return original(c);
  Ret(c, Ctype(ch) & Arg(c, 1));
}

// The number parsers. They classify bytes with the table (directly, or
// through _isctype in a multibyte code page, whose answer for a byte is
// the table's), so whatever the code page marks as a space or a digit
// counts: a digit's value is its byte minus '0', whatever the byte.

// atol(s) and atoi(s): spaces, a sign, then decimal digits; no overflow
// check (the value wraps), no errno.
void atol(Ctx& c, GuestFn) {
  const uint8_t* p = P(Arg(c, 0));
  while (Ctype(*p) & 0x08) p++;
  const uint8_t sign = *p;
  if (sign == '-' || sign == '+') p++;
  uint32_t v = 0;
  for (; Ctype(*p) & 0x04; p++) v = v * 10 + *p - '0';
  Ret(c, sign == '-' ? 0 - v : v);
}
void atoi(Ctx& c, GuestFn original) { atol(c, original); }

// strtol(s, end, base) (through strtoxl): spaces, a sign, a 0x/0X prefix
// in base 16 (base 0: 16 after 0x, 8 after another 0, else 10), then
// digits and letters of the base. The original runs for a value that does
// not fit (it sets errno, in the thread's data) and for a byte over 0x7F
// the table marks as a letter (it passes the signed char to toupper).
void strtol(Ctx& c, GuestFn original) {
  const uint32_t s = Arg(c, 0), end = Arg(c, 1);
  uint32_t base = Arg(c, 2);
  const uint8_t* p = P(s);
  uint8_t ch = *p++;
  while (Ctype(ch) & 0x08) ch = *p++;
  const bool neg = ch == '-';
  if (ch == '-' || ch == '+') ch = *p++;
  if (int32_t(base) < 0 || base == 1 || base > 36) {
    if (end) Wr<uint32_t>(g_guest_base, end, s);
    return Ret(c, 0);
  }
  if (base == 0) base = ch != '0' ? 10 : (*p == 'x' || *p == 'X') ? 16 : 8;
  if (base == 16 && ch == '0' && (*p == 'x' || *p == 'X')) {
    ch = p[1];
    p += 2;
  }
  const uint32_t max = 0xFFFFFFFFu / base, max_digit = 0xFFFFFFFFu % base;
  uint32_t v = 0;
  bool any = false;
  for (;; ch = *p++) {
    uint32_t d;
    const uint16_t t = Ctype(ch);
    if (t & 0x04) {
      d = uint32_t(int32_t(int8_t(ch)) - '0');
    } else if (t & 0x103) {
      if (ch >= 0x80) return original(c);
      d = uint32_t((ch & ~0x20) - 0x37);  // toupper's, for a letter 'A'-'Z'
    } else {
      break;
    }
    if (d >= base) break;
    any = true;
    if (v > max || (v == max && d > max_digit)) return original(c);
    v = v * base + d;
  }
  p--;
  if (!any) {
    if (end) Wr<uint32_t>(g_guest_base, end, s);
    return Ret(c, 0);
  }
  if (neg ? v > 0x80000000u : v > 0x7FFFFFFFu) return original(c);
  if (end) Wr<uint32_t>(g_guest_base, end, s + uint32_t(p - P(s)));
  Ret(c, neg ? 0 - v : v);
}

// floor(x) and ceil(x), double in ST0. The original sets its own control
// word (all exceptions masked, 64-bit precision, RC down for floor, up for
// ceil) with _ctrlfp, rounds with frndint, compares the result with x,
// puts the caller's control word back with _ctrlfp (EAX: the control word
// it replaces, sign-extended) and loads the result. The same steps on the
// x87 model leave the same status word (PE for a fraction, DE for a
// denormal x, the compare's condition codes). The original runs for a NaN
// or an infinity (_except1 or _handle_qnan1), for a fraction when the
// caller has the precision exception unmasked (_except1 raises it), when
// the caller has any other exception unmasked, and when the stack has no
// free register.
namespace {

void Round(Ctx& c, uint16_t cw, GuestFn original) {
  x87::State& s = c.fpu;
  const uint16_t saved = x87::ControlWord(s);
  const uint64_t x = uint64_t(Arg(c, 1)) << 32 | Arg(c, 0);
  const bool full = !(s.empty >> ((s.top - 1) & 7) & 1);
  if ((x >> 52 & 0x7FF) == 0x7FF || full || (saved & 0x3F) != 0x3F) return original(c);
  x87::SetControlWord(s, cw);
  const uint16_t own = x87::ControlWord(s);  // as fnstcw reads it back
  x87::LoadF64(s, x);
  x87::RndInt(s);
  const uint64_t r = x87::StoreF64(s);
  x87::Pop(s);
  x87::LoadF64(s, r);
  x87::CompareF64(s, x);
  x87::Pop(s);
  x87::SetControlWord(s, saved);
  x87::LoadF64(s, r);
  c.eax = uint32_t(int32_t(int16_t(own)));
  c.esp += 4;
}

}  // namespace

void floor(Ctx& c, GuestFn original) { Round(c, 0x173F, original); }
void ceil(Ctx& c, GuestFn original) { Round(c, 0x1B3F, original); }

// _CIpow and _CIfmod, the compiler's intrinsics for pow(x, y) and
// fmod(x, y): x in ST1, y in ST0, both popped, the result pushed. The
// natives take the originals' steps on the x87 model, so the registers,
// the status word and EAX are what the originals leave (ECX and EDX are
// not kept). The original runs, from the start, for whatever its error
// and special-case paths handle, for a caller with an exception unmasked,
// and when the stack lacks the registers the original pushes.
namespace {

const Math& M() {
  if (!g_math.matherr_flag) FatalF("msvc_crt::vc6: an intrinsic ran before SetMath");
  return g_math;
}
uint32_t Global(uint32_t va) { return Rd<uint32_t>(g_guest_base, va); }
bool Empty(const x87::State& s, int i) { return s.empty >> ((s.top + i) & 7) & 1; }
bool C3(const x87::State& s) { return s.sw & 0x4000; }

// _twoToTOS: 2^ST0, as frndint, f2xm1 of the fraction and fscale.
void TwoToTos(x87::State& s) {
  x87::LoadSt(s, 0);
  x87::RndInt(s);
  x87::ArithStiSt0(s, x87::Op::SubR, 1);
  x87::Xch(s, 1);
  x87::Chs(s);
  x87::F2xm1(s);
  x87::LoadConst(s, x87::Const::One);
  x87::ArithStiSt0(s, x87::Op::Add, 1);
  x87::Pop(s);
  x87::Scale(s);
  x87::StoreSt(s, 1);
  x87::Pop(s);
}

// Whether ST0 is an integer: a copy rounded with frndint, compared with
// it and popped (`pops` 2: ST0 popped too, fcompp). The low half of EAX
// takes the status word, as the original's fnstsw ax.
bool IsInteger(x87::State& s, int pops, uint32_t& eax) {
  x87::LoadSt(s, 0);
  x87::RndInt(s);
  x87::CompareSt(s, 1, false);
  while (pops--) x87::Pop(s);
  eax = (eax & 0xFFFF0000) | x87::StatusWord(s);
  return C3(s);
}

}  // namespace

// _CIpow stores x and y as doubles (under the caller's control word),
// sets the caller's precision with round to nearest and all exceptions
// masked (unless the control word is 0x27F already), loads x back and
// takes y * log2(x) (fyl2x, on the register y, not its double) to the
// power of two. A negative x needs an integer y (odd: the result
// negated). Then, unless _fastflag is set, it stores the result as a
// double and checks its exponent. EAX: the double result's exponent bits
// with 0x27F (that control word) or 0x20 (another one) in the low half;
// with _fastflag, x's exponent bits (x denormal: its significand bits
// ORed) with, for a negative x, the status word from its last integer
// check in the low half. The original runs for an infinite or NaN x or y
// as doubles, a zero x, a negative x with a fractional y, and, without
// _fastflag, a result that underflows or overflows as a double. It needs
// two free registers.
void _CIpow(Ctx& c, GuestFn original) {
  x87::State& s = c.fpu;
  const x87::State entry = s;
  const uint16_t saved = x87::ControlWord(s);
  if ((saved & 0x3F) != 0x3F || Empty(s, 0) || Empty(s, 1) || !Empty(s, 6) || !Empty(s, 7))
    return original(c);
  const bool fast = Global(M().fastflag);
  auto bail = [&] {
    s = entry;
    original(c);
  };
  x87::Xch(s, 1);
  const uint64_t x = x87::StoreF64(s);
  x87::Pop(s);
  const uint64_t y = x87::StoreF64(s);
  if (saved != 0x27F) x87::SetControlWord(s, (saved & 0x300) | 0x7F);
  const uint32_t xhi = uint32_t(x >> 32);
  if ((y >> 52 & 0x7FF) == 0x7FF || (x >> 52 & 0x7FF) == 0x7FF || !(x << 1)) return bail();
  x87::LoadF64(s, x);
  uint32_t eax = xhi & 0x7FF00000 ? xhi & 0x7FF00000 : (xhi & 0xFFFFF) | uint32_t(x);
  bool odd = false;
  if (x >> 63) {
    x87::LoadSt(s, 1);
    if (!IsInteger(s, 1, eax)) return bail();
    x87::ArithF64(s, x87::Op::Mul, 0x3FE0000000000000);  // 0.5
    odd = !IsInteger(s, 2, eax);
    x87::Chs(s);
  }
  x87::Yl2x(s);
  TwoToTos(s);
  if (odd) x87::Chs(s);
  if (!fast) {
    const uint32_t e = uint32_t(x87::StoreF64(s) >> 32) & 0x7FF00000;
    if (e == 0 || e == 0x7FF00000) return bail();
    eax = e | (saved == 0x27F ? 0x27F : 0x20);
  }
  if (saved != 0x27F) x87::SetControlWord(s, saved);
  Ret(c, eax);
}

// _CIfmod (through _cintrindisp2 and _trandisp2) sets the control word
// 0x133F (64-bit precision, round to nearest, all exceptions masked),
// classifies y and x with fxam (both normal: the fmod core; the rest are
// its table's special cases), then repeats fprem until it is complete.
// Unless _fastflag is set it stores the result as a double (its flags
// stay), and it puts the caller's control word back. Without _matherr_flag it first
// stores x and y as doubles too. EAX: 0x20, or with _fastflag the status
// word of the last fprem. The original runs for any other class of x or
// y and with _adjust_fdiv set.
void _CIfmod(Ctx& c, GuestFn original) {
  x87::State& s = c.fpu;
  const x87::State entry = s;
  const uint16_t saved = x87::ControlWord(s);
  if ((saved & 0x3F) != 0x3F || Empty(s, 0) || Empty(s, 1) || Global(M().adjust_fdiv) == 1)
    return original(c);
  if (!Global(M().matherr_flag)) {
    x87::Xch(s, 1);
    x87::StoreF64(s);
    x87::Xch(s, 1);
    x87::StoreF64(s);
  }
  x87::SetControlWord(s, 0x133F);
  auto normal = [&] {
    x87::Examine(s);
    return (s.sw & 0x4500) == 0x0400;
  };
  const bool y_normal = normal();
  x87::Xch(s, 1);
  const bool x_normal = normal();
  if (!y_normal || !x_normal) {
    s = entry;
    return original(c);
  }
  do x87::Prem(s);
  while (s.sw & 0x0400);
  uint32_t eax = x87::StatusWord(s);
  x87::StoreSt(s, 1);
  x87::Pop(s);
  if (!Global(M().fastflag)) {
    x87::StoreF64(s);
    eax = 0x20;
  }
  x87::SetControlWord(s, saved);
  Ret(c, eax);
}

}  // namespace recomp::native::msvc_crt::vc6
