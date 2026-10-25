/* The form oracle: runs every instruction form recomp_lift_ppc --forms
   wrote a native stub for (forms_native.S, forms_native.c) from
   kVectorsPerForm random states, and records each run (oracle.h, struct
   Record) from ORACLE_RECORDS on. Bare metal, 32-bit mode (crt0.S); the
   host test (lift_forms_test.cpp) replays the records through the lifted
   forms. */
#include "oracle.h"

extern const struct FormInfo kFormInfo[];
extern const int kFormInfoCount;

/* Floating-point and VMX forms get more states: their edge cases (double
   rounding, tininess, NaN priority, saturation) are rarer in random data. */
enum { kVectorsPerForm = 64, kVectorsPerFpForm = 256 };

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint64_t Next(void) { return OracleSplitMix(&rng); }

/* Mostly random, sometimes a value at an edge of 32- or 64-bit arithmetic. */
static uint64_t Value(void) {
  static const uint64_t kEdges[] = {
      0, 1, 2, 31, 32, 33, 63, 64, 127,
      0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull,
      0xFFFFFFFF80000000ull, 0xFFFFFFFF7FFFFFFFull, 0x7FFFFFFFFFFFFFFFull,
      0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFEull,
      0x00000000FFFF0000ull, 0x8000000080000000ull};
  uint64_t r = Next();
  switch (r & 7) {
    case 0: case 1: return kEdges[(r >> 8) % (sizeof kEdges / sizeof kEdges[0])];
    case 2: return (r >> 8) & 0xFF;
    case 3: return (uint64_t)(int64_t)(int32_t)(r >> 16);
    default: return Next();
  }
}

/* A double's bits: special values (zeros, infinities, quiet and signaling
   NaNs, denormals, the extremes of double and single), values exact in
   single precision (what single-precision arithmetic is defined on),
   integers (for the conversions) and random patterns. */
static uint64_t SingleBits(uint32_t w) {
  /* LFS's conversion, for normal and special single values. */
  int special = ((w >> 23) & 0xFF) == 0xFF;
  uint64_t w1 = (w >> 30) & 1;
  uint64_t top = (uint64_t)(w >> 30) << 62 | ((special ? w1 : !w1) ? 7ull << 59 : 0);
  return top | (uint64_t)(w & 0x3FFFFFFF) << 29;
}
static uint64_t FpValue(void) {
  static const uint64_t kSpecial[] = {
      0x0000000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
      0x7FF8000000000000ull, 0xFFF8000000000001ull, 0x7FF0000000000001ull, 0xFFF4000000000000ull,
      0x0000000000000001ull, 0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull,
      0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x4000000000000000ull, 0x3FE0000000000000ull,
      0x47EFFFFFE0000000ull, 0x3810000000000000ull, 0x36A0000000000000ull, 0x36A0000000000001ull,
      0x41DFFFFFFFC00000ull, 0xC1E0000000000000ull, 0x41E0000000000000ull, 0x43E0000000000000ull,
      0x4330000000000001ull, 0xC3E0000000000001ull, 0x3FF8000000000000ull, 0x400921FB54442D18ull};
  uint64_t r = Next();
  switch (r & 7) {
    case 0: case 1: return kSpecial[(r >> 8) % (sizeof kSpecial / sizeof kSpecial[0])];
    case 2: case 3: {
      /* Single-precision values of moderate exponent, and their kin. */
      uint32_t w = (uint32_t)(r >> 16);
      w = (w & 0x807FFFFFu) | ((100u + (uint32_t)((r >> 48) % 56)) << 23);
      return SingleBits(w);
    }
    case 4: return SingleBits((uint32_t)(r >> 16));
    case 5: {
      /* Moderate doubles. */
      uint64_t v = Next();
      return (v & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(0x3C0 + (r >> 40) % 0x80) << 52);
    }
    default: return Next();
  }
}

/* FPSCR inputs: random exception bits, FR FI FPRF and rounding mode; the
   enables and NI clear (their effects are not modelled). */
static uint64_t Fpscr(void) {
  uint32_t v = (uint32_t)Next();
  v &= ~0x000000FCu; /* VE OE UE ZE XE NI */
  if (Next() & 1) v &= 0x00000003u; /* often a clean FPSCR */
  return v;
}

/* Vector lanes: words of floating-point interest (as singles), or random
   bytes. */
