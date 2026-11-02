// The flag analysis (tools/lifter/flags.h) against every flag spilled:
// the programs of lift_flags_test.s and lift_flags_indirect.s, lifted both
// ways (recomp_lift --flags-test), run from the same random registers,
// flags (DF included) and memory; registers, EFLAGS (each program stores
// them with pushfd after its function returns) and memory must match. The
// masks the analysis found are checked against what the programs need,
// and against more than they need where that is known. Portable.

#include <signal.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "lift_flags_forms.h"
#include "runtime/guest_memory.h"
#include "runtime/lifted.h"

namespace recomp {
uint8_t* g_guest_base = nullptr;
// The version of the program running (Dispatch looks targets up there).
const FlagsFn* g_fns = nullptr;
int g_fn_count = 0;
void Dispatch(Ctx& c, uint32_t target, uint32_t va) {
  for (int i = 0; i < g_fn_count; i++)
    if (g_fns[i].va == target) return g_fns[i].fn(c);
  std::printf("dispatch from %08X to %08X: no such function\n", va, target);
  std::abort();
}
void Unimplemented(Ctx&, uint32_t va, const char* what) {
  std::printf("unimplemented %08X %s\n", va, what);
  std::abort();
}
void BadJump(Ctx&, uint32_t from, uint32_t to) {
  std::printf("bad jump %08X -> %08X\n", from, to);
  std::abort();
}
void FatalF(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::printf("\n");
  std::abort();
}
}  // namespace recomp

