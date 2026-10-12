#pragma once
// Everything generated (lifted) code needs: guest memory access, x86 flag
// semantics, and the runtime entry points it calls. Included by every file
// in generated/. Portable C++ only (see CLAUDE.md product constraints);
// __builtin_* used here are compiler builtins, not x86 intrinsics.

#include <cstdint>
#include <cstring>

#include "runtime/ctx.h"
#include "runtime/sse.h"
#include "runtime/x87.h"

namespace recomp {

extern uint8_t* g_guest_base;

using GuestFn = void (*)(Ctx&);

// Emitted by the lifter into generated/table.cpp.
struct LiftedFn {
  uint32_t va;
  GuestFn fn;
};
extern const LiftedFn kLiftedFunctions[];
extern const uint32_t kLiftedFunctionCount;

// Thrown at manifest `resume` sites: the guest abandons the current call
// chain and continues at `target` in the frame whose EBP is `ebp` (SEH
// __except blocks). Caught by the lifted function owning that frame.
struct GuestResume {
  uint32_t target, ebp, from;
};

// Runtime services called from lifted code (runtime/dispatch.cpp).
// `va` is always the guest address of the instruction doing the call, for
// diagnostics.
void Dispatch(Ctx& c, uint32_t target, uint32_t va);
[[noreturn]] void Unimplemented(Ctx& c, uint32_t va, const char* what);
[[noreturn]] void BadJump(Ctx& c, uint32_t va, uint32_t target);
[[noreturn]] void Unreachable(Ctx& c, uint32_t va);
void Trap(Ctx& c, uint32_t va);                // int3 / int n
void RaiseDivideError(Ctx& c, uint32_t va);    // #DE from div/idiv
void Cpuid(Ctx& c);                            // eax/ecx in, eax..edx out
uint64_t Rdtsc(Ctx& c);
void Probe(Ctx& c, uint32_t va, const char* name);  // manifest probes

namespace lift {

// ---- memory -----------------------------------------------------------
template <typename T>
inline T Rd(const uint8_t* b, uint32_t a) {
  T v;
  std::memcpy(&v, b + a, sizeof(T));
  return v;
}
template <typename T>
inline void Wr(uint8_t* b, uint32_t a, T v) {
  std::memcpy(b + a, &v, sizeof(T));
}
inline x87::F80 RdF80(const uint8_t* b, uint32_t a) {
  x87::F80 v;
  std::memcpy(&v.sig, b + a, 8);
  std::memcpy(&v.se, b + a + 8, 2);
  return v;
}
inline void WrF80(uint8_t* b, uint32_t a, x87::F80 v) {
  std::memcpy(b + a, &v.sig, 8);
  std::memcpy(b + a + 8, &v.se, 2);
}

// ---- widths -------------------------------------------------------------
template <int N>
struct W;
template <>
struct W<1> {
  static constexpr uint32_t kMask = 0xFFu, kSign = 0x80u, kBits = 8;
};
template <>
struct W<2> {
  static constexpr uint32_t kMask = 0xFFFFu, kSign = 0x8000u, kBits = 16;
};
template <>
struct W<4> {
  static constexpr uint32_t kMask = 0xFFFFFFFFu, kSign = 0x80000000u,
                            kBits = 32;
};

template <int N>
inline int32_t Sext(uint32_t v) {
  if constexpr (N == 1) return static_cast<int8_t>(v);
  if constexpr (N == 2) return static_cast<int16_t>(v);
  return static_cast<int32_t>(v);
}

inline bool Parity(uint32_t v) { return !__builtin_parity(v & 0xFFu); }

template <int N>
inline void SetSZP(Flags& f, uint32_t r) {
  r &= W<N>::kMask;
  f.zf = r == 0;
  f.sf = (r & W<N>::kSign) != 0;
  f.pf = Parity(r);
}

// ---- arithmetic (operands are pre-masked to N bytes by the emitter) -----
template <int N>
inline uint32_t Add(Flags& f, uint32_t a, uint32_t b) {
  uint64_t full = uint64_t(a) + b;
  uint32_t r = uint32_t(full) & W<N>::kMask;
  f.cf = full > W<N>::kMask;
  f.of = ((a ^ r) & (b ^ r) & W<N>::kSign) != 0;
  f.af = ((a ^ b ^ r) & 0x10) != 0;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
inline uint32_t Adc(Flags& f, uint32_t a, uint32_t b) {
  uint64_t full = uint64_t(a) + b + (f.cf ? 1 : 0);
  uint32_t r = uint32_t(full) & W<N>::kMask;
  f.cf = full > W<N>::kMask;
  f.of = ((a ^ r) & (b ^ r) & W<N>::kSign) != 0;
  f.af = ((a ^ b ^ r) & 0x10) != 0;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
inline uint32_t Sub(Flags& f, uint32_t a, uint32_t b) {
  uint32_t r = (a - b) & W<N>::kMask;
  f.cf = a < b;
  f.of = ((a ^ b) & (a ^ r) & W<N>::kSign) != 0;
  f.af = ((a ^ b ^ r) & 0x10) != 0;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
inline uint32_t Sbb(Flags& f, uint32_t a, uint32_t b) {
  uint32_t cin = f.cf ? 1 : 0;
  uint32_t r = (a - b - cin) & W<N>::kMask;
  f.cf = uint64_t(a) < uint64_t(b) + cin;
  f.of = ((a ^ b) & (a ^ r) & W<N>::kSign) != 0;
  f.af = ((a ^ b ^ r) & 0x10) != 0;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
inline uint32_t Inc(Flags& f, uint32_t a) {
  bool cf = f.cf;
  uint32_t r = Add<N>(f, a, 1);
  f.cf = cf;
  return r;
}
template <int N>
inline uint32_t Dec(Flags& f, uint32_t a) {
  bool cf = f.cf;
  uint32_t r = Sub<N>(f, a, 1);
  f.cf = cf;
  return r;
}
template <int N>
inline uint32_t Neg(Flags& f, uint32_t a) {
  return Sub<N>(f, 0, a);
}
template <int N>
inline uint32_t Logic(Flags& f, uint32_t r) {
  r &= W<N>::kMask;
  f.cf = f.of = f.af = false;
  SetSZP<N>(f, r);
  return r;
}

// ---- shifts and rotates (count is the raw CL/imm8 value) ----------------
template <int N>
inline uint32_t Shl(Flags& f, uint32_t a, uint32_t cnt) {
  cnt &= 31;
  if (!cnt) return a;
  uint64_t v = uint64_t(a & W<N>::kMask) << cnt;
  uint32_t r = uint32_t(v) & W<N>::kMask;
  f.cf = (v >> W<N>::kBits) & 1;
  f.of = ((r & W<N>::kSign) != 0) != f.cf;
  f.af = false;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
inline uint32_t Shr(Flags& f, uint32_t a, uint32_t cnt) {
  cnt &= 31;
  if (!cnt) return a;
  a &= W<N>::kMask;
  f.cf = cnt <= W<N>::kBits ? (a >> (cnt - 1)) & 1 : 0;
  uint32_t r = cnt >= W<N>::kBits ? 0 : a >> cnt;
  f.of = (a & W<N>::kSign) != 0;
  f.af = false;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
inline uint32_t Sar(Flags& f, uint32_t a, uint32_t cnt) {
  cnt &= 31;
  if (!cnt) return a;
  int32_t s = Sext<N>(a);
  f.cf = (s >> (cnt - 1)) & 1;
  uint32_t r = uint32_t(s >> cnt) & W<N>::kMask;
  f.of = false;
  f.af = false;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
inline uint32_t Rol(Flags& f, uint32_t a, uint32_t cnt) {
  if (!(cnt & 31)) return a;
  uint32_t k = (cnt & 31) % W<N>::kBits;
  a &= W<N>::kMask;
  uint32_t r = k ? ((a << k) | (a >> (W<N>::kBits - k))) & W<N>::kMask : a;
  f.cf = r & 1;
  f.of = ((r & W<N>::kSign) != 0) != f.cf;
  return r;
}
template <int N>
inline uint32_t Ror(Flags& f, uint32_t a, uint32_t cnt) {
  if (!(cnt & 31)) return a;
  uint32_t k = (cnt & 31) % W<N>::kBits;
  a &= W<N>::kMask;
  uint32_t r = k ? ((a >> k) | (a << (W<N>::kBits - k))) & W<N>::kMask : a;
  f.cf = (r & W<N>::kSign) != 0;
  f.of = f.cf != ((r & (W<N>::kSign >> 1)) != 0);
  return r;
}
template <int N>
inline uint32_t Rcl(Flags& f, uint32_t a, uint32_t cnt) {
  uint32_t k = (cnt & 31) % (W<N>::kBits + 1);
  a &= W<N>::kMask;
  for (uint32_t i = 0; i < k; i++) {
    bool out = (a & W<N>::kSign) != 0;
    a = ((a << 1) | (f.cf ? 1 : 0)) & W<N>::kMask;
    f.cf = out;
  }
  if (cnt & 31) f.of = ((a & W<N>::kSign) != 0) != f.cf;
  return a;
}
template <int N>
inline uint32_t Rcr(Flags& f, uint32_t a, uint32_t cnt) {
  uint32_t k = (cnt & 31) % (W<N>::kBits + 1);
  a &= W<N>::kMask;
  if (cnt & 31) f.of = ((a & W<N>::kSign) != 0) != f.cf;
  for (uint32_t i = 0; i < k; i++) {
    bool out = a & 1;
    a = (a >> 1) | (f.cf ? W<N>::kSign : 0);
    f.cf = out;
  }
  return a;
}
template <int N>
inline uint32_t Shld(Flags& f, uint32_t dst, uint32_t src, uint32_t cnt) {
  cnt &= 31;
  if (!cnt) return dst;
  uint64_t v = (uint64_t(dst & W<N>::kMask) << W<N>::kBits) |
               (src & W<N>::kMask);
  f.cf = (v >> (2 * W<N>::kBits - cnt)) & 1;  // last bit shifted out
  v <<= cnt;
  uint32_t r = uint32_t(v >> W<N>::kBits) & W<N>::kMask;
  f.of = ((r ^ dst) & W<N>::kSign) != 0;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
inline uint32_t Shrd(Flags& f, uint32_t dst, uint32_t src, uint32_t cnt) {
  cnt &= 31;
  if (!cnt) return dst;
  uint64_t v = (uint64_t(src & W<N>::kMask) << W<N>::kBits) |
               (dst & W<N>::kMask);
  f.cf = (v >> (cnt - 1)) & 1;
  uint32_t r = uint32_t(v >> cnt) & W<N>::kMask;
  f.of = ((r ^ dst) & W<N>::kSign) != 0;
  SetSZP<N>(f, r);
  return r;
}

// ---- multiply / divide --------------------------------------------------
// Two- and three-operand IMUL: truncated product, CF=OF=overflow.
template <int N>
inline uint32_t Imul2(Flags& f, uint32_t a, uint32_t b) {
  int64_t p = int64_t(Sext<N>(a)) * Sext<N>(b);
  uint32_t r = uint32_t(p) & W<N>::kMask;
  f.cf = f.of = p != Sext<N>(r);
  SetSZP<N>(f, r);
  return r;
}
// One-operand MUL/IMUL. lo/hi are AL:AH, AX:DX or EAX:EDX values.
template <int N>
inline void Mul1(Flags& f, uint32_t a, uint32_t b, uint32_t& lo,
                 uint32_t& hi) {
  uint64_t p = uint64_t(a & W<N>::kMask) * (b & W<N>::kMask);
  lo = uint32_t(p) & W<N>::kMask;
  hi = uint32_t(p >> W<N>::kBits) & W<N>::kMask;
  f.cf = f.of = hi != 0;
  SetSZP<N>(f, lo);
}
template <int N>
inline void Imul1(Flags& f, uint32_t a, uint32_t b, uint32_t& lo,
                  uint32_t& hi) {
  int64_t p = int64_t(Sext<N>(a)) * Sext<N>(b);
  lo = uint32_t(p) & W<N>::kMask;
  hi = uint32_t(uint64_t(p) >> W<N>::kBits) & W<N>::kMask;
  f.cf = f.of = p != Sext<N>(lo);
  SetSZP<N>(f, lo);
}
// Returns false on #DE (divide by zero or quotient overflow).
template <int N>
inline bool Div(uint32_t& lo, uint32_t& hi, uint32_t d) {
  d &= W<N>::kMask;
  if (!d) return false;
  uint64_t n = (uint64_t(hi & W<N>::kMask) << W<N>::kBits) |
               (lo & W<N>::kMask);
  uint64_t q = n / d;
  if (q > W<N>::kMask) return false;
  lo = uint32_t(q);
  hi = uint32_t(n % d);
  return true;
}
template <int N>
inline bool Idiv(uint32_t& lo, uint32_t& hi, uint32_t d) {
  int64_t dv = Sext<N>(d);
  if (!dv) return false;
  int64_t n = int64_t((uint64_t(hi & W<N>::kMask) << W<N>::kBits) |
                      (lo & W<N>::kMask));
  if constexpr (N < 4) {
    constexpr int s = 64 - 2 * W<N>::kBits;  // sign-extend the dividend
    n = int64_t(uint64_t(n) << s) >> s;
  }
  if (n == INT64_MIN && dv == -1) return false;
  int64_t q = n / dv;
  int64_t lim = int64_t(1) << (W<N>::kBits - 1);
  if (q >= lim || q < -lim) return false;
  lo = uint32_t(q) & W<N>::kMask;
  hi = uint32_t(n % dv) & W<N>::kMask;
  return true;
}

// ---- bit ops --------------------------------------------------------------
template <int N>
inline uint32_t Bsf(Flags& f, uint32_t dst, uint32_t src) {
  src &= W<N>::kMask;
  f.zf = src == 0;
  return src ? uint32_t(__builtin_ctz(src)) : dst;
}
template <int N>
inline uint32_t Bsr(Flags& f, uint32_t dst, uint32_t src) {
  src &= W<N>::kMask;
  f.zf = src == 0;
  return src ? uint32_t(31 - __builtin_clz(src)) : dst;
}

// ---- partial registers --------------------------------------------------
inline uint32_t SetLo8(uint32_t r, uint32_t v) {
  return (r & ~0xFFu) | (v & 0xFFu);
}
inline uint32_t SetHi8(uint32_t r, uint32_t v) {
  return (r & ~0xFF00u) | ((v & 0xFFu) << 8);
}
inline uint32_t SetLo16(uint32_t r, uint32_t v) {
  return (r & ~0xFFFFu) | (v & 0xFFFFu);
}

// ---- flags as bytes (sahf/lahf) -----------------------------------------
inline void Sahf(Flags& f, uint32_t ah) {
  f.cf = ah & 0x01;
  f.pf = ah & 0x04;
  f.af = ah & 0x10;
  f.zf = ah & 0x40;
  f.sf = ah & 0x80;
}
inline uint32_t Lahf(const Flags& f) {
  return uint32_t(PackEFlags(f) & 0xD5u) | 0x02u;
}

// ---- atomics for lock-prefixed instructions -----------------------------
template <typename T>
inline T* AtomicPtr(uint8_t* b, uint32_t a) {
  return reinterpret_cast<T*>(b + a);
}

}  // namespace lift
}  // namespace recomp

// Register/flag caching. Lifted functions keep guest registers in locals
// (which nothing can alias) and write them back before anything that may
// observe or change them.
#define LIFT_ENTER                                                       \
  uint8_t* const B = ::recomp::g_guest_base;                                \
  uint32_t eax = c.eax, ecx = c.ecx, edx = c.edx, ebx = c.ebx,           \
           esp = c.esp, ebp = c.ebp, esi = c.esi, edi = c.edi;           \
  ::recomp::Flags F = c.f;                                                  \
  const uint32_t FS = c.fs_base;                                         \
  (void)FS;

// Tail calls between lifted functions (all void(Ctx&)) are guaranteed
// where the compiler supports it.
#if defined(__clang__)
#define LIFT_MUSTTAIL [[clang::musttail]]
#elif defined(__GNUC__) && __GNUC__ >= 15
#define LIFT_MUSTTAIL [[gnu::musttail]]
#else
#define LIFT_MUSTTAIL
#endif

#define LIFT_SPILL                                                       \
  c.eax = eax; c.ecx = ecx; c.edx = edx; c.ebx = ebx;                    \
  c.esp = esp; c.ebp = ebp; c.esi = esi; c.edi = edi; c.f = F;

#define LIFT_RELOAD                                                      \
  eax = c.eax; ecx = c.ecx; edx = c.edx; ebx = c.ebx;                    \
  esp = c.esp; ebp = c.ebp; esi = c.esi; edi = c.edi; F = c.f;
