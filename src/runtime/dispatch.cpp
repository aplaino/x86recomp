// Indirect control flow and the fatal paths of lifted code.
//
// Dispatch() maps a guest address to its lifted function through a dense
// table over the .text of the EXE or of a lifted DLL, or to an HLE handler
// for thunk addresses. Anything else is fatal: there is no interpreter
// fallback (CLAUDE.md).

#include <execinfo.h>
#include <pthread.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <fcntl.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/lifted.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp {

uint8_t* g_guest_base = nullptr;
bool g_trace = false;

namespace {

// One per image: the EXE's first, then the lifted DLLs'. Built before any
// guest code runs and never changed afterwards.
struct CodeRange {
  uint32_t lo = 0, size = 0;
  std::vector<GuestFn> table;  // by va - lo
};
CodeRange g_exe;
std::vector<CodeRange> g_dlls;

CodeRange TextOf(const PeImage& img) {
  CodeRange r;
  for (const auto& s : img.sections)
    if (s.name == ".text") {
      r.lo = s.va;
      r.size = s.vsize;
    }
  r.table.assign(r.size, nullptr);
  for (uint32_t i = 0; i < kLiftedFunctionCount; i++) {
    const LiftedFn& f = kLiftedFunctions[i];
    if (f.va - r.lo < r.size) r.table[f.va - r.lo] = f.fn;
  }
  return r;
}

void DumpGuestState(const Ctx& c) {
  std::fprintf(stderr,
               "  eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
               "  esp=%08X ebp=%08X esi=%08X edi=%08X\n"
               "  flags: cf=%d zf=%d sf=%d of=%d df=%d  fpu: cw=%04X sw=%04X\n",
               c.eax, c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi, c.f.cf,
               c.f.zf, c.f.sf, c.f.of, c.f.df, x87::ControlWord(c.fpu),
               x87::StatusWord(c.fpu));
  if (!g_proc) return;
  // Guest backtrace along the EBP chain (VC6 code mostly keeps frames).
  std::fprintf(stderr, "  guest backtrace (ebp chain):\n");
  uint32_t ebp = c.ebp;
  for (int i = 0; i < 24; i++) {
    if (!g_proc->mem.IsCommitted(ebp) || !g_proc->mem.IsCommitted(ebp + 4))
      break;
    uint32_t ret = Rd32(ebp + 4);
    std::fprintf(stderr, "    #%-2d ret %08X  (frame %08X)\n", i, ret, ebp);
    uint32_t next = Rd32(ebp);
    if (next <= ebp) break;
    ebp = next;
  }
  std::fprintf(stderr, "  stack top:");
  for (int i = 0; i < 8; i++)
    if (g_proc->mem.IsCommitted(c.esp + 4 * i))
      std::fprintf(stderr, " %08X", Rd32(c.esp + 4 * i));
  std::fprintf(stderr, "\n");
}

}  // namespace

void FatalF(const char* fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  std::fflush(stdout);
  // The report goes to stderr and, so it survives a filtered or closed
  // terminal, to $XDG_DATA_HOME/<id>/crashes/crash-<time>-<pid>.txt.
  auto report = [&] {
    std::fprintf(stderr, "FATAL: %s\n", msg);
    if (Thread* t = CurrentThread()) {
      std::fprintf(stderr, "  guest thread %04X\n", t->tid);
      DumpGuestState(t->ctx);
    }
    std::fprintf(stderr, "  host backtrace:\n");
    std::fflush(stderr);
    void* frames[64];
    int nf = backtrace(frames, 64);
    backtrace_symbols_fd(frames, nf, 2);
  };
  report();
  std::string path = ReportPath("crash");
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd >= 0) {
    std::fprintf(stderr, "  (report saved to %s)\n", path.c_str());
    std::fflush(stderr);
    dup2(fd, 2);
    report();
  }
  std::_Exit(3);
}

