// Process and thread bring-up: the arena, guest threads' stacks and Ctx,
// and the host threads that run guest code. The platform does the rest
// through Process's hooks.

#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <thread>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "runtime/log.h"
#include "runtime/platform.h"
#include "x86/hle.h"
#include "x86/lifted.h"
#include "x86/runtime.h"
#include "x86/sse.h"
#include "x86/watchdog.h"

namespace recomp {

void InitDispatch();
void InstallSignalStack();

Process* g_proc = nullptr;

namespace {

thread_local Thread* t_current = nullptr;
std::mutex g_threads_mu;
std::map<uint32_t, std::unique_ptr<Thread>> g_threads;  // by tid

// Host stack for each guest thread. Guest call depth maps 1:1 onto host
// call depth, and lifted frames are larger than the originals, so be
// generous; the memory is only touched as needed.
constexpr size_t kHostStack = 256u << 20;

}  // namespace

Thread* CurrentThread() { return t_current; }

uint32_t GuestProcessorCount() {
  unsigned n = std::thread::hardware_concurrency();
  return std::clamp<unsigned>(n, 2, 8);  // capped: 2002 code may assume few
}

const char* Env(const char* name) {
  if (!g_proc) return nullptr;
  std::string var;
  for (char ch : g_proc->game.id) var += char(std::toupper(uint8_t(ch)));
  var += '_';
  var += name;
  return std::getenv(var.c_str());
}

bool Background() {
  static const bool bg = [] {
    const char* e = Env("BACKGROUND");
    return e && *e && *e != '0';
  }();
  return bg;
}

std::string DataDir() {
  const std::string& id = g_proc->game.id;
  if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x)
    return std::string(x) + "/" + id;
  if (const char* h = std::getenv("HOME"); h && *h)
    return std::string(h) + platform::kUserDataDir + id;
  return "/tmp/" + id;
}

void MakeDirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); i++)
    if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

std::string ReportPath(const char* kind) {
  std::string k = kind;  // crash -> crashes, hang -> hangs
  std::string dir = DataDir() + "/" + k + (k.ends_with("sh") ? "es" : "s");
  MakeDirs(dir);
  char name[64];
  std::snprintf(name, sizeof name, "/%s-%ld-%d.txt", kind, long(std::time(nullptr)),
                int(getpid()));
  return dir + name;
}

std::string GStr(gaddr va) {
  if (!va) return {};
  return std::string(g_proc->mem.ptr<const char>(va));
}

std::u16string GWStr(gaddr va) {
  std::u16string s;
  if (!va) return s;
  for (;; va += 2) {
    char16_t ch = Rd16(va);
    if (!ch) break;
    s.push_back(ch);
  }
  return s;
}

void PutStr(gaddr va, const std::string& s) {
  std::memcpy(GPtr(va), s.c_str(), s.size() + 1);
}

void PutWStr(gaddr va, const std::string& s) {
  for (size_t i = 0; i <= s.size(); i++) Wr16(va + 2 * i, uint8_t(s.c_str()[i]));
}

