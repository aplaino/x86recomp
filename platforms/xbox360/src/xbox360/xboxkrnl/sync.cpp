// xboxkrnl: events, semaphores, mutants, waits, handles, critical sections,
// spin locks and IRQL, interlocked singly linked lists.

#include <algorithm>
#include <thread>

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";
constexpr uint32_t kMaxWaitObjects = 64;
constexpr uint32_t kPcrIrql = 0x18;
constexpr uint8_t kDispatchLevel = 2;

// A wait's timeout argument: a pointer to a LARGE_INTEGER, or 0 for none.
const int64_t* Timeout(uint32_t ptr, int64_t& storage) {
  if (!ptr) return nullptr;
  storage = int64_t(Rd64(ptr));
  return &storage;
}

uint32_t Pcr(const ppc::Ctx& c) { return uint32_t(c.r[13]); }

// Objects of named handles are not supported yet: the attributes' name.
void RefuseNamed(ppc::Ctx& c, uint32_t attributes, const char* what) {
  if (attributes && Rd32(attributes + 4))
    ppc::Fatal(&c, "%s with a name: named objects are not supported yet", what);
}

uint32_t NewObject(uint32_t size, ObjectType type, int32_t state) {
  uint32_t obj = SystemAlloc(size);
  InitObject(obj, type, state);
  Own(obj, SystemFree);
  return obj;
}

// Guest 32-bit words updated atomically (the lock words of critical
// sections and spin locks).
uint32_t* Word(uint32_t va) { return reinterpret_cast<uint32_t*>(Mem() + va); }
bool Cas(uint32_t va, uint32_t expected, uint32_t desired) {
  uint32_t e = __builtin_bswap32(expected);
  return __atomic_compare_exchange_n(Word(va), &e, __builtin_bswap32(desired), false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
}
// Adds to a big-endian word; returns the new value.
int32_t AtomicAdd(uint32_t va, int32_t delta) {
  uint32_t old = __atomic_load_n(Word(va), __ATOMIC_SEQ_CST);
  for (;;) {
    uint32_t next = __builtin_bswap32(uint32_t(int32_t(__builtin_bswap32(old)) + delta));
    if (__atomic_compare_exchange_n(Word(va), &old, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      return int32_t(__builtin_bswap32(next));
  }
}

}  // namespace

// ---- events -------------------------------------------------------------
// KeSetEvent(event, increment, wait), KeResetEvent(event): the previous
// state.
HLE(kKrnl, 157, KeSetEvent) { return uint32_t(SetSignal(Arg(c, 0), 1)); }
HLE(kKrnl, 143, KeResetEvent) { return uint32_t(SetSignal(Arg(c, 0), 0)); }

// NtCreateEvent(*handle, attributes, type, initial state): type 0
// notification, 1 synchronization.
HLE(kKrnl, 209, NtCreateEvent) {
  RefuseNamed(c, Arg(c, 1), "NtCreateEvent");
  uint32_t obj = NewObject(kDispatcherHeaderSize, Arg(c, 2) ? kSynchronizationEvent : kNotificationEvent,
                           Arg(c, 3) ? 1 : 0);
  Wr32(Arg(c, 0), NewHandle(obj));
  return kStatusSuccess;
}

// NtSetEvent(handle, *previous), NtClearEvent(handle), NtPulseEvent(handle, *previous).
HLE(kKrnl, 246, NtSetEvent) {
  uint32_t obj = HandleObject(Arg(c, 0));
  if (!obj) return kStatusInvalidHandle;
  int32_t old = SetSignal(obj, 1);
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(old));
  return kStatusSuccess;
}
HLE(kKrnl, 206, NtClearEvent) {
  uint32_t obj = HandleObject(Arg(c, 0));
  if (!obj) return kStatusInvalidHandle;
  SetSignal(obj, 0);
  return kStatusSuccess;
}
HLE(kKrnl, 226, NtPulseEvent) {
  uint32_t obj = HandleObject(Arg(c, 0));
  if (!obj) return kStatusInvalidHandle;
  int32_t old = PulseEvent(obj);
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(old));
  return kStatusSuccess;
}

