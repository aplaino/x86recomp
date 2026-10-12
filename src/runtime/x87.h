#pragma once
// Exact model of the x87 FPU for lifted code (see ARCHITECTURE.md, "x87:
// full 80-bit model"). Portable C++ on top of Berkeley SoftFloat 3e; never
// uses the host FPU, long double or intrinsics.
//
// Conventions:
// - "st(i)" is relative to TOP; physical register = (top + i) & 7.
// - Every operation honours the control word: PC (bits 8-9) rounds the
//   significand of add/sub/mul/div/sqrt results to 24/53/64 bits with the
//   full 15-bit exponent range kept; RC (bits 10-11) applies to every
//   rounding (arithmetic, stores to m32/m64/int, frndint).
// - Exceptions are assumed masked (Windows default 0x027F, game uses
//   masked modes). Flags are accumulated sticky in the status word. If an
//   exception occurs while its mask bit is clear, unmasked_fault latches
//   (checked by the tests); the masked response is still delivered.
// - Stack overflow/underflow follow the masked response: IE + SF set, C1
//   = 1 on overflow / 0 on underflow, result is the real indefinite.
// - Compare results go to C0/C2/C3 of the status word (for fnstsw+sahf);
//   the FCOMI family returns EFLAGS bits instead.

#include <cstdint>

