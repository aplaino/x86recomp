// The runtime test title (tests/CMakeLists.txt, xbox360_runtime): a
// program that calls the kernel as a title does, through imports, and
// checks what the kernel's documented behaviour gives. Built for the
// 64-bit ELFv2 ABI like the fixture, so pointers are 64-bit here while
// the guest's structures hold 32-bit ones (uint32_t fields), and calls
// into the kernel pass at most eight arguments: ELFv2's parameter area is
// not the Xenon's (start.S's sprintf9 makes the one call that needs it).
// Reports each
// failed check with DbgPrint and returns their number, which the title's
// end makes the process's exit status.

#include <stdint.h>

typedef uint32_t u32;

// The imports (imports.S). Every result is declared unsigned: the kernel
// returns 32-bit values, zero-extended here.
u32 DbgPrint(const char* fmt, ...);
u32 ExAllocatePool(u32 size);
u32 ExCreateThread(u32* handle, u32 stack, u32* id, void* startup, void* start, void* context, u32 flags);
u32 ExFreePool(u32 p);
u32 InterlockedFlushSList(void* head);
u32 InterlockedPopEntrySList(void* head);
u32 KeDelayExecutionThread(u32 mode, u32 alertable, int64_t* interval);
u32 KeGetCurrentProcessType(void);
u32 KeInitializeSemaphore(void* sem, u32 count, u32 limit);
uint64_t KeQueryPerformanceFrequency(void);
u32 KeQuerySystemTime(uint64_t* time);
u32 KeReleaseSemaphore(void* sem, u32 increment, u32 adjustment, u32 wait);
u32 KeResetEvent(void* event);
u32 KeSetEvent(void* event, u32 increment, u32 wait);
u32 KeWaitForSingleObject(void* obj, u32 reason, u32 mode, u32 alertable, int64_t* timeout);
u32 KfAcquireSpinLock(u32* lock);
u32 KfReleaseSpinLock(u32* lock, u32 irql);
u32 MmAllocatePhysicalMemoryEx(u32 flags, u32 size, u32 protect, u32 lo, u32 hi, u32 align);
u32 MmFreePhysicalMemory(u32 type, u32 va);
u32 MmGetPhysicalAddress(u32 va);
u32 MmQueryAddressProtect(u32 va);
u32 NtAllocateVirtualMemory(u32* base, u32* size, u32 type, u32 protect, u32 debug);
u32 NtClose(u32 handle);
u32 NtCreateEvent(u32* handle, void* attributes, u32 type, u32 state);
u32 NtCreateMutant(u32* handle, void* attributes, u32 owned);
u32 NtFreeVirtualMemory(u32* base, u32* size, u32 type, u32 debug);
u32 NtQueryVirtualMemory(u32 base, void* info, u32 debug);
u32 NtReleaseMutant(u32 handle, u32* previous);
u32 NtResumeThread(u32 handle, u32* previous);
u32 NtSetEvent(u32 handle, u32* previous);
u32 NtWaitForSingleObjectEx(u32 handle, u32 mode, u32 alertable, int64_t* timeout);
u32 NtWaitForMultipleObjectsEx(u32 count, u32* handles, u32 type, u32 mode, u32 alertable, int64_t* timeout);
u32 ObReferenceObjectByHandle(u32 handle, u32 type, u32* object);
u32 ObDereferenceObject(u32 object);
u32 RtlEnterCriticalSection(void* cs);
u32 RtlFillMemoryUlong(void* dest, u32 length, u32 pattern);
u32 RtlInitAnsiString(void* dest, const char* src);
u32 RtlInitializeCriticalSection(void* cs);
u32 RtlLeaveCriticalSection(void* cs);
u32 RtlTryEnterCriticalSection(void* cs);
u32 KeTlsAlloc(void);
u32 KeTlsGetValue(u32 index);
u32 KeTlsSetValue(u32 index, u32 value);
u32 sprintf(char* buf, const char* fmt, ...);
u32 _vsnprintf(char* buf, u32 count, const char* fmt, uint64_t* args);
extern u32 __imp_NtClose, __imp_ExThreadObjectType, __imp_KeTimeStampBundle, __imp_XboxKrnlVersion;

// start.S
void thread_startup(void);
u32 sprintf9(char* buf, const char* fmt, int a, int b, int c, int d, int e, int f, int g);
uint64_t time_base(void);
uint64_t pcr(void);

