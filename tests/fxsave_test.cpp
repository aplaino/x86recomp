// fxsave_test: FXSAVE/FXRSTOR (sse::Fxsave, sse::Fxrstor) against the host
// CPU (x86-64 hosts). Each trial gives the CPU and the model the same
// random x87 state (FRSTOR of a random image), MXCSR and XMM0-15, then
// compares the FXSAVE areas (both forms, written over the same random
// bytes); and loads both from a random area with FXRSTOR, comparing what
// FNSAVE, STMXCSR and the XMM registers show afterwards.
//
// Where the host may differ from the modelled CPU without either being
// wrong, the test checks the model's value instead of comparing:
// MXCSR_MASK (0xFFFF: the bits the model's LDMXCSR takes; CPUs with
// misaligned-SSE mode report bit 17 too), the x87 pointers and opcode
// while no exception is pending (the modelled family saves and restores
// them only with FSW.ES set; later CPUs always do), and the 32-bit form's
// FCS and FDS selectors, which the model does not keep (zero, as on CPUs
// that deprecate them).

#include <cstdio>
#include <cstring>
#include <random>

#include "runtime/ctx.h"
#include "runtime/sse.h"
#include "runtime/x87.h"

namespace recomp {
void FatalF(const char* fmt, ...);
}

namespace {

int failures = 0;
long checks = 0;

void Fail(const char* what, int trial, const uint8_t* hw, const uint8_t* model, int n) {
  if (++failures > 12) return;
  std::printf("FAIL %s, trial %d:\n", what, trial);
  for (int i = 0; i < n; i++)
    if (hw[i] != model[i]) std::printf("  byte %3d: hw %02X model %02X\n", i, hw[i], model[i]);
}

struct Native {
  alignas(16) uint8_t xmm[16][16];
  uint32_t mxcsr;
};

// FRSTOR img, LDMXCSR, XMM0-15, then FXSAVE (rex_w: FXSAVE64) into out.
void HwSave(const uint8_t* img, const Native& in, uint8_t* out, bool rex_w) {
  uint32_t host;
#define LOAD_XMM                                                                      \
  "movdqu 0x00(%2), %%xmm0\n movdqu 0x10(%2), %%xmm1\n movdqu 0x20(%2), %%xmm2\n"     \
  "movdqu 0x30(%2), %%xmm3\n movdqu 0x40(%2), %%xmm4\n movdqu 0x50(%2), %%xmm5\n"     \
  "movdqu 0x60(%2), %%xmm6\n movdqu 0x70(%2), %%xmm7\n movdqu 0x80(%2), %%xmm8\n"     \
  "movdqu 0x90(%2), %%xmm9\n movdqu 0xA0(%2), %%xmm10\n movdqu 0xB0(%2), %%xmm11\n"   \
  "movdqu 0xC0(%2), %%xmm12\n movdqu 0xD0(%2), %%xmm13\n movdqu 0xE0(%2), %%xmm14\n"  \
  "movdqu 0xF0(%2), %%xmm15\n"
#define CLOBBERS                                                                       \
  "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",   \
      "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "st", "st(1)",   \
      "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)"
  if (rex_w)
    asm volatile("stmxcsr (%4)\n frstor (%0)\n ldmxcsr (%1)\n" LOAD_XMM
                 "fxsave64 (%3)\n fninit\n ldmxcsr (%4)\n"
                 :
                 : "r"(img), "r"(&in.mxcsr), "r"(in.xmm), "r"(out), "r"(&host)
                 : CLOBBERS);
  else
    asm volatile("stmxcsr (%4)\n frstor (%0)\n ldmxcsr (%1)\n" LOAD_XMM
                 "fxsave (%3)\n fninit\n ldmxcsr (%4)\n"
                 :
                 : "r"(img), "r"(&in.mxcsr), "r"(in.xmm), "r"(out), "r"(&host)
                 : CLOBBERS);
}

// FXRSTOR64 area, then FNSAVE into img, STMXCSR and XMM0-15 into out.
void HwRestore(const uint8_t* area, uint8_t* img, Native* out) {
  uint32_t host;
  asm volatile(
      "stmxcsr (%3)\n fxrstor64 (%0)\n fnsave (%1)\n stmxcsr (%4)\n"
      "movdqu %%xmm0, 0x00(%2)\n movdqu %%xmm1, 0x10(%2)\n movdqu %%xmm2, 0x20(%2)\n"
      "movdqu %%xmm3, 0x30(%2)\n movdqu %%xmm4, 0x40(%2)\n movdqu %%xmm5, 0x50(%2)\n"
      "movdqu %%xmm6, 0x60(%2)\n movdqu %%xmm7, 0x70(%2)\n movdqu %%xmm8, 0x80(%2)\n"
      "movdqu %%xmm9, 0x90(%2)\n movdqu %%xmm10, 0xA0(%2)\n movdqu %%xmm11, 0xB0(%2)\n"
      "movdqu %%xmm12, 0xC0(%2)\n movdqu %%xmm13, 0xD0(%2)\n movdqu %%xmm14, 0xE0(%2)\n"
      "movdqu %%xmm15, 0xF0(%2)\n fninit\n ldmxcsr (%3)\n"
      :
      : "r"(area), "r"(img), "r"(out->xmm), "r"(&host), "r"(&out->mxcsr)
      : CLOBBERS);
}

void Put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }

