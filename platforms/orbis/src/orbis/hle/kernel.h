#pragma once
// libkernel's HLE (recomp_orbis_kernel): what its handlers share. libkernel
// also exports libScePosix (the plain POSIX names); both are here.
//
// Errors: the sceKernel* and scePthread* functions return an SCE error
// code (kSceKernelError + the FreeBSD errno), the POSIX ones -1 with errno
// set, or (pthread_*) the errno value itself.

#include <cstdint>

#include "runtime/ctx.h"
#include "runtime/guest_memory.h"

namespace recomp::orbis::kernel {

// FreeBSD's errno values (sys/errno.h).
enum : int {
  kEPERM = 1, kENOENT = 2, kESRCH = 3, kEINTR = 4, kEIO = 5, kEBADF = 9,
  kEDEADLK = 11, kENOMEM = 12, kEACCES = 13, kEFAULT = 14, kEBUSY = 16,
  kEEXIST = 17, kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22, kEMFILE = 24,
  kENOSPC = 28, kERANGE = 34, kEAGAIN = 35, kETIMEDOUT = 60,
  kENAMETOOLONG = 63, kENOSYS = 78,
};

inline constexpr uint32_t kSceKernelError = 0x80020000;
inline uint64_t SceError(int e) { return kSceKernelError + uint32_t(e); }

// Handles of HLE objects (threads, mutexes, semaphores, ...): distinct
// 64-byte guest blocks, recycled once freed; the guest only stores and
// compares them.
gaddr NewHandle();
void FreeHandle(gaddr h);

// The calling thread's errno (__error).
gaddr ErrnoAddr(const Ctx& c);
void SetErrno(const Ctx& c, int e);

}  // namespace recomp::orbis::kernel
