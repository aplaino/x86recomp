// End-to-end oracle test for x87 lifting: every distinct x87 instruction
// form in the game EXE or the fixture (lifted by `recomp_lift --x87-test`)
// is run as lifted C++ on the x87 model and natively on this machine's
// FPU, from the same random state (control word, stack, memory operand,
// aimed at as in lift_form_mem.h). Results must match bit for bit:
// control/status/tag words, all registers, memory, and for fcomi/fnstsw
// the integer side effects. Built with LIFT_X87_X64 it runs the forms of
// x64 code.
// x86-64 hosts only; dev-time.
//
// Transcendentals (fsin, fptan, ...) are known to differ from hardware in
// the last bit for some inputs (the model rounds once, the hardware is
// within 1 ulp; tests/x86/x87_test.cpp): for them a 1-ulp register
// difference in a result the model rounded is counted separately and does
// not fail the test, nor does C1, which reports the direction of the
// hardware's own rounding. Exact results must match.

#include <asm/prctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "lift_x87_forms.h"
#ifdef LIFT_X87_X64
#define RECOMP_LIFT_X64 1
#endif
#include "runtime/guest_memory.h"
#include "x86/lifted.h"
#include "x86/x87.h"

namespace recomp {
uint8_t* g_guest_base = nullptr;
#ifdef LIFT_X87_X64
using GuestVa = uint64_t;
#else
using GuestVa = uint32_t;
#endif
// Referenced by lifted code that never runs here.
void Unimplemented(Ctx&, GuestVa va, const char* what) {
  std::printf("unimplemented %08llX %s\n", (unsigned long long)va, what);
  std::abort();
}
void BadJump(Ctx&, GuestVa from, GuestVa to) {
  std::printf("bad jump %08llX -> %08llX\n", (unsigned long long)from,
              (unsigned long long)to);
  std::abort();
}
}  // namespace recomp

