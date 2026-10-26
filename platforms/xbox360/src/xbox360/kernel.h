#pragma once
// The kernel's state the HLE modules share (ARCHITECTURE.md, "Runtime:
// threads and objects"): dispatcher objects in guest memory and the waits
// on them, handles, guest threads with the structures guest code reads
// through r13 and the kernel's own threads, the title's module, and time.

#include <cstdint>
#include <string>

#include "ppc/ctx.h"

namespace recomp::xbox360 {

struct XexModule;

// NTSTATUS values.
constexpr uint32_t kStatusSuccess = 0, kStatusAbandoned = 0x80, kStatusAlerted = 0x101,
                   kStatusTimeout = 0x102, kStatusPending = 0x103,
                   kStatusUnsuccessful = 0xC0000001, kStatusNotImplemented = 0xC0000002,
                   kStatusInvalidHandle = 0xC0000008, kStatusInvalidParameter = 0xC000000D,
                   kStatusBufferTooSmall = 0xC0000023, kStatusInvalidParameter2 = 0xC00000F0,
                   kStatusInvalidParameter3 = 0xC00000F1,
                   kStatusNoMemory = 0xC0000017, kStatusConflictingAddresses = 0xC0000018,
                   kStatusObjectTypeMismatch = 0xC0000024,
                   kStatusInvalidPageProtection = 0xC0000045, kStatusMutantNotOwned = 0xC0000046,
                   kStatusSemaphoreLimitExceeded = 0xC0000047,
                   kStatusMemoryNotAllocated = 0xC00000A0, kStatusFreeVmNotAtBase = 0xC000009F,
                   kStatusDllNotFound = 0xC0000135, kStatusNotFound = 0xC0000225,
                   kStatusDriverEntrypointNotFound = 0xC0000263;
// ... and the file system's.
constexpr uint32_t kStatusBufferOverflow = 0x80000005, kStatusNoMoreFiles = 0x80000006,
                   kStatusInvalidInfoClass = 0xC0000003, kStatusInfoLengthMismatch = 0xC0000004,
                   kStatusNoSuchFile = 0xC000000F, kStatusEndOfFile = 0xC0000011,
                   kStatusAccessDenied = 0xC0000022, kStatusObjectNameInvalid = 0xC0000033,
                   kStatusObjectNameNotFound = 0xC0000034, kStatusObjectNameCollision = 0xC0000035,
                   kStatusObjectPathNotFound = 0xC000003A, kStatusDiskFull = 0xC000007F,
                   kStatusFileIsADirectory = 0xC00000BA, kStatusDirectoryNotEmpty = 0xC0000101,
                   kStatusNotADirectory = 0xC0000103;

// ---- dispatcher objects -----------------------------------------------
// Every waitable object starts with a DISPATCHER_HEADER in guest memory:
// the type (byte 0), the signal state (be32 at 4) and the list of waiting
// blocks (8). Guest code initialises events itself (KeInitializeEvent is
// inline), so the header in guest memory is the object's state; the host
// keeps no copy. KSEMAPHORE adds its limit at 0x10, KMUTANT its owner
// thread at 0x18 and its abandoned flag at 0x1C.
enum ObjectType : uint8_t {
  kNotificationEvent = 0,
  kSynchronizationEvent = 1,
  kMutantObject = 2,
  kProcessObject = 3,
  kQueueObject = 4,
  kSemaphoreObject = 5,
  kThreadObject = 6,
  kNotificationTimer = 8,
  kSynchronizationTimer = 9,
};
constexpr uint32_t kDispatcherHeaderSize = 0x10, kSemaphoreSize = 0x14, kMutantSize = 0x20;

// The header of a new object: type, signal state, empty wait list.
void InitObject(uint32_t obj, ObjectType type, int32_t signal_state);
// Changes an object's signal state and wakes its waiters, under the
// dispatcher lock: SetSignal returns the previous state (KeSetEvent,
// KeResetEvent), PulseEvent sets it, lets current waiters through and
// clears it again.
int32_t SetSignal(uint32_t obj, int32_t state);
int32_t PulseEvent(uint32_t obj);
// KeReleaseSemaphore: false (and no change) if the count would exceed the
// limit.
bool ReleaseSemaphore(uint32_t sem, int32_t count, int32_t& previous);
// KeReleaseMutant on behalf of the current thread: kStatusMutantNotOwned
// if it does not own it.
uint32_t ReleaseMutant(uint32_t mutant, int32_t& previous);

// RTL_CRITICAL_SECTION (0x1C bytes): a synchronization event's header (its
// byte 1 the spin count / 256), the lock count at 0x10 (-1 free), the
// recursion count at 0x14 and the owning KTHREAD at 0x18; free.
constexpr uint32_t kCriticalSectionSize = 0x1C;
void InitCriticalSection(uint32_t cs, uint32_t spin_count);

// KeWaitForMultipleObjects for the current thread. `timeout` is a 100 ns
// count, NT's LARGE_INTEGER: negative relative, otherwise an absolute
// system time; nullptr waits forever. Returns the index of the object
// that satisfied a wait-any (plus kStatusAbandoned for an abandoned
// mutant), 0 for a wait-all, or kStatusTimeout. Alertable waits are not
// woken by APCs yet (none are queued).
uint32_t Wait(const uint32_t* objs, uint32_t count, bool wait_all, const int64_t* timeout,
              bool alertable);
// KeDelayExecutionThread: a wait on nothing.
uint32_t Delay(int64_t timeout, bool alertable);

// ---- handles ------------------------------------------------------------
// A handle names a dispatcher object (or another kernel object) by its
// guest address, and holds a reference on it. Objects the kernel created
// (NtCreateEvent, threads) are freed with their last reference.
constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE, kCurrentProcessHandle = 0xFFFFFFFF;
uint32_t NewHandle(uint32_t obj);
// The object of a handle (pseudo handles included), 0 if it names none.
uint32_t HandleObject(uint32_t handle);
bool CloseHandle(uint32_t handle);
// References on objects (ObReferenceObjectByHandle, ObDereferenceObject).
// An object registered with Own() is freed when its references drop to
// zero.
void Own(uint32_t obj, void (*destroy)(uint32_t obj));
void Reference(uint32_t obj);
void Dereference(uint32_t obj);

// ---- threads ------------------------------------------------------------
struct Thread;
constexpr uint32_t kCreateSuspended = 0x1;

struct ThreadParams {
  uint32_t stack_size;  // 0: the title's default
  uint32_t startup;     // XAPI's startup routine, called as startup(start, context); or 0
  uint32_t start, context;
  uint32_t flags;       // kCreateSuspended; the processor in bits 24-31
  // The title's main thread: its start routine returning ends the process
  // (the C runtime's exit), with what it returned.
  bool main = false;
};
// A new guest thread on its own host thread; it holds one reference on its
// KTHREAD until it ends.
Thread* CreateThread(const ThreadParams& p);
Thread* CurrentThread();
// The guest KTHREAD of a thread (its dispatcher object) and back.
uint32_t KThread(const Thread* t);
Thread* ThreadOf(uint32_t kthread);
uint32_t ThreadId(const Thread* t);
// The thread's last error (RtlSetLastError, GetLastError read it inline).
void SetLastError(uint32_t error);
// The guest address of a thread's dynamic TLS slots (KeTlsGetValue).
uint32_t TlsSlots(const Thread* t);
// Its processor (KPCR and KTHREAD), 0-5.
void SetProcessor(Thread* t, uint32_t cpu);
// Suspend counts (KeSuspendThread, KeResumeThread): the previous count.
uint32_t SuspendThread(Thread* t);
uint32_t ResumeThread(Thread* t);
// Ends the current thread (ExTerminateThread, or its start routine
// returning) with `status`. The process ends with its last thread.
[[noreturn]] void ExitThread(uint32_t status);
// The IRQL, a byte of the KPCR.
constexpr uint32_t kPcrIrql = 0x18;
constexpr uint8_t kDispatchLevel = 2;
// A thread of the kernel's own (the GPU's interrupts, its command
// processor): `body(arg)` runs on it, host code, with guest structures so
// that it can call guest code (CallDpc); it does not keep the process
// alive.
Thread* CreateSystemThread(void (*body)(void* arg), void* arg);
// Calls guest function fn(a0, a1) on the current system thread, returning
// r3: CallGuest as a thread does, CallDpc as a DPC does (at
// DISPATCH_LEVEL, the KPCR's DPC-active flag set, on processor `cpu`).
uint32_t CallGuest(uint32_t fn, uint32_t a0, uint32_t a1);
uint32_t CallDpc(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t cpu);

// The title process (KeGetCurrentProcessType): kTitleProcess.
constexpr uint32_t kTitleProcess = 1;
// The title's dynamic TLS slot count and static TLS (XEX TLS info), the
// default stack size; set by the loader before the first thread.
struct ProcessInfo {
  uint32_t tls_slots, tls_data_size, tls_raw_data, tls_raw_size;
  uint32_t stack_size;
};
void SetProcessInfo(const ProcessInfo& info);
const ProcessInfo& Process();
// Ends the host process with `status` (the title's exit).
[[noreturn]] void ExitProcess(int status);

// ---- modules ------------------------------------------------------------
// The title's module as the loader describes it to guest code: its loader
// entry (LDR_DATA_TABLE_ENTRY), which XexExecutableModuleHandle points at
// and module handles are, and the XEX headers it was loaded from, in guest
// memory from the entry's 0x58.
constexpr uint32_t kLdrXexHeader = 0x58;
// Sets up the title's module: `m` as loaded, `name` the file it was
// started as (what XexGetModuleHandle matches). Before BindImports.
void LoadExecutableModule(const XexModule& m, const std::string& name);
uint32_t ExecutableModule();  // its loader entry, 0 before
const std::string& ExecutableModuleName();
// Export `ordinal` of the title's module, 0 if it has none.
uint32_t ExecutableExport(uint32_t ordinal);
// RtlImageXexHeaderField: optional header `key` of the XEX headers at
// `header` in guest memory, as the key's low byte says where its data is:
// 0 the value in the header table itself, 1 a dword in the table (its
// address), otherwise data at an offset from the headers (its address).
// 0 if the headers have no such key.
uint32_t XexHeaderField(uint32_t header, uint32_t key);

// ---- time ---------------------------------------------------------------
// The time base runs at the Xenon's 3.192 GHz / 64; xam converts its ticks
// with that constant.
constexpr uint64_t kTimeBaseFrequency = 49875000;
uint64_t TimeBaseTicks();  // since start
uint64_t SystemTime();     // 100 ns units since 1601 (FILETIME), the host's clock
uint64_t InterruptTime();  // 100 ns units since start

}  // namespace recomp::xbox360
