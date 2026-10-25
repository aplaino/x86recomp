// Test fixture: a freestanding AArch64 program whose output depends on
// every class of integer instruction the lifter handles: arithmetic and
// flags, 128-bit arithmetic, bitfields, loads and stores of every width,
// calls through pointers and tables, jump tables, tail calls, recursion,
// varargs and atomics. Its output, one line per section, is compared
// between the lifted program and the Unicorn oracle (tests/oracle).

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

u32 nx_output_debug_string(const char* s, u64 len);
u64 ops_flags(u64 a, u64 b);
u64 ops_div(u64 a, u64 b);
u64 ops_bits(u64 a, u64 b);
u64 ops_tpidr(u64 v);
u64 ops_atomics(void* p, u64 v);
u64 ops_mem(void* p);

// The compiler may emit calls to these for loops and struct copies.
void* memcpy(void* d, const void* s, size_t n) {
  u8* dp = d;
  const u8* sp = s;
  while (n--) *dp++ = *sp++;
  return d;
}
void* memmove(void* d, const void* s, size_t n) {
  u8* dp = d;
  const u8* sp = s;
  if (dp < sp) {
    while (n--) *dp++ = *sp++;
  } else {
    while (n--) dp[n] = sp[n];
  }
  return d;
}
void* memset(void* d, int c, size_t n) {
  u8* dp = d;
  while (n--) *dp++ = (u8)c;
  return d;
}
size_t strlen(const char* s) {
  const char* p = s;
  while (*p) p++;
  return (size_t)(p - s);
}

// ---- output -------------------------------------------------------------
static char g_line[256];
static u32 g_len;

static void put_char(char c) {
  if (g_len < sizeof g_line) g_line[g_len++] = c;
}
static void put_str(const char* s) {
  while (*s) put_char(*s++);
}
static void put_hex(u64 v) {
  static const char kDigits[] = "0123456789abcdef";
  for (int i = 60; i >= 0; i -= 4) put_char(kDigits[(v >> i) & 0xF]);
}
static void put_dec(u64 v) {
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n) put_char(tmp[--n]);
}
static void flush(void) {
  put_char('\n');
  nx_output_debug_string(g_line, g_len);
  g_len = 0;
}
static void report(const char* name, u64 v) {
  put_str(name);
  put_str(": ");
  put_hex(v);
  flush();
}

// ---- inputs ---------------------------------------------------------------
static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u64 next(void) {
  u64 x = g_rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return g_rng = x;
}
// Values that hit carries, sign changes and zero more often than random.
static u64 special(void) {
  static const u64 kSpecial[] = {
      0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000ull,
      0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull,
      0xFFFFFFFF00000000ull, 63, 64, 31, 32};
  u64 r = next();
  if (r % 3 == 0) return kSpecial[(r >> 8) % (sizeof kSpecial / sizeof kSpecial[0])];
  return next();
}

static u64 mix(u64 h, u64 v) {
  h ^= v;
  h *= 0x100000001B3ull;
  return h ^ (h >> 29);
}

// ---- sections -------------------------------------------------------------
static u64 arith(void) {
  u64 h = 0xCBF29CE484222325ull;
  for (int i = 0; i < 400; i++) {
    u64 a = special(), b = special();
    u32 aw = (u32)a, bw = (u32)b;
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    h = mix(h, a + b);
    h = mix(h, a - (b << 3));
    h = mix(h, a * b);
    h = mix(h, (u64)aw * bw);
    h = mix(h, (u64)((int64_t)(int32_t)aw * (int32_t)bw));
    h = mix(h, a >> (b & 63));
    h = mix(h, (u64)(sa >> (b & 63)));
    h = mix(h, aw << (bw & 31));
    h = mix(h, (a >> 7) | (a << 57));
    h = mix(h, b ? a / b : 0);
    h = mix(h, b ? a % b : 0);
    h = mix(h, bw ? aw / bw : 0);
    if (sb != 0 && !(sa == INT64_MIN && sb == -1)) h = mix(h, (u64)(sa / sb) ^ (u64)(sa % sb));
    h = mix(h, sa < sb ? 1 : sa == sb ? 2 : 3);
    h = mix(h, aw > bw ? aw : bw);
    h = mix(h, (int32_t)aw < (int32_t)bw ? aw - bw : bw + 1);
    h = mix(h, sa < 0 ? 0 - a : a);
    h = mix(h, (a & 0xFF00FF00FF00FF00ull) | (b & ~0xFFFFull));
    h = mix(h, a + 0x123000);
    h = mix(h, (u16)a * (u64)(int16_t)b);
    h = mix(h, (int64_t)(int8_t)a + (u8)b);
  }
  return h;
}

