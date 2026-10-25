// libSceFiber HLE: fibers as host coroutines (platform::Coroutine), one
// host stack each, since lifted code keeps its call frames on the host
// stack. A fiber's guest stack is the context memory the game gives it.
// Switching saves the running fiber's registers (all of Ctx) and loads
// the next one's. The SceFiber block (256 bytes, 8-aligned) is the
// game's: it gets the magic words libSceFiber 11.00 checks and its idle
// state; the rest of its fields are libSceFiber's own and stay unwritten.
// Checks and error codes are 11.00's. A fiber made without context memory
// runs on the stack of the thread that first runs it, below that call's
// frame, as libSceFiber starts it.

#include <sys/mman.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "orbis/hle.h"
#include "runtime/log.h"
#include "runtime/platform.h"
#include "runtime/runtime.h"

namespace recomp::orbis::fiber {
namespace {

constexpr uint32_t kErrNull = 0x80590001, kErrAlignment = 0x80590002, kErrRange = 0x80590003,
                   kErrInvalid = 0x80590004, kErrPermission = 0x80590005, kErrState = 0x80590006;
constexpr uint32_t kMagicStart = 0xDEF1649C, kMagicEnd = 0xB37592A0, kOptMagic = 0xBB40E64D;
constexpr uint32_t kStateIdle = 2;
constexpr size_t kHostStack = 64u << 20;

struct Fiber {
  gaddr guest = 0, entry = 0, context = 0;
  uint64_t arg_init = 0, context_size = 0;
  std::string name;
  void* host_stack = nullptr;
  platform::Coroutine* co = nullptr;
  Ctx saved;  // its registers while another runs
  bool running = false;
  uint64_t arg_in = 0;  // what the switch that resumes it passes
  gaddr stack_top = 0;  // without context memory: where it starts
};

// The calling thread's fibers: its own context, its registers while a
// fiber runs, the running fiber and what it returns to the thread.
struct ThreadFibers {
  platform::Coroutine* self = nullptr;
  Ctx saved;
  Fiber* current = nullptr;
  uint64_t ret_arg = 0;
};
thread_local ThreadFibers* t_fibers = nullptr;

std::mutex g_mu;
std::map<gaddr, std::unique_ptr<Fiber>> g_fibers;

ThreadFibers& Mine() {
  if (!t_fibers) {
    t_fibers = new ThreadFibers;
    t_fibers->self = platform::ThreadCoroutine();
  }
  return *t_fibers;
}

// The fiber at f, or an error.
uint32_t Lookup(gaddr f, Fiber** out) {
  if (!f) return kErrNull;
  if (f & 7) return kErrAlignment;
  if (Rd32(f) != kMagicStart || Rd32(f + 0xFC) != kMagicEnd) return kErrInvalid;
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_fibers.find(f);
  if (it == g_fibers.end()) return kErrInvalid;
  *out = it->second.get();
  return 0;
}

// A fiber's first run, on its coroutine: entry(arg_init, arg_on_run) on
// its context memory.
void Start(void* arg) {
  Fiber* f = static_cast<Fiber*>(arg);
  Ctx& c = CurrentThread()->ctx;
  c.rsp = f->context ? (f->context + f->context_size) & ~uint64_t(15) : f->stack_top;
  CallGuest(c, f->entry, {f->arg_init, f->arg_in});
  FatalF("fiber %s returned from its entry function", f->name.c_str());
}

// Switches the thread from `from` (its registers in `from_saved`) to fiber
// `to`, delivering arg; returns once something switches back to `from`.
void SwitchTo(Ctx& c, platform::Coroutine* from, Ctx* from_saved, Fiber* to, uint64_t arg) {
  ThreadFibers& t = Mine();
  *from_saved = c;
  t.current = to;
  to->running = true;
  to->arg_in = arg;
  if (!to->co) {
    // Below the frame of the call that starts it (a red zone further).
    if (!to->context) to->stack_top = (c.rsp - 0x100) & ~uint64_t(15);
    to->host_stack = mmap(nullptr, kHostStack, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (to->host_stack == MAP_FAILED) FatalF("fiber %s: no host stack", to->name.c_str());
    to->co = platform::NewCoroutine(to->host_stack, kHostStack, Start, to);
  } else {
    c = to->saved;
  }
  platform::SwitchCoroutine(from, to->co);
}

}  // namespace
}  // namespace recomp::orbis::fiber

using namespace recomp::orbis::fiber;
using recomp::gaddr;
using recomp::hle::Arg;

// int _sceFiberInitializeImpl(SceFiber* fiber, const char* name,
// SceFiberEntry entry, uint64_t arg_on_init, void* context, uint64_t
// context_size, const SceFiberOptParam* opt, uint32_t build_version)
ORBIS_HLE("libSceFiber", _sceFiberInitializeImpl, 8) {
  const gaddr f = Arg(c, 0), name = Arg(c, 1), entry = Arg(c, 2), context = Arg(c, 4);
  const uint64_t size = Arg(c, 5);
  const gaddr opt = Arg(c, 6);
  if (!f || !entry || !name) return kErrNull;
  if (f & 7 || context & 15 || (opt && opt & 7)) return kErrAlignment;
  if (size - 1 < 0x1FF) return kErrRange;
  if (!context != !size || size & 15) return kErrInvalid;
  if (opt && recomp::Rd32(opt) != kOptMagic) return kErrInvalid;
  auto fb = std::make_unique<Fiber>();
  fb->guest = f;
  fb->entry = entry;
  fb->arg_init = Arg(c, 3);
  fb->context = context;
  fb->context_size = size;
  fb->name = recomp::GStr(name);
  recomp::Wr32(f, kMagicStart);
  recomp::Wr32(f + 4, kStateIdle);
  recomp::Wr32(f + 0xFC, kMagicEnd);
  std::lock_guard<std::mutex> l(g_mu);
  g_fibers[f] = std::move(fb);
  return 0;
}

ORBIS_HLE("libSceFiber", sceFiberFinalize, 1) {
  Fiber* f;
  if (uint32_t e = Lookup(Arg(c, 0), &f)) return e;
  if (f->running) return kErrState;
  if (f->co) {
    recomp::platform::FreeCoroutine(f->co);
    munmap(f->host_stack, kHostStack);
  }
  std::lock_guard<std::mutex> l(g_mu);
  g_fibers.erase(Arg(c, 0));
  return 0;
}

// int sceFiberRun(SceFiber* fiber, uint64_t arg_on_run_to, uint64_t*
// arg_on_return): from a thread; returns when the fibers it leads to
// return to the thread.
ORBIS_HLE("libSceFiber", sceFiberRun, 3) {
  Fiber* f;
  if (uint32_t e = Lookup(Arg(c, 0), &f)) return e;
  ThreadFibers& t = Mine();
  if (t.current) return kErrPermission;
  if (f->running) return kErrState;
  SwitchTo(c, t.self, &t.saved, f, Arg(c, 1));
  // Back on the thread (sceFiberReturnToThread restored its registers).
  if (Arg(c, 2)) recomp::Wr64(Arg(c, 2), t.ret_arg);
  return 0;
}

// int sceFiberSwitch(SceFiber* fiber, uint64_t arg_on_run_to, uint64_t*
// arg_on_run): from a fiber to another.
ORBIS_HLE("libSceFiber", sceFiberSwitch, 3) {
  Fiber* to;
  if (uint32_t e = Lookup(Arg(c, 0), &to)) return e;
  ThreadFibers& t = Mine();
  if (!t.current) return kErrPermission;
  if (to->running) return kErrState;
  Fiber* self = t.current;
  self->running = false;
  SwitchTo(c, self->co, &self->saved, to, Arg(c, 1));
  if (Arg(c, 2)) recomp::Wr64(Arg(c, 2), self->arg_in);
  return 0;
}

// int sceFiberReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run)
ORBIS_HLE("libSceFiber", sceFiberReturnToThread, 2) {
  ThreadFibers& t = Mine();
  Fiber* self = t.current;
  if (!self) return kErrPermission;
  self->saved = c;
  self->running = false;
  t.ret_arg = Arg(c, 0);
  t.current = nullptr;
  c = t.saved;
  recomp::platform::SwitchCoroutine(self->co, t.self);
  // Resumed by sceFiberRun or sceFiberSwitch, which loaded its registers.
  if (Arg(c, 1)) recomp::Wr64(Arg(c, 1), self->arg_in);
  return 0;
}

ORBIS_HLE("libSceFiber", sceFiberGetSelf, 1) {
  if (!Arg(c, 0)) return kErrNull;
  ThreadFibers& t = Mine();
  if (!t.current) return kErrPermission;
  recomp::Wr64(Arg(c, 0), t.current->guest);
  return 0;
}