namespace {

constexpr uint32_t kStackVa = 0x00300000;  // guest stack (ret pops 4 or 8)

using recomp::x87::F80;

// What the native stub reads and writes.
struct Regs {
  uint64_t r[8];       // 0x00 eax ecx edx ebx (esp) ebp esi edi
  uint64_t flags;      // 0x40: out
  const uint8_t* in;   // 0x48: frstor image
  uint8_t* out;        // 0x50: fnsave image
};
using NativeFn = void (*)(Regs*);

// push rbx, rbp, r12-r15; mov r15, rdi; mov r14, [r15+0x48]; frstor [r14];
// load regs; <insn>; pushfq; pop [r15+0x40]; store regs;
// mov r14, [r15+0x50]; fnsave [r14]; pop ...; ret
NativeFn MakeNative(uint8_t* page, const X87Form& f) {
  static const uint8_t pre[] = {
      0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  // push
      0x49, 0x89, 0xFF,                                            // mov r15,rdi
      0x4D, 0x8B, 0x77, 0x48, 0x41, 0xDD, 0x26,                    // frstor
      0x49, 0x8B, 0x47, 0x00, 0x49, 0x8B, 0x4F, 0x08, 0x49, 0x8B, 0x57, 0x10,
      0x49, 0x8B, 0x5F, 0x18, 0x49, 0x8B, 0x6F, 0x28, 0x49, 0x8B, 0x77, 0x30,
      0x49, 0x8B, 0x7F, 0x38};                                     // load regs
  static const uint8_t post[] = {
      0x9C, 0x41, 0x8F, 0x47, 0x40,                                // flags out
      0x49, 0x89, 0x47, 0x00, 0x49, 0x89, 0x4F, 0x08, 0x49, 0x89, 0x57, 0x10,
      0x49, 0x89, 0x5F, 0x18, 0x49, 0x89, 0x6F, 0x28, 0x49, 0x89, 0x77, 0x30,
      0x49, 0x89, 0x7F, 0x38,                                      // regs out
      0x4D, 0x8B, 0x77, 0x50, 0x41, 0xDD, 0x36,                    // fnsave
      0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B, 0xC3};
  uint8_t* p = page;
  std::memcpy(p, pre, sizeof pre);
  p += sizeof pre;
  std::memcpy(p, f.bytes, f.len);
  p += f.len;
  std::memcpy(p, post, sizeof post);
  return reinterpret_cast<NativeFn>(page);
}

F80 RandomValue(std::mt19937_64& g) {
  F80 v{};
  uint16_t sign = (g() & 1) ? 0x8000 : 0;
  switch (g() % 16) {
    case 0: return {0, sign};                                   // zero
    case 1: return {0x8000000000000000ull, uint16_t(sign | 0x7FFF)};  // inf
    case 2: return {0xC000000000000000ull | (g() >> 2), 0x7FFF};    // qnan
    case 3: return {g() >> 1, sign};                            // denormal
    case 4: {                                                   // small int
      int64_t n = int64_t(g() % 2001) - 1000;
      if (!n) return {0, sign};
      uint64_t a = uint64_t(n < 0 ? -n : n);
      int sh = __builtin_clzll(a);
      return {a << sh, uint16_t((n < 0 ? 0x8000 : 0) | (0x3FFF + 63 - sh))};
    }
    case 5:  // float-precision significand
      return {(g() | 0x8000000000000000ull) & 0xFFFFFF0000000000ull,
              uint16_t(sign | (0x3FFF + int(g() % 41) - 20))};
    case 6:  // double-precision significand
      return {(g() | 0x8000000000000000ull) & 0xFFFFFFFFFFFFF800ull,
              uint16_t(sign | (0x3FFF + int(g() % 81) - 40))};
    case 7:  // huge / tiny exponents
      return {g() | 0x8000000000000000ull,
              uint16_t(sign | ((g() & 1) ? 0x7F00 + g() % 0xFE : 1 + g() % 0x100))};
    default:
      v.sig = g() | 0x8000000000000000ull;
      v.se = uint16_t(sign | (0x3FFF + int(g() % 61) - 30));
      return v;
  }
}

// Tag of a non-empty register as FNSAVE writes it.
int Tag(const F80& v) {
  uint16_t e = v.se & 0x7FFF;
  if (e == 0) return v.sig == 0 ? 1 : 2;
  if (e == 0x7FFF || !(v.sig >> 63)) return 2;
  return 0;
}

const uint16_t kCws[] = {0x027F, 0x037F, 0x007F, 0x0C7F, 0x047F, 0x087F,
                         0x0E7F, 0x067F, 0x0A7F, 0x0F7F, 0x017F};

void RandomState(std::mt19937_64& g, uint8_t img[108]) {
  std::memset(img, 0, 108);
  uint16_t cw = kCws[g() % (sizeof kCws / sizeof *kCws)];
  int top = int(g() % 8);
  int depth = int(g() % 10);
  if (depth > 8) depth = 8 - int(g() % 3);  // bias towards some free slots
  uint16_t sw = uint16_t(top << 11) | uint16_t(g() & 0x4700);  // C3,C2,C1,C0
  uint16_t tw = 0xFFFF;
  // Under 0x027F, often registers a double holds exactly, so runs take
  // their host-double form (x87.h) and not only its exits.
  bool doubles = cw == 0x027F && (g() & 1);
  for (int i = 0; i < 8; i++) {  // i = ST(i)
    F80 v = RandomValue(g);
    if (doubles && (v.sig >> 63) && (v.se & 0x7FFF) != 0x7FFF && g() % 8) {
      v.sig &= ~0x7FFull;
      int e = int(v.se & 0x7FFF) - 0x3FFF;
      if (e < -300 || e > 300) v.se = uint16_t((v.se & 0x8000) | (0x3FFF + int(g() % 81) - 40));
    }
    std::memcpy(img + 28 + i * 10, &v.sig, 8);
    std::memcpy(img + 28 + i * 10 + 8, &v.se, 2);
    int phys = (top + i) & 7;
    if (i < depth) {
      tw = uint16_t(tw & ~(3 << (2 * phys))) | uint16_t(Tag(v) << (2 * phys));
    }
  }
  std::memcpy(img + 0, &cw, 2);
  std::memcpy(img + 4, &sw, 2);
  std::memcpy(img + 8, &tw, 2);
}

void RandomMem(std::mt19937_64& g, int bytes, uint8_t* m) {
  for (int i = 0; i < 16; i++) m[i] = uint8_t(g());
  if (bytes == 4 && (g() & 1)) {
    float f = float(std::ldexp(double(int64_t(g() % 2000001) - 1000000), int(g() % 30) - 15));
    std::memcpy(m, &f, 4);
  } else if (bytes == 8 && (g() & 1)) {
    double d = std::ldexp(double(int64_t(g() % 2000001) - 1000000), int(g() % 60) - 30);
    std::memcpy(m, &d, 8);
  } else if (bytes == 10 && (g() & 1)) {
    F80 v = RandomValue(g);
    std::memcpy(m, &v.sig, 8);
    std::memcpy(m + 8, &v.se, 2);
  } else if (bytes == 2 && (g() & 1)) {
    // Mostly sane control words for fldcw: masked exceptions.
    uint16_t cw = kCws[g() % (sizeof kCws / sizeof *kCws)];
    std::memcpy(m, &cw, 2);
  }
  if (bytes == 2 && (g() % 4)) {  // fldcw with unmasked exceptions would trap
    uint16_t v;
    std::memcpy(&v, m, 2);
    v |= 0x3F;
    std::memcpy(m, &v, 2);
  }
}

// An FXSAVE image of a random state for FXRSTOR (FXSAVE overwrites it):
// the x87 part as the model saves it, MXCSR the default, XMM registers
// zero (as the lifted run's).
void FxImage(std::mt19937_64& g, uint8_t m[512]) {
  uint8_t img[108];
  RandomState(g, img);
  recomp::x87::State s{};
  recomp::x87::Restore(s, img);
  std::memset(m, 0, 512);
  recomp::x87::FxSave(s, m);
  const uint32_t mxcsr = 0x1F80, mask = 0xFFFF;
  std::memcpy(m + 24, &mxcsr, 4);
  std::memcpy(m + 28, &mask, 4);
}

// Memory after the run: an FXSAVE image (512 bytes) compared in its x87
// part but for the last instruction's opcode and pointers (CPUs store them
// differently), and in the bytes it leaves alone; its MXCSR and XMM
// registers are the host's natively.
bool SameMem(const uint8_t* a, const uint8_t* b, int span) {
  if (span < 512) return std::memcmp(a, b, span) == 0;
  return std::memcmp(a, b, 6) == 0 && std::memcmp(a + 32, b + 32, 128) == 0 &&
         std::memcmp(a + 416, b + 416, 96) == 0;
}

bool IsTranscendental(const std::string& t) {
  for (const char* n : {"fsin", "fcos", "fptan", "fpatan", "f2xm1", "fyl2x"})
    if (t.rfind(n, 0) == 0) return true;
  return false;
}

// Distance in units of the last place between two finite F80s with the
// same sign and exponent (else huge).
uint64_t Ulps(const uint8_t* a, const uint8_t* b) {
  uint64_t sa, sb;
  uint16_t ea, eb;
  std::memcpy(&sa, a, 8);
  std::memcpy(&sb, b, 8);
  std::memcpy(&ea, a + 8, 2);
  std::memcpy(&eb, b + 8, 2);
  if ((ea ^ eb) & 0x8000) return ~0ull;  // different signs
  // (exponent, significand) is monotonic for normal values, so a result
  // one ulp below a power of two counts as 1 ulp from it.
  auto key = [](uint16_t e, uint64_t s) {
    return (static_cast<unsigned __int128>(e & 0x7FFF) << 63) | (s & ~(1ull << 63));
  };
  if (!(sa >> 63) || !(sb >> 63)) {  // denormals/zeros: same exponent only
    if (ea != eb) return ~0ull;
    return sa > sb ? sa - sb : sb - sa;
  }
  unsigned __int128 ka = key(ea, sa), kb = key(eb, sb);
  unsigned __int128 d = ka > kb ? ka - kb : kb - ka;
  return d > ~0ull ? ~0ull : static_cast<uint64_t>(d);
}

}  // namespace

