#pragma once
// Everything generated (lifted) code needs: PowerPC fixed-point semantics,
// big-endian guest memory access and the runtime entry points it calls.
// Included by every file recomp_lift_ppc writes. Portable C++ only
// (CLAUDE.md product constraints); __builtin_* and __int128 are compiler
// features available on every 64-bit host clang and GCC target, not CPU
// intrinsics.
//
// The guest runs in 32-bit mode (MSR[SF] = 0) on a 64-bit CPU, as Xbox 360
// titles do: registers and results are 64-bit, but CR0 of record forms,
// CA and OV of adds and subtracts, the CTR test of branches, branch
// targets and effective addresses come from the low 32 bits (the guest
// arena is 4 GiB). Multiplies and divides set OV as their own width says,
// in any mode.
//
// RECOMP_PPC_MUTATION=<n> plants a known bug (see Mutated below) so the
// differential tests can show they catch it; never set it elsewhere.

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "ppc/ctx.h"

#ifndef RECOMP_PPC_MUTATION
#define RECOMP_PPC_MUTATION 0
#endif

namespace recomp::ppc {

using GuestFn = void (*)(Ctx&);

// Floating-point arithmetic operations (FpArith), shared with the decoder.
enum FpOp : uint8_t {
  kFAdd, kFSub, kFMul, kFDiv, kFSqrt, kFRes, kFRsqrte,
  kFMadd, kFMsub, kFNmadd, kFNmsub,
};

// Emitted by the lifter into generated/table.cpp.
struct LiftedFn {
  uint32_t va;
  GuestFn fn;
};
extern const LiftedFn kLiftedFunctions[];
extern const uint32_t kLiftedFunctionCount;
// TextHash() of the module the table was lifted from.
extern const uint64_t kLiftedTextHash;
// Hands that table to the runtime (ppc/dispatch.cpp), which as a shared
// library cannot refer to it; called from ppc/lifted_table.cpp, which
// recomp_add_lifted_ppc compiles into the game's lifted code.
void RegisterLiftedFunctions(const LiftedFn* fns, uint32_t count, uint64_t text_hash);

// Runtime services called from lifted code. `va` is always the guest
// address of the instruction making the call, for diagnostics.
void Dispatch(Ctx& c, uint64_t target, uint64_t va);  // BCCTR, BCLR to a non-return
// Defined by the platform module, never by this runtime: the system call
// instruction, traps that fire (TW TWI TD TDI; `raw` is the instruction),
// the time base, and calls to an imported function (the lifter emits one
// per import thunk; `module` and `ordinal` name the export).
void Sc(Ctx& c, uint64_t va);
void Trap(Ctx& c, uint32_t raw, uint64_t va);
uint64_t TimeBase(Ctx& c);
void Import(Ctx& c, const char* module, uint32_t ordinal, uint64_t va);
[[noreturn]] void Unimplemented(Ctx& c, uint64_t va, const char* what);
[[noreturn]] void BadJump(Ctx& c, uint64_t va, uint64_t target);

namespace lift {

constexpr bool Mutated(int n) { return RECOMP_PPC_MUTATION == n; }

// ---- condition register and XER ---------------------------------------
constexpr uint32_t kLt = 8, kGt = 4, kEq = 2, kSo = 1;  // a CR field's bits
constexpr uint32_t kXerSo = 1u << 31, kXerOv = 1u << 30, kXerCa = 1u << 29;

// CR field n (0 = CR0, the most significant nibble).
[[gnu::always_inline]] inline uint32_t SetField(uint32_t cr, unsigned n, uint32_t v) {
  unsigned s = 28 - 4 * n;
  return (cr & ~(0xFu << s)) | (v << s);
}
[[gnu::always_inline]] inline uint32_t Field(uint32_t cr, unsigned n) { return (cr >> (28 - 4 * n)) & 0xF; }
// CR bit b in the ISA's numbering (0 = CR0[LT]).
[[gnu::always_inline]] inline bool CrBit(uint32_t cr, unsigned b) { return (cr >> (31 - b)) & 1; }
[[gnu::always_inline]] inline uint32_t SetCrBit(uint32_t cr, unsigned b, bool v) {
  return (cr & ~(1u << (31 - b))) | (uint32_t(v) << (31 - b));
}

// Compare: LT GT EQ, and SO copied from XER.
template <typename T>
[[gnu::always_inline]] inline uint32_t Compare(T a, T b, uint32_t xer) {
  uint32_t v = a < b ? kLt : a > b ? kGt : kEq;
  return v | (xer >> 31);
}
// Record forms (Rc = 1): CR0 from the result's low word compared with 0.
[[gnu::always_inline]] inline uint32_t Record(uint32_t cr, uint64_t r, uint32_t xer) {
  if constexpr (Mutated(1)) return SetField(cr, 0, Compare<int64_t>(int64_t(r), 0, xer));
  return SetField(cr, 0, Compare<int32_t>(int32_t(r), 0, xer));
}
[[gnu::always_inline]] inline uint32_t SetCa(uint32_t xer, bool ca) {
  return (xer & ~kXerCa) | (ca ? kXerCa : 0);
}
// OE = 1: OV from this result, SO sticky.
[[gnu::always_inline]] inline uint32_t SetOv(uint32_t xer, bool ov) {
  return (xer & ~kXerOv) | (ov ? kXerOv | kXerSo : 0);
}
[[gnu::always_inline]] inline bool Ca(uint32_t xer) { return xer & kXerCa; }

// a + b + carry: the 64-bit sum, and the carry out of and the signed
// overflow of its low word (ADD ADDC ADDE ADDME ADDZE ADDIC, and the SUBF
// family as ~a + b + 1).
struct AddResult {
  uint64_t r;
  bool ca, ov;
};
[[gnu::always_inline]] inline AddResult Add(uint64_t a, uint64_t b, bool carry) {
  uint64_t r = a + b + carry;
  uint64_t low = uint64_t(uint32_t(a)) + uint32_t(b) + carry;
  bool ov = ((uint32_t(a) ^ uint32_t(r)) & (uint32_t(b) ^ uint32_t(r))) >> 31;
  if constexpr (Mutated(2)) return {r, r < a, ov};
  return {r, bool(low >> 32), ov};
}

// ---- multiply and divide --------------------------------------------------
// MULLW: the full 64-bit product of the low words; OV if it does not fit
// in 32 bits.
[[gnu::always_inline]] inline uint64_t Mullw(uint64_t a, uint64_t b, bool* ov) {
  int64_t p = int64_t(int32_t(a)) * int32_t(b);
  if (ov) *ov = p != int64_t(int32_t(p));
  return uint64_t(p);
}
[[gnu::always_inline]] inline uint64_t Mulld(uint64_t a, uint64_t b, bool* ov) {
  __int128 p = (__int128)int64_t(a) * int64_t(b);
  if (ov) *ov = p != (__int128)int64_t(p);
  return uint64_t(p);
}
[[gnu::always_inline]] inline uint64_t Mulhd(uint64_t a, uint64_t b) {
  return uint64_t((unsigned __int128)((__int128)int64_t(a) * int64_t(b)) >> 64);
}
[[gnu::always_inline]] inline uint64_t Mulhdu(uint64_t a, uint64_t b) {
  return uint64_t((unsigned __int128)a * b >> 64);
}
// MULHW(U): the high word of the 32x32 product in the low word. The ISA
// leaves the high word undefined; this is what the oracle CPU leaves.
[[gnu::always_inline]] inline uint64_t Mulhw(uint64_t a, uint64_t b) {
  uint32_t h = uint32_t(uint64_t(int64_t(int32_t(a)) * int32_t(b)) >> 32);
  return h;
}
[[gnu::always_inline]] inline uint64_t Mulhwu(uint64_t a, uint64_t b) {
  uint32_t h = uint32_t((uint64_t(uint32_t(a)) * uint32_t(b)) >> 32);
  return h;
}
// DIVW(U), DIVD(U). Division by zero and the most negative value over -1
// set OV; the ISA leaves their quotient (and DIVW's high word) undefined,
// and this returns what the oracle CPU returns.
[[gnu::always_inline]] inline uint64_t Divw(uint64_t a, uint64_t b, bool* ov) {
  int32_t x = int32_t(a), y = int32_t(b);
  bool bad = y == 0 || (x == INT32_MIN && y == -1);
  if (ov) *ov = bad;
  return bad ? 0 : uint32_t(x / y);
}
[[gnu::always_inline]] inline uint64_t Divwu(uint64_t a, uint64_t b, bool* ov) {
  uint32_t x = uint32_t(a), y = uint32_t(b);
  if (ov) *ov = y == 0;
  return y ? x / y : 0;
}
[[gnu::always_inline]] inline uint64_t Divd(uint64_t a, uint64_t b, bool* ov) {
  int64_t x = int64_t(a), y = int64_t(b);
  bool bad = y == 0 || (x == INT64_MIN && y == -1);
  if (ov) *ov = bad;
  return bad ? 0 : uint64_t(x / y);
}
[[gnu::always_inline]] inline uint64_t Divdu(uint64_t a, uint64_t b, bool* ov) {
  if (ov) *ov = b == 0;
  return b ? a / b : 0;
}

// ---- rotates, masks and shifts --------------------------------------------
[[gnu::always_inline]] inline uint64_t Rotl64(uint64_t v, unsigned n) {
  n &= 63;
  return n ? (v << n) | (v >> (64 - n)) : v;
}
// ROTL32: the low word rotated, in both halves (the 32-bit rotates see a
// doubled word, so masks that wrap pick up its copy).
[[gnu::always_inline]] inline uint64_t Rotl32(uint64_t v, unsigned n) {
  uint64_t w = uint32_t(v);
  return Rotl64(w | (w << 32), n & 31);
}
// MASK(mb, me): ones from bit mb to bit me (bit 0 most significant),
// wrapping when mb > me.
constexpr uint64_t Mask(unsigned mb, unsigned me) {
  uint64_t a = ~0ull >> mb, b = ~0ull << (63 - me);
  return mb <= me ? a & b : a | b;
}
[[gnu::always_inline]] inline uint64_t Cntlz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
[[gnu::always_inline]] inline uint64_t Cntlz32(uint64_t v) {
  return uint32_t(v) ? __builtin_clz(uint32_t(v)) : 32;
}
// SLW SRW: 6-bit amounts, 32 and up give 0.
[[gnu::always_inline]] inline uint64_t Slw(uint64_t v, uint64_t n) {
  n &= 63;
  return n > 31 ? 0 : uint32_t(uint32_t(v) << n);
}
[[gnu::always_inline]] inline uint64_t Srw(uint64_t v, uint64_t n) {
  n &= 63;
  return n > 31 ? 0 : uint32_t(v) >> n;
}
// SLD SRD: 7-bit amounts, 64 and up give 0.
[[gnu::always_inline]] inline uint64_t Sld(uint64_t v, uint64_t n) {
  n &= 127;
  return n > 63 ? 0 : v << n;
}
[[gnu::always_inline]] inline uint64_t Srd(uint64_t v, uint64_t n) {
  n &= 127;
  return n > 63 ? 0 : v >> n;
}
// SRAW SRAWI: the low word shifted arithmetically, sign-extended to 64
// bits; CA if it is negative and any 1 bits were shifted out.
[[gnu::always_inline]] inline uint64_t Sraw(uint64_t v, uint64_t n, bool* ca) {
  n &= 63;
  int32_t w = int32_t(v);
  if (n > 31) {
    *ca = w < 0;
    return uint64_t(int64_t(w < 0 ? -1 : 0));
  }
  *ca = w < 0 && (uint32_t(w) & ((1u << n) - 1)) != 0;
  if constexpr (Mutated(3)) *ca = w < 0;
  return uint64_t(int64_t(w >> n));
}
[[gnu::always_inline]] inline uint64_t Srad(uint64_t v, uint64_t n, bool* ca) {
  n &= 127;
  int64_t x = int64_t(v);
  if (n > 63) {
    *ca = x < 0;
    return uint64_t(x < 0 ? -1 : 0);
  }
  *ca = x < 0 && n && (v & ((1ull << n) - 1)) != 0;
  return uint64_t(x >> n);
}

// ---- floating point (ppc/fp.cpp) ------------------------------------------
// FPSCR bits.
constexpr uint32_t kFx = 1u << 31, kFex = 1u << 30, kVx = 1u << 29, kOx = 1u << 28,
                   kUx = 1u << 27, kZx = 1u << 26, kXx = 1u << 25, kVxsnan = 1u << 24,
                   kVxisi = 1u << 23, kVxidi = 1u << 22, kVxzdz = 1u << 21, kVximz = 1u << 20,
                   kVxvc = 1u << 19, kFr = 1u << 18, kFi = 1u << 17, kFprf = 0x1Fu << 12,
                   kFpcc = 0xFu << 12, kVxsoft = 1u << 10, kVxsqrt = 1u << 9, kVxcvi = 1u << 8,
                   kVe = 1u << 7, kOe = 1u << 6, kUe = 1u << 5, kZe = 1u << 4, kXe = 1u << 3,
                   kNi = 1u << 2, kRn = 3u;
// Arithmetic (FpOp): FRT from FRA, FRB, FRC under
// FPSCR[RN], with every FPSCR effect. Single precision rounds the exact
// result once to single, as the ISA defines it for any double operands.
uint64_t FpArith(Ctx& c, unsigned op, bool single, uint64_t a, uint64_t b, uint64_t cc);
uint64_t Frsp(Ctx& c, uint64_t b);
// FCTIW FCTIWZ FCTID FCTIDZ: `mode` bit 0 toward zero, bit 1 doubleword.
uint64_t Fctiw(Ctx& c, unsigned mode, uint64_t b);
uint64_t Fcfid(Ctx& c, uint64_t b);
// FCMPU / FCMPO: the CR field (LT GT EQ UN) and FPSCR[FPCC].
uint32_t Fcmp(Ctx& c, bool ordered, uint64_t a, uint64_t b);
void Mtfsf(Ctx& c, uint32_t flm, uint64_t b);
void Mtfsfi(Ctx& c, unsigned field, unsigned imm);
void Mtfsb(Ctx& c, unsigned bit, bool v);
// MCRFS: FPSCR field `field`, then that field's exception bits cleared.
uint32_t Mcrfs(Ctx& c, unsigned field);
// LFS's and STFS's format conversions: bit-exact, no rounding, NaNs kept.
uint64_t SingleToDouble(uint32_t w);
uint32_t DoubleToSingle(uint64_t d);
[[gnu::always_inline]] inline uint64_t Fsel(uint64_t a, uint64_t b, uint64_t cc) {
  // FRA >= 0 (not a NaN; -0 counts) ? FRC : FRB.
  bool nan = (a & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
  bool ge = !nan && (!(a >> 63) || !(a << 1));
  return ge ? cc : b;
}

// ---- traps ---------------------------------------------------------------
// TO bits: 16 signed <, 8 signed >, 4 =, 2 unsigned <, 1 unsigned >.
template <typename S>
[[gnu::always_inline]] inline bool TrapIf(unsigned to, S a, S b) {
  using U = std::make_unsigned_t<S>;
  return ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) ||
         ((to & 2) && U(a) < U(b)) || ((to & 1) && U(a) > U(b));
}

// ---- memory: big-endian, guest address = mem + low 32 bits -----------------
template <typename T>
[[gnu::always_inline]] inline T Swap(T v) {
  if constexpr (sizeof(T) == 1) return v;
  else if constexpr (sizeof(T) == 2) return T(__builtin_bswap16(uint16_t(v)));
  else if constexpr (sizeof(T) == 4) return T(__builtin_bswap32(uint32_t(v)));
  else return T(__builtin_bswap64(uint64_t(v)));
}
template <typename T>
[[gnu::always_inline]] inline T Ld(const uint8_t* m, uint32_t a) {
  T v;
  std::memcpy(&v, m + a, sizeof(T));
  return Swap(v);
}
template <typename T>
[[gnu::always_inline]] inline void St(uint8_t* m, uint32_t a, T v) {
  v = Swap(v);
  std::memcpy(m + a, &v, sizeof(T));
}
// The byte-reversed forms (LHBRX STWBRX ...): little-endian.
template <typename T>
[[gnu::always_inline]] inline T LdRev(const uint8_t* m, uint32_t a) {
  T v;
  std::memcpy(&v, m + a, sizeof(T));
  return v;
}
template <typename T>
[[gnu::always_inline]] inline void StRev(uint8_t* m, uint32_t a, T v) {
  std::memcpy(m + a, &v, sizeof(T));
}
// DCBZ (32 bytes) and the Xenon's DCBZ128 (its 128-byte cache line): zero
// the block containing the address.
[[gnu::always_inline]] inline void Dcbz(uint8_t* m, uint32_t a, uint32_t size) {
  std::memset(m + (a & ~(size - 1)), 0, size);
}
// LWARX / LDARX, STWCX. / STDCX. (see Ctx). Aligned, as the ISA requires;
// the compare-and-swap is sequentially consistent.
template <typename T>
[[gnu::always_inline]] inline T Larx(Ctx& c, uint32_t a) {
  T raw = __atomic_load_n(reinterpret_cast<T*>(c.mem + a), __ATOMIC_ACQUIRE);
  c.resv_addr = a;
  c.resv_value = raw;
  c.resv_valid = true;
  return Swap(raw);
}
template <typename T>
[[gnu::always_inline]] inline bool Stcx(Ctx& c, uint32_t a, T v) {
  bool ok = c.resv_valid && c.resv_addr == a;
  c.resv_valid = false;
  if (!ok) return false;
  T expected = T(c.resv_value);
  return __atomic_compare_exchange_n(reinterpret_cast<T*>(c.mem + a), &expected, Swap(v), false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
// SYNC and EIEIO: a full fence; LWSYNC orders everything but stores
// before loads (acquire-release); ISYNC orders nothing a lifted program
// can observe.
[[gnu::always_inline]] inline void Fence() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
[[gnu::always_inline]] inline void LwFence() { __atomic_thread_fence(__ATOMIC_ACQ_REL); }

}  // namespace lift
}  // namespace recomp::ppc
