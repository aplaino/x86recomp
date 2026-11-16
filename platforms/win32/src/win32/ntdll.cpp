// ntdll's native API, as games find it with GetProcAddress. Protectors ask
// it whether a debugger is attached and take a missing function for
// tampering; the answers are those of a process nobody debugs.

#include <cstring>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/process.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kStatusSuccess = 0, kStatusInfoLengthMismatch = 0xC0000004,
                   kStatusPortNotSet = 0xC0000353;

}  // namespace

// NtQueryInformationProcess(process, class, info, length, &returned)
HLE64("ntdll.dll", NtQueryInformationProcess, 5) {
  const uint32_t cls = uint32_t(Arg(c, 1)), len = uint32_t(Arg(c, 3));
  const gaddr info = Arg(c, 2), returned = Arg(c, 4);
  const uint32_t ptr = g_proc->x64 ? 8 : 4;
  auto answer = [&](uint32_t need, uint32_t status) -> uint64_t {
    if (returned) Wr32(returned, need);
    return status;
  };
  switch (cls) {
    case 0: {  // ProcessBasicInformation: {ExitStatus, PebBaseAddress,
               // AffinityMask, BasePriority, UniqueProcessId, InheritedFrom...}
      const uint32_t need = 6 * ptr;
      if (len < need) return answer(need, kStatusInfoLengthMismatch);
      std::memset(GPtr(info), 0, need);
      Wr32(info, 0x103);  // STILL_ACTIVE
      WrPtr(info + ptr, g_proc->x64 ? kPebVa64 : kPebVa);
      WrPtr(info + 2 * ptr, (1ull << GuestProcessorCount()) - 1);
      Wr32(info + 3 * ptr, 8);  // NORMAL_PRIORITY_CLASS's base priority
      WrPtr(info + 4 * ptr, kGuestPid);
      return answer(need, kStatusSuccess);
    }
    case 7:  // ProcessDebugPort: none
      if (len < ptr) return answer(ptr, kStatusInfoLengthMismatch);
      WrPtr(info, 0);
      return answer(ptr, kStatusSuccess);
    case 0x1E:  // ProcessDebugObjectHandle: none
      if (len < ptr) return answer(ptr, kStatusInfoLengthMismatch);
      WrPtr(info, 0);
      return answer(ptr, kStatusPortNotSet);
    case 0x1F:  // ProcessDebugFlags: NoDebugInherit, set when nobody debugs
      if (len < 4) return answer(4, kStatusInfoLengthMismatch);
      Wr32(info, 1);
      return answer(4, kStatusSuccess);
    default:
      FatalF("NtQueryInformationProcess: information class %u is not supported", cls);
  }
}

}  // namespace recomp::hle