// A random x87 state as an FNSAVE image, its status word consistent with
// its control word (ES and B set exactly when an exception is unmasked).
void RandomImage(std::mt19937_64& g, uint8_t img[108]) {
  for (int i = 0; i < 108; i++) img[i] = uint8_t(g());
  const uint16_t cw = uint16_t((g() & 0x1F3F) | 0x40);
  uint16_t sw = uint16_t(g() & 0x7F7F & ~0x80);
  if (g() % 2) sw &= ~0x3F;  // often nothing pending
  if (sw & ~cw & 0x3F) sw |= 0x8080;
  Put16(img + 0, cw);
  Put16(img + 4, sw);
  for (int r = 0; r < 8; r++) {  // some registers zero, infinite or denormal
    uint8_t* v = img + 28 + 10 * r;
    switch (g() % 6) {
      case 0: std::memset(v, 0, 8); Put16(v + 8, uint16_t(g() & 0x8000)); break;
      case 1: std::memset(v, 0, 7); v[7] = 0x80; Put16(v + 8, uint16_t((g() & 0x8000) | 0x7FFF)); break;
      case 2: v[7] &= 0x7F; Put16(v + 8, uint16_t(g() & 0x8000)); break;
      default: break;
    }
  }
}

bool PendingException(const uint8_t* sw_cw_img) {
  uint16_t cw, sw;
  std::memcpy(&cw, sw_cw_img, 2);
  std::memcpy(&sw, sw_cw_img + 4, 2);
  return sw & ~cw & 0x3F;
}

// CPUID 8000_0008h EBX bit 2: FXSAVE/XSAVE always save the x87 pointers.
bool HostAlwaysSavesPointers() {
  uint32_t a = 0x80000008, b, c = 0, d;
  asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
  return b & 4;
}

}  // namespace

int main() {
  std::mt19937_64 g(16);
  const bool always_ptrs = HostAlwaysSavesPointers();
  constexpr int kTrials = 20000;
  for (int t = 0; t < kTrials; t++) {
    alignas(16) uint8_t img[108];
    RandomImage(g, img);
    Native in{};
    for (auto& x : in.xmm)
      for (auto& b : x) b = uint8_t(g());
    in.mxcsr = uint32_t(g() & 0xFFFF) | 0x1F80;  // all exceptions masked
    const bool es = PendingException(img);
    for (bool rex_w : {false, true}) {
      alignas(16) uint8_t hw[512], model[512];
      for (auto& b : hw) b = uint8_t(g());
      std::memcpy(model, hw, 512);
      HwSave(img, in, hw, rex_w);
      recomp::Ctx c{};
      recomp::x87::Init(c.fpu);
      recomp::x87::Restore(c.fpu, img);
      c.mxcsr = in.mxcsr;
      std::memcpy(c.xmm, in.xmm, sizeof in.xmm);
      recomp::sse::Fxsave(c, model);
      checks++;
      uint32_t mask;
      std::memcpy(&mask, model + 28, 4);
      if (mask != 0xFFFF) Fail("MXCSR_MASK", t, hw, model, 32);
      std::memcpy(model + 28, hw + 28, 4);
      if (!rex_w) {
        if (model[12] | model[13] | model[20] | model[21]) Fail("selectors", t, hw, model, 24);
        std::memcpy(model + 12, hw + 12, 2);
        std::memcpy(model + 20, hw + 20, 2);
      }
      if (!es && always_ptrs) {
        static const uint8_t kZero[18] = {};
        if (std::memcmp(model + 6, kZero, 18)) Fail("pointers without ES", t, hw, model, 24);
        std::memcpy(model + 6, hw + 6, 18);
      }
      if (std::memcmp(hw, model, 512)) Fail(rex_w ? "fxsave64" : "fxsave", t, hw, model, 512);
    }

    // FXRSTOR of a random area: valid MXCSR, a consistent FSW.
    alignas(16) uint8_t area[512];
    for (auto& b : area) b = uint8_t(g());
    std::memcpy(area, img, 2);       // FCW
    std::memcpy(area + 2, img + 4, 2);  // FSW
    const uint32_t mx = in.mxcsr;
    std::memcpy(area + 24, &mx, 4);
    alignas(16) uint8_t hw_img[108], model_img[108];
    Native hw_out{};
    HwRestore(area, hw_img, &hw_out);
    recomp::Ctx c{};
    recomp::x87::Init(c.fpu);
    recomp::sse::Fxrstor(c, area, 0);
    recomp::x87::Save(c.fpu, model_img);
    checks++;
    if (!es && always_ptrs) {  // FIP, FOP, FDP and the selectors
      std::memcpy(model_img + 12, hw_img + 12, 16);
    }
    if (std::memcmp(hw_img, model_img, 108)) Fail("fxrstor: x87 state", t, hw_img, model_img, 108);
    if (hw_out.mxcsr != c.mxcsr || std::memcmp(hw_out.xmm, c.xmm, sizeof c.xmm))
      Fail("fxrstor: mxcsr/xmm", t, reinterpret_cast<uint8_t*>(&hw_out.mxcsr),
           reinterpret_cast<uint8_t*>(&c.mxcsr), 4);
  }
  std::printf("%ld checks (host %s the x87 pointers), %d failures\n", checks,
              always_ptrs ? "always saves" : "saves only with ES", failures);
  std::printf("%s\n", failures ? "fxsave: FAILED" : "fxsave: ok");
  return failures ? 1 : 0;
}
