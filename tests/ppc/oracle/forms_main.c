/* The form oracle: runs every instruction form recomp_lift_ppc --forms
   wrote a native stub for (forms_native.S, forms_native.c) from
   kVectorsPerForm random states, and records each run (oracle.h, struct
   Record) from ORACLE_RECORDS on. Bare metal, 32-bit mode (crt0.S); the
   host test (lift_forms_test.cpp) replays the records through the lifted
   forms. */
#include "oracle.h"

extern const struct FormInfo kFormInfo[];
extern const int kFormInfoCount;

enum { kVectorsPerForm = 96 };

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

/* Base registers: the effective address is inside the window, naturally
   aligned for the access; the high word is random, which 32-bit mode
   ignores. */
static void Place(const struct FormInfo* f, struct NativeState* s) {
  uint32_t size = f->size ? (uint32_t)f->size : 1;
  uint32_t align = size > 8 ? 8 : size;
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
    for (int v = 0; v < kVectorsPerForm; v++, n++) {
      struct NativeState s;
      for (int r = 0; r < 32; r++) s.r[r] = Value();
      s.cr = (uint32_t)Next();
      s.xer = (uint32_t)Next() & 0xE000007Fu;
      s.lr = Value();
      s.ctr = Value();
      s.host_sp = 0;
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