int main(int argc, char** argv) {
  int trials = argc > 1 ? std::atoi(argv[1]) : 20000;
  // The operand's 16 bytes at kFormTarget, in a buffer mapped at the same
  // address natively; fs: operands run natively as gs: (fs is the C
  // library's).
  const uint32_t kBuf = uint32_t(kFormTarget - 0x1000), kBufSize = 0x2000;
  const uint32_t seg_base = uint32_t(kFormTarget - kFormSegOffset);
  if (syscall(SYS_arch_prctl, ARCH_SET_GS, uintptr_t(seg_base))) {
    std::printf("cannot set the gs base\n");
    return 1;
  }
  recomp::GuestMemory mem;
#ifdef LIFT_X87_X64
  if (!mem.Reserve(36)) return 1;
#else
  if (!mem.Reserve()) return 1;
#endif
  recomp::g_guest_base = mem.base();
  mem.Commit(kBuf, kBufSize, recomp::Prot::RW);
  mem.Commit(kStackVa - 0x1000, 0x2000, recomp::Prot::RW);
  auto* native_buf = static_cast<uint8_t*>(
      mmap(reinterpret_cast<void*>(uintptr_t(kBuf)), kBufSize, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
  if (native_buf != reinterpret_cast<uint8_t*>(uintptr_t(kBuf))) {
    std::printf("cannot map the native buffer at %08X\n", kBuf);
    return 1;
  }
  uint8_t* const hm = native_buf + (kFormTarget - kBuf);
  uint8_t* const lm = mem.ptr(uint32_t(kFormTarget));
  auto* page = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (page == MAP_FAILED) return 1;

  std::mt19937_64 g(12345);
  int failed_forms = 0;
  long checks = 0, approx = 0;
  for (int fi = 0; fi < kX87FormCount; fi++) {
    const X87Form& f = kX87Forms[fi];
    std::string text = f.text;
    NativeFn native = MakeNative(page, f);
    bool transc = IsTranscendental(text);
    bool flags = text.find("comi") != std::string::npos;
    bool ax = text.find("fnstsw ax") != std::string::npos;
    // Memory compared: the operand's 16 bytes and 16 past them (zero), or
    // an FXSAVE image.
    const int span = f.mem_bytes == 512 ? 512 : 32;
    int fails = 0;
    for (int t = 0; t < trials; t++) {
      uint8_t in[108], hw[108], lf[108], m0[512] = {};
      RandomState(g, in);
      RandomMem(g, f.mem_bytes, m0);
      if (f.mem_bytes == 512) FxImage(g, m0);
      Regs rg{};
      for (int r = 0; r < 8; r++) rg.r[r] = r == 4 ? 0 : uint32_t(g());
      if (f.m.mem) AimForm(f.m, rg.r, g() % 8);
      // Native.
      std::memcpy(hm, m0, span);
      Regs hr = rg;
      hr.in = in;
      hr.out = hw;
      native(&hr);
      uint64_t hflags = hr.flags, hrax = hr.r[0];
      // Lifted.
      std::memcpy(lm, m0, span);
      auto lifted = [&](const uint8_t* img, uint8_t* out) {
        recomp::Ctx c{};
        recomp::x87::Restore(c.fpu, img);
        // The registers' values are below 4 GiB: 32-bit code sees the same
        // in the low halves.
        c.rax = rg.r[0]; c.rcx = rg.r[1]; c.rdx = rg.r[2]; c.rbx = rg.r[3];
        c.rbp = rg.r[5]; c.rsi = rg.r[6]; c.rdi = rg.r[7];
        c.esp = kStackVa;
        c.mxcsr = 0x1F80;
        c.fs_base = c.gs_base = seg_base;
        f.fn(c);
        recomp::x87::Save(c.fpu, out);
        return c;
      };
      recomp::Ctx c = lifted(in, lf);
      checks++;
      const bool regs_same = std::memcmp(hw + 28, lf + 28, 80) == 0;
      // Compare: CW, SW, TW, registers; memory; flags/ax where relevant.
      std::string why;
      if (std::memcmp(hw, lf, 2)) why += " cw";
      {
        // C1 reports the rounding direction; for transcendentals the
        // hardware's own (tests/x86/x87_test.cpp).
        uint16_t hs, ls;
        std::memcpy(&hs, hw + 4, 2);
        std::memcpy(&ls, lf + 4, 2);
        if (transc) hs &= ~0x0200, ls &= ~0x0200;
        // Intel documents the FCOMI family as clearing C1, AMD CPUs keep
        // it; the model follows Intel (as tests/x86/x87_test.cpp says).
        if (flags) hs &= ~0x0200, ls &= ~0x0200;
        if (hs != ls) why += " sw";
      }
      if (std::memcmp(hw + 8, lf + 8, 2)) why += " tw";
      if (!SameMem(hm, lm, span)) why += " mem";
      // Whether the model rounded: PE from the same run with PE clear on
      // entry (memory is compared already; the rerun writes it again).
      bool rounded = false;
      if (transc && !regs_same) {
        uint8_t pin[108], pout[108];
        std::memcpy(pin, in, 108);
        pin[4] &= ~0x20;
        std::memcpy(lm, m0, span);
        lifted(pin, pout);
        rounded = pout[4] & 0x20;
      }
      for (int r = 0; r < 8; r++) {
        if (std::memcmp(hw + 28 + r * 10, lf + 28 + r * 10, 10) == 0) continue;
        if (rounded && Ulps(hw + 28 + r * 10, lf + 28 + r * 10) <= 1) continue;
        why += " st" + std::to_string(r);
      }
      if (flags) {
        uint32_t hf = uint32_t(hflags) & 0x45;  // CF PF ZF
        uint32_t lfl = c.eflags & 0x45;
        if (hf != lfl) why += " eflags";
      }
      if (ax && uint16_t(hrax) != uint16_t(c.eax)) why += " ax";
      if (why.empty()) {
        if (!regs_same) approx++;
        continue;
      }
      if (++fails <= 3) {
        std::printf("FAIL %-28s (form %d, e.g. %08X):%s\n", text.c_str(), fi,
                    f.example_va, why.c_str());
        auto dump = [](const char* who, const uint8_t* img) {
          uint16_t cw, sw, tw;
          std::memcpy(&cw, img, 2);
          std::memcpy(&sw, img + 4, 2);
          std::memcpy(&tw, img + 8, 2);
          std::printf("   %s cw=%04X sw=%04X tw=%04X", who, cw, sw, tw);
          for (int r = 0; r < 3; r++) {
            uint64_t s;
            uint16_t e;
            std::memcpy(&s, img + 28 + r * 10, 8);
            std::memcpy(&e, img + 28 + r * 10 + 8, 2);
            std::printf(" st%d=%04X:%016llX", r, e, (unsigned long long)s);
          }
          std::printf("\n");
        };
        dump("in  ", in);
        dump("hw  ", hw);
        dump("lift", lf);
        if (f.mem_bytes) {
          std::printf("   mem in ");
          for (int i = 0; i < std::min(f.mem_bytes, 16); i++) std::printf("%02X", m0[i]);
          std::printf(" hw ");
          for (int i = 0; i < std::min(f.mem_bytes, 16); i++) std::printf("%02X", hm[i]);
          std::printf(" lift ");
          for (int i = 0; i < std::min(f.mem_bytes, 16); i++) std::printf("%02X", lm[i]);
          std::printf("\n");
        }
      }
    }
    if (fails) {
      failed_forms++;
      std::printf("  -> %s: %d/%d trials differ\n", text.c_str(), fails, trials);
    }
  }
  std::printf("%d forms, %ld checks, %d forms failed; %ld transcendental "
              "results within 1 ulp\n",
              kX87FormCount, checks, failed_forms, approx);
  return failed_forms ? 1 : 0;
}