namespace {

using recomp::Ctx;
constexpr uint32_t kBuf = 0x00200000, kBufSize = 0x1000;
constexpr uint32_t kStack = 0x002F0000, kStackSize = 0x10000;
constexpr uint32_t kCF = 0x1, kPF = 0x4, kZF = 0x40, kOF = 0x800, kAll = 0x8D5;

uint32_t RandomValue(std::mt19937_64& g) {
  switch (g() % 6) {
    case 0: return 0;
    case 1: return 0xFFFFFFFFu;
    case 2: return 0x80000000u;
    case 3: return uint32_t(g() % 4);
    default: return uint32_t(g());
  }
}

struct State {
  Ctx c;
  std::vector<uint8_t> buf, stack;
};

void Load(const State& s, Ctx& c) {
  c = s.c;
  std::memcpy(recomp::g_guest_base + kBuf, s.buf.data(), kBufSize);
  std::memcpy(recomp::g_guest_base + kStack, s.stack.data(), kStackSize);
}
State Save(const Ctx& c) {
  State s{c, std::vector<uint8_t>(kBufSize), std::vector<uint8_t>(kStackSize)};
  std::memcpy(s.buf.data(), recomp::g_guest_base + kBuf, kBufSize);
  std::memcpy(s.stack.data(), recomp::g_guest_base + kStack, kStackSize);
  return s;
}

int failures = 0;
const char* volatile g_running = "";

// Wrong flags can turn a loop of the programs into an endless one.
void OnAlarm(int) {
  std::printf("%s: no end (the optimised version loops: wrong flags)\n", g_running);
  std::_Exit(1);
}
void Expect(bool ok, const char* program, const char* what) {
  if (!ok) {
    std::printf("%s: %s\n", program, what);
    failures++;
  }
}

const FlagsMask* Mask(const FlagsProgram& p, const char* name) {
  for (int i = 0; i < p.mask_count; i++)
    if (!std::strcmp(p.masks[i].name, name)) return &p.masks[i];
  std::printf("%s: no function %s\n", p.name, name);
  std::abort();
}

// What the programs need (lower bounds), and where the analysis must not
// be more conservative than that.
void CheckMasks(const FlagsProgram& p) {
  auto reads = [&](const char* n) { return Mask(p, n)->reads; };
  auto ret = [&](const char* n) { return Mask(p, n)->ret | p.indirect; };
  if (!std::strcmp(p.name, "calls")) {
    Expect((ret("g_noflags") & kCF) && (reads("g_noflags") & kCF), p.name,
           "flags through a callee that writes none");
    Expect(reads("g_adc") == kCF, p.name, "adc at entry reads CF only");
    Expect((ret("g_test") & (kZF | kPF)) == (kZF | kPF) && reads("g_test") == 0, p.name,
           "test's ZF/PF returned, nothing read");
    Expect((reads("g_setb") & (kCF | kOF)) == (kCF | kOF), p.name, "setb/seto in a tail callee");
    Expect((ret("g_mid") & kCF) && (ret("g_stc") & kCF) && !(reads("g_stc") & kCF), p.name,
           "stc returned two calls up");
    Expect(reads("g_pushfd") == kAll, p.name, "pushfd reads every flag");
    Expect((reads("g_noflags2") & (kCF | kZF)) == (kCF | kZF), p.name,
           "rep cmpsb and shl by cl keep flags");
    Expect((reads("g_inc") & kCF) && !(reads("g_inc") & kZF) && (ret("g_inc") & kZF), p.name,
           "inc keeps CF, writes ZF");
    Expect((ret("g_tailtest") & kZF) && (ret("g_test") & kZF), p.name,
           "flags returned through a tail call");
    Expect(reads("p2") == 0 && reads("t1") == 0, p.name, "functions that write first read none");
    Expect(p.indirect == (kCF | kZF), p.name, "flags read after indirect calls are returned");
  } else {
    Expect(p.any == 0, p.name, "no function whose address is taken reads flags");
    Expect(p.indirect == (kCF | kZF), p.name, "indirect: CF and ZF read after indirect calls");
    Expect(reads("g_test") == 0 && reads("p1") == 0 && reads("p2") == 0 &&
               reads("g_jmpreg") == (kCF | kZF) && reads("g_none") == (kCF | kZF),
           p.name,
           "a write first reads none; an indirect jump and a callee writing none pass them on");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int trials = argc > 1 ? std::atoi(argv[1]) : 20000;
  recomp::GuestMemory mem;
  if (!mem.Reserve()) return 1;
  recomp::g_guest_base = mem.base();
  mem.Commit(kBuf, kBufSize, recomp::Prot::RW);
  mem.Commit(kStack, kStackSize, recomp::Prot::RW);
  signal(SIGALRM, OnAlarm);
  std::mt19937_64 g(4242);
  long runs = 0;
  for (int pi = 0; pi < kFlagsProgramCount; pi++) {
    const FlagsProgram& p = kFlagsPrograms[pi];
    CheckMasks(p);
    for (int k = 0; k < p.mask_count; k++) {
      const FlagsMask& m = p.masks[k];
      if (m.name[0] != 't') continue;  // the tests: tN calls pN, then pushfd
      int bad = 0;
      for (int t = 0; t < trials && bad < 5; t++) {
        State s{Ctx{}, std::vector<uint8_t>(kBufSize), std::vector<uint8_t>(kStackSize)};
        for (auto& b : s.buf) b = uint8_t(g() % 4);
        for (auto& b : s.stack) b = uint8_t(g());
        s.c.eax = RandomValue(g);
        s.c.ebx = g() % 3 ? RandomValue(g) : s.c.eax;
        s.c.ecx = uint32_t(g() % 4);
        s.c.edx = RandomValue(g);
        s.c.ebp = RandomValue(g);
        s.c.esi = kBuf + 0x400 + uint32_t(g() % 0x100);
        s.c.edi = kBuf + 0x800 + uint32_t(g() % 0x100);
        s.c.esp = kStack + kStackSize - 0x100;
        s.c.eflags = 0x202u | (uint32_t(g()) & kAll) | (g() % 2 ? 0x400u : 0);
        s.c.mxcsr = 0x1F80;
        if (p.name[0] == 'c' && std::strcmp(m.name, "t15") == 0)  // popfd ebx: no TF
          s.c.ebx &= ~0x100u;
        Ctx c;
        Load(s, c);
        recomp::g_fns = p.ref;
        recomp::g_fn_count = p.fn_count;
        recomp::g_guest_base = mem.base();
        // ref and opt list the same functions in the same order.
        int fi = 0;
        while (p.ref[fi].va != m.va) fi++;
        p.ref[fi].fn(c);
        const State want = Save(c);
        Load(s, c);
        recomp::g_fns = p.opt;
        g_running = m.name;
        alarm(10);
        p.opt[fi].fn(c);
        alarm(0);
        const State got = Save(c);
        runs++;
        std::string why;
        static const char* kNames[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
        const uint64_t* wr = &want.c.rax;
        const uint64_t* gr = &got.c.rax;
        for (int r = 0; r < 8; r++) {
          const uint32_t a = uint32_t(wr[r]), b = uint32_t(gr[r]);
          if (a != b) why += std::string(" ") + kNames[r];
        }
        if ((want.c.eflags & 0x240400u) != (got.c.eflags & 0x240400u)) why += " DF/AC/ID";
        if (want.buf != got.buf) why += " memory";
        if (want.stack != got.stack) why += " stack";
        if (!why.empty()) {
          std::printf("%s %s: trial %d differs:%s (eax=%08X ebx=%08X ecx=%u eflags=%03X)\n",
                      p.name, m.name, t, why.c_str(), s.c.eax, s.c.ebx, s.c.ecx, s.c.eflags);
          bad++;
          failures++;
        }
      }
    }
  }
  std::printf("lift_flags: %ld runs, %d failures\n", runs, failures);
  return failures ? 1 : 0;
}
