// xboxkrnl: the runtime library's strings and memory routines, debug
// output, formatting.

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ppc/fatal.h"
#include "xbox360/format.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";

// Copies s and a NUL into a guest buffer of `count` bytes as _vsnprintf
// does: the length, or -1 (no NUL) if it does not fit.
int32_t PutLimited(uint32_t buf, uint32_t count, const std::string& s) {
  uint32_t n = uint32_t(s.size());
  std::memcpy(Mem() + buf, s.data(), std::min(n, count));
  if (n < count) {
    Wr8(buf + n, 0);
    return int32_t(n);
  }
  return n == count ? int32_t(n) : -1;
}

}  // namespace

// ANSI_STRING / UNICODE_STRING: Length, MaximumLength (be16, bytes), Buffer.
HLE(kKrnl, 300, RtlInitAnsiString) {
  uint32_t dest = Arg(c, 0), src = Arg(c, 1);
  uint32_t n = src ? uint32_t(std::min<size_t>(ReadString(src).size(), 0xFFFE)) : 0;
  Wr16(dest, uint16_t(n));
  Wr16(dest + 2, uint16_t(src ? n + 1 : 0));
  Wr32(dest + 4, src);
  return 0;
}

HLE(kKrnl, 301, RtlInitUnicodeString) {
  uint32_t dest = Arg(c, 0), src = Arg(c, 1);
  uint32_t n = src ? uint32_t(std::min<size_t>(ReadString16(src).size() * 2, 0xFFFC)) : 0;
  Wr16(dest, uint16_t(n));
  Wr16(dest + 2, uint16_t(src ? n + 2 : 0));
  Wr32(dest + 4, src);
  return 0;
}

// RtlFreeAnsiString(string): its buffer, from the kernel pool.
HLE(kKrnl, 295, RtlFreeAnsiString) {
  uint32_t s = Arg(c, 0);
  if (Rd32(s + 4)) SystemFree(Rd32(s + 4));
  Wr16(s, 0);
  Wr16(s + 2, 0);
  Wr32(s + 4, 0);
  return 0;
}

// RtlFillMemoryUlong(dest, length, pattern): whole words.
HLE(kKrnl, 294, RtlFillMemoryUlong) {
  uint32_t dest = Arg(c, 0), length = Arg(c, 1), pattern = Arg(c, 2);
  for (uint32_t k = 0; k < length / 4; k++) Wr32(dest + 4 * k, pattern);
  return 0;
}

// RtlCompareMemoryUlong(source, length, pattern): the bytes before the
// first word that differs.
HLE(kKrnl, 283, RtlCompareMemoryUlong) {
  uint32_t src = Arg(c, 0), length = Arg(c, 1), pattern = Arg(c, 2);
  uint32_t k = 0;
  while (k + 4 <= length && Rd32(src + k) == pattern) k += 4;
  return k;
}

// DbgPrint(format, ...): the title's debug output, to stdout.
HLE(kKrnl, 3, DbgPrint) {
  GuestArgs args{&c, 1};
  std::string s = Format(ReadString(Arg(c, 0)), args);
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
  return kStatusSuccess;
}

HLE(kKrnl, 1, DbgBreakPoint) { ppc::Fatal(&c, "DbgBreakPoint (called from %llx)", (unsigned long long)c.lr); }

// sprintf(buffer, format, ...): the length.
HLE(kKrnl, 315, sprintf) {
  GuestArgs args{&c, 2};
  std::string s = Format(ReadString(Arg(c, 1)), args);
  std::memcpy(Mem() + Arg(c, 0), s.c_str(), s.size() + 1);
  return uint32_t(s.size());
}

// _vsnprintf(buffer, count, format, va_list).
HLE(kKrnl, 333, _vsnprintf) {
  GuestArgs args{&c, 0, Arg(c, 3)};
  return uint32_t(PutLimited(Arg(c, 0), Arg(c, 1), Format(ReadString(Arg(c, 2)), args)));
}

}  // namespace recomp::xbox360
