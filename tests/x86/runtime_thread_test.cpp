// runtime_thread_test: per-thread state of the x86 core's runtime across
// guest faults and thread exits: guest access violations handled by an
// exception (the fault handler's nesting count), the fault handler of a
// host library installed before the runtime's, a finished guest thread's
// stack, signal stacks of exited threads, and the watchdog's list of
// threads. Linux hosts (the fault path that throws through the signal
// frame; /proc for the mappings).

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "x86/layout.h"
#include "x86/lifted.h"
#include "x86/runtime.h"
#include "x86/watchdog.h"

namespace recomp {
void InstallSignalStack();
}  // namespace recomp

using namespace recomp;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

struct Handled {};

// Inside the 32-bit arena, never committed: every access faults.
constexpr gaddr kUnmapped = 0x07000000;

__attribute__((noinline)) uint8_t Touch(gaddr a) {
  return *reinterpret_cast<volatile uint8_t*>(g_guest_base + a);
}

// A host page whose faults a handler installed before the runtime's owns.
uint8_t* g_host_page = nullptr;
bool g_chained = false;
void EarlierHandler(int, siginfo_t* si, void*) {
  if (si->si_addr != g_host_page) std::abort();
  mprotect(g_host_page, 4096, PROT_READ | PROT_WRITE);
  g_chained = true;
}

bool Mapped(void* p) { return msync(p, 4096, MS_ASYNC) == 0 || errno != ENOMEM; }

// Thread headers in this process's hang dump, or -1 if there is none yet.
int DumpedThreads(const std::string& dir) {
  const std::string suffix = "-" + std::to_string(getpid()) + ".txt";
  DIR* d = opendir(dir.c_str());
  if (!d) return -1;
  int n = -1;
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix))
      continue;
    std::ifstream f(dir + "/" + name);
    std::stringstream s;
    s << f.rdbuf();
    const std::string text = s.str();
    n = 0;
    for (size_t at = 0; (at = text.find("=== thread", at)) != std::string::npos; at++) n++;
  }
  closedir(d);
  return n;
}

}  // namespace

int main() {
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) return 1;
  const std::string data = std::string(cwd) + "/runtime_thread_test.data";
  setenv("XDG_DATA_HOME", data.c_str(), 1);
  setenv("RTT_WATCHDOG_SECS", "0.5", 1);

  g_host_page = static_cast<uint8_t*>(
      mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  struct sigaction sa{};
  sa.sa_sigaction = EarlierHandler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);

  static const LiftedFn kNone[1] = {};
  RegisterLiftedFunctions(kNone, 0);
  Process& p = StartProcess(GameConfig{"rtt"}, cwd, false);
  p.vm.Init(&p.mem, 0x00010000, kThunkBase);
  p.guest_fault = [](Ctx&, bool, gaddr) -> bool { throw Handled{}; };
  InitDispatch();

  *reinterpret_cast<volatile uint8_t*>(g_host_page) = 1;
  Check(g_chained, "a host fault outside the arena goes to the earlier handler");

  // More guest faults on one thread than may nest, each left by an
  // exception from the platform's handler.
  Thread* t = CreateGuestThread(0);
  const gaddr stack = t->stack_lo;
  int handled = 0;
  RunMainThread(t, [&](Ctx&) -> uint64_t {
    for (int i = 0; i < 32; i++) {
      try {
        Touch(kUnmapped);
      } catch (const Handled&) {
        handled++;
      }
    }
    return 0;
  });
  Check(handled == 32, "every guest fault is dispatched (the nesting count unwinds)");
  GuestAllocator::Region r;
  Check(!p.vm.Find(stack, &r), "a finished guest thread's stack is released");

  // A thread's signal stack is installed once and unmapped when it exits.
  for (int i = 0; i < 4; i++) {
    void* first = nullptr;
    void* second = nullptr;
    std::thread([&] {
      stack_t ss;
      InstallSignalStack();
      sigaltstack(nullptr, &ss);
      first = ss.ss_sp;
      InstallSignalStack();
      sigaltstack(nullptr, &ss);
      second = ss.ss_sp;
    }).join();
    Check(first && first == second, "a thread's signal stack is installed once");
    Check(first && !Mapped(first), "an exited thread's signal stack is unmapped");
  }

  // Threads that registered with the watchdog and exited are not in its
  // hang dump; this (registered, still running) thread is.
  for (int i = 0; i < 4; i++)
    std::thread([i] { RegisterGuestHostThread("exited", 0x1000u + unsigned(i)); }).join();
  WatchdogHeartbeat();
  WatchdogFrame();
  const std::string hangs = data + "/rtt/hangs";
  int dumped = -1;
  for (int i = 0; i < 100 && dumped < 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dumped = DumpedThreads(hangs);
  }
  std::this_thread::sleep_for(std::chrono::seconds(1));  // let the dump finish
  dumped = DumpedThreads(hangs);
  Check(dumped == 1, "the hang dump lists the live guest thread only");

  std::printf("%s\n", failures ? "runtime_thread: FAILED" : "runtime_thread: ok");
  std::fflush(stdout);
  std::_Exit(failures ? 1 : 0);  // the watchdog thread is still running
}