void WarnF(const char* fmt, ...) {
  std::fprintf(stderr, "warning: ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

void TraceF(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

// Host faults (SIGSEGV/SIGBUS) in guest memory are guest access
// violations: they go to the guest SEH chain (Dungeon Siege demand-pages Tank
// data from an exception filter). Anything unhandled, or a fault outside
// the arena (a runtime bug), is reported with the host call chain (lifted
// functions are named f_<guest va>) and exits.
namespace hle {
bool RaiseAccessViolation(Ctx& c, bool write, uint32_t addr);
extern thread_local int t_hle_depth;
}

namespace {
// Faults can nest: an exception filter may itself touch a not-yet-paged
// page (the Tank decompressor does). Windows dispatches a nested
// exception; so do we, up to a sanity limit.
thread_local int t_fault_depth = 0;
constexpr int kMaxFaultDepth = 8;

void FaultHandler(int sig, siginfo_t* si, void* uc) {
  char buf[256];
  uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_guest_base);
  if (a - base < (1ull << 32) && CurrentThread() &&
      t_fault_depth < kMaxFaultDepth && hle::t_hle_depth == 0) {
    bool write = false;
#if defined(__linux__) && defined(__x86_64__)
    write = (static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#endif
    ++t_fault_depth;
    bool cont = hle::RaiseAccessViolation(CurrentThread()->ctx, write,
                                          uint32_t(a - base));
    --t_fault_depth;
    if (cont) return;  // retry the faulting instruction
  }
  int n;
  if (a - base < (1ull << 32))
    n = std::snprintf(buf, sizeof buf,
                      "FATAL: %s: guest memory fault at guest %08lX\n",
                      sig == SIGSEGV ? "SIGSEGV" : "SIGBUS",
                      static_cast<unsigned long>(a - base));
  else
    n = std::snprintf(buf, sizeof buf,
                      "FATAL: %s: host fault at %p (runtime bug)\n",
                      sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", si->si_addr);
  (void)!write(2, buf, size_t(n));
  char name[32] = "?";
  pthread_getname_np(pthread_self(), name, sizeof name);
#if defined(__linux__) && defined(__x86_64__)
  // A call through a null pointer leaves pc 0: the caller is at [sp].
  const greg_t* g = static_cast<ucontext_t*>(uc)->uc_mcontext.gregs;
  void* caller = *reinterpret_cast<void* const*>(g[REG_RSP]);
  n = std::snprintf(buf, sizeof buf, "  thread '%s', pc %p, [sp]:\n", name,
                    reinterpret_cast<void*>(g[REG_RIP]));
  (void)!write(2, buf, size_t(n));
  backtrace_symbols_fd(&caller, 1, 2);
  n = 0;
#else
  n = std::snprintf(buf, sizeof buf, "  thread '%s'\n", name);
#endif
  (void)!write(2, buf, size_t(n));
  void* frames[48];
  int nf = backtrace(frames, 48);
  backtrace_symbols_fd(frames, nf, 2);
  _exit(4);
}
}  // namespace

// Each host thread running guest code needs its own signal stack: SEH
// handlers for guest faults run lifted code on it, so it is large.
void InstallSignalStack() {
  constexpr size_t kSize = 16u << 20;
  void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  stack_t ss{};
  ss.ss_sp = p;
  ss.ss_size = kSize;
  sigaltstack(&ss, nullptr);
}

namespace {
void InstallFaultHandler() {
  InstallSignalStack();
  struct sigaction sa{};
  sa.sa_sigaction = FaultHandler;
  // SA_NODEFER: a fault inside a guest exception filter must be
  // deliverable while we are still in the handler for the first one.
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
}
}  // namespace

void InitDispatch() {
  InstallFaultHandler();
  g_exe = TextOf(g_proc->img);
  for (const Module& m : g_proc->modules) {
    CodeRange& r = g_dlls.emplace_back(TextOf(m.img));
    // A DLL lifted at another base, or not at all, has no functions here.
    if (m.img.entry && !r.table[m.img.entry - r.lo])
      FatalF("%s at %08X: its entry point %08X is not lifted (lift it with "
             "recomp_lift --module, at the same --base)",
             m.name.c_str(), m.img.image_base, m.img.entry);
  }
}

void Dispatch(Ctx& c, uint32_t target, uint32_t va) {
  const CodeRange* r = &g_exe;
  if (target - r->lo >= r->size) {
    if (target - kThunkBase < kThunkRegionSize) {
      hle::CallThunk(c, target);
      return;
    }
    r = nullptr;
    for (const CodeRange& d : g_dlls)
      if (target - d.lo < d.size) r = &d;
    if (!r) FatalF("indirect branch to %08X from %08X: not code", target, va);
  }
  if (GuestFn fn = r->table[target - r->lo]) {
    fn(c);
    return;
  }
  FatalF("indirect branch to unlifted address %08X from %08X "
         "(add it to the manifest as a function entry)", target, va);
}

void Unimplemented(Ctx& c, uint32_t va, const char* what) {
  (void)c;
  FatalF("unimplemented instruction at %08X: %s", va, what);
}

void BadJump(Ctx& c, uint32_t va, uint32_t target) {
  (void)c;
  FatalF("branch at %08X to %08X outside lifted code", va, target);
}

void Unreachable(Ctx& c, uint32_t va) {
  (void)c;
  FatalF("returned from a no-return call at %08X", va);
}

void Trap(Ctx& c, uint32_t va) {
  (void)c;
  // int3 in shipped code is an assert/DebugBreak. EXCEPTION_BREAKPOINT
  // is not raised through SEH (no game path has needed it); stop loudly.
  FatalF("breakpoint/trap instruction at %08X", va);
}

void RaiseDivideError(Ctx& c, uint32_t va) {
  (void)c;
  FatalF("integer divide error (#DE) at %08X", va);
}

void Cpuid(Ctx& c) {
  // A fixed CPU, never the host's: lifted code behaves the same on every
  // machine (Apple Silicon has no CPUID), and nothing beyond SSE2 is
  // claimed. SSE and SSE2 are reported because every PC these games still
  // run on has them, so their SSE paths are the ones that matter
  // (runtime/sse.h). MMX comes with any SSE CPU; no supported game has
  // MMX code, and the lifter would report it as unimplemented. Family 6
  // model 13 (Pentium M) is a real SSE2 part without SSE3.
  switch (c.eax) {
    case 0:
      c.eax = 2;
      c.ebx = 0x756E6547;  // "GenuineIntel"
      c.edx = 0x49656E69;
      c.ecx = 0x6C65746E;
      break;
    case 1:
      c.eax = 0x000006D8;  // family 6, model 13, stepping 8
      c.ebx = 0;
      c.ecx = 0;
      c.edx = 0x0783F9FF;  // fpu tsc cx8 cmov mmx fxsr sse sse2
      break;
    default:
      c.eax = c.ebx = c.ecx = c.edx = 0;
      break;
  }
}

uint64_t Rdtsc(Ctx& c) {
  (void)c;
  using namespace std::chrono;
  return uint64_t(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
          .count());
}

void Probe(Ctx& c, uint32_t va, const char* name) {
  std::string line;
  char buf[96];
  std::snprintf(buf, sizeof buf, "probe: %s (%08X) ecx %08X args", name, va, c.ecx);
  line = buf;
  for (int i = 1; i <= 4; i++) {
    uint32_t a = Rd32(c.esp + 4 * i);
    std::snprintf(buf, sizeof buf, " %08X", a);
    line += buf;
    // Show arguments that point at a short printable string, directly or
    // through one pointer (the game's string class holds a char*).
    auto str = [&](uint32_t v, std::string* out) {
      if (v < 0x10000 || !g_proc->mem.IsCommitted(v) || !g_proc->mem.IsCommitted(v + 63))
        return false;
      const char* p = g_proc->mem.ptr<const char>(v);
      int n = 0;
      while (n < 64 && p[n] >= 0x20 && p[n] < 0x7F) n++;
      if (!(n == 64 || p[n] == 0) || (n < 3 && n != 0)) return false;
      *out = std::string(p, n);
      return n >= 3;
    };
    std::string sv;
    if (str(a, &sv)) {
      line += "=\"" + sv + "\"";
    } else if (a >= 0x10000 && g_proc->mem.IsCommitted(a) && g_proc->mem.IsCommitted(a + 3)) {
      uint32_t q = Rd32(a);
      if (q == 0) line += "->null";
      else if (str(q, &sv)) line += "->\"" + sv + "\"";
      else if (q >= 0x10000 && g_proc->mem.IsCommitted(q) && !GPtr(q)[0]) line += "->\"\"";
    }
  }
  std::fprintf(stderr, "%s\n", line.c_str());
}

}  // namespace recomp
