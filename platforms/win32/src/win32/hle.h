#pragma once
// The Win32 side of the HLE (x86/hle.h): DLL names, the pseudo module
// handles LoadLibrary and GetProcAddress hand out, and the conventions
// Win32 handlers share (last error, counted strings, handles).

#include <cstdint>
#include <initializer_list>
#include <string>

#include "win32/process.h"
#include "x86/hle.h"

namespace recomp::hle {

// Guest range used for pseudo HMODULE values of the DLLs we provide.
inline constexpr uint32_t kModuleRegion = 0x70000000;
inline constexpr uint32_t kModuleRegionSize = 0x00200000;
inline constexpr gaddr kModuleRegion64 = 0xFFE000000;  // x64 (win32/process.h)

// The full export table of a DLL we provide, for games that GetProcAddress
// every entry point up front and only call some (an engine's GL loader
// that resolves all of opengl32). GetProcAddress then returns a thunk for
// any listed name; calling one without a handler is fatal and names it,
// as for imports.
struct Exports {
  Exports(const char* dll, std::initializer_list<const char*> names);
};

// A handle as a return value: Win64 handles are pointer-sized, their values
// sign-extended 32-bit ones (the pseudo handles are -1 and -2). Handle
// arguments are read as their low 32 bits, as Windows does.
inline uint64_t HandleRet(uint32_t h) {
  return g_proc->x64 ? uint64_t(int64_t(int32_t(h))) : h;
}

// IAT value for a static import dll!name of the EXE or a lifted DLL: its
// handler's thunk (PeImage::Load's binder).
gaddr ImportThunk(const std::string& dll, const std::string& name);

// "KERNEL32.dll", "C:\x\Foo" -> "kernel32.dll", "foo.dll".
std::string NormaliseDll(std::string dll);

// Pseudo module handles for DLLs the game loads (LoadLibrary/
// GetModuleHandle). Returns 0 for DLLs we don't provide.
gaddr ModuleHandle(const std::string& dll);
// Thunk address for dll!name (GetProcAddress); 0 if not implemented.
gaddr ProcAddress(gaddr hmodule, const std::string& name);

void SetLastError(uint32_t e);

// Writes s + NUL to a counted guest buffer the Win32 way: returns the
// length written without NUL, or the size needed (with NUL) if it does not
// fit.
uint32_t CopyOut(gaddr buf, uint32_t size, const std::string& s);
// The NLS convention (GetLocaleInfo, GetDateFormat): `cap` characters
// (WCHARs if `wide`), 0 asks for the size; returns the count with NUL, or
// 0 with ERROR_INSUFFICIENT_BUFFER.
uint32_t PutCounted(gaddr buf, uint32_t cap, const std::string& s, bool wide);

// Makes [va, va+len) readable before host code copies it, the way a DLL's
// read would on Windows: each uncommitted page raises a guest access
// violation, which the game's handlers may resolve (a game that
// demand-pages archive data commits file views this way). False if a page stays
// inaccessible.
bool EnsureReadable(Ctx& c, gaddr va, uint64_t len);

}  // namespace recomp::hle
