// End-to-end oracle test for integer lifting: every distinct integer
// instruction form in the game EXE or the fixture (recomp_lift --int-test)
// runs lifted and natively on this CPU from the same random registers,
// flags and memory. Registers (except esp), the memory buffer and every
// flag the instruction defines must match bit for bit, and every form
// must run often enough (trials that fault natively are skipped). x86-64
// hosts only; dev-time.
//
// The memory operand is at a buffer mapped at the same low address in this
// process and in the guest arena (lift_form_mem.h), so register values
// (pointers included) are identical in both runs. The stack is in that
// buffer too: the native run switches to it for the instruction, so push
// and pop move the same memory and esp.

#include <asm/prctl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include "lift_int_forms.h"
#include "runtime/guest_memory.h"
#include "x86/lifted.h"

namespace recomp {
uint8_t* g_guest_base = nullptr;
void Unimplemented(Ctx&, uint32_t va, const char* what) {
  std::printf("unimplemented %08X %s\n", va, what);
  std::abort();
}
void BadJump(Ctx&, uint32_t from, uint32_t to) {
  std::printf("bad jump %08X -> %08X\n", from, to);
  std::abort();
}
// Trials that divide-fault natively are skipped before the lifted run.
void RaiseDivideError(Ctx&, uint32_t va) {
  std::printf("lifted divide error at %08X where the CPU had none\n", va);
  std::abort();
}
}  // namespace recomp

namespace {

constexpr uint32_t kBufSize = 0x4000;  // from kFormTarget - 0x1000
constexpr uint32_t kStack = 0x3800;     // esp's offset in the buffer

struct Regs {
  uint64_t r[8];      // 0x00 eax ecx edx ebx esp ebp esi edi
  uint64_t flags;     // 0x40
  uint64_t host_rsp;  // 0x48
};

using NativeFn = void (*)(Regs*);

void Stub(uint8_t* p, const IntForm& f) {
  static const uint8_t pre[] = {
      0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  // push
      0x49, 0x89, 0xFF,                                            // mov r15,rdi
      0x49, 0x8B, 0x47, 0x00, 0x49, 0x8B, 0x4F, 0x08, 0x49, 0x8B, 0x57, 0x10,
      0x49, 0x8B, 0x5F, 0x18, 0x49, 0x8B, 0x6F, 0x28, 0x49, 0x8B, 0x77, 0x30,
      0x49, 0x8B, 0x7F, 0x38,                                      // load regs
      0x41, 0xFF, 0x77, 0x40, 0x9D,                                // flags
      0x49, 0x89, 0x67, 0x48, 0x49, 0x8B, 0x67, 0x20};             // to esp
  static const uint8_t post[] = {
      0x49, 0x89, 0x67, 0x20, 0x49, 0x8B, 0x67, 0x48,              // back
      0x9C, 0x41, 0x8F, 0x47, 0x40,                                // flags out
      0x49, 0x89, 0x47, 0x00, 0x49, 0x89, 0x4F, 0x08, 0x49, 0x89, 0x57, 0x10,
      0x49, 0x89, 0x5F, 0x18, 0x49, 0x89, 0x6F, 0x28, 0x49, 0x89, 0x77, 0x30,
      0x49, 0x89, 0x7F, 0x38,                                      // regs out
      0xFC,                                                        // cld
      0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B, 0xC3};
  std::memcpy(p, pre, sizeof pre);
  p += sizeof pre;
  std::memcpy(p, f.bytes, f.len);
  p += f.len;
  std::memcpy(p, post, sizeof post);
}

sigjmp_buf g_jmp;
volatile int g_phase = 0;  // 1 native, 2 lifted
const char* volatile g_form = "";
// Runs on a stack of its own: a native fault may leave rsp in the buffer.
void OnFault(int sig) {
  if (g_phase == 1) siglongjmp(g_jmp, 1);
  std::printf("signal %d in the lifted run of %s\n", sig, g_form);
  std::_Exit(2);
}

uint32_t RandomValue(std::mt19937_64& g) {
  switch (g() % 10) {
    case 0: return 0;
    case 1: return 0xFFFFFFFFu;
    case 2: return 0x80000000u >> (g() % 3 * 8);
    case 3: return 0x7FFFFFFFu;
    case 4: return uint32_t(g() % 64);
    case 5: return uint32_t(int32_t(int8_t(g())));
    case 6: return 0xFFFFFF00u | uint32_t(g() & 0xFF);
    default: return uint32_t(g());
  }
}

}  // namespace

