// libkernel HLE: errno and the process-wide data it exports.

#include "orbis/hle/kernel.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "orbis/hle.h"
#include "orbis/process.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::orbis::kernel {

gaddr ErrnoAddr(const Ctx& c) { return c.fs_base + kTcbErrno; }
void SetErrno(const Ctx& c, int e) { Wr32(ErrnoAddr(c), uint32_t(e)); }

namespace {

std::mutex g_handles_mu;
std::vector<gaddr> g_free;
gaddr g_next = 0, g_end = 0;

}  // namespace

gaddr NewHandle() {
  std::lock_guard<std::mutex> l(g_handles_mu);
  if (!g_free.empty()) {
    gaddr h = g_free.back();
    g_free.pop_back();
    return h;
  }
  if (g_next == g_end) {
    constexpr uint64_t kChunk = 0x100000;
    g_next = g_proc->vm.Reserve(0, kChunk);
    if (!g_next || !g_proc->vm.Commit(g_next, kChunk, Prot::RW))
      FatalF("libkernel objects: out of guest memory");
    g_end = g_next + kChunk;
  }
  gaddr h = g_next;
  g_next += 64;
  return h;
}

void FreeHandle(gaddr h) {
  std::lock_guard<std::mutex> l(g_handles_mu);
  g_free.push_back(h);
}

namespace {

// The stack protector's canary: a fixed value, so runs are reproducible.
void InitStackGuard(gaddr a) { Wr64(a, 0x00000AFF0A0D0000ull); }

// __progname: a char* to the program's name (stored right after it).
void InitProgname(gaddr a) {
  Wr64(a, a + 8);
  PutStr(a + 8, "eboot.bin");
}

}  // namespace

}  // namespace recomp::orbis::kernel

using namespace recomp::orbis::kernel;

ORBIS_DATA("libkernel", "__stack_chk_guard", stack_chk_guard, 8, InitStackGuard);
ORBIS_DATA("libkernel", "__progname", progname, 24, InitProgname);

// The eboot's process parameters (SceProcParam: size, "ORBI", the SDK
// version, then pointers to the process name, the main thread's name,
// priority and stack size, and libc's parameters), or null.
ORBIS_HLE("libkernel", sceKernelGetProcParam, 0) { return ::recomp::orbis::ProcParam(); }

// libc hands libkernel its malloc family (a table of functions) for
// libkernel's own allocations, which the HLE does not make.
ORBIS_HLE("libkernel", _sceKernelRtldSetApplicationHeapAPI, 1) { return 0; }

// int* __error(void): the calling thread's errno.
ORBIS_HLE("libkernel", __error, 0) { return ErrnoAddr(c); }
