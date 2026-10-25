/* What the bare-metal oracle programs (run by qemu_run.py on
   qemu-system-ppc64, a PowerPC 970 in 32-bit mode like a Xenon running a
   title) and the host tests share: the memory layout, the done area, and
   the form test's state and records. C, for both sides. */
#ifndef XBOX360_TESTS_ORACLE_H
#define XBOX360_TESTS_ORACLE_H

#include <stdint.h>

/* Physical (= guest) addresses. The machine has 3 GiB of RAM from 0, so a
   title's addresses (0x82000000) are RAM too. */
#define ORACLE_STACK_TOP 0x00D00000u
#define ORACLE_DONE 0x00E00000u      /* struct OracleDone */
#define ORACLE_OUTPUT 0x00E00100u    /* the fixture's printed text */
#define ORACLE_BUF 0x01000000u       /* the form test's memory operands */
#define ORACLE_BUF_SIZE 0x10000u
#define ORACLE_WINDOW (ORACLE_BUF + ORACLE_BUF_SIZE / 2 - 256)
#define ORACLE_WINDOW_SIZE 512u
#define ORACLE_RECORDS 0x10000000u   /* the form test's records */
#define ORACLE_PATTERN_SEED 0x5EEDull
#define ORACLE_DONE_MAGIC 0xD0E0D0E0u

/* Written when a program stops: by `sc` (status 0, code from r3) or by an
   interrupt (status = its vector, srr0 where it hit). qemu_run.py dumps
   [data, data + length) when status is 0. */
struct OracleDone {
  uint32_t magic, status, code, length, data, srr0;
};

/* The form test: one instruction run from `in`, ending in `out`. r31 is
   the stub's state pointer, so no form names it. */
struct NativeState {
  uint64_t r[32];
  uint64_t cr, xer, lr, ctr;
  uint64_t host_sp; /* the stub's own */
};

enum FormMem { FORM_MEM_NONE, FORM_MEM_DISP, FORM_MEM_INDEXED, FORM_MEM_MULTIPLE };

struct FormInfo {
  void (*stub)(struct NativeState*); /* 0: not run on the oracle */
  uint32_t raw;
  int mem, ra, rb, size;
  long long imm;
  int aligned;
};

struct Record {
  uint32_t form, raw;
  struct NativeState in;
  uint8_t window_in[ORACLE_WINDOW_SIZE];
  struct NativeState out;
  uint8_t window_out[ORACLE_WINDOW_SIZE];
  uint32_t crc, pad; /* CRC-32 of the whole buffer afterwards */
};

/* Shared by both sides so they agree byte for byte. */
static inline uint64_t OracleSplitMix(uint64_t* s) {
  uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static inline void OraclePattern(uint8_t* buf) {
  uint64_t s = ORACLE_PATTERN_SEED;
  for (uint32_t k = 0; k < ORACLE_BUF_SIZE; k += 8) {
    uint64_t v = OracleSplitMix(&s);
    for (int b = 0; b < 8; b++) buf[k + b] = (uint8_t)(v >> (8 * b));
  }
}
/* zlib's CRC-32 (reflected 0xEDB88320), bit by bit. */
static inline uint32_t OracleCrc32(const uint8_t* p, uint32_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}

#endif