static void VecValue(uint8_t* v) {
  for (int k = 0; k < 16; k += 4) {
    uint32_t w;
    uint64_t r = Next();
    if (r & 1) {
      uint64_t d = FpValue();
      /* The double's single counterpart where it has one, else its top. */
      w = (uint32_t)((d >> 32 & 0xC0000000u) | (d >> 29 & 0x3FFFFFFFu));
    } else {
      w = (uint32_t)(r >> 16);
    }
    v[k] = (uint8_t)(w >> 24);
    v[k + 1] = (uint8_t)(w >> 16);
    v[k + 2] = (uint8_t)(w >> 8);
    v[k + 3] = (uint8_t)w;
  }
}

/* Base registers: the effective address is inside the window, naturally
   aligned for the access; the high word is random, which 32-bit mode
   ignores. */
static void Place(const struct FormInfo* f, struct NativeState* s) {
  uint32_t size = f->size ? (uint32_t)f->size : 1;
  uint32_t align = size > 16 ? 16 : size;
  uint32_t ea = ORACLE_WINDOW + (uint32_t)(Next() % (ORACLE_WINDOW_SIZE - size + 1));
  ea &= ~(align - 1);
  uint64_t high = Next() << 32;
  if (f->mem == FORM_MEM_DISP) {
    s->r[f->ra] = high | (uint32_t)(ea - (uint32_t)f->imm);
  } else if (f->ra == 0) {
    s->r[f->rb] = high | ea;
  } else if (f->ra == f->rb) {
    ea &= ~(2 * align - 1);
    s->r[f->ra] = ea / 2;
  } else {
    int32_t k = (int32_t)(Next() % 513) - 256;
    k &= ~(int32_t)(align - 1);
    s->r[f->rb] = (uint64_t)(int64_t)k;
    s->r[f->ra] = high | (uint32_t)(ea - (uint32_t)k);
  }
}

static void Copy(void* d, const void* s, uint32_t n) {
  uint8_t* a = d;
  const uint8_t* b = s;
  while (n--) *a++ = *b++;
}

int main(void) {
  uint8_t* buf = (uint8_t*)(uintptr_t)ORACLE_BUF;
  uint8_t* window = (uint8_t*)(uintptr_t)ORACLE_WINDOW;
  struct Record* rec = (struct Record*)(uintptr_t)ORACLE_RECORDS;
  uint32_t n = 0;
  for (int k = 0; k < kFormInfoCount; k++) {
    const struct FormInfo* f = &kFormInfo[k];
    if (!f->stub) continue;
    uint32_t primary = f->raw >> 26;
    int vectors = primary == 59 || primary == 63 || (primary >= 4 && primary <= 6) ? kVectorsPerFpForm
                                                                                    : kVectorsPerForm;
    for (int v = 0; v < vectors; v++, n++) {
      struct NativeState s __attribute__((aligned(16)));
      for (int r = 0; r < 32; r++) s.r[r] = Value();
      s.cr = (uint32_t)Next();
      s.xer = (uint32_t)Next() & 0xE000007Fu;
      s.lr = Value();
      s.ctr = Value();
      s.host_sp = 0;
      for (int r = 0; r < 32; r++) s.f[r] = FpValue();
      s.fpscr = Fpscr();
      s.fpscr_in = 0;
      s.pad = 0;
      for (int r = 0; r < 32; r++) VecValue(s.v[r]);
      for (int b = 0; b < 16; b++) s.vscr[b] = s.vscr_in[b] = 0;
      s.vscr[13] = (uint8_t)(Next() & 1); /* NJ */
      s.vscr[15] = (uint8_t)(Next() & 1); /* SAT */
      if (f->mem != FORM_MEM_NONE) Place(f, &s);
      OraclePattern(buf);
      for (uint32_t b = 0; b < ORACLE_WINDOW_SIZE; b += 8) {
        uint64_t x = Next();
        for (int j = 0; j < 8; j++) window[b + j] = (uint8_t)(x >> (8 * j));
      }
      rec[n].form = (uint32_t)k;
      rec[n].raw = f->raw;
      Copy(&rec[n].in, &s, sizeof s);
      Copy(rec[n].window_in, window, ORACLE_WINDOW_SIZE);
      f->stub(&s);
      s.host_sp = 0;
      rec[n].in.fpscr_in = s.fpscr_in;
      Copy(rec[n].in.vscr_in, s.vscr_in, 16);
      Copy(&rec[n].out, &s, sizeof s);
      Copy(rec[n].window_out, window, ORACLE_WINDOW_SIZE);
      rec[n].crc = OracleCrc32(buf, ORACLE_BUF_SIZE);
      rec[n].pad = 0;
    }
  }
  struct OracleDone* done = (struct OracleDone*)(uintptr_t)ORACLE_DONE;
  done->data = ORACLE_RECORDS;
  done->length = n * (uint32_t)sizeof(struct Record);
  return (int)n;
}