uint64_t CallGuest(Ctx& c, gaddr va, std::initializer_list<uint64_t> args) {
  // Guest code runs here: faults are guest access violations again.
  struct DepthGuard {
    int saved = hle::t_hle_depth;
    DepthGuard() { hle::t_hle_depth = 0; }
    ~DepthGuard() { hle::t_hle_depth = saved; }
  } guard;
  if (g_proc->sysv) {
    // System V: rdi, rsi, rdx, rcx, r8, r9, then the stack above the
    // return address; rsp is 16-byte aligned before the call.
    const uint64_t saved = c.rsp;
    const uint64_t n = args.size() > 6 ? args.size() - 6 : 0;
    uint64_t sp = ((c.rsp - 8 * n) & ~15ull) - 8;
    uint64_t* regs[6] = {&c.rdi, &c.rsi, &c.rdx, &c.rcx, &c.r8, &c.r9};
    size_t i = 0;
    for (uint64_t a : args) {
      if (i < 6) *regs[i] = a;
      else Wr64(sp + 8 + 8 * (i - 6), a);
      i++;
    }
    Wr64(sp, kReturnToHost64);
    c.rsp = sp;
    Dispatch(c, va, kReturnToHost64);
    c.rsp = saved;
    return c.rax;
  }
  if (g_proc->x64) {
    // Win64: the callee finds its stack arguments above the return
    // address and 32 bytes of shadow space (homes for rcx..r9), and rsp
    // is 16-byte aligned before the call.
    const uint64_t saved = c.rsp;
    const uint64_t n = std::max<uint64_t>(args.size(), 4);
    uint64_t sp = ((c.rsp - 8 * n) & ~15ull) - 8;
    uint64_t* regs[4] = {&c.rcx, &c.rdx, &c.r8, &c.r9};
    size_t i = 0;
    for (uint64_t a : args) {
      if (i < 4) *regs[i] = a;
      Wr64(sp + 8 + 8 * i, a);
      i++;
    }
    Wr64(sp, kReturnToHost64);
    c.rsp = sp;
    Dispatch(c, va, kReturnToHost64);
    c.rsp = saved;
    return c.rax;
  }
  uint32_t saved = c.esp;
  for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
    c.esp -= 4;
    Wr32(c.esp, uint32_t(*it));
  }
  c.esp -= 4;
  Wr32(c.esp, kReturnToHost);
  Dispatch(c, uint32_t(va), kReturnToHost);
  c.esp = saved;  // stdcall callees popped their args, cdecl didn't
  return c.eax;
}

Thread* CreateGuestThread(uint64_t stack_size) {
  auto t = std::make_unique<Thread>();
  Process& p = *g_proc;

  stack_size = std::max<uint64_t>(stack_size, 0x100000);
  gaddr stack = p.vm.Reserve(0, stack_size);
  if (!stack) FatalF("cannot reserve a %llu byte guest stack", (unsigned long long)stack_size);
  p.vm.Commit(stack, stack_size, Prot::RW);
  t->stack_lo = stack;
  t->stack_hi = stack + stack_size;

  t->tid = p.next_tid.fetch_add(4);
  Ctx& c = t->ctx;
  std::memset(&c, 0, sizeof c);
  c.eflags = 0x202;
  if (p.x64)
    c.rsp = t->stack_hi - 16;
  else
    c.esp = uint32_t(t->stack_hi - 16);
  c.thread = t.get();
  x87::Init(c.fpu);
  x87::SetControlWord(c.fpu, p.fpu_control);
  c.mxcsr = sse::kMxcsrDefault;
  if (p.thread_init) p.thread_init(t.get());

  Thread* raw_t = t.get();
  std::lock_guard<std::mutex> l(g_threads_mu);
  g_threads[raw_t->tid] = std::move(t);
  return raw_t;
}

// The x87 model's host-double paths need the default floating-point
// environment on every thread that runs guest code. Nothing here changes
// it; this catches a host library that does.
static void CheckHostFp() {
  if (const char* why = platform::HostFpProblem())
    FatalF("host floating-point environment: %s", why);
}

// Runs body (which calls guest code: the process entry point, or a
// thread's start routine) on the calling host thread as guest thread t.
// Returns its exit code.
static uint32_t RunGuestThread(Thread* t, const std::function<uint64_t(Ctx&)>& body) {
  // Thread start is a full barrier on Windows. pthread_create already
  // makes the creator's writes visible here; the fence states it for
  // weakly ordered hosts like every other guest sync point (runtime.h).
  // The exit side fences after the thread_exit hook.
  GuestSyncFence();
  t_current = t;
  CheckHostFp();
  InstallSignalStack();
  RegisterGuestHostThread("guest", t->tid);
  uint32_t r;
  try {
    r = uint32_t(body(t->ctx));
  } catch (const GuestThreadExit& e) {
    r = e.code;
  }
  if (g_proc->thread_exit) g_proc->thread_exit(t, r);
  GuestSyncFence();
  return r;
}

// Starts fn(arg) on a new detached host thread with the big guest stack.
static void SpawnHostThread(void* (*fn)(void*), void* arg) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kHostStack);
  pthread_t th;
  if (pthread_create(&th, &attr, fn, arg) != 0) FatalF("pthread_create failed");
  pthread_attr_destroy(&attr);
  pthread_detach(th);
}

struct ThreadStart {
  Thread* t;
  gaddr entry;
  uint64_t param;
};

