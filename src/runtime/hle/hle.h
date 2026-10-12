#pragma once
// High-level emulation of the Win32/DirectX API surface: host
// implementations of imported functions, bound to the import thunks.
//
// A handler reads its arguments from the guest stack (Arg(c, i)), returns
// the value for EAX, and the framework pops the return address plus nargs
// stack slots (stdcall). nargs = kCdecl pops only the return address.
// Imports without a handler are bound to a fatal stub that names them:
// there are no silent default stubs.

#include <cstdint>
#include <string>

#include "runtime/ctx.h"
#include "runtime/runtime.h"

namespace recomp::hle {

inline constexpr int kCdecl = -1;

// >0 while an HLE handler runs on this thread (0 again inside CallGuest).
// A host fault then is a runtime bug, not a guest access violation.
extern thread_local int t_hle_depth;

// Guest range used for pseudo HMODULE values of the DLLs we provide.
inline constexpr uint32_t kModuleRegion = 0x70000000;
inline constexpr uint32_t kModuleRegionSize = 0x00200000;

using Fn = uint32_t (*)(Ctx& c);

// A handler, registered under (dll, export name), e.g. ("kernel32.dll",
// "Sleep") or ("mss32.dll", "_AIL_startup@0").
struct Def {
  int nargs;
  Fn fn;
};

struct Registrar {
  Registrar(const char* dll, const char* name, int nargs, Fn fn);
};

inline uint32_t Arg(const Ctx& c, int i) { return Rd32(c.esp + 4 + 4 * i); }

// Binds every static import of the image to its handler. Call once after
// the image is mapped.
void BindImports();

// Called by Dispatch for any address in the thunk region.
void CallThunk(Ctx& c, uint32_t thunk);

// Pseudo module handles for DLLs the game loads (LoadLibrary/
// GetModuleHandle). Returns 0 for DLLs we don't provide.
uint32_t ModuleHandle(const std::string& dll);
// Thunk address for dll!name (GetProcAddress); 0 if not implemented.
uint32_t ProcAddress(uint32_t hmodule, const std::string& name);

// Thunk address for dll!name whether or not a handler exists (calling an
// unimplemented one is fatal and names it). Used for COM vtables.
uint32_t ThunkFor(const std::string& dll, const std::string& name);

void SetLastError(uint32_t e);

// Writes s + NUL to a counted guest buffer the Win32 way: returns the
// length written without NUL, or the size needed (with NUL) if it does not
// fit.
uint32_t CopyOut(uint32_t buf, uint32_t size, const std::string& s);
// The NLS convention (GetLocaleInfo, GetDateFormat): `cap` characters
// (WCHARs if `wide`), 0 asks for the size; returns the count with NUL, or
// 0 with ERROR_INSUFFICIENT_BUFFER.
uint32_t PutCounted(uint32_t buf, uint32_t cap, const std::string& s, bool wide);

// Makes [va, va+len) readable before host code copies it, the way a DLL's
// read would on Windows: each uncommitted page raises a guest access
// violation, which the game's handlers may resolve (Dungeon Siege's Tank
// demand paging commits file views this way). False if a page stays
// inaccessible.
bool EnsureReadable(Ctx& c, uint32_t va, uint32_t len);

}  // namespace recomp::hle

#define RECOMP_HLE_CAT2(a, b) a##b
#define RECOMP_HLE_CAT(a, b) RECOMP_HLE_CAT2(a, b)

// HLE(dll, Name, nargs) { ... return eax; }  for C-identifier names.
#define HLE(dll, name, nargs) HLE_NAMED(dll, #name, name, nargs)

// HLE_NAMED(dll, "export@8", ident, nargs) for decorated export names.
#define HLE_NAMED(dll, export_name, ident, nargs)                        \
  static uint32_t RECOMP_HLE_CAT(hle_, ident)(::recomp::Ctx & c);              \
  static ::recomp::hle::Registrar RECOMP_HLE_CAT(reg_, ident){                 \
      dll, export_name, nargs, &RECOMP_HLE_CAT(hle_, ident)};               \
  static uint32_t RECOMP_HLE_CAT(hle_, ident)([[maybe_unused]] ::recomp::Ctx & c)
