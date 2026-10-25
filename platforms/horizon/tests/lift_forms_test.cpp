// Differential test of every distinct instruction form in the fixture NRO
// (recomp_lift_a64 --forms): each lifted form runs from the input states the
// Unicorn oracle (tests/oracle/oracle.py forms) ran the original
// instruction from, and must end in the same state bit for bit: general
// registers, SP, NZCV, TPIDR_EL0 and a 64 KiB memory buffer.
//   horizon_lift_forms_test <fixture.nro> <vectors.bin>
//
// The fixture is mapped at its console address and the buffer at the same
// address as in the oracle, so addresses (literal loads, ADR, pointers in
// registers) are identical in both runs.
//
// On an arm64 host the CPU itself is a second oracle: each form that has a
// native stub (forms_native.S) also runs as the original instruction from
// the same input, and the lifted result must match it too. Where Unicorn
// and the CPU disagree the CPU is right; those cases are counted apart.

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "lift_forms.h"
#include "horizon/layout.h"
#include "horizon/nso_image.h"
#include "a64/platform.h"

namespace {

// Repeated in tests/oracle/oracle.py.
constexpr uint64_t kBuf = 0x6000000000ull, kBufSize = 0x10000;
constexpr uint64_t kWindow = kBuf + kBufSize / 2 - 256, kWindowSize = 512;
constexpr uint64_t kPatternSeed = 0x5EED;

struct State {
  uint64_t x[31], sp, nzcv, tpidr;
};
struct Record {
  uint32_t form, raw;
  State in;
  uint8_t window[kWindowSize];
  State out;
  uint32_t crc, pad;
};
static_assert(sizeof(Record) == 8 + 34 * 8 + kWindowSize + 34 * 8 + 8);

uint64_t SplitMix(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// zlib's CRC-32 (reflected 0xEDB88320), as Python's zlib.crc32.
uint32_t Crc32(const uint8_t* p, size_t n) {
  static uint32_t table[256];
  if (!table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

#ifdef __aarch64__
// A native run that faults (an address off the buffer) is skipped. The
// stub runs with SP pointing at its NativeState, so the handler needs its
// own stack.
sigjmp_buf g_jmp;
volatile sig_atomic_t g_in_native = 0;
void OnFault(int sig) {
  if (g_in_native) siglongjmp(g_jmp, 1);
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}
void InstallFaultHandler() {
  static std::vector<uint8_t> stack(1 << 16);
  stack_t ss{};
  ss.ss_sp = stack.data();
  ss.ss_size = stack.size();
  sigaltstack(&ss, nullptr);
  struct sigaction sa{};
  sa.sa_handler = OnFault;
  sa.sa_flags = SA_ONSTACK | SA_NODEFER;
  for (int sig : {SIGSEGV, SIGBUS, SIGILL}) sigaction(sig, &sa, nullptr);
}
#endif

#ifdef __APPLE__
constexpr bool kX18Reserved = true;
#else
constexpr bool kX18Reserved = false;
#endif

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: horizon_lift_forms_test <fixture.nro> <vectors.bin>\n");
    return 2;
  }
  recomp::horizon::Module m;
  std::string err;
  if (!recomp::horizon::LoadNro(argv[1], m, err) || !recomp::horizon::Relocate(m, recomp::horizon::kMainBase, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  if (!recomp::a64::platform::MapFixed(recomp::horizon::kMainBase, m.image.size()) ||
      !recomp::a64::platform::MapFixed(kBuf, kBufSize)) {
    std::fprintf(stderr, "cannot map the fixture or the buffer\n");
    return 1;
  }
  std::memcpy(reinterpret_cast<void*>(recomp::horizon::kMainBase), m.image.data(), m.image.size());
  std::vector<uint8_t> pattern(kBufSize);
  uint64_t seed = kPatternSeed;
  for (size_t i = 0; i < kBufSize; i += 8) {
    uint64_t v = SplitMix(seed);
    std::memcpy(&pattern[i], &v, 8);
  }

  std::ifstream f(argv[2], std::ios::binary);
  char magic[8];
  if (!f.read(magic, 8) || std::memcmp(magic, "NXFV0001", 8)) {
    std::fprintf(stderr, "%s: not a vector file\n", argv[2]);
    return 1;
  }
  auto* buf = reinterpret_cast<uint8_t*>(kBuf);
  std::map<int, int> fails, trials, native_fails;
  std::map<int, long> unicorn_wrong;
  long checks = 0, native_checks = 0, native_faults = 0;
#ifdef __aarch64__
  InstallFaultHandler();
#endif
  Record r;
  while (f.read(reinterpret_cast<char*>(&r), sizeof r)) {
    if (int(r.form) >= kFormCount || kForms[r.form].raw != r.raw) {
      std::fprintf(stderr, "vector for form %u does not match the lifted forms\n", r.form);
      return 1;
    }
    std::memcpy(buf, pattern.data(), kBufSize);
    std::memcpy(buf + (kWindow - kBuf), r.window, kWindowSize);
    recomp::a64::Ctx c{};
    std::memcpy(c.x, r.in.x, sizeof c.x);
    c.sp = r.in.sp;
    c.nzcv = uint32_t(r.in.nzcv);
    c.tpidr_el0 = r.in.tpidr;
    kForms[r.form].fn(c);
    checks++;
    trials[r.form]++;

    std::string why;
    for (int i = 0; i < 31; i++)
      if (c.x[i] != r.out.x[i]) why += " x" + std::to_string(i);
    if (c.sp != r.out.sp) why += " sp";
    if (c.nzcv != r.out.nzcv) why += " nzcv";
    if (c.tpidr_el0 != r.out.tpidr) why += " tpidr_el0";
    uint32_t crc = Crc32(buf, kBufSize);
    if (crc != r.crc) why += " memory";

    const Form& form = kForms[r.form];
    if (form.native && !(kX18Reserved && form.uses_x18)) {
      std::memcpy(buf, pattern.data(), kBufSize);
      std::memcpy(buf + (kWindow - kBuf), r.window, kWindowSize);
      NativeState ns{};
      std::memcpy(ns.x, r.in.x, sizeof ns.x);
      ns.nzcv = r.in.nzcv;
      bool faulted = false;
#ifdef __aarch64__
      g_in_native = 1;
      if (sigsetjmp(g_jmp, 1)) faulted = true;
      else form.native(&ns);
      g_in_native = 0;
#endif
      if (faulted) {
        native_faults++;
      } else {
        native_checks++;
        // macOS may zero x18 under any form (forms using it are skipped
        // above), so the CPU is no oracle for it; Unicorn still is.
        if (kX18Reserved) ns.x[18] = c.x[18];
        uint32_t native_crc = Crc32(buf, kBufSize);
        bool lifted_ok = native_crc == crc && ns.nzcv == c.nzcv &&
                         !std::memcmp(ns.x, c.x, sizeof ns.x);
        bool unicorn_ok = native_crc == r.crc && ns.nzcv == r.out.nzcv &&
                          !std::memcmp(ns.x, r.out.x, sizeof ns.x);
        if (!unicorn_ok) unicorn_wrong[r.form]++;
        if (!lifted_ok && native_fails[r.form]++ < 3)
          std::printf("FAIL form %u (%s %08X at %llX): lifted differs from this CPU\n", r.form,
                      form.name, form.raw, static_cast<unsigned long long>(form.va));
      }
    }
    if (why.empty()) continue;
    if (fails[r.form]++ < 3) {
      const Form& fm = kForms[r.form];
      std::printf("FAIL form %u (%s %08X at %llX):%s\n", r.form, fm.name, fm.raw,
                  static_cast<unsigned long long>(fm.va), why.c_str());
      for (int i = 0; i < 31; i++)
        if (c.x[i] != r.out.x[i] || c.x[i] != r.in.x[i])
          std::printf("    x%-2d in %016llx  oracle %016llx  lifted %016llx\n", i,
                      static_cast<unsigned long long>(r.in.x[i]),
                      static_cast<unsigned long long>(r.out.x[i]),
                      static_cast<unsigned long long>(c.x[i]));
      std::printf("    nzcv in %08llx  oracle %08llx  lifted %08x\n",
                  static_cast<unsigned long long>(r.in.nzcv),
                  static_cast<unsigned long long>(r.out.nzcv), c.nzcv);
    }
  }
  int untested = 0;
  for (int i = 0; i < kFormCount; i++)
    if (!trials.count(i)) {
      untested++;
      std::printf("no vectors for form %d (%s %08X at %llX)\n", i, kForms[i].name, kForms[i].raw,
                  static_cast<unsigned long long>(kForms[i].va));
    }
  std::printf("%ld checks over %zu of %d forms, %zu forms failed, %d untested\n", checks,
              trials.size(), kFormCount, fails.size(), untested);
#ifdef __aarch64__
  for (const auto& [form, n] : unicorn_wrong)
    std::printf("unicorn differs from this CPU on form %d (%s %08X): %ld trials\n", form,
                kForms[form].name, kForms[form].raw, n);
  std::printf("native: %ld checks against this CPU (%ld faulted), %zu forms failed\n",
              native_checks, native_faults, native_fails.size());
#endif
  return fails.empty() && native_fails.empty() && !untested ? 0 : 1;
}
