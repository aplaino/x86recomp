// Differential test of every distinct instruction form in the fixture
// (recomp_lift_ppc --forms): each lifted form runs from the input states the
// oracle ran the original instruction from (oracle/forms_main.c, on a 970
// in 32-bit mode under qemu-system-ppc64), and must end in the same state
// bit for bit: general and floating-point registers, CR, XER, LR, CTR,
// FPSCR, the 512-byte window
// its memory operands are in and the CRC of the whole 64 KiB buffer. Only
// what the ISA leaves undefined is not compared (Undefined below); the
// lifter's choice there waits for the Xenon itself.
//   xbox360_lift_forms_test <records.bin>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "arena.h"
#include "lift_forms.h"
#include "oracle.h"
#include "ppc/lifted.h"

namespace recomp::ppc {
// Forms never trap, call or read the time base; the time base is 0.
void Sc(Ctx&, uint64_t va) { std::fprintf(stderr, "sc in a form at %llx\n", (unsigned long long)va), std::abort(); }
void Trap(Ctx&, uint32_t, uint64_t va) { std::fprintf(stderr, "trap in a form at %llx\n", (unsigned long long)va), std::abort(); }
uint64_t TimeBase(Ctx&) { return 0; }
void Import(Ctx&, const char*, uint32_t, uint64_t va) { std::fprintf(stderr, "import in a form at %llx\n", (unsigned long long)va), std::abort(); }
}  // namespace recomp::ppc