enum {
  kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000,
  kMemLargePages = 0x20000000, kMem16MbPages = 0x80000000, kPageReadWrite = 0x04,
  kStatusTimeout = 0x102, kStatusConflictingAddresses = 0xC0000018, kStatusInvalidHandle = 0xC0000008,
  kStatusMutantNotOwned = 0xC0000046,
};

void* memset(void* d, int v, unsigned long n) {
  for (unsigned long k = 0; k < n; k++) ((volatile unsigned char*)d)[k] = (unsigned char)v;
  return d;
}
void* memcpy(void* d, const void* s, unsigned long n) {
  for (unsigned long k = 0; k < n; k++) ((volatile unsigned char*)d)[k] = ((const unsigned char*)s)[k];
  return d;
}

static int failures;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      failures++;                                                            \
      DbgPrint("FAIL line %d: %s\n", __LINE__, #cond);                       \
    }                                                                        \
  } while (0)

static volatile u32* At(u32 va) { return (volatile u32*)(uintptr_t)va; }
static int Streq(const char* a, const char* b) {
  while (*a && *a == *b) a++, b++;
  return *a == *b;
}

// ---- memory -------------------------------------------------------------
static void Memory(void) {
  u32 base = 0, size = 0x3000;
  CHECK(NtAllocateVirtualMemory(&base, &size, kMemReserve | kMemCommit, kPageReadWrite, 0) == 0);
  CHECK(base && base < 0x40000000 && (base & 0xFFF) == 0 && size == 0x3000);
  *At(base + 0x1FFC) = 0x12345678;
  CHECK(*At(base + 0x1FFC) == 0x12345678);
  u32 info[7];
  CHECK(NtQueryVirtualMemory(base + 0x1000, info, 0) == 0);
  CHECK(info[0] == base + 0x1000 && info[1] == base && info[3] == 0x2000 && info[4] == kMemCommit &&
        info[5] == kPageReadWrite);
  // Decommitted pages come back zero.
  u32 b2 = base + 0x1000, s2 = 0x1000;
  CHECK(NtFreeVirtualMemory(&b2, &s2, kMemDecommit, 0) == 0);
  CHECK(NtQueryVirtualMemory(base + 0x1000, info, 0) == 0 && info[4] == kMemReserve && info[3] == 0x1000);
  b2 = base + 0x1000;
  s2 = 0x1000;
  CHECK(NtAllocateVirtualMemory(&b2, &s2, kMemCommit, kPageReadWrite, 0) == 0 && b2 == base + 0x1000);
  CHECK(*At(base + 0x1FFC) == 0);
  // A fixed reservation over one that exists fails.
  u32 b3 = base, s3 = 0x1000;
  CHECK(NtAllocateVirtualMemory(&b3, &s3, kMemReserve, kPageReadWrite, 0) == kStatusConflictingAddresses);
  u32 rel = 0;
  CHECK(NtFreeVirtualMemory(&base, &rel, kMemRelease, 0) == 0 && rel == 0x3000);
  // Large pages: 64 KiB granularity from 0x40000000.
  base = 0;
  size = 0x1000;
  CHECK(NtAllocateVirtualMemory(&base, &size, kMemReserve | kMemCommit | kMemLargePages, kPageReadWrite, 0) == 0);
  CHECK(base >= 0x40000000 && base < 0x80000000 && (base & 0xFFFF) == 0 && size == 0x10000);
  rel = 0;
  CHECK(NtFreeVirtualMemory(&base, &rel, kMemRelease, 0) == 0);
  // The kernel pool.
  u32 p = ExAllocatePool(100);
  CHECK(p != 0 && (p & 15) == 0);
  ExFreePool(p);
}

