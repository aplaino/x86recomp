// Functions of a statically linked MSVC CRT, natively: what a program's
// manifest binds with `native <va> msvc_crt::<name>` (ARCHITECTURE.md,
// "Shared native functions"). Their behaviour, the versions they were
// checked against and what each keeps of the original: docs/msvc_crt.md.
// recomp_add_msvc_crt_oracle checks each against the program's own copy.
//
// They work on guest memory in place. A fault on an uncommitted guest page
// reaches the program's exception filters as it does from lifted code, and
// the faulting access is retried. Byte arguments (the character of memset,
// strchr, memchr) are the low byte of their int, as in the originals;
// comparisons are of unsigned chars and return -1, 0 or 1 (strcmp,
// strncmp).

#include <cstdint>
#include <cstring>

#include "x86/lifted.h"

namespace recomp::native::msvc_crt {

namespace {

using lift::Rd;
using lift::Wr;

uint32_t Arg(const Ctx& c, int n) { return Rd<uint32_t>(g_guest_base, c.esp + 4 + 4 * n); }
uint8_t* P(uint32_t va) { return g_guest_base + va; }
const char* S(uint32_t va) { return reinterpret_cast<const char*>(g_guest_base + va); }
uint16_t W(uint32_t va) { return Rd<uint16_t>(g_guest_base, va); }

// A cdecl function returning v: the caller pops the arguments.
void Ret(Ctx& c, uint32_t v) {
  c.eax = v;
  c.esp += 4;
}

// The 64-bit helpers: two 64-bit arguments (low dword first), popped by
// the callee (ret 16); the result in EDX:EAX.
uint64_t Arg64(const Ctx& c, int n) { return uint64_t(Arg(c, 2 * n + 1)) << 32 | Arg(c, 2 * n); }
void Ret64(Ctx& c, uint64_t v) {
  c.eax = uint32_t(v);
  c.edx = uint32_t(v >> 32);
  c.esp += 4 + 16;
}

int Sign(int d) { return (d > 0) - (d < 0); }

// The address of hit in the guest string at s, or 0 for none.
uint32_t At(uint32_t s, const void* hit) {
  return hit ? s + uint32_t(static_cast<const uint8_t*>(hit) - P(s)) : 0;
}

}  // namespace

// memset(dst, c, n) -> dst.
void memset(Ctx& c, GuestFn) {
  const uint32_t dst = Arg(c, 0);
  std::memset(P(dst), uint8_t(Arg(c, 1)), Arg(c, 2));
  Ret(c, dst);
}

// memcpy(dst, src, n) -> dst. The CRT builds memcpy from memmove's
// source: overlapping copies are as memmove's.
void memcpy(Ctx& c, GuestFn) {
  const uint32_t dst = Arg(c, 0);
  std::memmove(P(dst), P(Arg(c, 1)), Arg(c, 2));
  Ret(c, dst);
}

// memmove(dst, src, n) -> dst.
void memmove(Ctx& c, GuestFn) {
  const uint32_t dst = Arg(c, 0);
  std::memmove(P(dst), P(Arg(c, 1)), Arg(c, 2));
  Ret(c, dst);
}

// memchr(buf, c, n) -> the first byte equal to c, or 0.
void memchr(Ctx& c, GuestFn) {
  const uint32_t buf = Arg(c, 0);
  Ret(c, At(buf, std::memchr(P(buf), uint8_t(Arg(c, 1)), Arg(c, 2))));
}

// strlen(s).
void strlen(Ctx& c, GuestFn) { Ret(c, uint32_t(std::strlen(S(Arg(c, 0))))); }

// strcpy(dst, src) -> dst and strcat(dst, src) -> dst (strcat.asm, whose
// code also serves _mbscpy and _mbscat).
void strcpy(Ctx& c, GuestFn) {
  const uint32_t dst = Arg(c, 0), src = Arg(c, 1);
  std::memmove(P(dst), P(src), std::strlen(S(src)) + 1);
  Ret(c, dst);
}
void strcat(Ctx& c, GuestFn) {
  const uint32_t dst = Arg(c, 0), src = Arg(c, 1);
  const uint32_t end = dst + uint32_t(std::strlen(S(dst)));
  std::memmove(P(end), P(src), std::strlen(S(src)) + 1);
  Ret(c, dst);
}

// strncpy(dst, src, n) -> dst: src up to its NUL or n bytes, the rest of
// the n bytes zeroed.
void strncpy(Ctx& c, GuestFn) {
  const uint32_t dst = Arg(c, 0), src = Arg(c, 1), n = Arg(c, 2);
  const uint32_t len = uint32_t(::strnlen(S(src), n));
  std::memmove(P(dst), P(src), len);
  std::memset(P(dst + len), 0, n - len);
  Ret(c, dst);
}

// strncat(dst, src, n) -> dst: at most n bytes of src, then a NUL;
// nothing written when n is 0.
void strncat(Ctx& c, GuestFn) {
  const uint32_t dst = Arg(c, 0), src = Arg(c, 1), n = Arg(c, 2);
  if (n) {
    const uint32_t end = dst + uint32_t(std::strlen(S(dst)));
    const uint32_t len = uint32_t(::strnlen(S(src), n));
    std::memmove(P(end), P(src), len);
    P(end)[len] = 0;
  }
  Ret(c, dst);
}

// strcmp(a, b) -> -1, 0 or 1.
void strcmp(Ctx& c, GuestFn) { Ret(c, uint32_t(Sign(std::strcmp(S(Arg(c, 0)), S(Arg(c, 1)))))); }

// strncmp(a, b, n) -> -1, 0 or 1.
void strncmp(Ctx& c, GuestFn) {
  Ret(c, uint32_t(Sign(std::strncmp(S(Arg(c, 0)), S(Arg(c, 1)), Arg(c, 2)))));
}

// strchr(s, c) -> the first c in s, its NUL included, or 0.
void strchr(Ctx& c, GuestFn) {
  const uint32_t s = Arg(c, 0);
  Ret(c, At(s, std::strchr(S(s), char(Arg(c, 1)))));
}

// strrchr(s, c) -> the last c in s, its NUL included, or 0.
void strrchr(Ctx& c, GuestFn) {
  const uint32_t s = Arg(c, 0);
  Ret(c, At(s, std::strrchr(S(s), char(Arg(c, 1)))));
}

// strstr(s, sub) -> the first sub in s (s itself for an empty sub), or 0.
void strstr(Ctx& c, GuestFn) {
  const uint32_t s = Arg(c, 0);
  Ret(c, At(s, std::strstr(S(s), S(Arg(c, 1)))));
}

// strcspn(s, set) and strpbrk(s, set) -> the first byte of s in set, or 0.
void strcspn(Ctx& c, GuestFn) { Ret(c, uint32_t(std::strcspn(S(Arg(c, 0)), S(Arg(c, 1))))); }
void strpbrk(Ctx& c, GuestFn) {
  const uint32_t s = Arg(c, 0);
  Ret(c, At(s, std::strpbrk(S(s), S(Arg(c, 1)))));
}

// The wide-character functions (16-bit wchar_t), as the CRT's wcs*.c.

// wcslen(s).
void wcslen(Ctx& c, GuestFn) {
  const uint32_t s = Arg(c, 0);
  uint32_t p = s;
  while (W(p)) p += 2;
  Ret(c, (p - s) / 2);
}

// wcscpy(dst, src) -> dst: unit by unit, front to back.
void wcscpy(Ctx& c, GuestFn) {
  const uint32_t dst = Arg(c, 0);
  uint32_t d = dst, s = Arg(c, 1);
  uint16_t ch;
  do {
    ch = W(s);
    Wr<uint16_t>(g_guest_base, d, ch);
    s += 2, d += 2;
  } while (ch);
  Ret(c, dst);
}

// wcsncpy(dst, src, n) -> dst: src up to its NUL or n units, the rest of
// the n units zeroed.
void wcsncpy(Ctx& c, GuestFn) {
  const uint32_t dst = Arg(c, 0);
  uint32_t d = dst, s = Arg(c, 1), n = Arg(c, 2);
  for (; n; n--, d += 2) {
    const uint16_t ch = W(s);
    Wr<uint16_t>(g_guest_base, d, ch);
    if (!ch) break;
    s += 2;
  }
  // The NUL is copied and counted; the units after it are zeroed.
  if (n) {
    for (n--, d += 2; n; n--, d += 2) Wr<uint16_t>(g_guest_base, d, 0);
  }
  Ret(c, dst);
}

// wcschr(s, c) -> the first c in s, its NUL included, or 0.
void wcschr(Ctx& c, GuestFn) {
  uint32_t p = Arg(c, 0);
  const uint16_t ch = uint16_t(Arg(c, 1));
  while (W(p) && W(p) != ch) p += 2;
  Ret(c, W(p) == ch ? p : 0);
}

// wcsrchr(s, c) -> the last c in s, its NUL included, or 0.
void wcsrchr(Ctx& c, GuestFn) {
  const uint32_t s = Arg(c, 0);
  const uint16_t ch = uint16_t(Arg(c, 1));
  uint32_t p = s;
  while (W(p)) p += 2;
  while (p != s && W(p) != ch) p -= 2;
  Ret(c, W(p) == ch ? p : 0);
}

// wcscspn(s, set).
void wcscspn(Ctx& c, GuestFn) {
  const uint32_t s = Arg(c, 0), set = Arg(c, 1);
  uint32_t p = s;
  for (; W(p); p += 2) {
    uint32_t q = set;
    while (W(q) && W(q) != W(p)) q += 2;
    if (W(q)) break;
  }
  Ret(c, (p - s) / 2);
}

// The compiler's 64-bit integer helpers.

// _allmul(a, b) -> a * b, mod 2^64.
void _allmul(Ctx& c, GuestFn) { Ret64(c, Arg64(c, 0) * Arg64(c, 1)); }

// _aulldiv(a, b) -> a / b and _aullrem(a, b) -> a % b, unsigned. A zero
// divisor faults in the original's div; the original runs then, to raise
// it from there.
void _aulldiv(Ctx& c, GuestFn original) {
  const uint64_t b = Arg64(c, 1);
  if (!b) return original(c);
  Ret64(c, Arg64(c, 0) / b);
}
void _aullrem(Ctx& c, GuestFn original) {
  const uint64_t b = Arg64(c, 1);
  if (!b) return original(c);
  Ret64(c, Arg64(c, 0) % b);
}

// _allshl: EDX:EAX <<= CL, 0 from 64 on. Register arguments, nothing on
// the stack.
void _allshl(Ctx& c, GuestFn) {
  const uint32_t n = c.ecx & 0xFF;
  const uint64_t v = n < 64 ? (uint64_t(c.edx) << 32 | c.eax) << n : 0;
  c.eax = uint32_t(v);
  c.edx = uint32_t(v >> 32);
  c.esp += 4;
}

// _alloca_probe / _chkstk: allocates EAX bytes of stack for its caller.
// It touches each page from the top down (Windows commits a stack a guard
// page at a time), returns with ESP lowered by EAX, ECX kept and the
// return address in EAX. Its own pushes are kept too: ECX at the old
// ESP-4, the return address just below the new ESP.
void _alloca_probe(Ctx& c, GuestFn) {
  const uint32_t ret = Rd<uint32_t>(g_guest_base, c.esp);
  const uint32_t top = c.esp + 4, bottom = top - c.eax;
  Wr<uint32_t>(g_guest_base, c.esp - 4, c.ecx);
  for (uint32_t left = c.eax, p = top; left >= 0x1000; left -= 0x1000) {
    p -= 0x1000;
    (void)*reinterpret_cast<volatile uint32_t*>(P(p));
  }
  (void)*reinterpret_cast<volatile uint32_t*>(P(bottom));
  Wr<uint32_t>(g_guest_base, bottom - 4, ret);
  c.eax = ret;
  c.esp = bottom;
}

// _ftol: ST0 to a 64-bit integer, truncated, popped; EDX:EAX. The
// original stores it with fistp under RC=chop and puts the control word
// back; the x87 model does the same, so out-of-range values, NaNs and the
// exception flags are the hardware's.
void _ftol(Ctx& c, GuestFn) {
  x87::State& s = c.fpu;
  const uint16_t cw = x87::ControlWord(s);
  x87::SetControlWord(s, cw | 0x0C00);
  const int64_t v = x87::StoreI64(s);
  x87::Pop(s);
  x87::SetControlWord(s, cw);
  c.eax = uint32_t(v);
  c.edx = uint32_t(uint64_t(v) >> 32);
  c.esp += 4;
}

}  // namespace recomp::native::msvc_crt
