#pragma once
// Process-level state owned by the kernel32 HLE: heaps, handles, the
// command line and environment, and guest <-> host path mapping.

#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "runtime/guest_alloc.h"

namespace recomp {

inline constexpr uint32_t kGuestPid = 0x1000;
// The process that started the guest, as process snapshots report it.
inline constexpr uint32_t kGuestParentPid = 0x0FFC;
// Where the game believes it is installed (GameConfig::guest_dir) and the
// full guest path of its EXE.
const std::string& GuestGameDir();
std::string GuestExePath();

void InitKernel32State();

namespace hle {
// EnterCriticalSection / LeaveCriticalSection on a guest CRITICAL_SECTION
// (for the condition variables that release and retake one).
void EnterCs(gaddr cs);
void LeaveCs(gaddr cs);
// A new guest thread running entry(param) (CreateThread, _beginthreadex):
// its handle, and its id in *tid. stack 0 is the EXE's default.
uint32_t NewThread(uint64_t stack, gaddr entry, uint64_t param, bool suspended, uint32_t* tid);
// Waits for the thread behind handle h to finish: its exit code, or false
// if h is not a thread.
bool JoinThread(uint32_t h, uint32_t* exit_code);
// Runs the user APCs queued for the calling guest thread (ReadFileEx
// completion routines); false if there were none. Alertable waits call it.
bool RunUserApcs(Ctx& c);
// The host file descriptor behind a guest file handle; -1 if it is not one.
int HostFileFd(uint32_t handle);
}  // namespace hle

// QueryPerformanceCounter's clock: 100 ns ticks (QueryPerformanceFrequency
// is 10 MHz) since the process started.
uint64_t PerfCounter();

// The process environment, as NAME=value strings.
std::vector<std::string> EnvironmentVars();

// SYSTEMTIME at guest address st from a broken-down time and milliseconds.
void WriteSystemTime(uint32_t st, const std::tm& t, int ms);

gaddr HeapAllocProcess(uint64_t size, bool zero);

// <ID>_WAIT_LOG=ms: a guest wait (an HLE call's lifetime) longer than that
// is logged with the thread, what it waited on and the guest caller (the
// return address on the stack at entry).
struct WaitLog {
  const char* what;
  uint64_t object;
  uint32_t caller;
  uint32_t esp;  // for the callers further up
  uint64_t t0;
  WaitLog(const Ctx& c, const char* what, uint64_t object);
  ~WaitLog();
};
void HeapFreeProcess(gaddr p);
// HeapReAlloc on the process heap (0 if p is not a live block or the
// size cannot be had); HeapSize, UINT64_MAX if p is not a live block.
gaddr HeapReAllocProcess(gaddr p, uint64_t size);
uint64_t HeapSizeProcess(gaddr p);

// Milliseconds since the process started: GetTickCount, timeGetTime and
// message times.
uint32_t TickCount();

// FILETIME (100 ns units since 1601) of the Unix epoch.
inline constexpr uint64_t kFileTimeUnixEpoch = 116444736000000000ull;
GuestHeap* FindHeap(gaddr handle);

// Kernel object handles. Values are small multiples of 4, as on Windows.
struct KObject {
  enum class Type { File, Event, Mutex, Semaphore, Thread, Mapping, Find, StdIo, Socket, Snapshot, Process };
  explicit KObject(Type t) : type(t) {}
  virtual ~KObject() = default;
  Type type;
};

struct ProcessObject : KObject {
  explicit ProcessObject(uint32_t p) : KObject(Type::Process), pid(p) {}
  uint32_t pid;
};

uint32_t NewHandle(std::shared_ptr<KObject> obj);
int FileFd(KObject* o);  // host fd of a File/StdIo object, or -1
std::shared_ptr<KObject> GetHandle(uint32_t h);
bool CloseHandleValue(uint32_t h);

// Guest path (absolute, e.g. below GameConfig::guest_dir, or relative to
// the guest current directory) to a host path, resolving case-insensitively
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
// A Unicode path (W functions, UTF-8 APIs) as the guest's narrow
// Windows-1252 path, the form HostPath takes; fatal for a character
// outside Windows-1252.
std::string NarrowPath(const std::u16string& path);
// The install and overlay directories behind a guest directory, for
// merged listings (overlay first). One entry outside the install.
std::vector<std::string> HostDirs(const std::string& guest_dir);
std::string GuestCurrentDir();

// Thrown by ExitThread to unwind a guest thread's host stack back to its
// start routine; caught in process.cpp.
struct GuestThreadExit {
  uint32_t code;
};
void OnGuestThreadFinished(uint32_t tid, uint32_t exit_code);

struct Thread;
// SetEvent (or PulseEvent) on an event handle from host code (WinMM
// timers). False if `h` is not an event.
bool SignalEventHandle(uint32_t h, bool pulse);

// Wakes the main thread if it is blocked in a wait, so that it pumps host
// events (see RunOnUiThread).
void WakeMainThreadWait();
// A wait for any of several objects takes, among such events set at once,
// the one set first (Windows: the lowest index). For DirectSound position
// notifications.
bool MarkEventOrdered(uint32_t h);
// A new unnamed event (CreateEvent from host code: xlive notifications).
uint32_t NewEventHandle(bool manual_reset, bool initial);
// ResetEvent from host code. False if `h` is not an event.
bool ResetEventHandle(uint32_t h);

// Runs guest function entry(param) on a new detached host thread bound to
// guest thread t.
void StartHostThread(Thread* t, gaddr entry, uint64_t param);

// Memory ordering of guest synchronisation calls. On Windows every one of
// them (critical sections, waits, events, mutexes, Interlocked*, thread
// start and exit, TlsAlloc/TlsFree, Sleep) is a full barrier: a locked
// instruction or a kernel entry. x86 is TSO, but arm64 hosts are weakly
// ordered and lifted code reaches guest memory with plain loads and
// stores, so a guest that hands data over through one of these calls
// relies on more than the host primitive behind it gives. A host mutex is
// acquire on lock and release on unlock: enough for data written inside a
// critical section to be seen by the next owner, but accesses outside it
// may move into it and pass each other there, so lock + unlock is not a
// full barrier. Each such handler therefore opens a GuestSyncPoint, a
// seq_cst fence on entry and on exit (also when it returns early or
// throws): every guest access before the call is ordered before the call's
// own effects, and those before every guest access after it.
inline void GuestSyncFence() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
struct GuestSyncPoint {
  GuestSyncPoint() { GuestSyncFence(); }
  ~GuestSyncPoint() { GuestSyncFence(); }
  GuestSyncPoint(const GuestSyncPoint&) = delete;
  GuestSyncPoint& operator=(const GuestSyncPoint&) = delete;
};

}  // namespace recomp