// ---- physical memory and its views ---------------------------------------------
static void Physical(void) {
  u32 small = MmAllocatePhysicalMemoryEx(0, 0x2000, kPageReadWrite, 0, 0, 0x1000);
  u32 large = MmAllocatePhysicalMemoryEx(0, 0x10000, kPageReadWrite | kMemLargePages, 0, 0, 0x10000);
  u32 huge = MmAllocatePhysicalMemoryEx(0, 0x1000000, kPageReadWrite | kMem16MbPages, 0, 0x1FFFFFFF, 0x1000000);
  CHECK(small >= 0xE0000000 && large >= 0xA0000000 && large < 0xC0000000 && huge >= 0xC0000000 &&
        huge < 0xE0000000);
  // The 4 KiB view is one page into physical memory: what is written
  // there is read through the 64 KiB view at its physical address.
  u32 pa = MmGetPhysicalAddress(small);
  CHECK(pa == small - 0xE0000000 + 0x1000);
  *At(small + 4) = 0xCAFEF00D;
  CHECK(*At(0xA0000000 + pa + 4) == 0xCAFEF00D);
  CHECK(*At(0xC0000000 + pa + 4) == 0xCAFEF00D);
  CHECK(MmGetPhysicalAddress(large) == large - 0xA0000000);
  CHECK(MmQueryAddressProtect(large) == kPageReadWrite);
  // No two allocations share physical pages, whatever their views.
  u32 pl = MmGetPhysicalAddress(large), ph = MmGetPhysicalAddress(huge);
  CHECK(pa + 0x2000 <= pl || pl + 0x10000 <= pa);
  CHECK(pa + 0x2000 <= ph || ph + 0x1000000 <= pa);
  CHECK(pl + 0x10000 <= ph || ph + 0x1000000 <= pl);
  // Physical bounds.
  u32 low = MmAllocatePhysicalMemoryEx(0, 0x10000, kPageReadWrite | kMemLargePages, 0x100000, 0x1FFFFF, 0x10000);
  CHECK(low && MmGetPhysicalAddress(low) >= 0x100000 && MmGetPhysicalAddress(low) + 0x10000 <= 0x200000);
  MmFreePhysicalMemory(0, low);
  MmFreePhysicalMemory(0, small);
  MmFreePhysicalMemory(0, large);
  MmFreePhysicalMemory(0, huge);
}

// ---- threads, TLS, synchronisation ------------------------------------------
struct Shared {
  u32 cs[7];      // RTL_CRITICAL_SECTION
  u32 event[4];   // a synchronization event (KEVENT)
  u32 count;
  u32 tls_index;
  u32 tls_seen;   // what a worker read from its own slot before setting it
  u32 kthread;    // a worker's KPCR+0x100
  u32 thread_id;  // a worker's KTHREAD+0x14C
};
static struct Shared shared;

static u32 Counter(void* arg) {
  (void)arg;
  for (int k = 0; k < 20000; k++) {
    RtlEnterCriticalSection(shared.cs);
    RtlEnterCriticalSection(shared.cs);  // recursion
    u32 v = shared.count;
    shared.count = v + 1;
    RtlLeaveCriticalSection(shared.cs);
    RtlLeaveCriticalSection(shared.cs);
  }
  return 7;
}

static u32 Probe(void* arg) {
  shared.tls_seen = KeTlsGetValue(shared.tls_index);
  KeTlsSetValue(shared.tls_index, 0x2222);
  shared.kthread = *At((u32)pcr() + 0x100);
  shared.thread_id = *At(shared.kthread + 0x14C);
  KeWaitForSingleObject(shared.event, 3, 1, 0, 0);
  return (u32)(uintptr_t)arg;
}