namespace {

// What the ISA leaves undefined in this run, and so the oracle's
// implementation chooses: the quotient of a divide by zero or of the most
// negative value by -1 (with CR0's LT, GT and EQ of its record form),
// XER's reserved bits, the high word of FCTIW(Z)'s and MFFS's FRT, and
// FPRF after the conversions to integer; and the FPSCR bits where the
// oracle departs from the ISA (UndefinedIn).
struct Undefined {
  int gpr = -1;
  uint32_t cr = 0;
  uint32_t xer = ~(0xE000007Fu);
  int fpr = -1;
  uint64_t fpr_bits = 0;
  uint32_t fpscr = 0;
};
Undefined UndefinedIn(uint32_t raw, const NativeState& in) {
  Undefined u;
  // Where the oracle departs from the ISA, which the model follows:
  // - FR: QEMU's is not "the rounding increased the magnitude" (spot
  //   checks with exact arithmetic);
  // - FX: QEMU sets it whenever an exception occurs, the ISA only when an
  //   exception bit goes from 0 to 1 (CR1 of the record forms copies it);
  // - FCMPU/FCMPO change FI and FPRF's C bit in QEMU, only FPCC in the ISA;
  // - FRES: QEMU's FPRF classifies its double intermediate, and its
  //   estimate of 1/±0 is ±0.5 (the ISA's, with ZX as QEMU sets it, ±inf).
  if (raw >> 26 == 59 || raw >> 26 == 63) {
    u.fpscr |= 0x80040000u;
    if (raw & 1) u.cr |= 0x08000000u;
    uint32_t xo = (raw >> 1) & 0x3FF;
    if (raw >> 26 == 63 && (xo == 0 || xo == 32)) u.fpscr |= 0x00030000u;
    if (raw >> 26 == 59 && (xo & 31) == 24) {
      u.fpscr |= 0x1Fu << 12;
      if (!(in.f[(raw >> 11) & 31] << 1)) {
        u.fpr = int((raw >> 21) & 31);
        u.fpr_bits = ~0ull;
      }
    }
  }
  // FCTIW(Z): FRT's high word and FPRF; MFFS: FRT's high word.
  if (raw >> 26 == 63) {
    uint32_t xo = (raw >> 1) & 0x3FF;
    if (xo == 14 || xo == 15 || xo == 814 || xo == 815) {
      u.fpr = int((raw >> 21) & 31);
      u.fpr_bits = xo < 100 ? 0xFFFFFFFF00000000ull : 0;
      u.fpscr |= 0x1Fu << 12;
    } else if (xo == 583) {
      u.fpr = int((raw >> 21) & 31);
      u.fpr_bits = 0xFFFFFFFF00000000ull;
    }
    return u;
  }
  if (raw >> 26 != 31) return u;
  uint32_t xo = (raw >> 1) & 0x1FF;
  bool word = xo == 459 || xo == 491, sign = xo == 491 || xo == 489;
  if (!word && xo != 457 && xo != 489) return u;
  uint64_t a = in.r[(raw >> 16) & 31], b = in.r[(raw >> 11) & 31];
  bool bad = word ? uint32_t(b) == 0 || (sign && int32_t(a) == INT32_MIN && int32_t(b) == -1)
                  : b == 0 || (sign && int64_t(a) == INT64_MIN && int64_t(b) == -1);
  if (bad) {
    u.gpr = int((raw >> 21) & 31);
    if (raw & 1) u.cr = 0xE0000000u;
  }
  return u;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: xbox360_lift_forms_test records.bin\n");
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (data.empty() || data.size() % sizeof(Record)) {
    std::fprintf(stderr, "%s: no records\n", argv[1]);
    return 1;
  }
  // The oracle is big-endian: its records' integers are swapped here.
  size_t n = data.size() / sizeof(Record);
  std::vector<Record> recs(n);
  std::memcpy(recs.data(), data.data(), data.size());
  auto swap_state = [](NativeState& st) {
    for (uint64_t& v : st.r) v = __builtin_bswap64(v);
    st.cr = __builtin_bswap64(st.cr);
    st.xer = __builtin_bswap64(st.xer);
    st.lr = __builtin_bswap64(st.lr);
    st.ctr = __builtin_bswap64(st.ctr);
    for (uint64_t& v : st.f) v = __builtin_bswap64(v);
    st.fpscr = __builtin_bswap64(st.fpscr);
    st.fpscr_in = __builtin_bswap64(st.fpscr_in);
  };
  for (Record& r : recs) {
    r.form = __builtin_bswap32(r.form);
    r.raw = __builtin_bswap32(r.raw);
    r.crc = __builtin_bswap32(r.crc);
    swap_state(r.in);
    swap_state(r.out);
  }

  uint8_t* mem = MapArena();
  uint8_t pattern[ORACLE_BUF_SIZE];
  OraclePattern(pattern);
  std::map<uint32_t, int> failures, runs;
  // Per failing form: the FPSCR and CR bits that differed, and how often.
  std::map<uint32_t, std::map<uint32_t, int>> fpscr_diff, cr_diff;
  int shown = 0;
  for (size_t k = 0; k < n; k++) {
    const Record& r = recs[k];
    if (r.form >= uint32_t(kFormCount) || kForms[r.form].raw != r.raw) {
      std::fprintf(stderr, "record %zu: form %u does not match this build's forms\n", k, r.form);
      return 1;
    }
    std::memcpy(mem + ORACLE_BUF, pattern, sizeof pattern);
    std::memcpy(mem + ORACLE_WINDOW, r.window_in, ORACLE_WINDOW_SIZE);
    recomp::ppc::Ctx c{};
    std::memcpy(c.r, r.in.r, sizeof c.r);
    c.cr = uint32_t(r.in.cr);
    c.xer = uint32_t(r.in.xer);
    c.lr = r.in.lr;
    c.ctr = r.in.ctr;
    std::memcpy(c.f, r.in.f, sizeof c.f);
    c.fpscr = uint32_t(r.in.fpscr_in);
    c.mem = mem;
    kForms[r.form].fn(c);
    runs[r.form]++;

    std::string why;
    Undefined u = UndefinedIn(r.raw, r.in);
    for (int g = 0; g < 32; g++)
      if (g != u.gpr && c.r[g] != r.out.r[g]) {
        char b[96];
        std::snprintf(b, sizeof b, " r%d %016llx want %016llx", g, (unsigned long long)c.r[g],
                      (unsigned long long)r.out.r[g]);
        why += b;
      }
    auto spr = [&](const char* name, uint64_t got, uint64_t want) {
      if (got == want) return;
      char b[96];
      std::snprintf(b, sizeof b, " %s %llx want %llx", name, (unsigned long long)got, (unsigned long long)want);
      why += b;
    };
    spr("cr", c.cr & ~u.cr, r.out.cr & ~u.cr);
    spr("xer", c.xer & ~u.xer, r.out.xer & ~u.xer);
    spr("lr", c.lr, r.out.lr);
    spr("ctr", c.ctr, r.out.ctr);
    for (int g = 0; g < 32; g++) {
      uint64_t mask = g == u.fpr ? ~u.fpr_bits : ~0ull;
      if ((c.f[g] & mask) != (r.out.f[g] & mask)) {
        char b[96];
        std::snprintf(b, sizeof b, " f%d %016llx want %016llx", g, (unsigned long long)c.f[g],
                      (unsigned long long)r.out.f[g]);
        why += b;
      }
    }
    spr("fpscr", c.fpscr & ~u.fpscr, uint32_t(r.out.fpscr) & ~u.fpscr);
    if (std::memcmp(mem + ORACLE_WINDOW, r.window_out, ORACLE_WINDOW_SIZE)) why += " memory (window)";
    else if (OracleCrc32(mem + ORACLE_BUF, ORACLE_BUF_SIZE) != r.crc) why += " memory (outside the window)";
    if (!why.empty()) {
      uint32_t fd = (c.fpscr ^ uint32_t(r.out.fpscr)) & ~u.fpscr, cd = (c.cr ^ uint32_t(r.out.cr)) & ~u.cr;
      if (fd) fpscr_diff[r.form][fd]++;
      if (cd) cr_diff[r.form][cd]++;
      if (failures[r.form]++ < 3 && shown++ < 60) {
        std::printf("FAIL form %u %08x %s at %llx:%s\n", r.form, r.raw, kForms[r.form].name,
                    (unsigned long long)kForms[r.form].va, why.c_str());
        // The operands, for floating point.
        if (r.raw >> 26 == 59 || r.raw >> 26 == 63)
          std::printf("     in: fpscr %08x fra %016llx frb %016llx frc %016llx\n", uint32_t(r.in.fpscr_in),
                      (unsigned long long)r.in.f[(r.raw >> 16) & 31], (unsigned long long)r.in.f[(r.raw >> 11) & 31],
                      (unsigned long long)r.in.f[(r.raw >> 6) & 31]);
      }
    }
  }
  for (const auto& [form, n] : failures) {
    std::printf("form %u %s: %d of %d records differ;", form, kForms[form].name, n, runs[form]);
    for (const auto& [bits, k] : fpscr_diff[form]) std::printf(" fpscr^%08x x%d", bits, k);
    for (const auto& [bits, k] : cr_diff[form]) std::printf(" cr^%08x x%d", bits, k);
    std::printf("\n");
  }
  int without = 0;
  for (int k = 0; k < kFormCount; k++)
    if (!runs.count(k)) without++;
  std::printf("%zu records over %zu forms; %zu forms failed; %d forms have no oracle run\n", n,
              runs.size(), failures.size(), without);
  return failures.empty() ? 0 : 1;
}