// ---- semaphores and mutants -----------------------------------------------
// KeInitializeSemaphore(semaphore, count, limit).
HLE(kKrnl, 116, KeInitializeSemaphore) {
  InitObject(Arg(c, 0), kSemaphoreObject, int32_t(Arg(c, 1)));
  Wr32(Arg(c, 0) + 0x10, Arg(c, 2));
  return 0;
}

// KeReleaseSemaphore(semaphore, increment, adjustment, wait): the previous
// count. Past the limit the kernel raises an exception, which no guest
// handler would expect: fatal.
HLE(kKrnl, 136, KeReleaseSemaphore) {
  int32_t old;
  if (!ReleaseSemaphore(Arg(c, 0), int32_t(Arg(c, 2)), old))
    ppc::Fatal(&c, "KeReleaseSemaphore(%x) past its limit", Arg(c, 0));
  return uint32_t(old);
}

// NtCreateMutant(*handle, attributes, initial owner).
HLE(kKrnl, 212, NtCreateMutant) {
  RefuseNamed(c, Arg(c, 1), "NtCreateMutant");
  uint32_t obj = NewObject(kMutantSize, kMutantObject, Arg(c, 2) ? 0 : 1);
  if (Arg(c, 2)) Wr32(obj + 0x18, KThread(CurrentThread()));
  Wr32(Arg(c, 0), NewHandle(obj));
  return kStatusSuccess;
}

// NtReleaseMutant(handle, *previous count).
HLE(kKrnl, 242, NtReleaseMutant) {
  uint32_t obj = HandleObject(Arg(c, 0));
  if (!obj) return kStatusInvalidHandle;
  int32_t old;
  uint32_t status = ReleaseMutant(obj, old);
  if (status == kStatusSuccess && Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(old));
  return status;
}

// ---- waits --------------------------------------------------------------
// KeWaitForSingleObject(object, reason, mode, alertable, *timeout).
HLE(kKrnl, 176, KeWaitForSingleObject) {
  uint32_t obj = Arg(c, 0);
  int64_t t;
  return Wait(&obj, 1, false, Timeout(Arg(c, 4), t), Arg(c, 3) != 0);
}

// KeWaitForMultipleObjects(count, objects[], wait type (0 all, 1 any),
// reason, mode, alertable, *timeout, wait blocks).
HLE(kKrnl, 175, KeWaitForMultipleObjects) {
  uint32_t count = Arg(c, 0), list = Arg(c, 1);
  if (!count || count > kMaxWaitObjects) return kStatusInvalidParameter;
  uint32_t objs[kMaxWaitObjects];
  for (uint32_t i = 0; i < count; i++) objs[i] = Rd32(list + 4 * i);
  int64_t t;
  return Wait(objs, count, Arg(c, 2) == 0, Timeout(Arg(c, 6), t), Arg(c, 5) != 0);
}

// NtWaitForSingleObjectEx(handle, mode, alertable, *timeout).
HLE(kKrnl, 253, NtWaitForSingleObjectEx) {
  uint32_t obj = HandleObject(Arg(c, 0));
  if (!obj) return kStatusInvalidHandle;
  int64_t t;
  return Wait(&obj, 1, false, Timeout(Arg(c, 3), t), Arg(c, 2) != 0);
}

// NtWaitForMultipleObjectsEx(count, handles[], wait type, mode, alertable, *timeout).
HLE(kKrnl, 254, NtWaitForMultipleObjectsEx) {
  uint32_t count = Arg(c, 0), list = Arg(c, 1);
  if (!count || count > kMaxWaitObjects) return kStatusInvalidParameter;
  uint32_t objs[kMaxWaitObjects];
  for (uint32_t i = 0; i < count; i++)
    if (!(objs[i] = HandleObject(Rd32(list + 4 * i)))) return kStatusInvalidHandle;
  int64_t t;
  return Wait(objs, count, Arg(c, 2) == 0, Timeout(Arg(c, 5), t), Arg(c, 4) != 0);
}

