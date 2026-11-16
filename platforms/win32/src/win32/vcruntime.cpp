// VCRUNTIME140.dll for x64 guests: the memory and string primitives, the
// type_info and std::exception helpers. Its exception handling
// (_CxxThrowException, __CxxFrameHandler4, __C_specific_handler) belongs
// to the x64 unwinder (ARCHITECTURE.md, "64-bit guests").

#include <cstring>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

const char* S(gaddr p) { return g_proc->mem.ptr<const char>(p); }

}  // namespace

// ---- memory ---------------------------------------------------------------
// Guest memory is one host mapping, so these are the host's on it.

HLE64("vcruntime140.dll", memcpy, kCdecl) {
  std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memmove, kCdecl) {
  std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memset, kCdecl) {
  std::memset(GPtr(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memcmp, kCdecl) {
  int r = std::memcmp(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return uint32_t(r < 0 ? -1 : r > 0 ? 1 : 0);
}
HLE64("vcruntime140.dll", memchr, kCdecl) {
  const uint8_t* p = GPtr(Arg(c, 0));
  const void* r = std::memchr(p, int(uint8_t(Arg(c, 1))), Arg(c, 2));
  return r ? Arg(c, 0) + gaddr(static_cast<const uint8_t*>(r) - p) : 0;
}

// ---- strings ----------------------------------------------------------------

HLE64("vcruntime140.dll", strchr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strchr(p, int(char(Arg(c, 1))));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("vcruntime140.dll", strrchr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strrchr(p, int(char(Arg(c, 1))));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("vcruntime140.dll", strstr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strstr(p, S(Arg(c, 1)));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}

// Wide strings: wchar_t is 16 bits on Windows.
const uint16_t* W(gaddr p) { return g_proc->mem.ptr<const uint16_t>(p); }

HLE64("vcruntime140.dll", wcschr, kCdecl) {
  const uint16_t ch = uint16_t(Arg(c, 1));
  for (gaddr p = Arg(c, 0);; p += 2) {
    const uint16_t w = Rd16(p);
    if (w == ch) return p;
    if (!w) return 0;
  }
}
HLE64("vcruntime140.dll", wcsrchr, kCdecl) {
  const uint16_t ch = uint16_t(Arg(c, 1));
  gaddr last = 0;
  for (gaddr p = Arg(c, 0);; p += 2) {
    const uint16_t w = Rd16(p);
    if (w == ch) last = p;
    if (!w) return last;
  }
}
HLE64("vcruntime140.dll", wcsstr, kCdecl) {
  const gaddr hay = Arg(c, 0), needle = Arg(c, 1);
  size_t n = 0;
  while (W(needle)[n]) n++;
  for (gaddr p = hay;; p += 2) {
    if (!std::memcmp(W(p), W(needle), 2 * n)) return p;
    if (!Rd16(p)) return 0;
  }
}

// ---- type_info ----------------------------------------------------------------
// __std_type_info_data {const char* _UndecoratedName; char _DecoratedName[];}
// (after type_info's vtable pointer). Types are equal when their decorated
// names are, past the leading '.'.

HLE64("vcruntime140.dll", __std_type_info_compare, kCdecl) {
  gaddr a = Arg(c, 0), b = Arg(c, 1);
  if (a == b) return 0;
  int r = std::strcmp(S(a + 8 + 1), S(b + 8 + 1));
  return uint32_t(r < 0 ? -1 : r > 0 ? 1 : 0);
}

// The list of undecorated names type_info::name() allocated: never built
// (name() is not provided), so nothing to free.
HLE64("vcruntime140.dll", __std_type_info_destroy_list, kCdecl) { return 0; }

// ---- std::exception -------------------------------------------------------------
// __std_exception_data {const char* _What; bool _DoFree;}: _DoFree means
// _What was allocated (copied) and is owned.

HLE64("vcruntime140.dll", __std_exception_copy, kCdecl) {
  gaddr from = Arg(c, 0), to = Arg(c, 1);
  gaddr what = Rd64(from);
  if (Rd8(from + 8) && what) {
    std::string s = S(what);
    gaddr copy = HeapAllocProcess(s.size() + 1, false);
    PutStr(copy, s);
    Wr64(to, copy);
    Wr8(to + 8, 1);
  } else {
    Wr64(to, what);
    Wr8(to + 8, 0);
  }
  return 0;
}

HLE64("vcruntime140.dll", __std_exception_destroy, kCdecl) {
  gaddr data = Arg(c, 0);
  if (Rd8(data + 8)) HeapFreeProcess(Rd64(data));
  Wr64(data, 0);
  Wr8(data + 8, 0);
  return 0;
}

// ---- RTTI -----------------------------------------------------------------------
// MSVC's x64 RTTI: a vtable's slot -1 is the RTTICompleteObjectLocator
// {signature (1), offset, cdOffset, type descriptor, class hierarchy
// descriptor, self}, the last three RVAs (self gives the image base). The
// hierarchy descriptor {signature, attributes, base count, base class
// array} lists base class descriptors {type descriptor, contained bases,
// PMD {mdisp, pdisp, vdisp}, attributes, hierarchy descriptor}, the
// complete class first. TypeDescriptor {vftable, spare, name}.

constexpr uint32_t kChdMultiple = 1, kChdVirtual = 2;
constexpr uint32_t kBcdNotVisible = 1, kBcdAmbiguous = 2;

bool SameType(gaddr a, gaddr b) { return a == b || !std::strcmp(S(a + 16), S(b + 16)); }

int64_t PmdOffset(gaddr obj, gaddr pmd) {
  int64_t off = 0;
  const int32_t mdisp = int32_t(Rd32(pmd)), pdisp = int32_t(Rd32(pmd + 4)),
                vdisp = int32_t(Rd32(pmd + 8));
  if (pdisp >= 0) off = pdisp + int32_t(Rd32(Rd64(obj + pdisp) + vdisp));
  return off + mdisp;
}

// __RTDynamicCast(inptr, VfDelta, SrcType, TargetType, isReference): the
// target subobject, found among the complete object's bases; the first
// visible, unambiguous match.
HLE64("vcruntime140.dll", __RTDynamicCast, kCdecl) {
  gaddr in = Arg(c, 0);
  const gaddr target = Arg(c, 3);
  const bool ref = uint32_t(Arg(c, 4)) != 0;
  if (!in) return 0;
  gaddr col = Rd64(Rd64(in) - 8);
  gaddr obj = in - Rd32(col + 4);
  if (uint32_t cd = Rd32(col + 8)) obj -= int32_t(Rd32(in - cd));
  const gaddr image = col - Rd32(col + 20);
  const gaddr chd = image + Rd32(col + 16);
  const uint32_t attrs = Rd32(chd + 4), n = Rd32(chd + 8);
  const gaddr bases = image + Rd32(chd + 12);
  for (uint32_t i = 0; i < n; i++) {
    gaddr bcd = image + Rd32(bases + 4 * i);
    if (!SameType(image + Rd32(bcd), target)) continue;
    if ((attrs & (kChdMultiple | kChdVirtual)) && (Rd32(bcd + 20) & (kBcdNotVisible | kBcdAmbiguous)))
      continue;
    return obj + PmdOffset(obj, bcd + 8);
  }
  if (ref) FatalF("guest C++: dynamic_cast to a reference failed (std::bad_cast is not thrown yet)");
  return 0;
}

// ---- setjmp -----------------------------------------------------------------------
// setjmp itself returns 0 the first time; a longjmp back would need the
// caller to be resumable (a lifter `setjmp` line for the import).
HLE64("vcruntime140.dll", __intrinsic_setjmp, kCdecl) { return 0; }
HLE64("vcruntime140.dll", longjmp, kCdecl) {
  FatalF("guest longjmp: setjmp through the import is not resumable yet");
}

HLE64("vcruntime140.dll", __vcrt_InitializeCriticalSectionEx, kCdecl) {
  InitCs(Arg(c, 0), uint32_t(Arg(c, 1)));
  return 1;
}

HLE64("vcruntime140.dll", __std_terminate, kCdecl) { FatalF("guest called std::terminate()"); }
HLE64("vcruntime140.dll", _purecall, kCdecl) {
  FatalF("guest called a pure virtual function (_purecall)");
}

}  // namespace recomp::hle
