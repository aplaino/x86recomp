#pragma once
// High-level emulation: host implementations of the functions a program
// imports from system libraries, bound to import thunks. The registry and
// the calling conventions are the x86 core's; each platform names its
// libraries and binds its imports (Win32: win32/hle.h).
//
// A handler reads its arguments with Arg(c, i), returns the value for EAX,
// and in 32-bit guests the framework pops the return address plus nargs
// stack slots (stdcall). nargs = kCdecl pops only the return address.
// Imports without a handler are bound to a fatal stub that names them:
// there are no silent default stubs.
//
// x64 processes call the same handlers with the Win64 convention (or the
// System V one, Process::sysv): Arg reads the argument registers and then
// the stack, the return value goes to rax, nargs is unused (the caller
// pops). Handlers written for 32-bit guests keep guest pointers and
// structure fields in uint32_t, so only those declared with HLE64
// (audited: pointers in gaddr, x64 structure layouts) can be called from
// x64 code; calling another is fatal and names it.

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include "runtime/log.h"
#include "x86/ctx.h"
#include "x86/runtime.h"

namespace recomp::hle {

inline constexpr int kCdecl = -1;

// >0 while an HLE handler runs on this thread (0 again inside CallGuest).
// A host fault then is a runtime bug, not a guest access violation.
extern thread_local int t_hle_depth;

using Fn = uint64_t (*)(Ctx& c);

// A handler, registered under (library, export name), e.g.
// ("kernel32.dll", "Sleep") or ("mss32.dll", "_AIL_startup@0").
struct Def {
  int nargs;
  Fn fn;
  bool x64;  // audited for x64 guests (HLE64)
};

struct Registrar {
  Registrar(const char* dll, const char* name, int nargs, Fn fn, bool x64 = false);
};

// A data export of a library we provide (a C++ static member, a global the
// guest reads through its IAT slot): `size` bytes of guest memory, zero or
// set by `init`, allocated on first import. They live in the upper half of
// the thunk region (kDataExportOffset), which is writable.
inline constexpr uint32_t kDataExportOffset = kThunkRegionSize / 2;
struct DataExport {
  DataExport(const char* dll, const char* name, uint32_t size, void (*init)(gaddr) = nullptr);
};

// Every handler of library `lib` runs under one process-wide lock, for a
// library whose state is not reentrant and that games call from several
// threads (DirectDraw with DDSCL_MULTITHREADED).
struct Serialised {
  explicit Serialised(const char* lib);
};

// Argument i of the call into a handler. Win64: rcx, rdx, r8, r9, then
// the stack above the return address and the 32-byte shadow space.
// System V: the i-th integer or pointer argument (float ones are counted
// apart, see ArgD): rdi, rsi, rdx, rcx, r8, r9, then the stack above the
// return address (as long as no float argument went to the stack).
inline uint64_t Arg(const Ctx& c, int i) {
  if (!g_proc->x64) return Rd32(c.esp + 4 + 4 * i);
  if (g_proc->sysv) {
    switch (i) {
      case 0: return c.rdi;
      case 1: return c.rsi;
      case 2: return c.rdx;
      case 3: return c.rcx;
      case 4: return c.r8;
      case 5: return c.r9;
      default: return Rd64(c.rsp + 8 + 8 * uint64_t(i - 6));
    }
  }
  switch (i) {
    case 0: return c.rcx;
    case 1: return c.rdx;
    case 2: return c.r8;
    case 3: return c.r9;
    default: return Rd64(c.rsp + 8 + 8 * uint64_t(i));
  }
}

// Floating-point arguments and results. Win64: argument i of a float or
// double type is in xmm<i> (i < 4; later ones on the stack like integers).
// System V: the i-th float or double argument is in xmm<i> (i < 8; where
// a later one sits on the stack depends on the whole signature, so a
// handler that needs one reads it itself). The result goes to xmm0.
inline double ArgD(const Ctx& c, int i) {
  double v;
  if (g_proc->sysv && i >= 8) FatalF("System V float argument %d is on the stack", i);
  if (i < (g_proc->sysv ? 8 : 4)) std::memcpy(&v, c.xmm[i].b, 8);
  else {
    uint64_t b = Arg(c, i);
    std::memcpy(&v, &b, 8);
  }
  return v;
}
inline float ArgF(const Ctx& c, int i) {
  float v;
  if (g_proc->sysv && i >= 8) FatalF("System V float argument %d is on the stack", i);
  if (i < (g_proc->sysv ? 8 : 4)) std::memcpy(&v, c.xmm[i].b, 4);
  else {
    uint32_t b = uint32_t(Arg(c, i));
    std::memcpy(&v, &b, 4);
  }
  return v;
}
// Returns from a handler with a double (float) result in xmm0.
inline uint64_t RetD(Ctx& c, double v) {
  std::memcpy(c.xmm[0].b, &v, 8);
  return 0;
}
inline uint64_t RetF(Ctx& c, float v) {
  std::memcpy(c.xmm[0].b, &v, 4);
  return 0;
}

// The value for an import slot of lib!name, lib named as the HLE registers
// it: a data export's address or a handler's thunk (an unimplemented
// import's, fatal when called).
gaddr BindImport(const std::string& lib, const std::string& name);
// True if lib!name is a data export (DataExport).
bool IsDataExport(const std::string& lib, const std::string& name);
// True if lib!name has a handler.
bool HasHandler(const std::string& lib, const std::string& name);

// Thunk address for lib!name whether or not a handler exists (calling an
// unimplemented one is fatal and names it). Used for COM vtables.
gaddr ThunkFor(const std::string& lib, const std::string& name);

// Called by Dispatch for any address in the thunk region.
void CallThunk(Ctx& c, gaddr thunk);

}  // namespace recomp::hle

#define RECOMP_HLE_CAT2(a, b) a##b
#define RECOMP_HLE_CAT(a, b) RECOMP_HLE_CAT2(a, b)

// HLE(dll, Name, nargs) { ... return eax; }  for C-identifier names.
#define HLE(dll, name, nargs) HLE_NAMED(dll, #name, name, nargs)
// HLE64(dll, Name, nargs): the same, audited for x64 guests too.
#define HLE64(dll, name, nargs) HLE_NAMED_X(dll, #name, name, nargs, true)

// HLE_NAMED(dll, "export@8", ident, nargs) for decorated export names.
#define HLE_NAMED(dll, export_name, ident, nargs) \
  HLE_NAMED_X(dll, export_name, ident, nargs, false)
#define HLE_NAMED_X(dll, export_name, ident, nargs, x64)                   \
  static uint64_t RECOMP_HLE_CAT(hle_, ident)(::recomp::Ctx & c);              \
  static ::recomp::hle::Registrar RECOMP_HLE_CAT(reg_, ident){                 \
      dll, export_name, nargs, &RECOMP_HLE_CAT(hle_, ident), x64};          \
  static uint64_t RECOMP_HLE_CAT(hle_, ident)([[maybe_unused]] ::recomp::Ctx & c)