static u64 wide(void) {
  u64 h = 0;
  for (int i = 0; i < 300; i++) {
    unsigned __int128 a = ((unsigned __int128)special() << 64) | special();
    unsigned __int128 b = ((unsigned __int128)special() << 64) | special();
    __int128 sa = (__int128)a;
    unsigned __int128 r = a + b;
    h = mix(h, (u64)r ^ (u64)(r >> 64));
    r = a - b;
    h = mix(h, (u64)r ^ (u64)(r >> 64));
    r = (unsigned __int128)(u64)a * (u64)b;
    h = mix(h, (u64)r ^ (u64)(r >> 64));
    __int128 s = (__int128)(int64_t)(u64)a * (int64_t)(u64)b;
    h = mix(h, (u64)s ^ (u64)((unsigned __int128)s >> 64));
    r = a * b;
    h = mix(h, (u64)(r >> 64));
    h = mix(h, a < b ? 5 : 9);
    h = mix(h, sa < (__int128)b ? 3 : 4);
    h = mix(h, (u64)(a >> (special() & 127)));
  }
  return h;
}

struct Bits {
  u32 a : 3;
  u32 b : 11;
  int32_t c : 7;
  u32 d : 11;
};

static u32 popcount64(u64 v) {
  u32 n = 0;
  while (v) {
    v &= v - 1;
    n++;
  }
  return n;
}

static u64 bits(void) {
  u64 h = 0;
  struct Bits bf = {0};
  for (int i = 0; i < 300; i++) {
    u64 a = special(), b = next();
    h = mix(h, a ? (u64)__builtin_clzll(a) : 64);
    h = mix(h, a ? (u64)__builtin_ctzll(a) : 64);
    h = mix(h, (u64)__builtin_clrsbll((long long)a));
    h = mix(h, popcount64(a));
    h = mix(h, __builtin_bswap64(a) ^ __builtin_bswap32((u32)b) ^ __builtin_bswap16((u16)a));
    h = mix(h, __builtin_bitreverse64(a) ^ __builtin_bitreverse32((u32)b));
    bf.a = (u32)a;
    bf.b = (u32)(b >> 5);
    bf.c = (int32_t)(a >> 9);
    bf.d += (u32)b;
    h = mix(h, (u64)bf.a + bf.b + (u64)(int64_t)bf.c + bf.d);
    h = mix(h, (a >> 13) & 0x7FF);
    h = mix(h, (u64)((int64_t)(a << 20) >> 41));
    h = mix(h, (a & ~(0x3FFull << 22)) | ((b & 0x3FF) << 22));
    h = mix(h, ops_bits(a, b));
    h = mix(h, ops_flags(a, b));
    h = mix(h, ops_div(a, special()));
  }
  return h;
}

// Loads and stores of every width and signedness through structs.
struct Rec {
  u8 tag;
  int8_t delta;
  u16 count;
  int16_t bias;
  u32 id;
  int32_t offset;
  u64 key;
};

static struct Rec g_recs[64];

