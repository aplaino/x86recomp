#pragma once
// Everything generated (lifted) code needs: AArch64 integer semantics,
// guest memory access and the runtime entry points it calls. Included by
// every file recomp_lift_a64 writes. Portable C++ only (CLAUDE.md product
// constraints); __builtin_* and __int128 are compiler features available
// on every 64-bit host clang and GCC target, not CPU intrinsics.
//
// RECOMP_A64_MUTATION=<n> plants a known bug (see Mutated below) so the
// differential tests can show they catch it; never set it elsewhere.

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "a64/ctx.h"

#ifndef RECOMP_A64_MUTATION
#define RECOMP_A64_MUTATION 0
#endif

namespace recomp::a64 {

using GuestFn = void (*)(Ctx&);

// Emitted by the lifter into generated/table.cpp.
struct LiftedFn {
  uint64_t va;
  GuestFn fn;
};
extern const LiftedFn kLiftedFunctions[];
extern const uint32_t kLiftedFunctionCount;
// TextHash() of the module the table was lifted from.
extern const uint64_t kLiftedTextHash;
// Hands that table to the runtime (a64/dispatch.cpp), which as a shared
// library cannot refer to it; called from a64/lifted_table.cpp, which
// recomp_add_lifted_a64 compiles into the game's lifted code.
void RegisterLiftedFunctions(const LiftedFn* fns, uint32_t count, uint64_t text_hash);

// Runtime services called from lifted code. `va` is always the guest
// address of the instruction making the call, for diagnostics.
void Dispatch(Ctx& c, uint64_t target, uint64_t va);  // BLR, BR
// The guest OS's kernel: defined by the platform module (the Switch's in
// platforms/horizon/src/horizon/svc.cpp), never by this runtime.
void Svc(Ctx& c, uint32_t imm, uint64_t va);
[[noreturn]] void Unimplemented(Ctx& c, uint64_t va, const char* what);
[[noreturn]] void BadJump(Ctx& c, uint64_t va, uint64_t target);
[[noreturn]] void Brk(Ctx& c, uint64_t va, uint32_t imm);

namespace lift {

constexpr bool Mutated(int n) { return RECOMP_A64_MUTATION == n; }

// ---- flags --------------------------------------------------------------
struct Nzcv {
  bool n, z, c, v;
};
[[gnu::always_inline]] inline uint32_t Pack(const Nzcv& f) {
  return (f.n ? 1u << 31 : 0) | (f.z ? 1u << 30 : 0) | (f.c ? 1u << 29 : 0) |
         (f.v ? 1u << 28 : 0);
}
[[gnu::always_inline]] inline Nzcv Unpack(uint64_t v) {
  return {(v >> 31 & 1) != 0, (v >> 30 & 1) != 0, (v >> 29 & 1) != 0, (v >> 28 & 1) != 0};
}

// AddWithCarry() of the Arm ARM: the sum and, if `f`, the flags. SUB is
// a + ~b + 1, SBC a + ~b + C.
template <typename T>
[[gnu::always_inline]] inline T Adc(Nzcv* f, T a, T b, bool carry) {
  T r = T(a + b + T(carry));
  if (f) {
    f->n = r >> (sizeof(T) * 8 - 1);
    f->z = r == 0;
    f->c = carry ? r <= a : r < a;
    if constexpr (Mutated(1)) f->c = r < a;
    f->v = ((a ^ r) & (b ^ r)) >> (sizeof(T) * 8 - 1);
  }
  return r;
}

// ---- shifts and extends (amounts are constants or masked by the caller) --
template <typename T>
[[gnu::always_inline]] inline T Lsl(T v, unsigned s) {
  return s ? T(v << s) : v;
}
template <typename T>
[[gnu::always_inline]] inline T Lsr(T v, unsigned s) {
  return s ? T(v >> s) : v;
}
template <typename T>
[[gnu::always_inline]] inline T Asr(T v, unsigned s) {
  using S = std::make_signed_t<T>;
  if constexpr (Mutated(2)) return T(v >> s);
  return T(S(v) >> s);
}
template <typename T>
[[gnu::always_inline]] inline T Ror(T v, unsigned s) {
  constexpr unsigned kBits = sizeof(T) * 8;
  return s ? T((v >> s) | (v << (kBits - s))) : v;
}

// ExtendReg(): option 0-7 is UXTB UXTH UXTW UXTX SXTB SXTH SXTW SXTX.
[[gnu::always_inline]] inline uint64_t Extend(uint64_t v, unsigned option, unsigned shift) {
  switch (option) {
    case 0: v = uint8_t(v); break;
    case 1: v = uint16_t(v); break;
    case 2: v = uint32_t(v); break;
    case 4: v = uint64_t(int64_t(int8_t(v))); break;
    case 5: v = uint64_t(int64_t(int16_t(v))); break;
    case 6: v = uint64_t(int64_t(int32_t(v))); break;
    default: break;
  }
  return v << shift;
}

// ---- bitfield moves (immr, imms are lifter constants: this folds) --------
// DecodeBitMasks() for the bitfield instructions (immediate = false).
template <typename T>
constexpr T BitfieldWMask(unsigned immr, unsigned imms) {
  constexpr unsigned kBits = sizeof(T) * 8;
  T welem = imms + 1 >= kBits ? T(~T(0)) : T((T(1) << (imms + 1)) - 1);
  return Ror<T>(welem, immr % kBits);
}
template <typename T>
constexpr T BitfieldTMask(unsigned immr, unsigned imms) {
  constexpr unsigned kBits = sizeof(T) * 8;
  unsigned d = (imms - immr) & (kBits - 1);
  return d + 1 >= kBits ? T(~T(0)) : T((T(1) << (d + 1)) - 1);
}
template <typename T>
[[gnu::always_inline]] inline T Ubfm(T src, unsigned immr, unsigned imms) {
  return Ror<T>(src, immr) & BitfieldWMask<T>(immr, imms) & BitfieldTMask<T>(immr, imms);
}
template <typename T>
[[gnu::always_inline]] inline T Sbfm(T src, unsigned immr, unsigned imms) {
  T wm = BitfieldWMask<T>(immr, imms), tm = BitfieldTMask<T>(immr, imms);
  T bot = Ror<T>(src, immr) & wm;
  T top = (src >> imms) & 1 ? T(~T(0)) : T(0);
  if constexpr (Mutated(3)) return bot & tm;
  return (top & ~tm) | (bot & tm);
}
template <typename T>
[[gnu::always_inline]] inline T Bfm(T dst, T src, unsigned immr, unsigned imms) {
  T wm = BitfieldWMask<T>(immr, imms), tm = BitfieldTMask<T>(immr, imms);
  T bot = (dst & ~wm) | (Ror<T>(src, immr) & wm);
  return (dst & ~tm) | (bot & tm);
}

// ---- one- and two-source data processing ---------------------------------
template <typename T>
[[gnu::always_inline]] inline T Clz(T v) {
  if (!v) return T(sizeof(T) * 8);
  if constexpr (sizeof(T) == 8) return T(__builtin_clzll(v));
  return T(__builtin_clz(v));
}
// CLS: CountLeadingZeroBits(x[N-1:1] EOR x[N-2:0]), an N-1 bit value.
template <typename T>
[[gnu::always_inline]] inline T Cls(T v) {
  return T(Clz<T>(T(T(v ^ T(v << 1)) >> 1)) - 1);
}
template <typename T>
[[gnu::always_inline]] inline T Rbit(T v) {
  if constexpr (sizeof(T) == 8) return __builtin_bitreverse64(v);
  return __builtin_bitreverse32(v);
}
// REV16 / REV32 / REV: byte reversal within 2-, 4- or 8-byte containers.
template <typename T>
[[gnu::always_inline]] inline T Rev(T v, unsigned container) {
  T r = 0;
  for (unsigned base = 0; base < sizeof(T); base += container)
    for (unsigned i = 0; i < container; i++)
      r |= T((v >> ((base + i) * 8)) & 0xFF) << ((base + container - 1 - i) * 8);
  return r;
}
template <typename T>
[[gnu::always_inline]] inline T Udiv(T a, T b) {
  return b ? T(a / b) : T(0);
}
template <typename T>
[[gnu::always_inline]] inline T Sdiv(T a, T b) {
  using S = std::make_signed_t<T>;
  if (!b) return 0;
  if (S(a) == S(T(1) << (sizeof(T) * 8 - 1)) && S(b) == -1) return a;  // overflow wraps
  return T(S(a) / S(b));
}
[[gnu::always_inline]] inline uint64_t Umulh(uint64_t a, uint64_t b) {
  return uint64_t((unsigned __int128)a * b >> 64);
}
[[gnu::always_inline]] inline uint64_t Smulh(uint64_t a, uint64_t b) {
  if constexpr (Mutated(4)) return Umulh(a, b);
  return uint64_t((unsigned __int128)((__int128)int64_t(a) * int64_t(b)) >> 64);
}

// ---- memory: guest addresses are host addresses ---------------------------
template <typename T>
[[gnu::always_inline]] inline T Ld(uint64_t a) {
  T v;
  std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
  return v;
}
template <typename T>
[[gnu::always_inline]] inline void St(uint64_t a, T v) {
  std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T));
}
// LDAR / STLR. Guest atomics are naturally aligned, as the architecture
// requires of them.
template <typename T>
[[gnu::always_inline]] inline T LdAcq(uint64_t a) {
  return __atomic_load_n(reinterpret_cast<T*>(a), __ATOMIC_ACQUIRE);
}
template <typename T>
[[gnu::always_inline]] inline void StRel(uint64_t a, T v) {
  __atomic_store_n(reinterpret_cast<T*>(a), v, __ATOMIC_RELEASE);
}
// LDXR / LDAXR, STXR / STLXR (see Ctx). The compare-and-swap is
// sequentially consistent, which covers both orderings.
template <typename T>
[[gnu::always_inline]] inline T Ldxr(Ctx& c, uint64_t a, bool acquire) {
  T v = acquire ? __atomic_load_n(reinterpret_cast<T*>(a), __ATOMIC_ACQUIRE)
                : __atomic_load_n(reinterpret_cast<T*>(a), __ATOMIC_RELAXED);
  c.excl_addr = a;
  c.excl_value = v;
  c.excl_valid = true;
  return v;
}
template <typename T>
[[gnu::always_inline]] inline uint32_t Stxr(Ctx& c, uint64_t a, T v) {
  bool ok = c.excl_valid && c.excl_addr == a;
  c.excl_valid = false;
  if (!ok) return 1;
  T expected = T(c.excl_value);
  return __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, v, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
             ? 0
             : 1;
}
[[gnu::always_inline]] inline void Clrex(Ctx& c) { c.excl_valid = false; }
// DMB/DSB: ISH, ISHLD and the rest all become a full fence; ISB orders
// nothing a lifted program can observe beyond that.
[[gnu::always_inline]] inline void Fence() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

}  // namespace lift
}  // namespace recomp::a64
