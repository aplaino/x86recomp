// libSceLibcInternal HLE: the firmware's own C library, which system
// libraries that ship with games (and the games' PRXs) import; a game's
// libc is its own PRX, lifted with it.

#include <cstdio>
#include <cstring>
#include <string>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/guest_alloc.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

// The library's version marker (a symbol that modules link against).
ORBIS_DATA("libSceLibcInternal", "Need_sceLibcInternal", need_libc_internal, 8, nullptr);
// stdout as a FILE: an opaque block that the stdio handlers recognise.
static recomp::gaddr g_stdout = 0;
ORBIS_DATA("libSceLibcInternal", "_Stdout", stdout_file, 0x100,
           [](recomp::gaddr a) { g_stdout = a; });

namespace {

// The library's heap (malloc and free): its own, in guest memory.
recomp::GuestHeap& Heap() {
  static recomp::GuestHeap* h = new recomp::GuestHeap(recomp::g_proc->vm, 1u << 20, 0);
  return *h;
}

// The arguments of a System V va_list (gp_offset, fp_offset,
// overflow_arg_area, reg_save_area): integers from the saved registers,
// then the stack; floating point likewise from the saved XMM registers.
struct VaList {
  recomp::gaddr va;
  uint64_t Int() {
    const uint32_t gp = recomp::Rd32(va);
    if (gp < 48) {
      recomp::Wr32(va, gp + 8);
      return recomp::Rd64(recomp::Rd64(va + 16) + gp);
    }
    return Overflow();
  }
  double Float() {
    const uint32_t fp = recomp::Rd32(va + 4);
    uint64_t bits;
    if (fp < 176) {
      recomp::Wr32(va + 4, fp + 16);
      bits = recomp::Rd64(recomp::Rd64(va + 16) + fp);
    } else {
      bits = Overflow();
    }
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
  }
  uint64_t Overflow() {
    const recomp::gaddr at = recomp::Rd64(va + 8);
    recomp::Wr64(va + 8, at + 8);
    return recomp::Rd64(at);
  }
};

// printf's conversions over a guest format and va_list: flags, width and
// precision (also from arguments), the length modifiers, d i u o x X c s
// p e E f F g G a A and %%; each conversion formatted by the host's.
std::string Format(const std::string& fmt, VaList& a) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    std::string spec = "%";
    size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) spec += fmt[j++];
    auto number = [&] {
      if (j < fmt.size() && fmt[j] == '*') {
        spec += std::to_string(int32_t(a.Int()));
        j++;
        return;
      }
      while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') spec += fmt[j++];
    };
    number();
    if (j < fmt.size() && fmt[j] == '.') {
      spec += fmt[j++];
      number();
    }
    std::string len;
    while (j < fmt.size() && std::strchr("hljztL", fmt[j])) len += fmt[j++];
    if (j >= fmt.size()) break;
    const char conv = fmt[j];
    i = j;
    char buf[512];
    switch (conv) {
      case '%': out += '%'; continue;
      case 'd': case 'i': {
        int64_t v = int64_t(a.Int());
        if (len == "hh") v = int8_t(v);
        else if (len == "h") v = int16_t(v);
        else if (len.empty()) v = int32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)v);
        break;
      }
      case 'u': case 'o': case 'x': case 'X': {
        uint64_t v = a.Int();
        if (len == "hh") v = uint8_t(v);
        else if (len == "h") v = uint16_t(v);
        else if (len.empty()) v = uint32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)v);
        break;
      }
      case 'c': std::snprintf(buf, sizeof buf, (spec + "c").c_str(), int(a.Int())); break;
      case 's': {
        const recomp::gaddr p = a.Int();
        const std::string str = p ? recomp::GStr(p) : "(null)";
        std::snprintf(buf, sizeof buf, (spec + "s").c_str(), str.c_str());
        break;
      }
      case 'p': std::snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)a.Int()); break;
      case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
        std::snprintf(buf, sizeof buf, (spec + conv).c_str(), a.Float());
        break;
      default:
        recomp::FatalF("libSceLibcInternal printf: conversion %%%c not implemented yet", conv);
    }
    out += buf;
  }
  return out;
}

}  // namespace

using recomp::hle::Arg;

// int fflush(FILE* stream): stdout (or every stream: null) is the log's,
// written as it goes.
ORBIS_HLE("libSceLibcInternal", fflush, 1) {
  const recomp::gaddr f = Arg(c, 0);
  if (f && f != g_stdout)
    recomp::FatalF("libSceLibcInternal fflush: a stream other than stdout not implemented yet");
  std::fflush(stdout);
  return 0;
}

// int vprintf(const char* format, va_list ap): to the log's stdout.
ORBIS_HLE("libSceLibcInternal", vprintf, 2) {
  VaList a{Arg(c, 1)};
  const std::string s = Format(recomp::GStr(Arg(c, 0)), a);
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
  return s.size();
}

ORBIS_HLE("libSceLibcInternal", memcpy, 3) {
  std::memcpy(recomp::GPtr(Arg(c, 0)), recomp::GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
ORBIS_HLE("libSceLibcInternal", memset, 3) {
  std::memset(recomp::GPtr(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
ORBIS_HLE("libSceLibcInternal", malloc, 1) {
  const recomp::gaddr p = Heap().Alloc(Arg(c, 0) ? Arg(c, 0) : 1, false);
  if (!p) recomp::orbis::kernel::SetErrno(c, recomp::orbis::kernel::kENOMEM);
  return p;
}
ORBIS_HLE("libSceLibcInternal", free, 1) {
  if (Arg(c, 0) && !Heap().Free(Arg(c, 0)))
    recomp::FatalF("libSceLibcInternal free(%llX): not a block of its heap",
                   (unsigned long long)Arg(c, 0));
  return 0;
}