static u64 memory(void) {
  u64 h = 0;
  for (int i = 0; i < 64; i++) {
    u64 r = next();
    g_recs[i].tag = (u8)r;
    g_recs[i].delta = (int8_t)(r >> 8);
    g_recs[i].count = (u16)(r >> 16);
    g_recs[i].bias = (int16_t)(r >> 24);
    g_recs[i].id = (u32)(r >> 32);
    g_recs[i].offset = (int32_t)r;
    g_recs[i].key = r * 31;
  }
  for (int round = 0; round < 8; round++) {
    for (int i = 0; i < 64; i++) {
      struct Rec* a = &g_recs[i];
      struct Rec* b = &g_recs[(i * 7 + round) & 63];
      int64_t v = a->delta + a->bias + (int64_t)a->offset;
      h = mix(h, (u64)v + a->tag + a->count + a->id + b->key);
      struct Rec t = *a;
      t.key ^= (u64)v;
      t.delta = (int8_t)(t.delta + b->tag);
      *b = t;
    }
  }
  static u8 buf[256] __attribute__((aligned(16)));
  for (int i = 0; i < 256; i++) buf[i] = (u8)next();
  h = mix(h, ops_mem(buf));
  for (int i = 0; i < 256; i += 8) h = mix(h, *(u64*)(buf + i));
  char text[64];
  memset(text, 0, sizeof text);
  for (int i = 0; i < 40; i++) text[i] = (char)(0x41 + next() % 90);
  for (const char* p = text; *p; p++) h = mix(h, (u64)(int64_t)*p);
  h = mix(h, strlen(text));
  memmove(text + 3, text, 30);
  for (int i = 0; i < 64; i++) h = mix(h, (u8)text[i]);
  return h;
}

// Sorting through comparators reached by pointer, from a table the loader
// relocates.
typedef int (*Cmp)(u32, u32);
static int cmp_up(u32 a, u32 b) { return a < b ? -1 : a > b; }
static int cmp_down(u32 a, u32 b) { return cmp_up(b, a); }
static int cmp_low(u32 a, u32 b) { return cmp_up(a & 0xFFF, b & 0xFFF); }
static int cmp_signed(u32 a, u32 b) {
  return (int32_t)a < (int32_t)b ? -1 : (int32_t)a > (int32_t)b;
}
static const Cmp kCmps[] = {cmp_up, cmp_down, cmp_low, cmp_signed};

static void quicksort(u32* v, int lo, int hi, Cmp cmp) {
  while (lo < hi) {
    u32 pivot = v[(lo + hi) / 2];
    int i = lo, j = hi;
    while (i <= j) {
      while (cmp(v[i], pivot) < 0) i++;
      while (cmp(v[j], pivot) > 0) j--;
      if (i <= j) {
        u32 t = v[i];
        v[i++] = v[j];
        v[j--] = t;
      }
    }
    if (j - lo < hi - i) {
      quicksort(v, lo, j, cmp);
      lo = i;
    } else {
      quicksort(v, i, hi, cmp);
      hi = j;
    }
  }
}

static u64 sorting(void) {
  static u32 v[200];
  u64 h = 0;
  for (int k = 0; k < 4; k++) {
    for (int i = 0; i < 200; i++) v[i] = (u32)next();
    quicksort(v, 0, 199, kCmps[k]);
    for (int i = 0; i < 200; i++) h = mix(h, v[i]);
  }
  return h;
}

// A dense switch the compiler turns into a jump table; the cases make
// calls so it cannot become a lookup table.
static u64 __attribute__((noinline)) step(u32 op, u64 acc, u64 x) {
  switch (op) {
    case 0: return acc + x;
    case 1: return acc - mix(acc, x);
    case 2: return acc ^ (x << 3);
    case 3: return mix(acc, x * 3);
    case 4: return acc * (x | 1);
    case 5: return acc >> (x & 31);
    case 6: return mix(x, acc);
    case 7: return acc + mix(x, 7);
    case 8: return ~acc;
    case 9: return acc | mix(x, 9);
    case 10: return acc & (x | 0xFF);
    case 11: return mix(acc, ops_tpidr(x));
    case 12: return acc + 12;
    case 13: return mix(acc ^ x, 13);
    default: return acc ^ 0x5555;
  }
}