static void Threads(void) {
  CHECK(KeGetCurrentProcessType() == 1);
  // r13: the KPCR, whose current thread is this thread's KTHREAD, a
  // thread object.
  u32 me = *At((u32)pcr() + 0x100);
  CHECK(me != 0 && (*At(me) >> 24) == 6);

  // TLS: a slot per thread.
  shared.tls_index = KeTlsAlloc();
  CHECK(shared.tls_index != 0xFFFFFFFF);
  CHECK(KeTlsSetValue(shared.tls_index, 0x1111) && KeTlsGetValue(shared.tls_index) == 0x1111);

  // A worker created suspended, started, waiting on an event.
  shared.event[0] = 1 << 24;  // KeInitializeEvent: a synchronization event, not signalled
  shared.event[1] = 0;
  shared.event[2] = shared.event[3] = (u32)(uintptr_t)&shared.event[2];
  u32 h = 0, id = 0;
  CHECK(ExCreateThread(&h, 0, &id, (void*)thread_startup, (void*)Probe, (void*)(uintptr_t)42, 1) == 0);
  CHECK(h != 0 && id != 0);
  int64_t ten_ms = -100000;
  CHECK(NtWaitForSingleObjectEx(h, 1, 0, &ten_ms) == kStatusTimeout);  // still suspended
  u32 previous = 9;
  CHECK(NtResumeThread(h, &previous) == 0 && previous == 1);
  int64_t zero = 0;
  CHECK(NtWaitForSingleObjectEx(h, 1, 0, &zero) == kStatusTimeout);    // waiting on the event
  KeDelayExecutionThread(1, 0, &ten_ms);
  KeSetEvent(shared.event, 1, 0);
  CHECK(NtWaitForSingleObjectEx(h, 1, 0, 0) == 0);
  CHECK(shared.event[1] == 0);  // the waiter took the synchronization event
  CHECK(shared.tls_seen == 0 && KeTlsGetValue(shared.tls_index) == 0x1111);
  CHECK(shared.kthread != me && shared.thread_id == id);
  // The thread's exit status, and its KTHREAD through its handle.
  u32 obj = 0;
  CHECK(ObReferenceObjectByHandle(h, __imp_ExThreadObjectType, &obj) == 0 && obj == shared.kthread);
  CHECK(*At(obj + 0x140) == 42);
  ObDereferenceObject(obj);
  CHECK(NtClose(h) == 0 && NtClose(h) == kStatusInvalidHandle);

  // Four threads contending for a critical section.
  RtlInitializeCriticalSection(shared.cs);
  u32 hs[4];
  for (int k = 0; k < 4; k++) CHECK(ExCreateThread(&hs[k], 0x10000, 0, (void*)thread_startup, (void*)Counter, 0, 0) == 0);
  CHECK(NtWaitForMultipleObjectsEx(4, hs, 0, 1, 0, 0) == 0);
  CHECK(shared.count == 80000);
  CHECK(shared.cs[4] == 0xFFFFFFFF && shared.cs[6] == 0);  // free again
  for (int k = 0; k < 4; k++) NtClose(hs[k]);
  CHECK(RtlTryEnterCriticalSection(shared.cs) == 1 && shared.cs[6] == me);
  RtlLeaveCriticalSection(shared.cs);
}

static void Objects(void) {
  // A notification event created signalled stays signalled through waits.
  u32 e = 0;
  CHECK(NtCreateEvent(&e, 0, 0, 1) == 0);
  int64_t zero = 0;
  CHECK(NtWaitForSingleObjectEx(e, 1, 0, &zero) == 0 && NtWaitForSingleObjectEx(e, 1, 0, &zero) == 0);
  // A synchronization event is taken by one wait.
  u32 s = 0, previous = 9;
  CHECK(NtCreateEvent(&s, 0, 1, 0) == 0);
  CHECK(NtWaitForSingleObjectEx(s, 1, 0, &zero) == kStatusTimeout);
  CHECK(NtSetEvent(s, &previous) == 0 && previous == 0);
  u32 both[2] = {s, e};
  CHECK(NtWaitForMultipleObjectsEx(2, both, 1, 1, 0, &zero) == 0);  // any: the first, s
  CHECK(NtWaitForMultipleObjectsEx(2, both, 1, 1, 0, &zero) == 1);  // s taken: e
  CHECK(NtWaitForMultipleObjectsEx(2, both, 0, 1, 0, &zero) == kStatusTimeout);  // all
  NtClose(s);
  NtClose(e);
  // A semaphore counts down with waits.
  u32 sem[5];
  KeInitializeSemaphore(sem, 2, 3);
  CHECK(KeWaitForSingleObject(sem, 3, 1, 0, &zero) == 0 && KeWaitForSingleObject(sem, 3, 1, 0, &zero) == 0);
  CHECK(KeWaitForSingleObject(sem, 3, 1, 0, &zero) == kStatusTimeout);
  CHECK(KeReleaseSemaphore(sem, 1, 3, 0) == 0 && sem[1] == 3);
  // A mutant is recursive for its owner.
  u32 m = 0;
  CHECK(NtCreateMutant(&m, 0, 1) == 0);
  CHECK(NtWaitForSingleObjectEx(m, 1, 0, &zero) == 0);
  CHECK(NtReleaseMutant(m, &previous) == 0 && NtReleaseMutant(m, &previous) == 0 && previous == 0);
  CHECK(NtReleaseMutant(m, &previous) == kStatusMutantNotOwned);
  NtClose(m);
  // A spin lock holds the owner's KPCR.
  u32 lock = 0;
  u32 irql = KfAcquireSpinLock(&lock);
  CHECK(lock == (u32)pcr() && irql == 0);
  KfReleaseSpinLock(&lock, irql);
  CHECK(lock == 0);
  // An interlocked list: pop and flush.
  u32 nodes[3] = {(u32)(uintptr_t)&nodes[1], (u32)(uintptr_t)&nodes[2], 0};
  u32 head[2] = {(u32)(uintptr_t)&nodes[0], 3 << 16};
  CHECK(InterlockedPopEntrySList(head) == (u32)(uintptr_t)&nodes[0]);
  CHECK(head[0] == (u32)(uintptr_t)&nodes[1] && (head[1] >> 16) == 2);
  CHECK(InterlockedFlushSList(head) == (u32)(uintptr_t)&nodes[1] && head[0] == 0 && (head[1] >> 16) == 0);
  // A call through an import's slot reaches its handler.
  u32 (*close)(u32) = (u32(*)(u32))(uintptr_t)__imp_NtClose;
  CHECK(close(0x1234) == kStatusInvalidHandle);
}

