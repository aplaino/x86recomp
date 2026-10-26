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

// The ANSI code page's conversions: bytes are the first 256 code points
// (Latin-1), and a character beyond them becomes '?', as Xenia converts
// (console check).
uint8_t ToAnsi(uint16_t u) { return u < 0x100 ? uint8_t(u) : '?'; }

// Win32 errors of the statuses this kernel returns, as Windows NT maps
// them (RtlNtStatusToDosError).
struct DosError {
  uint32_t status, error;
};
constexpr DosError kDosErrors[] = {
    {0x00000103, 997},  // pending: ERROR_IO_PENDING
    {0x80000005, 234},  // buffer overflow: ERROR_MORE_DATA
    {0x80000006, 18},   // no more files: ERROR_NO_MORE_FILES
    {0xC0000001, 31},   // unsuccessful: ERROR_GEN_FAILURE
    {0xC0000002, 1},    // not implemented: ERROR_INVALID_FUNCTION
    {0xC0000003, 87},   // invalid info class: ERROR_INVALID_PARAMETER
    {0xC0000004, 24},   // info length mismatch: ERROR_BAD_LENGTH
    {0xC0000008, 6},    // invalid handle: ERROR_INVALID_HANDLE
    {0xC000000D, 87},   // invalid parameter: ERROR_INVALID_PARAMETER
    {0xC000000F, 2},    // no such file: ERROR_FILE_NOT_FOUND
    {0xC0000011, 38},   // end of file: ERROR_HANDLE_EOF
    {0xC0000017, 8},    // no memory: ERROR_NOT_ENOUGH_MEMORY
    {0xC0000018, 487},  // conflicting addresses: ERROR_INVALID_ADDRESS
    {0xC0000022, 5},    // access denied: ERROR_ACCESS_DENIED
    {0xC0000023, 122},  // buffer too small: ERROR_INSUFFICIENT_BUFFER
    {0xC0000024, 6},    // object type mismatch: ERROR_INVALID_HANDLE
    {0xC0000033, 123},  // object name invalid: ERROR_INVALID_NAME
    {0xC0000034, 2},    // object name not found: ERROR_FILE_NOT_FOUND
    {0xC0000035, 183},  // object name collision: ERROR_ALREADY_EXISTS
    {0xC000003A, 3},    // object path not found: ERROR_PATH_NOT_FOUND
    {0xC0000045, 87},   // invalid page protection: ERROR_INVALID_PARAMETER
    {0xC0000046, 288},  // mutant not owned: ERROR_NOT_OWNER
    {0xC0000047, 298},  // semaphore limit exceeded: ERROR_TOO_MANY_POSTS
    {0xC000007F, 112},  // disk full: ERROR_DISK_FULL
    {0xC000009F, 487},  // free VM not at base: ERROR_INVALID_ADDRESS
    {0xC00000A0, 487},  // memory not allocated: ERROR_INVALID_ADDRESS
    {0xC00000BA, 5},    // file is a directory: ERROR_ACCESS_DENIED
    {0xC00000F1, 87},   // invalid parameter 3: ERROR_INVALID_PARAMETER
    {0xC0000101, 145},  // directory not empty: ERROR_DIR_NOT_EMPTY
    {0xC0000103, 267},  // not a directory: ERROR_DIRECTORY
    {0xC0000135, 126},  // DLL not found: ERROR_MOD_NOT_FOUND
    {0xC0000225, 1168}, // not found: ERROR_NOT_FOUND
    {0xC0000263, 127},  // driver entry point not found: ERROR_PROC_NOT_FOUND
};

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

// RtlMultiByteToUnicodeN(unicode, its bytes, *written bytes, ansi, its
// bytes) and RtlUnicodeToMultiByteN(ansi, its bytes, *written bytes,
// unicode, its bytes): as many characters as both sides hold.
HLE(kKrnl, 307, RtlMultiByteToUnicodeN) {
  uint32_t dest = Arg(c, 0), written = Arg(c, 2), src = Arg(c, 3);
  uint32_t n = std::min(Arg(c, 1) / 2, Arg(c, 4));
  for (uint32_t k = 0; k < n; k++) Wr16(dest + 2 * k, Rd8(src + k));
  if (written) Wr32(written, 2 * n);
  return kStatusSuccess;
}

HLE(kKrnl, 323, RtlUnicodeToMultiByteN) {
  uint32_t dest = Arg(c, 0), written = Arg(c, 2), src = Arg(c, 3);
  uint32_t n = std::min(Arg(c, 1), Arg(c, 4) / 2);
  for (uint32_t k = 0; k < n; k++) Wr8(dest + k, ToAnsi(Rd16(src + 2 * k)));
  if (written) Wr32(written, n);
  return kStatusSuccess;
}

// RtlUnicodeStringToAnsiString(ansi, unicode, allocate): into a buffer from
// the kernel pool (RtlFreeAnsiString frees it), or into the string's own,
// truncated with STATUS_BUFFER_OVERFLOW if it does not fit.
HLE(kKrnl, 322, RtlUnicodeStringToAnsiString) {
  uint32_t dest = Arg(c, 0), src = Arg(c, 1);
  uint32_t n = Rd16(src) / 2, buf = Rd32(src + 4);
  if (n > 0xFFFE) return kStatusInvalidParameter2;
  uint32_t out, capacity;
  if (Arg(c, 2)) {
    capacity = n + 1;
    out = SystemAlloc(capacity);
    Wr32(dest + 4, out);
    Wr16(dest + 2, uint16_t(capacity));
  } else {
    out = Rd32(dest + 4);
    capacity = Rd16(dest + 2);
    if (!capacity) return kStatusBufferOverflow;
  }
  uint32_t copy = std::min(n, capacity - 1);
  for (uint32_t k = 0; k < copy; k++) Wr8(out + k, ToAnsi(Rd16(buf + 2 * k)));
  Wr8(out + copy, 0);
  Wr16(dest, uint16_t(copy));
  return copy < n ? kStatusBufferOverflow : kStatusSuccess;
}

// RtlNtStatusToDosError(status): success is 0; a customer status (bit 29)
// stays itself, an HRESULT of FACILITY_WIN32 (0x8007xxxx) gives its error;
// a status this kernel does not return is fatal, to be added with its
// mapping.
HLE(kKrnl, 309, RtlNtStatusToDosError) {
  uint32_t status = Arg(c, 0);
  if (!status || status & 0x20000000) return status;
  if (status >> 16 == 0x8007) return status & 0xFFFF;
  for (const DosError& e : kDosErrors)
    if (e.status == status) return e.error;
  ppc::Fatal(&c, "RtlNtStatusToDosError(%x): no mapping for this status here", status);
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