static u64 switches(void) {
  u64 acc = 1;
  for (int i = 0; i < 1000; i++) acc = step((u32)(next() % 17), acc, next());
  return acc;
}

static u64 __attribute__((noinline)) ackermann(u64 m, u64 n) {
  if (m == 0) return n + 1;
  if (n == 0) return ackermann(m - 1, 1);
  return ackermann(m - 1, ackermann(m, n - 1));
}
static u64 __attribute__((noinline)) fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

// Tail calls through a pointer and to known functions.
typedef u64 (*Fn)(u64);
static u64 __attribute__((noinline)) twice(u64 v) { return v * 2 + 1; }
static u64 __attribute__((noinline)) thrice(u64 v) { return v * 3 + 2; }
static u64 __attribute__((noinline)) via(Fn f, u64 v) { return f(v ^ 0x55); }
static u64 __attribute__((noinline)) chain(u64 v) { return v & 1 ? twice(v) : thrice(v); }

static u64 __attribute__((noinline)) sum_va(int n, ...) {
  va_list ap;
  va_start(ap, n);
  u64 s = 0;
  for (int i = 0; i < n; i++) s = mix(s, va_arg(ap, u64));
  va_end(ap);
  return s;
}

static u64 calls(void) {
  u64 h = ackermann(2, 3);
  h = mix(h, fib(18));
  static Fn fns[2] = {twice, thrice};
  for (int i = 0; i < 50; i++) {
    h = mix(h, via(fns[i & 1], h));
    h = mix(h, chain(h));
  }
  h = mix(h, sum_va(3, 1ull, 2ull, h));
  h = mix(h, sum_va(11, 1ull, 2ull, 3ull, 4ull, 5ull, 6ull, 7ull, 8ull, 9ull, 10ull, h));
  return h;
}

static u64 atomics(void) {
  static u64 a64;
  static u32 a32;
  static u16 a16;
  static u8 a8;
  static u64 block[4] __attribute__((aligned(16)));
  u64 h = 0;
  for (int i = 0; i < 50; i++) {
    u64 v = next();
    h = mix(h, __atomic_fetch_add(&a64, v, __ATOMIC_RELAXED));
    h = mix(h, __atomic_fetch_sub(&a32, (u32)v, __ATOMIC_ACQ_REL));
    h = mix(h, __atomic_fetch_or(&a16, (u16)v, __ATOMIC_SEQ_CST));
    h = mix(h, __atomic_fetch_xor(&a8, (u8)v, __ATOMIC_RELEASE));
    h = mix(h, __atomic_exchange_n(&a32, (u32)(v >> 32), __ATOMIC_ACQUIRE));
    u64 expected = v & 1 ? a64 : v;
    h = mix(h, __atomic_compare_exchange_n(&a64, &expected, v, 0, __ATOMIC_SEQ_CST,
                                           __ATOMIC_RELAXED));
    h = mix(h, expected);
    __atomic_store_n(&a16, (u16)(v >> 16), __ATOMIC_RELEASE);
    h = mix(h, __atomic_load_n(&a16, __ATOMIC_ACQUIRE));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    block[i & 3] = v;
    h = mix(h, ops_atomics(block, v));
  }
  return h;
}

int fixture_main(void) {
  report("arith", arith());
  report("wide", wide());
  report("bits", bits());
  report("memory", memory());
  report("sorting", sorting());
  report("switches", switches());
  report("calls", calls());
  report("atomics", atomics());
  put_str("done ");
  put_dec(g_rng % 100000);
  flush();
  return 0;
}