static void* GuestThreadMain(void* arg) {
  std::unique_ptr<ThreadStart> s(static_cast<ThreadStart*>(arg));
  RunGuestThread(s->t, [&](Ctx& c) { return CallGuest(c, s->entry, {s->param}); });
  return nullptr;
}

void StartHostThread(Thread* t, gaddr entry, uint64_t param) {
  SpawnHostThread(GuestThreadMain, new ThreadStart{t, entry, param});
}

// A host-driven thread that may call into guest code (Miles' service
// thread delivering callbacks): bound to guest thread t, with the same big
// host stack and signal stack as other guest threads. Outside CallGuest it
// counts as HLE code, so a host fault there is a runtime bug.
struct ServiceStart {
  Thread* t;
  std::function<void()> fn;
};

static void* ServiceThreadMain(void* arg) {
  std::unique_ptr<ServiceStart> s(static_cast<ServiceStart*>(arg));
  t_current = s->t;
  CheckHostFp();
  InstallSignalStack();
  RegisterGuestHostThread("service", s->t->tid);
  hle::t_hle_depth = 1;
  s->fn();
  return nullptr;
}

void StartServiceThread(Thread* t, std::function<void()> fn) {
  SpawnHostThread(ServiceThreadMain, new ServiceStart{t, std::move(fn)});
}

// The guest main thread runs on the process main thread (SDL, and Cocoa on
// macOS, require UI calls there) but on a big private stack.
namespace {
Thread* g_main_thread = nullptr;
const std::function<uint64_t(Ctx&)>* g_main_body = nullptr;
uint32_t g_main_exit = 0;

void GuestMainTrampoline() {
  g_main_exit = RunGuestThread(g_main_thread, *g_main_body);
}
}  // namespace

uint32_t RunMainThread(Thread* t, const std::function<uint64_t(Ctx&)>& body) {
  void* stack = mmap(nullptr, kHostStack, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (stack == MAP_FAILED) FatalF("cannot allocate the main guest stack");
  g_main_thread = t;
  g_main_body = &body;
  platform::RunOnStack(stack, kHostStack, GuestMainTrampoline);
  munmap(stack, kHostStack);
  return g_main_exit;
}

namespace {
struct BesideStart {
  Thread* t;
  const std::function<uint64_t(Ctx&)>* body;
  std::atomic<bool> done{false};
  uint32_t exit = 0;
};

void* BesideMain(void* arg) {
  BesideStart* s = static_cast<BesideStart*>(arg);
  s->exit = RunGuestThread(s->t, *s->body);
  s->done.store(true, std::memory_order_release);
  return nullptr;
}
}  // namespace

uint32_t RunMainThreadBeside(Thread* t, const std::function<uint64_t(Ctx&)>& body,
                             const std::function<void(const std::atomic<bool>&)>& host_loop) {
  BesideStart s{t, &body};
  SpawnHostThread(BesideMain, &s);
  host_loop(s.done);
  if (!s.done.load(std::memory_order_acquire)) FatalF("host loop returned before the guest main thread");
  return s.exit;
}

Process& StartProcess(const GameConfig& game, const std::string& game_dir, bool x64,
                      unsigned arena_bits) {
  g_proc = new Process;
  Process& p = *g_proc;
  p.game = game;
  p.game_dir = game_dir;
  const char* runs = Env("X87_RUNS");
  x87::run::g_enabled = !(runs && runs[0] == '0');
  const char* doubles = Env("X87_DOUBLE");
  x87::run::g_doubles = !(doubles && doubles[0] == '0');
  StartWatchdog();
  p.x64 = x64;
  if (!p.mem.Reserve(!p.x64 ? 32 : arena_bits ? arena_bits : kArenaBits64)) FatalF("cannot reserve the guest arena");
  g_guest_base = p.mem.base();
  // Calls to import thunks go to HLE handlers (Dispatch); nothing runs
  // there, but the IAT values point at committed memory as on Windows.
  const gaddr thunks = p.x64 ? kThunkBase64 : kThunkBase;
  if (!p.mem.Commit(thunks, kThunkRegionSize, Prot::R))
    FatalF("cannot commit the thunk region");
  // Its upper half holds the HLE's data exports, which are variables.
  p.mem.Protect(thunks + hle::kDataExportOffset, kThunkRegionSize - hle::kDataExportOffset,
                Prot::RW);
  return p;
}

}  // namespace recomp