// ---- handles and object references -----------------------------------------
HLE(kKrnl, 207, NtClose) { return CloseHandle(Arg(c, 0)) ? kStatusSuccess : kStatusInvalidHandle; }

// NtDuplicateObject(handle, *new handle, options): options 1
// (DUPLICATE_CLOSE_SOURCE) closes the original.
HLE(kKrnl, 218, NtDuplicateObject) {
  uint32_t obj = HandleObject(Arg(c, 0));
  if (!obj) return kStatusInvalidHandle;
  Wr32(Arg(c, 1), NewHandle(obj));
  if (Arg(c, 2) & 1) CloseHandle(Arg(c, 0));
  return kStatusSuccess;
}

// The object type a caller may ask ObReferenceObjectByHandle for.
HLE_VARIABLE(kKrnl, 27, ExThreadObjectType, 0x40, nullptr);

// ObReferenceObjectByHandle(handle, object type (0: any), *object).
HLE(kKrnl, 272, ObReferenceObjectByHandle) {
  uint32_t obj = HandleObject(Arg(c, 0)), type = Arg(c, 1);
  if (!obj) return kStatusInvalidHandle;
  if (type == VariableAddress(kKrnl, 27) && Rd8(obj) != kThreadObject) return kStatusObjectTypeMismatch;
  Reference(obj);
  Wr32(Arg(c, 2), obj);
  return kStatusSuccess;
}

HLE(kKrnl, 261, ObDereferenceObject) {
  Dereference(Arg(c, 0));
  return 0;
}

// ---- critical sections ------------------------------------------------------
// RTL_CRITICAL_SECTION: a synchronization event's header (its byte 1 the
// spin count / 256), the lock count at 0x10 (-1 free), the recursion count
// at 0x14 and the owning KTHREAD at 0x18; the kernel's own algorithm: take
// it with an atomic increment of the lock count, wait on the event when it
// was held.
HLE(kKrnl, 302, RtlInitializeCriticalSection) {
  uint32_t cs = Arg(c, 0);
  InitObject(cs, kSynchronizationEvent, 0);
  Wr32(cs + 0x10, 0xFFFFFFFF);
  Wr32(cs + 0x14, 0);
  Wr32(cs + 0x18, 0);
  return 0;
}

// RtlInitializeCriticalSectionAndSpinCount(cs, spin count).
HLE(kKrnl, 303, RtlInitializeCriticalSectionAndSpinCount) {
  uint32_t cs = Arg(c, 0);
  InitObject(cs, kSynchronizationEvent, 0);
  Wr8(cs + 1, uint8_t(std::min<uint32_t>((Arg(c, 1) + 255) / 256, 255)));
  Wr32(cs + 0x10, 0xFFFFFFFF);
  Wr32(cs + 0x14, 0);
  Wr32(cs + 0x18, 0);
  return kStatusSuccess;
}

HLE(kKrnl, 293, RtlEnterCriticalSection) {
  uint32_t cs = Arg(c, 0), me = KThread(CurrentThread());
  if (Rd32(cs + 0x18) == me) {
    AtomicAdd(cs + 0x10, 1);
    Wr32(cs + 0x14, Rd32(cs + 0x14) + 1);
    return 0;
  }
  for (uint32_t spin = Rd8(cs + 1) * 256u; spin; spin--) {
    if (Cas(cs + 0x10, 0xFFFFFFFF, 0)) {
      Wr32(cs + 0x18, me);
      Wr32(cs + 0x14, 1);
      return 0;
    }
    std::this_thread::yield();
  }
  if (AtomicAdd(cs + 0x10, 1) != 0) Wait(&cs, 1, false, nullptr, false);
  Wr32(cs + 0x18, me);
  Wr32(cs + 0x14, 1);
  return 0;
}

HLE(kKrnl, 304, RtlLeaveCriticalSection) {
  uint32_t cs = Arg(c, 0);
  uint32_t recursion = Rd32(cs + 0x14) - 1;
  Wr32(cs + 0x14, recursion);
  if (recursion) {
    AtomicAdd(cs + 0x10, -1);
    return 0;
  }
  Wr32(cs + 0x18, 0);
  if (AtomicAdd(cs + 0x10, -1) >= 0) SetSignal(cs, 1);
  return 0;
}

