// The lifter's whole-program fixture: integer C that exercises calls,
// recursion, loops, a switch (a jump table), function pointers, 64- and
// 32-bit arithmetic, byte swaps and a compare-and-swap, and prints what it
// computed into ORACLE_OUTPUT (oracle/oracle.h). Built freestanding for
// big-endian PowerPC64; run bare metal in 32-bit mode on the oracle
// (oracle/qemu_run.py) and lifted (fixture_main.cpp), the two outputs must
// match.

#include "oracle.h"

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;

void* memset(void* p, int v, unsigned long n) {
  u8* b = p;
  while (n--) *b++ = (u8)v;
  return p;
}
void* memcpy(void* d, const void* s, unsigned long n) {
  u8* a = d;
  const u8* b = s;
  while (n--) *a++ = *b++;
  return d;
}

static char* const out = (char*)(uintptr_t)ORACLE_OUTPUT;
static unsigned out_len;

static void put(const char* s) {
  while (*s && out_len < 4096) out[out_len++] = *s++;
}
static void hex(const char* label, u64 v) {
  char b[17];
  for (int k = 15; k >= 0; k--, v >>= 4) b[k] = "0123456789abcdef"[v & 15];
  b[16] = 0;
  put(label);
  put(" ");
  put(b);
  put("\n");
}

static u64 state = 0x243F6A8885A308D3ull;
static u64 next(void) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

__attribute__((noinline)) static u64 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

__attribute__((noinline)) static u32 classify(u32 v) {
  switch (v % 13) {
    case 0: return v * 3;
    case 1: return v ^ 0x5555;
    case 2: return v >> 3;
    case 3: return v + 1000;
    case 4: return ~v;
    case 5: return v * v;
    case 6: return v - 77;
    case 7: return v << 5;
    case 8: return v | 0x80000000u;
    case 9: return v & 0xFF00FF;
    case 10: return v / 7;
    case 11: return v % 1000;
    default: return 12345;
  }
}

// fixture/tables.S: the Xbox 360 compiler's jump table shapes.
int table_words(unsigned v);
int table_bytes(unsigned v);

typedef u64 (*Op)(u64, u64);
__attribute__((noinline)) static u64 op_add(u64 a, u64 b) { return a + b; }
__attribute__((noinline)) static u64 op_mul(u64 a, u64 b) { return a * b; }
__attribute__((noinline)) static u64 op_div(u64 a, u64 b) { return b ? a / b : 0; }
__attribute__((noinline)) static u64 op_sdiv(u64 a, u64 b) { return b ? (u64)((i64)a / (i64)(b | 1)) : 0; }
__attribute__((noinline)) static u64 op_rot(u64 a, u64 b) { return (a << (b & 63)) | (a >> ((64 - (b & 63)) & 63)); }
static Op ops_table[] = {op_add, op_mul, op_div, op_sdiv, op_rot};

__attribute__((noinline)) static u32 bits(u32 v) {
  u32 r = (u32)__builtin_clz(v | 1) + (u32)__builtin_popcount(v);
  return r ^ __builtin_bswap32(v) ^ (u32)(v >> 7 | v << 25);
}

__attribute__((noinline)) static i64 narrow(i64 a, i64 b) {
  i32 x = (i32)a, y = (i32)b | 1;
  i64 r = (i64)x * y;
  r += (i64)(x / y) + (i64)((u32)x % (u32)y);
  r += (i64)((i64)(short)a + (signed char)b);
  return r;
}

static u32 counter;
__attribute__((noinline)) static u32 bump(u32 by) {
  u32 old = __atomic_load_n(&counter, __ATOMIC_RELAXED);
  while (!__atomic_compare_exchange_n(&counter, &old, old + by, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
  }
  return old;
}

// Floating point, double and single: the arithmetic, a fused multiply-add,
// square roots, conversions both ways, compares and selects. The results
// are printed as bits.
static u64 bits_of(double d) {
  u64 v;
  memcpy(&v, &d, 8);
  return v;
}
static u32 bits_of_float(float f) {
  u32 v;
  memcpy(&v, &f, 4);
  return v;
}
__attribute__((noinline)) static double horner(double x) {
  return ((((0.0083333 * x - 0.1666667) * x) * x + 1.0) * x) / (1.0 + x * x);
}
__attribute__((noinline)) static float lerp(float a, float b, float t) { return a + (b - a) * t; }
__attribute__((noinline)) static u64 floats(void) {
  u64 h = 0;
  double x = 0.125;
  for (u32 k = 0; k < 200; k++) {
    double v = (double)(i64)next() / 9.007199254740992e15;
    x = horner(v) + __builtin_sqrt(__builtin_fabs(v) + x * x);
    h = h * 31 + bits_of(x);
    i32 iv = (i32)(v * 1000.0);
    h ^= (u64)(i64)iv + bits_of((double)(i64)next());
    float f = lerp((float)v, (float)x, 0.25f) * 1.5f;
    h = h * 7 + bits_of_float(f) + bits_of_float(__builtin_sqrtf(__builtin_fabsf(f)));
    h += (v < x) + 2 * (f > 0.5f) + 4 * (x == x);
    double m = v > 0 ? v : -v;
    h ^= bits_of(m / (x + 3.0));
  }
  return h;
}

struct Item {
  u32 key;
  unsigned short tag;
  signed char delta;
  u64 value;
};

int main(void) {
  u64 sum = 0;
  for (u32 n = 0; n < 24; n++) sum = sum * 31 + fib(n);
  hex("fib", sum);

  u64 sw = 0;
  for (u32 k = 0; k < 400; k++) sw = sw * 33 + classify((u32)next());
  hex("switch", sw);

  hex("float", floats());

  u64 jt = 0;
  for (u32 k = 0; k < 64; k++) jt = jt * 5 + (u64)(i64)table_words(k % 7) + (u64)(i64)table_bytes(k % 8);
  hex("tables", jt);

  u64 acc = 1;
  for (u32 k = 0; k < 200; k++) acc ^= ops_table[k % 5](next(), next() >> (k % 61));
  hex("calls", acc);

  u64 b = 0;
  for (u32 k = 0; k < 300; k++) b = b * 7 + bits((u32)next());
  hex("bits", b);

  i64 nr = 0;
  for (u32 k = 0; k < 300; k++) nr = nr * 3 + narrow((i64)next(), (i64)next());
  hex("narrow", (u64)nr);

  struct Item items[32];
  for (u32 k = 0; k < 32; k++) {
    items[k].key = (u32)next();
    items[k].tag = (unsigned short)next();
    items[k].delta = (signed char)next();
    items[k].value = next();
  }
  // Insertion sort on the keys.
  for (u32 k = 1; k < 32; k++) {
    struct Item t = items[k];
    u32 j = k;
    while (j > 0 && items[j - 1].key > t.key) {
      items[j] = items[j - 1];
      j--;
    }
    items[j] = t;
  }
  u64 st = 0;
  for (u32 k = 0; k < 32; k++) st = st * 1000003 + items[k].key + items[k].tag + (u64)(i64)items[k].delta + items[k].value;
  hex("sort", st);

  for (u32 k = 0; k < 50; k++) bump(k);
  hex("atomic", counter);

  struct OracleDone* done = (struct OracleDone*)(uintptr_t)ORACLE_DONE;
  done->data = ORACLE_OUTPUT;
  done->length = out_len;
  return (int)(sum & 0x7F);
}
