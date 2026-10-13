// Process and thread bring-up: arena, image, PEB/TEB, TLS, stacks, and the
// host threads that run guest code.

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

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/lifted.h"
#include "runtime/log.h"
#include "runtime/platform.h"
#include "runtime/runtime.h"
#include "runtime/sse.h"
#include "runtime/watchdog.h"

namespace recomp {

void InitDispatch();
void InstallSignalStack();

Process* g_proc = nullptr;

namespace {

thread_local Thread* t_current = nullptr;
std::mutex g_threads_mu;
std::map<uint32_t, std::unique_ptr<Thread>> g_threads;  // by tid
uint32_t g_next_teb = kTebTop;

// Host stack for each guest thread. Guest call depth maps 1:1 onto host
// call depth, and lifted frames are larger than the originals, so be
// generous; the memory is only touched as needed.
constexpr size_t kHostStack = 256u << 20;

constexpr uint32_t kTlsArraySlots = 64;

}  // namespace

Thread* CurrentThread() { return t_current; }

uint32_t GuestProcessorCount() {
  unsigned n = std::thread::hardware_concurrency();
  return std::clamp<unsigned>(n, 2, 8);  // capped: 2002 code may assume few
}

const GameConfig& Game() { return g_proc->game; }

const char* Env(const char* name) {
  if (!g_proc) return nullptr;
  std::string var;
  for (char ch : g_proc->game.id) var += char(std::toupper(uint8_t(ch)));
  var += '_';
  var += name;
  return std::getenv(var.c_str());
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

std::string GStr(uint32_t va) {
  if (!va) return {};
  return std::string(g_proc->mem.ptr<const char>(va));
}

std::u16string GWStr(uint32_t va) {
  std::u16string s;
  if (!va) return s;
  for (;; va += 2) {
    char16_t ch = Rd16(va);
    if (!ch) break;
    s.push_back(ch);
  }
  return s;
}

void PutStr(uint32_t va, const std::string& s) {
  std::memcpy(GPtr(va), s.c_str(), s.size() + 1);
}

void PutWStr(uint32_t va, const std::string& s) {
  for (size_t i = 0; i <= s.size(); i++) Wr16(va + 2 * uint32_t(i), uint8_t(s.c_str()[i]));
}

uint32_t CallGuest(Ctx& c, uint32_t va, std::initializer_list<uint32_t> args) {
  uint32_t saved = c.esp;
  // Guest code runs here: faults are guest access violations again.
  struct DepthGuard {
    int saved = hle::t_hle_depth;
    DepthGuard() { hle::t_hle_depth = 0; }
    ~DepthGuard() { hle::t_hle_depth = saved; }
  } guard;
  for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
    c.esp -= 4;
    Wr32(c.esp, *it);
  }
  c.esp -= 4;
  Wr32(c.esp, kReturnToHost);
  Dispatch(c, va, kReturnToHost);
  c.esp = saved;  // stdcall callees popped their args, cdecl didn't
  return c.eax;
}

Thread* CreateGuestThread(uint32_t stack_size) {
  auto t = std::make_unique<Thread>();
  Process& p = *g_proc;

  stack_size = std::max<uint32_t>(stack_size, 0x100000);
  uint32_t stack = p.vm.Reserve(0, stack_size);
  if (!stack) FatalF("cannot reserve a %u byte guest stack", stack_size);
  p.vm.Commit(stack, stack_size, Prot::RW);
  t->stack_lo = stack;
  t->stack_hi = stack + stack_size;

  {
    std::lock_guard<std::mutex> l(g_threads_mu);
    g_next_teb -= kTebStride;
    t->teb = g_next_teb;
  }
  t->tid = p.next_tid.fetch_add(4);
  p.mem.Commit(t->teb, kTebStride, Prot::RW);
  uint32_t teb = t->teb;
  Wr32(teb + 0x00, 0xFFFFFFFF);  // SEH chain: empty
  Wr32(teb + 0x04, t->stack_hi);
  Wr32(teb + 0x08, t->stack_lo);
  Wr32(teb + 0x18, teb);         // Self
  Wr32(teb + 0x20, kGuestPid);
  Wr32(teb + 0x24, t->tid);
  Wr32(teb + 0x30, kPebVa);

  // Static TLS of the image (__declspec(thread)): a per-thread copy of the
  // template, found through TEB+0x2C[tls_index].
  uint32_t tls_array = HeapAllocProcess(kTlsArraySlots * 4, true);
  Wr32(teb + 0x2C, tls_array);
  if (p.img.has_tls) {
    const PeTls& tls = p.img.tls;
    uint32_t raw = tls.raw_end - tls.raw_start;
    uint32_t block = HeapAllocProcess(raw + tls.zero_fill + 16, true);
    std::memcpy(GPtr(block), GPtr(tls.raw_start), raw);
    Wr32(tls_array + 4 * Rd32(tls.index_va), block);
  }

  Ctx& c = t->ctx;
  std::memset(&c, 0, sizeof c);
  c.esp = t->stack_hi - 16;
  c.fs_base = teb;
  c.thread = t.get();
  x87::Init(c.fpu);
  x87::SetControlWord(c.fpu, 0x027F);  // Windows default for new threads
  c.mxcsr = sse::kMxcsrDefault;

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

// Runs guest function `entry` (with no argument, as the process entry
// point, or with `param`) on the calling host thread as guest thread t.
// Returns its exit code.
static uint32_t RunGuestThread(Thread* t, uint32_t entry,
                               const uint32_t* param) {
  // Thread start is a full barrier on Windows. pthread_create already
  // makes the creator's writes visible here; the fence states it for
  // weakly ordered hosts like every other guest sync point
  // (kernel32_state.h). The exit side fences in OnGuestThreadFinished.
  GuestSyncFence();
  t_current = t;
  CheckHostFp();
  InstallSignalStack();
  RegisterGuestHostThread("guest", t->tid);
  uint32_t r;
  try {
    r = param ? CallGuest(t->ctx, entry, {*param}) : CallGuest(t->ctx, entry, {});
  } catch (const GuestThreadExit& e) {
    r = e.code;
  }
  OnGuestThreadFinished(t->tid);
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
  uint32_t entry, param;
};

static void* GuestThreadMain(void* arg) {
  std::unique_ptr<ThreadStart> s(static_cast<ThreadStart*>(arg));
  RunGuestThread(s->t, s->entry, &s->param);
  return nullptr;
}

void StartHostThread(Thread* t, uint32_t entry, uint32_t param) {
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
uint32_t g_main_entry = 0, g_main_exit = 0;

void GuestMainTrampoline() {
  g_main_exit = RunGuestThread(g_main_thread, g_main_entry, nullptr);
}
}  // namespace

static uint32_t RunOnMainThread(Thread* t, uint32_t entry) {
  void* stack = mmap(nullptr, kHostStack, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (stack == MAP_FAILED) FatalF("cannot allocate the main guest stack");
  g_main_thread = t;
  g_main_entry = entry;
  platform::RunOnStack(stack, kHostStack, GuestMainTrampoline);
  munmap(stack, kHostStack);
  return g_main_exit;
}

const PeImage* ImageAt(uint32_t va) {
  const Process& p = *g_proc;
  if (va - p.img.image_base < p.img.image_size) return &p.img;
  for (const Module& m : p.modules)
    if (va - m.img.image_base < m.img.image_size) return &m.img;
  return nullptr;
}

void MapModule(Module& m) {
  Process& p = *g_proc;
  if (m.img.image_size) p.mem.Decommit(m.img.image_base, m.img.image_size);
  std::string err;
  if (!m.img.Load(m.host_path, p.mem, &err, m.base, hle::ImportThunk))
    FatalF("lifted DLL %s: %s", m.name.c_str(), err.c_str());
  // Windows XP did not set up static TLS for LoadLibrary'd DLLs either,
  // so DLLs of that era have none.
  if (m.img.has_tls)
    FatalF("lifted DLL %s has static TLS (__declspec(thread)): not supported",
           m.name.c_str());
  m.stale = false;
}

int RunProcess(const GameConfig& game, const std::string& exe_path,
               const std::string& game_dir, const std::string& game_args) {
  g_proc = new Process;
  Process& p = *g_proc;
  p.game = game;
  p.game_dir = game_dir;
  const char* runs = Env("X87_RUNS");
  x87::run::g_enabled = !(runs && runs[0] == '0');
  const char* doubles = Env("X87_DOUBLE");
  x87::run::g_doubles = !(doubles && doubles[0] == '0');
  p.game_args = game_args;
  StartWatchdog();
  if (!p.mem.Reserve()) FatalF("cannot reserve the guest arena");
  g_guest_base = p.mem.base();
  // Calls to import thunks go to HLE handlers (Dispatch); nothing runs
  // there, but the IAT values point at committed memory as on Windows.
  if (!p.mem.Commit(kThunkBase, kThunkRegionSize, Prot::R))
    FatalF("cannot commit the thunk region");
  std::string err;
  if (!p.img.Load(exe_path, p.mem, &err, 0, hle::ImportThunk))
    FatalF("%s", err.c_str());
  for (const auto& d : game.lifted_dlls) {
    Module& m = p.modules.emplace_back();
    m.name = hle::NormaliseDll(d.name);
    m.host_path = HostPath(GuestGameDir() + "\\" + d.name);
    m.base = d.base;
    MapModule(m);
  }

  p.vm.Init(&p.mem, 0x00010000, kThunkBase);
  p.vm.MarkReserved(p.img.image_base, p.img.image_size);
  for (const Module& m : p.modules) p.vm.MarkReserved(m.img.image_base, m.img.image_size);
  p.vm.MarkReserved(hle::kModuleRegion, hle::kModuleRegionSize);

  // PEB: only the fields Win32 code commonly peeks at.
  p.mem.Commit(kPebVa, 0x1000, Prot::RW);
  Wr32(kPebVa + 0x08, p.img.image_base);
  Wr32(kPebVa + 0x64, GuestProcessorCount());  // NumberOfProcessors
  Wr32(kPebVa + 0xA4, 5);      // OSMajorVersion (Windows XP)
  Wr32(kPebVa + 0xA8, 1);      // OSMinorVersion
  Wr16(kPebVa + 0xAC, 2600);   // OSBuildNumber
  Wr32(kPebVa + 0xB0, 2);      // OSPlatformId: VER_PLATFORM_WIN32_NT

  // The image's TLS index: the only module with static TLS (MapModule),
  // so slot 0.
  if (p.img.has_tls) Wr32(p.img.tls.index_va, 0);

  InitDispatch();
  InitKernel32State();

  Thread* main = CreateGuestThread(p.img.stack_reserve);
  return int(RunOnMainThread(main, p.img.entry));
}

}  // namespace recomp