HLE(kKrnl, 321, RtlTryEnterCriticalSection) {
  uint32_t cs = Arg(c, 0), me = KThread(CurrentThread());
  if (Cas(cs + 0x10, 0xFFFFFFFF, 0)) {
    Wr32(cs + 0x18, me);
    Wr32(cs + 0x14, 1);
    return 1;
  }
  if (Rd32(cs + 0x18) == me) {
    AtomicAdd(cs + 0x10, 1);
    Wr32(cs + 0x14, Rd32(cs + 0x14) + 1);
    return 1;
  }
  return 0;
}

// ---- spin locks and IRQL ---------------------------------------------------
// A spin lock holds its owner's KPCR (0 free). Raising IRQL changes only
// the KPCR's byte: no interrupt or DPC runs on these threads.
HLE(kKrnl, 177, KfAcquireSpinLock) {
  uint32_t lock = Arg(c, 0), pcr = Pcr(c);
  while (!Cas(lock, 0, pcr)) std::this_thread::yield();
  uint8_t old = Rd8(pcr + kPcrIrql);
  Wr8(pcr + kPcrIrql, kDispatchLevel);
  return old;
}
HLE(kKrnl, 180, KfReleaseSpinLock) {
  Cas(Arg(c, 0), Pcr(c), 0);
  Wr8(Pcr(c) + kPcrIrql, uint8_t(Arg(c, 1)));
  return 0;
}
HLE(kKrnl, 77, KeAcquireSpinLockAtRaisedIrql) {
  while (!Cas(Arg(c, 0), 0, Pcr(c))) std::this_thread::yield();
  return 0;
}
HLE(kKrnl, 137, KeReleaseSpinLockFromRaisedIrql) {
  Cas(Arg(c, 0), Pcr(c), 0);
  return 0;
}
HLE(kKrnl, 133, KeRaiseIrqlToDpcLevel) {
  uint8_t old = Rd8(Pcr(c) + kPcrIrql);
  Wr8(Pcr(c) + kPcrIrql, kDispatchLevel);
  return old;
}
HLE(kKrnl, 179, KfLowerIrql) {
  Wr8(Pcr(c) + kPcrIrql, uint8_t(Arg(c, 0)));
  return 0;
}

// ---- interlocked singly linked lists -----------------------------------------
// SLIST_HEADER: the first entry (be32), then the depth and a sequence
// number (be16 each), swapped as one doubleword.
namespace {
uint64_t* SList(uint32_t va) { return reinterpret_cast<uint64_t*>(Mem() + va); }
}  // namespace

HLE(kKrnl, 44, InterlockedPopEntrySList) {
  uint32_t head = Arg(c, 0);
  uint64_t old = __atomic_load_n(SList(head), __ATOMIC_SEQ_CST);
  for (;;) {
    uint64_t v = __builtin_bswap64(old);
    uint32_t first = uint32_t(v >> 32);
    if (!first) return 0;
    uint32_t depth = uint32_t(v >> 16) & 0xFFFF, sequence = uint32_t(v) & 0xFFFF;
    uint64_t next = uint64_t(Rd32(first)) << 32 | ((depth - 1) & 0xFFFF) << 16 | ((sequence + 1) & 0xFFFF);
    if (__atomic_compare_exchange_n(SList(head), &old, __builtin_bswap64(next), false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
      return first;
  }
}

HLE(kKrnl, 43, InterlockedFlushSList) {
  uint32_t head = Arg(c, 0);
  uint64_t old = __atomic_load_n(SList(head), __ATOMIC_SEQ_CST);
  for (;;) {
    uint64_t v = __builtin_bswap64(old);
    uint64_t next = v & 0xFFFF;  // no entries, depth 0, the sequence kept
    if (__atomic_compare_exchange_n(SList(head), &old, __builtin_bswap64(next), false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
      return uint32_t(v >> 32);
  }
}

}  // namespace recomp::xbox360
