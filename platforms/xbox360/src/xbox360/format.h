#pragma once
// printf-style formatting of guest arguments, as the system's C runtime
// formats them (DbgPrint, sprintf, _vsnprintf): Microsoft's conversions
// and size prefixes (%S and %ls wide strings, I64, I32), every argument
// in an 8-byte slot of the parameter area (a double as its bit pattern).

#include <cstdint>
#include <string>

#include "ppc/ctx.h"

namespace recomp::xbox360 {

// The arguments after the format: from argument `first` of a call
// (registers, then the parameter area), or from a va_list (the guest
// address of the next slot).
struct GuestArgs {
  const ppc::Ctx* c = nullptr;
  int next = 0;
  uint32_t va_list = 0;
  uint64_t Next();
};

std::string Format(const std::string& fmt, GuestArgs& args);

}  // namespace recomp::xbox360