// ---- strings and formatting, variables, time ------------------------------------------
static u32 Vsn(char* buf, u32 count, const char* fmt, ...) {
  // The guest's va_list: the 8-byte slots of the parameter area.
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  u32 n = _vsnprintf(buf, count, fmt, (uint64_t*)ap);
  __builtin_va_end(ap);
  return n;
}

static void Strings(void) {
  static const char kName[] = "kernel";
  u32 s[2];
  RtlInitAnsiString(s, kName);
  CHECK(s[0] == (6u << 16 | 7) && s[1] == (u32)(uintptr_t)kName);
  char buf[96];
  u32 n = sprintf(buf, "%d|%5x|%-4s|%08X|%I64d|%.3f", -12, 0xAB, "ab", 0xBEEF, (int64_t)-5000000000, 2.5);
  CHECK(Streq(buf, "-12|   ab|ab  |0000BEEF|-5000000000|2.500") && n == 41);
  static const uint16_t kWide[] = {'w', 'i', 0};
  CHECK(sprintf(buf, "%c|%%|%S|%ls|%-3d|", 'z', kWide, kWide, 7) == 14 && Streq(buf, "z|%|wi|wi|7  |"));
  // The ninth argument comes from the caller's parameter area.
  CHECK(sprintf9(buf, "%c%c%c%c%c%c%c", 'a', 'b', 'c', 'd', 'e', 'f', 'g') == 7 && Streq(buf, "abcdefg"));
  CHECK(Vsn(buf, sizeof buf, "%s=%u", "n", 7u) == 3 && Streq(buf, "n=7"));
  CHECK(Vsn(buf, 4, "%s", "toolong") == 0xFFFFFFFF);
  u32 words[4] = {0, 0, 0, 0};
  RtlFillMemoryUlong(words, 12, 0x01020304);
  CHECK(words[0] == 0x01020304 && words[2] == 0x01020304 && words[3] == 0);
}

static void Variables(void) {
  const uint16_t* version = (const uint16_t*)(uintptr_t)__imp_XboxKrnlVersion;
  CHECK(version[0] == 2 && version[2] == 17559);
  // The time stamp bundle's tick count follows the time.
  volatile u32* bundle = At(__imp_KeTimeStampBundle);
  u32 before = bundle[4];
  int64_t fifty_ms = -500000;
  KeDelayExecutionThread(1, 0, &fifty_ms);
  u32 after = bundle[4];
  CHECK(after - before >= 40 && after - before < 1000);
  uint64_t t0 = time_base();
  KeDelayExecutionThread(1, 0, &fifty_ms);
  uint64_t t1 = time_base(), f = KeQueryPerformanceFrequency();
  CHECK(f == 49875000 && t1 - t0 >= f / 25 && t1 - t0 < f);
  uint64_t now = 0;
  KeQuerySystemTime(&now);
  CHECK(now > 0x01D0000000000000ull);  // after 2014
}

int main(void) {
  Memory();
  Physical();
  Threads();
  Objects();
  Strings();
  Variables();
  DbgPrint("runtime test: %d failed\n", failures);
  return failures;
}
