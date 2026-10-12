#pragma once
// Process-level state owned by the kernel32 HLE: heaps, handles, the
// command line and environment, and guest <-> host path mapping.

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "runtime/guest_alloc.h"

namespace recomp {

inline constexpr uint32_t kGuestPid = 0x1000;
// Where the game believes it is installed (GameConfig::guest_dir) and the
// full guest path of its EXE.
const std::string& GuestGameDir();
std::string GuestExePath();

void InitKernel32State();

// SYSTEMTIME at guest address st from a broken-down time and milliseconds.
void WriteSystemTime(uint32_t st, const std::tm& t, int ms);

uint32_t HeapAllocProcess(uint32_t size, bool zero);
void HeapFreeProcess(uint32_t p);

// Milliseconds since the process started: GetTickCount, timeGetTime and
// message times.
uint32_t TickCount();

// FILETIME (100 ns units since 1601) of the Unix epoch.
inline constexpr uint64_t kFileTimeUnixEpoch = 116444736000000000ull;
GuestHeap* FindHeap(uint32_t handle);

// Kernel object handles. Values are small multiples of 4, as on Windows.
struct KObject {
  enum class Type { File, Event, Mutex, Thread, Mapping, Find, StdIo };
  explicit KObject(Type t) : type(t) {}
  virtual ~KObject() = default;
  Type type;
};

uint32_t NewHandle(std::shared_ptr<KObject> obj);
int FileFd(KObject* o);  // host fd of a File/StdIo object, or -1
std::shared_ptr<KObject> GetHandle(uint32_t h);
bool CloseHandleValue(uint32_t h);

// Guest path ("C:\Dungeon Siege\Resources\Logic.dsres", or relative to the
// guest current directory) to a host path, resolving case-insensitively
// against what exists on disk. Paths outside known roots map below the
// per-user data directory.
//
// The game install is never written: below the guest install directory,
// paths resolve to an overlay in the per-user data directory
// ($XDG_DATA_HOME/<id>/install) when the file exists there, else to the
// install. kWrite always gives the overlay path (creating the overlay's
// parent directories when the guest can see that directory); kUpdate also
// copies an install-only file into the overlay first (open for update).
enum class PathAccess { kRead, kWrite, kUpdate };
std::string HostPath(const std::string& guest_path,
                     PathAccess access = PathAccess::kRead);
// The install and overlay directories behind a guest directory, for
// merged listings (overlay first). One entry outside the install.
std::vector<std::string> HostDirs(const std::string& guest_dir);
std::string GuestCurrentDir();

// Thrown by ExitThread to unwind a guest thread's host stack back to its
// start routine; caught in process.cpp.
struct GuestThreadExit {
  uint32_t code;
};
void OnGuestThreadFinished(uint32_t tid);

struct Thread;
// Runs guest function entry(param) on a new detached host thread bound to
// guest thread t.
void StartHostThread(Thread* t, uint32_t entry, uint32_t param);

}  // namespace recomp