namespace recomp::x87 {

// 80-bit extended value, same layout as the x87 memory format and as
// SoftFloat's extFloat80M: 64-bit significand (explicit integer bit), then
// sign (bit 15) and biased exponent (bits 0-14).
struct F80 {
  uint64_t sig;
  uint16_t se;
};

struct State {
  F80 r[8];        // physical registers R0..R7
  uint16_t cw;     // control word
  uint16_t sw;     // status word, TOP field (bits 11-13) kept in `top`
  uint16_t tw;     // full tag word: 2 bits per physical register
                   // (00 valid, 01 zero, 10 special, 11 empty)
  uint8_t top;     // 0..7
  bool unmasked_fault;  // an unmasked exception was raised (see above)
  // Last instruction / operand pointers: zeroed by finit and only carried
  // through fnstenv/fldenv and fnsave/frstor (no game code inspects them).
  uint32_t fip, fdp;
  uint16_t fop;
};

// Test hook: true disables every integer fast path (arithmetic, stores,
// trig), leaving SoftFloat, their reference.
extern bool g_softfloat_only;

enum class Op : uint8_t { Add, Mul, Sub, SubR, Div, DivR };
// Sub: dst - src, SubR: src - dst, Div: dst / src, DivR: src / dst, where
// dst is the register being written (Intel manual operand order).

enum class Const : uint8_t { One, L2T, L2E, Pi, LG2, LN2, Zero };

// FCOMI/FUCOMI result as EFLAGS bits.
struct EFlagsResult {
  bool zf, pf, cf;
};

// --- state -----------------------------------------------------------
void Init(State& s);              // FNINIT: cw 0x037F, sw 0, all empty
uint16_t StatusWord(const State& s);  // FNSTSW: sw with TOP merged in
uint16_t ControlWord(const State& s);
void SetControlWord(State& s, uint16_t cw);  // FLDCW
void ClearExceptions(State& s);   // FNCLEX
uint16_t TagWord(const State& s);

// --- loads (push) ----------------------------------------------------
void LoadF32(State& s, uint32_t bits);   // FLD m32 (exact)
void LoadF64(State& s, uint64_t bits);   // FLD m64 (exact)
void LoadF80(State& s, F80 v);           // FLD m80
void LoadSt(State& s, int i);            // FLD st(i)
void LoadI16(State& s, int16_t v);       // FILD m16
void LoadI32(State& s, int32_t v);       // FILD m32
void LoadI64(State& s, int64_t v);       // FILD m64
void LoadConst(State& s, Const k);       // FLD1/FLDZ/FLDPI/... (RC-aware
                                         // like hardware for pi, l2e, ...)

// --- stores (st0 -> destination; caller pops for the P forms) --------
uint32_t StoreF32(State& s);   // FST m32: rounded with RC
uint64_t StoreF64(State& s);   // FST m64: rounded with RC
F80 StoreF80(State& s);        // FSTP m80 (exact)
void StoreSt(State& s, int i); // FST st(i)
int16_t StoreI16(State& s);    // FIST m16: RC, indefinite 0x8000 + IE
int32_t StoreI32(State& s);    // FIST m32: RC, indefinite 0x80000000
int64_t StoreI64(State& s);    // FISTP m64: RC, indefinite INT64_MIN
void Pop(State& s);            // mark st0 empty, ++TOP

// --- arithmetic ------------------------------------------------------
void ArithF32(State& s, Op op, uint32_t bits);  // st0 = st0 op m32
void ArithF64(State& s, Op op, uint64_t bits);  // st0 = st0 op m64
void ArithI16(State& s, Op op, int16_t v);      // FIADD m16 etc.
void ArithI32(State& s, Op op, int32_t v);      // FIADD m32 etc.
void ArithSt0Sti(State& s, Op op, int i);       // st0 = st0 op st(i)
void ArithStiSt0(State& s, Op op, int i);       // st(i) = st(i) op st0
                                                // (caller pops for ...P)

// --- compares (caller pops for FCOMP/FCOMPP/FUCOMPP) ------------------
// quiet = FUCOM semantics (only SNaN raises IE); otherwise FCOM.
void CompareSt(State& s, int i, bool quiet);
void CompareF32(State& s, uint32_t bits);
void CompareF64(State& s, uint64_t bits);
void CompareI16(State& s, int16_t v);   // FICOM m16
void CompareI32(State& s, int32_t v);   // FICOM m32
EFlagsResult CompareStEFlags(State& s, int i, bool quiet);  // FCOMI/FUCOMI
void Test(State& s);    // FTST
void Examine(State& s); // FXAM

// --- unary / misc ----------------------------------------------------
void Chs(State& s);
void Abs(State& s);
void Sqrt(State& s);     // PC-rounded
void RndInt(State& s);   // RC
void Scale(State& s);    // FSCALE
void Xtract(State& s);   // FXTRACT (pushes)
void Prem(State& s);     // FPREM (C0/C1/C3 quotient bits, C2 partial)
void Prem1(State& s);    // FPREM1
void Sin(State& s);      // FSIN   (C2=1 and unchanged if |x| >= 2^63)
void Cos(State& s);      // FCOS
void SinCos(State& s);   // FSINCOS (pushes)
void Ptan(State& s);     // FPTAN (pushes 1.0)
void Patan(State& s);    // FPATAN: st1 = atan2(st1, st0), pop
void F2xm1(State& s);    // st0 = 2^st0 - 1
void Yl2x(State& s);     // st1 = st1 * log2(st0), pop
void Yl2xp1(State& s);   // st1 = st1 * log2(st0 + 1), pop
void Xch(State& s, int i);      // FXCH st(i)
void CopySt(State& s, int i);   // st0 = st(i) (FCMOVcc when taken)
void Free(State& s, int i);     // FFREE st(i)
void IncStp(State& s);
void DecStp(State& s);

// --- value-level operations for lifted x87 runs ----------------------
//
// Pure forms of the operations above for lifted runs (ARCHITECTURE.md
// "x87 runs"); both share the same cores, so results are identical.
namespace run {

// Lifted runs are used when true (<ID>_X87_RUNS=0 disables them, for A/B;
// RunProcess reads it).
extern bool g_enabled;

// A register value in a run: F80 without padding (two full 64-bit fields),
// so it travels between calls in registers; passing F80 itself makes
// compilers assemble its padded half in memory (store-forwarding stalls).
struct V {
  uint64_t sig;
  uint64_t se;  // sign/exponent in bits 0-15, the rest zero
};
inline V ToV(F80 f) { return {f.sig, f.se}; }
inline F80 ToF80(V v) { return {v.sig, static_cast<uint16_t>(v.se)}; }

// A register result: value, exception flags and C1 (rounded up), packed
// so it comes back in two registers.
struct RV {
  uint64_t sig;
  uint64_t meta;  // bits 0-15 sign/exponent, 16-31 flags, 32 up
};
inline V Val(RV r) { return {r.sig, r.meta & 0xFFFF}; }
inline uint32_t Flags(RV r) { return static_cast<uint32_t>(r.meta >> 16) & 0xFFFF; }
inline bool Up(RV r) { return (r.meta >> 32) & 1; }

// A memory result: stored bits, flags, C1.
struct SV {
  uint64_t bits;
  uint64_t meta;  // bits 0-15 flags, 16 up
};
inline uint32_t Flags(SV r) { return static_cast<uint32_t>(r.meta) & 0xFFFF; }
inline bool Up(SV r) { return (r.meta >> 16) & 1; }

// dst op src (Intel operand order; dst is the register written).
RV Arith(uint32_t cw, Op op, V dst, V src);
RV ArithF32(uint32_t cw, Op op, V st0, uint32_t bits);
RV ArithF64(uint32_t cw, Op op, V st0, uint64_t bits);
RV ArithI16(uint32_t cw, Op op, V st0, int16_t v);
RV ArithI32(uint32_t cw, Op op, V st0, int32_t v);
RV Sqrt(uint32_t cw, V v);

// FLD m32/m64: the value pushed (SNaN quieted) and IE/DE.
RV LoadF32(uint32_t bits);
RV LoadF64(uint64_t bits);
V FromInt(int64_t v);                // FILD (exact)
V ConstValue(uint32_t cw, Const k);    // FLD1/FLDZ/FLDPI/...

SV StoreF32(uint32_t cw, V v);
SV StoreF64(uint32_t cw, V v);
SV StoreI16(uint32_t cw, V v);  // bits = the integer stored
SV StoreI32(uint32_t cw, V v);
SV StoreI64(uint32_t cw, V v);

// FCOM family: C3/C2/C0 (status word positions) | flags << 16.
uint32_t Compare(V a, V b, bool quiet);
uint32_t CompareF32(V a, uint32_t bits);
uint32_t CompareF64(V a, uint64_t bits);

// --- inline fast paths (the game's common case) ---
//
// D3D leaves the FPU at PC=24 (the game runs it with RC=chop), and m32
// loads and PC=24 results have 24-bit significands, so most run operations
// are add, sub or mul of two such normal values. Those are exact in 64-bit
// integers; rounding to 24 bits with RC `rc` is done here as RoundPack does
// it. This core is
// also the model's own fast path (x87.cpp, Fast24), so lifted code and the
// State operations cannot disagree. Anything else (other modes, specials,
// results leaving the normal range, division) returns false.
// Forced inline: compilers otherwise keep these out of line in the big
// lifted functions, which costs most of the gain.
enum class F24Op { Add, Sub, Mul };  // Sub: a - b
[[gnu::always_inline]] inline bool Fast24(F24Op op, int rc, V a, V b, RV* out) {
  constexpr uint64_t kLow40 = (1ull << 40) - 1, kJ = 1ull << 63;
  uint32_t ea = a.se & 0x7FFFu, eb = b.se & 0x7FFFu;
  if (((a.sig | b.sig) & kLow40) || !(a.sig & b.sig & kJ) || ea - 1 >= 0x7FFEu ||
      eb - 1 >= 0x7FFEu)
    return false;
  bool sa = (a.se & 0x8000u) != 0, sb = (b.se & 0x8000u) != 0, sign;
  uint64_t ma = a.sig >> 40, mb = b.sig >> 40, v;  // [2^23, 2^24)
  int32_t exp;
  if (op == F24Op::Mul) {
    v = ma * mb;  // value = v / 2^46 * 2^(ea + eb - 2 * 0x3FFF)
    sign = sa != sb;
    int lz = __builtin_clzll(v);
    v <<= lz;
    exp = int32_t(ea + eb) - 0x3FFF + 17 - lz;
  } else {
    bool sbx = op == F24Op::Sub ? !sb : sb;
    bool swap = eb > ea || (eb == ea && mb > ma);
    uint64_t mx = swap ? mb : ma, my = swap ? ma : mb;
    int32_t ex = int32_t(swap ? eb : ea), d = ex - int32_t(swap ? ea : eb);
    sign = swap ? sbx : sa;
    if (d > 39) return false;
    uint64_t x = mx << 39, y = (my << 39) >> d;  // exact: value = . / 2^62
    v = sa != sbx ? x - y : x + y;
    if (!v) {  // exact zero: -0 when rounding down, else +0
      *out = {0, rc == 1 ? 0x8000u : 0u};
      return true;
    }
    int lz = __builtin_clzll(v);
    v <<= lz;
    exp = ex + 1 - lz;
  }
  uint64_t keep = v >> 40, rest = v & kLow40;
  bool inc = rc == 0 ? rest > (1ull << 39) || (rest == (1ull << 39) && (keep & 1))
           : rc == 1 ? sign && rest
           : rc == 2 ? !sign && rest
                     : false;
  if (inc && ++keep == (1ull << 24)) {
    keep >>= 1;
    ++exp;
  }
  if (exp < 1 || exp > 0x7FFE) return false;
  *out = {keep << 40, uint64_t(sign ? 0x8000u : 0u) | uint64_t(exp) |
                          (rest ? uint64_t(0x20) << 16 : 0) | uint64_t(inc) << 32};
  return true;
}

[[gnu::always_inline]] inline bool Fast24Op(int rc, Op op, V dst, V src, RV* out) {
  switch (op) {
    case Op::Add: return Fast24(F24Op::Add, rc, dst, src, out);
    case Op::Mul: return Fast24(F24Op::Mul, rc, dst, src, out);
    case Op::Sub: return Fast24(F24Op::Sub, rc, dst, src, out);
    case Op::SubR: return Fast24(F24Op::Sub, rc, src, dst, out);
    default: return false;
  }
}

// What lifted runs call: the fast path inline, the rest out of line.
[[gnu::always_inline]] inline RV ArithFast(uint32_t cw, Op op, V dst, V src) {
  RV r;
  if ((cw & 0x0300) == 0 && Fast24Op(int(cw >> 10) & 3, op, dst, src, &r)) return r;
  return Arith(cw, op, dst, src);
}
[[gnu::always_inline]] inline RV ArithF32Fast(uint32_t cw, Op op, V st0, uint32_t bits) {
  uint32_t e = (bits >> 23) & 0xFF;
  if ((cw & 0x0300) == 0 && e - 1 < 254) {  // PC=24, normal m32 (exact, no DE)
    V m{(uint64_t(bits & 0x7FFFFF) | 0x800000) << 40, ((bits >> 31) << 15) | (e - 127 + 0x3FFF)};
    RV r;
    if (Fast24Op(int(cw >> 10) & 3, op, st0, m, &r)) return r;
  }
  return ArithF32(cw, op, st0, bits);
}
[[gnu::always_inline]] inline RV LoadF32Fast(uint32_t bits) {
  uint32_t e = (bits >> 23) & 0xFF;
  if (e - 1 < 254)
    return {(uint64_t(bits & 0x7FFFFF) | 0x800000) << 40,
            uint64_t(((bits >> 31) << 15) | (e - 127 + 0x3FFF))};
  return LoadF32(bits);
}
// FST m32 of a normal value with a 24-bit significand in the m32 range:
// exact (no flags, C1 = 0).
[[gnu::always_inline]] inline SV StoreF32Fast(uint32_t cw, V v) {
  int32_t e = int32_t(v.se & 0x7FFF) - 0x3FFF + 127;
  if (!(v.sig & ((1ull << 40) - 1)) && (v.sig >> 63) && e >= 1 && e <= 254)
    return {((v.se & 0x8000u) ? 0x80000000ull : 0) | uint64_t(e) << 23 |
                ((v.sig >> 40) & 0x7FFFFF),
            0};
  return StoreF32(cw, v);
}

// Tag of a non-empty register: 00 valid, 01 zero, 10 special.
inline uint32_t Tag(V v) {
  uint32_t e = v.se & 0x7FFFu;
  if (e == 0) return v.sig ? 2 : 1;
  if (e == 0x7FFF || !(v.sig >> 63)) return 2;
  return 0;
}

// Bit p set when physical register p is empty (tag 11).
inline uint32_t EmptyMask(uint32_t tw) {
  uint32_t x = tw & (tw >> 1) & 0x5555u;
  x = (x | (x >> 1)) & 0x3333u;
  x = (x | (x >> 2)) & 0x0F0Fu;
  return (x | (x >> 4)) & 0x00FFu;
}

// The run guard. `valid`/`empty` are entry-relative masks (bit k for
// st(k) at entry, slots below the top as k & 7): registers the run reads
// before writing, and slots it pushes into.
inline bool Guard(const State& s, uint32_t valid, uint32_t empty) {
  if (!g_enabled || (s.cw & 0x3F) != 0x3F) return false;
  uint32_t t = s.top;
  uint32_t e = EmptyMask(s.tw);
  uint32_t v = ((valid << t) | (valid >> (8 - t))) & 0xFF;
  uint32_t m = ((empty << t) | (empty >> (8 - t))) & 0xFF;
  return (e & v) == 0 && (e & m) == m;
}

// The status word FNSTSW reports for sticky `sw` (C bits included), TOP
// `top`, under control word `cw` (as StatusWord()).
inline uint16_t StatusWordOf(uint32_t sw, uint32_t top, uint32_t cw) {
  uint32_t r = (sw & ~0xB880u) | (top << 11);  // drop TOP, ES, B
  if (sw & ~cw & 0x3F) r |= 0x8080;
  return static_cast<uint16_t>(r);
}

}  // namespace run

// --- environment (32-bit protected-mode formats) ---------------------
void StoreEnv(State& s, uint8_t out[28]);       // FNSTENV, then masks all
void LoadEnv(State& s, const uint8_t in[28]);   // FLDENV
void Save(State& s, uint8_t out[108]);          // FNSAVE, then FNINIT
void Restore(State& s, const uint8_t in[108]);  // FRSTOR

}  // namespace recomp::x87
