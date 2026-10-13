#pragma once
// Host platform specifics (Linux, macOS) that the runtime cannot express
// in plain POSIX: signal contexts, stack switching, stat's time fields.
// Everything here is behind one interface so the rest of the runtime has
// no #ifdef __linux__ / __APPLE__.

#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace recomp::platform {

// ---- files
inline timespec StatAccessTime(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_atimespec;
#else
  return st.st_atim;
#endif
}
inline timespec StatModifyTime(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_mtimespec;
#else
  return st.st_mtim;
#endif
}

// Per-user data under $HOME (without XDG_DATA_HOME), with a trailing '/'.
#if defined(__APPLE__)
inline constexpr const char* kUserDataDir = "/Library/Application Support/";
#else
inline constexpr const char* kUserDataDir = "/.local/share/";
#endif

// ---- stacks
// Runs fn() on [stack, stack + size) on the calling thread and returns
// when it does (the process main thread must stay the UI thread, but the
// guest needs a bigger stack than the OS gives it).
void RunOnStack(void* stack, size_t size, void (*fn)());

// ---- faults (SIGSEGV/SIGBUS handlers; `uc` is the handler's ucontext)
struct FaultContext {
  bool write_known = false;  // whether `write` comes from the CPU
  bool write = false;
  uintptr_t pc = 0;
  // Return address of the faulting function's caller as far as the
  // registers show it: [sp] on x86-64 at a call through null, the link
  // register on arm64. 0 when unknown.
  uintptr_t caller = 0;
};
FaultContext ReadFaultContext(const void* uc);

// Whether a C++ exception thrown inside a signal handler unwinds through
// the signal frame into the interrupted code. glibc's unwinder steps
// through the kernel's signal trampoline; Darwin's does not.
#if defined(__APPLE__)
inline constexpr bool kThrowThroughSignalFrames = false;
#else
inline constexpr bool kThrowThroughSignalFrames = true;
#endif

// Where kThrowThroughSignalFrames is false: makes the interrupted thread
// resume, when the handler returns, in fn() as if the faulting instruction
// had called it. An exception fn() throws then unwinds through the
// faulting function like one thrown by a call made there. The faulting
// function must have a frame record (lifted code is built with
// -mno-omit-leaf-frame-pointer on such hosts: the link register is
// overwritten). False if this host has no implementation.
bool RedirectToCall(void* uc, void (*fn)());

}  // namespace recomp::platform