int main(int argc, char** argv) {
  int trials = argc > 1 ? std::atoi(argv[1]) : 200;
  const char* only = argc > 2 ? argv[2] : nullptr;
  const uint32_t kBuf = uint32_t(kFormTarget - 0x1000);
  const uint32_t seg_base = uint32_t(kFormTarget - kFormSegOffset);
  // fs: operands run natively as gs: (fs is the C library's).
  if (syscall(SYS_arch_prctl, ARCH_SET_GS, uintptr_t(seg_base))) {
    std::printf("cannot set the gs base\n");
    return 1;
  }
  recomp::GuestMemory mem;
  if (!mem.Reserve()) return 1;
  recomp::g_guest_base = mem.base();
  mem.Commit(kBuf, kBufSize, recomp::Prot::RW);
  auto* native_buf = static_cast<uint8_t*>(
      mmap(reinterpret_cast<void*>(uintptr_t(kBuf)), kBufSize, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
  if (native_buf != reinterpret_cast<uint8_t*>(uintptr_t(kBuf))) {
    std::printf("cannot map the native buffer at %08X\n", kBuf);
    return 1;
  }
  auto* page = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  struct sigaction sa{};
  sa.sa_handler = OnFault;
  sa.sa_flags = SA_NODEFER | SA_ONSTACK;
  static std::vector<uint8_t> alt(1 << 16);
  stack_t ss{};
  ss.ss_sp = alt.data();
  ss.ss_size = alt.size();
  sigaltstack(&ss, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::mt19937_64 g(777);
  std::vector<uint8_t> init(kBufSize);
  int failed_forms = 0;
  long checks = 0, skipped = 0;
  std::map<std::string, int> fails_by_mnemonic;
  std::map<std::string, long> skips_by_mnemonic;
  for (int fi = 0; fi < kIntFormCount; fi++) {
    const IntForm& f = kIntForms[fi];
    std::string text = f.text;
    if (only && text.find(only) == std::string::npos) continue;
    Stub(page, f);
    auto native = reinterpret_cast<NativeFn>(page);
    bool is_string = text.find("movs") != std::string::npos ||
                     text.find("stos") != std::string::npos ||
                     text.find("lods") != std::string::npos ||
                     text.find("scas") != std::string::npos ||
                     text.find("cmps") != std::string::npos;
    is_string = is_string && text.find("movsx") == std::string::npos;
    bool is_xlat = text.rfind("xlat", 0) == 0;
    bool is_bt = text.rfind("bt", 0) == 0 && f.m.mem;
    // Every other trial divides a small dividend (high half zero), so
    // divisions run without the quotient overflowing (#DE natively).
    bool is_div = text.rfind("div", 0) == 0 || text.rfind("idiv", 0) == 0;
    // A 16-bit double shift by a count (cl & 31) above 16 leaves its result
    // undefined: its trials keep the count below 16.
    bool is_dshift16 = (text.rfind("shld", 0) == 0 || text.rfind("shrd", 0) == 0) &&
                       std::regex_search(text, std::regex(", [a-d]x, cl$|, [sd]i, cl$|, bp, cl$"));
    uint32_t defined = (0x1u | 0x4u | 0x10u | 0x40u | 0x80u | 0x400u | 0x800u) & ~f.undef;
    int fails = 0;
    long form_checks = 0;
    for (int t = 0; t < trials; t++) {
      Regs in{};
      for (int r = 0; r < 8; r++) in.r[r] = RandomValue(g);
      if (is_bt)
        for (int r = 0; r < 8; r++) in.r[r] &= 0x7F;
      if (is_div && t % 2) {
        in.r[0] &= 0x7F;
        in.r[2] = 0;
      }
      if (is_dshift16) in.r[1] &= ~0x10ull;
      if (f.m.mem) AimForm(f.m, in.r, g() % 8);
      in.r[4] = kBuf + kStack;
      if (is_xlat) in.r[3] = f.m.seg ? kFormSegOffset : kFormTarget;  // ebx
      if (is_string) {
        in.r[6] = f.m.seg ? kFormSegOffset : kFormTarget;  // esi
        in.r[7] = kBuf + 0x2000;      // edi
        in.r[1] = g() % 9;            // ecx
      }
      in.flags = 0x2 | (g() & 0x8D5) | ((g() % 4 == 0) ? 0x400 : 0);
      in.flags &= is_string ? ~0ull : ~0x400ull;  // DF only matters there
      for (auto& b : init) b = uint8_t(g());

      Regs hw = in;
      std::memcpy(native_buf, init.data(), kBufSize);
      g_form = f.text;
      g_phase = 1;
      if (sigsetjmp(g_jmp, 1)) {
        asm volatile("cld");
        g_phase = 0;
        skipped++;
        skips_by_mnemonic[text.substr(0, text.find(' '))]++;
        continue;  // natively faulted (#DE, or an address off the buffer)
      }
      native(&hw);
      g_phase = 2;

      std::memcpy(mem.ptr(kBuf), init.data(), kBufSize);
      recomp::Ctx c{};
      c.eax = uint32_t(in.r[0]); c.ecx = uint32_t(in.r[1]); c.edx = uint32_t(in.r[2]);
      c.ebx = uint32_t(in.r[3]); c.ebp = uint32_t(in.r[5]); c.esi = uint32_t(in.r[6]);
      c.edi = uint32_t(in.r[7]);
      c.esp = uint32_t(in.r[4]);
      c.eflags = uint32_t(in.flags);
      c.fs_base = seg_base;
      f.fn(c);
      checks++;
      form_checks++;

      // The snippet's ret popped 4 bytes (it reads the buffer, writes
      // nothing).
      uint32_t got[8] = {c.eax, c.ecx, c.edx, c.ebx, c.esp - 4, c.ebp, c.esi, c.edi};
      static const char* names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
      std::string why;
      for (int r = 0; r < 8; r++)
        if (got[r] != uint32_t(hw.r[r])) why += std::string(" ") + names[r];
      uint32_t lf = c.eflags;
      if ((lf ^ uint32_t(hw.flags)) & defined) {
        char b[48];
        std::snprintf(b, sizeof b, " flags(%03X vs %03X)", uint32_t(hw.flags) & defined,
                      lf & defined);
        why += b;
      }
      if (std::memcmp(native_buf, mem.ptr(kBuf), kBufSize)) why += " mem";
      if (why.empty()) continue;
      if (++fails <= 2) {
        std::printf("FAIL %-36s (e.g. %08llX, base %d, index %d):%s\n", f.text,
                    (unsigned long long)f.example_va, f.m.base, f.m.index, why.c_str());
        std::printf("   in  ");
        for (int r = 0; r < 8; r++) std::printf(" %s=%08X", names[r], uint32_t(in.r[r]));
        std::printf(" fl=%03X\n   hw  ", uint32_t(in.flags));
        for (int r = 0; r < 8; r++) std::printf(" %s=%08X", names[r], uint32_t(hw.r[r]));
        std::printf(" fl=%03X\n   lift", uint32_t(hw.flags) & 0xFD5);
        for (int r = 0; r < 8; r++) std::printf(" %s=%08X", names[r], got[r]);
        std::printf(" fl=%03X\n", lf & 0xFD5);
      }
    }
    // A form that faults natively in most trials is barely tested.
    if (form_checks < trials / 4) {
      std::printf("FAIL %-36s (e.g. %08llX): %ld of %d trials ran\n", f.text,
                  (unsigned long long)f.example_va, form_checks, trials);
      fails++;
    }
    if (fails) {
      failed_forms++;
      fails_by_mnemonic[text.substr(0, text.find(' '))]++;
    }
  }
  std::printf("%d forms, %ld checks (%ld skipped: native faults), %d forms failed\n",
              kIntFormCount, checks, skipped, failed_forms);
  for (const auto& [m, n] : fails_by_mnemonic) std::printf("  %-10s %d forms\n", m.c_str(), n);
  std::printf("native faults (skipped trials) by mnemonic:\n");
  for (const auto& [m, n] : skips_by_mnemonic) std::printf("  %-10s %ld\n", m.c_str(), n);
  return failed_forms ? 1 : 0;
}
